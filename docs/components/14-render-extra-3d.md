# 14 · 3D 渲染扩展组件

> 归属模块：`renderer`（文档 01/13）。
> 上游：`03-render.md`（渲染组件通用约定与单向数据流）、`13`（WebGPU / T1 pass 层 / DrawList）、
> `06`（反射 = 组件形状唯一真相）、`07`（句柄 / asset_ref 强引用计数）、`08`（资产加载）、
> `assets/01-material.md`（材质资产）、`18 §2C/§3`（缺失清单与取舍）。
> 本文补齐 `18` 标记的 **3D 渲染缺口**：`Billboard`、`ReflectionProbe`、`Skybox/Environment`、`Terrain`。
> 全部为**纯渲染组件**：只描述"画什么/怎么算光照"，**绝不回写模拟**（ADR-0005，13 §8）。

## 0. 通用约定（同 `03-render.md` §0，从略）

- **单向数据流**：模拟 → bake → 提取（cull/batch）→ DrawList → Render Graph。本文组件只读入提取。
- **维度归属**：全部归 **3D 世界**分支（`RenderLayer.kind = World3D`），需 `Transform3D` 配对。
- **配对校验**：缺失 `Transform3D` = 校验错误；运行期提取时跳过该节点（不崩溃）。
- **`transient` 派生数据**：探针烘焙图、地形 LOD / 高度缓存、环境 IBL 预计算、天空盒立方体贴图
  **一律 `transient`**，运行时由引擎重建，反序列化后不依赖已烘焙结果。
- **引用一律句柄**：`asset_ref<T>`（强引用计数，07 §6/08 §5）、`entity_ref`（弱，世代检测悬垂）。
  **禁裸指针、`std::` 容器、隐式所有权**（17 §2.2）。

---

## Billboard

- **归属模块 / 上游文档**：`renderer`；`13 §4/§5`、`18 §2C`（高频补全项）。
- **说明**：让所在节点（通常带 `MeshRenderer` / `ParticleEmitter`）在提取阶段**朝向相机**绘制，
  用于公告板叶片、光晕、粒子、植被卡等。**只改提取期的中性绘制变换**，不改节点在模拟中的
  `Transform3D`（保持模拟确定性）。挂载后由渲染器读取相机视图矩阵并生成朝向，**不回写**。
  与 `MeshRenderer` / `ParticleEmitter` **配对使用**（决定"把谁朝相机"）。
- **依赖类型 / enum**：

```yaml
- enum: BillboardFacing     # 朝向模式
  values:
    - { name: FullScreen,      value: 0 }   # 完整朝向相机（含 roll）
    - { name: YAxisLocked,     value: 1 }   # 仅绕世界 Y 轴转向（植被/角色，常用）
    - { name: ViewPlane,       value: 2 }   # 面向视平面（保留屏幕对齐，用于光晕）
    - { name: LookAtCamera,    value: 3 }   # 仅朝向相机位置，不重定向 up
- enum: BillboardPivot
  values: [ { name: Center, value: 0 }, { name: Bottom, value: 1 } ]  # Bottom 常用于地面植被
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `facing` | `BillboardFacing` | `YAxisLocked` | | 朝向模式（见 enum） |
| `pivot` | `BillboardPivot` | `Center` | | 旋转支点：绕中心还是底面 |
| `lock_scale` | `bool` | `false` | | true 则缩放不随相机距离变化（配合 `scale_ref`） |
| `align_up` | `Vec3` | `{0,1,0}` | | `ViewPlane`/`LookAtCamera` 时的 up 参考轴 |
| `target_camera` | `entity_ref` | 空 | `ref: Camera3D`，`weak` | 指定朝哪台相机；空则用本 `RenderLayer` 生效相机 |
| `enabled` | `bool` | `true` | | 局部开关（不改节点 `Active`） |

- **句柄/引用**：`target_camera` 为**弱** `entity_ref`（可跨场景，世代检测悬垂，07 §6）；无资产引用。
- **生命周期 / 确定性**：仅在**提取阶段**读取相机视图矩阵计算朝向，结果进入 DrawList（`transient`）。
  **不改写** `Transform3D` 或任何模拟字段；模拟 checksum 不含本组件（10 §确定性）。
- **序列化**：字段表全部持久化；朝向结果矩阵、缓存相机引用 **`transient`**。

---

## ReflectionProbe

> **可选 / 可延后**：与"麻雀虽小"定位及嵌入式 / WASM 目标冲突较大，列为**可选**（`18 §3`）。
> 默认世界用环境 HDRI（见 §Skybox/Environment）即可；本组件按需启用。

- **归属模块 / 上游文档**：`renderer`；`13 §5/§6`（离屏目标 / 管线）、`18 §2C/§3`。
- **说明**：在场景某点烘焙**立方体反射贴图**（cube map），供附近 `MeshRenderer` 的 PBR 材质采样，
  实现局部反射 / 烘托金属与光滑材质。分**烘焙**与**实时**两种来源。烘焙在编辑期或加载期离屏执行，
  运行期只读采样；实时探针按 `update_mode` 在帧内产一张立方体贴图。**只影响渲染，不回写模拟**。
  需 `Transform3D` 配对（探针位置 / 影响盒）。与 `MeshRenderer.material` 的反射通道配对。
- **依赖类型 / enum**：

```yaml
- enum: ProbeSource            # 反射来源
  values:
    - { name: Baked,     value: 0 }   # 烘焙一次，运行期静态采样（默认，最省）
    - { name: Realtime,  value: 1 }   # 实时渲染立方体贴图
- enum: ProbeUpdateMode        # 实时刷新策略
  values:
    - { name: OnLoad,    value: 0 }   # 加载 / 手动触发时刷新
    - { name: EveryFrame, value: 1 }  # 每帧刷新（最贵）
    - { name: Interval,  value: 2 }   # 按 interval 秒刷新
- enum: ProbeShape             # 影响范围形状
  values: [ { name: Sphere, value: 0 }, { name: Box, value: 1 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `source` | `ProbeSource` | `Baked` | | 烘焙 / 实时来源 |
| `update_mode` | `ProbeUpdateMode` | `OnLoad` | | 实时刷新策略（`Baked` 时忽略） |
| `interval` | `float` | `1.0` | `range:(0,]` `unit: s` | `Interval` 模式刷新间隔 |
| `resolution` | `uint16_t` | `128` | `range:[32,1024]`, `enum:[32,64,128,256,512,1024]` | 立方体贴图单面边长（像素） |
| `shape` | `ProbeShape` | `Sphere` | | 影响范围形状 |
| `radius` | `float` | `10` | `range:(0,]` `unit: m` | 球影响半径（`shape=Sphere`） |
| `box_size` | `Vec3` | `{10,10,10}` | `unit: m` | 盒影响半尺寸（`shape=Box`） |
| `blend_distance` | `float` | `1` | `range:[0,]` `unit: m` | 影响边缘融合带（避免突变） |
| `intensity` | `float` | `1` | `range:[0,]` | 反射强度调制 |
| `priority` | `int32_t` | `0` | | 影响区重叠时数值大者优先 |
| `culling_mask` | `uint32_t` | `0xFFFFFFFF` | `bitmask` | 烘焙/实时捕获的可见性剔掩码 |
| `layer` | `entity_ref` | 空 | `ref: RenderLayer`，`weak` | 所属世界层；空则默认世界层 |
| `baked_cubemap` | `asset_ref<Texture>` | 空 | `ref: Texture`，`transient` | 烘焙产物（运行期派生，不入库） |

- **句柄/引用**：`layer` 弱 `entity_ref`；`baked_cubemap` 为**运行时 GPU 资源**，标 `transient`
  （烘焙缓存不回写模拟、不参与 checksum）；影响盒来自同节点 `Transform3D`。
- **生命周期 / 确定性**：烘焙 / 实时捕获在**提取期之后**离屏执行，**不参与模拟步进**；更新顺序按
  `priority` 降序、`entity` id 升序破平（同帧确定）。相同输入 → 相同贴图（渲染 pass 本身无随机）。
- **序列化**：字段表持久化；`baked_cubemap`、实时捕获目标、影响区加速结构 **`transient`**。

---

## Skybox / Environment

> 天空盒本身**可延后**（`18 §3`）；但**环境光照来源**作为场景级资产应尽早定义（默认关闭 IBL 亦可）。
> 与"麻雀虽小"冲突点在于 HDRI 预计算（辐照度 / 预过滤 / BRDF LUT）的成本；嵌入式 / WASM 可退化为
> **纯天空盒贴图 + 固定环境色**，不启用 IBL。

- **归属模块 / 上游文档**：`renderer`；`13 §4`（`Camera3D.clear_mode = Skybox`）、`assets/01`、`18 §2C/§3`。
- **说明**：提供场景**背景天空盒**与**环境光照（IBL）**。既是 `Camera3D.clear_mode = Skybox` 时绘制的
  背景，也是 PBR 材质的间接漫反射 / 镜面环境来源。通常**每场景一个**，作为**场景级单例**语义
  （同一 `RenderLayer` 内至多一个 active；冲突由校验告警，12 §8）。**只影响渲染，不回写模拟**。
  天空盒绘制在**提取期**产出 DrawList 项；环境贴图供材质采样。

  > 说明：本组件可挂在**专用节点**上（带 `Transform3D` 决定环境朝向），也可由场景/世界设置引用
  > 一份 `Environment` 资产。二者取其一；建议默认走**场景级资产**，需要旋转天空盒时再挂节点。

- **依赖类型 / enum**：

```yaml
- enum: EnvironmentSource      # 环境光照来源
  values:
    - { name: None,      value: 0 }   # 无环境光照（仅用清屏色）
    - { name: Color,     value: 1 }   # 单一环境色（最省，嵌入式 / WASM 推荐）
    - { name: Gradient,  value: 2 }   # 天空/地面两色渐变
    - { name: Texture,   value: 3 }   # 等距圆柱（equirect）HDRI 贴图 → 运行时转立方图
- enum: SkyboxMode
  values: [ { name: Procedural, value: 0 }, { name: Cubemap, value: 1 },
            { name: Equirect, value: 2 }, { name: Color, value: 3 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `skybox_mode` | `SkyboxMode` | `Color` | | 背景绘制方式 |
| `skybox_texture` | `asset_ref<Texture>` | 空 | `ref: Texture` | 天空盒贴图（Cubemap / Equirect 用；`asset_ref` 存句柄，序列化为稳定路径） |
| `skybox_color` | `Color` | `{0.05,0.05,0.08,1}` | | `Color` 模式背景色 |
| `skybox_rotation` | `float` | `0` | `range:[0,360]` `unit: deg` | 天空盒绕 Y 轴旋转 |
| `skybox_intensity` | `float` | `1` | `range:[0,]` | 背景亮度调制 |
| `env_source` | `EnvironmentSource` | `Color` | | 环境光照来源 |
| `env_texture` | `asset_ref<Texture>` | 空 | `ref: Texture` | HDRI（equirect）贴图；`Texture` 模式用 |
| `env_color` | `Color` | `{0.2,0.2,0.22,1}` | | 单色环境光（`Color` 模式） |
| `env_sky_color` | `Color` | `{0.3,0.4,0.6,1}` | | 渐变上半（`Gradient` 模式） |
| `env_ground_color` | `Color` | `{0.2,0.18,0.15,1}` | | 渐变下半（`Gradient` 模式） |
| `env_intensity` | `float` | `1` | `range:[0,]` | 环境光照强度 |
| `diffuse_contribution` | `float` | `1` | `range:[0,1]` | 漫反射（辐照度）贡献占比 |
| `specular_contribution` | `float` | `1` | `range:[0,1]` | 镜面（预过滤）贡献占比 |
| `priority` | `int32_t` | `0` | | 同层多组件时数值大者生效 |
| `active` | `bool` | `true` | | 参与单例择一 |

- **句柄/引用**：`skybox_texture` / `env_texture` 为**强** `asset_ref<Texture>`（引用计数控制存活，
  07 §6/08 §5）。运行时由其派生的**立方图 / 辐照度 / 预过滤 / BRDF LUT** 均为 **`transient`** GPU
  资源，不入库、不参与 checksum。
- **生命周期 / 确定性**：单例择一在提取前确定（`priority` 降序、`entity` id 升序破平）。HDRI 预计算
  为**加载期**一次性烘焙，运行期只读；不改写任何模拟字段。
- **序列化**：字段表持久化；贴图写稳定 ID/路径；**IBL 预计算产物、立方图转换结果 `transient`**。

---

## Terrain

> **可选 / 可延后**：地形属于重资产（高度场 + LOD + 大世界流式），与"麻雀虽小"及嵌入式 / WASM
> 目标冲突较大，列为**可选**（`18 §3`）。轻量需求可用 `MeshRenderer` + `TileMap` 样式方案替代。

- **归属模块 / 上游文档**：`renderer`；`13 §5`（实例化 / 剔除）、`08`（Terrain 数据资产）、`18 §2C/§3`。
- **说明**：基于**高度场 + 分层材质**的大幅面地表绘制。数据与外观分离：本组件只**引用地形数据资产**
  并声明 LOD / 图层混合参数，具体高度、图层权重来自 `asset_ref<TerrainData>`。提取阶段按相机距离
  做 **LOD 选择 / 分块剔除**，产出网格块绘制项（`transient` 烘焙缓存）。**只读消费，不回写模拟**；
  若需与物理交互（地形碰撞），由 physics 侧另建专用数据（见 05），本组件不承担物理语义。需 `Transform3D`。
- **依赖类型 / enum**：

```yaml
- enum: TerrainLodMode         # LOD 策略
  values:
    - { name: Auto,       value: 0 }   # 按相机距离自动分块降级（默认）
    - { name: Fixed,      value: 1 }   # 固定 LOD 层级（用于编辑 / 预览）
- struct: TerrainLevel        # LOD 层级声明
  size: 16
  fields:
    - { name: level,        type: "uint8_t" }     # 层级索引（0=最高精度）
    - { name: max_distance, type: "float" }       # 该层生效的最大相机距离（m）
    - { name: resolution,   type: "uint16_t" }    # 该层高度采样密度（顶点/块边）
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `terrain_data` | `asset_ref<TerrainData>` | 空 | `ref: TerrainData` | 高度场 + 图层权重数据资产；空则跳过 |
| `layer_material` | `asset_ref<Material>` | 空 | `ref: Material` | 地表材质（遮挡贴图 / 图层混合由材质定义，见 `assets/01`） |
| `tile_size` | `float` | `16` | `range:(0,]` `unit: m` | 单块世界尺寸（分块剔除 / LOD 粒度） |
| `lod_mode` | `TerrainLodMode` | `Auto` | | LOD 策略 |
| `lod_levels` | `TerrainLevel[]` | 由资产派生 | | LOD 层级表（`Auto` 按距离匹配；`Fixed` 取 `fixed_lod`） |
| `fixed_lod` | `uint8_t` | `0` | `range:[0,]` | `Fixed` 模式下的固定层级 |
| `lod_bias` | `float` | `0` | `range:[0,]` | LOD 偏移（正值更早降级，更省） |
| `height_scale` | `float` | `1` | | 高度缩放（相对归一化高度场） |
| `cast_shadow` | `bool` | `true` | | 地表投射阴影 |
| `receive_shadow` | `bool` | `true` | | 地表接收阴影 |
| `visible` | `bool` | `true` | | 局部可见开关（不改节点 `Active`） |
| `culling_mask` | `uint32_t` | `0xFFFFFFFF` | `bitmask` | 与相机 `culling_mask` 求交 |
| `layer` | `entity_ref` | 空 | `ref: RenderLayer`，`weak` | 所属世界层；空则默认世界层 |
| `lod_chunks` | `uint16_t[]` | `[]` | `transient` | 运行期分块 LOD 选择结果（派生缓存，不入库） |

- **句柄/引用**：`terrain_data` / `layer_material` 为**强** `asset_ref`（引用计数，07 §6）；`layer` 为
  **弱** `entity_ref`。世界变换来自同节点 `Transform3D`（含缩放影响 `tile_size` 的最终投影）。
- **生命周期 / 确定性**：LOD 选择 / 分块在**提取阶段**按相机位置计算，结果进入 DrawList（`transient`）；
  `lod_levels` 若未显式配置则由 `terrain_data` **加载期派生**。相同相机 + 相同数据 → 相同分块选择；
  不引入墙钟 / 系统熵。**不回写** `Transform3D` 或 `terrain_data`。
- **序列化**：字段表持久化（`lod_levels` 可省略，用资产默认）；`lod_chunks`、烘焙网格块、加速结构
  **`transient`**；`terrain_data` 写稳定 ID/路径。

---

## 与 13 的对接速览

| 组件 | 图层/分支 | 输入提取 | 输出中性项 | 回写模拟 |
|------|-----------|----------|------------|:--------:|
| `Billboard` | 3D 世界 | 相机视图矩阵 + 节点变换 | 朝向后的绘制变换 | 否 |
| `ReflectionProbe` | 3D 世界 | 位置/范围/`priority` | 立方反射贴图（离屏烘焙/实时） | 否 |
| `Skybox/Environment` | 3D 世界 | 贴图/环境色 | 背景 DrawList 项 + IBL 环境贴图 | 否 |
| `Terrain` | 3D 世界 | 高度场资产 + 相机 | 分块 LOD 网格块 | 否 |

> **取舍提示（`18 §3`）**：
> - **优先实现**：`Billboard`（真实项目几乎必用，成本低）。
> - **可选 / 可延后**：`ReflectionProbe`、`Skybox/Environment`、`Terrain`——
>   与"麻雀虽小"及嵌入式 / WASM 目标冲突较大；嵌入式 / WASM 上可退化为无 IBL 的天空盒 / 环境色、
>   无实时反射探针、无地形（用网格代替）。
> 全部组件均为 `renderer` 模块解释、**布局中立**、可被反射统一读写（17 §2.6）；渲染器仅消费
> **可序列化的绘制描述**，后端不可用或渲染失败**不回写**任何模拟字段（13 §8）。
