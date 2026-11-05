/* 场景自定义 shader（全屏 pass / 多通道 graph / shader 精灵；docs/13）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S6，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

/* ==========================================================================
 * 场景资源全屏 shader pass（docs/13）
 *   · 资源是场景目录里的 .wgsl（相对路径、VFS 根 = 场景目录）——**不是** builtin shader；
 *   · 装配期加载/编译一次；每帧只推进 time（模拟 dt，确定性）并提交给渲染器；
 *   · 失败（文件缺失/编译错）打印明确原因并跳过，场景其余部分照常运行。
 * ==========================================================================*/
/* `render.ShaderSprite.shader` / `render.ShaderGraph.program` 是否指向多阶段 program：
 * 约定按扩展名 `.json` 判定（.wgsl = 单 pass 契约；program JSON = 多阶段图）。 */
static bool is_shader_graph_program(const std::string& path) {
    if (path.size() < 5) return false;
    std::string ext = path.substr(path.size() - 5);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".json";
}

void Engine::destroy_shader_passes() {
    if (rapi && rself && rapi->destroy_shader_pass)
        for (auto& p : loaded_shader_passes)
            if (p.handle >= 0) rapi->destroy_shader_pass(rself, p.handle);
    loaded_shader_passes.clear();
    has_shader_pass = false;
}

void Engine::setup_shader_passes() {
    destroy_shader_passes();    if (!scene || !vfs || !rapi || !rself || !rapi->create_shader_pass) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    /* 两种组件共用同一套加载/编译路径：全屏 pass 与 shader 精灵只是在 gfx 侧的绘制方式不同。 */
    auto load = [&](const char* type, bool sprite) {
        for (int id : ids) {
            const mq_type_desc* d = nullptr;
            auto* blob = comp_by_name(*this, id, type, &d);
            if (!blob || !d) continue;
            const std::string* path = read_str(d, blob->data(), "shader");
            if (!path || path->empty()) continue;
            /* `render.ShaderSprite.shader` 指向 program JSON 时是多阶段 shader 图（由
             * setup_shader_graphs 加载），不能当 WGSL 编译。 */
            if (sprite && is_shader_graph_program(*path)) continue;
            void* src = nullptr;
            size_t n = 0;
            const std::string vpath = "/" + *path;
            if (mq_vfs_read(vfs, vpath.c_str(), &src, &n) != MQ_VFS_OK || !src || n == 0) {
                std::print(stderr, "runtime: {} '{}' 未找到（相对场景目录，经 VFS 读取）\n",
                           sprite ? "shader sprite" : "shader pass", *path);
                if (src) mq_vfs_free(src);
                continue;
            }
            std::string wgsl((const char*)src, n);
            mq_vfs_free(src);
            int32_t handle = -1;
            const int32_t rc = rapi->create_shader_pass(rself, wgsl.c_str(), &handle);
            if (rc != 0 || handle < 0) {
                std::print(stderr, "runtime: {} '{}' 编译失败 rc={}"
                                   "（需 @fragment fs_main + group0 binding0 的 32B uniform，见 docs/13）\n",
                           sprite ? "shader sprite" : "shader pass", *path, (int)rc);
                continue;
            }
            loaded_shader_passes.push_back({ id, handle, sprite });
            std::print("runtime: {} '{}' loaded (entity={}, id={})\n",
                       sprite ? "shader sprite" : "shader pass", *path, id, (long long)handle);
        }
    };
    load("render.ShaderPass", false);
    load("render.ShaderSprite", true);
    has_shader_pass = !loaded_shader_passes.empty();
}

void Engine::shader_pass_step(float dt) {
    if (!scene) return;
    std::vector<int> ids;
    scene->collect_preorder(ids);
    /* 三种时间驱动器共享 speed/paused/time；类型在编译期已知 → 直接成员推进。
     * 原实现每实体做 3 次按名查组件 + 4 次按名查字段（每次 ~30ns）。 */
    auto step = [dt](auto* c) {
        if (!c || c->paused || c->speed == 0.0F) return;
        c->time += dt * c->speed;
    };
    for (int id : ids) {
        SceneNode* e = scene->at(id);
        if (!e) continue;
        step(component<render::ShaderPass>(*e));
        step(component<render::ShaderSprite>(*e));
        step(component<render::ShaderGraph>(*e));
    }
}

void Engine::extract_shader_passes(std::vector<mq_renderer_shader_pass>& out) {
    out.clear();
    if (!scene || loaded_shader_passes.empty()) return;
    /* 指针：世界设计坐标 → 屏幕 Canvas → 渲染目标像素。 */
    const float rx = canvas.window_w > 0.0F ? (float)TW / canvas.window_w : render_scale_req;
    const float ry = canvas.window_h > 0.0F ? (float)TH / canvas.window_h : render_scale_req;
    const float mx = (canvas.offset_x + mouse_x * canvas.scale_x) * rx;
    const float my = (canvas.offset_y + ((float)H - mouse_y) * canvas.scale_y) * ry;
    for (const auto& p : loaded_shader_passes) {
        if (p.sprite) continue;                       /* 精灵走 extract_shader_sprites（2D 图层） */
        SceneNode* e = scene->at(p.entity);
        const auto* comp = e ? component<render::ShaderPass>(*e) : nullptr;
        if (!comp || !comp->enabled) continue;
        mq_renderer_shader_pass sp{};
        sp.shader = p.handle;
        sp.time = comp->time;
        sp.frame = (float)frame;
        sp.mouse[0] = mx;
        sp.mouse[1] = my;
        out.push_back(sp);
    }
}

/* ==========================================================================
 * 场景资源多通道 shader 图（render.ShaderGraph；docs/13 §多通道 shader）
 *
 * program JSON（路径相对场景目录，经 VFS 读取）：
 *   {
 *     "buffers": [ { "name": "a", "size": 540 }, ... ],          // RGBA32F 中间缓冲
 *     "passes": [
 *       { "name": "buffer_a", "shader": "buffer_a.wgsl", "common": "common.wgsl",
 *         "target": "a", "channels": ["self", "", "", ""] },
 *       ...,
 *       { "name": "image", "shader": "image.wgsl", "common": "common.wgsl",
 *         "target": "", "channels": ["b", "c", "", ""] }          // "" / "screen" = 主目标
 *     ]
 *   }
 *
 * 语义：
 *   · 每个 buffer 两张纹理（ping-pong）：本帧写 tex[write]、上一帧内容在 tex[1-write]；
 *     所有 pass 都写完后下一帧再交换，所以 "self"/"prev" 永远读到上一帧（自反馈）。
 *   · 同一帧内引用某个 buffer 时读它**本帧最新**的内容（前面 pass 刚写的），因此可串联；
 *     引用本 pass 的目标缓冲时读到的是上一帧内容（ping-pong 保证不会同 pass 读自己）。
 *   · target 为空 / "screen" / "main" = 主渲染目标（背景层，先于 3D/2D 绘制）。
 *   · common 非空时把 common 源码拼在每个 pass 源码之前（共享函数/常量库）。
 * ==========================================================================*/

void Engine::shader_graph_swap_targets() {
    for (auto& g : loaded_shader_graphs)
        for (auto& b : g.buffers)
            b.write ^= 1;
}

/* `render.ShaderSprite.shader` / `render.ShaderGraph.program` 是否指向多阶段 program：
 * 约定按扩展名 `.json` 判定（.wgsl = 单 pass 契约；program JSON = 多阶段图）。 */

void Engine::destroy_shader_graphs() {
    if (rapi && rself) {
        if (rapi->destroy_shader_graph_pass)
            for (auto& g : loaded_shader_graphs)
                for (auto& p : g.passes)
                    if (p.shader >= 0) rapi->destroy_shader_graph_pass(rself, p.shader);
        if (rapi->destroy_texture)
            for (auto& g : loaded_shader_graphs)
                for (auto& b : g.buffers)
                    for (int k = 0; k < 2; ++k)
                        if (b.tex[k] >= 0) rapi->destroy_texture(rself, b.tex[k]);
    }
    loaded_shader_graphs.clear();
    has_shader_graph = false;
    shader_graph_swap_frame = ~0ull;
}

void Engine::setup_shader_graphs() {
    destroy_shader_graphs();
    if (!scene || !vfs || !rapi || !rself) return;
    if (!rapi->create_shader_graph_pass || !rapi->create_shader_graph_target) return;

    auto read_text = [&](const std::string& rel, std::string& out) -> bool {
        void* src = nullptr;
        size_t n = 0;
        const std::string vpath = "/" + rel;
        if (mq_vfs_read(vfs, vpath.c_str(), &src, &n) != MQ_VFS_OK || !src || n == 0) {
            if (src) mq_vfs_free(src);
            return false;
        }
        out.assign((const char*)src, n);
        mq_vfs_free(src);
        return true;
    };

    std::vector<int> ids;
    scene->collect_preorder(ids);

    /* 加载一份 program：背景形态（render.ShaderGraph）与精灵形态（render.ShaderSprite + .json）共用。 */
    auto load_program = [&](int id, const std::string& program_path, bool sprite) {
        const std::string* path = &program_path;
        std::string text;
        if (!read_text(*path, text)) {
            std::print(stderr, "runtime: shader graph program '{}' 未找到（相对场景目录，经 VFS 读取）\n", *path);
            return;
        }
        mq_json* dom = mq_json_parse(text.data(), text.size());
        if (!dom) {
            std::print(stderr, "runtime: shader graph program '{}' JSON 解析失败\n", *path);
            return;
        }

        LoadedShaderGraph g;
        g.entity = id;
        g.program_path = *path;
        g.sprite = sprite;
        g.start_frame = frame;          /* 阶段的 frame 从 0 起算：重载后自反馈缓冲会重新初始化 */
        /* pass/common 的路径相对 **program 文件所在目录**（program 自身相对场景目录）。 */
        std::string base_dir;
        {
            const size_t slash = path->find_last_of('/');
            if (slash != std::string::npos) base_dir = path->substr(0, slash + 1);
        }
        auto resolve = [&](const std::string& rel) -> std::string {
            if (rel.empty()) return rel;
            if (rel[0] == '/') return rel.substr(1);
            return base_dir + rel;
        };

        /* 中间缓冲：每个两张纹理（ping-pong）。`size` = 正方形边长；也可以写 `width`+`height`
         * 要非方形缓冲（例如按屏幕比例的画面缓冲、光照单元网格），`size` 仍然有效且优先。 */
        const mq_json* jbufs = mq_json_get(dom, "buffers");
        const uint32_t nbuf = mq_json_len(jbufs);
        for (uint32_t i = 0; i < nbuf; ++i) {
            const mq_json* jb = mq_json_at(jbufs, i);
            const char* name = mq_json_str(mq_json_get(jb, "name"), "");
            const int32_t size = (int32_t)mq_json_num(mq_json_get(jb, "size"), 0);
            int32_t bw = size, bh = size;
            if (size <= 0) {
                bw = (int32_t)mq_json_num(mq_json_get(jb, "width"), 0);
                bh = (int32_t)mq_json_num(mq_json_get(jb, "height"), 0);
            }
            if (!name || !*name || bw <= 0 || bh <= 0) {
                std::print(stderr, "runtime: shader graph '{}' buffers[{}] 需要 name + "
                                   "(size>0 或 width>0 且 height>0)，已跳过\n", *path, i);
                continue;
            }
            ShaderGraphBuffer b;
            b.name = name;
            b.w = bw;
            b.h = bh;
            bool ok = true;
            for (int k = 0; k < 2 && ok; ++k) {
                int32_t tid = -1;
                if (rapi->create_shader_graph_target(rself, b.w, b.h, &tid) != 0 || tid < 0) {
                    std::print(stderr, "runtime: shader graph '{}' 缓冲 '{}' 创建失败（{}x{}）\n",
                               *path, b.name, b.w, b.h);
                    ok = false;
                } else {
                    b.tex[k] = tid;
                }
            }
            if (!ok) {
                for (int k = 0; k < 2; ++k)
                    if (b.tex[k] >= 0) rapi->destroy_texture(rself, b.tex[k]);
                continue;
            }
            g.buffers.push_back(std::move(b));
        }

        auto buffer_index = [&](const char* name) -> int32_t {
            if (!name || !*name) return -1;
            for (size_t bi = 0; bi < g.buffers.size(); ++bi)
                if (g.buffers[bi].name == name) return (int32_t)bi;
            return -1;
        };

        /* 阶段：shader（+ 可选 common）→ 编译；target/channels 解析成缓冲下标。 */
        const mq_json* jpasses = mq_json_get(dom, "passes");
        const uint32_t npass = mq_json_len(jpasses);
        for (uint32_t i = 0; i < npass; ++i) {
            const mq_json* jp = mq_json_at(jpasses, i);
            const char* shader = mq_json_str(mq_json_get(jp, "shader"), "");
            const char* common = mq_json_str(mq_json_get(jp, "common"), "");
            const char* target = mq_json_str(mq_json_get(jp, "target"), "");
            if (!shader || !*shader) {
                std::print(stderr, "runtime: shader graph '{}' passes[{}] 缺 shader，已跳过\n", *path, i);
                continue;
            }
            std::string src;
            if (common && *common) {
                if (!read_text(resolve(common), src)) {
                    std::print(stderr, "runtime: shader graph '{}' common '{}' 未找到\n", *path, common);
                    continue;
                }
                src += "\n";
            }
            std::string body;
            if (!read_text(resolve(shader), body)) {
                std::print(stderr, "runtime: shader graph '{}' pass '{}' 未找到\n", *path, shader);
                continue;
            }
            src += body;
            int32_t handle = -1;
            if (rapi->create_shader_graph_pass(rself, src.c_str(), &handle) != 0 || handle < 0) {
                std::print(stderr, "runtime: shader graph '{}' pass '{}' 编译失败"
                                   "（common='{}'；契约见 docs/13 §多通道 shader）\n",
                           *path, shader, (common && *common) ? common : "");
                continue;
            }
            ShaderGraphPass p;
            p.path = shader;
            p.name = mq_json_str(mq_json_get(jp, "name"), shader);
            p.shader = handle;
            if (target && *target && std::strcmp(target, "screen") != 0 && std::strcmp(target, "main") != 0) {
                p.target_buffer = buffer_index(target);
                if (p.target_buffer < 0) {
                    std::print(stderr, "runtime: shader graph '{}' pass '{}' 的输出缓冲 '{}' 不存在，已跳过\n",
                               *path, shader, target);
                    rapi->destroy_shader_graph_pass(rself, handle);
                    continue;
                }
            }
            const mq_json* jch = mq_json_get(jp, "channels");
            for (int k = 0; k < 4; ++k) {
                const char* cn = mq_json_str(mq_json_at(jch, (uint32_t)k), "");
                if (!cn || !*cn) continue;
                if (std::strcmp(cn, "self") == 0 || std::strcmp(cn, "prev") == 0) {
                    p.channels[k] = p.target_buffer;          /* 自反馈：上一帧内容（ping-pong） */
                } else {
                    p.channels[k] = buffer_index(cn);
                    if (p.channels[k] < 0)
                        std::print(stderr, "runtime: shader graph '{}' pass '{}' 的 iChannel{} '{}' 不存在（按未绑定处理）\n",
                                   *path, shader, k, cn);
                }
            }
            g.passes.push_back(std::move(p));
        }
        mq_json_free(dom);

        if (g.passes.empty()) {
            /* 全部失败：回收缓冲，保留上面的清晰报错（场景其余部分照常运行）。 */
            for (auto& b : g.buffers)
                for (int k = 0; k < 2; ++k)
                    if (b.tex[k] >= 0) rapi->destroy_texture(rself, b.tex[k]);
            return;
        }
        std::print("runtime: {} '{}' loaded (entity={}, passes={}, buffers={})\n",
                   sprite ? "shader sprite graph" : "shader graph",
                   *path, id, (long long)g.passes.size(), (long long)g.buffers.size());
        loaded_shader_graphs.push_back(std::move(g));
    };

    /* 背景形态：render.ShaderGraph.program */
    for (int id : ids) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, id, "render.ShaderGraph", &d);
        if (!blob || !d) continue;
        const std::string* path = read_str(d, blob->data(), "program");
        if (!path || path->empty()) continue;
        load_program(id, *path, false);
    }
    /* 精灵形态：render.ShaderSprite.shader 指向 program JSON（与 .wgsl 共用同一个字段）。 */
    for (int id : ids) {
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, id, "render.ShaderSprite", &d);
        if (!blob || !d) continue;
        const std::string* path = read_str(d, blob->data(), "shader");
        if (!path || path->empty() || !is_shader_graph_program(*path)) continue;
        load_program(id, *path, true);
    }
    has_shader_graph = !loaded_shader_graphs.empty();
}

/* 一条阶段链的提取（背景形态与精灵形态共用）：缓冲的最新纹理 + 每个阶段的目标/通道/尺寸/输入。
 * screen_w/h = 该图的"屏幕"空间（背景 = 渲染目标像素；精灵 = 精灵矩形设计像素），
 * 主目标阶段的分辨率与 screen_resolution 都取它，mouse 也必须在同一空间。 */
void Engine::build_shader_graph_stages(const LoadedShaderGraph& g, float time, float frame_value,
                                       float mx, float my, bool pressed, const float keys[4],
                                       float screen_w, float screen_h,
                                       std::vector<mq_renderer_shader_graph_pass>& out) {
    /* 帧初：每个 buffer 的最新内容 = 上一帧写的那张（tex[1-write]）。 */
    std::vector<int32_t> latest(g.buffers.size());
    for (size_t i = 0; i < g.buffers.size(); ++i)
        latest[i] = g.buffers[i].tex[1 - g.buffers[i].write];
    for (const auto& p : g.passes) {
        mq_renderer_shader_graph_pass rp{};
        rp.shader = p.shader;
        rp.frame = frame_value;
        rp.time = time;
        rp.mouse[0] = mx;
        rp.mouse[1] = my;
        rp.mouse[2] = pressed ? 1.0F : 0.0F;
        rp.mouse[3] = 0.0F;
        for (int k = 0; k < 4; ++k) rp.keys[k] = keys[k];
        rp.screen_resolution[0] = screen_w;
        rp.screen_resolution[1] = screen_h;
        if (p.target_buffer >= 0) {
            const ShaderGraphBuffer& b = g.buffers[(size_t)p.target_buffer];
            rp.target = b.tex[b.write];
            rp.resolution[0] = (float)b.w;
            rp.resolution[1] = (float)b.h;
        } else {
            rp.target = -1;
            rp.resolution[0] = screen_w;
            rp.resolution[1] = screen_h;
        }
        for (int k = 0; k < 4; ++k) {
            const int32_t bi = p.channels[k];
            rp.channels[k] = (bi >= 0 && bi < (int32_t)latest.size()) ? latest[(size_t)bi] : -1;
        }
        /* 保护：同一 pass 不能读写同一张纹理（WebGPU 校验会拒绝）。正常声明不会触发
         *（"self" 走 ping-pong 的另一张）；这里兜住"同一帧内重复写同一个缓冲"的写法。 */
        for (int k = 0; k < 4; ++k) {
            if (rp.channels[k] >= 0 && rp.channels[k] == rp.target) {
                const int32_t bi = p.channels[k];
                rp.channels[k] = g.buffers[(size_t)bi].tex[1 - g.buffers[(size_t)bi].write];
            }
        }
        out.push_back(rp);
        if (p.target_buffer >= 0) latest[(size_t)p.target_buffer] = rp.target;
    }
}

/* 每帧提取（背景形态：render.ShaderGraph.program）。 */
void Engine::extract_shader_graphs(std::vector<mq_renderer_shader_graph_pass>& out) {
    out.clear();
    if (!scene || loaded_shader_graphs.empty() || !rapi || !rself) return;
    /* ping-pong 每**模拟帧**只切换一次：同一模拟帧可能多次成帧（主目标 + RenderTexture）。 */
    if (frame != shader_graph_swap_frame) {
        shader_graph_swap_frame = frame;
        shader_graph_swap_targets();
    }
    /* 指针：世界设计坐标 → 屏幕 Canvas → 渲染目标像素（与 render.ShaderPass 同一条映射）。 */
    const float rx = canvas.window_w > 0.0F ? (float)TW / canvas.window_w : render_scale_req;
    const float ry = canvas.window_h > 0.0F ? (float)TH / canvas.window_h : render_scale_req;
    const float mx = (canvas.offset_x + mouse_x * canvas.scale_x) * rx;
    const float my = (canvas.offset_y + ((float)H - mouse_y) * canvas.scale_y) * ry;
    const bool pressed = input && mq_input_pointer_down(input, 0) != 0;
    float keys[4] = { 0.0F, 0.0F, 0.0F, 0.0F };
    if (input) {
        keys[0] = mq_input_key_down(input, MQ_KEY_BACKSPACE) ? 1.0F : 0.0F;
        keys[1] = shift_down ? 1.0F : 0.0F;
        keys[2] = mq_input_key_down(input, MQ_KEY_ENTER) ? 1.0F : 0.0F;
    }
    for (auto& g : loaded_shader_graphs) {
        if (g.sprite) continue;                  /* 精灵形态 → extract_shader_graph_sprites */
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, g.entity, "render.ShaderGraph", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        const float time = read_num(d, blob->data(), "time", 0.0F);
        build_shader_graph_stages(g, time, (float)(frame - g.start_frame),
                                  mx, my, pressed, keys, (float)TW, (float)TH, out);
    }
}

/* 多通道 shader 图的精灵形态（render.ShaderGraph 且 width/height>0）：
 * 位置/尺寸/旋转/透明度语义与 render.ShaderSprite 一致；每个精灵自带完整阶段链，
 * 主目标阶段先渲染到该精灵的离屏纹理，再由 gfx 当普通 2D 图元合成。 */
void Engine::extract_shader_graph_sprites(std::vector<mq_renderer_shader_graph_sprite>& out) {
    out.clear();
    graph_sprite_stage_scratch.clear();
    if (!scene || loaded_shader_graphs.empty() || !rapi || !rself) return;
    /* F4「2D/UI 图层」开关只隐藏游戏层（时间仍照常推进）。 */
    if (!opt_2d_enabled) return;
    if (frame != shader_graph_swap_frame) {
        shader_graph_swap_frame = frame;
        shader_graph_swap_targets();
    }
    const bool pressed = input && mq_input_pointer_down(input, 0) != 0;
    float keys[4] = { 0.0F, 0.0F, 0.0F, 0.0F };
    if (input) {
        keys[0] = mq_input_key_down(input, MQ_KEY_BACKSPACE) ? 1.0F : 0.0F;
        keys[1] = shift_down ? 1.0F : 0.0F;
        keys[2] = mq_input_key_down(input, MQ_KEY_ENTER) ? 1.0F : 0.0F;
    }
    std::vector<size_t> offsets;                  /* 每个精灵的阶段在 scratch 里的起点 */
    for (auto& g : loaded_shader_graphs) {
        if (!g.sprite) continue;                  /* 背景形态 → extract_shader_graphs */
        SceneNode* e = scene->at(g.entity);
        if (!e) continue;
        /* 精灵形态的参数（width/height/enabled/time）在 render.ShaderSprite 上。 */
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, g.entity, "render.ShaderSprite", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        float w = 0.0F, h = 0.0F;
        read_f32(d, blob->data(), "width", &w);
        read_f32(d, blob->data(), "height", &h);
        if (w <= 0.0F || h <= 0.0F) continue;
        if (!node_active_effective(*this, g.entity)) continue;
        float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
        accumulate_node_attrs(*this, g.entity, node_angle, node_scale, node_opacity);
        const float half_w = w * 0.5F * node_scale;
        const float half_h = h * 0.5F * node_scale;
        const float alpha = std::clamp(node_opacity, 0.0F, 1.0F);
        if (half_w <= 0.0F || half_h <= 0.0F || alpha <= 0.0F) continue;
        /* 指针：世界坐标 → 精灵内局部**设计像素**（原点左上）；在矩形外 → 不按下、坐标 0。 */
        const float lx = mouse_x - (e->world_x - half_w);
        const float ly = (e->world_y + half_h) - mouse_y;
        const bool inside = lx >= 0.0F && ly >= 0.0F && lx <= 2.0F * half_w && ly <= 2.0F * half_h;
        const float mx = inside ? lx : 0.0F;
        const float my = inside ? ly : 0.0F;
        const bool sprite_pressed = pressed && inside;
        const float time = read_num(d, blob->data(), "time", 0.0F);

        const size_t begin = graph_sprite_stage_scratch.size();
        build_shader_graph_stages(g, time, (float)(frame - g.start_frame),
                                  mx, my, sprite_pressed, keys,
                                  2.0F * half_w, 2.0F * half_h, graph_sprite_stage_scratch);

        mq_renderer_shader_graph_sprite s{};
        s.z = g.entity;                              /* 稳定序（与 2D 图元的 z = 实体 id 一致） */
        s.x = e->world_x;
        s.y = (float)H - e->world_y;                 /* 世界 y 向上 → 屏幕 y 向下 */
        s.half_w = half_w;
        s.half_h = half_h;
        s.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
        s.a = alpha;
        s.passes = nullptr;                          /* 阶段全部追加完后再取指针（vector 可能扩容） */
        s.pass_count = (uint32_t)(graph_sprite_stage_scratch.size() - begin);
        if (s.pass_count == 0) continue;
        out.push_back(s);
        offsets.push_back(begin);
    }
    for (size_t i = 0; i < out.size(); ++i)
        out[i].passes = graph_sprite_stage_scratch.data() + offsets[i];
}

/* shader 精灵 → 2D 图元式记录（位置/尺寸/旋转/透明度语义与 scn.Sprite 一致）。
 * 位置是**设计坐标**（中心；y 已翻到屏幕向下），尺寸含节点 scale；gfx 据此先渲染离屏纹理、
 * 再当普通带纹理矩形合成，因此画布缩放/letterbox/旋转/图层顺序都走既有 2D 通路。 */
void Engine::extract_shader_sprites(std::vector<mq_renderer_shader_sprite>& out) {
    out.clear();
    if (!scene || loaded_shader_passes.empty()) return;
    /* F4「2D/UI 图层」开关只隐藏游戏层：shader 精灵是 2D 图元，一并隐藏（时间仍照常推进）。 */
    if (!opt_2d_enabled) return;
    for (const auto& p : loaded_shader_passes) {
        if (!p.sprite) continue;
        SceneNode* e = scene->at(p.entity);
        if (!e) continue;
        const mq_type_desc* d = nullptr;
        auto* blob = comp_by_name(*this, p.entity, "render.ShaderSprite", &d);
        if (!blob || !d) continue;
        if (read_num(d, blob->data(), "enabled", 1.0F) < 0.5F) continue;
        if (!node_active_effective(*this, p.entity)) continue;
        float w = 64.0F, h = 64.0F;
        read_f32(d, blob->data(), "width", &w);
        read_f32(d, blob->data(), "height", &h);
        if (w <= 0.0F || h <= 0.0F) continue;
        float node_angle = 0.0F, node_scale = 1.0F, node_opacity = 1.0F;
        accumulate_node_attrs(*this, p.entity, node_angle, node_scale, node_opacity);
        const float half_w = w * 0.5F * node_scale;
        const float half_h = h * 0.5F * node_scale;
        const float alpha = std::clamp(node_opacity, 0.0F, 1.0F);
        if (alpha <= 0.0F) continue;
        mq_renderer_shader_sprite s{};
        s.shader = p.handle;
        s.z = p.entity;                              /* 稳定序（与 2D 图元的 z = 实体 id 一致） */
        s.x = e->world_x;
        s.y = (float)H - e->world_y;                 /* 世界 y 向上 → 屏幕 y 向下 */
        s.half_w = half_w;
        s.half_h = half_h;
        s.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
        s.a = alpha;
        s.time = read_num(d, blob->data(), "time", 0.0F);
        s.frame = (float)frame;
        /* 指针：世界坐标 → 精灵内局部**设计像素**（原点左上）。在矩形外 → 0（shader 的鼠标语义）。 */
        const float lx = mouse_x - (e->world_x - half_w);
        const float ly = (e->world_y + half_h) - mouse_y;
        if (lx >= 0.0F && ly >= 0.0F && lx <= 2.0F * half_w && ly <= 2.0F * half_h) {
            s.mouse[0] = lx;
            s.mouse[1] = ly;
        }
        out.push_back(s);
    }
}

}  // namespace maque::rt
