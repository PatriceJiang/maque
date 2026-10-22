# 28 · ImGui 调试面板（引擎侧）

定位：**开发工具**，不是游戏 UI。游戏内 HUD 仍用声明式 `ui.*`（进场景/快照/可回放）；
ImGui 面板是视图层 —— 不进快照、不进校验和、不参与模拟。

## 能做什么

- **默认 stats 面板**（左上角，所有窗口程序都有）：FPS、逻辑耗时、渲染耗时、
  drawcall、三角形、帧号、渲染目标尺寸。
- **脚本可用的控件 API**：`imgui_begin/end`、文本、按钮、复选框、单选、滑块、拖动、
  输入框、下拉、列表、树、折叠头、标签页、表格、菜单、进度条、颜色、tooltip、
  plot（引擎侧环形缓冲）、内建演示窗口（`imgui_show_demo`）等 **74 个函数**
  （声明在 `schema/runtime/host.yaml`，实现见 `modules/runtime/src/runtime.cpp` 的
  `namespace host`）。
- **中文**：复用 `scenes/fonts/` 的 TTF，图集一次烘两档字号（26px 主字体 + 15px 小字体）。
- **F2 开关**：引擎级热键（沿检测，按住不会连续切换）；`MAQUE_IMGUI=1/0` 可强制开关。

## 结构

| 层 | 内容 |
|---|---|
| `modules/ui/imgui` | ImGui 上下文/帧/控件 C ABI（`mq_imgui.h`、`mq_imgui_widgets.h`）、字体图集、官方 SDL_Renderer 后端（实验） |
| `modules/runtime` | 生命周期、帧时序、输入转发与隔离、stats 计量、脚本绑定实现 |
| `schema/runtime/host.yaml` | 74 个 `imgui_*` 脚本函数声明（codegen 生成绑定） |

## 帧时序

1. 引擎采样输入 → `imgui_input_feed`（窗口像素）
2. `imgui_frame_begin`：初始化（惰性）、上传字体图集、`ImGui::NewFrame`、画 **stats**
3. 脚本 `update()`：脚本画自己的面板
4. 引擎渲染所有图层（含 ui pass）
5. `imgui_extract`：ImGui draw data → 引擎 mesh 图元（`layer = MQ_IMGUI_LAYER = 100`）
6. present → 窗口

数值上"面板在所有引擎图层之上"，因为 mgui mesh 的 layer 高于引擎 UI 的 layer=1。

## 已知问题（重要）

1. **面板不可拖动**：覆盖层窗口当前用 `NoMove|NoResize`。
   原因：自研"draw data → mesh 图元"链在拖动时会出现**残影/内容不跟随**
   （实际现象：拖动后旧内容留在原处、阶梯状三角形）。
2. **官方 SDL_Renderer 后端路径画不出面板**：`MAQUE_PRESENT=accelerated` 时后端初始化成功、
   无断言，但面板不出现在窗口（已加"面板绘制后再 present"仍无效，根因待查）。
   因此**默认呈现保持 software**，面板走自研网格链（可见、可截图验证）。
3. **面板不参与 `--shot` 之外的抓图**：`--shot` 截的是引擎像素缓冲（含面板）；
   `MAQUE_CAPTURE` 抓窗口表面（software 模式下含面板）。

## 使用

```powershell
# 默认启动就有 stats（窗口模式）
./build-win/samples/player/maque_player.exe samples/spine/spine.scn

# 强制开/关
MAQUE_IMGUI=1 ./build-win/samples/player/maque_player.exe <scene>
MAQUE_IMGUI=0 ./build-win/samples/player/maque_player.exe <scene>

# 字号（设计像素）
MAQUE_IMGUI_FONT_SIZE=32 ./build-win/samples/player/maque_player.exe <scene>
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
- `ctest -R shot_e2e`：`--shot` 截图路径。

## 现状快照（2026-10-22）

记录当时的实现程度与实测结论，便于后续接手。基线提交见 `git log`（本文件所在提交附近）。

### 已可用（有实测证据）

| 项 | 状态 | 证据 |
|---|---|---|
| `modules/ui/imgui` 模块（上下文/帧/控件 C ABI） | ✅ | `ctest -R imgui`（34 项断言：图集非空、命令几何、坐标映射、参数校验） |
| 74 个 `imgui_*` 脚本函数 | ✅ | schema → codegen → runtime 实现一致；`bindings_e2e` 通过 |
| 中文字体（TTF + 两档字号 26px/15px） | ✅ | `--shot` 截图里中文正常、stats 用小字号 |
| 默认 stats 面板（左上角） | ✅ | `--shot` 截图：FPS / logic / render / draws / tris / frame / target 七项 |
| F2 开关（沿检测） | ✅ | `imgui_toggle_e2e`（单次/按住/两次/反复开关/像素可见） |
| 输入隔离（面板吃输入时清空本帧设备事件） | ⚠️ 已实现未实测 | 代码路径 + `imgui_want_mouse/keyboard` |
| 绘制在引擎所有图层之上 | ✅ | 面板网格 layer=100（引擎 UI = 1） |

### 不可用 / 有限制（全部为实测）

| 项 | 现象 | 定位 |
|---|---|---|
| **面板拖动** | 拖动后内容不跟随/留残影（阶梯状三角形） | 自研"draw data → mesh 图元"链；覆盖层暂用 `NoMove\|NoResize` 禁用拖动 |
| **加速呈现 + 官方 SDL_Renderer 后端** | 后端初始化成功、无断言，但面板**不出现在窗口**；加"面板绘制后再 present"仍无效 | 双重 present/renderer 状态冲突，根因未定位 |
| **默认呈现** | 为规避上一条，Windows 默认仍是 `software`（面板走网格链，可见但不可拖） | platform `present_mode` 默认值 |
| `PushFont(size)` 改字号 | 静态图集只烘两档；推其它尺寸会取不到字形 → 空白 | 已改为"图集内两档 + push 字体指针" |
| 弹窗/样式/拖放/剪贴板/IME | 未实现 | 见待办 |

### 关键结论（避免重复踩坑）

1. ImGui 1.92+ 的**动态图集**要求后端实现完整的 `WantCreate/WantUpdates` 状态机；
   本集成改用**静态图集**（`Build()` + `GetTexDataAsRGBA32()` + `SetTexID`），
   但因此**不能**再启用官方动态后端（否则命中
   `Build() before RendererHasTextures` 断言）。
2. 自研网格链的索引必须是**命令内 0 基**；gfx 绘制时会再加 `baseVertex`，
   宿主**不能**自己再加偏移（否则三角形拿到别的图元顶点 → 画面糊成一片）。
3. ImGui 显示空间：网格链模式 = **设计/目标空间**（脚本按 `screen_w()` 定位）；
   官方后端模式 = **窗口像素空间**（两者不可混用）。
4. `ImGuiWindowFlags_AlwaysAutoResize` + 每帧 `SetNextWindowPos` + 可拖动
   → "窗口动了、文字不跟"；要么固定位置，要么只首帧定位一次。
5. 官方后端必须在**第一帧之前**初始化（renderer 提前创建），否则先走静态图集再切后端会崩。

### 下一步（按优先级）

1. **统一呈现**：游戏帧与面板画到同一 renderer，只 `present` 一次
   → 同时解决"加速路径面板不可见"与"网格链拖动残影"，然后恢复面板可拖动。
2. 面板可拖动后，补 e2e：拖动/缩放后像素仍正确。
3. 样式 API（`push_style_color/var`）、弹窗、拖放、剪贴板/IME。
4. `MAQUE_STATS=0` 开关 stats 面板。

## 待办

1. 修"面板可拖动"：统一呈现结构（渲染游戏帧与面板到同一 renderer 后只 present 一次），
   然后恢复覆盖层可拖动、并让 accelerated 路径也能显示面板。
2. 多字号/主题样式 API（`push_style_color/var`）、弹窗（popup/modal）、拖放、剪贴板/IME。
3. 把 stats 面板做成可选开关（`MAQUE_STATS=0`）。
