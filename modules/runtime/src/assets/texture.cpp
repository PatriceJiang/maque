/* 资源 · 纹理/网格/材质（含 sRGB mip、sampler 路径、路径缓存；docs/13、plans/20）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S4，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- 贴图路径 sampler 后缀（plans/20：glTF sampler 的 wrap/filter 落到 VFS 路径上）----
 * 路径形如 `textures/x.png#wrap=clamp,mirror&filter=nearest`：
 *   · '#' 之前是真正的 VFS 路径（读文件/解码用它）；
 *   · wrap=repeat|clamp|mirror → create_texture2 的 wrap_u/wrap_v（0/1/2）；可写一个（u=v）或 "u,v"；
 *   · filter=nearest|linear → create_texture2 的 filter（0/1）；
 *   · 无 '#'：文件贴图默认 repeat+linear（glTF sampler 缺省）；builtin 贴图由调用方默认 clamp。
 * 缓存 key 用**完整字符串**：同一张图配不同 sampler 是两个独立的 GPU 纹理对象。 */
static int wrap_mode_code(const std::string& s) {
    if (s == "repeat") return 0;
    if (s == "clamp") return 1;
    if (s == "mirror") return 2;
    return -1;                       /* 未知值 → 忽略该项（用默认） */
}

TexPathParams parse_tex_path(const std::string& ref) {
    TexPathParams p;
    p.vfs = ref;
    const size_t hash = ref.find('#');
    if (hash == std::string::npos) return p;
    p.vfs.assign(ref, 0, hash);
    const std::string query = ref.substr(hash + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string key = kv.substr(0, eq);
            const std::string val = kv.substr(eq + 1);
            if (key == "wrap") {
                const size_t comma = val.find(',');
                const int u = wrap_mode_code(comma == std::string::npos ? val : val.substr(0, comma));
                const int v = wrap_mode_code(comma == std::string::npos ? val : val.substr(comma + 1));
                if (u >= 0) { p.wrap_u = u; p.wrap_v = (v >= 0 ? v : u); p.wrap_set = true; }
            } else if (key == "filter") {
                if (val == "nearest") { p.filter = 0; p.filter_set = true; }
                else if (val == "linear") { p.filter = 1; p.filter_set = true; }
            }
        }
        pos = amp + 1;
    }
    return p;
}

std::string Engine::scoped_texture_ref(int entity, const std::string& ref) const {
    static constexpr const char* kRt = "rendertexture://";
    if (ref.rfind(kRt, 0) != 0) return ref;
    const std::string tail = ref.substr(std::strlen(kRt));
    if (tail.find('/') != std::string::npos) return ref;       /* 已是规范 URI */
    auto it = entity_instance.find(entity);
    const int owner = it == entity_instance.end() ? 0 : it->second;
    return std::string(kRt) + std::to_string(owner) + "/" + tail;
}

int Engine::load_texture(const std::string& path) {
    auto it = tex_cache.find(path);
    if (it != tex_cache.end()) return it->second;
    /* 旧 create_texture 没有 sampler 参数：这里只把 '#' 后缀剥掉再读文件（sampler 由 2D 全局采样器决定）；
     * PBR 贴图走 load_texture_srgb/mips → create_texture2 才真正生效。 */
    const TexPathParams tp = parse_tex_path(path);
    void* data = nullptr; size_t n = 0;
    if (mq_vfs_read(vfs, tp.vfs.c_str(), &data, &n) != 0) { std::print("runtime: texture not found '{}'\n", path); return -1; }
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(data, n, &rgba, &w, &h, err, sizeof err);
    mq_vfs_free(data);
    if (rc != 0) { std::print("runtime: decode failed '{}': {}\n", path, err); return -1; }
    int id = rapi->create_texture(rself, rgba, (int)w, (int)h);
    mq_image_free(rgba);
    tex_cache[path] = id;
    return id;
}

/* plans/19：CPU box-filter mip 链（RGBA8，各层连续存放）。
 * 无 mip 时高频贴图（布纹/车漆细节）在远处会强烈闪烁/摩尔纹——PBR 正确性的重要一环。
 * srgb=true（M4g albedo/emissive）：RGB 必须**先解码到线性再平均、再编码回 sRGB**——
 * 直接在 sRGB 编码字节上平均会让明暗混合显著偏暗（黑白棋盘 2×2 的线性均值 0.5 →
 * 编码域平均只剩 ~0.214，线性亮度 -57%）。alpha 始终是线性数据，直接平均。
 * 线性数据贴图（MR/AO/normal）走 srgb=false，与旧实现逐字节一致。 */
static void srgb_mip_tables(float* decode, float* encode_thr) {
    for (int i = 0; i < 256; ++i) {
        const double c = (double)i / 255.0;
        decode[i] = (float)(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
    }
    /* encode_thr[b] = 线性域中「编码值恰为 (b+0.5)/255」的门槛：值 >= 门槛即进位到 b+1，
     * 二分即得 round(255*srgb(linear))，避免逐像素 pow。 */
    for (int b = 0; b < 255; ++b) {
        const double s = ((double)b + 0.5) / 255.0;
        encode_thr[b] = (float)(s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4));
    }
}

static uint8_t srgb_encode_from_linear(float linear, const float* encode_thr) {
    if (linear <= 0.0F) return 0;
    if (linear >= 1.0F) return 255;
    int lo = 0, hi = 255;                       /* 找第一个 > linear 的门槛 → 输出 lo */
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (linear < encode_thr[mid]) hi = mid; else lo = mid + 1;
    }
    return (uint8_t)lo;
}

/* 启动期资源加载并行度（MAQUE_TEX_THREADS；0/未设 = auto，1 = 关闭）。
 * 仅用于装配期 decode/mip；模拟帧仍单线程。WASM P0 无 pthread，强制 1。 */
int asset_worker_count() {
#if defined(__EMSCRIPTEN__) || defined(MQ_PLATFORM_WASM)
    return 1;
#else
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    int n = (int)std::min(hw, 8u);
#if defined(MQ_PLATFORM_ANDROID)
    n = std::min(n, 4);                       /* 移动端内存更紧：默认最多 4 个 worker */
#endif
    if (const char* v = std::getenv("MAQUE_TEX_THREADS"); v && *v) {
        const int parsed = std::atoi(v);
        if (parsed > 0) n = std::min(parsed, 32);
        else if (parsed < 0) n = 1;          /* <0：明确串行；0 = auto */
    }
    return std::max(1, n);
#endif
}

#if defined(__EMSCRIPTEN__) || defined(MQ_PLATFORM_WASM)
template <class Fn>
static void parallel_rows(int rows, int, Fn&& fn) { fn(0, rows); }
#else
template <class Fn>
static void parallel_rows(int rows, int threads, Fn&& fn) {
    threads = std::clamp(threads, 1, std::max(1, rows));
    if (threads <= 1 || rows < 128) { fn(0, rows); return; }
    std::vector<std::thread> workers;
    workers.reserve((size_t)threads);
    const int chunk = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int b = t * chunk;
        const int e = std::min(rows, b + chunk);
        if (b >= e) break;
        workers.emplace_back([&fn, b, e] { fn(b, e); });
    }
    for (auto& w : workers) w.join();
}
#endif

/* CPU box-filter mip 链（RGBA8，各层连续存放）。输出与旧实现**逐字节一致**，
 * 但：① 一次算好总大小、只分配一次；② 大层按行分给 1..N 个线程（行间无依赖）；
 * ③ 内层循环拆开 4 邻居读取，避免每像素的 min/分支。
 * threads<=0 = auto（asset_worker_count）；预载 worker 内调用时传 1，避免嵌套线程。 */
static std::vector<uint8_t> build_mip_chain(const uint8_t* src, int w0, int h0, int& levels,
                                            bool srgb, int threads = 0) {
    levels = 0;
    std::vector<uint8_t> out;
    if (!src || w0 <= 0 || h0 <= 0) return out;
    size_t total = (size_t)w0 * (size_t)h0 * 4u;
    int tw = w0, th = h0;
    int count = 1;
    while (tw > 1 || th > 1) {
        tw = std::max(1, tw / 2);
        th = std::max(1, th / 2);
        total += (size_t)tw * (size_t)th * 4u;
        ++count;
    }
    out.resize(total);
    std::memcpy(out.data(), src, (size_t)w0 * (size_t)h0 * 4u);
    levels = count;

    float decode[256];
    float encode_thr[255];
    if (srgb) srgb_mip_tables(decode, encode_thr);
    const int nt = threads > 0 ? threads : asset_worker_count();
    int w = w0, h = h0;
    size_t src_off = 0;
    uint8_t* base = out.data();
    while (w > 1 || h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const size_t dst_off = src_off + (size_t)w * (size_t)h * 4u;
        uint8_t* dst = base + dst_off;
        const uint8_t* src_level = base + src_off;
        const int use_threads = (nt > 1 && (int64_t)nw * nh >= 256 * 256) ? nt : 1;
        parallel_rows(nh, use_threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int sy0 = std::min(h - 1, y * 2);
                const int sy1 = std::min(h - 1, y * 2 + 1);
                const uint8_t* r0 = src_level + (size_t)sy0 * (size_t)w * 4u;
                const uint8_t* r1 = src_level + (size_t)sy1 * (size_t)w * 4u;
                uint8_t* d = dst + (size_t)y * (size_t)nw * 4u;
                for (int x = 0; x < nw; ++x, d += 4) {
                    const int sx0 = std::min(w - 1, x * 2);
                    const int sx1 = std::min(w - 1, x * 2 + 1);
                    const uint8_t* p00 = r0 + (size_t)sx0 * 4u;
                    const uint8_t* p01 = r0 + (size_t)sx1 * 4u;
                    const uint8_t* p10 = r1 + (size_t)sx0 * 4u;
                    const uint8_t* p11 = r1 + (size_t)sx1 * 4u;
                    if (srgb) {
                        const float r = decode[p00[0]] + decode[p01[0]] + decode[p10[0]] + decode[p11[0]];
                        const float g = decode[p00[1]] + decode[p01[1]] + decode[p10[1]] + decode[p11[1]];
                        const float b = decode[p00[2]] + decode[p01[2]] + decode[p10[2]] + decode[p11[2]];
                        const int a = p00[3] + p01[3] + p10[3] + p11[3];
                        d[0] = srgb_encode_from_linear(r * 0.25F, encode_thr);
                        d[1] = srgb_encode_from_linear(g * 0.25F, encode_thr);
                        d[2] = srgb_encode_from_linear(b * 0.25F, encode_thr);
                        d[3] = (uint8_t)((a + 2) / 4);
                    } else {
                        d[0] = (uint8_t)((p00[0] + p01[0] + p10[0] + p11[0] + 2) / 4);
                        d[1] = (uint8_t)((p00[1] + p01[1] + p10[1] + p11[1] + 2) / 4);
                        d[2] = (uint8_t)((p00[2] + p01[2] + p10[2] + p11[2] + 2) / 4);
                        d[3] = (uint8_t)((p00[3] + p01[3] + p10[3] + p11[3] + 2) / 4);
                    }
                }
            }
        });
        src_off = dst_off;
        w = nw;
        h = nh;
    }
    return out;
}

/* ---- CPU 预载单元：decode + mip 链（不碰 GPU）----
 * 并行只在**装配期**；GPU 上传仍由主线程做（wgpu 句柄/描述符不在 worker 里创建）。 */
struct PreparedTexture {
    std::vector<uint8_t> mips;   /* RGBA8 各层连续 */
    uint32_t w = 0, h = 0;
    int levels = 1;
};

static bool prepare_texture_rgba(const void* data, size_t n, bool srgb,
                                 PreparedTexture& out, std::string& err, int threads) {
    uint8_t* rgba = nullptr;
    uint32_t w = 0, h = 0;
    char ebuf[128] = { 0 };
    if (mq_image_decode(data, n, &rgba, &w, &h, ebuf, sizeof ebuf) != 0) {
        err = ebuf[0] ? ebuf : "decode failed";
        return false;
    }
    int levels = 1;
    std::vector<uint8_t> mips = build_mip_chain(rgba, (int)w, (int)h, levels, srgb, threads);
    mq_image_free(rgba);
    if (mips.empty() || w == 0 || h == 0) {
        err = "mip chain failed";
        return false;
    }
    out.mips = std::move(mips);
    out.w = w;
    out.h = h;
    out.levels = levels;
    return true;
}

static bool read_texture_file(Engine& e, const std::string& vfs_path,
                              std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (!e.vfs) { err = "vfs unavailable"; return false; }
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(e.vfs, vfs_path.c_str(), &data, &n) != 0 || !data) {
        err = "not found";
        return false;
    }
    out.assign((const uint8_t*)data, (const uint8_t*)data + n);
    mq_vfs_free(data);
    return true;
}

static int32_t upload_prepared_texture(Engine& e, const PreparedTexture& p, bool srgb,
                                       int filter, int wrap_u, int wrap_v) {
    if (!e.rapi || !e.rself || p.mips.empty()) return -1;
    int32_t id = -1;
    if (e.rapi->create_texture2) {
        mq_renderer_texture_desc td{};
        td.width = (int32_t)p.w;
        td.height = (int32_t)p.h;
        td.pixel_format = srgb ? 1 : 0;       /* 1 = RGBA8_SRGB，0 = RGBA8（线性数据） */
        td.kind = 0;
        td.mip_levels = p.levels;
        td.filter = filter;
        td.wrap_u = wrap_u;
        td.wrap_v = wrap_v;
        td.data = p.mips.data();
        td.data_size = (uint64_t)p.mips.size();
        if (e.rapi->create_texture2(e.rself, &td, &id) != 0) id = -1;
    }
    if (id < 0 && e.rapi->create_texture)
        id = e.rapi->create_texture(e.rself, p.mips.data(), (int32_t)p.w, (int32_t)p.h);
    return id;
}

int Engine::load_texture_srgb(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    /* builtin: 是程序化纹理（checker 等），沿用既有 UNorm 路径，不做 sRGB 解码 */
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "srgb:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    std::vector<uint8_t> bytes;
    std::string err;
    if (!read_texture_file(*this, tp.vfs, bytes, err)) {
        std::print("runtime: srgb texture not found '{}'\n", path);
        return -1;
    }
    PreparedTexture prep;
    if (!prepare_texture_rgba(bytes.data(), bytes.size(), /*srgb=*/true, prep, err, 0)) {
        std::print("runtime: srgb decode failed '{}': {}\n", path, err);
        return -1;
    }
    const int32_t id = upload_prepared_texture(*this, prep, /*srgb=*/true,
                                               tp.filter, tp.wrap_u, tp.wrap_v);
    if (id < 0) std::print("runtime: srgb texture upload failed '{}'\n", path);
    tex_cache[key] = id;
    return id;
}

/* plans/19：线性数据贴图（MR/AO/normal/clearcoat normal）+ CPU mip 链；采样 RGBA8Unorm。 */
int Engine::load_texture_mips(const std::string& path) {
    if (path.rfind("rendertexture://", 0) == 0) {
        auto rt = tex_cache.find(path);
        return rt == tex_cache.end() ? -1 : rt->second;
    }
    const TexPathParams tp = parse_tex_path(path);
    if (tp.vfs.rfind("builtin:", 0) == 0) return load_texture_ref(path);
    const std::string key = "mip:" + path;
    auto it = tex_cache.find(key);
    if (it != tex_cache.end()) return it->second;
    std::vector<uint8_t> bytes;
    std::string err;
    if (!read_texture_file(*this, tp.vfs, bytes, err)) {
        std::print("runtime: texture not found '{}'\n", path);
        return -1;
    }
    PreparedTexture prep;
    if (!prepare_texture_rgba(bytes.data(), bytes.size(), /*srgb=*/false, prep, err, 0)) {
        std::print("runtime: decode failed '{}': {}\n", path, err);
        return -1;
    }
    int32_t id = upload_prepared_texture(*this, prep, /*srgb=*/false,
                                         tp.filter, tp.wrap_u, tp.wrap_v);
    if (id < 0) id = load_texture(path);      /* create_texture2 不可用时退回无 mip 路径（内部会剥后缀） */
    tex_cache[key] = id;
    return id;
}


/* PBR/材质贴图预载：装配期把 decode + mip 链分批并行，GPU 上传回主线程。
 * 只处理文件贴图（builtin/rendertexture 跳过）；按缓存 key 去重、排序 → 上传顺序确定。 */
void Engine::preload_texture_refs(std::vector<std::pair<std::string, bool>> refs, const char* label) {
    if (refs.empty() || !rapi || !rself) return;
    if (const char* v = std::getenv("MAQUE_TEX_PRELOAD");
        v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0))
        return;
    struct Job {
        std::string ref, key, vfs;
        bool srgb = false;
        int filter = 1, wrap_u = 0, wrap_v = 0;
    };
    std::vector<Job> jobs;
    std::unordered_set<std::string> seen;
    jobs.reserve(refs.size());
    for (auto& [ref, srgb] : refs) {
        if (ref.empty() || ref.rfind("rendertexture://", 0) == 0) continue;
        const TexPathParams tp = parse_tex_path(ref);
        if (tp.vfs.rfind("builtin:", 0) == 0) continue;
        std::string key = (srgb ? "srgb:" : "mip:") + ref;
        if (tex_cache.find(key) != tex_cache.end()) continue;
        if (!seen.insert(key).second) continue;
        jobs.push_back(Job{ ref, std::move(key), tp.vfs, srgb, tp.filter, tp.wrap_u, tp.wrap_v });
    }
    if (jobs.empty()) return;
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.key < b.key; });

    const int threads = asset_worker_count();
    const auto t0 = std::chrono::steady_clock::now();
    size_t uploaded = 0, failed = 0;
    for (size_t i = 0; i < jobs.size();) {
        const size_t n = std::min((size_t)threads, jobs.size() - i);
        std::vector<std::vector<uint8_t>> bytes(n);
        std::vector<char> readable(n, 0);
        std::vector<PreparedTexture> prep(n);
        std::vector<std::string> errs(n);
        std::vector<char> ok(n, 0);
        for (size_t j = 0; j < n; ++j) {
            std::string err;
            if (read_texture_file(*this, jobs[i + j].vfs, bytes[j], err)) readable[j] = 1;
            else {
                std::print(stderr, "runtime: texture preload 读不到 '{}': {}\n", jobs[i + j].ref, err);
                ++failed;
            }
        }
#if defined(__EMSCRIPTEN__) || defined(MQ_PLATFORM_WASM)
        const bool use_workers = false;
#else
        const bool use_workers = threads > 1 && n > 1;
#endif
        if (use_workers) {
#if !defined(__EMSCRIPTEN__) && !defined(MQ_PLATFORM_WASM)
            std::vector<std::thread> workers;
            workers.reserve(n);
            for (size_t j = 0; j < n; ++j) {
                if (!readable[j]) continue;
                workers.emplace_back([&, j] {
                    ok[j] = prepare_texture_rgba(bytes[j].data(), bytes[j].size(),
                                                 jobs[i + j].srgb, prep[j], errs[j], 1);
                });
            }
            for (auto& w : workers) w.join();
#endif
        } else {
            for (size_t j = 0; j < n; ++j) {
                if (!readable[j]) continue;
                ok[j] = prepare_texture_rgba(bytes[j].data(), bytes[j].size(),
                                             jobs[i + j].srgb, prep[j], errs[j], 1);
            }
        }
        for (size_t j = 0; j < n; ++j) {
            if (!readable[j]) continue;               /* 读失败不缓存，让 lazy 路径重试 */
            if (!ok[j]) {
                std::print(stderr, "runtime: texture preload 解码失败 '{}': {}\n",
                           jobs[i + j].ref, errs[j]);
                ++failed;
                continue;
            }
            const int32_t id = upload_prepared_texture(*this, prep[j], jobs[i + j].srgb,
                                                       jobs[i + j].filter, jobs[i + j].wrap_u,
                                                       jobs[i + j].wrap_v);
            tex_cache[jobs[i + j].key] = id;
            if (id >= 0) ++uploaded;
            else {
                ++failed;
                std::print(stderr, "runtime: texture preload 上传失败 '{}'\n", jobs[i + j].ref);
            }
            prep[j].mips.clear();                      /* 上传后立即释放 CPU 副本 */
        }
        i += n;
    }
    if (std::getenv("MAQUE_TEX_INFO")) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::print("runtime: texture preload '{}': jobs={} uploaded={} failed={} threads={} ({:.1f} ms)\n",
                   label ? label : "", jobs.size(), uploaded, failed, threads, ms);
    }
}

int Engine::load_texture_ref(const std::string& ref) {
    const TexPathParams tp = parse_tex_path(ref);
    if (tp.vfs.rfind("builtin:", 0) == 0) {
        auto it = tex_cache.find(ref);
        if (it != tex_cache.end()) return it->second;
        int w = 1, h = 1;
        std::vector<uint8_t> px;
        int builtin_mips = 1;
        int builtin_format = 0;             /* 0=RGBA8Unorm 1=RGBA8UnormSrgb */
        int builtin_wrap_u = -1, builtin_wrap_v = -1;
        if (tp.vfs == "builtin:/textures/checker") {
            w = h = 16; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                bool alt = (((x / 4) + (y / 4)) & 1) != 0;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = alt ? 255 : 230; c[1] = alt ? 255 : 140; c[2] = alt ? 255 : 60; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/toon_ramp") {
            /* Toon 渐变：256×1 四档硬阶（与 toon_steps=4 对应；线性 clamp 采样） */
            w = 256; h = 1; px.resize((size_t)w * 4);
            static const uint8_t steps[4] = { 64, 132, 200, 255 };
            for (int x = 0; x < w; ++x) {
                const uint8_t v = steps[std::min(3, x * 4 / w)];
                uint8_t* c = &px[(size_t)x * 4]; c[0] = v; c[1] = v; c[2] = v; c[3] = 255;
            }
        } else if (tp.vfs == "builtin:/textures/matcap_studio") {
            /* Matcap：256×256 冷色相影棚球（左上高光、右下暗），线性采样/clamp；用作 view-space 法线查表 */
            w = h = 256; px.resize((size_t)w * h * 4);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const float nx = (float(x) + 0.5F) / w * 2.0F - 1.0F;
                const float ny = (float(y) + 0.5F) / h * 2.0F - 1.0F;
                const float r2 = nx * nx + ny * ny;
                const float nz = std::sqrt(std::max(0.0F, 1.0F - r2));
                /* 视空间光：左上前方（与 view 法线约定一致；纹理 v 向下 → ny 取反） */
                const float lx = -0.42F, ly = -0.50F, lz = 0.76F;
                const float ndl = std::max(0.0F, nx * lx + ny * ly + nz * lz);
                const float spec = std::pow(std::max(0.0F, nx * (-0.20F) + ny * (-0.26F) + nz * 0.94F), 28.0F);
                const float diff = 0.12F + 0.88F * ndl;
                const float edge = std::clamp(1.0F - r2, 0.0F, 1.0F);   /* 球外渐暗 */
                const float r = (0.20F * diff + spec * 0.95F) * edge;
                const float g = (0.34F * diff + spec * 0.98F) * edge;
                const float b = (0.62F * diff + spec * 0.95F) * edge;
                const float a = 1.0F;
                uint8_t* c = &px[((size_t)y * w + x) * 4];
                c[0] = (uint8_t)std::clamp((int)(r * 255.0F + 0.5F), 0, 255);
                c[1] = (uint8_t)std::clamp((int)(g * 255.0F + 0.5F), 0, 255);
                c[2] = (uint8_t)std::clamp((int)(b * 255.0F + 0.5F), 0, 255);
                c[3] = (uint8_t)(a * 255.0F);
            }
        } else { px = { 255, 255, 255, 255 }; }
        int id = -1;
        if (rapi && rself && rapi->create_texture2) {
            /* builtin 查表纹理默认 clamp（matcap 球/渐变在边界外不能 wrap）；路径后缀可覆盖。 */
            const int wrap_u = tp.wrap_set ? tp.wrap_u : (builtin_wrap_u >= 0 ? builtin_wrap_u : 1);
            const int wrap_v = tp.wrap_set ? tp.wrap_v : (builtin_wrap_v >= 0 ? builtin_wrap_v : 1);
            const int filter = tp.filter_set ? tp.filter : 1;
            mq_renderer_texture_desc td{};
            td.width = w; td.height = h; td.pixel_format = builtin_format; td.kind = 0;
            td.mip_levels = builtin_mips;
            td.filter = filter; td.wrap_u = wrap_u; td.wrap_v = wrap_v;
            td.data = px.data(); td.data_size = px.size();
            if (rapi->create_texture2(rself, &td, &id) != 0) id = -1;
        }
        if (id < 0) id = rapi->create_texture(rself, px.data(), w, h);
        tex_cache[ref] = id;
        return id;
    }
    return load_texture(ref);   // assets（load_texture 内部剥 sampler 后缀）
}

/* M4c：加载离线导入的 .mesh.json（tools/gltf_import.py 产出）→ 渲染器网格句柄。
 * 只读资产文件；按路径缓存（失败也缓存 -1，避免每帧重试/刷日志）。
 * plans/22 §6.1：vertex_layout 2/3 = 48/56B 基础 + joints u16x4 + weights f32x4（stride 72/80）；
 * morph 网格额外保存**打包后的基础顶点** + targets/default_weights，创建后立即应用默认权重。 */
/* ---- .mesh.json 的 CPU 解析与 GPU 上传分离（装配期并行；docs/bugs/015）---- */
struct PreparedMeshData {
    std::vector<float> verts;
    std::vector<uint32_t> idx;
    std::vector<uint8_t> packed;
    float bounds_min[3] = { 0.0F, 0.0F, 0.0F };
    float bounds_max[3] = { 0.0F, 0.0F, 0.0F };
    int32_t vertex_layout = 0;
    uint32_t row_floats = 0, base_stride = 0, stride = 0;
    uint32_t vn = 0, in = 0;
    bool skinned = false;
    MorphMesh morph;
    bool ok = false;
    std::string error;
    std::string warning;
};

static void parse_prepared_mesh(mq_json* dom, const std::string& path, PreparedMeshData& pm) {
    pm = PreparedMeshData{};
    std::vector<float>& verts = pm.verts;
    std::vector<uint32_t>& idx = pm.idx;
    std::vector<uint8_t>& packed = pm.packed;
    /* 顶点布局（plans/22 §5.1）：
     *   0 = 旧 48B：pos3|normal3|tangent4|uv0 2（12 float/顶点）
     *   1 = 新 56B：pos3|normal3|color4|uv0 2|uv1 2（14 float/顶点）
     *   2 = 48B + joints u16x4 + weights f32x4（stride 72；JSON 行宽仍 12）
     *   3 = 56B + joints u16x4 + weights f32x4（stride 80；JSON 行宽仍 14）
     * 蒙皮数据在独立 joints/weights 数组 → 运行时按 GPU stride 打包。 */
    int32_t& vertex_layout = pm.vertex_layout;
    if (const mq_json* jlay = mq_json_get(dom, "vertex_layout"))
        vertex_layout = (int32_t)mq_json_num(jlay, 0.0);
    const bool base56 = vertex_layout == 1 || vertex_layout == 3;
    const uint32_t row_floats = base56 ? 14u : 12u;
    const uint32_t base_stride = base56 ? 56u : 48u;
    const bool skinned = vertex_layout >= 2 && vertex_layout <= 3;
    const uint32_t stride = base_stride + (skinned ? 24u : 0u);
    pm.row_floats = row_floats;
    pm.base_stride = base_stride;
    pm.stride = stride;
    pm.skinned = skinned;
    const mq_json* jv = mq_json_get(dom, "vertices");
    const uint32_t vn = mq_json_len(jv);
    verts.reserve((size_t)vn * row_floats);
    const mq_json* ji = mq_json_get(dom, "indices");
    const uint32_t in = mq_json_len(ji);
    idx.reserve(in);
    bool ok = vn > 0 && in > 0 && vertex_layout >= 0 && vertex_layout <= 3;
    pm.vn = vn;
    pm.in = in;
    if (!ok) {
        char b[192];
        std::snprintf(b, sizeof b, "无效（vertices=%u indices=%u vertex_layout=%d）",
                      vn, in, vertex_layout);
        pm.error = b;
    }
    for (uint32_t i = 0; ok && i < vn; ++i) {
        const mq_json* row = mq_json_at(jv, i);
        if (mq_json_len(row) != row_floats) {
            char b[128];
            std::snprintf(b, sizeof b, "顶点行 %u 宽度 %u（期望 %u）", i, mq_json_len(row), row_floats);
            pm.error = b;
            ok = false;
            break;
        }
        for (uint32_t k = 0; k < row_floats; ++k)
            verts.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
    }
    for (uint32_t i = 0; ok && i < in; ++i)
        idx.push_back((uint32_t)mq_json_num(mq_json_at(ji, i), 0.0));
    if (ok && skinned) {
        const mq_json* jj = mq_json_get(dom, "joints");
        const mq_json* jw = mq_json_get(dom, "weights");
        ok = mq_json_len(jj) == vn && mq_json_len(jw) == vn;
        if (!ok) {
            char b[192];
            std::snprintf(b, sizeof b, "蒙皮数据缺失/长度不符（joints=%u weights=%u vertices=%u）",
                          mq_json_len(jj), mq_json_len(jw), vn);
            pm.error = b;
        }
        if (ok) {
            packed.resize((size_t)vn * stride);
            for (uint32_t i = 0; i < vn; ++i) {
                uint8_t* dst = packed.data() + (size_t)i * stride;
                std::memcpy(dst, verts.data() + (size_t)i * row_floats, base_stride);
                const mq_json* jr = mq_json_at(jj, i);
                const mq_json* wr = mq_json_at(jw, i);
                if (mq_json_len(jr) != 4 || mq_json_len(wr) != 4) {
                    pm.error = "蒙皮属性分量数不是 4";
                    ok = false;
                    break;
                }
                uint16_t* jdst = (uint16_t*)(dst + base_stride);
                for (uint32_t k = 0; k < 4; ++k) {
                    int v = (int)mq_json_num(mq_json_at(jr, k), 0.0);
                    if (v < 0) v = 0;
                    if (v > 65535) v = 65535;
                    jdst[k] = (uint16_t)v;
                }
                float* wdst = (float*)(dst + base_stride + 8);
                for (uint32_t k = 0; k < 4; ++k)
                    wdst[k] = (float)mq_json_num(mq_json_at(wr, k), 0.0);
            }
        }
    }
    /* morph（可选）：default_weights + targets（逐顶点 pos/normal 增量；targets ≤ 8） */
    MorphMesh& morph = pm.morph;
    if (ok) {
        if (const mq_json* jm = mq_json_get(dom, "morph")) {
            const mq_json* jdw = mq_json_get(jm, "default_weights");
            const uint32_t ndw = mq_json_len(jdw);
            morph.default_weights.reserve(ndw);
            for (uint32_t k = 0; k < ndw; ++k)
                morph.default_weights.push_back((float)mq_json_num(mq_json_at(jdw, k), 0.0));
            const mq_json* jts = mq_json_get(jm, "targets");
            const uint32_t nts = mq_json_len(jts);
            if (nts > 8) {
                char b[128];
                std::snprintf(b, sizeof b, "morph targets=%u 超出上限 8，后续忽略", nts);
                pm.warning = b;
            }
            for (uint32_t t = 0; t < nts && t < 8 && ok; ++t) {
                const mq_json* jt = mq_json_at(jts, t);
                MorphTarget tgt;
                const mq_json* jp = mq_json_get(jt, "positions");
                const mq_json* jn = mq_json_get(jt, "normals");
                if (mq_json_len(jp) != vn) { pm.error = "morph positions 数量不符"; ok = false; break; }
                tgt.positions.reserve((size_t)vn * 3);
                for (uint32_t i = 0; i < vn && ok; ++i) {
                    const mq_json* row = mq_json_at(jp, i);
                    if (mq_json_len(row) != 3) { pm.error = "morph positions 行宽不是 3"; ok = false; break; }
                    for (uint32_t k = 0; k < 3; ++k)
                        tgt.positions.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                }
                if (ok && mq_json_len(jn) == vn) {
                    tgt.normals.reserve((size_t)vn * 3);
                    for (uint32_t i = 0; i < vn && ok; ++i) {
                        const mq_json* row = mq_json_at(jn, i);
                        if (mq_json_len(row) != 3) { pm.error = "morph normals 行宽不是 3"; ok = false; break; }
                        for (uint32_t k = 0; k < 3; ++k)
                            tgt.normals.push_back((float)mq_json_num(mq_json_at(row, k), 0.0));
                    }
                }
                if (ok) morph.targets.push_back(std::move(tgt));
            }
            if (morph.default_weights.size() < morph.targets.size())
                morph.default_weights.resize(morph.targets.size(), 0.0F);
        }
    }
    const mq_json* bmin = mq_json_get(dom, "bounds_min");
    const mq_json* bmax = mq_json_get(dom, "bounds_max");
    for (uint32_t k = 0; k < 3 && k < mq_json_len(bmin); ++k)
        pm.bounds_min[k] = (float)mq_json_num(mq_json_at(bmin, k), 0.0);
    for (uint32_t k = 0; k < 3 && k < mq_json_len(bmax); ++k)
        pm.bounds_max[k] = (float)mq_json_num(mq_json_at(bmax, k), 0.0);
    pm.ok = ok;
}

static int32_t upload_prepared_mesh(Engine& e, const std::string& path, PreparedMeshData& pm,
                                    bool render3d_info, double elapsed_ms) {
    if (!pm.ok) return -1;
    int32_t id = -1;
    const void* vptr = pm.packed.empty() ? (const void*)pm.verts.data() : (const void*)pm.packed.data();
    const uint64_t vbytes = pm.packed.empty()
                                ? (uint64_t)pm.verts.size() * sizeof(float)
                                : (uint64_t)pm.packed.size();
    if (e.rapi && e.rself && e.rapi->create_mesh) {
        mq_renderer_mesh_gpu md{};
        md.vertices = vptr;
        md.vertex_bytes = vbytes;
        md.vertex_stride = pm.stride;
        md.vertex_layout = pm.vertex_layout;
        md.indices = pm.idx.data();
        md.index_count = (uint32_t)pm.idx.size();
        for (int k = 0; k < 3; ++k) {
            md.bounds_min[k] = pm.bounds_min[k];
            md.bounds_max[k] = pm.bounds_max[k];
        }
        if (e.rapi->create_mesh(e.rself, &md, &id) != 0) id = -1;
        if (id >= 0) e.mesh_stats[id] = { pm.vn, (uint32_t)pm.idx.size() };
    }
    /* morph：保存打包后的基础顶点（CPU 重算起点），创建后立即应用默认权重 */
    if (id >= 0 && !pm.morph.targets.empty()) {
        pm.morph.mesh = id;
        pm.morph.vertex_count = pm.vn;
        pm.morph.stride = pm.stride;
        pm.morph.layout = pm.vertex_layout;
        pm.morph.base = std::move(pm.packed);
        if (pm.morph.base.empty()) {
            pm.morph.base.resize((size_t)pm.vn * pm.stride);
            std::memcpy(pm.morph.base.data(), pm.verts.data(), (size_t)pm.vn * pm.base_stride);
        }
        pm.morph.applied_weights = pm.morph.default_weights;
        e.morph_meshes[path] = std::move(pm.morph);
        bool any_w = false;
        for (float w : e.morph_meshes[path].default_weights) if (w != 0.0F) { any_w = true; break; }
        if (any_w) (void)e.apply_morph(path, e.morph_meshes[path].default_weights);
        if (render3d_info)
            std::print("runtime: morph mesh '{}' targets={} vertices={} stride={}\n",
                       path, (int)e.morph_meshes[path].targets.size(),
                       (long long)e.morph_meshes[path].vertex_count,
                       (long long)e.morph_meshes[path].stride);
    }
    e.mesh_layout[path] = (id >= 0) ? pm.vertex_layout : -1;
    e.mesh_cache[path] = id;
    if (id >= 0 && render3d_info)
        std::print("runtime: mesh '{}' vertices={} indices={} ({:.1f} ms)\n",
                   path, pm.vn, pm.in, elapsed_ms);
    return id;
}

int32_t Engine::load_mesh(const std::string& path) {
    auto it = mesh_cache.find(path);
    if (it != mesh_cache.end()) return it->second;
    if (!gltf_env_read) read_gltf_env();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> bytes;
    if (!read_vfs_file(path, bytes)) {
        std::print(stderr, "runtime: mesh not found '{}'\n", path);
        mesh_cache[path] = -1;
        return -1;
    }
    mq_json* dom = mq_json_parse((const char*)bytes.data(), bytes.size());
    if (!dom) {
        std::print(stderr, "runtime: mesh json 解析失败 '{}'\n", path);
        mesh_cache[path] = -1;
        return -1;
    }
    PreparedMeshData pm;
    parse_prepared_mesh(dom, path, pm);
    mq_json_free(dom);
    if (!pm.warning.empty()) std::print(stderr, "runtime: mesh '{}' {}\n", path, pm.warning);
    if (!pm.ok) {
        std::print(stderr, "runtime: mesh '{}' {}\n", path, pm.error.empty() ? "解析失败" : pm.error);
        mesh_cache[path] = -1;
        return -1;
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    const int32_t id = upload_prepared_mesh(*this, path, pm, render3d_info, ms);
    if (id < 0) std::print(stderr, "runtime: mesh 载入失败 '{}'\n", path);
    return id;
}

/* ---- 运行时 glTF/GLB 直接加载（plans/23/24）----
 * mesh_path 指向 .gltf/.glb 时不必先跑 Python importer：装配期解析 + 上传一次，之后每帧只读缓存。
 * 资源（buffer/image）的相对 URI 按**模型文件所在目录**经 VFS 解析（远程挂载同样适用）。 */
bool Engine::read_vfs_file(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    if (!vfs) return false;
    void* data = nullptr;
    size_t n = 0;
    if (mq_vfs_read(vfs, path.c_str(), &data, &n) != 0 || !data) return false;
    out.assign((const uint8_t*)data, (const uint8_t*)data + n);
    mq_vfs_free(data);
    return true;
}

/* .mesh.json 预载：CPU JSON/顶点解析并行，GPU create_mesh 主线程。
 * 只处理非 glTF 的 mesh_path（glTF 的 mesh 在 load_gltf_model 内解析）。 */
void Engine::preload_mesh_refs(std::vector<std::string> paths) {
    if (paths.empty() || !rapi || !rself) return;
    if (!gltf_env_read) read_gltf_env();   /* render3d_info 决定 parse/upload 诊断日志 */
    if (const char* v = std::getenv("MAQUE_MESH_PRELOAD");
        v && (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0))
        return;
    std::unordered_set<std::string> seen;
    std::vector<std::string> jobs;
    jobs.reserve(paths.size());
    for (auto& p : paths) {
        if (p.empty() || mesh_cache.find(p) != mesh_cache.end()) continue;
        if (seen.insert(p).second) jobs.push_back(std::move(p));
    }
    if (jobs.empty()) return;
    std::sort(jobs.begin(), jobs.end());

    const int threads = asset_worker_count();
    const auto t0 = std::chrono::steady_clock::now();
    size_t uploaded = 0, failed = 0;
    for (size_t i = 0; i < jobs.size();) {
        const size_t n = std::min((size_t)threads, jobs.size() - i);
        std::vector<std::vector<uint8_t>> bytes(n);
        std::vector<char> readable(n, 0);
        std::vector<PreparedMeshData> prep(n);
        std::vector<std::string> errs(n);
        std::vector<double> parse_ms(n, 0.0);
        std::vector<char> ok(n, 0);
        for (size_t j = 0; j < n; ++j) {
            if (read_vfs_file(jobs[i + j], bytes[j])) readable[j] = 1;
            else {
                std::print(stderr, "runtime: mesh preload 读不到 '{}'\n", jobs[i + j]);
                mesh_cache[jobs[i + j]] = -1;
                ++failed;
            }
        }
#if defined(__EMSCRIPTEN__) || defined(MQ_PLATFORM_WASM)
        const bool use_workers = false;
#else
        const bool use_workers = threads > 1 && n > 1;
#endif
        auto parse_one = [&](size_t j) {
            const auto pt0 = std::chrono::steady_clock::now();
            mq_json* dom = mq_json_parse((const char*)bytes[j].data(), bytes[j].size());
            if (!dom) { errs[j] = "json parse failed"; return; }
            parse_prepared_mesh(dom, jobs[i + j], prep[j]);
            mq_json_free(dom);
            parse_ms[j] = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - pt0).count();
            ok[j] = prep[j].ok ? 1 : 0;
        };
        if (use_workers) {
#if !defined(__EMSCRIPTEN__) && !defined(MQ_PLATFORM_WASM)
            std::vector<std::thread> workers;
            workers.reserve(n);
            for (size_t j = 0; j < n; ++j) {
                if (!readable[j]) continue;
                workers.emplace_back([&, j] { parse_one(j); });
            }
            for (auto& w : workers) w.join();
#endif
        } else {
            for (size_t j = 0; j < n; ++j) {
                if (!readable[j]) continue;
                parse_one(j);
            }
        }
        for (size_t j = 0; j < n; ++j) {
            if (!readable[j]) continue;
            if (!ok[j]) {
                std::print(stderr, "runtime: mesh preload 解析失败 '{}': {}\n",
                           jobs[i + j], errs[j].empty() ? prep[j].error : errs[j]);
                mesh_cache[jobs[i + j]] = -1;
                ++failed;
                prep[j] = PreparedMeshData{};
                continue;
            }
            if (!prep[j].warning.empty())
                std::print(stderr, "runtime: mesh '{}' {}\n", jobs[i + j], prep[j].warning);
            const int32_t id = upload_prepared_mesh(*this, jobs[i + j], prep[j], render3d_info,
                                                    parse_ms[j]);
            if (id >= 0) ++uploaded;
            else {
                ++failed;
                std::print(stderr, "runtime: mesh preload 上传失败 '{}'\n", jobs[i + j]);
            }
            prep[j] = PreparedMeshData{};
        }
        i += n;
    }
    if (std::getenv("MAQUE_MESH_INFO")) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        std::print("runtime: mesh preload: jobs={} uploaded={} failed={} threads={} ({:.1f} ms)\n",
                   jobs.size(), uploaded, failed, threads, ms);
    }
}

int32_t Engine::upload_texture_rgba(const uint8_t* rgba, int w, int h, bool srgb,
                                    int filter, int wrap_u, int wrap_v) {
    if (!rapi || !rself || !rgba || w <= 0 || h <= 0) return -1;
    PreparedTexture prep;
    prep.mips = build_mip_chain(rgba, w, h, prep.levels, srgb);
    if (prep.mips.empty()) return -1;
    prep.w = (uint32_t)w;
    prep.h = (uint32_t)h;
    return upload_prepared_texture(*this, prep, srgb, filter, wrap_u, wrap_v);
}

/* 材质资源：builtin:/ 或 assets（JSON，反射反序列化 scn.SpriteMaterial）。 */
bool Engine::load_material(const std::string& ref, scn::SpriteMaterial& out) {
    if (ref.rfind("builtin:", 0) == 0) {
        out.base_color = math::Vec4{ 1, 1, 1, 1 };
        out.texture = (ref == "builtin:/materials/checker") ? "builtin:/textures/checker"
                                                            : "builtin:/textures/white";
        return true;
    }
    void* d = nullptr; size_t n = 0;
    std::string path = "/" + ref;
    if (mq_vfs_read(vfs, path.c_str(), &d, &n) != 0) { std::print("runtime: material not found '{}'\n", ref); return false; }
    mq_json* dom = mq_json_parse((const char*)d, n);
    mq_vfs_free(d);
    if (!dom) return false;
    const mq_type_desc* mt = mq_reflect_type_by_name(reflect, "scn.SpriteMaterial");
    int rc = mt ? mq_persist_apply_json(reflect, mt, dom, &out) : 1;
    mq_json_free(dom);
    return rc == 0;
}

}  // namespace maque::rt
