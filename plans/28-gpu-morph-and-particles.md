# 28 · GPU morph target 动画 与 3D 粒子批渲染（设计 + 路线图）

> **实现状态（2026-11）**：本文 §3（GPU morph：morph 增量 storage + 逐 draw 权重 + shader
> `vertex_index` 累加）与 §4 的 **M1/M2**（48B 粒子实例 + `particle_system` 批次 + gfx 实例化
> billboard 管线）已在当前代码落地，回归 `gltf_morph_e2e` / `particles3d_e2e` / `boids_e2e`；
> **未做**：§4 的 GPU 解析模拟（stateless、默认关、不进 checksum）与 CUBICSPLINE 精确切线。
> 下游写实现时以代码为准，本文保留设计与验收口径。
>
> 上游：[21](21-gltf-animation-and-compression.md)/[22](22-gltf-animation-implementation.md)（glTF 蒙皮/动画/morph 落地）、
> [23](23-runtime-gltf-loading.md)/[24](24-runtime-gltf-capabilities.md)（运行时直读与能力矩阵）、
> [27](27-threejs-feature-migration.md)（M1-1 图元 / M1-2 雾 / M1-3 色调映射 / **M1-4 3D 粒子 MVP**）、
> [14](14-particle.md)（particle 模块计划）、[18](18-pbr-3d.md)（PBR/3D 与直接呈现）。
> 约束来源：`docs/02`（C ABI/版本化）、`docs/10`（确定性/固定步长/引擎 RNG）、`docs/13`（WebGPU 渲染）、
> `docs/29 §10`（Tracy 显存域）、`docs/30 §5`（"视觉状态不进 checksum"的既有先例）。
>
> **基线说明（写作时）**：仓库工作区包含 plans/27 M1-1～M1-4 的未提交改动——程序化图元、
> 场景雾/色调映射、以及 **3D 粒子的 CPU MVP**（`render.ParticleEmitter3D` + `systems/particles.cpp`
> 的 `particles3d_step` + `extract3d.cpp` 的"每发射器一张动态 billboard 网格"），
> 配套已有样例 `scenes/pbr/particles3d.scn`、e2e `tests/particles3d_e2e.sh`、内置软边纹理
> `builtin:/textures/particle_soft`（`assets/texture.cpp:527` 程序化生成）。
> 本文以**该状态**为基线：morph 仍是纯 CPU；3D 粒子有组件、有 CPU 模拟、有样例与 e2e，
> 但**没有实例化、没有专用管线**。
>
> **性质**：设计 + 路线图，不是又一份接口声明。所有 schema/ABI 建议一律**尾部追加 + 默认值等于旧行为**；
> 本文不要求改任何现存字段语义。

## 0. 结论（评审先看这十条）

1. **morph 可以 GPU 化，且不需要把 renderer 升到 v5。** 关键是把"逐 draw 的权重"放进
   `frame3d` **尾部**的 `const float*`（与 `skins` 完全同构），**不要**动 `draw3d` 或
   `pbr_material` 这两个**数组元素**的字段（那会让 stride 变化 = 破坏性变更）。
   逐网格的增量走新 record `mq_renderer_morph`，挂在 `mesh_gpu` 尾部（单结构体，安全）。
2. **增量只能进 storage buffer（group1 binding 24），不能进顶点属性。**
   `create_mesh` 按 `vertex_layout` 硬校验 stride（48/56/72/80），新增属性 = 新 layout + 新 stride，
   属于破坏性变更；而且 morph 增量是"每网格 8 份顶点集"，常驻显存比逐帧上传更合理。
3. **不新增顶点入口。** 现有 4 个主入口（`vs`/`vs1`/`vs_skin`/`vs1_skin`）与 4 个阴影入口
   （`vs_shadow*`）里加一个**uniform 守卫**的 morph 块（`dr.morph0.x > 0` 才进循环），
   非 morph draw 的代价 = 一次 uniform 比较 + 一个分支。
   这避免把管线缓存 `p3dMatPipeline[fmt][msaa][model][blend][side][mir][layout]` 再乘 2（→ 4608 项）。
4. **每帧上传量从 O(顶点数 × stride) 降到 O(draw 数 × 32B)。** 例：1 万顶点、stride 72 的 morph 角色，
   现状每次权重变化 = 720KB 顶点重传；GPU 路径 = 8 个 float 权重。CPU 侧的逐顶点累加整段消失。
5. **权重是"每 draw"而不是"每材质"。** 顺带修掉当前 CPU 路径"共享 mesh 只能首实体驱动"的限制
   （`extract3d.cpp:606-613`、`gltf_anim.cpp:731-739`）：同一 glTF 实例化两次、各自 `time` 不同时，
   GPU 路径天然正确。
6. **上限 8 个 target**（与离线 importer `MORPH_TARGET_LIMIT = 8` 一致）。超过 8 或渲染器
   `caps.morph_max_targets == 0` → 走**既有 CPU `update_mesh` 路径**；`MAQUE_MORPH=0` → 完全不应用 morph
   （基础网格，与今天的语义一致）；两条路径可同帧共存（按 primitive 选择），但同一 primitive 不混用。
7. **Transform（节点 TRS）动画不做 GPU。** 节点世界矩阵在 CPU 侧有 4 个消费者（蒙皮调色板、`draw3d.model`、
   OBB 拾取、RenderTexture 相机），GPU 化必须回读 → 同步点 + 1 帧延迟 + 跨 GPU 浮点差异；
   而 CPU 采样是"几十个节点 × 每通道一次二分查找 + lerp/slerp"，µs 级。
   GPU 真正值得做的是**实例化人群**（per-instance TRS 或 GPU 骨骼），见 §3.10。
8. **3D 粒子现状是"能看但不对"**：每发射器一张 `max_count` 容量的动态网格、每帧整段
   `update_mesh` 上传、按容量（`cap*6` 索引）绘制、且**未进 checksum/快照**、未参与
   `visual_dynamic` 空闲跳帧判定、透明排序用 identity model（排序距离恒为原点）。见 §1.5 的 G5–G9。
9. **粒子默认 CPU 确定性模拟 + GPU 实例化渲染**（进 checksum/快照，可回放）；
   **GPU 解析模拟（stateless）必须 opt-in、不进 checksum、不进快照**，
   与 `docs/30 §5` 音频 playhead 同一策略。默认路径不允许出现"换机器画面不同"。
10. **验收 = 新 e2e + 旧场景零回归 + 三门禁**（`codegen_drift` / `schema_imports` / `builtin_shaders`）
    + Windows 实机与 WASM headless 冒烟。不接受"只看能编译"。

## 1. 现状基线

### 1.1 CPU morph（要替换的部分）

| 环节 | 代码 | 事实 |
|---|---|---|
| glTF 解析 | `modules/runtime/src/gltf_model.cpp`、`gltf_model.h:146-172` | 每个 primitive 存 `morph_targets[{positions, normals}]` + `morph_weights`（默认权重） |
| glTF 上传 | `assets/gltf.cpp:432-533` | `create_mesh` 后把打包顶点另存一份 `pr.base`；有默认权重就调一次 `apply_gltf_morph` |
| `.mesh.json` 上传 | `assets/texture.cpp:577-733, 772-793` | `morph:{default_weights,targets}` → `MorphMesh{base, targets, ...}`；targets ≤ 8 |
| 每帧驱动（glTF 直读） | `assets/gltf_anim.cpp:721-743` | 动画 `weights` 通道 → `apply_gltf_morph`；`applied_weights` 不变则跳过 |
| 每帧驱动（mesh.json） | `render/extract3d.cpp:598-615` | `morph_weight_override`（动画）> 默认；权重变了才 `apply_morph` |
| 重算 | `gltf_anim.cpp:216-245`、`487-519` | 逐顶点 × 逐 target 累加 pos/nrm，normal 归一化；**每次都从 `base` 整段复制** |
| 上传 | `rapi->update_mesh`（ABI 尾部方法） | 全量顶点缓冲重传（`vertex_bytes` 必须与创建时一致） |
| 共享网格 | `MorphMesh::owner_entity` / `Prim::morph_owner` | **首实体驱动，其余实体忽略并告警一次** |
| 开关 | `MAQUE_MORPH=0` | 不应用 morph（基础网格，A/B 对照） |

### 1.2 GPU 蒙皮（可复用的机制，morph 要贴着它做）

| 机制 | 位置 | 说明 |
|---|---|---|
| 调色板 storage buffer | `assets/builtin/shaders/pbr/00_types.wgsl:99-101` | `@group(1) @binding(23) var<storage, read> skin_palette: array<mat4x4f>`，Vertex 阶段 |
| 绑定布局 | `gfx.cpp:813-831` | group1 共 24 项：0=uniform(336B)、1..22=贴图/sampler、23=只读 storage（minBindingSize 64） |
| 逐 draw 槽位 | `gfx.cpp:3749-3781` `ensure_draw_slot` | 每个 draw 一份 uniform buffer + bind group + 调色板 buffer；复用键 = 材质贴图句柄 + joint 数 |
| 占位 buffer | `gfx.cpp:1150-1158` `dummySkinBuf`（64B 单位 mat4） | 非蒙皮 draw 也绑，shader 不读 |
| 阴影 pass | `gfx.cpp:4215-4246` | 与主 pass 共用 `fill_material_bind_entries` + `ensure_draw_slot`，同一 binding 23 |
| ABI | `renderer.yaml` 的 `skin` record + `draw3d.skin` + `frame3d.skins` | 每帧由 runtime 生成，**不进快照/校验和** |

### 1.3 3D 粒子（MVP，要改造渲染路径）

| 环节 | 位置 | 事实 |
|---|---|---|
| 组件 | `render/render.yaml:281-299`（`render.ParticleEmitter3D`） | 字段：`emitting/rate/lifetime/speed/spread/direction/gravity/size_start/size_end/color_start/color_end/rotation/rotation_speed/texture/max_count` |
| 模拟 | `systems/particles.cpp:65-141` `particles3d_step` | 固定步长、引擎 RNG、定点发射累加（`rate*dt*1024`）、每发射器 `max_count`、全局硬编码 4096 |
| 粒子状态 | `runtime_engine.h:63-74` `SystemsRuntime::Particle3D` | 世界坐标 + 速度 + 重力 + age/life + size0/1 + rot/rot_speed + c0/c1 |
| 渲染 | `extract3d.cpp:630-785` | **每发射器一张 56B 布局动态网格**（`cap` 个 quad）、CPU 排序、逐帧写满整个容量、`update_mesh` 全量上传 |
| 材质 | `extract3d.cpp:747-773` | 每发射器 push 一条 `Basic` + `alpha_mode=2` + `DoubleSide` 材质，顶点色承载颜色/alpha |
| 绘制 | `extract3d.cpp:775-783` | `dr.model = identity`（顶点已是世界坐标）、`skin=-1`、`layer=-1`、追加到 `out_draws` |
| 阴影 | `gfx.cpp:4222-4225` | `alpha_mode==2` 不投影 → 粒子不投影（符合预期） |
| 开关 | `MAQUE_PARTICLES=0` | **只关渲染**，模拟继续（checksum 不受开关影响） |
| 清理 | `systems/nav.cpp` `clear_asset_caches` | 销毁动态网格 + 清 `particles3d` / 累加器 |

### 1.4 与本文相关的 ABI 事实

| 事实 | 值 | 影响 |
|---|---|---|
| 渲染接口版本 | `renderer.yaml: version: 4`、`abi_name: maque.renderer.v4`、`MQ_RENDERER_API_VERSION 4u` | 插件按**精确版本**查询（`mq_module_query`），旧插件不会静默加载 |
| 数组元素记录 | `pbr_material` 352B、`draw3d` 104B、`view3d` 184B、`light` 64B | gfx 用 `static_assert` 守尺寸；**改这些 = stride 变化 = 必须升 v5** |
| 单结构体记录 | `mesh_gpu`、`frame3d`、`caps`、`environment`、`post`、`atmosphere` | 只在尾部追加 → 旧调用方读旧前缀，安全 |
| group1 绑定 | 0..23 已用，**24 空闲** | morph 增量放 binding 24 |
| 材质 uniform | `kMatUniformFloats = 84`（336B），`pbr/00_types.wgsl` 的 `Draw` = 21 个 vec4 | morph 权重需要在此尾部加 3 个 vec4（→ 96 floats / 384B） |
| 相机 uniform | `float cam[236]` = 944B；`view` 在 float 180..195，`eye` 在 16..19 | 粒子 shader **不复用**这份结构（见 §4.5） |

### 1.5 现状问题清单（本文要逐条关掉）

| 编号 | 问题 | 证据 |
|---|---|---|
| **G1** | morph 每次权重变化都 `memcpy(base)` + 逐顶点重算 + 全量 `update_mesh` | `gltf_anim.cpp:216`、`487` |
| **G2** | 共享网格只能首实体驱动 morph | `extract3d.cpp:606-613`、`gltf_anim.cpp:731-739` |
| **G3** | glTF 直读没有 target 上限（importer 是 8） | `gltf_model.cpp` 无 cap；`gltf_import.py:984` 有 |
| **G4** | `update_mesh` 是同步整段 copy；WASM/移动端是带宽热点 | `gfx.cpp` `update_mesh` 实现 |
| **G5** | 粒子按**容量**绘制（`cap*6` 索引）且 `mesh_stats` 记 `cap*4/cap*6` → draw 统计与三角形数虚高 | `extract3d.cpp:690` |
| **G6** | 3D 粒子**未进** `world_checksum` / `world_snapshot`（2D 粒子进了） | `scene/world_state.cpp:21-32, 172-184` |
| **G7** | `visual_dynamic` 只判 `systems.particles`，不判 `particles3d` → 空闲跳帧可能冻结粒子 | `engine/frame.cpp:544-545` |
| **G8** | 粒子 draw 用 identity model，gfx 透明排序按 `model[12..14]` 的距离 → 粒子恒以原点参与排序 | `gfx.cpp:4017-4027` |
| **G9** | 每帧上传 `4 顶点 × 56B = 224B/粒子`；实例化只需 `48B/粒子` | `extract3d.cpp:671, 741-743` |

## 2. ABI 判定：能不能不动版本？

**结论：两个特性都能在 renderer v4 内完成**，前提是新增内容全部落在"单结构体尾部"或"新 record"上。

| 改动 | 记录类型 | 是否数组元素 | 判定 | 说明 |
|---|---|---|---|---|
| 新增 `mq_renderer_morph` | 新类型 | — | ✅ 安全 | 只是给 `mesh_gpu` 用；声明顺序需在 `mesh_gpu` 之前（C 头需要完整类型） |
| `mesh_gpu.morph` | 单结构体（指针传入） | 否 | ✅ 安全 | 旧插件读旧前缀；`create_mesh` 忽略新字段 = 不上传增量 |
| `frame3d.morph_weights` / `morph_weight_count` | 单结构体 | 否 | ✅ 安全 | 与 `skins` 同构；旧插件忽略 → 画基础网格 |
| `caps.morph_max_targets` | 单结构体（out） | 否 | ✅ 安全 | 能力探测：0 = 不支持 → runtime 走 CPU |
| 新增 `mq_renderer_particle` / `mq_renderer_particle_system` | 新类型 | — | ✅ 安全 | 与 `shader_sprite` 同类 |
| `frame3d.particles/particle_systems` | 单结构体 | 否 | ✅ 安全 | 尾部追加 |
| ~~`pbr_material.morph_weights[8]`~~ | 数组元素 | 是 | ❌ 需 v5 | 见下 |
| ~~`draw3d.morph`~~ | 数组元素 | 是 | ❌ 需 v5 | 见下 |

**为什么权重不放在 `pbr_material`（评审最可能问的一条）**：今天 runtime 在提取时**逐 draw 复制**材质
（`extract3d.cpp:516`、`190`、`773`），所以往材质里塞权重"碰巧能用"；但语义是错的——材质是
"怎么画"，权重是"这个实例摆成什么样"，两者生命周期不同（材质可缓存/去重，权重逐帧变）。
一旦以后做材质去重或 `pbr_material` 缓存，权重就会被别的实例覆盖。放 `frame3d` 还有一个硬好处：
`draw3d` / `pbr_material` 的 `static_assert` 尺寸与 gfx 的 `draw_slot_key` 都不用动。

**如果将来一定要动数组元素**（例如要把权重压到 4 个 float 放进 `draw3d`）：必须
`version: 5` + `abi_name: maque.renderer.v5`，并按既有做法同步 `null_renderer.cpp:283`、
`runtime/src/engine/setup.cpp:167,186`、`gfx.cpp:5041`。本文的设计刻意避免了这一步。

## 3. Feature A：GPU morph target 动画

### 3.1 方案总览

```
装配期（IO 合法，不在模拟帧内）
  glTF / .mesh.json 解析 ──► mq_renderer_morph{ deltas, target_count, vertex_count, flags }
        │                         │
        │                         └─ create_mesh(mesh_gpu{..., morph}) ──► GPU storage buffer（只读、常驻）
        └─ CPU 仍保留 base/targets（仅回退路径用；GPU 生效后可释放 targets）

每帧（模拟帧内，只读）
  动画 weights 通道 / 默认权重 / （可选）MeshRenderer.morph_weights
        └─ extract_3d：按 draw 下标填 frame3d.morph_weights[di*8 .. di*8+7]

gfx（成帧）
  pack_draw_uniform：du[84..95] = (count,w0..w2)(w3..w6)(w7,vertex_count,has_normals,0)
  顶点着色器：pos/nrm = base + Σ w_t · Δ_t   →   再乘 skin（若蒙皮）→ 再乘 model
  阴影着色器：只算位置（跳过 normal 累加）
```

### 3.2 命名与 schema（renderer.yaml，全部尾部追加）

```yaml
# ① 每个 morph 网格的 GPU 常驻增量（create_mesh 时一次性上传；之后只读）
- record: morph
  fields:
    - { name: deltas,       type: "const float*" }  # target_count × vertex_count × 8 float
                                                    # 行 = [pos.xyz, pad, nrm.xyz, pad]（32B/目标/顶点）
    - { name: target_count, type: uint32_t }        # 1..8；0 = 无 morph
    - { name: vertex_count, type: uint32_t }        # 每条 target 的顶点数（= deltas 的行宽）
    - { name: flags,        type: int32_t }         # bit0 = 含 normal 增量（0 → shader 跳过 nrm 累加）

# ② mesh_gpu（单结构体，尾部追加）
- record: mesh_gpu
  fields:
    # ... 既有 8 个字段保持不变 ...
    - { name: morph, type: mq_renderer_morph }

# ③ frame3d（尾部追加；权重按 **draw 下标**，每个 draw 8 个 float，行优先）
    - { name: morph_weights,      type: "const float*" }
    - { name: morph_weight_count, type: uint32_t }   # 必须 == draw_count 才生效；否则视为"无 morph"

# ④ caps（out 结构，尾部追加；能力探测）
    - { name: morph_max_targets, type: int32_t }     # 0 = 插件不支持 GPU morph → runtime 保留 CPU 路径
```

组件面（可选，M2）：

```yaml
# render.MeshRenderer 尾部追加（脚本手动驱动表情/形变；空 = 走动画与默认权重）
- { name: morph_weights, type: "float[]" }   # 长度 ≤ 8；非空时优先级最高
```

优先级（写进组件 doc，必须实现成一条链）：
**`MeshRenderer.morph_weights`（非空） > glTF 动画 `weights` 通道覆盖 > primitive 默认权重**。

### 3.3 内存布局与预算

**布局**：`deltas[t * vertex_count + v] = { pos: vec4f, nrm: vec4f }`，stride **32B**。

- 为什么是 32B 而不是紧凑 24B：`vec3f` 在 storage 数组里对齐 16B，24B stride 会迫使 shader 用标量读
  或让后端插入额外指令；32B 允许一次 `MorphDelta` 结构读（两次 16B load），且与 WGSL 结构体布局一一对应。
- 为什么是"target 主序"：GPU 侧 `t * vertex_count + v` 只做一次整数乘；且 `create_mesh` 一次性上传，
  内存连续最省事。

| 场景 | 顶点数 | targets | GPU 增量 | 现状每帧重传（stride 72） | 现状 CPU 重算量级（估算） |
|---|---|---|---|---|---|
| 小道具 | 2 000 | 2 | 128 KB | 144 KB | 4k 次内层迭代，~0.02 ms |
| 角色 | 10 000 | 4 | 1.28 MB | 720 KB | 40k 次，~0.2 ms |
| 高模 | 50 000 | 8 | 12.8 MB | 3.6 MB | 400k 次，~2 ms（Debug） |

> CPU 估算按"每顶点每 target 约 10–14 flop + 一次 `memcpy(base)`"折算；真实值随构建类型变化，
> 验收以 `MAQUE_TRACE_STATS` / Tracy 的前后对比为准，不用绝对毫秒做门。

**上传频率**：

| 数据 | 频率 | 字节/帧 |
|---|---|---|
| morph 增量 | 装配期一次 | 0 |
| 权重 | 每 draw 每帧（写进已有 uniform buffer） | 48B × morph draw 数（50 draw ≈ 2.4 KB） |
| 顶点缓冲 | **不再重传** | 0 |

**CPU 内存**：GPU 路径生效后 `MorphMesh::targets` / `Prim::morph_targets` 可释放
（`base` 保留给回退路径，或整体释放并用 `update_mesh` 的旧路径视为不可用）。建议用
`MAQUE_MORPH_CPU_FALLBACK` 决定是否保留（默认保留 1 份，便于 A/B 与 >8 target）。

### 3.4 WGSL 变更

```wgsl
// pbr/00_types.wgsl（尾部追加）
struct MorphDelta { pos: vec4f, nrm: vec4f };            // 32B
@group(1) @binding(24) var<storage, read> morph_deltas: array<MorphDelta>;

// Draw 结构体尾部追加 3 个 vec4（84 → 96 floats / 384B）：
//   morph0: vec4f,  // x = target_count, yzw = w[0..2]
//   morph1: vec4f,  // w[3..6]
//   morph2: vec4f,  // x = w[7], y = vertex_count, z = has_normals(0/1), w = 预留
```

```wgsl
// pbr/40_vertex.wgsl（新增共享函数；阴影只要位置，单独一条快路径）
fn morph_weight(i: u32) -> f32 {
  // 用 if 链而不是 vec 动态下标：部分后端会把动态下标落到 local memory
  if (i == 0u) { return dr.morph0.y; }
  if (i == 1u) { return dr.morph0.z; }
  if (i == 2u) { return dr.morph0.w; }
  if (i == 3u) { return dr.morph1.x; }
  if (i == 4u) { return dr.morph1.y; }
  if (i == 5u) { return dr.morph1.z; }
  if (i == 6u) { return dr.morph1.w; }
  return dr.morph2.x;                                  // i == 7
}

fn morph_position(vi: u32, p: vec3f) -> vec3f {
  let cnt = u32(dr.morph0.x + 0.5);
  if (cnt == 0u) { return p; }
  let nv = u32(dr.morph2.y + 0.5);
  var out = p;
  for (var t = 0u; t < 8u; t = t + 1u) {
    if (t >= cnt) { break; }
    let w = morph_weight(t);
    if (w == 0.0) { continue; }
    out = out + morph_deltas[t * nv + vi].pos.xyz * w;
  }
  return out;
}

struct MorphRes { pos: vec3f, nrm: vec3f };

fn morph_apply(vi: u32, p: vec3f, n: vec3f) -> MorphRes {
  var r: MorphRes; r.pos = p; r.nrm = n;
  let cnt = u32(dr.morph0.x + 0.5);
  if (cnt == 0u) { return r; }
  let nv = u32(dr.morph2.y + 0.5);
  let want_n = dr.morph2.z > 0.5;
  var touched = false;
  for (var t = 0u; t < 8u; t = t + 1u) {
    if (t >= cnt) { break; }
    let w = morph_weight(t);
    if (w == 0.0) { continue; }
    let d = morph_deltas[t * nv + vi];
    r.pos = r.pos + d.pos.xyz * w;
    if (want_n) { r.nrm = r.nrm + d.nrm.xyz * w; touched = true; }
  }
  // 与 CPU 路径一致：先归一化，再交给蒙皮（vs_common_skin 内部还会 normalize，幂等）
  if (touched) { r.nrm = normalize(r.nrm); }
  return r;
}
```

```wgsl
// 四个主入口统一改成（示意 vs_skin；其余三个同构）
@vertex fn vs_skin(@builtin(vertex_index) vi: u32,
                   @location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f,
                   @location(5) joints: vec4u, @location(6) weights: vec4f) -> VOut {
  let m = morph_apply(vi, pos, nrm);                       // ① 先 morph（mesh 空间）
  let c = vs_common_skin(m.pos, m.nrm, skin_matrix(joints, weights));   // ② 再蒙皮
  ...
}

// 四个阴影入口（只要位置）
@vertex fn vs_shadow_skin(@builtin(vertex_index) vi: u32, @location(0) pos: vec3f,
                          @location(2) uv: vec2f, @location(5) joints: vec4u,
                          @location(6) weights: vec4f) -> ShadowVOut {
  let p = morph_position(vi, pos);
  let skin = skin_matrix(joints, weights);
  o.p = cam.light_vp * dr.model * (skin * vec4f(p, 1.0));
  ...
}
```

**顺序说明（glTF 规范）**：morph 在 **mesh 空间**先应用，然后才是蒙皮（`skin * (base + ΣΔ)`）。
现有 CPU 路径已经是这个顺序（先重算顶点、再由 GPU 蒙皮），所以 GPU 化不需要改语义。

**`@builtin(vertex_index)` 的语义**：所有 3D 绘制都是 `drawIndexed`，`vertex_index` = 解开索引后的顶点号，
正好等于 `deltas` 的 `v` 下标。粒子/告示板走的是另一条管线（§4），不受影响。

### 3.5 gfx 集成点（逐条，按现有函数）

| 位置 | 改动 |
|---|---|
| `gfx.cpp:813-831` group1 BGL | `e[24]` → `e[25]`，`entryCount = 25`；binding24 = `Vertex` 阶段 `ReadOnlyStorage`，`minBindingSize = 32` |
| `gfx.cpp:3649-3698` `fill_material_bind_entries` | 形参 `e[24]` → `e[25]`；新增 `e[24].binding=24; e[24].buffer = morph_buf ? morph_buf : dv->dummyMorphBuf; e[24].size = WGPU_WHOLE_SIZE` |
| `gfx.cpp:1150-1158` 附近 | 新增 `dummyMorphBuf`（32B 全 0）与 identity 调色板同批创建 |
| `gfx.cpp:3749-3781` `ensure_draw_slot` | 新增 `WGPUBuffer morph_buf` 形参；`draw_slot_key` 混入 morph buffer 句柄（**否则跨帧/换场景后会绑到上一个 draw 的 mesh 增量**） |
| `gfx.cpp:3501-3565` `pack_draw_uniform` / `kMatUniformFloats` | `84` → `96`；新增 `morph_weights` 形参（8 float）+ `target_count/vertex_count/has_normals`；写 `du[84..95]`；`BGL e[0].minBindingSize` 336 → 384 |
| `gfx.cpp` `create_mesh`（约 1949-2000） | 校验 `data->morph.target_count ≤ 8 && vertex_count == vn`；建 storage buffer（`Storage | CopyDst`），存进 `MeshGpu{morph, morph_targets, morph_vertices, morph_has_normals}`；`destroy_mesh` 同步释放 |
| `gfx.cpp:4229-4245` 阴影 draw | `ensure_draw_slot(..., morph_of(mesh))`；`pack_draw_uniform` 传权重 |
| `gfx.cpp:4286-4291` 主 draw | 同上 |
| `gfx.cpp:32-49` static_assert | 新增 `sizeof(mq_renderer_particle) == 48`、`sizeof(mq_renderer_particle_system) == 32`（两者不含指针，可硬断言）；`mq_renderer_morph` **含指针** → 只能做 `offsetof` 断言（`deltas` 必须在 0、`target_count` 在 `sizeof(void*)`）；`mq_renderer_caps` 由 32 → **36**，必须同步改 `gfx.cpp:32` 的断言 |
| `MeshGpu::gpu_bytes`（`gfx.cpp:1997-1998`）与 `r_resource_bytes2`（`gfx.cpp:2026`） | 把 morph buffer 计入 `gpu_bytes`；粒子实例缓冲计入 `buffer_bytes`（docs/29 §10 的 gpu 域） |
| 管线 | **不新增 entry、不新增管线**；`p3dMatPipeline` / `shadowPipeline[4]` 的维度不变 |

### 3.6 runtime 集成点

| 位置 | 改动 |
|---|---|
| `assets/gltf.cpp:468-512` | 填 `md.morph = {deltas, target_count, vertex_count, flags}`；`deltas` 由 `pr.morph_targets` 打包（`vn × targets × 8` float）；**去掉** 526-533 的"默认权重立即 `apply_gltf_morph`"（GPU 生效时由每帧权重表达） |
| `assets/texture.cpp:733-793` | `.mesh.json` 同样打包 `pm.morph`；`morph.targets` 保留给回退 |
| `assets/gltf_anim.cpp:721-743` | GPU 生效 → 不再调 `apply_gltf_morph`，只把 `st.morph_weights[pi]` 存起来给 extract 用；`morph_owner` 限制只在回退路径生效 |
| `render/extract3d.cpp:598-615` | 改为：填 `morph_weights_out`（`draw_count × 8`）而不是调用 `apply_morph`；权重来源 = 组件（若加）> 动画覆盖 > 默认；**每实体独立**（修 G2） |
| `gltf_model.h` / `runtime_common.h` | 增加 `MorphMesh::gpu_ok`、`Prim::gpu_morph` 标记（装配期由 `caps.morph_max_targets` 决定） |
| `setup.cpp` | `rapi->caps()` **目前只在 `want_wgpu || MAQUE_RENDER3D_INFO` 时调用**（`setup.cpp:249-262`），且 `r_caps` 会 `memset(out,0,…)`。morph 能力探测要求：装配期**无条件**调用一次 `caps()`（或在既有块里提到条件外），`r_caps` 里补 `out->morph_max_targets = 8`；据此定下 `morph_gpu_enabled`（`caps.morph_max_targets >= 8` 且非 `MAQUE_MORPH_GPU=0`），切场景不重置 |
| 新开关 | `MAQUE_MORPH_GPU=0`（强制 CPU 路径做 A/B）、`MAQUE_RENDER3D_INFO=1` 打印 `morph=gpu/cpu targets=N` |

### 3.7 回退矩阵（必须逐格实现并测试）

| 条件 | 行为 | 结果 |
|---|---|---|
| `caps.morph_max_targets >= 8` 且 `target_count ≤ 8` 且 `MAQUE_MORPH_GPU != 0` | GPU 路径 | 顶点着色器累加 |
| `MAQUE_MORPH=0` | 不填权重（`dr.morph0.x = 0`） | 同一入口、一次比较后返回基础网格（**与历史 `MAQUE_MORPH=0` 像素等价**） |
| `target_count > 8` | 该 primitive 走 CPU `update_mesh`（GPU 缓冲不建） | 视觉正确，性能回到旧水平；告警一次 |
| 插件 `caps.morph_max_targets == 0`（旧插件/null） | 全部走 CPU 路径 | 与当前行为一致 |
| `create_mesh` 时 morph 校验失败（`vertex_count` 不符） | 不建 morph buffer，`morph.target_count = 0` | 画基础网格；告警一次 |
| `.mesh.json` 有 morph 但 `MAQUE_MORPH_GPU=0` | CPU 路径 | A/B 像素门用它 |

### 3.8 风险

| 风险 | 对策 |
|---|---|
| 共享 mesh、多实体不同权重 | 权重是 per-draw（本文设计的直接收益）；e2e 用"同一 glTF 两个实体、`time` 不同"断言两处轮廓不同 |
| morph + skin 顺序写反 | 先 morph 后 skin；用 `MAQUE_MORPH_GPU=0` 的 PPM 做金标准（mean abs diff ≤ 1/255） |
| 法线归一化差异（CPU 先归一再蒙皮 vs GPU 反序） | shader 里 morph 累加后**先 normalize**；与 CPU 对齐后再进 `vs_common_skin` |
| 稀疏 accessor / CUBICSPLINE | 已在解析期解决（`gltf_model.cpp:1009-1113` 的 `apply_sparse_float`；`gltf_anim.cpp:459-471` 的 CUBICSPLINE），GPU 路径零改动；补 fixture 覆盖 |
| 循环上界/动态下标 | 固定 8 次上界 + `break`；权重用 if 链；不用 `array<f32>` 动态下标 |
| WebGPU storage 限制 | group1 只用 2 个 storage（skin 23 + morph 24），远低于默认 8；单 mesh 缓冲 ≤ 25.6MB < `maxStorageBufferBindingSize` 默认 128MB；`create_device` 已向适配器请求上限（`gfx.cpp:1190-1193`） |
| draw slot 复用到错 buffer | slot key 必须混入 morph buffer 句柄（容易漏，验收要覆盖"换场景后仍正确"） |
| 阴影 pass 漏绑 | 阴影与主 pass 共用 `fill_material_bind_entries`/`ensure_draw_slot`；e2e 断言"影子随权重变化" |
| 管线数量爆炸 | 不新增 entry（见 §0 第 3 条） |
| 内存翻倍（CPU + GPU 各一份） | `MAQUE_MORPH_CPU_FALLBACK=0` 时可释放 CPU targets；默认保留以便回退 |
| `.mesh.json` 与 glTF 两条装配路分叉 | 共用同一个打包函数（`pack_morph_deltas()`），单测覆盖两条路径产出的字节一致 |

### 3.9 验收

- **单测**：`modules/runtime/tests/morph_layout_test.cpp` —— deltas 打包（32B/目标/顶点、target 主序、
  normal 缺失补 0、`target_count > 8` 拒绝）；权重数组按 draw 展开（8 对齐、缺省 0）。
- **e2e**：`tests/morph_gpu_e2e.sh` + `scenes/pbr/morph_gpu.scn`：
  1. 默认权重非 0 → 出图与 `MAQUE_MORPH=0` 的图不同（结构断言：包围盒/像素差）；
  2. 动画 `weights` 通道 → 帧 1 / 帧 30 哈希不同，同帧两次一致；
  3. `MAQUE_MORPH_GPU=0` 与 GPU 路径同帧 mean abs diff ≤ 1/255；
  4. 两实体共享同一 glTF、不同 `time` → 两处轮廓不同（修 G2 的回归）；
  5. 开/关阴影：morph 角色的影子随权重变化（证明阴影 pass 绑到了增量）。
- **回归**：`ctest -R 'gltf_morph_e2e|gltf_skin_e2e|gltf_anim_e2e|render3d_scene_e2e|determinism'` 全绿；
  非 morph 场景 PPM 逐字节不变。
- **门禁**：`node parser.js --check`、`ctest -R 'codegen_drift|schema_imports|builtin_shaders'`。
- **平台**：Windows 实机 + WASM headless 冒烟（`wasm_p0_smoke`）；Android 仅功能冒烟。

### 3.10 Transform Animation：为什么不做 GPU，哪里值得做

**结论：节点 TRS 关键帧采样保持在 CPU（现状），不迁移。**

| 维度 | 事实 |
|---|---|
| 规模 | 典型模型 10–200 个节点，每节点 ≤ 3 条通道；每帧一次二分查找 + lerp/slerp |
| 成本 | 整帧 µs 级；`update_gltf_state` 的耗时在 `MAQUE_RENDER3D_INFO` 里可见（与 720KB 顶点重传不是一个量级） |
| CPU 消费者 | ① 蒙皮调色板（`palette = world(joint) × IBM`，在 CPU 生成后上传，`binding 23`）；② `draw3d.model`（ABI）；③ OBB 拾取（`scene_pick`/`scene_raycast` 用 `world_m`）；④ RenderTexture 相机 |
| GPU 化的代价 | 必须把世界矩阵**回读**才能算调色板/填 model/拾取 → 同步点 + 1 帧延迟；或者把蒙皮也整体搬 GPU（compute pass + 间接 draw），工程量与风险都远超收益 |
| 确定性 | 跨 GPU/驱动的 slerp、矩阵乘结合顺序可能不同 → 同机 e2e 仍绿，但跨平台截图/像素门会漂；而 `world_checksum` 又不覆盖 GPU 侧，等于"错得看不见" |

**GPU transform 真正值得用的三个场景**（按性价比排序，均属 M3+）：

1. **实例化人群/碎片**（`render.InstancedMesh`，plans/27 §5.2）：per-instance TRS 由 `instance_index`
   从实例缓冲读，或按 `time` uniform 解析。属于**纯视觉层**：不进 checksum、不参与拾取（用 CPU 近似包围盒）。
2. **GPU 骨骼（palette 在 GPU 生成）**：compute 或 VS 里由关节层级算 palette，配合 `canvas` 无关的
   storage buffer。收益出现在"上百个同骨架角色"时；单个角色反而更慢（多一个 pass）。
3. **VAT / 顶点动画纹理**：把 morph/skin 烘焙成纹理，天然与本文的 morph 增量缓冲同构，
   适合大量同模型 NPC；代价是资产烘焙管线。

**建议**：先做本文 §3 的 morph（收益确定、改动收敛），等真的出现"同屏 >100 个动画角色"再立项
InstancedMesh + GPU 骨骼。不要把"节点 TRS 采样"当性能问题——它从来不是。

## 4. Feature B：3D 粒子系统（CPU 确定性 + GPU 批渲染）

### 4.1 现状 / 目标

**现状（plans/27 M1-4 的 MVP）**：`render.ParticleEmitter3D` 已有 15 个字段、CPU 确定性模拟、
每发射器一张 `max_count` 容量的动态 billboard 网格；渲染走 PBR 管线（Basic + blend + DoubleSide），
每帧 CPU 重建整段顶点 + `update_mesh` 全量上传，按容量绘制，且缺 checksum/快照/排序/统计口径（G5–G9）。

**目标（本文）**：

1. **渲染路径换成实例化 billboard**：1 个 quad 模板（无顶点缓冲，`vertex_index` 生成 4 角）
   + 每粒子 48B 实例数据；draw 数 = 系统数（可合并），顶点工作量与**存活数**成正比。
2. **补齐确定性契约**：CPU 模拟进 `world_checksum` 与 `world_snapshot`；空闲跳帧判定包含粒子；
   draw/tri 统计按存活数。
3. **补可选能力**：`blend`（alpha/additive）、`facing`（视平面/Y 轴锁定）、`local_space`、`sort`、
   `culling_mask`、图集帧、burst、drag/方差字段。
4. **GPU 解析模拟**作为 opt-in（M3），明确"不进 checksum、不进快照、跨平台不保证像素一致"。
5. **不动 2D `scn.ParticleEmitter`**：它继续走 `systems/particles.cpp` 的 2D 通路 + `items2d.cpp`
   的 `mq_renderer_item`，字段/语义/校验和口径全部不变。

### 4.2 组件：`render.ParticleEmitter3D`（已存在，只做尾部追加）

**已有字段（plans/27 M1-4，保持默认值不变）**：

| 字段 | 类型 | 默认 | 单位 | 说明 |
|---|---|---|---|---|
| `emitting` | bool | true | — | 停止发射（已存在的粒子继续走完生命周期） |
| `rate` | float | 30 | 1/s | 每秒发射数（定点累加，确定性） |
| `lifetime` | float | 1.5 | s | 寿命（±25% 随机，见 `lifetime_variance`） |
| `speed` | float | 1.2 | m/s | 初速度（±25%，见 `speed_variance`） |
| `spread` | float | 0.6 | 0..1 | 方向在"基准方向 ↔ 球面随机方向"之间的插值系数（**不是锥角**，保持既有语义） |
| `direction` | math.Vec3 | (0,1,0) | — | 基准方向（零向量回退 +Y） |
| `gravity` | math.Vec3 | (0,-1.5,0) | m/s² | 生成时快照到粒子上（之后改发射器不影响已生成粒子） |
| `size_start` / `size_end` | float | 0.25 / 0.05 | m | 世界直径（billboard 边长）随寿命线性插值 |
| `color_start` / `color_end` | math.Color | 暖橙 / 红透明 | 线性 | 逐粒子线性插值（含 alpha 淡出） |
| `rotation` / `rotation_speed` | float | 0 / 0 | rad、rad/s | 面内旋转 |
| `texture` | string | "" | — | 纹理路径（sRGB）；空 = 1×1 白 |
| `max_count` | uint32 | 256 | — | 该发射器存活上限（1..4096） |

**建议追加（尾部，全部有默认值 = 旧行为）**：

| 字段 | 类型 | 默认 | 阶段 | 说明 |
|---|---|---|---|---|
| `blend` | `render.BlendMode` | `Alpha` | M1 | 支持 `Alpha`/`Additive`；`Multiply` 暂按 Alpha 处理并告警一次 |
| `facing` | `render.BillboardFacing` | `ViewPlane` | M1 | 只实现 `ViewPlane`/`YAxisLocked`；其余回退 ViewPlane + 告警 |
| `sort` | bool | true | M1 | alpha 混合时按相机距离远→近；additive 可关 |
| `local_space` | bool | false | M2 | true = 已生成粒子跟随发射器节点变换（见 §4.7） |
| `culling_mask` | uint32 | 0xFFFFFFFF | M2 | 与 `draw3d.layer` 同一套位掩码（相机 `layer_mask` 过滤） |
| `layer` | entity_ref(RenderLayer) | — | M2 | 与其它 render 组件一致 |
| `drag` | float | 0.0 | M2 | 速度衰减 1/s（`v *= exp(-drag*dt)` 的显式欧拉近似） |
| `speed_variance` | float | 0.25 | M2 | 替换硬编码的 `0.75 + 0.5*u` |
| `lifetime_variance` | float | 0.25 | M2 | 同上 |
| `size_variance` | float | 0.0 | M2 | 每个粒子的尺寸乘子抖动 |
| `shape` | `EmitterShape`（新增枚举） | `Point` | M2 | `Point/Sphere/Box/Cone` 发射体 |
| `shape_size` | math.Vec3 | (0,0,0) | M2 | 发射体尺寸（球=半径、盒=半尺寸、锥=半径/高） |
| `burst` | uint32 | 0 | M2 | 每次爆发数量 |
| `burst_interval` | float | 0.0 | M2 | 爆发间隔（0 = 仅在 `on_start` 时一次） |
| `frame_count` / `frame_rate` | int32 / float | 1 / 0 | M3 | 图集帧数（横向）/ 帧率（0 = 按寿命均匀） |
| `simulation` | `ParticleSimulation`（新增枚举） | `Cpu` | M3 | `Cpu` = 确定性；`Gpu` = 解析模拟（见 §4.3） |

新增枚举（与组件同文件，紧邻声明）：

```yaml
- enum: ParticleSimulation
  values:
    - { name: Cpu, value: 0 }   # 默认：确定性模拟，进 checksum/快照
    - { name: Gpu, value: 1 }   # opt-in：解析模拟，纯视觉，不进 checksum（M3）
- enum: EmitterShape
  values:
    - { name: Point,  value: 0 }
    - { name: Sphere, value: 1 }
    - { name: Box,    value: 2 }
    - { name: Cone,   value: 3 }
```

#### 4.2.1 与 2D `scn.ParticleEmitter` 的关系（不许破）

| 维度 | `scn.ParticleEmitter`（2D，保留） | `render.ParticleEmitter3D`（本文） |
|---|---|---|
| 坐标 | 屏幕/世界设计像素，y 向下 | 3D 世界米，y 向上 |
| 渲染 | `mq_renderer_item`（2D 图元，走 UiBatcher） | `mq_renderer_particle`（3D 实例化 billboard） |
| 池 | `systems.particles`（全局池，2048） | `systems.particles3d`（每发射器 `max_count` + 全局上限） |
| checksum | `world_checksum` 已混入 `x/y/life` | 本文补入 `x/y/z/age`（CPU 模式） |
| 字段 | `rate/lifetime/speed/spread/gravity/size/rgb/max_count/enabled` | 见 §4.2（单位/语义不同，**不共享**） |
| 迁移 | 不迁移、不废弃、不改默认值 | 新场景用新组件 |

两条通路可以同屏共存：2D 粒子在 2D 覆盖层，3D 粒子在 3D pass；互不影响排序与 checksum 字段。

### 4.3 两种模式

| | (a) CPU 确定性模拟 + GPU 实例化渲染（**默认**） | (b) GPU 解析模拟（opt-in，M3） |
|---|---|---|
| 状态所有权 | CPU（`SystemsRuntime::Particle3D`） | 无状态：VS 由 `(spawn_time, seed, 参数, time)` 解析 |
| 确定性 | 固定步长 + 引擎 RNG → **逐位可复现** | 同机可复现；跨 GPU/驱动不保证 |
| checksum / 快照 | **进**（与 2D 粒子同口径） | **不进**（与 `docs/30 §5` 音频 playhead 同策略） |
| 能力 | 重力/阻力/碰撞查询/按寿命排序/程序化引导 | 只能解析式运动；无排序（additive 友好）、无碰撞 |
| 适用 | 玩法相关（技能判定、引导、可回放） | 纯视觉（环境火星、雨雪、远景氛围） |
| 开关 | 默认 | `simulation = ParticleSimulation::Gpu`（场景显式选择） |

**为什么 (b) 必须 opt-in 且排除 checksum**：`world_checksum` 是回放/失步定位的真相来源；
GPU 侧结果不可回读、跨平台不可复现，一旦混入会让"回放一致"变成"同机碰巧一致"。
反过来，`(b)` 也不允许参与 `scene_pick`/物理查询——它没有 CPU 侧状态。

### 4.4 ABI（renderer.yaml 尾部追加）

```yaml
# 单个粒子实例（48B；**与 GPU 实例缓冲布局逐字节一致** → gfx 直接 memcpy 上传）
- record: particle
  fields:
    - { name: pos,   type: "float[4]" }   # xyz 世界位置（m），w 世界尺寸（m）
    - { name: color, type: "float[4]" }   # 线性 RGBA（已含寿命曲线与淡出）
    - { name: misc,  type: "float[4]" }   # x 面内旋转（rad），y 图集帧索引，z 速度拉伸（0=圆形），w 预留

# 一个发射器/一个批次（32B）
- record: particle_system
  fields:
    - { name: texture, type: int32_t }    # 纹理句柄（-1 = 1×1 白）
    - { name: blend,   type: int32_t }    # 0=alpha 1=additive 2=multiply（2 暂按 alpha）
    - { name: facing,  type: int32_t }    # 0=view-plane 1=Y 轴锁定
    - { name: flags,   type: int32_t }    # bit0 = 已按远→近排序；bit1 = local_space 已烘焙（诊断）
    - { name: first,   type: uint32_t }   # frame3d.particles 起点
    - { name: count,   type: uint32_t }   # 本系统粒子数（0 = 跳过）
    - { name: layer,   type: int32_t }    # culling mask（bit30 语义同 draw3d）
    - { name: pad,     type: int32_t }

# frame3d 尾部追加
    - { name: particles,             type: "const mq_renderer_particle*" }
    - { name: particle_count,        type: uint32_t }
    - { name: particle_systems,      type: "const mq_renderer_particle_system*" }
    - { name: particle_system_count, type: uint32_t }
```

设计要点：

- **没有 `material` 字段**：粒子是 unlit billboard，不需要 10 种着色模型/贴图槽；用专用 shader + 2 个绑定
  比复用 PBR 管线便宜得多，也避免每发射器 push 一条假材质（现状 `extract3d.cpp:747-773` 的做法）。
- `particle` 的 48B **必须**与实例缓冲 stride 一致：gfx 只做一次 `wgpuQueueWriteBuffer`，零转换。
- `first/count` 允许"一个连续实例数组 + 多个系统切片"，从而支持同状态系统合并（§4.5 draw budget）。

### 4.5 渲染设计

**几何**：无顶点缓冲。`@builtin(vertex_index)` 0..3 生成 `TriangleStrip` 的 4 个角：

```wgsl
// corner: (-1,-1) (1,-1) (-1,1) (1,1)
var corner = vec2f(f32(vi & 1u), f32((vi >> 1u) & 1u)) * 2.0 - 1.0;
```

**实例数据**（vertex buffer，`stepMode = Instance`，stride 48B，3 个 `@location`）：

| location | 格式 | offset | 内容 |
|---|---|---|---|
| 0 | `float32x4` | 0 | `pos_size` |
| 1 | `float32x4` | 16 | `color` |
| 2 | `float32x4` | 32 | `misc`（旋转/帧/拉伸） |

**uniform（group0，128B，每视图一份）**：

```wgsl
struct ParticleGlobals {
  vp: mat4x4f,      // 64B  世界 → clip
  view: mat4x4f,    // 64B  世界 → 视图（列主序；transpose 后取行 = 相机基）
};
@group(0) @binding(0) var<uniform> pg: ParticleGlobals;
@group(1) @binding(0) var ptex: texture_2d<f32>;
@group(1) @binding(1) var psmp: sampler;
```

> **为什么不复用 pbr 的 `Camera`（944B）**：那个结构体每次加特性都在长（912→944），
> 让粒子 shader 复制一份 900+ 字节的声明，会把"改大气/改相机布局"变成"粒子 shader 也要跟着改"。
> 128B 的独立小结构体把耦合降到零。

**billboard 基向量**：

```wgsl
let vt = transpose(pg.view);
let right = vt[0].xyz;      // 相机右基（世界空间）
let up    = vt[1].xyz;      // 相机上基
// Y 轴锁定模式（facing=1）：right = normalize(cross(vec3f(0,1,0), -vt[2].xyz))，
//   up = normalize(cross(-vt[2].xyz, right))；|forward.y| > 0.99 时回退视平面基。
let cr = cos(misc.x); let sr = sin(misc.x);
let local = vec2f(corner.x * cr - corner.y * sr, corner.x * sr + corner.y * cr);
let world = pos_size.xyz + (right * local.x + up * local.y) * (pos_size.w * 0.5);
out.p = pg.vp * vec4f(world, 1.0);
```

**深度/混合/排序**：

| 项 | 设定 | 理由 |
|---|---|---|
| depth test | `LessEqual` | 允许与地面共面（`<` 会把贴地粒子裁掉） |
| depth write | `false` | 透明粒子不写深度 |
| cull | `None` | 双面可见（billboard 无背面概念） |
| topology | `TriangleStrip` | 4 顶点、无索引 |
| alpha | color `SrcAlpha/OneMinusSrcAlpha`，alpha `One/OneMinusSrcAlpha` | 与 3D 混合一致 |
| additive | color `SrcAlpha/One`，alpha `One/One` | 加法可交换 → 不需要排序 |
| 排序 | CPU 每系统按相机距离远→近（`std::stable_sort`，同深度按生成序） | 确定性排序；**只重排实例缓冲，不动模拟数组** |
| 绘制位置 | 3D pass 的 **blend 列表之后**（同一 render pass，深度沿用） | 粒子永远叠在透明物体之上；被不透明物体正确遮挡 |
| 阴影 | 不投影（等价现状：blend 材质不进 shadow pass） | 保持视觉语义 |

> **已知限制（写进文档）**：由于没有跨物体排序键，粒子与"透明物体"的前后关系固定为"粒子在后画"。
> 需要精确关系时用 `particle_system.sort_center`（M3 预留）把粒子并进 blend 列表统一排序。

**纹理与采样**：`texture` 路径 → `load_texture_srgb()`（与 `.mesh.json` 贴图共用缓存）；
`-1` → `dv->whiteTexView` + `dv->sampler`；图集帧：`misc.y` 为帧索引，shader 里
`uv = (uv + vec2(f32(frame), 0.0)) / vec2(f32(frame_count), 1.0)`（`frame_count` 走 `particle_system.flags`
旁边的预留位或 uniform——**建议 M3 再加一个 `frame_count` 字段，不要现在猜**）。

**draw 预算**：

| 场景 | 现状 | 本文（M1） | 本文（M2 合并后） |
|---|---|---|---|
| 1 个发射器、256 粒子 | 1 draw、**1536 索引**、1024 顶点上传（cap 满） | 1 draw、4 顶点 × 256 实例 | 同 |
| 8 个发射器、各 256 | 8 draw、12288 索引、8×57KB 上传 | 8 draw、8192 实例 | ≤ 状态数（纹理 × blend × facing），通常 2–4 draw |
| 全局 4096 粒子 | 4096 顶点缓冲 / 发射器 | 4096 × 48B = 196KB/帧 | 同 |

**实现落点**（gfx）：

| 位置 | 改动 |
|---|---|
| `Device` | 新增 `p3dPartShader`、`p3dPartBgl`（group0 128B + group1 贴图/采样）、`p3dBgPart`、`partPipeline[blend][facing][msaa][format]`（≤16 个）、`partGlobalsBuf`、`partInstBuf`（grow-only） |
| 初始化（`create_device`/`ensure_3d_pipelines`） | `mq_builtin_shader_text("particles")`；为空 → 打印一行原因并跳过粒子批次（与 solid/textured 剔除同策略） |
| `render_3d_only`（blend 列表之后） | 写 `partGlobalsBuf`（当前视图的 vp/view）→ `SetBindGroup(0)` → 逐系统：切管线/贴图 bg、`SetVertexBuffer(0, instBuf, first*48)`、`draw(4, count)` |
| 实例缓冲 | 所有系统一次性 `wgpuQueueWriteBuffer`（容量不足时重建为 2 的幂）；**不要**每帧创建 buffer |
| `resource_bytes2` | `buffer_bytes` 计入实例缓冲（docs/29 §10） |
| 剔除 | `particle_system.layer` 与 `view3d.layer_mask` 按 bit30 语义过滤；`count == 0` 直接跳过 |

### 4.6 shader 目录

```
assets/builtin/shaders/particles/
  00_types.wgsl     # ParticleGlobals(128B) + 注释里的实例布局（48B）
  10_common.wgsl    # billboard 基向量 / 角点 / 旋转
  20_particle.wgsl  # @vertex vs_particle / @fragment fs_particle
```

- 目录名 = shader 名，`cmake/shader_pack.cmake:43-60` 自动发现并生成 `MAQUE_SHADER_PARTICLES` 剔除开关，
  无需改 CMake 列表；`ctest -R builtin_shaders` 校验"注册表 = 开关 + 解压文本与磁盘逐字节一致"。
- gfx 侧取不到时打印原因并跳过 **粒子批次**（其它 3D 照常）。

### 4.7 runtime 模拟与确定性

| 主题 | 方案 |
|---|---|
| 固定步长 | 沿用 `dt = 1/60` 与 `step_frame`；发射累加用定点 `rate*dt*1024`（与 2D 粒子同款，避免浮点相位漂移） |
| RNG | `next_rng()`，**每个粒子固定消耗 4 个**（球面方向 2 个 + 速度 1 个 + 寿命 1 个）；顺序写进注释，改顺序 = 破坏回放 |
| 遍历顺序 | 实体 id 升序（`collect_preorder`）；模拟数组只追加/压缩，不按相机重排 |
| 池化 | 每发射器 `max_count`（clamp 1..4096）+ 全局 `systems.max_particles3d`（默认 4096，`MAQUE_PARTICLES_MAX` 可调）；满则丢弃新粒子（不覆盖老粒子，避免"寿命突变"） |
| local_space | 模拟始终在**发射器局部空间**做积分；extract 时用 `node.world_m` 变换到世界（纯表现）。`local_space=false` 时在生成瞬间把位置/速度变换到世界，之后独立 |
| 发射器销毁 | 粒子保留到寿命结束（现状）；`local_space=true` 的粒子随发射器一起销毁（否则会挂在原点） |
| checksum | `world_checksum()` 追加 `x/y/z/age`（与 2D 的 `x/y/life` 同款定点混入）；**只在 CPU 模式** |
| 快照 | `world_snapshot()` 在 2D 粒子块之后追加 `'P3D1'` 魔数块（数量 + 每粒子定长）；旧快照无此块 → 跳过（向后兼容） |
| 空闲跳帧 | `engine/frame.cpp:544` 的 `visual_dynamic` 增加 `!systems.particles3d.empty()`（修 G7） |
| 切场景 | `clear_asset_caches()` 清 `particles3d` / `particle3d_accum` / 实例缓冲（已部分存在，补齐 gfx 侧） |
| 诊断 | `MAQUE_PARTICLES_INFO=1` 打印 `systems/alive/draws/upload_bytes`；Tracy zone `systems.particles3d` 已存在 |

### 4.8 必须先修的缺口（按优先级）

| 编号 | 修法 | 验收 |
|---|---|---|
| G6 | checksum + 快照按 §4.7 补 | `particles3d_determinism_e2e`：回放 120 帧 trail 一致；快照回滚后画面一致 |
| G7 | `visual_dynamic` 加 `particles3d` | 静态场景 + 粒子 + RenderTexture：帧间画面必须变化 |
| G8 | 用 `particle_system` 排序键（或 §4.5 的"blend 之后"固定顺序）替换 identity-model 排序 | 粒子与透明玻璃同屏时顺序稳定（截图断言） |
| G5 | 绘制 `count` 个实例；`mesh_stats` 不再记容量（粒子不再有 mesh） | `[stats] tris/draws` 与存活数一致 |
| G9 | 实例化后 48B/粒子 | `MAQUE_PARTICLES_INFO` 的 upload_bytes = `48 × alive` |
| G3 | glTF 直读加 `≤ 8` 限制（>8 走 CPU 路径） | 单测 + 告警一次 |

### 4.9 性能预期与平台

| 指标 | 估算（公式） | 备注 |
|---|---|---|
| CPU 模拟 | `O(rate·dt)` 生成 + `O(n)` 积分 + `O(n log n)` 排序 | 4096 粒子排序 ≈ 50k 比较，Debug 下 ~0.2 ms 量级 |
| 上传 | `48B × alive` | 4096 → 196KB/帧（现状 917KB/帧） |
| draw | `≤ 状态数` | 目标 ≤ 8 |
| GPU 顶点 | `4 × alive` | 现状 `4 × cap` + `6 × cap` 索引 |
| 填充率 | `Σ(quad 像素)`，与 alpha/additive 相关 | **移动端主要瓶颈**；建议手游预算 256–512/发射器 |

平台：

- **WASM**：单线程、无 compute 依赖；排序在 CPU（4096 以内可接受）；不用 `create_texture` 的同步上传路径
  （粒子纹理走既有 `create_texture2`）。
- **Android**：fill-rate 敏感 → additive 大 billboard 要限尺寸；`render_scale` 已能降分辨率；
  实例缓冲 grow-only，避免每帧分配。
- **桌面**：无特殊项；`wgpuQueueWriteBuffer` 一次提交所有实例。

### 4.10 测试

| 层 | 用例 |
|---|---|
| 单测 | `modules/runtime/tests/particles3d_test.cpp`：① 定点累加器跨步不进位错 ② 同 seed 两次生成逐位相同 ③ `max_count` 截断 ④ local/world 变换一致性 ⑤ 排序稳定（同深度按生成序）⑥ 颜色/尺寸曲线在 t=0/0.5/1 的端点 ⑦ `drag` 不影响无 drag 的旧行为 |
| ABI | gfx `static_assert(sizeof(mq_renderer_particle) == 48)`、`sizeof(mq_renderer_particle_system) == 32)`；`offsetof` 校验 |
| e2e | `tests/particles3d_e2e.sh` + `scenes/pbr/particles3d.scn`：① `emitting=0/1` 像素差 ② 同帧两次运行逐字节一致 ③ 帧 1/30/60 画面变化（不是静态） ④ `MAQUE_PARTICLES=0` = 纯背景 ⑤ checksum 每帧变化（CPU 模式）；GPU 模式：`simulation=Gpu` 时 checksum 与"无粒子场景"一致 |
| 回归 | `ctest -R 'determinism|render3d_scene_e2e|gltf_*_e2e|ui_batch|spine'`；旧 2D 粒子场景 PPM 逐字节不变 |
| 平台 | `wasm_p0_smoke`（headless 逻辑/checksum）+ 桌面实机抓帧 |

### 4.11 里程碑

#### M1：实例化渲染 + 确定性契约（P0）

**DoD**

- ABI：`particle` / `particle_system` record + `frame3d` 四个字段（`codegen_drift` 绿，生成物入库）；
- gfx：`particles` shader 目录 + 专用管线 + 实例缓冲 + 视图剔除；`MAQUE_SHADER_PARTICLES=OFF` 可剔除；
- runtime：`extract_3d` 改填实例数组（不再建动态网格/`update_mesh`）；删除 `Particle3DMesh` 与
  `particle3d_meshes` 清理逻辑；
- 修 G5/G7/G8（统计口径、空闲跳帧、排序位置）；G6 的 checksum 部分；
- `particles3d_e2e` 全绿；旧场景（含 2D 粒子）PPM 逐字节不变；
- 文档：AGENTS.md 一行 + `docs/13` 补"3D 粒子批渲染"小节。

#### M2：表现力 + 预算

**DoD**

- `blend`（alpha/additive）、`facing`（view-plane/Y 轴锁定）、`sort`、`local_space`、`culling_mask`、`layer`；
- `drag` / 三个方差字段 / `shape` / `burst`；字段默认值保证旧场景零变化；
- 系统合并（同 texture+blend+facing 相邻系统合并为 1 draw）；`MAQUE_PARTICLES_INFO` draw/upload 统计；
- 快照块 `'P3D1'` + 回滚 e2e；Android 实机 512 粒子冒烟；
- 样例场景扩充：火焰（additive）、烟雾（alpha + local_space）、闪光（burst）。

#### M3：GPU 解析模拟（opt-in）+ 高级项

**DoD**

- `simulation=Gpu`：VS 解析求值（`spawn_time/seed/参数/time`），实现与文档明确"不进 checksum/快照、不参与拾取"；
- 图集帧（`frame_count/frame_rate`）、速度拉伸、`sort_center` 并入透明列表排序；
- 软粒子（需要深度纹理采样）——**独立评估**：当前 depth 附件不可采样，需新增 depth resolve 目标，
  按平台能力开关；
- 性能报告：M1 vs M3 在同一场景的 `[stats]` 对比（draws/tris/upload），写进本文附录。

### 4.12 风险（粒子）

| 风险 | 对策 |
|---|---|
| 过度绘制（fill rate） | 预算写进文档；移动端 256–512；additive 限尺寸；`render_scale` 可降 |
| GPU 解析模拟破坏"回放一致"的预期 | 默认关闭 + 排除 checksum + 场景显式选择 + 文档醒目标注 |
| 快照向后兼容 | 新块带魔数；旧快照缺块 → 视为"无 3D 粒子"并继续 |
| checksum 变更影响老场景 | 只有使用 `ParticleEmitter3D` 的场景才变；旧场景（无该组件）零变化 |
| 粒子与透明物体排序 | M1 固定"粒子最后画"，M3 用 `sort_center` 并入统一排序；限制写进文档 |
| 与 2D 粒子混淆（两套池/两套单位） | 文档 + 组件 doc 明确；测试断言两条通路互不影响 |
| ABI 增长（`frame3d` 继续变长） | 只尾部追加；`struct_size` 由 header 提供；旧插件不加载（版本精确匹配） |
| 每帧实例缓冲增长/抖动 | grow-only + 2 的幂扩容；`resource_bytes2` 可见 |
| 剔除/可见性错误（整批消失） | `layer` 位掩码与 `draw3d` 同一套语义；e2e 用两个相机 layer_mask 断言 |

## 5. 集成契约

### 5.1 数据流（新功能一律走这条）

```
schema/*.yaml ──codegen──► 类型/描述符/绑定/ABI（生成物入库）
      │
      ├─ .scn 反序列化 / 脚本 API / 控制面 / Inspector
      │
      ├─ 装配期（解析/解码/morph 增量打包/蒙皮调色板预建）—— 允许阻塞，不在模拟帧内
      │
      ├─ 帧内模拟（particles3d_step：固定步长、引擎 RNG、确定性）
      │
      ├─ 帧内提取（extract_3d：只读；填 draw/weights/particles）
      │
      └─ gfx（render_frame3d：morph 顶点累加 / 粒子实例化）
```

### 5.2 四条硬约束

1. **渲染只读**：morph 权重、粒子排序都不得写回组件/模拟数组；排序只作用于实例缓冲。
2. **尾部追加 + 默认值等于旧行为**：非 morph 场景 = `dr.morph0.x == 0`；无粒子场景 = count 0；
   两者都必须走**原有代码路径**（不是"新路径的空分支"）。
3. **确定性分层**：CPU 模拟状态进 checksum/快照；GPU 推导的视觉状态（GPU 粒子解析、未来的
   GPU 人群变换）一律不进，且必须在文档里点名。
4. **单一真相**：所有新字段先进 `modules/core/data/schema/`，再生成；禁止手改
   `generated/`、`abi/*.h` 或 gfx 的 shim 头。

### 5.3 测试矩阵

| 层 | 门禁 |
|---|---|
| schema | `node parser.js --check`、`ctest -R 'schema_imports|codegen_drift'` |
| shader | `ctest -R builtin_shaders` |
| ABI | gfx 的 `static_assert`（尺寸/偏移）+ 变体 e2e |
| 运行时 | 新场景 e2e（像素/结构断言）+ 旧场景 PPM 逐字节 A/B |
| 确定性 | `ctest -R determinism` + 粒子回放 e2e |
| 平台 | Windows 实机 + WASM headless 冒烟（涉及 gfx 管线时必须） |

## 6. Owner / 工作量

| 项 | Owner | 规模 |
|---|---|---|
| morph ABI + `create_mesh`/`ensure_draw_slot`/`pack_draw_uniform`/shader | `agent-renderer` | M |
| morph runtime（打包/权重/回退矩阵/组件字段） | `agent-render3d` | M |
| particle ABI + 实例化管线 + shader 目录 | `agent-renderer` | M–L |
| particle runtime（池化/checksum/快照/排序/清理） | `agent-render3d` + `agent-particle` | M |
| e2e 脚本 + 样例场景 + 预览图 | `agent-render3d` | S–M |
| 文档（AGENTS.md / docs/13 / plans/README 状态表） | `agent-render3d` | S |

## 7. 给实现的顺序建议

1. **先写测试骨架**（红）：`morph_layout_test` + `particles3d_test` + 两个 e2e 脚本的"跳过/失败"分支。
   没有金标准就不要动 gfx——morph 的正确性只能靠 `MAQUE_MORPH_GPU=0` 的 A/B 图证明。
2. **粒子先修 G6/G7/G5**（checksum/快照/空闲跳帧/统计），再做实例化。
   理由：这三条与渲染路径无关，却决定"回放/回滚是否可信"；先修可以让实例化改动只看像素差。
3. **morph ABI 一次落地**：`morph` record + `mesh_gpu.morph` + `frame3d.morph_weights` + `caps`，
   同时重新生成（`node parser.js` → `generated/` 入库）并跑 `codegen_drift`。
   不要分两次加字段（每次都触发全量重生成与 static_assert 调整）。
4. **gfx morph 先做"只位置"路径**（主 pass 的 `vs`/`vs1`），确认 `@builtin(vertex_index)` 与 storage 绑定
   在 wgpu-native + emdawnwebgpu 两边都能过；再补蒙皮入口与阴影入口。
5. **runtime 切默认路线**：`caps.morph_max_targets >= 8` 时默认 GPU；保留 `MAQUE_MORPH_GPU=0`。
   删除 `morph_owner` 的"首实体"限制只在 GPU 路径生效（CPU 回退仍需要它，别一起删）。
6. **粒子实例化替换动态网格**：先 1 个系统、1 张贴图、alpha 混合跑通；再补 additive/排序/剔除。
   替换完成后再删 `Particle3DMesh`（不留"旧路径"开关：多一条并行渲染路径的维护成本高于 git 回滚）。
7. **e2e 全绿后**再补 M2 字段（`local_space`/`shape`/`burst`/方差）。字段默认值必须让"只有一个发射器 +
   默认参数"的场景与 M1 逐字节一致。
8. **M3 的 GPU 解析模拟单独 PR**：先写文档与开关（默认关），再实现；验收里必须有一条
   "开 GPU 模拟时 `world_checksum` 与关粒子时相同"的断言，防止有人顺手把它混进模拟状态。
9. **每步都跑**：`ctest -R 'codegen_drift|schema_imports|builtin_shaders|determinism'` +
   `ctest -R 'gltf_morph|particles3d|render3d_scene'`；schema/ABI 改动后额外跑一次全量 `ctest`。
10. **回滚策略**：morph 用 `MAQUE_MORPH_GPU=0` 即时回退；粒子实例化回退 = revert 该 PR
    （没有开关也必须是"删掉新管线就回到 M1-4 的网格路径"的结构，别把旧路径代码拆一半）。
