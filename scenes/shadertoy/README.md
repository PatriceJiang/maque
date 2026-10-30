# scenes/shadertoy —— 场景自定全屏 shader 示例（Shadertoy 移植）

把 Shadertoy 作品作为**纯场景资源**搬进 maque：场景里挂一个 `render.ShaderPass` 组件，
引擎负责全屏三角形顶点着色器、uniform、绘制与管道，demo 只提供 fragment 入口。
两个示例都来自 Shadertoy，移植只做语言/坐标/ABI 适配。

| 文件 | 说明 |
|------|------|
| `seascape.wgsl` | Seascape 的全屏 fragment shader（只提供 `fs_main`，无顶点入口） |
| `seascape.scn` | 场景：640×360，单个实体挂 `render.ShaderPass`，`shader` 相对场景目录 |
| `mandelbulb.wgsl` | Mandelbulb 的全屏 fragment shader（同上） |
| `mandelbulb.scn` | 场景：640×360，`render.ShaderPass → mandelbulb.wgsl` |
| `README.md` | 本文件 |

## 作品与许可

两个原作均以 **CC BY-NC-SA 3.0**
（Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported
<https://creativecommons.org/licenses/by-nc-sa/3.0/>）发布：署名保留、**不得用于商业用途**，
修改/再分发需以同一许可（相同方式共享）发布。

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
4. 只画背景层（先于 3D/2D）；纯 shader 场景（无 3D 组件）也走这条通路。
5. 失败语义：文件缺失 / WGSL 编译失败 → stderr 明确报错，场景其余部分照常运行，不崩溃。

## 运行

```powershell
# 有窗口（默认 SDL 呈现）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\seascape.scn
.\build-win\samples\player\maque_player.exe scenes\shadertoy\mandelbulb.scn

# 无窗口（CI / 无显示环境）：仍会渲染，但不出窗口
$env:MAQUE_WINDOW='0x0'
.\build-win\samples\player\maque_player.exe scenes\shadertoy\mandelbulb.scn

# 可选：wgpu 交换链直呈（3D/直呈路径；否则默认 SDL_Renderer 呈现）
$env:MAQUE_PRESENT='wgpu'

# 截一张图（默认第 1 帧；.png/.ppm 均可，截完即退出）
.\build-win\samples\player\maque_player.exe --shot scenes\shadertoy\shot.png --frames 2 scenes\shadertoy\seascape.scn

# 批量截图看动画（帧号写进文件名）：Seascape 海浪/相机、Mandelbulb 相机轨道
.\build-win\samples\player\maque_player.exe --shot-seq scenes\shadertoy\mb_{}.png --shot-frames 1,60 scenes\shadertoy\mandelbulb.scn
```

Linux/macOS 把 `.\build-win\...` 换成 `./build/samples/player/maque_player`。
截图尺寸 = 虚拟屏幕尺寸（无窗口时为设计分辨率 640×360 × 视图数）× `MAQUE_RENDER_SCALE`；
有窗口交互运行时主目标则是 drawable 设备像素 × `render_scale`。

两个 shader 的负载差别很大：Seascape 是少量 noise/高度图求交（很快）；
Mandelbulb 是逐像素 raymarch（每步 4 次 DE + 更细的 3 次梯度采样 + 阴影光线），
在集显上也明显更慢——640×360 下仍可交互，但别拿它当性能基准。

## 为什么 shader 放在 `scenes/` 而不是 `assets/builtin/shaders/`？

- `assets/builtin/shaders/` 是**引擎内建 shader**：一个 shader 一个目录，配置期由
  `cmake/shader_pack.cmake` 打包、运行时按注册表取用，并可按 shader 名裁剪
  （`-DMAQUE_SHADER_XXX=OFF`）。把 demo 放进去会让它进入引擎构建/二进制与注册表，
  属于"引擎资产"而不是"游戏资产"。
- 本目录的 shader 是**场景资源**：路径相对场景目录、由 `render.ShaderPass` 直接加载源码，
  改完无需重编引擎；这正是"引擎即浏览器、游戏即资源"的演示——
  换一个 `.scn` / `.wgsl` 就是一个新 demo。
- 因此仓库中**不存在** `assets/builtin/shaders/seascape/` 与 `assets/builtin/shaders/mandelbulb/`。

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
- 换成自己的 shader：新建 `xxx.wgsl`（遵守上面的入口 + uniform 契约）与
  `xxx.scn`（`shader: "xxx.wgsl"`）。如果要多个 pass，可各自挂在不同实体上
  （绘制顺序按实体 id）。
