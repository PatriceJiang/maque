# scenes/shadertoy —— 场景自定全屏 shader 示例（Shadertoy 移植）

把一个 Shadertoy 作品作为**纯场景资源**搬进 maque：场景里挂一个 `render.ShaderPass` 组件，
引擎负责全屏三角形顶点着色器、uniform、绘制与管道，demo 只提供 fragment 入口。

| 文件 | 说明 |
|------|------|
| `seascape.wgsl` | 全屏 fragment shader（只提供 `fs_main`，无顶点入口） |
| `seascape.scn` | 场景：640×360，单个实体挂 `render.ShaderPass`，`shader` 相对场景目录 |
| `README.md` | 本文件 |

## 作品与许可

- 原作：**"Seascape" by Alexander Alekseev aka TDM — 2014**
  - Shadertoy：<https://www.shadertoy.com/view/Ms2SD1>
  - 许可：**CC BY-NC-SA 3.0**（Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported）
    <https://creativecommons.org/licenses/by-nc-sa/3.0/>
  - 联系方式（原作）：tdmaav@gmail.com
- `seascape.wgsl` 是该作品的 **WGSL 移植**：署名保留、**不得用于商业用途**，
  修改/再分发需以同一许可（相同方式共享）发布。
- 移植只做语言与 ABI 适配：GLSL → WGSL（含 `mat3 fromEuler` 的列主序/行向量乘法、
  `heightMapTracing` out 参数 → `ptr<function, vec3f>`、WebGPU 的 `frag_coord` y 轴翻转），
  并恢复 Shadertoy 的时间语义 `time = iTime*0.3 + iMouse.x*0.01`
  （某些 GLSL 移植版改写为 `uDrift*5`，那是移植侧改动，本文件不采用）。

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
       time: f32,           // 组件 time（秒）
       frame: f32,          // 帧号
       mouse: vec4f,        // xy = 指针在目标里的像素坐标（原点左上，与 frag_coord 同向）；zw 预留
   };
   @group(0) @binding(0) var<uniform> mq_pass: MqShaderPass;
   ```
3. `frag_coord` 是 WebGPU 帧缓冲坐标（**原点左上、y 向下**）；Shadertoy 的 `fragCoord`
   原点左下、y 向上 → 想与 Shadertoy 画面一致就在 shader 里翻 y：
   `vec2f(fc.x, resolution.y - fc.y)`（本文件已这么做）。
4. 只画背景层（先于 3D/2D）；纯 shader 场景（无 3D 组件）也走这条通路。
5. 失败语义：文件缺失 / WGSL 编译失败 → stderr 明确报错，场景其余部分照常运行，不崩溃。

## 运行

```powershell
# 有窗口（默认 SDL 呈现）
.\build-win\samples\player\maque_player.exe scenes\shadertoy\seascape.scn

# 无窗口（CI / 无显示环境）：仍会渲染，但不出窗口
$env:MAQUE_WINDOW='0x0'
.\build-win\samples\player\maque_player.exe scenes\shadertoy\seascape.scn

# 可选：wgpu 交换链直呈（3D/直呈路径；否则默认 SDL_Renderer 呈现）
$env:MAQUE_PRESENT='wgpu'

# 截一张图（默认第 1 帧；.png/.ppm 均可，截完即退出）
.\build-win\samples\player\maque_player.exe --shot scenes\shadertoy\shot.png --frames 2 scenes\shadertoy\seascape.scn
```

Linux/macOS 把 `.\build-win\...` 换成 `./build/samples/player/maque_player`。
截图尺寸 = 渲染目标（设计分辨率 640×360 × 视图数 × `MAQUE_RENDER_SCALE`，未设时 1.0）。

## 为什么 shader 放在 `scenes/` 而不是 `assets/builtin/shaders/`？

- `assets/builtin/shaders/` 是**引擎内建 shader**：一个 shader 一个目录，配置期由
  `cmake/shader_pack.cmake` 打包、运行时按注册表取用，并可按 shader 名裁剪
  （`-DMAQUE_SHADER_XXX=OFF`）。把 demo 放进去会让它进入引擎构建/二进制与注册表，
  属于"引擎资产"而不是"游戏资产"。
- 本目录的 shader 是**场景资源**：路径相对场景目录、由 `render.ShaderPass` 直接加载源码，
  改完无需重编引擎；这正是"引擎即浏览器、游戏即资源"的演示——
  换一个 `.scn` / `.wgsl` 就是一个新 demo。
- 因此仓库中**不存在** `assets/builtin/shaders/seascape/`。

## 如何修改

- 改画面：直接编辑 `seascape.wgsl`（海面高度/浪尖/速度/颜色常量在文件顶部：
  `SEA_HEIGHT`、`SEA_CHOPPY`、`SEA_SPEED`、`SEA_FREQ`、`SEA_BASE`、`SEA_WATER_COLOR`；
  光线步进参数：`NUM_STEPS`、`ITER_GEOMETRY`、`ITER_FRAGMENT`）。
- 改尺寸/位置：`seascape.scn` 的 `width`/`height`（shader 里用 `resolution` 做宽高比修正，改分辨率不会拉伸）。
- 动画/交互：`time` 由组件驱动；`mouse.x` 参与原作的时间偏移。想改推进速度就给组件加
  `"speed": 2.0`，想暂停就加 `"paused": true`；脚本也可读写成 scrub。
- 换成自己的 shader：新建 `xxx.wgsl`（遵守上面的入口 + uniform 契约）与
  `xxx.scn`（`shader: "xxx.wgsl"`）。如果要多个 pass，可各自挂在不同实体上
  （绘制顺序按实体 id）。
