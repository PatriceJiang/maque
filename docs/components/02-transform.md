# 02 · 变换组件（Transform2D · Transform3D）

> 上游：`17 §2/§3`（组件设计约定与记录格式）、`06`（反射 = 组件形状唯一真相）、`12`（节点树 / 父节点 / 生命周期）。
> 归属模块：`core`（`world` 命名空间；变换是跨渲染 / 物理 / UI 的**基础能力**，无独立模块可依赖）。
> 依赖类型：`math::Vec2`、`math::Vec3`、`math::Quat`、`math::Mat3`、`math::Mat4`（均来自 `modules/core/data/schema/` 的 `math` 命名空间）。
>
> 本文件同时说明与现有 `math::Transform`（Vec3 版）的关系（§1）。

## 1. 与现有 `math::Transform` 的关系（结论）

现有 `schema/` 的 `math::Transform`（`position/rotation/scale: Vec3`，size 48）是一个**纯数学值类型（value type，POD）**，
**不是**可挂在节点上的组件：它没有 `type_id` 语义、没有父/世界缓存、没有对节点树的约定。

本设计给出如下结论：

1. **`math::Transform` 作为 3D 变换的数学值类型保留，建议重命名为 `math::Transform3D`**，与新增的 `math::Transform2D` 对称。
   - 二者都是**可 `memcpy` 的 POD 局部变换值**（local TRS），只描述"相对父节点"的一步变换。
   - 组件 `Transform2D` / `Transform3D` **内联（embed）**对应值类型为 `local` 字段——复用现有布局，避免重复定义。
2. **命名冲突可接受**：`math::Transform3D`（值类型）与 `world::Transform3D`（组件）分属不同命名空间，语义层次不同（值 vs 记录）。
   若评审倾向零歧义，备选是把值类型命名为 `math::TRS3` / `math::TRS2`；本文默认采用 `Transform2D/3D` 命名。
3. **不采用"组件字段直接平铺 position/rotation/scale"**：平铺会迫使我们为 2D/3D 各写一份字段表且与 `math` 值类型重复；
   内联 `local` 让"数学值"与"组件语义（缓存 / 序列化 / 生命周期）"分离，符合 `17 §2.5` 单一职责。

建议 `schema/` 变更（示意，落地按此）：

```yaml
- namespace: math
  class_list:
    - class: Transform2D             # 新增值类型
      size: 12
      fields:
        - { name: position, type: Vec2, default: "{0.0F,0.0F}" }
        - { name: rotation, type: float, default: 0.0F, attrs: { unit: radian } }
        - { name: scale,    type: Vec2, default: "{1.0F,1.0F}" }
    - class: Transform3D             # 由现有 Transform 重命名，布局不变
      size: 48
      fields:
        - { name: position, type: Vec3, default: "{0.0F,0.0F,0.0F}" }
        - { name: rotation, type: Vec3, default: "{0.0F,0.0F,0.0F}", attrs: { unit: radian, tooltip: "Euler XYZ" } }
        - { name: scale,    type: Vec3, default: "{1.0F,1.0F,1.0F}" }
```

---

## 2. Transform2D

- **归属模块 / 上游文档**：`core`（`world`）；`17 §3`、`12 §3/§5`。
- **说明**：把一个节点置于 **2D 空间**，是 Sprite / Camera2D / TileMap / 2D Collider / UI（Control 之外）等获得位姿的前提。
  - 维度由组件决定：挂 `Transform2D` 即 2D 节点（`12 §3`），**不存在** `Node2D` 基类。
  - 一颗节点树可混合 2D/3D，各自在不同渲染图层/相机下解释（`12 §5`）。
  - `rotation` 用单浮点弧度（2D 只需绕 Z 轴），比 Euler 三元组更省且无歧义。
- **依赖类型 / enum**：`math::Vec2`（position / scale）、`math::Mat3`（world 缓存）、语言内建 `float`（rotation）。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `local` | `math::Transform2D` | 单位（pos 0、rot 0、scale 1） | | 相对**父节点**的局部 TRS；唯一被序列化的真值 |
| `world` | `math::Mat3` | 单位矩阵 | `transient` | 运行期派生的世界矩阵缓存；不序列化 |
| `world_dirty` | `uint8_t`(bool) | `1` | `transient` | 脏标记：父/自身 local 变更时置位，驱动传播 |

- **句柄 / 引用**：**不含任何 `entity_ref`**。父节点不由组件表达，而**隐含于节点树**（`12 §2` 的 `parent` 字段）。
  结论：**父用节点树 parent 隐含，组件内不存父句柄**——避免"树 parent"与"组件 parent"双真相源（`12 §9` 语义漂移风险）。
- **生命周期 / 确定性**：
  - 世界矩阵派生：每帧（或脏标记触发）对节点树做**深度优先**遍历，`world(child) = world(parent) × local`。
  - 传播顺序必须确定：按 `12 §6` 的**深度优先 + 声明顺序**；根节点先于子节点、兄弟按 children 声明序。
  - 与物理/渲染的读取顺序：同一帧内**先完成全部 transform 传播**，再进入消费系统（物理 `physics_process`、渲染提取），保证一致视图。
  - 不引入墙钟/熵；2D 不涉及随机。
- **序列化**：仅 `local` 序列化（稳定、可 diff）；`world` / `world_dirty` 为 `transient`，加载后重算。
  - 层级由场景自身的嵌套结构表达，`transform` 无需额外 parent 字段（见上）。

---

## 3. Transform3D

- **归属模块 / 上游文档**：`core`（`world`）；`17 §3`、`12 §3/§5`。
- **说明**：把一个节点置于 **3D 世界**，是 MeshRenderer / Camera3D / Light / 3D Collider / Skeleton / NavAgent 等获得位姿的前提。
  - `local` 内联复用 `math::Transform3D`（见 §1），即现有 `math::Transform` 的值布局，迁移成本近零。
  - `rotation` 序列化/编辑用 **Euler XYZ（弧度，`Vec3`）**，与现网 schema 一致、人类可读、可 diff；
    派生计算时转 `math::Quat`，缓存于 `rotation_quat`（`transient`），供插值/法线变换等使用。
- **依赖类型 / enum**：`math::Vec3`、`math::Quat`、`math::Mat4`；复用 `math::Transform3D` 值类型。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `local` | `math::Transform3D` | 单位（pos 0、rot 0、scale 1） | | 相对**父节点**的局部 TRS；唯一序列化真值 |
| `world` | `math::Mat4` | 单位矩阵 | `transient` | 运行期派生的世界矩阵缓存；不序列化 |
| `rotation_quat` | `math::Quat` | 单位四元数 | `transient` | 由 `local.rotation` 派生的四元数缓存；不序列化 |
| `world_dirty` | `uint8_t`(bool) | `1` | `transient` | 脏标记，驱动传播 |

- **句柄 / 引用**：**不含任何 `entity_ref`**（同 §2）。父节点隐含于节点树，理由同上。
- **生命周期 / 确定性**：
  - 传播：深度优先遍历节点树，`world(child) = world(parent) × compose(local)`；`rotation_quat` 随 `local.rotation` 派生。
  - 顺序确定：**深度优先 + 声明顺序**（`12 §6`）；每帧先算完全部 transform，再交物理/渲染消费。
  - `Quat` 仅作为**派生缓存**，不引入非确定性；欧拉→四元的转换在固定点（脏标记触发）执行，结果稳定可复现（`10` 快照友好）。
- **序列化**：仅 `local` 序列化；`world` / `rotation_quat` / `world_dirty` 均 `transient`，加载或反序列化后按需重算。
  - 与 `Transform2D` 一致：层级由场景嵌套表达，组件不冗余存父引用。

---

## 4. 二者共用契约（速查）

| 主题 | 结论 |
|------|------|
| 值 vs 组件 | `local` = 内联的 `math::Transform2D/3D` 值；组件只加"缓存 + 脏标记 + 语义" |
| 父节点 | **隐含于节点树 `parent`**，组件内**不**放 `entity_ref`（单一真相源） |
| 世界缓存 | `world` / `rotation_quat` / `world_dirty` 一律 `transient`，不序列化 |
| 序列化 | 只写 `local`；层级由场景嵌套结构承载 |
| 传播顺序 | 深度优先 + 声明顺序，全局确定（`12 §6`、`10`） |
| 维度选择 | 挂哪个组件即哪个维度，禁止节点继承（`12 §3`） |
| 配对 | `Sprite/MeshRenderer/Camera*/Collider/…` 需要对应 Transform（`17 §4`） |
| 模块中立 | 布局由反射统一读写，`core` 提供传播，`renderer/physics` 只消费 |
