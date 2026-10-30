# 28 · ImGui 调试面板（引擎侧）

定位：**开发工具**，不是游戏 UI。游戏内 HUD 仍用声明式 `ui.*`（进场景/快照/可回放）；
ImGui 面板是视图层 —— 不进快照、不进校验和、不参与模拟。

## 能做什么

- **stats 面板**（左上角；**默认不显示**，按 F2 或 `MAQUE_IMGUI=1` 打开）：FPS、逻辑耗时、渲染耗时、
  drawcall、三角形、帧号、渲染目标尺寸。**可拖动、可缩放**（标题栏拖动，边框缩放）。
  面板的**字号与尺寸 = 屏幕分辨率派生的固定值**（见下"固定 UI 度量"）：
  同一块屏上不随游戏窗口/场景分辨率变化，换屏/换 DPI 缩放才变。
- **脚本可用的控件 API**：`imgui_begin/end`、文本、按钮、复选框、单选、滑块、拖动、
  输入框、下拉、列表、树、折叠头、标签页、表格、菜单、进度条、颜色、tooltip、
  plot（引擎侧环形缓冲）、内建演示窗口（`imgui_show_demo`）等 **74 个函数**
  （声明在 `schema/runtime/host.yaml`，实现见 `modules/runtime/src/runtime.cpp` 的
  `namespace host`）。
- **中文**：复用 `scenes/fonts/` 的 TTF，图集一次烘两档字号（标题 + 正文，默认 20/15 逻辑像素）。
- **F2 开关**：引擎级热键（沿检测，按住不会连续切换）；`MAQUE_IMGUI=1/0` 设定**初始**开关。
- **F4 引擎选项面板**（默认收起，按 F4 弹出/收起；首次按 F4 会自动打开 ImGui）：运行期热调
  **视图层**渲染选项，**不写场景组件、不进快照/校验和**，只在成帧时生效（切场景后仍保留）：
  - **渲染模式**：`rendered`（完整材质）/ `textured`（base-color 贴图，无光照）/ `solid` / `wireframe`；
  - **游戏图层**：独立显示/隐藏 3D 与 2D/UI，F4 面板自身不受影响；
  - **MSAA**：跟随场景 / 关闭 / 4x（优先级：面板 > `MAQUE_MSAA` > `scn.Scene.msaa_samples`）；
  - **默认太阳光**：场景**没有任何光源**时补的那盏方向光的开关 / 强度 / 传播方向（老场景兜底行为可调）；
  - **主相机 near / far**：覆盖场景相机的近远裁剪面（覆盖关闭时字段自动跟随场景实际值，勾选后不跳变）。
  启动初值用 `MAQUE_ENGINE_OPTIONS`（CI/截图/演示无需鼠标，见下"使用"）。
- **F2 / F4 互不干扰**：
  · F4 只开关「引擎选项」面板 —— ImGui 本来关着时顺手打开它、再按 F4 收起面板时恢复关闭
    （不会只关面板却把 stats 留在屏幕上）；ImGui 已开（F2 / `MAQUE_IMGUI=1` / 脚本）时
    F4 只开关面板、**不动** stats 的开关。
  · F2 关闭整个 ImGui 时会一并收起选项面板状态 → **F2 再开不会"复活"F4 面板**（干净地从 stats 开始）。
  · **两个热键不双触发**：开关面板后 F2/F4 事件从本帧输入里消费掉，不再下发给脚本/玩法
    （否则"按 F2 开面板"会同时触发游戏自己的 F2 动作）；普通按键与其余功能键不受影响。
  · 回归：`ctest -R imgui_engine_options_e2e` 的 ⑥（状态日志 + 右侧区域像素对照）与 ⑦（按键路由）。

### 固定 UI 度量（屏幕分辨率 → 字号/面板大小）

| 量 | 规则 | 默认（1080p 逻辑屏） |
|---|---|---|
| UI 缩放 `ui_scale` | `clamp(屏幕逻辑高 / 1080, 0.7, 2.5)`；`MAQUE_IMGUI_SCALE` 覆盖 | 1080p→1.0；1707x1067（2560x1600@150%）→0.99 |
| 正文字号 | 15 逻辑像素 × ui_scale；`MAQUE_IMGUI_FONT_SIZE` 覆盖 | 15 |
| 标题字号 | 正文 × 4/3（只大一档，不再 26px:15px 压过内容） | 20 |
| stats 面板 | 固定 正文×16 × 正文×10（显示空间像素），窗口放不下时才缩到 85%/75% | 237×148 |

"逻辑像素"= 应用可见的桌面坐标（已含 OS DPI 缩放；`mq_platform_screen_size` 读
`SDL_GetDisplayBounds`）。所以 4K/1080p/HiDPI 上视觉大小一致，**窗口大小不参与**。

## 结构

| 层 | 内容 |
|---|---|
| `modules/ui/imgui` | ImGui 上下文/帧/控件 C ABI（`mq_imgui.h`、`mq_imgui_widgets.h`）、字体图集、官方 SDL_Renderer 后端 |
| `modules/runtime` | 生命周期、帧时序、输入转发与隔离、stats 计量、F4 引擎选项面板 |
| `modules/platform` | **唯一呈现通道 = SDL_Renderer**：游戏帧 → 后台缓冲 →（ImGui pass）→ 单次 present |
| `schema/runtime/host.yaml` | 74 个 `imgui_*` 脚本函数声明（codegen 生成绑定） |

## 帧时序（窗口模式）

1. 引擎采样输入 → `imgui_input_feed`（窗口像素坐标）
2. `imgui_frame_begin`：初始化（惰性）、`ImGui::NewFrame`、画 **stats** 与 **F4 引擎选项面板**
   （两者都是普通 ImGui 窗口；选项面板可交互，stats 常驻显示计量）
3. 脚本 `update()`：脚本画自己的面板（这一阶段 ImGui 只是**录制 draw list**，不渲染）
4. 引擎渲染所有图层到 CPU 像素（wgpu 离屏回读）
5. **呈现（统一 renderer，只 present 一次）**：
   1. `mq_platform_draw_rect`：引擎帧 → 流式纹理 → `SDL_Renderer` 后台缓冲；
   2. `mq_imgui_backend_render`：ImGui 官方后端在**同一后台缓冲**上再画一趟（单独的 render pass）；
   3. `mq_platform_present_ui`：抓帧（如需）+ `SDL_RenderPresent` → 游戏画面与面板一起上屏。
6. 无窗口 / `MAQUE_PRESENT=off` → 自研"draw data → mesh 图元"链把面板写进引擎像素缓冲
   （网格 layer = `MQ_IMGUI_LAYER = 100`，在引擎 UI 的 layer=1 之上）→ `--shot` 里也有面板。

## 已修复的关键问题（勿回退）

1. **「imgui 一开，游戏画面不上屏」**：根因是**呈现通道混用** ——
   游戏帧走 `SDL_GetWindowSurface + SDL_BlitSurface`，面板却走另一个 `SDL_Renderer`
   （为官方后端创建）。SDL2 明确规定同一窗口不能混用 window surface 与 renderer：
   先写进窗口表面的游戏帧被随后的 `SDL_RenderPresent` 覆盖 → 屏幕上只剩面板/黑屏。
   修复：**所有输出统一走同一个 SDL_Renderer**（游戏帧先画、ImGui 再叠、只 present 一次），
   不再调用任何 `SDL_GetWindowSurface` 路径。回归：`ctest -R imgui_present_e2e`。
2. **「stats 拖不动」**：覆盖层当初为规避网格链拖动错位加了 `NoMove|NoResize`。
   现在窗口模式下面板走官方后端（拖动/缩放是官方行为，坐标天然同步）→ 恢复普通窗口。
   网格链模式（headless）没有鼠标交互，不受影响。
3. **ImGui 官方后端初始化时序**：必须**先**拿到 `SDL_Renderer`（`mq_platform_ensure_renderer`）、
   **先声明** `mq_imgui_set_backend_mode(1)`、**再** `mq_imgui_init*()`。顺序反了会命中
   `Build() before RendererHasTextures` 断言。
4. **自研网格链索引必须是命令内 0 基**；gfx 绘制时会再加 `baseVertex`，
   宿主**不能**自己再加偏移（否则三角形拿到别的图元顶点 → 画面糊成一片）。
5. **显示空间两种模式不可混用**：官方后端 = **窗口设备像素空间**（`io.DisplaySize = 绘制区
   设备像素`，鼠标逻辑坐标 × content_scale 换算）；网格链 = **设计/目标空间**
   （脚本按 `screen_w()` 定位）。输入坐标按对应空间喂。
6. **「stats 标题太大 / 有些窗口又偏小」**：旧实现的字体倍率是
   `ps = win_w / TW`（窗口/渲染目标比）→ 窗口大小、场景分辨率、视图数都会改变面板大小。
   修复：字号/样式/面板尺寸改为**屏幕分辨率派生的固定 UI 度量**
   （`imgui_resolve_metrics`：`ui_scale = 屏幕逻辑高/1080`，正文 15、标题 = 正文×4/3，
   面板 = 正文×16 × 正文×10），**窗口大小不再参与**；换屏/换 DPI 缩放才变。
   回归：`imgui_present_e2e` 用两种窗口尺寸断言度量逐字一致。

## 已知限制

| 项 | 说明 |
|---|---|
| `PushFont(size)` 改字号 | 静态图集（网格链模式）只烘两档；官方后端模式走动态图集无此限制 |
| 弹窗/样式/拖放/剪贴板/IME | 未实现 |
| HiDPI 窗口模糊 | SDL 进程未开 DPI 感知时窗口会被系统整体放大（面板坐标与引擎帧同一缩放，非 ImGui 问题；见待办 3） |

## 使用

```powershell
# 默认不显示；窗口模式下按 F2 或 MAQUE_IMGUI=1 打开 stats
./build-win/samples/player/maque_player.exe samples/spine/spine.scn

# 启动时开/关（运行中 F2 仍可切换）
MAQUE_IMGUI=1 ./build-win/samples/player/maque_player.exe <scene>
MAQUE_IMGUI=0 ./build-win/samples/player/maque_player.exe <scene>

# 字号（正文字号，逻辑像素；默认 15，标题自动 = 正文 × 4/3）
MAQUE_IMGUI_FONT_SIZE=15 ./build-win/samples/player/maque_player.exe <scene>

# UI 缩放覆盖（相对 1080p 的倍率；默认 = 屏幕逻辑高/1080，固定不随窗口变）
MAQUE_IMGUI_SCALE=1.5 ./build-win/samples/player/maque_player.exe <scene>

# 呈现后端（默认 Windows accelerated / 其它平台 software；两者都支持面板）
MAQUE_PRESENT=software ./build-win/samples/player/maque_player.exe <scene>

# F4 弹出/收起「引擎选项」面板（渲染模式/图层/MSAA/默认太阳光/主相机 near·far；首次按 F4 会自动开 ImGui）
./build-win/samples/player/maque_player.exe <scene>

# 引擎选项的启动初值（不必用鼠标；逗号分隔，向量分量用 ':'）
#   键：mode=rendered|textured|solid|wireframe、3d=0|1、2d=0|1、msaa=0|1|4、
#       sun=0|1、sun_intensity=<f>、sun_dir=x:y:z、near=<f>、far=<f>
MAQUE_ENGINE_OPTIONS="mode=textured" ./build-win/samples/player/maque_player.exe <scene>
MAQUE_ENGINE_OPTIONS="msaa=4" ./build-win/samples/player/maque_player.exe <scene>
MAQUE_ENGINE_OPTIONS="sun=0" ./build-win/samples/player/maque_player.exe <scene>
MAQUE_ENGINE_OPTIONS="sun_intensity=2.5,sun_dir=0.4:-1:-0.2,near=0.01,far=500" \
    ./build-win/samples/player/maque_player.exe <scene>
```

脚本里画自己的面板：

```js
if (imgui_enabled()) {
    imgui_set_pos(8, 8);
    if (imgui_begin('我的面板')) {
        imgui_text('hello ' + director_frame());
        if (imgui_button('点我')) log('clicked');
        x = imgui_slider_float('x', x, 0, 100);
    }
    imgui_end();       // 必须无条件调用
}
```

## 管线注意

- **环境要求**：`imgui_begin/end` 必须配对；可见性用返回值判断。
- 参数只支持标量/字符串（DSL 限制），`combo`/`list_box` 用逗号分隔串。
- `input_text` 的编辑缓冲由引擎按 label 维护，返回当前文本。
- 输入隔离：ImGui 想吃鼠标/键盘时，runtime 清空本帧设备事件（不点穿到游戏）。

## 测试

- `ctest -R imgui`：模块单测（字体图集非空、命令几何、坐标映射、参数校验）。
- `ctest -R imgui_toggle_e2e`：F2 开关的沿检测（单次/按住/两次/反复开关/像素可见）。
- `ctest -R imgui_engine_options_e2e`：F4 引擎选项面板 —— F4 沿检测与面板像素、MSAA、
  `rendered/textured/solid/wireframe`（`textured` 采样 base-color 贴图且开关太阳光逐像素不变）、
  3D 与 2D/UI 图层开关、默认太阳光、near/far 覆盖、F2/F4 状态隔离及热键路由。
- `ctest -R imgui_present_e2e`：**修复回归** —— 真实窗口抓帧断言
  ① imgui 开启时游戏区域逐像素可见且不被面板扰动；② 合成指针拖动 stats 后
  面板确实换位置、游戏仍可见；③ `MAQUE_PRESENT=software` 回退路径同样可见；
  ④ 800x650 与 1200x900 两种窗口的 `ui scale/title/body/panel` 度量**逐字一致**
  （固定 UI 度量回归）。
  合成指针用 `MAQUE_IMGUI_POINTER="frame:x:y:type[;...]"`（type 0=move 1=down 2=up）
  压进本帧设备事件队列 —— 与真实鼠标同一条路径（`iframe → imgui_input_feed → ImGui`）。
- `ctest -R shot_e2e`：`--shot` 截图路径（网格链模式下面板进截图）。

## 现状快照（2026-10-22，统一呈现 + 固定 UI 度量）

### 已可用（有实测证据）

| 项 | 状态 | 证据 |
|---|---|---|
| 游戏帧 + 面板同屏、只 present 一次 | ✅ | `imgui_present_e2e`（游戏区逐像素一致 + 游戏区颜色 >5000 种）；真实窗口截屏 |
| stats 面板**可拖动/缩放** | ✅ | `imgui_present_e2e`（拖动后旧位置差异 0、新位置差异 >2 万）；真实鼠标拖拽截屏 |
| **UI 度量固定（屏幕分辨率派生）** | ✅ | `imgui_present_e2e`：400x300 / 800x650 / 1200x900 / 2 视图下 `panel=237x148` 等度量一致 |
| `modules/ui/imgui` 模块（上下文/帧/控件 C ABI） | ✅ | `ctest -R imgui` |
| 74 个 `imgui_*` 脚本函数 | ✅ | schema → codegen → runtime 实现一致；`bindings_e2e` 通过 |
| 中文字体（TTF + 标题/正文两档字号，默认 20/15） | ✅ | `--shot` 截图里中文正常、stats 用正文字号 |
| stats 面板（左上角，237x148；F2 / `MAQUE_IMGUI=1` 打开） | ✅ | FPS / logic / render / draws / tris / frame / target 七项 |
| F2 开关（沿检测） | ✅ | `imgui_toggle_e2e` |
| **F4 引擎选项面板**（rendered/textured/solid/wireframe、3D/2D 图层、MSAA、默认太阳光、主相机 near·far，视图层覆盖不改场景） | ✅ | `imgui_engine_options_e2e`（含 textured 纹理采样与无光照像素断言）；`MAQUE_ENGINE_OPTIONS` 初值 |
| `MAQUE_PRESENT=software/accelerated` 都能显示面板 | ✅ | `imgui_present_e2e`（software 抓帧） |
| 输入隔离（面板吃输入时清空本帧设备事件） | ✅ | `imgui_present_e2e` 拖动用合成指针走 `imgui_input_feed` |

## 待办

1. 样式 API（`push_style_color/var`）、弹窗（popup/modal）、拖放、剪贴板/IME。
2. `MAQUE_STATS=0` 开关 stats 面板。
3. HiDPI：SDL 窗口 DPI 感知（`SDL_HINT_WINDOWS_DPI_AWARENESS=permonitorv2`）+ 面板按帧缓冲比例渲染
   （当前窗口被系统放大时窗口与文字会糊；UI 度量本身已按逻辑分辨率算好）。
4. 引擎选项扩展：MSAA 支持 2x/8x（gfx 当前只有 1x/4x 管线）、太阳光颜色、`MAQUE_STATS` 同类脚本/控制面接口。
