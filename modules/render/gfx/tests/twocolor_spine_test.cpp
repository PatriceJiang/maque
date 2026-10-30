// 两色着色（TintBlack）**官方素材 → 像素**的端到端断言。
//
// 为什么要单独一个测试（`gfx_twocolor` 已经逐通道验过公式）：
//   `gfx_twocolor` 用的是自造网格 + 自造 dark 值，证明的是"**给定** dark 之后 GPU 算得对"。
//   它还差一环：**官方素材里的 dark 到底有没有真的传下来**（spine JSON → spine-cpp 的
//   SlotData::darkColor/hasDarkColor → spine2d 的 dark_colors → gfx 顶点 → 像素）。
//   本测试把这一环钉死，用的是**官方素材**而不是自造骨架：
//     `tests/data/spine/alien-pro.json`（spine-runtimes 4.2 examples/alien）
//   选它是因为**只有它的 dark 不是黑的**（脚本扫描官方全部 50 个 JSON 的 slot `dark` 键）：
//     · alien: 3 个槽带 dark，其中 `eyeball` 是 light=dfa921ff / dark=c57c1e（**唯一非黑**）
//     · coin: 1 个槽，dark=000000（与默认黑等价 → 像素零差异）
//     · tank: 52 个槽，dark 全是 000000（同上）
//     · raptor / spineboy(batching): **0 个 dark** → 开关两色着色像素完全一样，做不了断言
//
// 预期值为什么能"精确"算出来：所有像素断言都用 **1x1 页纹理**。
//   1x1 + ClampToEdge ⇒ 无论 UV 落在哪、无论线性还是最近邻，采样到的都是**那唯一一个纹素**，
//   `texColor` 全局恒定。于是官方片元公式
//       rgb = ((tex.a - 1)*dark.a + 1 - tex.rgb) * dark.rgb + tex.rgb * light.rgb
//   在 tex.a = dark.a = 1 时退化成 `rgb = (1 - tex.rgb) * dark.rgb + tex.rgb * light.rgb`，
//   而 light/dark 都是素材里给定的常量（spine2d 已把逐槽 light/dark 打到顶点），
//   所以每个像素的颜色可以手算到字节。
//
// ⚠️ 只渲染**含 eyeball 的那一个批次**，不是整只骨架 —— 这是刻意的：
//   1x1 纹理会把**每个附件都变成不透明的纯色块**，于是"后画的批次盖住先画的"就成了正确行为，
//   而 eyeball 在精灵绘制序里靠前，会被后面的身体/头部批次整片盖掉（实测：全黑纹理下
//   整只合并渲染的非黑像素为 0，单独画该批次为 9187 —— 不是引擎的 bug，是本测试的设计陷阱）。
//   颜色断言只关心"这个批次的像素是不是 dark→light 插值出来的正确颜色"，所以按批隔离渲染。
//   整只骨架的渲染另有一条 sanity 断言（白色纹理下必须出大量像素），确保网格路径本身没问题。
//
// 断言（覆盖 tex.rgb = 0 / 1 / 中灰，即 dark→light 插值的两端与中点）：
//   1. dark 通道内容 == 官方 JSON 的 eyeball 槽 c57c1e；其余顶点为默认黑 (0,0,0,1)。
//   2. 全黑纹理（tex.rgb=0）⇒ 该批次**只**出现一种非黑颜色 (197,124,30) = dark 的字节值；
//      关掉 dark 时整幅全黑。
//   3. 全白纹理（tex.rgb=1）⇒ 开/关整幅**逐字节相同**（公式只剩 light 一路）。
//   4. 中灰纹理（tex.rgb=128/255）⇒ 开/关之差 = (1-tex.rgb) × dark，逐通道精确。
//   5. dark=(0,0,0,1) 与 dark=nullptr 整幅逐字节相同（退化性质）。
#include "maque_ifaces.h"
#include "core/module/mq_module.h"
#include "anim/mq_spine2d.h"

#include <print>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <set>
#include <tuple>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print(stderr, "FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

constexpr int TW = 512, TH = 512;

/* 官方 alien 的 eyeball 槽（alien-pro.json → slots[eyeball]）：light=dfa921ff / dark=c57c1e */
constexpr int AD_R = 0xc5, AD_G = 0x7c, AD_B = 0x1e;

struct Buf {
    std::vector<uint8_t> px = std::vector<uint8_t>((size_t)TW * TH * 4);
    const uint8_t* at(size_t i) const { return &px[i * 4]; }
    bool all_black_rgb() const {
        for (size_t i = 0; i < (size_t)TW * TH; ++i)
            if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2]) return false;
        return true;
    }
    int nonblack() const {
        int n = 0;
        for (size_t i = 0; i < (size_t)TW * TH; ++i)
            if (px[i * 4] || px[i * 4 + 1] || px[i * 4 + 2]) ++n;
        return n;
    }
    std::set<std::tuple<int, int, int>> colors() const {
        std::set<std::tuple<int, int, int>> s;
        for (size_t i = 0; i < (size_t)TW * TH; ++i)
            s.insert({ (int)px[i * 4], (int)px[i * 4 + 1], (int)px[i * 4 + 2] });
        return s;
    }
};

bool read_file(const std::string& p, std::vector<char>& out) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)std::max(0L, n));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

int fake_tex_cb(void*, const char*, const void*, size_t, uint64_t* out_texture) {
    *out_texture = 1;                 // 非零即可（纹理由本测试自己造）
    return MQ_SPINE_OK;
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/spine";

    /* ---- 1) 官方 alien 素材 → spine2d 绘制数据 ---- */
    std::vector<char> sk, at;
    if (!read_file(dir + "/alien-pro.json", sk) || !read_file(dir + "/alien.atlas", at)) {
        std::print("twocolor_spine_test: SKIP (缺少 {}/alien-pro.json)\n", dir);
        return 0;                                        // SKIP：素材不在时不误报失败
    }
    sk.push_back('\0'); at.push_back('\0');
    mq_spine_data* d = mq_spine_data_create(sk.data(), sk.size() - 1, 0, at.data(), "",
                                            fake_tex_cb, nullptr);
    CHECK(d != nullptr);
    if (!d) return 1;
    std::print("twocolor_spine_test: alien-pro.json 页数={} 动画数={} 皮肤数={}\n",
               mq_spine_data_page_count(d), mq_spine_data_animation_count(d),
               mq_spine_data_skin_count(d));

    mq_spine_instance* in = mq_spine_instance_create(d);
    CHECK(in != nullptr);
    if (!in) return 1;

    std::vector<float> verts(16384), uvs(16384), cols(32768), darks(32768);
    std::vector<uint32_t> idxs(65536);
    std::vector<mq_spine_batch> batches(512);
    mq_spine_draw dr{};
    const int rc = mq_spine_instance_draw(in, verts.data(), uvs.data(), cols.data(), darks.data(),
                                          idxs.data(), batches.data(),
                                          (int)(verts.size() / 2), (int)idxs.size(),
                                          (int)batches.size(), &dr);
    CHECK(rc == MQ_SPINE_OK);
    std::print("twocolor_spine_test: 绘制 {} 顶点 / {} 索引 / {} 批次（setup 姿势）\n",
               dr.vertex_count, dr.index_count, dr.batch_count);
    CHECK(dr.vertex_count > 0 && dr.index_count > 0 && dr.batch_count > 0);

    /* ---- 2) dark 通道内容：必须来自官方 JSON 的 eyeball 槽 ---- */
    int n_tinted = 0, n_plain = 0;
    std::vector<uint8_t> tinted(dr.vertex_count, 0);
    for (int i = 0; i < dr.vertex_count; ++i) {
        const float r = darks[(size_t)i * 4], g = darks[(size_t)i * 4 + 1], b = darks[(size_t)i * 4 + 2];
        const float a = darks[(size_t)i * 4 + 3];
        if (r == 0.0F && g == 0.0F && b == 0.0F) { ++n_plain; CHECK(a == 1.0F); continue; }
        ++n_tinted; tinted[(size_t)i] = 1;
        CHECK(std::abs(r - (float)AD_R / 255.0F) < 1e-6F);   // c5
        CHECK(std::abs(g - (float)AD_G / 255.0F) < 1e-6F);   // 7c
        CHECK(std::abs(b - (float)AD_B / 255.0F) < 1e-6F);   // 1e
        CHECK(a == 1.0F);                                     // 官方：darkColor 的 alpha 恒为 1
    }
    std::print("twocolor_spine_test: dark 非黑顶点 {} 个 / 全黑顶点 {} 个（总 {}）\n",
               n_tinted, n_plain, dr.vertex_count);
    CHECK(n_tinted > 0);            // 官方素材**真的**启用了 TintBlack（不是"开关没差别"）
    CHECK(n_plain > n_tinted);      // 只有 eyeball 一个槽带非黑 dark

    /* 找出承载 eyeball 的那个批次（按批隔离渲染，见文件头"⚠️"） */
    int tb = -1;
    for (int b = 0; b < dr.batch_count; ++b) {
        const uint32_t f = batches[(size_t)b].first_index, n = batches[(size_t)b].index_count;
        int cnt = 0;
        for (uint32_t t = 0; t < n; ++t) if (tinted[idxs[(size_t)f + t]]) ++cnt;
        if (cnt > 0) {
            CHECK(tb < 0);          // 期望只有一个批次带 tinted 顶点
            tb = b;
        }
    }
    CHECK(tb >= 0);
    if (tb < 0) return 1;
    std::print("twocolor_spine_test: eyeball 落在批次 {}（blend={} index [{}, +{})）\n",
               tb, batches[(size_t)tb].blend, batches[(size_t)tb].first_index,
               batches[(size_t)tb].index_count);

    /* ---- 3) 打开 gfx 插件 ---- */
    mq_host* host = mq_host_create();
    mq_loaded_module* mod = nullptr;
    if (mq_host_load(host, MQ_GFX_PATH, &mod) != 0) { std::print("twocolor_spine_test: SKIP (no gfx plugin)\n"); return 0; }
    auto* api = (const mq_renderer_v2*)mq_host_query(host, MQ_RENDERER_API_NAME, MQ_RENDERER_API_VERSION);
    CHECK(api != nullptr);
    if (!api) return 1;
    const char* backend = nullptr;
    void* self = api->create(&backend);
    CHECK(self != nullptr);
    if (!self) return 1;
    std::print("twocolor_spine_test: gfx backend='{}'\n", backend ? backend : "?");

    /* 相机：把**实际顶点**的包围盒居中并缩放填满画布（不用 dr.bounds_*：那是姿势包围盒，
     * 与逐顶点范围不一定重合，差一点就会把整只骨架推出视口 → 全黑图且静默）。 */
    float vminx = 1e30F, vmaxx = -1e30F, vminy = 1e30F, vmaxy = -1e30F;
    for (int i = 0; i < dr.vertex_count; ++i) {
        vminx = std::min(vminx, verts[(size_t)i * 2]); vmaxx = std::max(vmaxx, verts[(size_t)i * 2]);
        vminy = std::min(vminy, verts[(size_t)i * 2 + 1]); vmaxy = std::max(vmaxy, verts[(size_t)i * 2 + 1]);
    }
    const float zoom = std::min((float)TW * 0.92F / std::max(vmaxx - vminx, 1.0F),
                                (float)TH * 0.92F / std::max(vmaxy - vminy, 1.0F));
    const float cam_x = (vminx + vmaxx) * 0.5F, cam_y = (vminy + vmaxy) * 0.5F;

    mq_renderer_view view{};
    view.vw = TW; view.vh = TH; view.cx = cam_x; view.cy = cam_y; view.zoom = zoom;
    const float bg[4] = { 0, 0, 0, 1 };

    auto make_tex = [&](int v) {
        const uint8_t t[4] = { (uint8_t)v, (uint8_t)v, (uint8_t)v, 255 };
        return api->create_texture(self, t, 1, 1);
    };

    /* 单个批次 → 一个网格（与 runtime::spine_extract 的每批一条 mq_renderer_mesh 同构） */
    auto one_batch = [&](int dark_mode) {           // 0=nullptr 1=真实 dark 2=显式(0,0,0,1)
        static std::vector<float> zero_dark;
        zero_dark.assign((size_t)dr.vertex_count * 4, 0.0F);
        for (int i = 0; i < dr.vertex_count; ++i) zero_dark[(size_t)i * 4 + 3] = 1.0F;
        mq_renderer_mesh m{};
        m.vertices = verts.data(); m.uvs = uvs.data(); m.colors = cols.data();
        m.dark_colors = (dark_mode == 0) ? nullptr : (dark_mode == 1 ? darks.data() : zero_dark.data());
        m.indices = idxs.data();
        m.vertex_count = (uint32_t)dr.vertex_count;
        m.first_index = batches[(size_t)tb].first_index;
        m.index_count = batches[(size_t)tb].index_count;
        m.blend = batches[(size_t)tb].blend;
        m.pma = 0;
        return m;
    };

    auto draw = [&](int texel, int dark_mode, Buf& out_buf) {
        mq_renderer_mesh m = one_batch(dark_mode);
        m.texture = make_tex(texel);
        out_buf.px.assign((size_t)TW * TH * 4, 0);
        const int r = api->render_mesh_views(self, TW, TH, &view, 1, nullptr, 0, &m, 1,
                                             bg, out_buf.px.data(), (uint32_t)out_buf.px.size());
        api->destroy_texture(self, m.texture);
        return r == 0;
    };

    /* ---- 4) 全黑纹理（tex.rgb=0）：该批次的非黑像素只应是 dark 的字节值 ---- */
    {
        Buf on, off;
        CHECK(draw(0, 1, on));       // 带官方 dark
        CHECK(draw(0, 0, off));      // 不给 dark（= 不支持两色着色的旧行为）
        CHECK(off.all_black_rgb());  // tex.rgb=0 ⇒ t*light = 0，整批全黑
        auto cs = on.colors();
        cs.erase({ 0, 0, 0 });
        std::print("  全黑纹理: 开 dark 后非黑颜色 {} 种:", cs.size());
        for (auto& c : cs) std::print(" ({},{},{})", std::get<0>(c), std::get<1>(c), std::get<2>(c));
        std::print("；覆盖像素 {} 个；关 dark 非黑像素 {} 个\n", on.nonblack(), off.nonblack());
        CHECK(cs.size() == 1);
        if (cs.size() == 1) {
            const auto c = *cs.begin();
            CHECK(std::get<0>(c) == AD_R);      // 197
            CHECK(std::get<1>(c) == AD_G);      // 124
            CHECK(std::get<2>(c) == AD_B);      //  30
        }
        CHECK(on.nonblack() > 0);
        CHECK(off.nonblack() == 0);
    }

    /* ---- 5) 全白纹理（tex.rgb=1）：开/关整幅逐字节相同（公式只剩 light 一路）---- */
    {
        Buf on, off;
        CHECK(draw(255, 1, on));
        CHECK(draw(255, 0, off));
        std::print("  全白纹理: 开/关整幅逐字节相同 = {}（非黑像素 {}）\n",
                   on.px == off.px ? "是" : "否", on.nonblack());
        CHECK(on.px == off.px);
        CHECK(on.nonblack() > 0);
    }

    /* ---- 6) 中灰纹理（tex.rgb=128/255）：开/关之差 = (1-tex.rgb) × dark ---- */
    {
        Buf on, off;
        CHECK(draw(128, 1, on));
        CHECK(draw(128, 0, off));
        const double t = 128.0 / 255.0;
        const int exp_d[3] = { (int)std::lround((1.0 - t) * AD_R),
                               (int)std::lround((1.0 - t) * AD_G),
                               (int)std::lround((1.0 - t) * AD_B) };
        int maxd[3] = { 0, 0, 0 };
        for (size_t i = 0; i < (size_t)TW * TH; ++i)
            for (int c = 0; c < 3; ++c)
                maxd[c] = std::max(maxd[c], std::abs((int)on.at(i)[c] - (int)off.at(i)[c]));
        std::print("  中灰纹理: 开/关最大逐通道差 = ({},{},{})；解析 (1-128/255)×dark = ({},{},{})\n",
                   maxd[0], maxd[1], maxd[2], exp_d[0], exp_d[1], exp_d[2]);
        for (int c = 0; c < 3; ++c) CHECK(std::abs(maxd[c] - exp_d[c]) <= 1);
        CHECK(on.px != off.px);
    }

    /* ---- 7) 退化性质：dark=(0,0,0,1) 与不给 dark 整幅逐字节相同 ---- */
    {
        Buf a, b;
        CHECK(draw(128, 2, a));
        CHECK(draw(128, 0, b));
        std::print("  dark=(0,0,0,1) 与 dark=nullptr 整幅逐字节相同 = {}\n", a.px == b.px ? "是" : "否");
        CHECK(a.px == b.px);
    }

    /* ---- 8) sanity：整只骨架（全部批次）在白纹理下必须出大量像素（网格路径本身没问题）---- */
    {
        std::vector<mq_renderer_mesh> all;
        const int tex = make_tex(255);
        for (int b = 0; b < dr.batch_count; ++b) {
            mq_renderer_mesh m{};
            m.vertices = verts.data(); m.uvs = uvs.data(); m.colors = cols.data();
            m.dark_colors = darks.data(); m.indices = idxs.data();
            m.vertex_count = (uint32_t)dr.vertex_count;
            m.first_index = batches[(size_t)b].first_index;
            m.index_count = batches[(size_t)b].index_count;
            m.blend = batches[(size_t)b].blend;
            m.pma = 0; m.texture = tex;
            all.push_back(m);
        }
        Buf whole;
        whole.px.assign((size_t)TW * TH * 4, 0);
        CHECK(api->render_mesh_views(self, TW, TH, &view, 1, nullptr, 0, all.data(),
                                     (uint32_t)all.size(), bg, whole.px.data(),
                                     (uint32_t)whole.px.size()) == 0);
        std::print("  整只骨架（白纹理）非黑像素 = {}\n", whole.nonblack());
        CHECK(whole.nonblack() > 10000);
        api->destroy_texture(self, tex);
    }

    api->destroy(self);
    mq_host_destroy(host);
    mq_spine_instance_destroy(in);
    mq_spine_data_destroy(d);

    /* ---- 9) 反向证据：**没有** TintBlack 的官方素材，dark 必须全是默认黑 (0,0,0,1)。
     * 这条把"两色着色不会改变既有 demo"钉成可复跑的断言（dragon 的 flying 动画就是没启用
     * TintBlack 的典型；它的 dark 恒为默认黑 ⇒ 片元公式精确退化成 `tex * light`）。 */
    {
        std::vector<char> dsk, dat;
        if (read_file(dir + "/dragon-ess.skel", dsk) && read_file(dir + "/dragon-pma.atlas", dat)) {
            /* atlas 是 C 字符串（`mq_spine_data_create` 只收 `const char*`，没有长度）→ **必须**补 NUL，
             * 否则解析器 strlen 会读越界（ASan 实测：READ of size 2034 after 2033-byte region）。
             * 骨架是二进制 + 显式长度，不需要补。 */
            dat.push_back('\0');
            mq_spine_data* dd = mq_spine_data_create(dsk.data(), dsk.size(), 1, dat.data(), "",
                                                     fake_tex_cb, nullptr);
            CHECK(dd != nullptr);
            if (dd) {
                mq_spine_instance* di = mq_spine_instance_create(dd);
                CHECK(di != nullptr);
                if (di) {
                    std::vector<float> v(16384), u(16384), c(32768), dk(32768);
                    std::vector<uint32_t> ix(65536);
                    std::vector<mq_spine_batch> ba(512);
                    mq_spine_draw ddr{};
                    const int r2 = mq_spine_instance_draw(di, v.data(), u.data(), c.data(), dk.data(),
                                                          ix.data(), ba.data(), 8192, 32768, 512, &ddr);
                    CHECK(r2 == MQ_SPINE_OK);
                    int non_default = 0;
                    for (int i = 0; i < ddr.vertex_count; ++i) {
                        if (dk[(size_t)i * 4] != 0.0F || dk[(size_t)i * 4 + 1] != 0.0F ||
                            dk[(size_t)i * 4 + 2] != 0.0F || dk[(size_t)i * 4 + 3] != 1.0F) ++non_default;
                    }
                    std::print("  dragon-ess（无 TintBlack）: {} 顶点，dark 非默认黑的 {} 个\n",
                               ddr.vertex_count, non_default);
                    CHECK(ddr.vertex_count > 0);
                    CHECK(non_default == 0);
                    mq_spine_instance_destroy(di);
                }
                mq_spine_data_destroy(dd);
            }
        } else {
            std::print("  dragon-ess 素材缺失 → 跳过反向证据（不误报失败）\n");
        }
    }

    if (g_failures == 0) { std::print("twocolor_spine_test: all checks passed\n"); return 0; }
    std::print(stderr, "twocolor_spine_test: {} failure(s)\n", g_failures);
    return 1;
}
