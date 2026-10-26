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

- **按需栅格化**：图集（每 字体 × 字号 一张）启动时只预热 ASCII 95 字形，其余码点在**排版时**惰性
  栅格化并按行打包（宽度固定 512、高度按需增长到 1024；确定性：追加顺序 = 码点出现顺序）。
  `mq_font_atlas_generation` 每次新增字形自增 → 宿主据此决定纹理是否重传。
- **两阶段提交**（必须）：① 所有标签先 measure+layout（把该帧需要的字形全部预热进图集）；
  ② 再按 (字体,字号) 刷新纹理，并按**最终图集尺寸**计算 UV 后提交图元。
  否则先排的标签会用"排版时"的图集尺寸算 UV，而纹理随后变大 → 采样错位（表现为文字重叠）。
- **纹理生命周期**：图集增长后重建纹理，**旧纹理延迟到下一帧**释放（同帧已排队的图元仍引用旧 id）；
  渲染器需容忍已释放的纹理槽（`destroy_texture` 置空，采样时按"无贴图"处理）。
- **栅格化倍率 = 渲染目标像素 / 设计像素**（当前实现 = 1，因为每个视图的目标就是设计分辨率）。
  窗口放大由 present 完成（整倍 → 最近邻，保持锐利）。**不要**按窗口缩放栅格化：
  3× 窗口下出 36px 字形再画进 320×240 的目标 = 降采样 3 倍 → 笔画丢失、文字发虚/乱码。
- **像素吸附必须作用在"图元中心"**：吸附在顶点着色器里执行，若逐顶点吸附
  （每个顶点都拿自己当中心）会使整个矩形偏移半像素 → 1:1 nearest 下部分行列取错纹素。
  正确做法：中心变换到目标空间 → 吸附 → 再加顶点偏移（`px = pc + kc`）。
- **范围**：UTF-8 码点级排版；CJK 逐字可断行；缺字形静默跳过（不画豆腐块）。

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
- 材质 uniform 256B（64 floats，plans/19 单一定义 + gfx 统一打包）；camera uniform 784B（含 view 矩阵，供 matcap）。
- 无贴图时的缺省绑定：颜色/参数用 1×1 白，法线用 1×1 中性法线 (0.5,0.5,1)（避免法线扰动）。
- 详细字段/映射/非目标见 plans/19-threejs-materials.md；glTF 扩展由 `tools/gltf_import.py` 导入。
