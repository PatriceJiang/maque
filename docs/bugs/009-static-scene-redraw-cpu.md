# 009 · 静态场景每帧重绘导致 CPU 占用偏高

- **类型**：性能（CPU）
- **状态**：fixed
- **影响面**：窗口 + 直呈（`MAQUE_PRESENT=wgpu`）的静态 3D 场景（典型：Box、单个 glTF）；
  静态 2D Label 场景的主体开销在文本重排，见文末
- **发现版本**：`43f2700` 之后；release 下静态 Box 约 **20% of one core**

## 症状

静态 Box（1 draw / 12 三角形），release、800×650、`MAQUE_PRESENT=wgpu`：

- 进程 CPU 约 **20% of one core**；`MAQUE_TRACE_STATS=1` 显示 `render=3–7 ms/frame`；
- 场景没有任何脚本/动画/输入，画面逐帧完全相同，但每帧仍完整走
  `extract_3d → command encoder/2 个 pass → submit → present`。

## 根因

主循环固定 60 Hz，**没有“画面是否变化”的概念**：即使视觉输入完全不变，也每帧重建
render pass/command buffer 并提交、present。对 1 个 draw 的场景，这个固定开销约 3–7 ms，
在 60 fps 下就是 20% 单核。

## 修复

在 runtime 的 3D 直呈分支加**静态帧跳过**：

- 对视觉输入做 FNV 哈希：`views / draws / views3d / lights3d / mats3d / draws3d / skins3d /
  passes3d / env / 目标尺寸 / 背景 / render_mode / msaa`；
- 哈希不变且没有外部像素消费者时，跳过本帧的 `render_frame3d` 与 `present`；
- 保守排除（任一存在就强制每帧渲染）：glTF 动画/蒙皮/morph、Spine、粒子、morph mesh、蒙皮 draw、
  RenderTexture、场景 shader pass、ImGui、输入事件、窗口 resize、`--shot`/`MAQUE_CAPTURE*`、
  `Capture.screen`；
- 每 **60 帧**强制重画一次，兜住窗口 expose/驱动 surface 变化；
- 2D 直呈分支做同样的哈希跳帧（`views/draws`）。

脚本、物理、动画采样等**非视觉逻辑仍每帧推进**，只是不提交 GPU 帧。

## 回归与证据

- 静态 Box（800×650，release，直呈）稳态 CPU：**20.4% → 1.7% of one core**；开 Tracy client
  （未连 profiler）为 **1.4%**；
- `MAQUE_TRACE_STATS=1`：`render` 从 3–7 ms 降到 **0.1 ms**，`total` 约 0.3 ms；
- 以下回归全部通过：`render_texture_e2e`、`gltf_anim/skin/morph_e2e`、`shot_e2e`、
  `imgui_present_e2e`、`window_resize_e2e`；
- 动画/截图/RenderTexture/ImGui/输入路径均因排除条件保持每帧渲染。

## 已知边界

- 静态 **2D 文本**场景（如 `scenes/label/label.scn`）跳过了 GPU 提交，但 `extract` 仍每帧重新排版
  207 个字形，`logic≈4.8 ms`，CPU 仍约 30%。要做“静态 2D 也接近 0”，下一步需要**文本/提取结果缓存**
  （场景无变化时复用上一帧 draws），不在本次范围内。
- 每 60 帧强制刷新一次：静态场景仍会有约 1 Hz 的 GPU 提交。

## 相关

- [008](008-wgpu-direct-present-no-poll.md)（直呈路径的延迟销毁）
- [29 · Tracy](../29-Tracy性能分析.md)
