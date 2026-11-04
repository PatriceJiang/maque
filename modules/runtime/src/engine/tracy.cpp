/* 诊断 · Tracy 性能分析（docs/29）：启动/采样/内存域
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S7，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ---- Tracy 性能分析（docs/29）------------------------------------------------------
 * 设计：客户端**手动启动**（F3 / MAQUE_TRACY=1）；未启动时所有宏 no-op。
 * 基础数据：frame/logic/render/present/FPS/draw/实体（每步）+ CPU%/内存（1Hz）。
 * F3 还会拉起 tracy-profiler GUI（独立进程；退出游戏后 trace 仍可看）。
 */
static std::string find_tracy_profiler() {
    std::error_code ec;
#if defined(_WIN32)
    const char* exe_name = "tracy-profiler.exe";
#elif defined(__APPLE__)
    const char* exe_name = "Tracy.app/Contents/MacOS/Tracy";
#else
    const char* exe_name = "tracy-profiler";
#endif
    std::vector<fs::path> cands;
    if (const char* e = std::getenv("MAQUE_TRACY_PROFILER"); e && *e) cands.emplace_back(fs::path(e));
    /* 从 CWD 逐级向上找 `.deps/tracy/<exe>`（典型：仓库根 或 build-win/samples/player 启动）。 */
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 6 && !dir.empty() && !ec; ++i) {
        cands.push_back(dir / ".deps" / "tracy" / exe_name);
        cands.push_back(dir / "tools" / "tracy" / exe_name);
        dir = dir.parent_path();
    }
    for (const auto& c : cands)
        if (fs::exists(c, ec) && !ec) return c.string();
    return {};
}

void Engine::tracy_start(const char* why) {
    if (maque::tracy::started()) return;
    if (!maque::tracy::startup()) {
        std::print(stderr, "runtime: [tracy] 本构建未编入 Tracy（MAQUE_TRACY=OFF）\n");
        return;
    }
    /* 基础数据曲线：颜色与模块/分区色一致（frametime=System 蓝 / logic=Script 绿 /
     * render=3D 红 / present=System 紫 …；内存域曲线在 memory_publish 首次上报时配置）。 */
    MQ_TRACY_PLOT_CONFIG("frametime", 0, false, true, maque::tracy::color(maque::tracy::Module::runtime));
    MQ_TRACY_PLOT_CONFIG("logic", 0, false, true, maque::tracy::color(maque::tracy::Module::script));
    MQ_TRACY_PLOT_CONFIG("render", 0, false, true, maque::tracy::color(maque::tracy::Module::render));
    MQ_TRACY_PLOT_CONFIG("present", 0, false, true, maque::tracy::color(maque::tracy::Module::platform));
    MQ_TRACY_PLOT_CONFIG("fps", 0, false, true, maque::tracy::color(maque::tracy::Module::ui));
    MQ_TRACY_PLOT_CONFIG("draws", 0, false, true, 0x9E9E9E);
    MQ_TRACY_PLOT_CONFIG("entities", 0, false, true, maque::tracy::color(maque::tracy::Mem::node));
    MQ_TRACY_PLOT_CONFIG("cpu", 2 /*percentage*/, false, true, maque::tracy::color(maque::tracy::Module::input));
    MQ_TRACY_PLOT_CONFIG("memory", 1 /*memory*/, false, true, maque::tracy::area_color(maque::tracy::Area::system));
    maque::tracy::thread_name("main");
    MQ_TRACY_MESSAGE("Tracy client started");
    const char* port = std::getenv("MAQUE_TRACY_PORT");
    std::print("runtime: [tracy] client started ({}) — listening on 127.0.0.1:{}\n", why,
               port && *port ? port : "8086");
}

void Engine::tracy_launch_gui() {
    if (tracy_gui_launched) return;
    if (!maque::tracy::started()) return;          // 未编入/启动失败：不弹 GUI
    const std::string exe = find_tracy_profiler();
    if (exe.empty()) {
        std::print(stderr,
                   "runtime: [tracy] 未找到 tracy-profiler；Windows 跑 tools/tracy/get-profiler.ps1 下载、"
                   "Linux/macOS 跑 tools/tracy/build-profiler.sh 构建 GUI，"
                   "或设 MAQUE_TRACY_PROFILER=<path>\n");
        return;
    }
    const char* env_port = std::getenv("MAQUE_TRACY_PORT");
    const std::string port = (env_port && *env_port) ? env_port : "8086";
    const std::string args = "-a 127.0.0.1 -p " + port;
    const int rc = platform ? mq_platform_spawn_detached(platform, exe.c_str(), args.c_str()) : 1;
    if (rc == 0) {
        tracy_gui_launched = true;
        std::print("runtime: [tracy] launched profiler '{} {}'\n", exe, args);
    } else {
        std::print(stderr, "runtime: [tracy] 启动 profiler 失败（rc={}）: '{}'\n", rc, exe);
    }
}

void Engine::tracy_tick() {
    if (!tracy_env_checked) {
        tracy_env_checked = true;
        const char* e = std::getenv("MAQUE_TRACY");
        if (e && *e && std::strcmp(e, "0") != 0) tracy_start("MAQUE_TRACY");
    }
    if (!maque::tracy::started()) return;
    /* 1Hz：进程 CPU%/常驻内存（Tracy 基础数据）。 */
    if (frame % 60 != 0 || !platform) return;
    double cpu = 0.0;
    uint64_t mem = 0;
    if (mq_platform_process_stats(platform, &cpu, &mem) == 0) {
        MQ_TRACY_PLOT("cpu", cpu);
        MQ_TRACY_PLOT("memory", (double)mem);
    }
}

/* ---- Tracy 内存域采样（docs/29 §10）----
 * 采样型域每 MAQUE_TRACY_MEM_INTERVAL（默认 1s）刷新一次；每个域只有一个写入者：
 *   · node    = 场景实体表 + 组件 blob + 名字/树/实例元数据（**估算**：map 桶开销按 3 指针/节点计）
 *   · texture = 渲染器 texture_bytes（含字形/ImGui 图集、RenderTexture、env cube）
 *   · gpu     = 渲染器 buffer_bytes（网格顶点/索引；旧插件只有总量时回退到 gpu）
 *   · font    = 文本模块 mq_font_memory_bytes（CPU 侧图集/字形表）
 * delta 型域（script / heap）由各自的分配器钩子维护，这里不碰。 */
void Engine::memory_sample() {
    /* ---- 节点树 ---- */
    uint64_t node = 0;
    if (scene) {
        node += sizeof(Scene);
        node += (uint64_t)scene->desc.title.capacity() + scene->desc.script.capacity() +
                scene->desc.overlay_script.capacity() + scene->desc.global_script.capacity() +
                scene->desc.env_texture.capacity() + scene->desc.env_cubemap.capacity();
        node += (uint64_t)scene->desc.scripts.capacity() * sizeof(std::string);
        for (const auto& s : scene->desc.scripts) node += (uint64_t)s.capacity();
        node += (uint64_t)scene->text_.capacity();
        node += (uint64_t)scene->nodes.size() * (sizeof(void*) * 3 + sizeof(SceneNode));   // 桶+链表节点估算
        for (const auto& [id, e] : scene->nodes) {
            (void)id;
            node += (uint64_t)e.name.capacity();
            node += (uint64_t)e.comps.size() *
                    (sizeof(uint32_t) + sizeof(std::vector<uint8_t>) + sizeof(void*) * 2);
            for (const auto& [tid, blob] : e.comps) { (void)tid; node += (uint64_t)blob.capacity(); }
        }
    }
    for (const auto& [iid, inst] : instances) {
        (void)iid;
        if (!inst) continue;
        node += sizeof(SceneInstanceRuntime);
        node += (uint64_t)inst->source.capacity() + inst->base.capacity() + inst->params.capacity() +
                inst->error.capacity();
        node += (uint64_t)inst->roots.capacity() * sizeof(int) +
                (uint64_t)inst->children.capacity() * sizeof(int);
        node += (uint64_t)inst->scripts.capacity() * sizeof(std::string) +
                (uint64_t)inst->script_hashes.capacity() * sizeof(uint64_t);
        for (const auto& s : inst->scripts) node += (uint64_t)s.capacity();
    }
    maque::tracy::memory_set(maque::tracy::Mem::node, node);

    /* ---- 字体/图集（CPU 侧）---- */
    uint64_t font = 0;
    for (const auto& fe : fonts) font += mq_font_memory_bytes(fe.font);
    font += mq_imgui_font_atlas_bytes();    /* 调试面板的静态图集（未打开/官方后端自管 → 0） */
    maque::tracy::memory_set(maque::tracy::Mem::font, font);

    /* ---- GPU：纹理 / 缓冲（渲染器是唯一真相）---- */
    uint64_t tex = 0, buf = 0;
    bool split = false;
    if (rapi && rself && MQ_RENDERER_HAS(rapi, resource_bytes2)) {
        mq_renderer_resource_usage u{};
        u.struct_size = (uint32_t)sizeof u;
        if (rapi->resource_bytes2(rself, &u) == 0) {
            tex = u.texture_bytes;
            buf = u.buffer_bytes + u.other_bytes;
            split = true;
        }
    }
    if (!split) buf = stat_vram_bytes;     /* 旧插件：只有总量 → 记到 gpu（texture 保持 0） */
    maque::tracy::memory_set(maque::tracy::Mem::texture, tex);
    maque::tracy::memory_set(maque::tracy::Mem::gpu, buf);

    /* ---- 控制面快照（Perf.get / CI 泄漏门）----
     * 与 Tracy 内存域同一份真相：采样型域用刚算出的值，delta 型域（script/heap）读分配器账本。
     * 键名带 `mem.` 前缀，避免与既有 metrics（entities/draws/...）冲突；值是字节数。
     * 这是**当前值 + 峰值 + 分配/释放次数**，够做"跑 N 帧后回到基线"的断言；需要调用栈/逐块
     * 归因时仍走 Tracy（MAQUE_TRACY=1 + Memory 视图）。 */
    auto gauge = [&](const char* name, uint64_t value) {
        mq_obs_gauge_set(obs, name, (double)value);
    };
    auto gauge_domain = [&](const char* name, maque::tracy::Mem m) {
        const maque::tracy::MemInfo info = maque::tracy::memory_info(m);
        char key[64];
        std::snprintf(key, sizeof key, "mem.%s", name);
        gauge(key, info.bytes);
        std::snprintf(key, sizeof key, "mem.peak.%s", name);
        gauge(key, info.peak);
        std::snprintf(key, sizeof key, "mem.%s.allocs", name);
        gauge(key, info.allocs);
        std::snprintf(key, sizeof key, "mem.%s.frees", name);
        gauge(key, info.frees);
    };
    gauge_domain("script", maque::tracy::Mem::script);
    gauge_domain("node", maque::tracy::Mem::node);
    gauge_domain("texture", maque::tracy::Mem::texture);
    gauge_domain("gpu", maque::tracy::Mem::gpu);
    gauge_domain("font", maque::tracy::Mem::font);
    gauge_domain("heap", maque::tracy::Mem::heap);
    gauge("mem.tracked", maque::tracy::memory_total());
}

}  // namespace maque::rt
