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
- WebGPU 需 `third_party/wgpu-native/lib/libwgpu_native.so`（二进制**不入库**）。
- 系统依赖：SDL2 / libcurl / libwebsockets（`libsdl2-dev libcurl4-openssl-dev libwebsockets-dev`）。
- 已 vendored：`third_party/quickjs-ng`（JS 后端）、`third_party/lua`（Lua 5.4 后端）、`third_party/wuffs`。

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

**脚本后端（`.js` / `.lua`）**：语言按脚本扩展名选择（`"script": "game.lua"` → Lua 后端，
docs/14）。两后端**同一套 API 面与回调**（DSL 生成绑定；`math.clamp` 等在 Lua 侧合并进 `math` 标准库）。
Lua 为沙箱运行（无 `io/os/package/debug/require/collectgarbage`），`math.random` 由引擎 RNG 接管，
GC 仅在帧边界推进（`mq_script_frame_boundary`）。加后端见 docs/14「脚本后端」一节。

**文本**：`ui_set_text/ui_get_text/ui_text_width/ui_text_height/ui_text_lines`（对 `scn.Label` /
`ui.TextLabel` / `render.RichText` 通用，按 role 定位）。排版字段：`font_size` / `align`（或 `align_h`）/
`wrap`（或 `wrap_mode`）/ `max_width` / `line_spacing`；实测结果写回 transient 字段
（`text_w`/`text_h`/`line_count` 或 `text_size`/`content_size`）。
富文本标记：`<color=#RRGGBB>`、`<size=NN>`、`<b>`、`<br/>`；`parse_tags=false` 时按纯文本渲染。
字体内置回退：`MAQUE_FONT=<ttf>` 指定字体 → 常见系统字体 → 内置 5x7 位图字体（`tools/font_gen.py` 生成）。

**文本 pixel-perfect（FreeType 集成）**：排版在**设备像素**空间进行 ——
`mq_text_style.device_scale`（= 窗口/目标缩放）决定实际栅格化字号（`round(font_size × scale)`，整数 ppem →
笔画落在像素网格），`pixel_snap=1` 把笔位置与字形位图左上角吸附到整数像素；
`mq_text_glyph` 输出位图矩形（`x0/y0` + `w/h`，已按 FreeType `bitmap_left/bitmap_top` 定位）。
渲染侧固定用 **矩形图元 + `filter=nearest` + `snap=1`**（方形会拉伸字形，线性会糊）。
硬证据：`ctest -R text_pixel` —— 渲染结果与字体模块位图**逐像素一致**（渗出 0、max_diff 0），
且内置"线性 + 半像素偏移"对照组证明测试有鉴别力。

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
