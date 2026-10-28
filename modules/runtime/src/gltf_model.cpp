/* glTF 2.0 / GLB 运行时解析实现（CPU 侧；plans/23）。
 *
 * 与 tools/gltf_import.py 的关系：输出布局/材质语义保持一致（顶点 48B/56B，材质核心 PBR），
 * 但这是**引擎运行时**的 C++ 实现：不生成中间文件、不依赖 Python；节点世界变换直接烘焙进顶点。
 * 外部 buffer/image 的 URI 通过 ReadFn（runtime 的 VFS）读取；GLB 的 BIN/JSON chunk 就地解析。
 */
#include "gltf_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <print>
#include <string>
#include <vector>

#include "abi/persist.h"

namespace gltf
{
namespace
{

/* ---------- 小工具 ---------- */
const mq_json* jget(const mq_json* o, const char* k)
{
    return mq_json_get(o, k);
}
double jnum(const mq_json* o, const char* k, double def)
{
    return mq_json_num(jget(o, k), def);
}
bool jbool(const mq_json* o, const char* k, bool def = false)
{
    const mq_json* v = jget(o, k);
    if (!v)
        return def;
    return mq_json_num(v, def ? 1.0 : 0.0) != 0.0;
}
const char* jstr(const mq_json* o, const char* k, const char* def = nullptr)
{
    return mq_json_str(jget(o, k), def);
}
/* 数组字段：root 对象的 key 对应数组。 */
const mq_json* jarr(const mq_json* root, const char* k)
{
    return jget(root, k);
}
const mq_json* jitem(const mq_json* arr, uint32_t i)
{
    return mq_json_at(arr, i);
}

int comp_size(int component_type)
{
    switch (component_type)
    {
        case 5120:
        case 5121:
            return 1;
        case 5122:
        case 5123:
            return 2;
        case 5125:
        case 5126:
            return 4;
        default:
            return 0;
    }
}
const char* component_name(int t)
{
    switch (t)
    {
        case 5120:
            return "BYTE";
        case 5121:
            return "UNSIGNED_BYTE";
        case 5122:
            return "SHORT";
        case 5123:
            return "UNSIGNED_SHORT";
        case 5125:
            return "UNSIGNED_INT";
        case 5126:
            return "FLOAT";
        default:
            return "?";
    }
}
int type_components(const char* t)
{
    if (!t)
        return 0;
    if (std::strcmp(t, "SCALAR") == 0)
        return 1;
    if (std::strcmp(t, "VEC2") == 0)
        return 2;
    if (std::strcmp(t, "VEC3") == 0)
        return 3;
    if (std::strcmp(t, "VEC4") == 0)
        return 4;
    if (std::strcmp(t, "MAT4") == 0)
        return 16;
    return 0;
}

template <class T>
T load_unaligned(const uint8_t* p)
{
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>& out)
{
    auto val = [](unsigned char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    out.clear();
    out.reserve(in.size() * 3 / 4 + 3);
    int acc = 0, bits = 0;
    for (unsigned char c : in)
    {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t')
            continue;
        const int v = val(c);
        if (v < 0)
            return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFF));
        }
    }
    return true;
}

int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::string uri_decode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size())
        {
            const int hi = hex_val(s[i + 1]), lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool has_scheme(const std::string& s)
{
    if (s.rfind("data:", 0) == 0)
        return true;
    return s.find("://") != std::string::npos;
}

bool starts_with(const std::string& s, const char* p)
{
    return s.rfind(p, 0) == 0;
}

std::string normalize_path(const std::string& p)
{
    std::string prefix, body = p;
    size_t scheme = p.find("://");
    if (scheme != std::string::npos)
    {
        const size_t slash = p.find('/', scheme + 3);
        if (slash == std::string::npos)
            return p;
        prefix = p.substr(0, slash + 1);
        body = p.substr(slash + 1);
    }
    bool leading = !body.empty() && body[0] == '/';
    if (leading)
        body.erase(0, 1);
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= body.size())
    {
        const size_t end = body.find('/', start);
        const std::string seg = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!seg.empty() && seg != ".")
        {
            if (seg == "..")
            {
                if (!segs.empty())
                    segs.pop_back();
            }
            else
            {
                segs.push_back(seg);
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    std::string out = prefix;
    if (leading)
        out.push_back('/');
    for (size_t i = 0; i < segs.size(); ++i)
    {
        if (i)
            out.push_back('/');
        out += segs[i];
    }
    if (out.empty())
        out = ".";
    return out;
}

/* glTF 里的相对 URI 相对**模型文件所在目录**解析（不是场景目录，也不是 CWD）。 */
std::string resolve_uri(const std::string& base_file, const std::string& uri)
{
    const std::string decoded = uri_decode(uri);
    if (has_scheme(decoded))
        return decoded;
    const size_t scheme = base_file.find("://");
    if (!decoded.empty() && decoded[0] == '/')
    {
        if (scheme != std::string::npos)
        {
            const size_t slash = base_file.find('/', scheme + 3);
            if (slash != std::string::npos)
                return base_file.substr(0, slash) + decoded;
        }
        return decoded; /* VFS 根 */
    }
    const size_t slash = base_file.rfind('/');
    const std::string dir = slash == std::string::npos ? std::string() : base_file.substr(0, slash + 1);
    return normalize_path(dir + decoded);
}

bool decode_data_uri(const std::string& uri, std::vector<uint8_t>& out)
{
    const size_t comma = uri.find(',');
    if (comma == std::string::npos)
        return false;
    const std::string meta = uri.substr(5, comma - 5);
    const std::string payload = uri.substr(comma + 1);
    if (meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0)
        return base64_decode(payload, out);
    /* 非 base64 的 data URI（percent-encoded 文本）——buffer 基本不会用，但按 URI 语义支持。 */
    const std::string decoded = uri_decode(payload);
    out.assign(decoded.begin(), decoded.end());
    return true;
}

/* ---------- 矩阵（列主序，与引擎 math 一致）---------- */
float mat_det3(const float m[16])
{
    const float a00 = m[0], a10 = m[1], a20 = m[2];
    const float a01 = m[4], a11 = m[5], a21 = m[6];
    const float a02 = m[8], a12 = m[9], a22 = m[10];
    return a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) +
           a02 * (a10 * a21 - a11 * a20);
}
/* 列主序 mat4 → TRS（node.matrix 的静态回退；动画节点需要 TRS 语义）。
 * 3x3 有剪切时只能近似；负行列式把 X 缩放取负以保持旋转矩阵为真旋转。 */
void mat_decompose_trs(const float m[16], float t[3], float q[4], float s[3])
{
    t[0] = m[12];
    t[1] = m[13];
    t[2] = m[14];
    float c0[3] = {m[0], m[1], m[2]};
    float c1[3] = {m[4], m[5], m[6]};
    float c2[3] = {m[8], m[9], m[10]};
    float sx = std::sqrt(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]);
    const float sy = std::sqrt(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    const float sz = std::sqrt(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]);
    if (mat_det3(m) < 0.0F)
        sx = -sx;
    const float isx = std::fabs(sx) > 1e-9F ? 1.0F / sx : 0.0F;
    const float isy = sy > 1e-9F ? 1.0F / sy : 0.0F;
    const float isz = sz > 1e-9F ? 1.0F / sz : 0.0F;
    /* 行主序旋转矩阵 R[row][col]。 */
    const float r00 = c0[0] * isx, r10 = c0[1] * isx, r20 = c0[2] * isx;
    const float r01 = c1[0] * isy, r11 = c1[1] * isy, r21 = c1[2] * isy;
    const float r02 = c2[0] * isz, r12 = c2[1] * isz, r22 = c2[2] * isz;
    const float trace = r00 + r11 + r22;
    float x = 0.0F, y = 0.0F, z = 0.0F, w = 1.0F;
    if (trace > 0.0F)
    {
        const float root = std::sqrt(trace + 1.0F) * 2.0F;
        w = 0.25F * root;
        x = (r21 - r12) / root;
        y = (r02 - r20) / root;
        z = (r10 - r01) / root;
    }
    else if (r00 > r11 && r00 > r22)
    {
        const float root = std::sqrt(1.0F + r00 - r11 - r22) * 2.0F;
        w = (r21 - r12) / root;
        x = 0.25F * root;
        y = (r01 + r10) / root;
        z = (r02 + r20) / root;
    }
    else if (r11 > r22)
    {
        const float root = std::sqrt(1.0F + r11 - r00 - r22) * 2.0F;
        w = (r02 - r20) / root;
        x = (r01 + r10) / root;
        y = 0.25F * root;
        z = (r12 + r21) / root;
    }
    else
    {
        const float root = std::sqrt(1.0F + r22 - r00 - r11) * 2.0F;
        w = (r10 - r01) / root;
        x = (r02 + r20) / root;
        y = (r12 + r21) / root;
        z = 0.25F * root;
    }
    const float ql = std::sqrt(x * x + y * y + z * z + w * w);
    if (ql > 1e-12F)
    {
        q[0] = x / ql;
        q[1] = y / ql;
        q[2] = z / ql;
        q[3] = w / ql;
    }
    else
    {
        q[0] = q[1] = q[2] = 0.0F;
        q[3] = 1.0F;
    }
    s[0] = sx;
    s[1] = sy;
    s[2] = sz;
}

/* ---------- 解析上下文 ---------- */
struct AccInfo
{
    int comp_type = 0;
    int comps = 0;
    int count = 0;
    bool normalized = false;
    size_t stride = 0;
    size_t elem_size = 0;
    const uint8_t* base = nullptr; /* 第一个分量；零访问器为 nullptr */
};

class Parser
{
public:
    Parser(const std::string& path, const ReadFn& read, Model& model)
        : path_(path)
        , read_(read)
        , model_(model)
    {
    }
    ~Parser()
    {
        if (js_)
            mq_json_free(js_);
    }

    bool run(const std::vector<uint8_t>& bytes);

private:
    bool parse_json(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& bin_chunk);
    bool parse_buffers();
    bool parse_materials();
    void parse_nodes();
    void expand_instancing();
    void parse_skins();
    void parse_animations();
    void parse_lights_cameras();
    void parse_variants();

    const mq_json* acc_json(int idx) const
    {
        const mq_json* arr = jarr(js_, "accessors");
        if (!arr || idx < 0 || (uint32_t)idx >= mq_json_len(arr))
            return nullptr;
        return jitem(arr, (uint32_t)idx);
    }
    bool acc_info(int idx, AccInfo& info, std::string& err);
    bool read_float_acc(int idx, std::vector<float>& out, int& comps, std::string& err);
    bool read_index_acc(int idx, std::vector<uint32_t>& out, std::string& err);
    bool apply_sparse_float(int idx, int comps, std::vector<float>& out, std::string& err);

    int image_count() const
    {
        const mq_json* arr = jarr(js_, "images");
        return arr ? (int)mq_json_len(arr) : 0;
    }
    bool load_image(int idx);
    Sampler sampler_at(int idx) const;
    bool make_texture_ref(const mq_json* tinfo, bool srgb, TextureRef& ref);

    bool build_primitive(const mq_json* prim, const mq_json* mesh, const mq_json* node,
                         int node_idx, int skin_idx, std::vector<Primitive>& out);
    void collect_node(int node_idx, std::vector<uint8_t>& visited, int depth);
    Material default_material() const;

    const std::string& path_;
    const ReadFn& read_;
    Model& model_;
    mq_json* js_ = nullptr;
    std::vector<std::vector<uint8_t>> buffers_;
    std::vector<uint8_t> glb_bin_;
    std::vector<uint8_t> image_loaded_; /* 0=未尝试 1=成功 2=失败 */
    std::vector<Material> materials_;
};

bool Parser::parse_json(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& bin_chunk)
{
    std::string text;
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0)
    {
        uint32_t magic = 0, version = 0, total = 0;
        std::memcpy(&magic, bytes.data(), 4);
        std::memcpy(&version, bytes.data() + 4, 4);
        std::memcpy(&total, bytes.data() + 8, 4);
        (void)magic;
        if (version != 2)
        {
            model_.error = "GLB 版本不是 2";
            return false;
        }
        if (total > bytes.size())
            total = (uint32_t)bytes.size();
        size_t off = 12;
        bool have_json = false;
        while (off + 8 <= total)
        {
            uint32_t len = 0, ctype = 0;
            std::memcpy(&len, bytes.data() + off, 4);
            std::memcpy(&ctype, bytes.data() + off + 4, 4);
            off += 8;
            if (off + len > total)
            {
                model_.error = "GLB chunk 越界";
                return false;
            }
            if (ctype == 0x4E4F534A)
            { /* JSON */
                text.assign((const char*)bytes.data() + off, len);
                have_json = true;
            }
            else if (ctype == 0x004E4942)
            { /* BIN */
                bin_chunk.assign(bytes.begin() + (long)off, bytes.begin() + (long)(off + len));
            }
            off += len;
        }
        if (!have_json)
        {
            model_.error = "GLB 缺 JSON chunk";
            return false;
        }
    }
    else
    {
        text.assign((const char*)bytes.data(), bytes.size());
    }
    js_ = mq_json_parse(text.data(), text.size());
    if (!js_)
    {
        model_.error = "JSON 解析失败";
        return false;
    }
    /* 只接受 glTF 2.0（1.0 的字段语义不同，静默错渲染不如明确拒绝）。 */
    const mq_json* asset = jget(js_, "asset");
    const char* ver = asset ? jstr(asset, "version", "") : "";
    if (!ver || ver[0] != '2')
    {
        model_.error = std::string("不是 glTF 2.0（asset.version=") + (ver ? ver : "") + "）";
        return false;
    }
    glb_bin_ = std::move(bin_chunk);
    return true;
}

bool Parser::parse_buffers()
{
    const mq_json* arr = jarr(js_, "buffers");
    const uint32_t n = arr ? mq_json_len(arr) : 0;
    buffers_.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const mq_json* buf = jitem(arr, i);
        const char* uri = jstr(buf, "uri", nullptr);
        if (!uri || !*uri)
        {
            if (i == 0 && !glb_bin_.empty())
            {
                buffers_[i] = glb_bin_;
                continue;
            }
            std::print(stderr, "runtime: gltf '{}' buffer[{}] 无 uri 且无 GLB BIN chunk\n", path_, i);
            continue;
        }
        if (starts_with(uri, "data:"))
        {
            if (!decode_data_uri(uri, buffers_[i]))
                std::print(stderr, "runtime: gltf '{}' buffer[{}] data URI 解码失败\n", path_, i);
            continue;
        }
        const std::string p = resolve_uri(path_, uri);
        if (!read_(p, buffers_[i]))
            std::print(stderr, "runtime: gltf '{}' buffer[{}] 读取失败 '{}'\n", path_, i, p);
    }
    return true;
}

bool Parser::load_image(int idx)
{
    if (idx < 0 || idx >= image_count())
        return false;
    if (image_loaded_.empty())
        image_loaded_.assign((size_t)image_count(), 0);
    if (idx >= (int)image_loaded_.size())
        return false;
    if (image_loaded_[(size_t)idx] == 1)
        return model_.images[(size_t)idx].valid;
    if (image_loaded_[(size_t)idx] == 2)
        return false;
    image_loaded_[(size_t)idx] = 2;
    const mq_json* img = jitem(jarr(js_, "images"), (uint32_t)idx);
    Image out;
    const char* uri = img ? jstr(img, "uri", nullptr) : nullptr;
    std::vector<uint8_t> bytes;
    if (uri && *uri)
    {
        out.uri = uri;
        if (starts_with(uri, "data:"))
        {
            if (!decode_data_uri(uri, bytes))
            {
                std::print(stderr, "runtime: gltf '{}' image[{}] data URI 解码失败\n", path_, idx);
                return false;
            }
        }
        else
        {
            const std::string p = resolve_uri(path_, uri);
            out.uri = p;
            if (!read_(p, bytes))
            {
                std::print(stderr, "runtime: gltf '{}' image[{}] 读取失败 '{}'\n", path_, idx, p);
                return false;
            }
        }
    }
    else if (img)
    {
        const int view_idx = (int)jnum(img, "bufferView", -1);
        const mq_json* views = jarr(js_, "bufferViews");
        if (view_idx < 0 || !views || (uint32_t)view_idx >= mq_json_len(views))
        {
            std::print(stderr, "runtime: gltf '{}' image[{}] 无 uri 且 bufferView 非法\n", path_, idx);
            return false;
        }
        const mq_json* view = jitem(views, (uint32_t)view_idx);
        const int b = (int)jnum(view, "buffer", -1);
        const size_t off = (size_t)jnum(view, "byteOffset", 0.0);
        const size_t len = (size_t)jnum(view, "byteLength", 0.0);
        if (b < 0 || (size_t)b >= buffers_.size() || off + len > buffers_[(size_t)b].size())
        {
            std::print(stderr, "runtime: gltf '{}' image[{}] bufferView 越界\n", path_, idx);
            return false;
        }
        bytes.assign(buffers_[(size_t)b].begin() + (long)off,
                     buffers_[(size_t)b].begin() + (long)(off + len));
        out.uri = "bufferView:" + std::to_string(view_idx);
    }
    if (bytes.empty())
    {
        std::print(stderr, "runtime: gltf '{}' image[{}] 空数据\n", path_, idx);
        return false;
    }
    out.bytes = std::move(bytes);
    out.valid = true;
    if ((int)model_.images.size() <= idx)
        model_.images.resize((size_t)idx + 1);
    model_.images[(size_t)idx] = std::move(out);
    image_loaded_[(size_t)idx] = 1;
    return true;
}

Sampler Parser::sampler_at(int idx) const
{
    Sampler s;
    const mq_json* arr = jarr(js_, "samplers");
    if (idx < 0 || !arr || (uint32_t)idx >= mq_json_len(arr))
        return s;
    const mq_json* j = jitem(arr, (uint32_t)idx);
    const double mag = jnum(j, "magFilter", 0.0);
    const double minf = jnum(j, "minFilter", 0.0);
    bool nearest = false;
    if (mag != 0.0)
        nearest = (int)mag == 9728;
    else if (minf != 0.0)
        nearest = ((int)minf == 9728 || (int)minf == 9984 || (int)minf == 9986);
    s.filter = nearest ? 0 : 1;
    const int wrap_s = (int)jnum(j, "wrapS", 10497.0);
    const int wrap_t = (int)jnum(j, "wrapT", 10497.0);
    auto wrap = [](int w)
    { return w == 33071 ? 1 : (w == 33648 ? 2 : 0); };
    s.wrap_u = wrap(wrap_s);
    s.wrap_v = wrap(wrap_t);
    return s;
}

bool Parser::make_texture_ref(const mq_json* tinfo, bool srgb, TextureRef& ref)
{
    ref = TextureRef{};
    if (!tinfo)
        return false;
    const int tex_idx = (int)jnum(tinfo, "index", -1);
    const mq_json* textures = jarr(js_, "textures");
    if (tex_idx < 0 || !textures || (uint32_t)tex_idx >= mq_json_len(textures))
        return false;
    const mq_json* tex = jitem(textures, (uint32_t)tex_idx);
    int img_idx = (int)jnum(tex, "source", -1);
    /* KHR_texture_basisu：KTX2 贴图通过 texture.extensions.source 引用 image。 */
    const mq_json* tex_exts = jget(tex, "extensions");
    const mq_json* basisu = tex_exts ? jget(tex_exts, "KHR_texture_basisu") : nullptr;
    if (basisu)
        img_idx = (int)jnum(basisu, "source", img_idx);
    if (img_idx < 0)
        return false;
    if (!load_image(img_idx))
        return false;
    ref.present = true;
    ref.srgb = srgb;
    ref.image = img_idx;
    ref.texcoord = (int)jnum(tinfo, "texCoord", 0.0);
    if (ref.texcoord < 0)
        ref.texcoord = 0;
    if (ref.texcoord > 1)
    {
        std::print(stderr, "runtime: gltf '{}' texture texCoord={} 不支持（退回 TEXCOORD_0）\n",
                   path_, ref.texcoord);
        ref.texcoord = 0;
    }
    ref.sampler = sampler_at((int)jnum(tex, "sampler", -1.0));
    const mq_json* exts = jget(tinfo, "extensions");
    const mq_json* tr = exts ? jget(exts, "KHR_texture_transform") : nullptr;
    if (tr)
    {
        ref.has_transform = true;
        const mq_json* off = jget(tr, "offset");
        const mq_json* sc = jget(tr, "scale");
        for (int k = 0; k < 2; ++k)
        {
            ref.offset[k] = (float)mq_json_num(jitem(off, (uint32_t)k), ref.offset[k]);
            ref.scale[k] = (float)mq_json_num(jitem(sc, (uint32_t)k), ref.scale[k]);
        }
        ref.rotation = (float)jnum(tr, "rotation", 0.0);
    }
    return true;
}

bool Parser::parse_materials()
{
    const mq_json* arr = jarr(js_, "materials");
    const uint32_t n = arr ? mq_json_len(arr) : 0;
    materials_.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const mq_json* jm = jitem(arr, i);
        Material m;
        if (const char* nm = jstr(jm, "name", nullptr))
            m.name = nm;
        const mq_json* pbr = jget(jm, "pbrMetallicRoughness");
        if (pbr)
        {
            const mq_json* bc = jget(pbr, "baseColorFactor");
            if (bc)
            {
                for (int k = 0; k < 4; ++k)
                    m.base_color[k] = (float)mq_json_num(jitem(bc, (uint32_t)k), m.base_color[k]);
            }
            m.metallic = (float)jnum(pbr, "metallicFactor", m.metallic);
            m.roughness = (float)jnum(pbr, "roughnessFactor", m.roughness);
            make_texture_ref(jget(pbr, "baseColorTexture"), true, m.base_color_tex);
            make_texture_ref(jget(pbr, "metallicRoughnessTexture"), false, m.metallic_roughness_tex);
        }
        const mq_json* em = jget(jm, "emissiveFactor");
        if (em)
        {
            for (int k = 0; k < 3; ++k)
                m.emissive[k] = (float)mq_json_num(jitem(em, (uint32_t)k), m.emissive[k]);
        }
        make_texture_ref(jget(jm, "emissiveTexture"), true, m.emissive_tex);
        make_texture_ref(jget(jm, "normalTexture"), false, m.normal_tex);
        const mq_json* nt = jget(jm, "normalTexture");
        if (nt)
            m.normal_scale = (float)jnum(nt, "scale", m.normal_scale);
        make_texture_ref(jget(jm, "occlusionTexture"), false, m.occlusion_tex);
        const mq_json* ot = jget(jm, "occlusionTexture");
        if (ot)
            m.occlusion_strength = (float)jnum(ot, "strength", m.occlusion_strength);
        m.alpha_cutoff = (float)jnum(jm, "alphaCutoff", m.alpha_cutoff);
        const char* alpha_mode = jstr(jm, "alphaMode", "OPAQUE");
        if (alpha_mode)
        {
            if (std::strcmp(alpha_mode, "MASK") == 0)
                m.alpha_mode = 1;
            else if (std::strcmp(alpha_mode, "BLEND") == 0)
                m.alpha_mode = 2;
        }
        m.double_sided = jbool(jm, "doubleSided", m.double_sided);
        const mq_json* exts = jget(jm, "extensions");
        if (exts)
        {
            m.unlit = jget(exts, "KHR_materials_unlit") != nullptr;
            m.physical = jget(exts, "KHR_materials_clearcoat") || jget(exts, "KHR_materials_sheen") ||
                         jget(exts, "KHR_materials_iridescence") ||
                         jget(exts, "KHR_materials_specular") || jget(exts, "KHR_materials_ior") ||
                         jget(exts, "KHR_materials_anisotropy");
            const mq_json* es = jget(exts, "KHR_materials_emissive_strength");
            if (es)
                m.emissive_strength = (float)jnum(es, "emissiveStrength", m.emissive_strength);
            auto vec3 = [](const mq_json* o, const char* key, float out[3])
            {
                const mq_json* a = jget(o, key);
                if (!a)
                    return;
                for (int k = 0; k < 3; ++k)
                    out[k] = (float)mq_json_num(jitem(a, (uint32_t)k), out[k]);
            };
            /* KHR_materials_clearcoat */
            if (const mq_json* e = jget(exts, "KHR_materials_clearcoat"))
            {
                m.clearcoat = (float)jnum(e, "clearcoatFactor", m.clearcoat);
                m.clearcoat_roughness = (float)jnum(e, "clearcoatRoughnessFactor", m.clearcoat_roughness);
                make_texture_ref(jget(e, "clearcoatTexture"), false, m.clearcoat_tex);
                make_texture_ref(jget(e, "clearcoatRoughnessTexture"), false, m.clearcoat_roughness_tex);
                make_texture_ref(jget(e, "clearcoatNormalTexture"), false, m.clearcoat_normal_tex);
            }
            /* KHR_materials_sheen */
            if (const mq_json* e = jget(exts, "KHR_materials_sheen"))
            {
                m.sheen = 1.0F;
                m.sheen_roughness = (float)jnum(e, "sheenRoughnessFactor", m.sheen_roughness);
                vec3(e, "sheenColorFactor", m.sheen_color);
                make_texture_ref(jget(e, "sheenColorTexture"), true, m.sheen_color_tex);
                make_texture_ref(jget(e, "sheenRoughnessTexture"), false, m.sheen_roughness_tex);
            }
            /* KHR_materials_iridescence */
            if (const mq_json* e = jget(exts, "KHR_materials_iridescence"))
            {
                m.iridescence = (float)jnum(e, "iridescenceFactor", m.iridescence);
                m.iridescence_ior = (float)jnum(e, "iridescenceIor", m.iridescence_ior);
                m.iridescence_thickness_min =
                    (float)jnum(e, "iridescenceThicknessMinimum", m.iridescence_thickness_min);
                m.iridescence_thickness_max =
                    (float)jnum(e, "iridescenceThicknessMaximum", m.iridescence_thickness_max);
                make_texture_ref(jget(e, "iridescenceTexture"), false, m.iridescence_tex);
                make_texture_ref(jget(e, "iridescenceThicknessTexture"), false,
                                 m.iridescence_thickness_tex);
            }
            /* KHR_materials_specular */
            if (const mq_json* e = jget(exts, "KHR_materials_specular"))
            {
                m.specular_intensity = (float)jnum(e, "specularFactor", m.specular_intensity);
                vec3(e, "specularColorFactor", m.specular_color);
                make_texture_ref(jget(e, "specularTexture"), false, m.specular_tex);
                make_texture_ref(jget(e, "specularColorTexture"), true, m.specular_color_tex);
            }
            /* KHR_materials_ior */
            if (const mq_json* e = jget(exts, "KHR_materials_ior"))
                m.ior = (float)jnum(e, "ior", m.ior);
            /* KHR_materials_anisotropy */
            if (const mq_json* e = jget(exts, "KHR_materials_anisotropy"))
            {
                m.anisotropy = (float)jnum(e, "anisotropyStrength", m.anisotropy);
                m.anisotropy_rotation = (float)jnum(e, "anisotropyRotation", m.anisotropy_rotation);
                make_texture_ref(jget(e, "anisotropyTexture"), false, m.anisotropy_tex);
            }
            /* KHR_materials_transmission（近似：改 BLEND + Fresnel alpha，与 importer 一致） */
            if (const mq_json* e = jget(exts, "KHR_materials_transmission"))
            {
                m.transmission = (float)jnum(e, "transmissionFactor", 0.0);
                if (m.transmission > 0.01F)
                {
                    const bool was_mask = m.alpha_mode == 1;
                    m.alpha_mode = 2;
                    m.opacity = std::min(m.opacity, std::max(0.15F, 1.0F - 0.85F * m.transmission));
                    if (was_mask)
                        m.alpha_mask_in_blend = true;
                }
                make_texture_ref(jget(e, "transmissionTexture"), false, m.transmission_tex);
            }
            /* KHR_materials_volume */
            if (const mq_json* e = jget(exts, "KHR_materials_volume"))
            {
                m.volume_thickness = (float)jnum(e, "thicknessFactor", m.volume_thickness);
                m.attenuation_distance = (float)jnum(e, "attenuationDistance", m.attenuation_distance);
                vec3(e, "attenuationColor", m.attenuation_color);
                make_texture_ref(jget(e, "thicknessTexture"), false, m.volume_thickness_tex);
            }
            /* KHR_materials_volume_scatter：各向同性 in-scatter 近似 */
            if (const mq_json* e = jget(exts, "KHR_materials_volume_scatter"))
                vec3(e, "multiscatterColorFactor", m.multiscatter_color);
            /* KHR_materials_dispersion */
            if (const mq_json* e = jget(exts, "KHR_materials_dispersion"))
                m.dispersion = (float)jnum(e, "dispersion", m.dispersion);
            /* KHR_materials_diffuse_transmission（不透明薄壁透光） */
            if (const mq_json* e = jget(exts, "KHR_materials_diffuse_transmission"))
            {
                m.diffuse_transmission = std::clamp(
                    (float)jnum(e, "diffuseTransmissionFactor", m.diffuse_transmission), 0.0F, 1.0F);
                vec3(e, "diffuseTransmissionColorFactor", m.diffuse_transmission_color);
                make_texture_ref(jget(e, "diffuseTransmissionTexture"), false,
                                 m.diffuse_transmission_tex);
                make_texture_ref(jget(e, "diffuseTransmissionColorTexture"), true,
                                 m.diffuse_transmission_color_tex);
            }
        }
        materials_[(size_t)i] = std::move(m);
    }
    return true;
}

/* KHR_materials_variants：顶层 variants 名称表；primitive 的 mappings 在 build_primitive 解析。 */
void Parser::parse_variants()
{
    const mq_json* ext = jget(jget(js_, "extensions"), "KHR_materials_variants");
    const mq_json* vars = ext ? jget(ext, "variants") : nullptr;
    const uint32_t n = mq_json_len(vars);
    model_.variants.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const char* name = jstr(jitem(vars, i), "name", nullptr);
        model_.variants.emplace_back(name ? name : "");
    }
}

bool Parser::acc_info(int idx, AccInfo& info, std::string& err)
{
    const mq_json* a = acc_json(idx);
    if (!a)
    {
        err = "accessor[" + std::to_string(idx) + "] 越界";
        return false;
    }
    info.comp_type = (int)jnum(a, "componentType", 0.0);
    info.comps = type_components(jstr(a, "type", ""));
    info.count = (int)jnum(a, "count", 0.0);
    info.normalized = jbool(a, "normalized", false);
    const int csize = comp_size(info.comp_type);
    if (!csize || info.comps <= 0 || info.count < 0)
    {
        err = "accessor[" + std::to_string(idx) + "] 字段非法";
        return false;
    }
    info.elem_size = (size_t)csize * (size_t)info.comps;
    const int view_idx = (int)jnum(a, "bufferView", -1);
    if (view_idx < 0)
    {
        info.base = nullptr;
        info.stride = info.elem_size;
        return true;
    }
    const mq_json* views = jarr(js_, "bufferViews");
    if (!views || (uint32_t)view_idx >= mq_json_len(views))
    {
        err = "accessor[" + std::to_string(idx) + "] bufferView 越界";
        return false;
    }
    const mq_json* view = jitem(views, (uint32_t)view_idx);
    const int buf_idx = (int)jnum(view, "buffer", -1);
    if (buf_idx < 0 || (size_t)buf_idx >= buffers_.size())
    {
        err = "accessor[" + std::to_string(idx) + "] buffer 越界";
        return false;
    }
    const size_t view_off = (size_t)jnum(view, "byteOffset", 0.0);
    const size_t acc_off = (size_t)jnum(a, "byteOffset", 0.0);
    const size_t stride = (size_t)jnum(view, "byteStride", 0.0);
    info.stride = stride ? stride : info.elem_size;
    if (info.stride < info.elem_size)
    {
        err = "accessor[" + std::to_string(idx) + "] byteStride 小于元素大小";
        return false;
    }
    const std::vector<uint8_t>& buf = buffers_[(size_t)buf_idx];
    const size_t first = view_off + acc_off;
    if (info.count > 0)
    {
        const size_t need = first + info.stride * (size_t)(info.count - 1) + info.elem_size;
        if (need > buf.size())
        {
            err = "accessor[" + std::to_string(idx) + "] 数据越界（need " + std::to_string(need) +
                  " > buffer " + std::to_string(buf.size()) + "）";
            return false;
        }
    }
    info.base = buf.data() + first;
    return true;
}

double read_component(const uint8_t* p, int type)
{
    switch (type)
    {
        case 5120:
            return (double)load_unaligned<int8_t>(p);
        case 5121:
            return (double)load_unaligned<uint8_t>(p);
        case 5122:
            return (double)load_unaligned<int16_t>(p);
        case 5123:
            return (double)load_unaligned<uint16_t>(p);
        case 5125:
            return (double)load_unaligned<uint32_t>(p);
        case 5126:
            return (double)load_unaligned<float>(p);
        default:
            return 0.0;
    }
}
float normalize_component(int type, double v)
{
    switch (type)
    {
        case 5120:
            return (float)std::max(v / 127.0, -1.0);
        case 5121:
            return (float)(v / 255.0);
        case 5122:
            return (float)std::max(v / 32767.0, -1.0);
        case 5123:
            return (float)(v / 65535.0);
        default:
            return (float)v;
    }
}

/* sparse accessor：base 数据读完后，用 sparse.indices/values 覆盖指定元素（glTF 2.0 §5.1.2）。 */
bool Parser::apply_sparse_float(int idx, int comps, std::vector<float>& out, std::string& err)
{
    const mq_json* a = acc_json(idx);
    const mq_json* sp = a ? jget(a, "sparse") : nullptr;
    if (!sp)
        return true;
    const mq_json* indices = jget(sp, "indices");
    const mq_json* values = jget(sp, "values");
    const int count = (int)jnum(sp, "count", 0.0);
    if (count <= 0)
        return true;
    if (!indices || !values)
    {
        err = "accessor[" + std::to_string(idx) + "] sparse 缺 indices/values";
        return false;
    }
    if (out.size() < (size_t)count * (size_t)comps)
    {
        err = "accessor[" + std::to_string(idx) + "] sparse 越界";
        return false;
    }
    const mq_json* views = jarr(js_, "bufferViews");
    auto view_ptr = [&](const mq_json* info, size_t off, size_t need, const uint8_t*& p)
    {
        const int vi = (int)jnum(info, "bufferView", -1.0);
        if (!views || vi < 0 || (uint32_t)vi >= mq_json_len(views))
            return false;
        const mq_json* v = jitem(views, (uint32_t)vi);
        const int bi = (int)jnum(v, "buffer", -1.0);
        if (bi < 0 || (size_t)bi >= buffers_.size())
            return false;
        const size_t base = (size_t)jnum(v, "byteOffset", 0.0) +
                            (size_t)jnum(info, "byteOffset", 0.0) + off;
        if (base + need > buffers_[(size_t)bi].size())
            return false;
        p = buffers_[(size_t)bi].data() + base;
        return true;
    };
    const int ic = (int)jnum(indices, "componentType", 5123.0);
    const size_t isz = (size_t)comp_size(ic);
    const int vt = (int)jnum(a, "componentType", 0.0);
    const size_t vsz = (size_t)comp_size(vt) * (size_t)comps;
    if (!isz || !vsz)
    {
        err = "accessor[" + std::to_string(idx) + "] sparse 分量类型非法";
        return false;
    }
    const uint8_t* ip = nullptr;
    const uint8_t* vp = nullptr;
    if (!view_ptr(indices, 0, (size_t)count * isz, ip) ||
        !view_ptr(values, 0, (size_t)count * vsz, vp))
    {
        err = "accessor[" + std::to_string(idx) + "] sparse bufferView 越界";
        return false;
    }
    const bool norm = jbool(a, "normalized", false);
    const size_t total = out.size() / (size_t)comps;
    for (int k = 0; k < count; ++k)
    {
        const int dst = (int)read_component(ip + (size_t)k * isz, ic);
        if (dst < 0 || (size_t)dst >= total)
        {
            err = "accessor[" + std::to_string(idx) + "] sparse 目标下标越界";
            return false;
        }
        for (int c = 0; c < comps; ++c)
        {
            const double v = read_component(vp + (size_t)k * vsz +
                                                (size_t)c * (size_t)comp_size(vt),
                                            vt);
            out[(size_t)dst * (size_t)comps + (size_t)c] =
                norm ? normalize_component(vt, v) : (float)v;
        }
    }
    return true;
}

bool Parser::read_float_acc(int idx, std::vector<float>& out, int& comps, std::string& err)
{
    AccInfo info;
    if (!acc_info(idx, info, err))
        return false;
    comps = info.comps;
    out.clear();
    if (info.count <= 0)
        return true;
    out.resize((size_t)info.count * (size_t)info.comps);
    if (!info.base)
    {
        std::fill(out.begin(), out.end(), 0.0F);
        return apply_sparse_float(idx, info.comps, out, err);
    }
    for (int i = 0; i < info.count; ++i)
    {
        const uint8_t* row = info.base + (size_t)i * info.stride;
        for (int c = 0; c < info.comps; ++c)
        {
            const double v = read_component(row + (size_t)c * (size_t)comp_size(info.comp_type),
                                            info.comp_type);
            out[(size_t)i * (size_t)info.comps + (size_t)c] =
                info.normalized ? normalize_component(info.comp_type, v) : (float)v;
        }
    }
    return apply_sparse_float(idx, info.comps, out, err);
}

bool Parser::read_index_acc(int idx, std::vector<uint32_t>& out, std::string& err)
{
    AccInfo info;
    if (!acc_info(idx, info, err))
        return false;
    if (info.comps != 1)
    {
        err = "indices accessor 不是 SCALAR";
        return false;
    }
    if (info.comp_type == 5120 || info.comp_type == 5122 || info.comp_type == 5126)
    {
        err = std::string("indices 分量类型非法（") + component_name(info.comp_type) + "）";
        return false;
    }
    out.clear();
    if (info.count <= 0)
        return true;
    out.resize((size_t)info.count);
    if (!info.base)
        std::fill(out.begin(), out.end(), 0u);
    else
        for (int i = 0; i < info.count; ++i)
        {
            const uint8_t* row = info.base + (size_t)i * info.stride;
            out[(size_t)i] = (uint32_t)read_component(row, info.comp_type);
        }
    if (jget(acc_json(idx), "sparse"))
    {
        std::vector<float> tmp(out.begin(), out.end());
        if (!apply_sparse_float(idx, 1, tmp, err))
            return false;
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = (uint32_t)std::max(0.0F, tmp[i]);
    }
    return true;
}

Material Parser::default_material() const
{
    Material m;
    m.name = "<default>";
    m.base_color[0] = m.base_color[1] = m.base_color[2] = 1.0F;
    m.base_color[3] = 1.0F;
    m.metallic = 0.0F;
    m.roughness = 0.6F;
    return m;
}

bool Parser::build_primitive(const mq_json* prim, const mq_json* mesh, const mq_json* node,
                             int node_idx, int skin_idx, std::vector<Primitive>& out)
{
    const int mode = (int)jnum(prim, "mode", 4.0);
    if (mode != 4)
    {
        std::print(stderr, "runtime: gltf '{}' 跳过非三角 primitive（mode={}，node={}）\n",
                   path_, mode, node_idx);
        return false;
    }
    const mq_json* attrs = jget(prim, "attributes");
    if (!attrs || !jget(attrs, "POSITION"))
    {
        std::print(stderr, "runtime: gltf '{}' primitive 缺 POSITION（node={}）→ 跳过\n",
                   path_, node_idx);
        return false;
    }
    std::string err;
    std::vector<float> positions;
    int pcomps = 0;
    if (!read_float_acc((int)jnum(attrs, "POSITION", -1.0), positions, pcomps, err) || pcomps != 3)
    {
        std::print(stderr, "runtime: gltf '{}' POSITION 读取失败（node={}）：{}\n", path_, node_idx, err);
        return false;
    }
    const size_t vn = positions.size() / 3;
    if (vn == 0)
        return false;

    std::vector<uint32_t> indices;
    if (jget(prim, "indices"))
    {
        if (!read_index_acc((int)jnum(prim, "indices", -1.0), indices, err))
        {
            std::print(stderr, "runtime: gltf '{}' indices 读取失败（node={}）：{}\n", path_, node_idx, err);
            return false;
        }
    }
    else
    {
        indices.resize(vn);
        for (size_t i = 0; i < vn; ++i)
            indices[i] = (uint32_t)i;
    }
    for (uint32_t v : indices)
    {
        if (v >= vn)
        {
            std::print(stderr, "runtime: gltf '{}' indices 越界（{} >= {}，node={}）→ 跳过 primitive\n",
                       path_, v, vn, node_idx);
            return false;
        }
    }

    std::vector<float> normals;
    int ncomps = 0;
    const int normal_acc = attrs ? (int)jnum(attrs, "NORMAL", -1.0) : -1;
    if (normal_acc >= 0)
    {
        if (!read_float_acc(normal_acc, normals, ncomps, err) || ncomps != 3 || normals.size() != vn * 3)
        {
            std::print(stderr, "runtime: gltf '{}' NORMAL 读取失败（node={}）：{}\n", path_, node_idx, err);
            normals.clear();
        }
    }
    if (normals.empty())
    {
        /* 缺 NORMAL：按**真实 indices**累计面法线（与 importer 同一算法）。 */
        normals.assign(vn * 3, 0.0F);
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
        {
            const uint32_t i0 = indices[t], i1 = indices[t + 1], i2 = indices[t + 2];
            const float* p0 = &positions[(size_t)i0 * 3];
            const float* p1 = &positions[(size_t)i1 * 3];
            const float* p2 = &positions[(size_t)i2 * 3];
            const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
            const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                                e1[2] * e2[0] - e1[0] * e2[2],
                                e1[0] * e2[1] - e1[1] * e2[0]};
            for (uint32_t vi : {i0, i1, i2})
            {
                normals[(size_t)vi * 3 + 0] += n[0];
                normals[(size_t)vi * 3 + 1] += n[1];
                normals[(size_t)vi * 3 + 2] += n[2];
            }
        }
        for (size_t i = 0; i < vn; ++i)
        {
            float* n = &normals[i * 3];
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-12F)
            {
                n[0] /= len;
                n[1] /= len;
                n[2] /= len;
            }
            else
            {
                n[0] = 0.0F;
                n[1] = 1.0F;
                n[2] = 0.0F;
            }
        }
    }
    else
    {
        for (size_t i = 0; i < vn; ++i)
        {
            float* n = &normals[i * 3];
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-12F)
            {
                n[0] /= len;
                n[1] /= len;
                n[2] /= len;
            }
            else
            {
                n[0] = 0.0F;
                n[1] = 1.0F;
                n[2] = 0.0F;
            }
        }
    }

    std::vector<float> uv0(vn * 2, 0.0F), uv1(vn * 2, 0.0F);
    int c2 = 0;
    const int uv0_acc = (int)jnum(attrs, "TEXCOORD_0", -1.0);
    if (uv0_acc >= 0)
    {
        if (!read_float_acc(uv0_acc, uv0, c2, err) || c2 != 2 || uv0.size() != vn * 2)
        {
            std::print(stderr, "runtime: gltf '{}' TEXCOORD_0 读取失败（node={}）：{}\n",
                       path_, node_idx, err);
            uv0.assign(vn * 2, 0.0F);
        }
    }
    const int uv1_acc = (int)jnum(attrs, "TEXCOORD_1", -1.0);
    if (uv1_acc >= 0)
    {
        if (!read_float_acc(uv1_acc, uv1, c2, err) || c2 != 2 || uv1.size() != vn * 2)
        {
            std::print(stderr, "runtime: gltf '{}' TEXCOORD_1 读取失败（node={}）：{}\n",
                       path_, node_idx, err);
            uv1 = uv0;
        }
    }
    else
    {
        uv1 = uv0;
    }

    std::vector<float> colors(vn * 4, 1.0F);
    bool has_color = false;
    if (attrs && jget(attrs, "COLOR_0"))
    {
        std::vector<float> c;
        int cc = 0;
        if (read_float_acc((int)jnum(attrs, "COLOR_0", -1.0), c, cc, err) &&
            (cc == 3 || cc == 4) && c.size() == vn * (size_t)cc)
        {
            has_color = true;
            for (size_t i = 0; i < vn; ++i)
            {
                colors[i * 4 + 0] = c[i * (size_t)cc + 0];
                colors[i * 4 + 1] = c[i * (size_t)cc + 1];
                colors[i * 4 + 2] = c[i * (size_t)cc + 2];
                colors[i * 4 + 3] = cc == 4 ? c[i * 4 + 3] : 1.0F;
            }
        }
        else
        {
            std::print(stderr, "runtime: gltf '{}' COLOR_0 读取失败（node={}）：{}\n",
                       path_, node_idx, err);
        }
    }

    int mat_idx = (int)jnum(prim, "material", -1.0);
    if (mat_idx < 0 || (size_t)mat_idx >= materials_.size())
        mat_idx = -1;
    const Material* mat = mat_idx >= 0 ? &materials_[(size_t)mat_idx] : nullptr;
    bool use_uv1 = has_color || uv1_acc >= 0;
    if (mat)
    {
        for (const TextureRef* tr : {&mat->base_color_tex, &mat->metallic_roughness_tex,
                                     &mat->occlusion_tex, &mat->normal_tex, &mat->emissive_tex})
        {
            if (tr->present && tr->texcoord > 0)
                use_uv1 = true;
        }
    }
    Primitive out_prim;
    out_prim.vertex_layout = use_uv1 ? 1 : 0;
    out_prim.material = mat_idx;
    /* KHR_materials_variants：primitive 的 mappings 覆盖到每个变体索引。 */
    if (!model_.variants.empty())
    {
        out_prim.variant_materials.assign(model_.variants.size(), -1);
        const mq_json* mv =
            jget(jget(prim, "extensions"), "KHR_materials_variants");
        const mq_json* mappings = mv ? jget(mv, "mappings") : nullptr;
        const uint32_t nm = mq_json_len(mappings);
        for (uint32_t mi = 0; mi < nm; ++mi)
        {
            const mq_json* map = jitem(mappings, mi);
            const int vmat = (int)jnum(map, "material", -1.0);
            if (vmat < 0 || (size_t)vmat >= materials_.size())
                continue;
            const mq_json* vlist = jget(map, "variants");
            const uint32_t nv = mq_json_len(vlist);
            for (uint32_t vi = 0; vi < nv; ++vi)
            {
                const int variant = (int)mq_json_num(jitem(vlist, vi), -1.0);
                if (variant >= 0 && (size_t)variant < out_prim.variant_materials.size())
                    out_prim.variant_materials[(size_t)variant] = vmat;
            }
        }
    }
    out_prim.node = node_idx;
    out_prim.skin = skin_idx;
    const uint32_t row_floats = use_uv1 ? 14u : 12u;
    out_prim.vertices.resize(vn * row_floats);

    float bmin[3] = {1e30F, 1e30F, 1e30F};
    float bmax[3] = {-1e30F, -1e30F, -1e30F};
    for (size_t i = 0; i < vn; ++i)
    {
        const float* p = &positions[i * 3];
        const float* n = &normals[i * 3];
        float* row = &out_prim.vertices[i * row_floats];
        row[0] = p[0];
        row[1] = p[1];
        row[2] = p[2];
        row[3] = n[0];
        row[4] = n[1];
        row[5] = n[2];
        if (use_uv1)
        {
            row[6] = colors[i * 4 + 0];
            row[7] = colors[i * 4 + 1];
            row[8] = colors[i * 4 + 2];
            row[9] = colors[i * 4 + 3];
            row[10] = uv0[i * 2 + 0];
            row[11] = uv0[i * 2 + 1];
            row[12] = uv1[i * 2 + 0];
            row[13] = uv1[i * 2 + 1];
        }
        else
        {
            row[6] = 1.0F;
            row[7] = 0.0F;
            row[8] = 0.0F;
            row[9] = 1.0F; /* TANGENT 占位（shader 不用） */
            row[10] = uv0[i * 2 + 0];
            row[11] = uv0[i * 2 + 1];
        }
        bmin[0] = std::min(bmin[0], p[0]);
        bmin[1] = std::min(bmin[1], p[1]);
        bmin[2] = std::min(bmin[2], p[2]);
        bmax[0] = std::max(bmax[0], p[0]);
        bmax[1] = std::max(bmax[1], p[1]);
        bmax[2] = std::max(bmax[2], p[2]);
    }
    out_prim.indices = std::move(indices);
    std::memcpy(out_prim.bounds_min, bmin, sizeof bmin);
    std::memcpy(out_prim.bounds_max, bmax, sizeof bmax);

    /* ---- 蒙皮顶点（JOINTS_0/WEIGHTS_0；>4 影响暂不支持）---- */
    if (skin_idx >= 0)
    {
        const int ja = attrs ? (int)jnum(attrs, "JOINTS_0", -1.0) : -1;
        const int wa = attrs ? (int)jnum(attrs, "WEIGHTS_0", -1.0) : -1;
        if (attrs && jget(attrs, "JOINTS_1"))
            std::print(stderr, "runtime: gltf '{}' 忽略 JOINTS_1/WEIGHTS_1（node={}，只支持 4 影响）\n",
                       path_, node_idx);
        std::vector<float> jr, wr;
        int jc = 0, wc = 0;
        bool skin_ok = ja >= 0 && wa >= 0;
        if (skin_ok)
            skin_ok = read_float_acc(ja, jr, jc, err) && jc == 4 && jr.size() == vn * 4;
        if (skin_ok)
            skin_ok = read_float_acc(wa, wr, wc, err) && wc == 4 && wr.size() == vn * 4;
        if (!skin_ok)
        {
            std::print(stderr, "runtime: gltf '{}' JOINTS_0/WEIGHTS_0 缺失或非法（node={}）→ 回退静态\n",
                       path_, node_idx);
            out_prim.skin = -1;
        }
        else
        {
            const int joint_count = ((size_t)skin_idx < model_.skins.size())
                                        ? (int)model_.skins[(size_t)skin_idx].joints.size()
                                        : 0;
            out_prim.joints.resize(vn * 4);
            out_prim.weights.resize(vn * 4);
            bool bad_joint = false;
            for (size_t i = 0; i < vn; ++i)
            {
                float sum = 0.0F;
                for (int k = 0; k < 4; ++k)
                {
                    int v = (int)jr[i * 4 + (size_t)k];
                    if (v < 0 || v >= joint_count)
                    {
                        bad_joint = true;
                        v = 0;
                    }
                    out_prim.joints[i * 4 + (size_t)k] = (uint16_t)v;
                    const float w = wr[i * 4 + (size_t)k];
                    out_prim.weights[i * 4 + (size_t)k] = w;
                    sum += w;
                }
                if (sum > 1e-8F)
                    for (int k = 0; k < 4; ++k)
                        out_prim.weights[i * 4 + (size_t)k] /= sum;
            }
            if (bad_joint)
                std::print(stderr, "runtime: gltf '{}' JOINTS_0 越界（node={}）→ 已夹到 0\n",
                           path_, node_idx);
        }
    }

    /* ---- morph targets（POSITION 必需；NORMAL 可选；上限 8 与引擎一致）---- */
    const mq_json* targets = jget(prim, "targets");
    const uint32_t nt = mq_json_len(targets);
    if (nt > 0)
    {
        if (nt > 8)
            std::print(stderr, "runtime: gltf '{}' morph targets={} 超过 8（node={}），只取前 8\n",
                       path_, nt, node_idx);
        const uint32_t use = std::min<uint32_t>(nt, 8u);
        out_prim.morph_targets.reserve(use);
        for (uint32_t t = 0; t < use; ++t)
        {
            const mq_json* tgt = jitem(targets, t);
            const int pa = (int)jnum(tgt, "POSITION", -1.0);
            if (pa < 0)
                continue;
            MorphTarget mt;
            int pc = 0;
            if (!read_float_acc(pa, mt.positions, pc, err) || pc != 3 || mt.positions.size() != vn * 3)
            {
                std::print(stderr, "runtime: gltf '{}' morph target[{}] POSITION 非法（node={}）→ 跳过\n",
                           path_, t, node_idx);
                continue;
            }
            const int na = (int)jnum(tgt, "NORMAL", -1.0);
            if (na >= 0)
            {
                int nc = 0;
                std::vector<float> nr;
                if (read_float_acc(na, nr, nc, err) && nc == 3 && nr.size() == vn * 3)
                    mt.normals = std::move(nr);
            }
            out_prim.morph_targets.push_back(std::move(mt));
        }
    }
    if (!out_prim.morph_targets.empty())
    {
        model_.has_morph = true;
        /* 默认权重：node.weights 优先于 mesh.weights；缺省 = 0。 */
        const mq_json* jw = node ? jget(node, "weights") : nullptr;
        if (!jw || mq_json_len(jw) == 0)
            jw = mesh ? jget(mesh, "weights") : nullptr;
        const uint32_t nw = mq_json_len(jw);
        out_prim.morph_weights.resize(out_prim.morph_targets.size(), 0.0F);
        for (uint32_t k = 0; k < out_prim.morph_weights.size() && k < nw; ++k)
            out_prim.morph_weights[k] = (float)mq_json_num(jitem(jw, k), 0.0F);
    }

    out.push_back(std::move(out_prim));
    return true;
}

void Parser::collect_node(int node_idx, std::vector<uint8_t>& visited, int depth)
{
    if (depth > 128)
    {
        std::print(stderr, "runtime: gltf '{}' 节点层级超过 128（node={}），截断\n", path_, node_idx);
        return;
    }
    const mq_json* nodes = jarr(js_, "nodes");
    if (!nodes || node_idx < 0 || (uint32_t)node_idx >= mq_json_len(nodes))
        return;
    if (visited[(size_t)node_idx])
        return;
    visited[(size_t)node_idx] = 1;
    model_.node_order.push_back(node_idx);
    const mq_json* node = jitem(nodes, (uint32_t)node_idx);
    const mq_json* children = jget(node, "children");
    const uint32_t nc = mq_json_len(children);
    for (uint32_t c = 0; c < nc; ++c)
        collect_node((int)mq_json_num(jitem(children, c), -1.0), visited, depth + 1);
}

void Parser::parse_nodes()
{
    const mq_json* nodes = jarr(js_, "nodes");
    const mq_json* meshes = jarr(js_, "meshes");
    const uint32_t nn = nodes ? mq_json_len(nodes) : 0;
    const uint32_t nm = meshes ? mq_json_len(meshes) : 0;

    /* ---- 1) 节点本地 TRS / matrix + 父链接 ---- */
    model_.nodes.resize(nn);
    for (uint32_t i = 0; i < nn; ++i)
    {
        const mq_json* jn = jitem(nodes, i);
        Node& n = model_.nodes[i];
        if (const char* name = jstr(jn, "name", nullptr))
            n.name = name;
        const mq_json* jm = jget(jn, "matrix");
        if (jm && mq_json_len(jm) >= 16)
        {
            n.has_matrix = true;
            for (int k = 0; k < 16; ++k)
                n.matrix[k] = (float)mq_json_num(jitem(jm, (uint32_t)k), k % 5 == 0 ? 1.0 : 0.0);
            mat_decompose_trs(n.matrix, n.translation, n.rotation, n.scale);
        }
        else
        {
            const mq_json* jt = jget(jn, "translation");
            const mq_json* jr = jget(jn, "rotation");
            const mq_json* jsc = jget(jn, "scale");
            for (int k = 0; k < 3; ++k)
            {
                n.translation[k] = (float)mq_json_num(jitem(jt, (uint32_t)k), n.translation[k]);
                n.scale[k] = (float)mq_json_num(jitem(jsc, (uint32_t)k), n.scale[k]);
            }
            for (int k = 0; k < 4; ++k)
                n.rotation[k] = (float)mq_json_num(jitem(jr, (uint32_t)k), n.rotation[k]);
        }
        const mq_json* children = jget(jn, "children");
        const uint32_t nc = mq_json_len(children);
        for (uint32_t c = 0; c < nc; ++c)
        {
            const int ci = (int)mq_json_num(jitem(children, c), -1.0);
            if (ci >= 0 && (uint32_t)ci < nn)
            {
                if (model_.nodes[(size_t)ci].parent < 0)
                    model_.nodes[(size_t)ci].parent = (int)i;
                else if (model_.nodes[(size_t)ci].parent != (int)i)
                    std::print(stderr, "runtime: gltf '{}' 节点 {} 有多个父节点（保留第一个）\n",
                               path_, ci);
            }
        }
    }

    /* KHR_node_visibility：可见性作用到整棵子树（静态；动画可见性未支持）。 */
    for (uint32_t i = 0; i < nn; ++i)
    {
        const mq_json* ve = jget(jget(jitem(nodes, i), "extensions"), "KHR_node_visibility");
        if (ve && !jbool(ve, "visible", true))
            model_.nodes[i].hidden = true;
    }
    for (uint32_t pass = 0; pass < nn; ++pass)
    {
        bool changed = false;
        for (uint32_t i = 0; i < nn; ++i)
        {
            Node& n = model_.nodes[i];
            if (!n.hidden && n.parent >= 0 && (uint32_t)n.parent < nn &&
                model_.nodes[(size_t)n.parent].hidden)
            {
                n.hidden = true;
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    /* ---- 2) 遍历顺序：父先子（世界矩阵累积）；scene 优先，否则无父节点当根 ---- */
    std::vector<uint8_t> visited(nn, 0);
    const mq_json* scenes = jarr(js_, "scenes");
    const int scene_idx = (int)jnum(js_, "scene", 0.0);
    bool any_root = false;
    if (scenes && (uint32_t)scene_idx < mq_json_len(scenes))
    {
        const mq_json* roots = jget(jitem(scenes, (uint32_t)scene_idx), "nodes");
        const uint32_t nr = mq_json_len(roots);
        for (uint32_t r = 0; r < nr; ++r)
        {
            collect_node((int)mq_json_num(jitem(roots, r), -1.0), visited, 0);
            any_root = true;
        }
    }
    if (!any_root)
    {
        for (uint32_t i = 0; i < nn; ++i)
            if (model_.nodes[i].parent < 0)
                collect_node((int)i, visited, 0);
    }
    /* 孤立/循环引用节点也纳入（避免丢几何；深度上限防死循环）。 */
    for (uint32_t i = 0; i < nn; ++i)
        if (!visited[i])
            collect_node((int)i, visited, 0);

    /* ---- 3) 每个 node 的 mesh primitive → 共享顶点（mesh 空间）---- */
    for (int idx : model_.node_order)
    {
        if ((size_t)idx < model_.nodes.size() && model_.nodes[(size_t)idx].hidden)
            continue;
        const mq_json* jn = jitem(nodes, (uint32_t)idx);
        const int mesh_idx = (int)jnum(jn, "mesh", -1.0);
        if (mesh_idx < 0 || (uint32_t)mesh_idx >= nm)
            continue;
        const mq_json* mesh = jitem(meshes, (uint32_t)mesh_idx);
        int skin_idx = (int)jnum(jn, "skin", -1.0);
        if (skin_idx < 0 || (size_t)skin_idx >= model_.skins.size())
            skin_idx = -1;
        if (skin_idx >= 0)
            model_.has_skin = true;
        const mq_json* prims = jget(mesh, "primitives");
        const uint32_t np = mq_json_len(prims);
        for (uint32_t p = 0; p < np; ++p)
            build_primitive(jitem(prims, p), mesh, jn, idx, skin_idx, model_.primitives);
    }

    /* 无 nodes 的极简文件：每个 mesh 单位变换导入。 */
    if (nn == 0)
    {
        for (uint32_t m = 0; m < nm; ++m)
        {
            const mq_json* mesh = jitem(meshes, m);
            const mq_json* prims = jget(mesh, "primitives");
            const uint32_t np = mq_json_len(prims);
            for (uint32_t p = 0; p < np; ++p)
                build_primitive(jitem(prims, p), mesh, nullptr, -1, -1, model_.primitives);
        }
    }
}

/* EXT_mesh_gpu_instancing：节点带 TRANSLATION/ROTATION/SCALE 实例 accessor 时，把该节点的
 * primitive 展开成 N 份，每份挂到一个合成子节点（世界 = 原节点世界 × 实例 TRS，与 importer 一致）。 */
void Parser::expand_instancing()
{
    const mq_json* jnodes = jarr(js_, "nodes");
    const uint32_t nn = jnodes ? mq_json_len(jnodes) : 0;
    if (nn == 0)
        return;
    const size_t base_n = model_.nodes.size();
    std::vector<int> inst_start(base_n, -1);
    std::vector<int> inst_count(base_n, 0);
    std::string err;
    for (uint32_t i = 0; i < nn && i < base_n; ++i)
    {
        if (model_.nodes[i].hidden)
            continue;
        const mq_json* ext = jget(jget(jitem(jnodes, i), "extensions"), "EXT_mesh_gpu_instancing");
        if (!ext)
            continue;
        const mq_json* attrs = jget(ext, "attributes");
        if (!attrs)
            continue;
        auto read_attr = [&](const char* key, int want, std::vector<float>& out)
        {
            out.clear();
            const int ai = (int)jnum(attrs, key, -1.0);
            if (ai < 0)
                return 0;
            int comps = 0;
            if (!read_float_acc(ai, out, comps, err) || comps != want)
            {
                out.clear();
                return 0;
            }
            return (int)(out.size() / (size_t)want);
        };
        std::vector<float> tr, rot, sc;
        const int nt = read_attr("TRANSLATION", 3, tr);
        const int nr = read_attr("ROTATION", 4, rot);
        const int ns = read_attr("SCALE", 3, sc);
        int n = std::max({nt, nr, ns});
        if (n <= 0)
            continue;
        if (n > 4096)
        {
            std::print(stderr, "runtime: gltf '{}' node[{}] 实例数 {} 超限，截断到 4096\n",
                       path_, i, n);
            n = 4096;
        }
        inst_start[i] = (int)model_.nodes.size();
        inst_count[i] = n;
        for (int k = 0; k < n; ++k)
        {
            Node nd;
            nd.name = model_.nodes[i].name + "#inst" + std::to_string(k);
            nd.parent = (int)i;
            if (nt > 0 && k < nt)
                for (int c = 0; c < 3; ++c)
                    nd.translation[c] = tr[(size_t)k * 3 + c];
            if (nr > 0 && k < nr)
                for (int c = 0; c < 4; ++c)
                    nd.rotation[c] = rot[(size_t)k * 4 + c];
            if (ns > 0 && k < ns)
                for (int c = 0; c < 3; ++c)
                    nd.scale[c] = sc[(size_t)k * 3 + c];
            model_.nodes.push_back(nd);
            model_.node_order.push_back((int)model_.nodes.size() - 1);
        }
    }
    bool any = false;
    for (int c : inst_count)
        any = any || c > 0;
    if (!any)
        return;
    std::vector<Primitive> old = std::move(model_.primitives);
    model_.primitives.clear();
    model_.primitives.reserve(old.size());
    for (Primitive& p : old)
    {
        if (p.node >= 0 && (size_t)p.node < base_n && inst_count[(size_t)p.node] > 0)
        {
            for (int k = 0; k < inst_count[(size_t)p.node]; ++k)
            {
                Primitive copy = p;
                copy.node = inst_start[(size_t)p.node] + k;
                model_.primitives.push_back(std::move(copy));
            }
        }
        else
        {
            model_.primitives.push_back(std::move(p));
        }
    }
}

void Parser::parse_skins()
{
    const mq_json* arr = jarr(js_, "skins");
    const uint32_t n = arr ? mq_json_len(arr) : 0;
    model_.skins.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const mq_json* jsk = jitem(arr, i);
        Skin sk;
        if (const char* name = jstr(jsk, "name", nullptr))
            sk.name = name;
        const mq_json* joints = jget(jsk, "joints");
        const uint32_t nj = mq_json_len(joints);
        sk.joints.reserve(nj);
        for (uint32_t k = 0; k < nj; ++k)
            sk.joints.push_back((int)mq_json_num(jitem(joints, k), -1.0));
        sk.inverse_bind.assign((size_t)nj * 16u, 0.0F);
        for (uint32_t j = 0; j < nj; ++j)
        {
            float* m = &sk.inverse_bind[(size_t)j * 16u];
            m[0] = m[5] = m[10] = m[15] = 1.0F;
        }
        const int ibm = (int)jnum(jsk, "inverseBindMatrices", -1.0);
        if (ibm >= 0)
        {
            std::vector<float> data;
            int comps = 0;
            std::string err;
            if (read_float_acc(ibm, data, comps, err) && comps == 16 && data.size() == nj * 16u)
                sk.inverse_bind = std::move(data);
            else
                std::print(stderr, "runtime: gltf '{}' skin[{}] inverseBindMatrices 非法（{}）→ 用单位阵\n",
                           path_, i, err);
        }
        model_.skins[i] = std::move(sk);
    }
}

/* KHR_animation_pointer → 材质字段（只认 /materials/<i>/... 的常用数值属性）。 */
static bool parse_pointer_material(const std::string& p, int& material, int& field, int& comps)
{
    const std::string prefix = "/materials/";
    if (p.rfind(prefix, 0) != 0)
        return false;
    size_t pos = prefix.size();
    int m = 0;
    bool any_digit = false;
    while (pos < p.size() && p[pos] >= '0' && p[pos] <= '9')
    {
        m = m * 10 + (p[pos] - '0');
        ++pos;
        any_digit = true;
    }
    if (!any_digit || pos >= p.size() || p[pos] != '/')
        return false;
    const std::string rest = p.substr(pos);
    struct Entry
    {
        const char* path;
        int field;
        int comps;
    };
    static const Entry kEntries[] = {
        {"/pbrMetallicRoughness/baseColorFactor", PF_BASE_COLOR, 4},
        {"/pbrMetallicRoughness/metallicFactor", PF_METALLIC, 1},
        {"/pbrMetallicRoughness/roughnessFactor", PF_ROUGHNESS, 1},
        {"/emissiveFactor", PF_EMISSIVE, 3},
        {"/alphaCutoff", PF_ALPHA_CUTOFF, 1},
        {"/extensions/KHR_materials_emissive_strength/emissiveStrength", PF_EMISSIVE_STRENGTH, 1},
        {"/extensions/KHR_materials_clearcoat/clearcoatFactor", PF_CLEARCOAT, 1},
        {"/extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor", PF_CLEARCOAT_ROUGHNESS, 1},
        {"/extensions/KHR_materials_sheen/sheenColorFactor", PF_SHEEN_COLOR, 3},
        {"/extensions/KHR_materials_sheen/sheenRoughnessFactor", PF_SHEEN_ROUGHNESS, 1},
        {"/extensions/KHR_materials_ior/ior", PF_IOR, 1},
        {"/extensions/KHR_materials_transmission/transmissionFactor", PF_TRANSMISSION, 1},
        {"/extensions/KHR_materials_volume/thicknessFactor", PF_VOLUME_THICKNESS, 1},
        {"/extensions/KHR_materials_volume/attenuationDistance", PF_ATTENUATION_DISTANCE, 1},
        {"/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",
         PF_DIFFUSE_TRANSMISSION, 1},
        {"/extensions/KHR_materials_specular/specularFactor", PF_SPECULAR, 1},
        {"/extensions/KHR_materials_iridescence/iridescenceFactor", PF_IRIDESCENCE, 1},
        {"/extensions/KHR_materials_anisotropy/anisotropyStrength", PF_ANISOTROPY, 1},
        {"/extensions/KHR_materials_dispersion/dispersion", PF_DISPERSION, 1},
    };
    for (const Entry& e : kEntries)
    {
        if (rest == e.path)
        {
            material = m;
            field = e.field;
            comps = e.comps;
            return true;
        }
    }
    return false;
}

void Parser::parse_animations()
{
    const mq_json* arr = jarr(js_, "animations");
    const uint32_t n = arr ? mq_json_len(arr) : 0;
    model_.animations.reserve(n);
    for (uint32_t ai = 0; ai < n; ++ai)
    {
        const mq_json* jan = jitem(arr, ai);
        Animation anim;
        if (const char* name = jstr(jan, "name", nullptr))
            anim.name = name;
        else
            anim.name = "anim" + std::to_string(ai);
        const mq_json* samplers = jget(jan, "samplers");
        const mq_json* channels = jget(jan, "channels");
        const uint32_t ns = mq_json_len(samplers);
        const uint32_t nc = mq_json_len(channels);
        int skipped = 0;
        for (uint32_t c = 0; c < nc; ++c)
        {
            const mq_json* ch = jitem(channels, c);
            const mq_json* target = jget(ch, "target");
            if (!target)
                continue;
            int node = (int)jnum(target, "node", -1.0);
            int path = -1;
            int pointer_material = -1;
            int pointer_field = PF_NONE;
            int pointer_comps = 0;
            const char* pointer_str = jstr(target, "pointer", nullptr);
            if (pointer_str)
            {
                /* KHR_animation_pointer：目标不是节点，而是材质 JSON 指针。 */
                if (!parse_pointer_material(pointer_str, pointer_material, pointer_field,
                                            pointer_comps))
                {
                    ++skipped;
                    continue;
                }
                path = 4;
            }
            else
            {
                if (node < 0)
                    continue;
                const char* path_str = jstr(target, "path", "");
                if (std::strcmp(path_str, "translation") == 0)
                    path = 0;
                else if (std::strcmp(path_str, "rotation") == 0)
                    path = 1;
                else if (std::strcmp(path_str, "scale") == 0)
                    path = 2;
                else if (std::strcmp(path_str, "weights") == 0)
                    path = 3;
                else
                {
                    ++skipped;
                    continue;
                }
            }
            const int si = (int)jnum(ch, "sampler", -1.0);
            if (si < 0 || (uint32_t)si >= ns)
                continue;
            const mq_json* smp = jitem(samplers, (uint32_t)si);
            const int in_acc = (int)jnum(smp, "input", -1.0);
            const int out_acc = (int)jnum(smp, "output", -1.0);
            if (in_acc < 0 || out_acc < 0)
                continue;
            std::vector<float> times;
            int tc = 0;
            std::string err;
            if (!read_float_acc(in_acc, times, tc, err) || tc != 1 || times.empty())
            {
                std::print(stderr, "runtime: gltf '{}' animation[{}] input 非法（{}）→ 跳过通道\n",
                           path_, ai, err);
                continue;
            }
            const char* interp_str = jstr(smp, "interpolation", "LINEAR");
            int interp = 1;
            if (std::strcmp(interp_str, "STEP") == 0)
                interp = 0;
            else if (std::strcmp(interp_str, "CUBICSPLINE") == 0)
                interp = 2;
            std::vector<float> values;
            int vc = 0;
            if (!read_float_acc(out_acc, values, vc, err))
            {
                std::print(stderr, "runtime: gltf '{}' animation[{}] output 非法（{}）→ 跳过通道\n",
                           path_, ai, err);
                continue;
            }
            const size_t keys = times.size();
            int comps = vc;
            if (path == 4)
            {
                comps = pointer_comps;
                const size_t need = keys * (interp == 2 ? 3u : 1u) * (size_t)comps;
                if (vc != comps || values.size() != need)
                {
                    std::print(stderr, "runtime: gltf '{}' animation[{}] pointer 分量/值数非法\n",
                               path_, ai);
                    continue;
                }
            }
            else if (path == 3)
            {
                /* weights：output accessor 是 SCALAR，扁平值数 = keys × targets × (cubic?3:1)。 */
                const size_t denom = keys * (interp == 2 ? 3u : 1u);
                if (denom == 0 || values.size() % denom != 0)
                {
                    std::print(stderr, "runtime: gltf '{}' animation[{}] weights 值数不是关键帧整数倍 → 跳过\n",
                               path_, ai);
                    continue;
                }
                comps = (int)(values.size() / denom);
                if (comps <= 0)
                    continue;
            }
            else
            {
                const int need_comps = (path == 1) ? 4 : 3;
                if (vc != need_comps)
                {
                    std::print(stderr, "runtime: gltf '{}' animation[{}] path={} 分量数 {} != {}\n",
                               path_, ai, path, vc, need_comps);
                    continue;
                }
                const size_t need = keys * (interp == 2 ? 3u : 1u) * (size_t)vc;
                if (values.size() != need)
                {
                    std::print(stderr, "runtime: gltf '{}' animation[{}] 关键帧值数 {} != {}\n",
                               path_, ai, values.size(), need);
                    continue;
                }
            }
            AnimationChannel out_ch;
            out_ch.node = node;
            out_ch.path = path;
            out_ch.interp = interp;
            out_ch.comps = comps;
            out_ch.pointer_material = pointer_material;
            out_ch.pointer_field = pointer_field;
            out_ch.times = std::move(times);
            out_ch.values = std::move(values);
            anim.duration = std::max(anim.duration, out_ch.times.back());
            anim.channels.push_back(std::move(out_ch));
        }
        if (skipped > 0)
            std::print(stderr, "runtime: gltf '{}' animation '{}' 跳过 {} 个不支持通道（pointer）\n",
                       path_, anim.name, skipped);
        if (!anim.channels.empty())
            model_.animations.push_back(std::move(anim));
    }
    model_.has_animation = !model_.animations.empty();
}

void Parser::parse_lights_cameras()
{
    const mq_json* nodes = jarr(js_, "nodes");
    const uint32_t nn = nodes ? mq_json_len(nodes) : 0;
    if (nn == 0)
        return;

    /* KHR_lights_punctual：顶层定义 + 节点引用 → 每个节点实例一条 Light。 */
    const mq_json* root_exts = jget(js_, "extensions");
    const mq_json* lp = root_exts ? jget(root_exts, "KHR_lights_punctual") : nullptr;
    const mq_json* defs = lp ? jget(lp, "lights") : nullptr;
    const uint32_t nl = mq_json_len(defs);
    for (uint32_t i = 0; i < nn; ++i)
    {
        const mq_json* jn = jitem(nodes, i);
        const mq_json* ne = jget(jn, "extensions");
        const mq_json* ln = ne ? jget(ne, "KHR_lights_punctual") : nullptr;
        const int li = ln ? (int)jnum(ln, "light", -1.0) : -1;
        if (li < 0 || (uint32_t)li >= nl)
            continue;
        const mq_json* def = jitem(defs, (uint32_t)li);
        Light L;
        L.node = (int)i;
        if ((size_t)L.node < model_.nodes.size() && model_.nodes[(size_t)L.node].hidden)
            continue;
        const char* type = jstr(def, "type", "directional");
        if (std::strcmp(type, "point") == 0)
            L.type = 1;
        else if (std::strcmp(type, "spot") == 0)
            L.type = 2;
        const mq_json* col = jget(def, "color");
        for (int k = 0; k < 3; ++k)
            L.color[k] = (float)mq_json_num(jitem(col, (uint32_t)k), L.color[k]);
        L.intensity = (float)jnum(def, "intensity", L.intensity);
        L.range = (float)jnum(def, "range", 0.0);
        const mq_json* spot = jget(def, "spot");
        if (spot)
        {
            L.inner_angle = (float)jnum(spot, "innerConeAngle", 0.0);
            L.outer_angle = (float)jnum(spot, "outerConeAngle", 0.7853981634);
        }
        model_.lights.push_back(L);
    }

    /* glTF cameras（只透传 perspective；ortho 记录但 runtime 回退自动机位）。 */
    const mq_json* cams = jarr(js_, "cameras");
    const uint32_t nc = cams ? mq_json_len(cams) : 0;
    for (uint32_t i = 0; i < nn; ++i)
    {
        const mq_json* jn = jitem(nodes, i);
        const int ci = (int)jnum(jn, "camera", -1.0);
        if (ci < 0 || (uint32_t)ci >= nc)
            continue;
        const mq_json* def = jitem(cams, (uint32_t)ci);
        Camera c;
        c.node = (int)i;
        if ((size_t)c.node < model_.nodes.size() && model_.nodes[(size_t)c.node].hidden)
            continue;
        const mq_json* persp = jget(def, "perspective");
        if (persp)
        {
            c.perspective = true;
            c.yfov = (float)jnum(persp, "yfov", c.yfov);
            c.znear = (float)jnum(persp, "znear", c.znear);
            c.zfar = (float)jnum(persp, "zfar", c.zfar);
            c.aspect = (float)jnum(persp, "aspectRatio", 0.0);
        }
        else
        {
            c.perspective = false;
            const mq_json* ortho = jget(def, "orthographic");
            if (ortho)
            {
                c.znear = (float)jnum(ortho, "znear", c.znear);
                c.zfar = (float)jnum(ortho, "zfar", c.zfar);
            }
        }
        model_.cameras.push_back(c);
    }
}

bool Parser::run(const std::vector<uint8_t>& bytes)
{
    std::vector<uint8_t> bin_chunk;
    if (!parse_json(bytes, bin_chunk))
        return false;
    parse_buffers();
    if (!parse_materials())
        return false;
    model_.materials = materials_;
    parse_skins();
    parse_variants();
    parse_nodes();
    expand_instancing();
    parse_animations();
    parse_lights_cameras();
    if (model_.primitives.empty() && model_.lights.empty() && model_.cameras.empty())
    {
        model_.error = "没有可导入的三角网格/灯光/相机";
        return false;
    }
    return true;
}

} // namespace

bool is_gltf_path(const std::string& path)
{
    auto ends_with_ci = [&](const char* ext)
    {
        const size_t n = std::strlen(ext);
        if (path.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i)
        {
            char a = path[path.size() - n + i];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a - 'A' + 'a');
            if (a != ext[i])
                return false;
        }
        return true;
    };
    return ends_with_ci(".gltf") || ends_with_ci(".glb");
}

Model parse(const std::string& path, const std::vector<uint8_t>& bytes, const ReadFn& read)
{
    Model model;
    Parser p(path, read, model);
    if (!p.run(bytes))
    {
        model.failed = true;
        if (model.error.empty())
            model.error = "未知错误";
    }
    return model;
}

} // namespace gltf
