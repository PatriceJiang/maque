# 010 · WGPU 直呈下 ImGui 鼠标位置映射错误（点击/拖动整体偏移）

- **类型**：正确性（调试面板交互）
- **状态**：fixed
- **影响面**：窗口 + WGPU 直呈（`MAQUE_PRESENT=wgpu`，或拿不到 SDL_Renderer 的网格链回退）
  下的全部 ImGui 交互：stats 拖不动、F4 控件点不中、hover/滚轮位置错。SDL 官方后端不受影响。
- **发现版本**：`95c2b00e`（`fix(imgui): 修正直呈裁剪并补齐面板字形`）之后；
  该提交把直呈 ImGui 改成屏幕空间绘制，但 runtime 的指针换算没跟着改。

## 症状

修复直呈面板绘制（[006](006-imgui-present-clipping-font-glyph.md)）后，面板**看起来正确**，
但鼠标点不中：

- stats 面板拖不动（在标题栏按下并向左/上拖，面板反而往相反方向跑，或完全不动）；
- F4「引擎选项」面板里的复选框/下拉点不中，或点到的位置与光标相差一段距离；
- 偏移量随窗口/场景比例变化：320×240 场景放进 800×650 窗口（游戏 zoom=2.5）时，
  点窗口 `(x,y)` 实际命中的是 `(x/2.5, (y-25)/2.5)` —— F4 面板（显示空间 x≈256..568）
  对应窗口 x≈640..1420，**整块面板落在可点范围之外**，等于完全不可交互。

SDL 官方后端（`MAQUE_PRESENT=sdl`）是正常对照。

## 最小复现

Windows / Git Bash（PowerShell 等价）；`samples/spine/spine.scn` 设计分辨率 320×240，
窗口 800×650，游戏视图 zoom=2.5：

```bash
# A. 直呈 + 合成指针拖动 stats 标题栏（向下 220px），抓第 8 帧
MAQUE_WINDOW=800x650 MAQUE_IMGUI=1 MAQUE_PRESENT=wgpu \
MAQUE_CAPTURE_WGPU=.kimi-tmp/bug010/wgpu_drag MAQUE_CAPTURE_FRAMES=8 MAQUE_MAX_FRAMES=12 \
MAQUE_IMGUI_POINTER="2:20:18:1;3:20:130:0;4:20:238:0;5:20:238:2" \
./build-win/samples/player/maque_player.exe samples/spine/spine.scn

# B. 同一命令的 SDL 参考（把 MAQUE_CAPTURE_WGPU 换成 MAQUE_CAPTURE=$PWD/.../sdl_drag）
```

修复前实测（同一场景/帧号，与"不拖动"基线比较整帧差异）：

| 通道 | 拖动后差异像素 | 差异行范围 | 结论 |
|---|---|---|---|
| WGPU 直呈 | **6418** | 8..72 | 只有悬停高亮变化 → 面板没动 |
| SDL 官方后端 | **70454** | 8..377 | 面板下移 220px（正确） |

## 根因

同一份坐标被**两套映射**解释：

1. `95c2b00e` 之后，直呈网格链的 ImGui 顶点由 gfx **恒等视图**绘制 —— 顶点坐标就是
   渲染目标像素（`MQ_IMGUI_LAYER` 分支），面板彻底是**屏幕空间覆盖层**；
2. 但 runtime 的 `ptr_target_x/y()` 仍按**画布反算**：
   `(窗口逻辑像素 - Canvas.offset) / Canvas.scale`，即把指针折算成"设计坐标"。

`Canvas.scale` 是设计分辨率 → 窗口的等比缩放（示例中 2.5），于是指针与面板之间差
一个 `1/scale` 倍率与 `offset/scale` 平移。`95c2b00e` 之前两者**同时**被画布变换，
误差互相抵消，所以旧实现看不出问题；只改渲染不改输入就把误差暴露出来。

## 修复

`modules/runtime/src/ui/debug_panel.cpp`：指针换算改为按 **ImGui 显示空间**算，与绘制同源：

- 新增 `imgui_show_size()`：显示空间尺寸 —— 官方后端 = 绘制区设备像素（`drawable_size`），
  网格链 = 渲染目标像素 `TW×TH`（与 `mq_imgui_begin_frame(show_w, show_h, ...)` 同一份值，
  `imgui_frame_begin` 直接复用它，去掉重复计算）；
- 新增 `imgui_show_scale()`：`显示空间宽 / 窗口逻辑宽`（无窗口 = 1；headless/`--shot`
  的指针本来就是目标像素）；
- `imgui_input_feed()` 用 `pointer * imgui_show_scale()` 喂 ImGui；
- 删除 `ptr_target_x/y()` 与 `imgui_input_scale`（画布反算路径不再存在）。

指针与顶点现在共用同一个空间定义：**窗口逻辑像素 ×(显示/逻辑) = 显示空间**，
显示空间又被逐像素画到目标上（直呈再按 surface/target 比例上屏）。

## 回归与证据

- `tests/imgui_present_e2e.sh` 新增第 ⑦ 段：WGPU 直呈 + 合成指针拖动 stats，抓第 8 帧，
  与"无 ImGui"同帧基线比较 —— 旧面板区差异必须≈0（`<100`）、新面板区差异必须 `>40%`。
  2026-11-02 Windows 实测：旧区 **0**、新区 **32071/32200**；
- 同一拖动场景整帧差异：修复前 6418（行 8..72）→ 修复后 70471 / 70464（两次实测，行 8..377），
  与 SDL 参考 70454 基本一致；直呈与 SDL 的终点帧差异 586 px（= 无拖动时的静态基线差，
  属字体栅格化/统计数字差异，非位置差）；
- F4 控件点击坐标扫描（`MAQUE_IMGUI_POINTER` 在窗口 y=50..350 每 10px 点一次）：
  修复后直呈与 SDL 的 `engine option` 切换序列**逐条一致**（`3d=off/on/off`、`2d=off/on`）；
- 以下测试通过：`ctest --test-dir build-win -R '^imgui$'`、`-R '^imgui_toggle_e2e$'`、
  `-R '^imgui_present_e2e$'`。
  同目录的 `imgui_engine_options_e2e` ⑤b（`vram_bytes` 期望 1584、实际 2608）与本问题无关：
  该子用例不打开 ImGui（`imgui_on` 为假时本修复的代码路径完全不执行），且 2026-10-30 的旧构建
  对同一场景也报 2608，属既有偏差，未在本次改动范围内处理。

## 相关

- [006](006-imgui-present-clipping-font-glyph.md)（引入本问题的直呈渲染修复）
- [28 · ImGui 调试面板](../28-ImGui调试面板.md)
- `modules/runtime/src/ui/debug_panel.cpp`（`imgui_show_size` / `imgui_show_scale` / `imgui_input_feed`）
- `modules/render/gfx/src/gfx.cpp`（`MQ_IMGUI_LAYER` 恒等视图分支）
- `tests/imgui_present_e2e.sh`（第 ⑥/⑦ 段）
