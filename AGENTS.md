# AGENTS.md — 面向 AI 协作者

maque 是一个以 **"引擎即浏览器、游戏即资源"** 为核心理念的轻量游戏引擎（C++23）。
本文件给自动化协作者提供**构建/测试命令、约定与架构落点**。人读概览见 `README.md`。

## 构建与测试（务必用这些命令）

```bash
# 首次：codegen 依赖（node + yaml）
cd modules/core/data && npm install && cd -

# 构建（Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 全部测试（CTest）
ctest --test-dir build --output-on-failure

# 只跑 codegen 校验（schema 语法/尺寸/引用）
cd modules/core/data && node parser.js --check

# 运行示例
./build/samples/player/maque_player scenes/ball.scn
```

- `-DMAQUE_BUILD_TESTS=ON|OFF` 控制测试（默认 ON）。
- **窗口（宿主启动期覆盖）**：`MAQUE_WINDOW=800x600` 设窗口尺寸（世界/渲染目标仍是场景尺寸，
  窗口内**等比缩放 + 居中留边**显示，输入坐标自动映射回世界坐标）；`MAQUE_TITLE=<标题>`；
  `MAQUE_VIEWS=n`；`MAQUE_PRESENT=off|software|accelerated`（默认 software：纯窗口表面，
  不初始化 GL/EGL，避免与渲染插件的 wgpu(EGL) 争用）；`MAQUE_CAPTURE=<前缀>` 从**窗口表面**
  回读前 3 帧为 PPM（用于真实窗口路径的像素级验证）；`MAQUE_KEEP_OPEN=1` 忽略脚本 `quit()`。
  示例：`MAQUE_WINDOW=800x600 MAQUE_TITLE=MaqueEngine ./build/samples/player/maque_player scenes/ball.scn`
- **输入（真实设备事件）**：`MAQUE_TRACE_INPUT=1` 打印 SDL 事件与平台队列；`MAQUE_HITS=1` 打印命中测试；
  `MAQUE_SIM_CLICK="frame:x:y[;...]"` 把指针事件压入 **SDL 事件队列**（与真实鼠标同一条翻译路径：
  SDL → 平台 → 输入模块 → 命中测试 → 脚本回调），用于 CI 验证输入通路。
  手动/脚本点击真实窗口：`python3 tools/xclick.py <窗口类名|标题> <窗口内 x> <窗口内 y>`
  （⚠️ 本机 Wayland 会话下 **XTest 注入无效**：指针不动，故 CI 用 `MAQUE_SIM_CLICK`）。
  演示时窗口易被其它窗口遮挡 → `MAQUE_ALWAYS_ON_TOP=1`、`MAQUE_WINDOW_POS=x,y`。
- **输入模拟器（一等机制，docs/19）**：`modules/input/simulator`（接口 DSL 声明 → `abi/sim.h`）。
  场景文件 `.sim.json`（帧索引事件：pointer_move/down/up、click、wheel、key_down/up、text；
  `frame` 绝对或 `delay` 相对；`space` window/world；`loop` 循环），样例见
  `tests/data/sim/ball_clicks.sim.json`。驱动方式：启动期 `MAQUE_SIM=<file>`、简写
  `MAQUE_SIM_CLICK="frame:x:y[;...]"`、控制面 `Input.sim_load/sim_status/sim_events/sim_stop`。
  语义：`mq_sim_due` 取"到期或已过期"事件（按块步进不漏）；模拟事件与设备事件**同一条路径**
  并被录制覆盖 → 录制/回放等价；引擎内部一律世界坐标（设备事件在平台边界转换）。
- **输入帧缓冲必须预分配**：`mq_input_frame.pointers/pointer_cap`（step_frame 内静态缓冲）——
  容量为 0 时平台会静默丢弃**全部**设备事件（曾导致"真实鼠标点击永远到不了脚本"）。
- **控制端点 / Inspector（plans/17、docs/09/11）**：`MAQUE_INSPECT=7777`（或 `tcp:7777` / `uds:/tmp/maque.sock`）
  在游戏内开一个**单线程、非阻塞、帧边界轮询**的 JSON-RPC 端点（与 stdin 控制面同一 dispatch，无线程 → 不改确定性）；
  默认**只读**（D2）：变更类方法（`control_list` 里 `control_mutating` 列出的）返回 `-32001` 并计入 `Control.info.rejected`；
  加 `MAQUE_INSPECT_RW=1` 才允许编辑。inspector 工具（独立进程，ImGui + SDL2 软件渲染）：
  `./build/samples/inspector/maque_inspector --connect 7777`（`--headless <方法…>` 用于 CI，无需显示器）。
  面板：场景树 / 属性（反射驱动，`Scene.get(id,"")` dump 组件全字段）/ 控制台（手写任意方法，与 MCP 同源）/
  **字体图集**（`Font.info`/`Font.glyphs`/`Font.atlas`：看动态图集的实时状态、缩放查看、悬浮显示码点与图集矩形）/
  **预览**（`Capture.screen`：分块传 PPM，可调刷新间隔与缩略图尺寸）/ 模式。
  界面字号：`--font-size`（默认 42px）、`--ui-scale`（连样式一起缩放）、`--tab <面板名>` 直接打开某面板；
  **键盘切页**：`1`~`5` 直选、`Ctrl+Tab` 循环（鼠标不便时用；状态栏有提示）；
  窗口默认**置顶**（`--no-on-top` / `MAQUE_INSPECTOR_ON_TOP=0` 关闭）与 `--pos x,y` 定位 ——
  否则容易被游戏窗口盖住（点击落到游戏上，表现为"tab 点不动"）。配对着用时游戏侧**不要**加 `MAQUE_ALWAYS_ON_TOP`。
  环境变量：`MAQUE_INSPECTOR_FONT_SIZE` / `MAQUE_INSPECTOR_UI_SCALE` / `MAQUE_INSPECTOR_ON_TOP` / `MAQUE_INSPECTOR_POS`；
  诊断/自测注入（本机 XTest 无法移动指针，故用这两条；两者都走真实事件路径）：
  `MAQUE_INSPECTOR_FAKE_KEY=3`（按键切页）、`MAQUE_INSPECTOR_FAKE_CLICK=x,y`（真实挪指针 + 三段式点击）、
  `MAQUE_INSPECTOR_FAKE_CLICK=sweep`（沿 tab 栏扫描点击，验证所有标签页可点）；
  配合 `MAQUE_INSPECTOR_TRACE=1` 打印鼠标 pos/down/clicked/悬停控件/悬停 tab/当前页。
  **点击被"抢回"的坑**：给标签页长期传 `ImGuiTabItemFlags_SetSelected` 会每帧强制选中，
  鼠标点选会被立刻覆盖；请求必须"满足后即清空"（请求页 / 当前页分离）。
  **二进制传输**：控制面是行分隔 JSON → 图集/画面走 **base64 分块**（`offset`/`max`，单块 ≤48KB，客户端在途上限 4 且偏移递增）；
  自建纹理必须 `ImGui::RegisterUserTexture()`（1.92+ 的外部纹理 API），否则后端不创建 SDL_Texture（白图）。
- WebGPU 需 `third_party/wgpu-native/lib/libwgpu_native.so`（二进制**不入库**）。
- 系统依赖：SDL2 / libcurl / libwebsockets（`libsdl2-dev libcurl4-openssl-dev libwebsockets-dev`）。
- 已 vendored：`third_party/quickjs-ng`（JS 后端）、`third_party/lua`（Lua 5.4 后端）、`third_party/wuffs`、
  `third_party/imgui`（master；**仅 inspector 链接**，游戏进程不引入 UI 依赖）。

## 硬约定（改代码前先读）

1. **语言**：C++23（`CXX_STANDARD 23`）；纯 C 模块用 C11。
2. **输出**：一律 `std::print`（`#include <print>`），**不要 `std::printf`**；stderr 用 `std::print(stderr, ...)`；`FILE*` 场景用 `std::print(f, ...)`。
3. **跨模块接口**：只暴露 **C ABI**（`extern "C"` + 版本化 vtable，见 `docs/02`）。
   禁跨边界异常/RTTI/STL；**分配者释放**；引用用**句柄**（`{index,generation}`）而非裸指针。
4. **无全局可变状态**（多实例，不同实例各持一份）。
5. **确定性**：模拟单线程、固定步长、引擎 RNG、帧内不发异步、**脚本 cycle GC 仅帧边界**（`docs/10/14`）。
6. **不做无关注释**；不改与本任务无关的文件。
7. **构建产物不入库，但生成物入库**：
   - 不入库：`build/`、`third_party/wgpu-native/lib/*`。
   - **入库**：`modules/core/data/generated/`（生成的头/绑定/描述符/JSON/**API.md**/**TYPES.md**）
     —— 生成的公开接口是交付物，改 `schema.yaml` 后必须重新生成并提交（`ctest -R codegen_drift` 会抓漂移）。

## 架构核心

- **声明 + 生成（单一真相 = `modules/core/data/schema.yaml`）**——**引擎的一切公开接口只声明一次**
  （`docs/21` §1；手写公开接口 = 漂移之源）：
  | DSL 段 | 声明内容 | 生成产物 | 编译目标 |
  |--------|----------|----------|----------|
  | `class_list` | 数据类型（struct/enum） | `maque_types.h` + `maque_desc.*`（含 `init_defaults` 默认构造器）+ `maque_schema.json` | `maque::schema` |
  | `api_list` | 引擎函数接口 | `maque_api.h`（声明）+ `maque_bindings.cpp`（**JS 绑定**） | `maque::api` / `maque::genbind` |
  | `interface` | 插件接口 | `maque_ifaces.h`（C vtable + 名称/版本宏） | 各模块 |
  | `control_list` | 控制面方法（AI 面，docs/11 §3） | `maque_control.h`（处理器声明）+ `maque_control_gen.cpp`（注册表 + 类型化 MCP `inputSchema`） | `maque::gencontrol` |
  | `callbacks` | 脚本事件回调（docs/14/19） | `maque_events.h` + `maque_events_gen.cpp`（回调枚举 + `on:` 输入映射 + 精确派发） | `maque::genevents` |
  | `module_abi` | 模块 C/C++ ABI（跨模块公开面，**20 个模块全覆盖**） | `abi/<module>.h`（纯 C 头 + `MQ_<MOD>_ABI_VERSION/IFACE_NAME` 宏；`language: cxx` + `namespace:` 时生成 C++ 命名空间声明） | 各模块（原手写头退化为 `#include` shim） |
  - 生成物在 `modules/core/data/generated/`（**源码树、随仓库提交**；构建配置期自动生成）；
    **改接口一律改 `schema.yaml`，不要改生成物或 shim 头**。人读文档：`generated/API.md`、`generated/TYPES.md`。
  - **MANUAL**：`api_list` 项可带 `manual: "<header>"` → 生成器只 `#include` 该头（实现内联于头，不重复声明）。
  - 加控制面方法 = 在 `control_list` 加一条 + 实现 `mq_control_<Domain>_<method>`（签名见生成头）。
  - 加模块 C ABI 函数 = 在 `module_abi.fns` 加一条；结构体/枚举/宏同理（`structs`/`enums`/`macros`）。
  - ABI 由静态断言守卫（如 `modules/world/physics/tests/physics_test.cpp` 的尺寸/偏移断言），漂移即编译失败。
  - `module_abi` 支持 `opaque/typedefs/consts/macros(值/`value_cxx`/`raw`)/code/structs/enums/fns`；
    迁移工具：`tools/hdr2abi.py`（头 → DSL）、`tools/migrate_abi.py`（转换 + 合并 schema.yaml）。
    **改接口只改 schema.yaml，不要改 shim 头或生成物**。
- **反射中枢**：`mq::reflect`（id/名/路径查询、标量读写、visitor）；持久化 `maque::persist` 反射驱动。
- **运行时（浏览器）**：`modules/runtime` 的 `mq_run_scene(path)`：加载 `.scn` → 反序列化 → 加载 `.js` → 固定步长循环 → WebGPU 渲染 → 退出。
  宿主 `samples/player` 仅一行；**游戏逻辑全在资源**（`scenes/*.scn`、`scenes/*.js`）。
- **渲染**：`maque::gfx`（wgpu-native）。注意：该构建的 `wgpuInstanceWaitAny` **未实现**，异步统一用 `wgpuInstanceProcessEvents` 轮询。
- **实体/组件**：runtime 内实体表（`maque::handle`）+ 组件；**渲染从组件提取**（只读）。

## 脚本可见 API（docs/18）

`api_list` 中的 `host` 命名空间即脚本全局函数（`global: true` → 无前缀）。本轮按参考实现的惯例补齐（对照见 docs/18）：

- **Node**：`node_set_parent/parent/child_count/child_at/find_child/find_path/set_name`、
  `node_set_position/position_x/position_y`、`node_set_scale/scale`、`node_set_angle/angle`、
  `node_set_opacity/opacity`、`node_set_z_index/z_index`、`node_width/height/set_content_size/set_anchor`、
  `node_set_active/active`、`node_destroy`（帧末生效）、`node_valid`
- **Component**：`node_has_component/add_component/remove_component/component_count/component_at/child_with_component`、
  `node_get_number/set_number/get_bool/set_bool/get_string/set_string`（组件字段名可用 `Type.field` 或短名）
- **事件**：`node_on(id, event, handler)` / `node_off` / `node_emit` / `node_emit_number`；emit 沿父链冒泡，
  处理器是脚本里的全局函数名（引擎按名字回调）
- **UI 值**：`ui_set_value/ui_get_value`（ProgressBar/Slider）、`ui_set_checked/ui_get_checked`（CheckBox）
- **Director**：`director_pause/resume/paused/frame/load_scene`
- **动作/定时**：`tween_start(id, "scn.Sprite.radius", to, duration, easing)`、`tween_stop/count/progress`、
  `schedule_once/schedule/unschedule/schedule_count`

**多脚本 / 跨语言（事件总线，docs/14 §多 VM）**：`scn.Scene.scripts: ["game.js", "ai.lua"]` →
每个脚本一个 VM，由**事件总线**统一投递：**顺序 = 声明顺序**、**只投给订阅者**、**载荷只有标量/字符串**、
**错误隔离**（`mq_script_bus_error_count` 增量判定 → 写崩溃上下文）、GC 逐 VM 在帧边界。
事件按**处理器名**路由（`node_on(id,"evt","on_evt")` + `node_emit_number(id,"evt",v)`）→ 语言无关，
JS 注册的处理器可由 Lua 触发，反之亦然。宿主/工具用控制面 **`Scene.emit`**（变更类方法，只读会话拒绝）。
样例：`scenes/multilang/`（JS 玩法 + Lua AI）；测试 `ctest -R multilang`。
**热重载（按文件、原子，docs/14 §热重载）**：`MAQUE_WATCH=1` 监视场景 + **全部脚本**；
只重建**改动的那一个** VM（其它脚本状态保留）、**槽位不变**（投递顺序不变）、
新 VM eval **成功才换入**（失败保留旧 VM → 热更出错不致命），换入后只对新 VM 派发一次 `on_start`
（重新注册事件处理器）。测试 `ctest -R hot_reload`。
⚠️ 长跑进程的日志在重定向到文件时是**全缓冲**：测试里要 `stdbuf -oL`，否则会误判"脚本没生效"。

**脚本后端（`.js` / `.lua`）**：语言按脚本扩展名选择（`"script": "game.lua"` → Lua 后端，
docs/14）。两后端**同一套 API 面与回调**（DSL 生成绑定；`math.clamp` 等在 Lua 侧合并进 `math` 标准库）。
Lua 为沙箱运行（无 `io/os/package/debug/require/collectgarbage`），`math.random` 由引擎 RNG 接管，
GC 仅在帧边界推进（`mq_script_frame_boundary`）。加后端见 docs/14「脚本后端」一节。

**文本**：`ui_set_text/ui_get_text/ui_text_width/ui_text_height/ui_text_lines`（对 `scn.Label` /
`ui.TextLabel` / `render.RichText` 通用，按 role 定位）。排版字段：`font_size` / `align`（或 `align_h`）/
`wrap`（或 `wrap_mode`）/ `max_width` / `line_spacing`；实测结果写回 transient 字段
（`text_w`/`text_h`/`line_count` 或 `text_size`/`content_size`）。
富文本标记：`<color=#RRGGBB>`、`<size=NN>`、`<b>`、`<br/>`；`parse_tags=false` 时按纯文本渲染。
**字体解析顺序**：`MAQUE_FONT=<ttf>` → **`scenes/fonts/` 下首个 `.ttf`（仓库自带，含中文）** →
常见系统字体 → 内置 5x7 位图字体（`tools/font_gen.py` 生成）。组件级覆盖：`scn.Label` /
`ui.TextLabel` / `render.RichText` 的 `font` 字段（相对工作目录或场景目录解析）。
**字形图集按需增长**（任意 Unicode/CJK）：ASCII 预热 95 字形，其余码点排版时惰性栅格化并打包；
`mq_font_atlas_generation` 变化 → 宿主刷新纹理（**旧纹理延迟到下一帧释放**，避免帧内悬空）；
排版按 **UTF-8 码点**（非字节），CJK 逐字可断行；文本图元**两阶段**提交：先全部排版（预热图集），
再按**最终图集尺寸**算 UV（否则先排的标签会用过期 UV 采样 → 错位/重叠）。

**文本 pixel-perfect（FreeType 集成）**：排版在**设备像素**空间进行 ——
`mq_text_style.device_scale`（= 窗口/目标缩放）决定实际栅格化字号（`round(font_size × scale)`，整数 ppem →
笔画落在像素网格），`pixel_snap=1` 把笔位置与字形位图左上角吸附到整数像素；
`mq_text_glyph` 输出位图矩形（`x0/y0` + `w/h`，已按 FreeType `bitmap_left/bitmap_top` 定位）。
渲染侧固定用 **矩形图元 + `filter=nearest` + `snap=1`**（方形会拉伸字形，线性会糊）。
硬证据：`ctest -R text_pixel` —— 渲染结果与字体模块位图**逐像素一致**（渗出 0、max_diff 0），
且内置"线性 + 半像素偏移"对照组证明测试有鉴别力。

**画布 / 分辨率策略（`ui.Canvas`）**：UI 按**设计分辨率**排版，运行时适配任意窗口与显示器。
`fit_mode`（None/FitWidth/FitHeight/ShowAll/NoBorder/Exact）+ `align` + `min/max_scale` + 安全区；
解析结果写回 transient（`scale/scale_x/scale_y/offset_x/offset_y/viewport_*/window_*/device_scale`）。
呈现、输入映射、文字栅格化**共用同一份映射**（错一处就错位）；策略数学在 `modules/ui/canvas`（纯函数 + 单测）。
脚本：`screen_w/h()`=设计分辨率，`window_w/h()`=设备像素，`canvas_scale()/canvas_offset_x/y()/canvas_device_scale()`。
测试/CI：`MAQUE_DPI_SCALE=2` 模拟 HiDPI，`MAQUE_SAFE_AREA="l,t,r,b"` 模拟刘海；`ctest -R 'canvas'` 覆盖。

**坐标约定**：节点位置、**输入事件、脚本可见坐标统一为世界坐标（y 向上）**；
设备事件在平台边界做一次"窗口 → 世界"转换（等比缩放 + 居中留边 + y 翻转），注入/模拟事件按世界坐标给出。
渲染时再翻到屏幕坐标（`y_screen = H - y_world`）。UI 控件（`ui.Control`）以**中心**为锚点。
文本排版坐标 y 向下，屏幕映射为 `y_screen = H - (node_y + label_y) + glyph_y`。

## 常见改动怎么做

- **加一个数据类型/组件**：编辑 `modules/core/data/schema.yaml` 的 `class_list` → 重构建（自动重生成）。
- **加一个引擎接口**：在 `api_list` 加 `fn/args/returns`（标量/string）；实现放 `modules/api/src/api.cpp`；
  要内联实现则用 `manual: "<header>"` 并把头放到 `modules/api/include/`。
- **加一个模块**：`modules/<area>/<name>/`，含 `include/`、`src/`、`tests/`、`CMakeLists.txt`；
  在根 `CMakeLists.txt` `add_subdirectory`；导出 `maque::<name>` 静态库 + `add_test`。
- **脚本里调用引擎**：用生成绑定（如 `math.clamp(...)`）；宿主 `log/cfg/draw_sprite/entity_*` 由 runtime 注册。

## 测试与代码风格

- 每个模块自带**独立单测**（`tests/`），用简易 `CHECK` 宏，经 `add_test` 挂到 CTest。
- 格式/静态检查：`.clang-format`、`.clang-tidy`（导出 `compile_commands.json`）。
- 经典用法：先加/改接口与测试，再实现。

## 文档地图

- 设计文档：`docs/`（根文档 `docs/00-设计哲学与约束.md`；索引 `docs/README.md`）。
- 接口绑定机制：`docs/21-接口声明与绑定生成.md`；**API 参考（自动生成）**：`docs/api/README.md` → `modules/core/data/generated/API.md`。
- 运行时/资源模型：`docs/20-运行时与资源模型.md`。
- 里程碑/路线图：`docs/milestones/00-路线图.md`。
- 各模块开发计划：`plans/`（`plans/README.md`）。
