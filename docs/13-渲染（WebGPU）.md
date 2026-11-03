# 13 · 渲染（WebGPU）

> 上游：`01 §3`（渲染只消费"可序列化绘制描述"）、ADR-0006（WebGPU）、`12 §5`（分层）、`11 §6`（离屏预览）。
> 原则：**WebGPU 是唯一后端**，不自建第二层图形抽象；渲染**不认识**场景存储细节。

## 1. 定位

- **唯一后端 = WebGPU**（ADR-0006）。原生经 wgpu/Dawn，浏览器为原生 WebGPU。
- **后端无关接口**：`maque::render::Renderer`（`modules/render/renderer`），世界/场景只产出中立的
  **`DrawItem`**（位置/尺寸/颜色/纹理 id），**不依赖**任何图形 API 类型。
- `modules/render/gfx` 是 WebGPU **后端实现**（`create_renderer()`）；世界层与 GPU 之间只隔 `Renderer` 这一层。
- **不依赖** scene 的内部存储；只消费**渲染器无关的绘制描述**（文档 01 §3、12 §4）。

## 2. 数据流

```
节点树（唯一模型）──flatten_world() 每帧展平──▶ 实体视图（world_x/y 缓存）──提取(cull/batch)──▶ 绘制描述(DrawList)
   (文档 12)                                    │
                                                ▼
                         Render Graph（pass 依赖）──▶ WebGPU 命令 ──▶ 交换/离屏目标
```

- **提取（extract）**：把可见对象转成**中性的绘制项**（mesh/材质/变换/layer），与存储解耦。
- **绘制描述可序列化** → 可被控制平面/AI 检视（A1）、可截帧调试。

## 3. Pass 结构：分级，不预设重型 Frame Graph

**关键前提**：WebGPU **由实现自动跟踪并插入资源同步**，且**不暴露显存别名**。
→ 经典 frame graph 的头号价值（手动 barrier/状态转换、Vulkan 式别名）**在 WebGPU 上拿不到**。
因此**分三级推进**，避免过早投入重编译器：

| 级别 | 内容 | 成本 | 定位 |
|------|------|------|------|
| **T0** 硬编码有序 pass | 每图层写死顺序 | 极低 | 起步 |
| **T1 轻量声明式 pass 层** | pass 列表 + 声明读写依赖；**不重排、不别名**；支持多目标/离屏 | 低 | ✅ **默认** |
| **T2 完整 frame graph** | DAG、自动重排、pass 裁剪、瞬态别名 | 高 | ⏳ **可选演进**（OD-11） |

**默认走 T1**。收益：

- **多视图组合**：2D 图层 / 3D 世界 / UI / **离屏预览**各成 pass 分支（文档 12 §5、本文 §6）。
- **可观测**：pass 可被控制平面**列举 / 启停 / dump 中间目标**（A1，文档 11 `Renderer` domain）。
- **留接口**：为将来升 T2 预留；T2 仅在**硬指标**（显存/pass 数）成为瓶颈时触发。

## 4. 分层与混合 2D/3D/UI

- 对应文档 12 §5：**2D 图层 / 3D 世界 / UI 层**各有相机与坐标，作为独立 Render Graph 分支。
- 2D：精灵批处理（batcher）；3D：网格 + 实例化；UI：独立层（可选独立 Render Graph）。
- 合成（composite）顺序显式声明。

## 5. 资源与管线

- **GPU 资源经 VFS 资产加载**（纹理/网格/着色器），异步 + 流式（文档 04/08）。
- **着色器**：WGSL，**文本入库**（文档 08 §4，diff/AI 友好）；**内建 shader 源码在
  `assets/builtin/shaders/<name>/`（一个 shader 一个目录）**，配置期由 `cmake/shader_pack.cmake` +
  `tools/shader_pack.js` 打包成构建目录里的**压缩源码**（zstd -19；运行时 libzstd 解压并缓存），
  可按 shader 剔除（`MAQUE_SHADER_<NAME>=OFF`，默认全部编入）。详见该目录的 README。
- **管线/绑定组缓存**：按状态哈希缓存，避免每帧重编译。
- **实例化 / 批处理 / 剔除**：CPU 侧配合 gather；GPU-driven 可选（按平台评估）。

### 成帧：一个视图一个 render pass + bind group 缓存（性能关键，勿回退）

`modules/render/gfx` 的成帧顺序：**稳定排序**（layer → z → 原始序）→ 合并图元/网格为一条命令序列 →
**每个视图只开一个 render pass**，所有命令在同一 pass 内用
`SetPipeline / SetBindGroup / SetScissorRect / SetVertexBuffer` 切换状态完成绘制。

- ⚠️ 旧实现是**每条命令一个 pass**（`all.scn` 331 条命令 → 331 次 `Begin/EndRenderPass`
  + 331 个 bind group），实测 render 12.5ms → 改为单 pass 后 8.4ms（Debug+ASan）、
  8.1ms → 5.1ms（Release）。不要再逐命令开 pass。
- bind group 按状态缓存（每视图一份：`solid` / `textured(贴图+采样器)` / `mesh(贴图)`），
  命令顺序不变 → 绘制结果与逐 pass 实现**逐像素一致**（`gfx`/`twocolor`/`text_pixel`/`spine_demos`/
  `shot_e2e` 等用例覆盖）。
- 每个模拟步渲染+present 一次；**屏幕刷新率 ≈ 1000 / 单步总耗时**。单步超过 16.7ms 就到不了 60fps
  （主循环的墙钟补偿会多跑模拟步，但每步仍各自渲染/呈现）。
  诊断：`MAQUE_TRACE_STATS=1` 每 30 帧打印
  `logic / render / present / draws / tris / target`，先定位是逻辑还是渲染/呈现。

## 6. 离屏渲染与 AI 预览（关键）

- 支持**离屏目标**渲染 → `readback` → 图像。
- 这是文档 11 §6 的 `Renderer.capture_frame`：**无 GUI 也能预览**，AI 可交叉判断正确性。
- 同样用于 CI 的**图像回归测试**（与基准帧比对）。

### 6.1 相机 RenderTexture

> 完整使用手册、场景示例、截图协议、性能建议和排错见
> [33 · RenderTexture：相机离屏纹理与截图](33-RenderTexture相机离屏纹理.md)。

`render.RenderTexture` 是挂在相机节点上的组件，不内嵌第二套相机字段。支持三种现有相机：

- `scn.Camera`：旧 2D 相机；
- `render.Camera2D`：位置取同节点 2D 世界变换，使用 `zoom/offset/viewport/layer_mask/clear_color`；
  当前 2D renderer 是等比缩放 ABI，`zoom.x != zoom.y` 时明确告警并使用 `zoom.x`；
- `render.Camera3D`：位置/朝向取同节点 `scene.Transform3D`，支持 Perspective/Orthographic、viewport、
  clear mode 与 culling mask。

同节点挂 RenderTexture 后，该相机不再参与主画面的相机选择，只负责自己的离屏目标。GPU 目标同时具有
`RenderAttachment | TextureBinding | CopySrc`，进入普通纹理句柄空间，因此 Sprite、网格和 PBR 贴图槽都不需要
专用材质类型。

| 字段 | 默认 | 语义 |
|------|------|------|
| `name` | 节点名 | 实例内唯一的纹理名；节点名也为空时用 `camera-<id>` |
| `width/height` | `0/0` | `0` 跟随场景设计分辨率；非零为固定像素尺寸，不随窗口缩放 |
| `update_mode` | `EveryFrame` | `EveryFrame` 每帧更新；`OnDemand` 首帧更新，之后仅标脏时更新 |
| `filter` | `Linear` | 采样过滤；像素画/监控小窗可用 `Nearest` |
| `include_ui` | `false` | 2D 相机默认只拍世界层；开启后包含游戏 UI，永不包含引擎 ImGui |
| `enabled` | `true` | 关闭时保留上一帧纹理内容，不提交该相机 pass |
| `clear_color` | 透明黑 | `scn.Camera` 的目标清屏色；Camera2D/Camera3D 使用相机自身的 clear color |
| `texture/ready/last_frame` | transient | 运行期只读状态，不写入场景保存/快照 |

**纹理引用**：根场景可写短形式 `rendertexture://minimap`；运行时规范形式为
`rendertexture://<scene-instance-id>/minimap`。短引用在纹理解析时按消费者所属实例限定作用域，且不会把运行期实例 id
写回场景；两个子场景里的同名目标不会串用，bake/save 后重新作为根场景加载也不会断链。`camera_texture(camera)` 返回规范 URI，可直接交给 `node_set_string(..., "texture", uri)`。

**帧序与依赖**：脚本/动画/展平完成后先执行 RenderTexture pass，再画主帧，所以主画面首帧即可采样本帧相机结果，
没有 CPU 回读。多个目标按纹理依赖排序；跨目标依赖环按相机 entity id 固定顺序执行，环中较早目标读取较晚目标的
上一帧，较晚目标可读取较早目标的本帧。目标对自身的采样批次由后端剔除，绝不把同一 WebGPU 纹理同时绑定为
attachment 和 sampled texture。目标创建及 resize 后先清屏，上一帧语义不会读取未初始化显存。

**脚本截图接口**：

```js
const camera = entity_find("capture_camera");
const texture = camera_texture(camera);     // rendertexture://0/minimap
camera_update(camera);                      // 标脏 OnDemand 目标
camera_capture(camera, "shots/minimap.png"); // 当前渲染帧更新后写 PNG；其它扩展写 PPM
```

`camera_capture` 是延迟到本帧 RenderTexture 完成后的冷路径回读，不会为截图建立另一套相机或重复渲染。控制面提供
`Capture.camera(camera, offset, max, max_dim)`（最新目标的 PPM/base64 分块）与变更类
`Capture.camera_save(camera, path)`（请求下一渲染帧更新后落盘）。

首版颜色目标固定为单 mip `RGBA8Unorm`；RenderTexture 可绑定到 PBR 槽，但它表达的是已渲染颜色，不适合作为
normal/MR 等数据贴图。PBR 当前每个材质共用一个 sampler；材质含 RenderTexture 时优先采用该 RT 的过滤设置，
否则采用第一张有效静态贴图的 sampler。逐帧路径保持 GPU→GPU，只有显式截图/控制面抓取才做 GPU→CPU readback。

样例：`scenes/render_texture/render_texture.scn`、`render_texture_2d.scn`、`render_texture_3d.scn`、
`render_texture_cube.scn`；回归：`ctest -R render_texture_e2e`。

## 7. 观测

- **WebGPU timestamp queries** → pass 级 GPU 耗时（文档 09 §3）。
- draw call 数、三角形数、显存水位 → 指标。
- 截帧：单帧完整状态落盘，供 AI/人分析（Chrome Trace 生态，文档 09 §4）。

## 8. 平台与确定性

| 平台 | 后端 | 备注 |
|------|------|------|
| 原生 | wgpu / Dawn | 驱动依赖真实 GPU |
| 嵌入式 | wgpu（**须有 Vulkan/Metal/D3D12**） | **不降级**：无可用后端的目标**不支持**（ADR-0029） |
| WASM | 浏览器原生 WebGPU | 与宿主同一 API |

- **渲染不得影响模拟确定性**（ADR-0005）：渲染是模拟的**只读消费方**，绝不回写模拟状态。

## 9. 遗留风险

| 风险 | 处理 |
|------|------|
| 过度设计 frame graph（WebGPU 收益低） | 默认 T1 轻量层；T2 按硬指标触发（OD-11） |
| 嵌入式 WebGPU 驱动缺失 | 不降级：**限定目标平台**（ADR-0029） |
| 离屏 readback 开销 | 仅按需捕获；异步回读 |
| 管线编译卡顿 | 状态哈希缓存 + 预热 |
| 渲染回写模拟（破坏确定性） | 单向数据流：模拟→提取→渲染 |
| 着色器维护 | 文本入库 + 编译期校验（文档 08） |

## 字形管线：按需图集与两阶段提交

- **shaping（HarfBuzz，可选依赖）**：每个文本 run 先做一次 HB shape —— GPOS kerning、GSUB 连字、
  combining marks/复杂脚本得到 **glyph id + 设计像素 advance**；随后 FreeType 按
  `(gid, 设备 ppem, 合成 bold/italic)` 栅格化。图集查找键是 **glyph id**（不是码点），
  `mq_text_glyph.codepoint` 对连字取 cluster 首码点。关闭：`MAQUE_TEXT_SHAPING=0` 或 `-DMAQUE_HARFBUZZ=OFF`
  （回退逐码点，逐字节不变）。
- **按需栅格化**：图集（每 字体 × 栅格化 ppem 一张）创建时预热 ASCII 95 字形，其余字形在**排版时**惰性
  栅格化并按行打包（宽度固定 512、高度按需增长到 1024；确定性：追加顺序 = 字形出现顺序）。
  `mq_font_atlas_generation` 每次新增字形自增 → 宿主据此决定纹理是否重传。
- **两阶段提交**（必须）：① 所有标签先 measure+layout（把该帧需要的字形全部预热进图集）；
  ② 再按 (字体,字号) 刷新纹理，并按**最终图集尺寸**计算 UV 后提交图元。
  否则先排的标签会用"排版时"的图集尺寸算 UV，而纹理随后变大 → 采样错位（表现为文字重叠）。
- **纹理生命周期**：图集增长后重建纹理，**旧纹理延迟到下一帧**释放（同帧已排队的图元仍引用旧 id）；
  渲染器需容忍已释放的纹理槽（`destroy_texture` 置空，采样时按"无贴图"处理）。
- **布局在「设计像素」空间**：与窗口/渲染倍率无关；`device_scale` 只决定字形栅格化 ppem
  （`round(font_size × device_scale)`）。窗口 resize 会为新的 ppem 惰性新建图集/纹理，旧 ppem 缓存保留。
- **像素吸附必须作用在"图元中心"**：吸附在顶点着色器里执行，若逐顶点吸附
  （每个顶点都拿自己当中心）会使整个矩形偏移半像素 → 1:1 nearest 下部分行列取错纹素。
  正确做法：中心变换到目标空间 → 吸附 → 再加顶点偏移（`px = pc + kc`）。
  **吸附用的 half 必须是目标像素尺寸（`half*zoom`）**：`hw` 是设计空间半宽高，若直接
  `floor(pc-hw+0.5)` 再 `pc=tl+hw`，ss≠1 时图元会按自身半高平移 `half*(1-zoom)`，
  不同高度字形 baseline 差 1px（khronos 标题的 "baseline 不齐" 根因）。
- **字形基线归一**：低 ppem 下 FreeType hinted 位图的可见底行（alpha≥128）会落在 baseline-1/-2；
  栅格化后把贴近基线的字形吸附到 baseline-1（圆笔/直笔一致，标点/重音/降部不动）。
- **换行（UAX #14 + UAX #29）**：`WORD` wrap 的断行机会由 vendored **libunibreak** 给出
  （`set_linebreaks_utf32` + `set_graphemebreaks_utf32`）：空格、连字符、ZWSP、CJK、禁则、组合字等；
  `CHAR` wrap 逐 cell 断但不拆字素。`MAQUE_TEXT_LINEBREAK=0` 回退旧的“空格 + CJK 贪心”。
- **跨 run 与行尾空格**：每个 run 先 shape，再拼成**全局 cell 流**统一换行/UAX 判定（富文本标签切开的词不会
  在 run 边界断开）；行宽、对齐、metrics 都不含行尾空格/制表符，避免 Center/End 被尾随空格推偏。
- **范围**：HarfBuzz 按 run shaping（LTR + CJK）；**不做 bidi**、无字体 fallback；缺字形跳过（不画豆腐块）。

## 图元：矩形、过滤与像素对齐（pixel-perfect）

渲染图元（`mq_renderer_item`）支持三种与像素精确相关的字段：

| 字段 | 语义 |
|------|------|
| `half_w` / `half_h` | **非方形矩形**半宽/半高（`<=0` → 回退到 `size` 的方形/圆形）。字形必须走矩形，方形会把非方形位图拉伸 |
| `filter` | `0` = 线性（默认，精灵缩放）；`1` = **最近邻**（字形/像素画：1 纹素 ↔ 1 像素） |
| `snap` | `1` = 把矩形**左上角**吸附到整数目标像素（中心吸附会因奇偶宽度差半像素而糊） |

实现要点：
- 采样器两套（`linear` / `nearest`），按批次（贴图 + 过滤）选择；批次键含 filter；
- 顶点着色器在相机/缩放/平移**之后**做左上角吸附（`floor(px - half + 0.5) + half`），
  因此任何相机下都能落在整数像素；
- 圆形图元才做 `dot(uv,uv)>1` 裁角（矩形不裁）；
- 实例布局与 WGSL `Inst` 严格对齐（stride 64B，`static_assert` 守卫）。

**窗口放大过滤**：呈现路径在**整数倍**放大时默认最近邻（`MAQUE_PRESENT_FILTER=nearest|linear` 可强制），
否则整帧会被线性插值糊掉。这样 1×/2×/3× 窗口下字形都是硬边。

## 3D 材质模型（plans/19）

3D 表面材质的单一数据源是节点上的 `asset.PbrMaterial` 组件：`shading_model` 选择 10 种 three.js 对齐模型
（Standard/Physical/Basic/Lambert/Phong/Toon/Normal/Matcap/Depth/Shadow），共同字段（`base_color`/`metallic`/
`roughness`/`emissive`/alpha/双面 + 各模型扩展参数 + 8 个贴图槽）由同一个 `pbr` shader 模块的不同
fragment 入口消费。

- gfx 按 `(shading_model, blend, side)` 懒创建管线；`side` 映射 cullMode（缺省 Double 兼容旧行为）。
- 材质 uniform 288B（72 floats，plans/19 单一定义 + gfx 统一打包）；camera uniform 816B（含 view 矩阵与
  analytic studio IBL 参数）。
- **环境光照**：无 HDRI/PMREM 管线时用解析近似 —— 天空/地面渐变 + 软光箱 + Karis env BRDF（漫反射用半球
  辐照度、镜面按 roughness 模糊后采样）；clearcoat 参与环境反射。场景通过 `scn.Scene.env_sky_color/
  env_ground_color/env_highlight/env_rotation` 控制（颜色全 0 = 由 `env_color` 派生）。
- 贴图 UV 变换：`uv_scale/uv_offset`（albedo/MR/AO/emissive）+ `normal_uv_scale/normal_uv_offset`
  （法线，KHR_texture_transform 各贴图可不同）；PBR 贴图运行时生成 CPU mip 链（避免远处高频闪烁）。
- 无贴图时的缺省绑定：颜色/参数用 1×1 白，法线用 1×1 中性法线 (0.5,0.5,1)（避免法线扰动）。
- 详细字段/映射/非目标见 plans/19-threejs-materials.md；glTF 扩展由 `tools/gltf_import.py` 导入。

## 场景自定义全屏 shader（render.ShaderPass）

场景可以让「一个 WGSL 文件」直接当全屏背景画：节点挂 `render.ShaderPass`，引擎用一个全屏三角形把它铺满
渲染目标（**先于 3D/2D 绘制 = 背景层**）。典型用途：Shadertoy 迁移、程序化天空/海洋/噪声背景，以及
**纯 shader 场景**（没有任何 3D/2D 组件，只靠这条通路出画面）。

### 组件字段（`render.ShaderPass`）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `shader` | string | `""` | WGSL 源文件路径，**相对场景目录**（运行时 VFS 根 = 场景目录），如 `"seascape.wgsl"` |
| `speed` | float | `1.0` | `time` 推进倍率（每帧 `time += dt * speed`） |
| `paused` | bool | `false` | `true` = 冻结时间（仍绘制） |
| `enabled` | bool | `true` | `false` = 不绘制 |
| `time` | float | `0.0` | 运行期推进的秒数；脚本可读写（可 scrub / 回放） |

### WGSL 资源契约

- **顶点着色器由引擎提供**（全屏三角形）；资源**只提供 fragment 入口**：

  ```wgsl
  @fragment
  fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f
  ```

- 资源必须声明**唯一** uniform：`group(0) binding(0)`，总 **32B**（无其它 bind group、无纹理）：

  ```wgsl
  struct MqShaderPass {
      resolution: vec2f,   // offset 0：渲染目标像素尺寸 (宽, 高)
      time: f32,           // offset 8：组件 time（秒，已按 speed 推进）
      frame: f32,          // offset 12：帧号
      mouse: vec4f,        // offset 16：xy = 指针在目标内的像素坐标（原点左上，与 frag_coord 同向）；zw 预留
  };
  @group(0) @binding(0) var<uniform> mq_pass: MqShaderPass;
  ```

  | 偏移 | 字段 | 类型/对齐 |
  |------|------|-----------|
  | 0 | `resolution` | vec2f（8B 对齐） |
  | 8 | `time` | f32 |
  | 12 | `frame` | f32 |
  | 16 | `mouse` | vec4f（16B 对齐） |
  | — | 合计 | **32B** |

- `frag_coord` 是 WebGPU 帧缓冲坐标（**原点左上、y 向下**）；Shadertoy 的 `fragCoord` 原点在**左下**，
  想逐行照搬 Shadertoy 时在 shader 里翻 y：
  `let fc = vec2f(frag_coord.x, mq_pass.resolution.y - frag_coord.y);`
- 路径解析：`shader` 相对**场景目录**（与 `render.MeshRenderer.mesh_path` 同一套 VFS 语义），也接受
  VFS 路径（如 `builtin:/...`）；场景自有的 shader 建议放场景目录下的 `.wgsl`，随场景一起分发。
- 失败语义：文件缺失 / WGSL 编译失败 → stderr 明确报错，场景其余部分照常运行（不崩溃）。

### 绘制顺序与限制

- shader pass 作为**背景层**先于 3D / 2D 绘制；**纯 shader 场景允许**没有 `scene.Transform3D` /
  `render.Camera3D` 等任何 3D 组件。
- 3D 相机的颜色 clear 会**覆盖**背景：场景里有生效的 `render.Camera3D` clear 时看不到 shader pass
  （本通路只画背景，不做叠层）。
- **每个实体一份全屏 pass**（一次全屏 draw）；需要多 pass 串联/反馈 buffer（上一帧、多 buffer 合成）
  用下一节的 `render.ShaderGraph`（`render.ShaderPass` 保持单 pass 的简单契约不变）。
- 资源侧限制：**没有 `iChannel*` 纹理**（不能采样音频/上一帧/其它贴图）；**没有 depth 测试与混合状态**，
  输出按不透明背景色处理。
- **不热重载 `.wgsl`**：改 shader 需重启或重新加载场景才会生效。

### 示例：Shadertoy demo 集（场景资源）

`scenes/shadertoy/` 用两个 Shadertoy 名作验证这条通路（均为 640x360，`render.ShaderPass` 单实体）：

- `seascape.scn` + `seascape.wgsl`：Shadertoy **Ms2SD1 "Seascape"**，作者 **Alexander Alekseev (TDM)**，
  2014；许可 **CC BY-NC-SA 3.0**（署名—非商业性使用—相同方式共享）。画面天空亮、海面暗且偏青。
- `mandelbulb.scn` + `mandelbulb.wgsl`：Shadertoy **MdXSWn "Mandelbulb"**，作者 **Inigo Quilez**，
  2013；许可 **CC BY-NC-SA 3.0**（参考其 SIGGRAPH 2012 WebCL 课程实现，作者授权）。
  画面为光线步进分形 + 渐变背景。

回归测试：`ctest -R shadertoy_e2e`（逐 demo：640x360 离屏结构断言 + frame 1/60 差异 +
同帧逐字节确定性 + WGSL 属场景资源而非内建 shader；单项 demo 缺失时该节 SKIP，不阻塞）。

## 场景自定义多通道 shader 图（render.ShaderGraph）

`render.ShaderPass` 只有一份 uniform、**没有纹理输入**，所以只能单 pass。需要「上一帧结果 / 中间缓冲 /
多阶段串联」时用 **`render.ShaderGraph`**：一个实体挂一份 **program JSON**，声明若干中间缓冲与有序阶段，
引擎每帧按声明顺序执行，最后画到屏幕的阶段就是背景层（先于 3D/2D）。

> **命名**：这里的 "graph" 指 **shader pass 之间的图**（阶段通过通道引用缓冲形成 DAG），
> 不是节点式可视化编辑器，也不是整帧渲染调度器（没有资源别名 / 自动依赖分析 / culling）。

### 组件字段（`render.ShaderGraph`）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `program` | string | `""` | program JSON 路径，**相对场景目录**（运行时 VFS 根 = 场景目录） |
| `speed` | float | `1.0` | `time` 推进倍率 |
| `paused` | bool | `false` | `true` = 冻结时间（仍绘制） |
| `enabled` | bool | `true` | `false` = 不执行任何阶段 |
| `time` | float | `0.0` | 运行期推进的秒数；所有阶段共享同一个 `time`（= Shadertoy `iTime`） |

> 只画在 2D 精灵矩形里时不用本组件：改用 `render.ShaderSprite` 并把 `shader` 指向 program JSON
> （见 §精灵形态）。

### program JSON

```json
{
  "buffers": [
    { "name": "a", "size": 540 },
    { "name": "b", "size": 540 }
  ],
  "passes": [
    { "name": "buffer_a", "shader": "buffer_a.wgsl", "common": "common.wgsl",
      "target": "a", "channels": ["self", "", "", ""] },
    { "name": "image", "shader": "image.wgsl", "common": "common.wgsl",
      "target": "screen", "channels": ["a", "", "", ""] }
  ]
}
```

| 字段 | 说明 |
|------|------|
| `buffers[].name` / `size` | 中间缓冲名（`channels` 引用它）与正方形边长（像素） |
| `passes[].shader` | 阶段 WGSL；**相对 program 文件所在目录**，失败只跳过该阶段并报错 |
| `passes[].common` | 可选：共享源码，引擎把它**拼在 shader 之前**（相当于 Shadertoy 的 Common 标签页） |
| `passes[].target` | 输出缓冲名；`""` / `"screen"` / `"main"` = 主渲染目标（背景层） |
| `passes[].channels` | 4 个 `iChannel`：缓冲名；`"self"`/`"prev"` = 本 pass 目标缓冲的**上一帧**内容；`""` = 未绑定 |

**缓冲语义**：每个缓冲两张 RGBA32F 纹理（ping-pong）。本帧写 `tex[write]`，读上一帧的 `tex[1-write]`；
同一帧内引用别的缓冲时读它**本帧最新**的结果（前面的阶段先写），所以阶段可以串联。ping-pong 每模拟帧
只交换一次（与成帧次数无关）。

### WGSL 阶段契约

引擎供全屏三角形 VS，阶段只提供 fragment 入口：

```wgsl
struct MqShaderGraphPass {
    resolution: vec2f,        // 本阶段渲染尺寸（像素）：缓冲边长或主目标尺寸
    time: f32,                // 组件 time（秒）
    frame: f32,               // 帧号
    mouse: vec4f,             // xy = 指针像素（原点左上，**屏幕空间**）；z = 按下(1)/松开(0)；w 预留
    keys: vec4f,              // x = Backspace，y = Shift，z = Enter（0/1）；w 预留
    screen_resolution: vec2f, // 主目标尺寸：mouse.xy 所在的空间
    pad: vec2f,               // 对齐填充
};
@group(0) @binding(0) var<uniform> mq_pass: MqShaderGraphPass;
@group(0) @binding(1) var ch0: texture_2d<f32>;   // iChannel0
@group(0) @binding(2) var ch1: texture_2d<f32>;   // iChannel1
@group(0) @binding(3) var ch2: texture_2d<f32>;   // iChannel2
@group(0) @binding(4) var ch3: texture_2d<f32>;   // iChannel3
@group(0) @binding(5) var mq_sampler: sampler;    // non-filtering（float32 目标不可过滤）

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f
```

- uniform 合计 **64B**（`resolution` 8 + `time` 4 + `frame` 4 + `mouse` 16 + `keys` 16 +
  `screen_resolution` 8 + 8 填充）。
- `mouse.xy` 是**屏幕**（主目标）像素，而中间缓冲的 `resolution` 通常更小（正方形）；
  要用指针构造射线/命中位置时取 `screen_resolution` 做分母，否则中间缓冲阶段会算偏
  （`scenes/shadertoy/mountains/buffer_a.wgsl` 的注释就是这条）。
- 中间缓冲是 **RGBA32F**：WebGPU 不能对 float32 纹理做硬件过滤，所以 `channels` 的采样器是
  nearest —— 需要双线性的 shader 自己用 `textureLoad` 插值（`scenes/shadertoy/mountains/common.wgsl`
  的 `textureSH` / `texelFetchSH` 就是这套封装）。
- **坐标约定**：`frag_coord` 原点左上（与 `render.ShaderPass` 相同）；Shadertoy 风格（原点左下）在
  入口翻 y 即可。纹理 v 方向同理——Buffer 通道按「Shadertoy 的 v=0 在下」读写时，采样要翻 y。
- 未绑定的通道绑 1×1 占位纹理（RGBA32F 0），shader 仍要声明全部 5 个绑定。
- 失败语义：program 缺失/JSON 非法/某阶段编译失败 → stderr 明确报错；全部阶段失败时该图不生效，
  场景其余部分照常运行。

### 精灵形态：`render.ShaderSprite` 的 `shader` 指向 program JSON

同一份 program 也可以只画在一枚 **2D 精灵矩形**里——**用 `render.ShaderSprite`，把 `shader` 写成
program JSON 的路径即可**（引擎按扩展名判定：`.wgsl` = 单 pass 契约，`.json` = 多阶段图）。
场景里**只需要那一个实体**，不需要额外的 backdrop 或第二个组件：

```json
{
  "name": "mountain_sprite",
  "components": [
    { "type": "scn.Transform2D", "x": 480, "y": 270 },
    { "type": "render.ShaderSprite", "shader": "mountains/program.json",
      "width": 720, "height": 405 }
  ]
}
```

- 精灵的位置/尺寸/旋转/透明度/图层序全部沿用 `render.ShaderSprite` 的既有语义（与 `scn.Sprite` 一致）；
  `width`/`height` 是设计像素，节点 `scale` 再乘。
- 整条阶段链先渲染到该精灵的离屏纹理（RGBA8，像素尺寸 = 设计尺寸 × 节点 scale × 2D 视图缩放），
  再由 2D 通路合成。
- **阶段链完全一致**：中间缓冲仍是 RGBA32F、仍做 ping-pong；只有 `target` 为主目标的那一步
  改成写精灵离屏纹理。因此同一份 program.json 可以同时用于全屏场景与精灵场景。
- 精灵的最终阶段拿到的 uniform：`resolution` / `screen_resolution` = 离屏纹理**像素尺寸**，
  `mouse.xy` = 精灵内局部像素（原点左上）；中间缓冲阶段仍拿设计尺寸与设计像素，
  两者只差一个统一比例，射线/命中计算不受影响。
- 指针只在矩形内生效（矩形外 `mouse.z = 0`、坐标 0），所以绘制不会"漏"到精灵外面。
- 受 F4「2D/UI 图层」开关控制（与普通 shader 精灵一致）。
- `render.ShaderGraph`（`program` 字段）是全屏背景形态；两者共享同一套 program 格式与阶段 ABI。

### 绘制顺序与限制

- 各阶段按数组顺序在**同一队列提交**里依次开 render pass；`target` 为主目标的阶段是背景层
  （先于 3D / 2D）。纯 shader 图场景（无任何 3D 组件）同样可用。
- 阶段**不混合、不清除历史**：每个阶段开始前清成 0，整阶段覆盖写（与 Shadertoy 的 buffer 语义一致）。
- 中间缓冲尺寸在装配期创建（program 里的 `size`），运行期不随窗口 resize 重建；主目标阶段的
  `resolution` 跟随渲染目标像素。
- 阶段数量/通道数上限：每个图没有硬上限（按声明执行）；单阶段 4 个通道（WebGPU 的 `iChannel` 同款）。
- **不热重载** `.wgsl` / program.json：改完需重新加载场景。

### 示例：Shadertoy "Mouse-Paint Eroded Mountains"（`scenes/shadertoy/mountains.scn`）

- 原作：**sf23W1 "Mouse-Paint Eroded Mountains"**，作者 **Rune Skovbo Johansen**，2026；
  Phacelle Noise / 侵蚀滤波为 **MPL 2.0**，地形渲染派生自 Fewes 的 `7ljcRW`。
- 4 个阶段：`buffer_a`（鼠标绘制的高度图，自反馈 = `channels[0] = "self"`）→ `buffer_b`
  （侵蚀滤波，含 `pack4` 打包数据到 alpha）→ `buffer_c`（细节噪声）→ `image`（光线步进渲染）。
- program 在 `mountains/program.json`，阶段 WGSL 与 `common.wgsl` 同目录，全部是场景资源。
- 交互：鼠标左键绘制山体（Shift 反向 = 下凹），Backspace 重置，Enter 开关侵蚀（对应原作的键盘纹理）。
- 同一份 program 还用于精灵场景 `mountain-shader-sprite.scn`：单个实体挂
  `render.ShaderSprite`、`shader` 写 `mountains/program.json`、720×405，
  用来验证"精灵形态与背景形态共享同一条阶段链"。

## 场景自定义 shader 精灵（render.ShaderSprite）

`render.ShaderPass` 只能铺满整屏；**`render.ShaderSprite`** 把同一个 WGSL 画进**节点所在的矩形**
——一枚由 shader 出图的 2D 精灵。典型用途：程序化头像/传送门/水面窗口/分屏对比，一个场景里放多块。

```json
{
  "name": "left",
  "components": [
    { "type": "scn.Transform2D", "x": 160, "y": 180 },
    { "type": "render.ShaderSprite", "shader": "mandelbulb.wgsl", "width": 312, "height": 344 }
  ]
}
```

### 组件字段（`render.ShaderSprite`）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `shader` | string | `""` | **`.wgsl` 源文件**或**多阶段 program JSON**，**相对场景目录**（与 `render.ShaderPass` 同一套 VFS 语义）。引擎按扩展名判定：`.json` = shader 图（多阶段/自反馈），其余按 WGSL 单 pass 编译 |
| `width` / `height` | float | `64` / `64` | 精灵尺寸（**设计像素**，单位 px）；节点 `scale` 再乘上去 |
| `speed` | float | `1.0` | `time` 推进倍率 |
| `paused` | bool | `false` | `true` = 冻结时间（仍绘制） |
| `enabled` | bool | `true` | `false` = 不绘制 |
| `time` | float | `0.0` | 运行期推进的秒数；脚本可读写 |

### WGSL 契约：与全屏 pass **完全一致**

同一份 `.wgsl` 既可以是 `render.ShaderPass`（铺满整屏）也可以是 `render.ShaderSprite`（只占矩形），
不需要改一行 shader：入口仍是 `@fragment fs_main`，uniform 仍是 `group(0) binding(0)` 的 32B 结构。
区别只在 `resolution` 的含义：

| 通路 | `resolution` | `frag_coord` 原点 |
|------|--------------|-------------------|
| `render.ShaderPass` | 渲染目标像素尺寸 | 渲染目标左上 |
| `render.ShaderSprite` | 该精灵**离屏纹理**像素尺寸（= 设计尺寸 × 视图缩放） | 该纹理左上（精灵内部坐标） |

`mouse.xy` 同理是**精灵内局部像素**（指针不在精灵内时为 0）。因此 Shadertoy 的
`fragCoord / resolution` 归一化在两种通路里都成立。

`shader` 指向 program JSON 时走的是多阶段通路（阶段契约换成 `render.ShaderGraph` 的 64B uniform +
4 个通道），精灵矩形/离屏纹理/合成方式完全不变 —— 细节见 §多通道 shader 的"精灵形态"。

### 绘制方式与限制

- 两阶段：**① 每个精灵渲染到自己的离屏 RGBA8 纹理**（尺寸 = 设计尺寸 × 2D 视图缩放，窗口放大不糊）
  → **② 作为带纹理矩形并入 2D 图元批次**。所以位置/旋转/节点缩放/画布 letterbox/图层 z 序
  全部复用既有 2D 通路；`scn.Sprite` 怎么摆，它就怎么摆。
- 与普通 2D 图元**同层排序**（`layer` 0 世界层，`z` = 实体 id 稳定序）；不受 3D 相机 clear 影响
  （它在 2D 图层，绘制于 3D 之后）。
- **time 推进进确定性**：`time += dt * speed`（模拟 dt），与全屏 pass 一致；快照/回放可读 `time`。
- 限制：纹理只有 RGBA8、无 depth/混合状态（输出按不透明）；精灵内容**不支持旋转后的像素级重采样**
  （旋转的是承载它的矩形，shader 看到的仍是自己的 uv 网格）；**不热重载 `.wgsl`**。
- 每个精灵一张离屏纹理 + 一次全屏 draw；同屏几十个以上时先量性能（可用 `MAQUE_TRACE_STATS=1`）。

### 示例：分屏对比（`scenes/shadertoy/sprite-shader.scn`）

同一个 640x360 场景左右并排两块精灵：左 `mandelbulb.wgsl`、右 `seascape.wgsl`，中间留 16px 中缝
（露出场景背景色）。回归：`ctest -R shadertoy_e2e` 的 `[sprite]` 一节——两条加载行、左右区域
颜色多样且**左暖右冷**（证明是两个不同作品）、中缝是背景色（精灵没有铺满整屏）、frame 1/60 有动画、
同帧两次逐字节一致。

### 示例：精灵 → 2D RenderTexture → cube 表面（`scenes/render_texture/shader-sprites-cube.scn`）

`shader sprite` 是普通 2D 图元，因此**2D 相机驱动的 RenderTexture 会把它一起捕获**（3D RT 不会，
它只按 `include_ui` 叠 UI 层）。这条链路可以做成"cube 正面一块屏幕"：

```
两个 render.ShaderSprite（layer 0 世界层）
  → render.Camera2D(layer_mask: 1) + render.RenderTexture(640x360, camera=2d)
  → asset.PbrMaterial{ shading_model: 2（Basic/unlit）, base_color_texture: "rendertexture://sprites" }
  → 贴在 cube 正面的 plane 上（position z=0.6075 避免 z-fighting）
```

场景里有两个工程细节：

- **让精灵只出现在 RT 里**：主画面的 2D 覆盖层会绘制所有 2D 图元，没有办法"只画到 RT"。
  这里把精灵摆到画布外（`y=1180`，超出 `Scene.height`），主视图的 scissor 会裁掉它们；
  2D 相机摆到同一区域（`x=320, y=1180, zoom=1`）就能把它们 1:1 收进 640x360 的 RT。
  `render.RenderTexture` 的 `update_mode: 0`（EveryFrame）+ 精灵 `time` 推进 → 屏幕内容逐帧动。
- **shader 必须是场景自带的副本**：VFS 有 `..` 穿越防护（docs/04 §9），跨场景引用不成立，
  所以 `scenes/render_texture/assets/shaders/` 放了 `scenes/shadertoy/` 两个文件的**逐字节副本**；
  `render_texture_e2e` 用 `cmp` 守住两份副本不漂移。

回归：`ctest -R render_texture_e2e` 的 `shader sprites -> RenderTexture -> cube face` 一节
（RT 创建日志 + 两条精灵加载行 + 屏幕里暖/冷像素都够 + 顶部是背景 + 场景副本与源一致）。

## 后处理（PostEffect）

**主输出阶段的后处理**：把 3D 层（场景 shader pass → 天空/3D）过一遍全屏 shader，再把 2D/UI 叠在
结果之上。它是**引擎级**（F4 引擎选项 / `MAQUE_ENGINE_OPTIONS`）的**视图层**效果——不写场景组件、
不进快照/校验和、不参与模拟，**默认关闭**。

### 作用范围（默认只叠加 3D）

| `frame3d.post.include_2d` | 行为 |
|---|---|
| **0（默认）** | 只处理 **3D 层**（场景 shader pass + 3D + 天空）；3D 先画进中间目标 → post → 目标，**2D/UI 再原样叠在 post 结果之上**（UI 的颜色/清晰度不受曝光、暗角、色差、Bloom 影响）。纯 2D 场景（没有 3D 组件/shaders pass）**不跑 post**（零成本） |
| 1 | 整帧（3D + 2D/UI 一起）后处理；纯 2D 场景也可以用（F4 勾「包含 2D / UI」/ `post_2d=1`） |

### Pass 顺序

```
默认（include_2d=0）：
  3D 层（render.ShaderPass → 3D PBR/阴影/天空）
    → 中间目标 postSrc（RGBA8Unorm，RenderAttachment|TextureBinding|CopySrc）
    → post 全屏 pass（内建 shader 'post'，64B uniform + 1 张贴图 + 线性 clamp 采样器）
    → 真正目标（swapchain / 离屏回读纹理 / 缩放用中间目标）
    → 2D/UI 覆盖层（图元/网格，叠在 post 结果上）

include_2d=1：
  3D 层 + 2D/UI → 中间目标 → post → 真正目标
```

- **关闭时零成本**：`frame3d.post.enabled == 0` → 不建中间目标、不建管线、不跑 pass，
  场景直接画进目标（旧调用方输出**逐字节不变**）。
- 中间目标始终带 `CopySrc`：transmission 的 scene-color 拷贝、`--shot` 离屏回读、`render_scale≠1`
  的缩放 blit 都从它拿源。
- **RenderTexture（相机离屏纹理）不叠后处理**：后处理是**主输出**阶段的效果；若叠到每张相机纹理上，
  同帧被采样的 RT 会出现"效果被采样两次"。场景相机的离屏图保持原始画面。

### 效果（`frame3d.post.effects` 位掩码）

| bit | 名称 | 参数 | 说明 |
|---|---|---|---|
| 0 | Bloom 泛光 | `bloom_intensity` / `bloom_threshold` / `bloom_radius` | 阈值亮度 × 12 个黄金角螺旋采样累积；`bloom_radius` = 目标高度的比例（图像无关），单 pass 近似（无 mip 链降采样） |
| 1 | 暗角 | `vignette` | 归一化半径 `smoothstep(0.55, 1.45)` 压暗边缘，中心不动 |
| 2 | 色差 | `aberration` | R/B 沿"离屏幕中心"的方向反向偏移（G 不动） |
| 3 | 色彩分级 | `saturation` / `contrast` / `grayscale` | 饱和度 → 对比度（绕 0.5）→ 灰度混合 |

另有两个"总是执行"的阶段（不占 bit）：`exposure`（曝光乘子）与 `tonemap`（0=none / 1=ACES）。
**色彩空间 = 已成像的显示空间**：3D 在 pbr shader 里已做过 ACES + sRGB、2D/UI 本来就是显示值，
所以这里的曝光/ACES 是**在成片上**再叠一层（F4 调试语义），不是 HDR 管线里的 tonemap。
真正的 HDR 中间目标 + 后置 tonemap 属后续工作。

### 确定性

采样序列是**固定**的（12 个黄金角采样点、不读时间/随机/uav），同一帧输入 + 同一组参数 →
输出逐字节一致（`ctest -R post_effect_e2e` 断言同选项两次运行逐字节相同）。

### 开关与剔除

- F4 面板「后处理 PostEffect」：启用、**包含 2D / UI**（默认关 = 只叠加 3D 层）、色调映射、曝光、
  Bloom（开关/强度/阈值/半径）、暗角（开关/强度）、色差（开关/强度）、色彩分级（开关/饱和度/对比度/灰度）。
- 启动初值：`MAQUE_ENGINE_OPTIONS="post=1,bloom=1,bloom_intensity=1.5,tonemap=aces,exposure=1.2"`
  （键见 docs/28；出现任何后处理键会自动打开总开关，显式 `post=0` 则关闭；`post_2d=1` 才含 2D/UI）。
- 剔除：`-DMAQUE_SHADER_POST=OFF` → 不生成/不链接该 shader；gfx 取不到时打印一行原因并**回退到
  无后处理画面**（不黑屏、不静默画错）。

回归测试：`ctest -R post_effect_e2e`（默认关闭逐字节零回归 / identity / 曝光·ACES / 暗角 /
灰度 / Bloom 光晕 / 色差 / 2D/UI 默认不受影响 / `post_2d=1` 整帧 / 同选项确定性 /
F4 面板共存 / 无 ASan）。
