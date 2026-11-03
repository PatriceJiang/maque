# scenes/shadertoy —— 场景自定全屏 shader 示例（Shadertoy 移植）

把 Shadertoy 作品作为**纯场景资源**搬进 maque：场景里挂一个 `render.ShaderPass` 组件，
引擎负责全屏三角形顶点着色器、uniform、绘制与管道，demo 只提供 fragment 入口。
单 pass 作品直接用 `render.ShaderPass`；多 buffer / 自反馈 / 多阶段的作品用
`render.ShaderGraph`（本目录的 `mountains/`）。示例都来自 Shadertoy，移植只做语言/坐标/ABI 适配。

| 文件 | 说明 |
|------|------|
| `seascape.wgsl` | Seascape 的全屏 fragment shader（只提供 `fs_main`，无顶点入口） |
| `seascape.scn` | 场景：640×360，单个实体挂 `render.ShaderPass`，`shader` 相对场景目录 |
| `mandelbulb.wgsl` | Mandelbulb 的全屏 fragment shader（同上） |
| `mandelbulb.scn` | 场景：640×360，`render.ShaderPass → mandelbulb.wgsl` |
| `sprite-shader.scn` | 场景：640×360，左右两块 `render.ShaderSprite`（左 mandelbulb / 右 seascape） |
| `mountains.scn` | 场景：960×540，`render.ShaderGraph → mountains/program.json`（4 阶段 + 鼠标绘制） |
| `mountain-shader-sprite.scn` | 场景：960×540，单个 `render.ShaderSprite` 把 `shader` 指向 `mountains/program.json`（720×405 精灵，多阶段图） |
| `mountains/program.json` | 多通道 shader 图：3 个中间缓冲 + 4 个阶段（buffer A/B/C + image） |
| `mountains/common.wgsl` | 各阶段共享源码（噪声/相机/BRDF/大气/打包/通道采样），运行时拼在阶段源码前 |
| `mountains/buffer_a.wgsl` | 鼠标绘制的高度图（读自己的上一帧 = 自反馈缓冲） |
| `mountains/buffer_b.wgsl` | 侵蚀滤波 + ridgemap/trees/debug 打包进 alpha |
| `mountains/buffer_c.wgsl` | 细节噪声缓冲 |
| `mountains/image.wgsl` | 光线步进地形渲染 + 大气 + ACES |
| `quartz_cave.scn` | 场景：960×540，`render.ShaderGraph → quartz/program.json`（5 阶段 / 4 缓冲） |
| `quartz/program.json` | 多通道 shader 图：3 个非方形缓冲 + 5 个阶段（state/scene/cells/centres + image） |
| `quartz/common.wgsl` | 各阶段共享源码（选项/常量/Shadertoy 兼容层），运行时拼在阶段源码前 |
| `quartz/buffer_a.wgsl` | 拖拽状态（1×6，自反馈） |
| `quartz/buffer_b.wgsl` | 主渲染：凸多面体晶体 + 湿岩 + 光柱 + 尘埃 + 色散/焦散，输出线性 HDR |
| `quartz/buffer_c.wgsl` | 光照单元（64×24）：高于阈值的亮度与质心 |
| `quartz/buffer_d.wgsl` | 最亮的 6 个分离光心（6×1） |
| `quartz/image.wgsl` | tone map + `value / distance` 星芒 |
| `README.md` | 本文件 |

## 作品与许可

四个原作许可**不同**，逐项见下面的小节：

- Seascape / Mandelbulb 以 **CC BY-NC-SA 3.0**
  （Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported
  <https://creativecommons.org/licenses/by-nc-sa/3.0/>）发布：署名保留、**不得用于商业用途**，
  修改/再分发需以同一许可（相同方式共享）发布。
- Mouse-Paint Eroded Mountains 的噪声/侵蚀代码为 **MPL 2.0**（弱 copyleft，按文件生效；
  修改后的 MPL 文件需以 MPL 发布），地形渲染部分派生自 Fewes 的 Shadertoy。
- quartz cave 的许可**以 Shadertoy 作品页标注为准**（原作未在本目录另附许可）；移植只做
  语言/坐标/ABI 适配，数学、材质、光照与全部参数值均来自原作。

### Seascape — "Seascape" by Alexander Alekseev aka TDM (2014)

- Shadertoy：<https://www.shadertoy.com/view/Ms2SD1>，联系方式（原作）：tdmaav@gmail.com
- 移植：GLSL → WGSL（`mat3 fromEuler` 列主序 + 行向量乘法 → `transpose(M) * v`、
  `heightMapTracing` out 参数 → `ptr<function, vec3f>`、WebGPU `frag_coord` y 轴翻转），
  并恢复 Shadertoy 时间语义 `time = iTime*0.3 + iMouse.x*0.01`
  （某些 GLSL 移植版改写为 `uDrift*5`，那是移植侧改动，本文件不采用）。

### Mandelbulb — "Mandelbulb" by Inigo Quilez (2013)

- Shadertoy：<https://www.shadertoy.com/view/MdXSWn>
- 参考源码：作者授权的 **SIGGRAPH 2012 WebCL 课程** OpenCL 移植
  （`examples/shadertoy/mandelbulb.cl`，同一份数学）。
- 移植：power-8 Mandelbulb 距离估计（多项式展开，`NumIte=7`、`Bailout=100`）、
  包围球半径 1.25、表面 epsilon `clamp(0.001*t*fovfactor, 1e-6, 0.005)`、
  3 次额外 DE 采样求梯度法线、20 秒一圈的相机轨道
  （`r = 1.4 + 0.2*cos(2πt/20)`、`campos = (r*sin, 0.3-0.4*sin, r*cos)`、目标 `(0,0.1,0)`、fov=1）、
  双光源 + 轨道陷阱材质混色（`sqrt(trap)*1.25`）+ 阴影光线 + AO + gamma `1.5*(rgb*0.15+0.85*sqrt(rgb))` + 暗角。
  `iTime` 直接对应组件 `time`（相机周期已写在 shader 里，无额外缩放）。

### Mouse-Paint Eroded Mountains — Rune Skovbo Johansen (2026)

- Shadertoy：<https://www.shadertoy.com/view/sf23W1>；技术文章：
  <https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html>
- 许可：**Phacelle Noise + 侵蚀滤波为 MPL 2.0**（<https://mozilla.org/MPL/2.0/>，版权归作者）；
  地形渲染派生自 Fewes 的 <https://www.shadertoy.com/view/7ljcRW>；
  BRDF 参考 <https://www.shadertoy.com/view/XlKSDR>，相机参考 <https://www.shadertoy.com/view/XsB3Rm>。
- 结构：原作 5 个标签页 → 本目录 5 个 WGSL 文件 + 1 份 program JSON。**Common 标签页 =
  `common.wgsl`**（运行时拼在每个阶段源码之前），三个 buffer + image 各自一个阶段。
- 移植要点：
  - Shadertoy 的 `#define` → `const`；只在原作里**默认开启**的配置（`WATER` / `TREES` /
    `DETAIL_TEXTURE` / `DRAINAGE` / `SHADOWS` / `FIXED_SUN`）被无条件保留；调试开关
    （`SHOW_*` / `GREYSCALE` / `COMPARISON_SLIDER`）原作未启用，不移植。
  - `iChannel0..3` → `ch0..ch3`；buffer 用 **RGBA32F**（`pack4` 把 ridgemap/trees/debug 打进 alpha，
    需要完整 32 位尾数）。WebGPU 不能硬件过滤 float32 → `textureSH()` 手写双线性、
    `texelFetchSH()` 翻 y（Shadertoy 纹理原点在左下）。
  - 键盘纹理（Backspace 重置 / Shift 反向绘制 / Enter 开关侵蚀）→ uniform `keys`；
    `iFrameRate` → 固定 60（引擎是固定步长，绘制速度因此确定性）。
  - `iChannel2`（抖动噪声贴图）→ 像素坐标的确定性哈希抖动（同 1/255 量级）。
  - Buffer 尺寸：原作在 iResolution 大小的纹理里用左下角 `BUFFER_SIZE²` 方块；本移植把缓冲本身
    做成 `BUFFER_SIZE²` 正方形，`image` 里 `BUFFER_SIZE` 取 `textureDimensions(ch0)`。
- 交互：鼠标左键绘制（Shift 反向）、Backspace 重置、Enter 开关侵蚀；相机按原作自转
  （`TIME_CAM_SPIN`），`iTime` = 组件 `time`。
- 精灵形态：`mountain-shader-sprite.scn` 只有**一个实体**——`render.ShaderSprite` 的 `shader` 直接写
  `"mountains/program.json"`（引擎按扩展名判定：`.json` = 多阶段图，`.wgsl` = 单 pass），`width/height`
  写 720×405。整条阶段链先渲染到该节点的离屏纹理再合成 2D 精灵；指针只在矩形内生效。

### quartz cave 💎 — 作者 aladiN

- 许可：**以 Shadertoy 作品页标注为准**（原作未在本目录另附许可）；移植只做语言/坐标/ABI 适配。
- 5 个阶段 / 4 个缓冲，其中 **3 个缓冲是非方形的**（`width`/`height`）：
  | 阶段 | 目标缓冲 | 尺寸 | 内容 |
  |------|----------|------|------|
  | `buffer_a` | `state`（自反馈） | 1×6 | 拖拽偏移/按下/锚点/起点/上次位置/时间戳 |
  | `buffer_b` | `scene` | 960×540 | 凸多面体晶体（6 棱柱 + 3 大 3 小菱面）、湿岩、光柱、尘埃、色散、焦散 → **线性 HDR** |
  | `buffer_c` | `cells` | 64×24 | 每单元累加高于 `BRIGHT_TH` 的亮度 + 质心 + 峰值 |
  | `buffer_d` | `centres` | 6×1 | 最亮的 6 个**分离**光心（合并的星保留各自的光） |
  | `image` | 主目标 | 屏幕 | tone map + 每个光心一颗 `value / distance` 星芒 |
- 移植要点：
  - 原作用 **iResolution 大小的纹理 + 角落若干 texel** 存状态/光照单元；这里直接给每个缓冲它
    真正用到的尺寸。所以这几个阶段里的「屏幕尺寸」取 `mq_pass.screen_resolution`（不是该阶段
    自己的 `resolution`）——`common.wgsl` 的 `dragN`、`buffer_c`、`buffer_d` 都按这条写。
  - Shadertoy 的**左下原点**纹理坐标由 `texelFetchSH` / `textureSH` 翻 y 保留；
    每个阶段的 `fragCoord` 也经 `fragCoordSH` 翻到同一约定（`image` 一旦漏翻，合成就上下颠倒）。
  - GLSL 行向量乘法 `v * M` → `transpose(M) * v`；WGSL 不允许给 swizzle 赋值、
    也不能用 `smoothstep(edge0 > edge1)`，所以有 `ss()` 与显式分量重建。
  - 原作 `#ifdef GL_ES` 的展开分支（`ZERO`/`SEQ_*` 命名槽）只为压低 shadertoy.com 的 ANGLE
    编译时间，这里用桌面分支：普通数组 + 动态索引。
  - `image` 按 `fragCoord / resolution * textureDimensions(ch1)` 取 `scene` 缓冲：主目标尺寸与
    `scene` 缓冲不同（窗口 resize）时是最近邻放大，相同尺寸时逐 texel 一一对应。
- 交互：**拖拽环绕**（拖得越高相机越靠近、越俯视晶体里的彩虹），单击回到初始视角。
  拖拽状态跨帧保留在 `state` 缓冲里（原作靠 `STATE_STAMP` 在热重载后复位，这里 `frame` 由引擎
  在场景重载时重置，同样安全）。
- 性能：`scene` 是 960×540 的逐像素闭式求交（晶体 12 个半空间 × 6 个 + 12 块岩石 + 光柱 erf +
  尘埃 DDA）。没有光线步进，但比 Seascape 重、比 Mandelbulb 轻。

## 引擎侧 shader-pass 契约（摘要）

组件 `render.ShaderPass`（完整字段见 schema `modules/core/data/schema/render/render.yaml`）：

| 字段 | 默认 | 说明 |
|------|------|------|
| `shader` | `""` | WGSL 文件路径，**相对场景目录**（VFS 根 = 场景目录） |
| `speed` | `1.0` | time 推进倍率 |
| `paused` | `false` | 冻结 time |
| `enabled` | `true` | `false` = 不绘制 |
| `time` | `0.0` | 运行期按 `dt*speed` 推进（脚本可读写/scrub） |

shader 文件侧必须满足：

1. **只提供 fragment 入口**（名字固定）：
   ```wgsl
   @fragment
   fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f
   ```
2. 唯一 uniform（group 0 / binding 0，共 32 字节，无其它 bind group）：
   ```wgsl
   struct MqShaderPass {
       resolution: vec2f,   // 渲染目标像素尺寸（宽, 高）
       time: f32,           // 组件 time（秒；Mandelbulb/Seascape 里对应 Shadertoy iTime）
       frame: f32,          // 帧号
       mouse: vec4f,        // xy = 指针在目标里的像素坐标（原点左上，与 frag_coord 同向）；zw 预留
   };
   @group(0) @binding(0) var<uniform> mq_pass: MqShaderPass;
   ```
3. `frag_coord` 是 WebGPU 帧缓冲坐标（**原点左上、y 向下**）；Shadertoy 的 `fragCoord`
   原点左下、y 向上 → 想与 Shadertoy 画面一致就在 shader 里翻 y：
   `vec2f(fc.x, resolution.y - fc.y)`（两个文件都已这么做）。
4. `render.ShaderPass` 只画背景层（先于 3D/2D）；纯 shader 场景（无 3D 组件）也走这条通路。
5. 失败语义：文件缺失 / WGSL 编译失败 → stderr 明确报错，场景其余部分照常运行，不崩溃。

## shader 精灵（`render.ShaderSprite`）

同一份 `.wgsl` 也可以只画在**节点矩形**里（2D 精灵），一个场景放多块——`sprite-shader.scn` 就是
左右并排两块：左 `mandelbulb.wgsl`、右 `seascape.wgsl`：

```json
{ "name": "left", "components": [
    { "type": "scn.Transform2D", "x": 160, "y": 180 },
    { "type": "render.ShaderSprite", "shader": "mandelbulb.wgsl", "width": 312, "height": 344 } ] }
```

| 字段 | 默认 | 说明 |
|------|------|------|
| `shader` | `""` | 同上（相对场景目录） |
| `width` / `height` | `64` | 精灵尺寸（设计像素，节点 `scale` 再乘） |
| `speed` / `paused` / `enabled` / `time` | 同 `render.ShaderPass` | 独立的 time 推进 |

**WGSL 契约完全不变**（还是 `fs_main` + 32B uniform）：只是 `resolution` 变成该精灵离屏纹理的像素
尺寸、`frag_coord` 从该纹理左上起算、`mouse.xy` 是精灵内局部像素。引擎先把精灵渲染到离屏纹理，
再当普通带纹理矩形并入 2D 图元批次，所以位置/旋转/画布缩放/letterbox/图层序都和 `scn.Sprite` 一致。
详见 `docs/13-渲染（WebGPU）.md` §场景自定义 shader 精灵。

## 运行

```powershell
# 有窗口（默认 SDL 呈现）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\seascape.scn
.\build-win\samples\player\maque_player.exe scenes\shadertoy\mandelbulb.scn
# 多通道 shader 图（4 阶段 + 鼠标绘制；山体由 Buffer A 的初始笔刷生成）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\mountains.scn
# 多通道 shader 图（5 阶段 + 3 个非方形缓冲；拖拽环绕 → 晶体里的彩虹）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\quartz_cave.scn
# 同一份 program 的精灵形态（画在节点矩形里，backdrop 衬底）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\mountain-shader-sprite.scn
# 两块 shader 精灵并排（同一份 WGSL 契约，画在节点矩形里）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\sprite-shader.scn

# 无窗口（CI / 无显示环境）：仍会渲染，但不出窗口
$env:MAQUE_WINDOW='0x0'
.\build-win\samples\player\maque_player.exe scenes\shadertoy\mandelbulb.scn

# 可选：wgpu 交换链直呈（3D/直呈路径；否则默认 SDL_Renderer 呈现）
$env:MAQUE_PRESENT='wgpu'

# 截一张图（默认第 1 帧；.png/.ppm 均可，截完即退出）
.\build-win\samples\player\maque_player.exe --shot scenes\shadertoy\shot.png --frames 2 scenes\shadertoy\seascape.scn

# 批量截图看动画（帧号写进文件名）：Seascape 海浪/相机、Mandelbulb 相机轨道、Mountains 相机自转
.\build-win\samples\player\maque_player.exe --shot-seq scenes\shadertoy\mb_{}.png --shot-frames 1,60 scenes\shadertoy\mandelbulb.scn
.\build-win\samples\player\maque_player.exe --shot-seq scenes\shadertoy\mt_{}.png --shot-frames 1,120 scenes\shadertoy\mountains.scn
.\build-win\samples\player\maque_player.exe --shot-seq scenes\shadertoy\qz_{}.png --shot-frames 1,120 scenes\shadertoy\quartz_cave.scn

# 用输入模拟器"画"山（世界坐标：按住左键拖动；Shift 反向、Backspace 重置、Enter 关侵蚀）
$env:MAQUE_SIM='tests\data\sim\ball_clicks.sim.json'   # 或自己的 .sim.json
# 用输入模拟器拖拽环绕 quartz cave（拖得越高相机越靠近晶体）
$env:MAQUE_SIM='tests\data\sim\quartz_orbit.sim.json'
$env:MAQUE_SHOT='scenes\shadertoy\qz_orbit.png'; $env:MAQUE_SHOT_FRAME='70'
.\build-win\samples\player\maque_player.exe scenes\shadertoy\quartz_cave.scn
```

Linux/macOS 把 `.\build-win\...` 换成 `./build/samples/player/maque_player`。
截图尺寸 = 虚拟屏幕尺寸（无窗口时为设计分辨率 × 视图数）× `MAQUE_RENDER_SCALE`；
有窗口交互运行时主目标则是 drawable 设备像素 × `render_scale`。

几个 shader 的负载差别很大：Seascape 是少量 noise/高度图求交（很快）；
Mandelbulb 是逐像素 raymarch（每步 4 次 DE + 更细的 3 次梯度采样 + 阴影光线）；
Mountains 每帧要跑 4 个阶段——Buffer B 是 5 个 octave × 16 个 Phacelle cell 的侵蚀滤波，
Image 是 96 步 × 4 次手写双线性采样（float32 缓冲不可硬件过滤）。960×540 下仍可交互，
但别拿它当性能基准。
Quartz cave 也是 5 个阶段，但主要负载在 Buffer B：逐像素闭式求交（6 个晶体 × 12 个半空间、
12 块岩石、光柱 erf 积分、尘埃 DDA），外加最后一次 per-wavelength 的色散重放。没有光线步进，
比 Mandelbulb 轻，但比 Seascape 重不少。
`sprite-shader.scn` 同屏跑**两个**作品（各一张离屏纹理），负载是两者之和；它也是
"同一份 WGSL 既做全屏背景、又做精灵"的例子。

`scenes/render_texture/shader-sprites-cube.scn` 把这两块精灵再往前推一步：精灵先渲染进
`rendertexture://sprites`（2D 相机），再贴到 cube 正面的屏幕上。注意 VFS 禁止 `..` 跨场景引用，
所以那边放的是本目录两个 `.wgsl` 的**逐字节副本**（`ctest -R render_texture_e2e` 用 `cmp` 防漂移）。

## 为什么 shader 放在 `scenes/` 而不是 `assets/builtin/shaders/`？

- `assets/builtin/shaders/` 是**引擎内建 shader**：一个 shader 一个目录，配置期由
  `cmake/shader_pack.cmake` 打包、运行时按注册表取用，并可按 shader 名裁剪
  （`-DMAQUE_SHADER_XXX=OFF`）。把 demo 放进去会让它进入引擎构建/二进制与注册表，
  属于"引擎资产"而不是"游戏资产"。
- 本目录的 shader 是**场景资源**：路径相对场景目录、由 `render.ShaderPass` / `render.ShaderSprite`
  直接加载源码，`render.ShaderGraph` 直接读 program JSON + 各阶段 `.wgsl`；
  改完无需重编引擎；这正是"引擎即浏览器、游戏即资源"的演示——
  换一个 `.scn` / `.wgsl` 就是一个新 demo。
- 因此仓库中**不存在** `assets/builtin/shaders/seascape/`、`assets/builtin/shaders/mandelbulb/`、
  `assets/builtin/shaders/mountains/` 或 `assets/builtin/shaders/quartz/`。

## 如何修改

- 改 Seascape：编辑 `seascape.wgsl`。海面参数在文件顶部（`SEA_HEIGHT`、`SEA_CHOPPY`、
  `SEA_SPEED`、`SEA_FREQ`、`SEA_BASE`、`SEA_WATER_COLOR`），光线步进参数
  `NUM_STEPS`、`ITER_GEOMETRY`、`ITER_FRAGMENT`；时间偏移在 `fs_main`（`iTime*0.3 + mouse.x*0.01`）。
- 改 Mandelbulb：编辑 `mandelbulb.wgsl`。迭代/逃逸阈值 `NUM_ITE`、`BAILOUT`；
  材质 `MATERIAL1/2/3`、光源 `LIGHT1/2`；相机轨道半径/高度/周期在 `fs_main`
  （`r = 1.4 + 0.2*cos(...)`、`0.3 - 0.4*sin(...)`、`20.0` 秒周期）。
  可以先用小分辨率（把 `.scn` 的 `width/height` 调小）快速迭代 raymarch 参数。
- 改尺寸/位置：`*.scn` 的 `width`/`height`（shader 里用 `resolution` 做坐标归一化，改分辨率不会拉伸画面；
  注意 Mandelbulb 沿用原作硬编码的 `1.33` 宽高比系数）。
- 动画/交互：`time` 由组件驱动。想改推进速度就给组件加 `"speed": 2.0`，想暂停就加
  `"paused": true`；脚本也可读写成 scrub。
- 换成自己的 shader：单 pass 就新建 `xxx.wgsl`（遵守上面的入口 + uniform 契约）与
  `xxx.scn`（`shader: "xxx.wgsl"`）。如果要多个 pass，可以各自挂在不同实体上
  （绘制顺序按实体 id），或者用 `render.ShaderGraph` + program JSON（见
  `mountains/` 与 `docs/13-渲染（WebGPU）.md` §多通道 shader）。
- 改 Mountains：侵蚀参数在 `mountains/buffer_b.wgsl` 的 `Heightmap()`
  （`EROSION_SCALE/STRENGTH/GULLY_WEIGHT/DETAIL/ROUNDING/ONSET/OCTAVES/...`）；
  材质色在 `mountains/common.wgsl`（`CLIFF_COLOR`/`DIRT_COLOR`/`GRASS_COLOR*`/`SAND_COLOR`/`WATER_*`）；
  相机在 `common.wgsl` 的 `CAMERA_DIST/FOV/ANGLE/ELEVATION/LOOKAT` 与 `TIME_CAM_SPIN`；
  初始地形笔刷在 `buffer_a.wgsl`（首帧的居中凸起，`0.35` 是笔刷半径）。
  缓冲分辨率在 `mountains/program.json` 的 `buffers[].size`（正方形；越大越清晰也越慢）。
- 改 Quartz cave：**全部开关在 `quartz/common.wgsl`**（原作 Common 标签页顶部）——
  `DISPERSION`（1 = 物理石英，5 = 更宽的彩虹）、`NLAMBDA`、`BOUNCES`、`CRACKS`、
  `CAUSTICS`、`DUST*`、`STARS`/`STAR_*`/`LEN_*`/`SRC_GLOW`、`SHAFT_BRIGHT`、
  `SUN_SWAY`/`CAM_SWAY`、`ZOOM_IN`、`EXPOSURE`、`AA`（2 = 每轴两采样，四倍代价）、`TIME_SPEED`。
  晶体形状/位置在 `buffer_b.wgsl` 的 `cry(i)`（轴向、半径、柱高、锥高、底部），
  岩石在 `bould(i)` 与 `BSQ`，太阳/环境色在文件顶部（`SUNE`/`SUNCOL`/`SKY_L`/`AMBC`/`ABSORB`/`HAZE`）。
  缓冲分辨率在 `quartz/program.json` 的 `buffers[].width/height`（`scene` 越大越清晰也越慢；
  `cells` 的宽高由 `STAR_ROWS` 决定，改 `STAR_ROWS` 时要同步改 `cells` 的 `height`）。
