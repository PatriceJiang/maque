# 006 · WGPU 直呈 ImGui 裁剪错误与静态图集缺字

- **类型**：正确性（调试面板绘制不完整、固定中文文案缺字）
- **状态**：fixed
- **影响面**：WGPU 自研 ImGui 网格路径；场景设计分辨率与窗口尺寸不一致时的 F4 面板，
  以及静态图集常用字表未覆盖的中文。SDL 官方后端与引擎 `text` 模块不是故障来源。
- **发现版本**：`8bc191b4`；WGPU 直呈打开 F4 面板，或显示包含“渲”的固定文案
- **修复提交**：`95c2b00e`（`fix(imgui): 修正直呈裁剪并补齐面板字形`）

## 症状

同一面板出现过两个可独立复现的症状：

1. 小设计分辨率场景放入较大窗口时，WGPU 直呈下 ImGui 面板被放大、偏移并大面积裁切，
   文字和控件绘制不完整；SDL 官方后端是正常对照。
2. 大面积裁切修复后，F4 面板的“渲染模式”仍显示为“?染模式”。其余文字可见，
   不是整段文本 clip 或纹理上传失败。

## 最小复现

在 Windows 的 Git Bash 中运行修复前版本。

### A. 直呈坐标与裁剪

```bash
MAQUE_WINDOW=800x650 MAQUE_PRESENT=wgpu MAQUE_IMGUI=0 \
MAQUE_SIM_KEY_AT=2:1004 MAQUE_CAPTURE_WGPU=.kimi-tmp/bug006/direct \
MAQUE_CAPTURE_FRAMES=4 MAQUE_MAX_FRAMES=8 \
./build-win/samples/player/maque_player.exe scenes/spine/spine.scn
```

该场景为 320×240，窗口为 800×650。检查第 4 帧，面板不应跟随游戏视图缩放，
且应与同参数的 `MAQUE_PRESENT=sdl` 对照一致。

### B. 静态图集缺字

```bash
MAQUE_AUDIO=off MAQUE_WINDOW=1077x900 MAQUE_PRESENT=wgpu \
MAQUE_CAPTURE_WGPU=.kimi-tmp/bug006/font MAQUE_CAPTURE_FRAMES=4 \
MAQUE_SIM_KEY_AT=2:1004 \
./build-win/samples/player/maque_player.exe --frames 8 scenes/khronos-bin/index.scn
```

打开 F4 后检查“渲染模式”。修复前 WGPU 静态图集路径显示“?染模式”。

## 根因

### 1. 屏幕空间 ImGui 错用游戏视图变换

ImGui 导出的顶点和 clip 已经是渲染目标像素空间，但 `gfx` 的 mesh 路径仍套用游戏视图的
zoom/camera uniform，并把 clip 再与游戏 viewport 求交。顶点被变换而 scissor 没有同步变换，
因此出现放大、偏移和裁切。多视图下还可能重复叠画同一覆盖层。

### 2. 静态图集没有预载 U+6E32「渲」

直接解码 `third_party/imgui/imgui_draw.cpp` 的 `accumulative_offsets_from_0x4E00`，
得到 2500 个码点，其中 `U+6E32 present=False`。WGPU 自研路径只在首次上传前 Build 一次静态图集，
之后不会补载，因此缺字落到 fallback `?`。

SDL 官方后端使用动态图集，可以按需加载该字；ImGui 与引擎文本模块使用不同图集本身不是 bug。

## 修复

- `modules/render/gfx/src/gfx.cpp`：`MQ_IMGUI_LAYER` 使用独立的屏幕空间 uniform；
  clip 与完整渲染目标求交，不再套用游戏 viewport；只在 `vi == 0` 绘制一次；
  mesh bind-group key 加入坐标空间位，避免同纹理的世界网格与 ImGui 共用错误 uniform。
- `modules/ui/imgui/src/imgui.cpp`：用 `ImFontGlyphRangesBuilder` 合并常用简体字表与
  F4 内建面板固定文案，两档字号共用同一组 ranges；ranges 生命周期覆盖静态 Build。
  不切换全量 CJK，也不增加运行时公开预载接口。

## 回归与证据

- `modules/ui/imgui/tests/imgui_test.cpp`：真实项目 TTF 下检查两档字体的 `U+6E32`
  在首次 Build 后均已加载，codepoint 正确、PackId 不同于 fallback，且 UV 对应区域有非零 alpha。
  检查只读已烘焙数据，不调用会现场补载并掩盖问题的 `FindGlyph*`。
- `tests/imgui_present_e2e.sh`：新增 320×240 场景 / 800×650 窗口的 WGPU 与 SDL 像素对照。
  2026-11-02 Windows 实测通过，面板区域差异 `0.0024%`（阈值 `< 2%`）。
- `ctest --test-dir build-win -R '^imgui$' --output-on-failure`：通过。
- `ctest --test-dir build-win -R '^imgui_present_e2e$' --output-on-failure`：通过。
- `scenes/khronos-bin/index.scn` 的 WGPU、SDL 第 4 帧实际抓帧均完整显示“渲染模式”。

## 相关

- [28 · ImGui 调试面板](../28-ImGui调试面板.md)
- `modules/render/gfx/src/gfx.cpp`
- `modules/ui/imgui/src/imgui.cpp`
- `modules/ui/imgui/tests/imgui_test.cpp`
- `tests/imgui_present_e2e.sh`
