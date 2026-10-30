# 03 · 渲染组件（2D/3D）

> 归属模块：`renderer`（文档 01/13）。
> 上游：`13`（WebGPU / T1 pass 层 / DrawList / 分层 2D·3D·UI）、`12 §5`（根级 layer/viewport）、
> `06`（反射 = 组件形状唯一真相）、`07`（句柄/asset_ref 强引用计数）、`08`（资产加载）。
> 本文定义 8 个渲染组件的**数据形状**；渲染解释与 pass 编排见文档 13，
> RenderTexture 完整使用手册见文档 33。

## 0. 通用约定（本类共同遵守）

- **单向数据流**：模拟（节点树/ECS）→ **bake** → 提取（cull/batch）→ DrawList → Render Graph。
  渲染是模拟的**只读消费方**；组件**只描述"画什么"**，**绝不回写模拟状态**（ADR-0005，13 §8）。
- **提取阶段只读**：渲染器读取组件产生**中性绘制项**（mesh/材质/变换/layer），不修改组件字段。
- **维度由组件决定**：`Sprite/TileMap/Camera2D` 归 **2D 图层**分支，`Camera3D/MeshRenderer/Light` 归
  **3D 世界**分支，`RenderLayer`（kind=UI）归 **UI 层**（12 §5，13 §4）。节点可自由混合，禁止继承节点基类。
- **配对校验**：`Sprite/TileMap/Camera2D` 要求同节点存在 `Transform2D`；`Camera3D/MeshRenderer/Light`
  要求 `Transform3D`。缺失 = 校验错误（编辑期报错，运行期提取时跳过该节点）。
- **全局/单例语义**：`Camera2D/Camera3D` 每图层**建议唯一 active**；同一 `RenderLayer` 内多相机按
  `priority` 择一，冲突由校验告警（12 §8）。同节点挂 `RenderTexture` 的相机退出主相机竞争，只驱动离屏目标。
- **缓存字段一律 `transient`**：世界矩阵缓存、提取结果（DrawList 项）、烘焙网格、pass 中间目标
  **不序列化、运行时派生**，反序列化后由引擎重建。

---

## Sprite

- **归属模块 / 上游文档**：`renderer`；`13 §4/§5`、`12 §5`。
- **说明**：2D 贴图/精灵绘制基元，进入 **2D 图层**的精灵批处理（batcher）。挂在带 `Transform2D`
  的节点上；多个 Sprite 由提取阶段按纹理/混合状态**批处理**（13 §5）。不能与 `MeshRenderer` 表达同一
  节点的"2D vs 3D"——由挂哪个组件决定（12 §3）。
- **依赖类型 / enum**：

```yaml
- enum: FlipMode
  values: [ { name: None, value: 0 }, { name: Horizontal, value: 1 },
            { name: Vertical, value: 2 }, { name: Both, value: 3 } ]
- enum: PivotAnchor           # 便于编辑器对齐；pivot 为归一化偏移
  values: [ { name: TopLeft, value: 0 }, { name: Center, value: 1 },
            { name: BottomCenter, value: 2 }, { name: Custom, value: 3 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `texture` | `asset_ref<Texture>` | 空 | `ref: Texture` | 主纹理；空则提取跳过 |
| `region` | `Rect` | `{0,0,1,1}` | | 图集子矩形（归一化 UV）；默认整图 |
| `hframes` | `uint16_t` | `1` | `range:[1,1024]` | 横向切分数（精灵表） |
| `vframes` | `uint16_t` | `1` | `range:[1,1024]` | 纵向切分数 |
| `frame` | `uint16_t` | `0` | `range:[0,]` | 当前帧索引（`hframes*vframes` 内） |
| `flip` | `FlipMode` | `None` | | 水平/垂直翻转 |
| `color` | `Color` | `{1,1,1,1}` | | 顶点色（tint × alpha） |
| `pivot` | `Vec2` | `{0.5,0.5}` | `unit: normalized` | 锚点（受 `pivot_anchor` 影响） |
| `pivot_anchor` | `PivotAnchor` | `Center` | | 预设锚点；选 `Custom` 时用 `pivot` |
| `material` | `asset_ref<Material>` | 空 | `ref: Material` | 可选自定义 2D 材质；空用默认 |
| `z_index` | `int32_t` | `0` | | 层内排序；同 `RenderLayer` 内越小越先画 |

- **句柄/引用**：`texture`/`material` 为**强** `asset_ref`（引用计数控制存活，07 §6/08 §5）。
- **生命周期 / 确定性**：`frame` 等字段只读入提取；**不改写**。相同字段 + 相同 transform → 相同 DrawList 项。
- **序列化**：字段表全部持久化；纹理/材质写稳定 ID/路径（06 §3）。

---

## TileMap

- **归属模块 / 上游文档**：`renderer`；`13 §4/§5`、`08`（TileSet 资产）。
- **说明**：基于**网格**的 2D 瓦片绘制，进入 **2D 图层**；提取阶段按 `tileset` 烘焙为可批处理的
  图块网格（bake）。需 `Transform2D` 配对。数据与图集分离：本组件存**索引网格**，外观来自 `TileSet`。
- **依赖类型 / enum**：

```yaml
- struct: TileCoord          # 图块逻辑坐标（网格索引）
  size: 8
  fields:
    - { name: x, type: "int32_t" }
    - { name: y, type: "int32_t" }
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `tileset` | `asset_ref<TileSet>` | 空 | `ref: TileSet` | 图集资产；空则跳过 |
| `cell_size` | `Vec2` | `{16,16}` | `unit: px` | 单元格尺寸 |
| `columns` | `uint32_t` | `0` | `readonly`? | 网格宽（列数）；由 `tile_ids` 布局决定 |
| `rows` | `uint32_t` | `0` | | 网格高（行数） |
| `tile_ids` | `uint32_t[]` | `[]` | | 行优先扁平索引；`0` = 空，`n` = `TileSet` 内第 n 项 |
| `origin` | `TileCoord` | `{0,0}` | | 网格原点偏移（图块坐标） |
| `modulate` | `Color` | `{1,1,1,1}` | | 整体着色 |
| `z_index` | `int32_t` | `0` | | 层内排序 |
| `collision_source` | `asset_ref<TileSet>` | 空 | `deprecated` | 物理用（见 05）；渲染忽略 |

- **句柄/引用**：`tileset` 强 `asset_ref`；`tile_ids` 为**动态数组**（引擎 arena 容器，07 §7，禁 `std::`）。
- **生命周期 / 确定性**：编辑期改 `tile_ids` 置脏；提取阶段只读并**烘焙缓存**（`transient`）。
  遍历顺序为行优先且确定。
- **序列化**：`tile_ids`/`columns`/`rows`/`cell_size`/`tileset` 持久化；烘焙网格与脏标记 `transient`。

---

## Camera2D

- **归属模块 / 上游文档**：`renderer`；`12 §5`（图层相机）、`13 §4`。
- **说明**：为**2D 图层**提供视图变换（正交）。**全局/单例语义**：同一 `RenderLayer` 内**至多一个
  active** 生效相机，多者按 `priority` 择一，冲突告警（12 §8）。需 `Transform2D` 配对。
- **依赖类型 / enum**：无（2D 固定正交投影）。复用 `Vec2/Rect`。

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `active` | `bool` | `true` | | 是否参与择一；同层多相机时生效条件 |
| `priority` | `int32_t` | `0` | | 数值大者优先 |
| `zoom` | `Vec2` | `{1,1}` | `range:[0.01,]` | 缩放（越大越放大） |
| `offset` | `Vec2` | `{0,0}` | | 视图中心偏移（世界单位） |
| `viewport` | `Rect` | `{0,0,1,1}` | `unit: normalized` | 归一化视口；默认全屏 |
| `layer_mask` | `uint32_t` | `0xFFFFFFFF` | `bitmask` | 参与渲染的 `RenderLayer`/位掩码 |
| `clear_color` | `Color` | `{0,0,0,1}` | | 本层清屏色（若层 `clear=true`） |

- **句柄/引用**：无外部资产引用；视图变换来自同节点 `Transform2D`。
- **生命周期 / 确定性**：择一在提取前确定（按 `priority` 降序、`entity` id 升序破平）；不参与模拟。
- **序列化**：全部字段持久化；派生视图矩阵 `transient`（提取期重建，不回写）。

---

## Camera3D

- **归属模块 / 上游文档**：`renderer`；`12 §5`、`13 §4/§5`。
- **说明**：为**3D 世界**分支提供视图/投影。**全局/单例语义**同 `Camera2D`（同层择一）。需
  `Transform3D` 配对；相机朝向来自变换，不在此存旋转。
- **依赖类型 / enum**：

```yaml
- enum: CameraProjection
  values: [ { name: Perspective, value: 0 }, { name: Orthographic, value: 1 } ]
- enum: ClearMode
  values: [ { name: Skybox, value: 0 }, { name: Color, value: 1 },
            { name: DepthOnly, value: 2 }, { name: None, value: 3 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `active` | `bool` | `true` | | 同层择一条件 |
| `priority` | `int32_t` | `0` | | 数值大者优先 |
| `projection` | `CameraProjection` | `Perspective` | | 投影类型 |
| `fov_y` | `float` | `60` | `range:(0,179]` `unit: deg` | 透视：垂直视场角 |
| `ortho_height` | `float` | `2` | `range:(0,]` | 正交：视口世界高度 |
| `near` | `float` | `0.1` | `range:(0,]` | 近裁剪面 |
| `far` | `float` | `1000` | `range:(near,]` | 远裁剪面 |
| `viewport` | `Rect` | `{0,0,1,1}` | `unit: normalized` | 归一化视口（分屏/画中画） |
| `culling_mask` | `uint32_t` | `0xFFFFFFFF` | `bitmask` | 可见性剔掩码 |
| `clear_mode` | `ClearMode` | `Skybox` | | 清屏方式 |
| `clear_color` | `Color` | `{0,0,0,1}` | | `Color` 模式下的清屏色 |
| `exposure` | `float` | `1` | `range:(0,]` | 曝光（HDR 色调映射；13 §5） |

- **句柄/引用**：无资产引用；`Skybox` 由场景/世界环境资产提供（见 08）。
- **生命周期 / 确定性**：择一同 `Camera2D`；投影矩阵为提取期派生 `transient`，不回写。
- **序列化**：字段表持久化；`view/proj` 矩阵 `transient`。

---

## RenderTexture

- **归属模块 / 上游文档**：`renderer`；`13 §6.1`、[33](../33-RenderTexture相机离屏纹理.md)。
- **说明**：与 `scn.Camera`、`render.Camera2D` 或 `render.Camera3D` **同节点挂载**，把该相机输出变成
  可被 Sprite、2D mesh 或 PBR 颜色贴图槽采样的 GPU 纹理。同节点相机不再参与主画面相机选择。
- **依赖类型 / enum**：

```yaml
- enum: RenderTextureUpdateMode
  values: [ { name: EveryFrame, value: 0 }, { name: OnDemand, value: 1 } ]
- enum: RenderTextureFilter
  values: [ { name: Nearest, value: 0 }, { name: Linear, value: 1 } ]
```

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `name` | `string` | `""` | | 实例内唯一名；空时用节点名，再空则用 `camera-<id>` |
| `width` | `uint32_t` | `0` | `unit:px` | 目标宽；0 跟随场景设计宽度 |
| `height` | `uint32_t` | `0` | `unit:px` | 目标高；0 跟随场景设计高度 |
| `update_mode` | `RenderTextureUpdateMode` | `EveryFrame` | | 每帧更新或按需更新 |
| `filter` | `RenderTextureFilter` | `Linear` | | 普通纹理采样过滤 |
| `include_ui` | `bool` | `false` | | 允许 Camera2D 收集游戏 UI；仍受 `layer_mask` 限制 |
| `enabled` | `bool` | `true` | | 自动提交开关；关闭时保留上次内容 |
| `clear_color` | `Color` | `{0,0,0,0}` | | `scn.Camera` 路径清屏色；新相机用自身 clear color |
| `texture` | `string` | `""` | `transient,readonly` | 运行期规范 URI：`rendertexture://<instance>/<name>` |
| `ready` | `bool` | `false` | `transient,readonly` | 至少成功渲染一次 |
| `last_frame` | `uint32_t` | `0` | `transient,readonly` | 最后成功更新帧号 |

- **句柄/引用**：场景持久化只保存逻辑 name 和短 URI `rendertexture://name`；GPU 句柄不公开、不序列化。
  短 URI 按消费者所属 SceneInstance 解析，子场景同名目标互不串用。
- **生命周期 / 确定性**：创建/resize 后先确定性清屏；依赖按生产者到消费者排序；自采样批次剔除；
  环按 entity id 稳定顺序使用本帧/上一帧。resize 保持纹理句柄稳定，节点/组件/场景销毁统一释放。
- **序列化**：除 `texture/ready/last_frame` 外全部持久化；RenderTexture 属视图资源，不进入模拟 checksum。
- **截图**：脚本用 `camera_texture/camera_update/camera_capture`；控制面用 `Capture.camera` 与
  `Capture.camera_save`。详细分块协议和限制见文档 33。

---

## MeshRenderer

- **归属模块 / 上游文档**：`renderer`；`13 §2/§5`、`08`。
- **说明**：3D 网格绘制基元，进入 **3D 世界**分支（网格 + 实例化）。需 `Transform3D` 配对；可有
  多子网格材质覆盖。提取阶段产出 mesh/材质/变换/layer 的中性绘制项（13 §2）。
- **依赖类型 / enum**：

```yaml
- enum: ShadowCastMode
  values: [ { name: Off, value: 0 }, { name: On, value: 1 },
            { name: DoubleSided, value: 2 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `mesh` | `asset_ref<Mesh>` | 空 | `ref: Mesh` | 网格；空则跳过 |
| `material` | `asset_ref<Material>` | 空 | `ref: Material` | 默认材质（覆盖全部子网格） |
| `material_overrides` | `asset_ref<Material>[]` | `[]` | `ref: Material` | 按子网格索引的材质覆盖 |
| `cast_shadow` | `ShadowCastMode` | `On` | | 投射阴影模式 |
| `receive_shadow` | `bool` | `true` | | 是否接收阴影 |
| `visible` | `bool` | `true` | | 局部可见开关（不改节点 `Active`） |
| `culling_mask` | `uint32_t` | `0xFFFFFFFF` | `bitmask` | 与相机 `culling_mask` 求交 |
| `layer` | `entity_ref` | 空 | `ref: RenderLayer`，`weak` | 指定所属 `RenderLayer`；空则用默认世界层 |

- **句柄/引用**：`mesh`/`material`/`material_overrides` 强 `asset_ref`（引用计数，07 §6）；
  `layer` 为**弱** `entity_ref`（可跨场景，世代检测悬垂，07 §6/12 §3）。
- **生命周期 / 确定性**：提取只读；实例化数据由 gather 阶段聚合，不落地到组件。
- **序列化**：字段持久化；世界矩阵、实例化缓冲、DrawList 项 `transient`。

---

## Light

- **归属模块 / 上游文档**：`renderer`；`13 §4/§5`。
- **说明**：3D 世界光照。需 `Transform3D` 配对；方向来自变换（方向光用 -Z 朝向）。光照**只影响渲染**，
  不回写模拟。每类光源受可同时存在数量约束（实现上限，超限按 `priority` 裁剪）。
- **依赖类型 / enum**：

```yaml
- enum: LightType
  values: [ { name: Directional, value: 0 }, { name: Point, value: 1 },
            { name: Spot, value: 2 } ]
- enum: ShadowMode
  values: [ { name: Off, value: 0 }, { name: Hard, value: 1 }, { name: Soft, value: 2 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `type` | `LightType` | `Directional` | | 光源类型 |
| `color` | `Color` | `{1,1,1,1}` | | 光色（含强度调制） |
| `intensity` | `float` | `1` | `range:[0,]` `unit: lux/rel` | 强度；类型相关单位 |
| `range` | `float` | `10` | `range:(0,]` `unit: m` | 作用距离（点/聚光） |
| `inner_angle` | `float` | `20` | `range:[0,179]` `unit: deg` | 聚光内锥角 |
| `outer_angle` | `float` | `30` | `range:[0,179]` `unit: deg` | 聚光外锥角（≥`inner_angle`） |
| `shadow_mode` | `ShadowMode` | `Off` | | 阴影模式 |
| `shadow_bias` | `float` | `0.001` | | 阴影偏移（防自阴影） |
| `shadow_distance` | `float` | `50` | `range:(0,]` `unit: m` | 阴影最大距离 |
| `culling_mask` | `uint32_t` | `0xFFFFFFFF` | `bitmask` | 影响对象掩码 |
| `priority` | `int32_t` | `0` | | 超上限时的裁剪优先级 |
| `layer` | `entity_ref` | 空 | `ref: RenderLayer`，`weak` | 所属光照层；空则默认 |

- **句柄/引用**：`layer` 弱 `entity_ref`；无资产引用（阴影贴图为运行时 GPU 资源，非组件字段）。
- **生命周期 / 确定性**：光照聚合（光照列表/集群）在提取期只读生成，`transient`。
- **序列化**：字段持久化；阴影贴图、聚合缓冲 `transient`。

---

## RenderLayer

- **归属模块 / 上游文档**：`renderer`；`12 §5`（`CanvasLayer`/`Viewport`）、`13 §3/§4`。
- **说明**：把绘制分组为**画布/世界/UI 层**（一个 Render Graph 分支的锚点）。每个层对应 **T1 pass 层**
  的一个分支（13 §3），声明其相机、视口、离屏目标与合成顺序。**全局/单例语义**：一个层分组一处，
  层间合成顺序由 `order` 显式声明（13 §4）。组件本身是普通节点记录，无需 transform 配对。
- **依赖类型 / enum**：

```yaml
- enum: LayerKind
  values: [ { name: Canvas2D, value: 0 }, { name: World3D, value: 1 },
            { name: UI, value: 2 } ]
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `kind` | `LayerKind` | `World3D` | | 层类别（2D 画布 / 3D 世界 / UI） |
| `order` | `int32_t` | `0` | | 合成顺序；越小越先合成 |
| `visible` | `bool` | `true` | | 整层可见开关 |
| `camera` | `entity_ref` | 空 | `ref: Camera2D`/`Camera3D`，`weak` | 本层相机；与 `kind` 匹配校验 |
| `viewport` | `Rect` | `{0,0,1,1}` | `unit: normalized` | 本层视口（可覆盖相机 viewport） |
| `clear` | `bool` | `true` | | 是否清屏 |
| `clear_color` | `Color` | `{0,0,0,1}` | | 清屏色 |
| `target` | `asset_ref<RenderTarget>` | 空 | `ref: RenderTarget` | 离屏目标（13 §6）；空 = 交换链 |
| `follow_viewport` | `bool` | `true` | | 层变换是否跟随相机滚动（Godot CanvasLayer 语义） |
| `offset` | `Vec2` | `{0,0}` | `unit: px` | 层整体偏移（2D/UI 画布平移） |
| `blend` | `BlendMode` | `Alpha` | | 合成到父层时的混合模式 |

- **句柄/引用**：`camera` 弱 `entity_ref`（层不拥有相机）；`target` 强 `asset_ref`（离屏目标生命周期）。
- **生命周期 / 确定性**：`order` + `kind` 决定 pass 分支与合成顺序，**在提取前确定**（`order` 升序、
  `entity` id 升序破平），保证同帧确定；不在模拟中改动。
- **序列化**：字段持久化；pass 中间目标、readback 影像 **`transient`**（13 §6），绝不入库。

---

## 与 13 的对接速览

| 组件 | 图层/分支 | 输入提取 | 输出中性项 | 回写模拟 |
|------|-----------|----------|------------|:--------:|
| `Sprite` / `TileMap` | 2D 图层 | 纹理/索引网格 + Transform2D | 精灵/图块批 | 否 |
| `Camera2D` | 2D 图层 | `priority` 择一 | 正交视图矩阵 | 否 |
| `MeshRenderer` | 3D 世界 | mesh + 材质 + Transform3D | 网格/实例项 | 否 |
| `Camera3D` | 3D 世界 | `priority` 择一 | 视图/投影矩阵 | 否 |
| `RenderTexture` | 离屏相机分支 | 相机 + 可见 item/mesh/材质依赖 | 可采样颜色目标 | 仅 transient 状态 |
| `Light` | 3D 世界 | 光色/锥角/阴影 | 光照列表/集群 | 否 |
| `RenderLayer` | 分支锚点 | `order`/`kind`/相机 | pass 分支与合成 | 否 |

> 所有组件均为 `renderer` 模块解释、**布局中立**、可被反射统一读写（17 §2.6）；渲染器仅消费
> **可序列化的绘制描述**（13 §1），渲染失败或后端不可用**不回写**任何模拟字段（13 §8）。
