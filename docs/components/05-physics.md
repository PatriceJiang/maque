# 05 · 物理类内置组件

> 归属模块：**physics**（Rapier via C ABI FFI，ADR-0028；文档 15 §3）。
> 上游：`17`（组件约定 §2/§3）、`15 §3`（物理集成）、`10`（确定性/快照/回放）、`06`（反射 = 组件形状唯一真相）、`12`（双视图 / bake）、`07`（句柄 / 内存）。
> 组件清单：**RigidBody · Collider · CharacterBody · Area · Joint**。

---

## 0. 通用约定（本类所有组件共守）

### 0.1 模块归属与解释权

- 组件是**布局中立的反射记录**（文档 06），但**只有 physics 模块解释它们**：bake 时把组件翻译成 Rapier 的 `RigidBody` / `Collider` / `Joint` 等，运行期由 physics 系统在固定步内迭代。
- physics 模块经 **C ABI** 暴露句柄（文档 02）：C++ 侧**永不持有 Rapier 指针 / C++ 类型**，只持 `u64` 句柄；分配者（Rust）释放，异常不得越界。

### 0.2 2D / 3D 的表达（决策）

- **不设 `dimension` 字段**：维度由**同实体的 `Transform2D` / `Transform3D`** 决定（文档 12 §5「维度由组件决定」）。
- 组件字段**统一用 `Vec3`**：2D 只用 `x/y`，`z` 恒为 0；同一套 enum（`ShapeType`/`JointType`）覆盖 2D/3D，通过语义映射到 Rapier 的 2D/3D 世界：

| maque 语义 | 3D 映射 | 2D 映射 |
|------------|---------|---------|
| `Box` | Cuboid | Rectangle |
| `Circle` | Ball(Sphere) | Circle |
| `Capsule` | Capsule | Capsule |
| `TriMesh` | TriMesh | Polyline / 凸分解 |

- **理由**：单一真相（避免组件上 `dimension` 与 Transform 冗余/冲突）；组件保持维度无关，bake 时把实体的维度与 `entity id` 一并登记到 physics 模块的 **world 绑定表**（`world_slot`，transient）。
- 校验：一个实体**不可**同时挂 `Transform2D` 与 `Transform3D`；bake 期检测并报错。

### 0.3 权威状态与句柄（关键）

- **Rapier 侧句柄一律 transient、不序列化**：`body_handle` / `collider_handle` / `joint_handle` / `controller_handle`、以及 `world_slot` 都是运行时派生，随世界重建**重绑**（与文档 12 §7 的引用重绑同源）。
- **权威模拟态在 physics 模块的 world blob**：Rapier 的 `RigidBodySet` / `ColliderSet` / `JointSet`（含速度、休眠、接触缓存）经 Rust serde 序列化为**独立 blob**，随快照/回滚一起存取（文档 10 §4）。组件字段是**作者意图 + 每帧只读回读量**，不是第二份真相——因此组件里的速度/接触结果类字段标 `transient`。
- **引用类型**：`body_a` / `body_b` 用 `entity_ref`（`{id, generation}`），序列化为稳定 ID（文档 06 §3、12 §7）。
- **资产引用**：复杂形状（凸包/三角网/高度场）用 `asset_ref<Mesh>`，存句柄、序列化为路径。

### 0.4 生命周期与确定性

- **相位**：物理只在 `physics_process(dt)` 推进，独立于 `process(dt)`（文档 12 §6、15 §3）。dt 为**固定步长**（文档 10 D1），累加器吸收渲染帧波动。
- **顺序确定**：每步按 **`entity id` 稳定排序**迭代（文档 10 D4），**绝不按指针/地址**；固定迭代次数、确定性求解器；接触对按 id 稳定排序（文档 15 §3）。
- **单线程**：模拟单线程（文档 10 D6 / ADR-0018），worker 不碰模拟态。
- **RNG**：物理本身不引入随机；若脚本在 `physics_process` 用随机，必须走实例 RNG（文档 10 D2）。
- **读回变换**：每步结束后把刚体权威变换**只读**写回 `Transform2D/3D`；**渲染插值**用前两物理状态插值（文档 15 §3）——**插值结果不回写模拟**，只供提取→渲染（文档 13 §8）。
- **确定性需实测**：Rapier 求解顺序/浮点在 L1 下验证（文档 10 §6）；不达标退化为自研求解器（ADR-0028）。

### 0.5 序列化总则

- **场景/Prefab 资产**（文档 08）：只写**非 transient** 字段 = 作者意图（形状、质量、阻尼、摩擦、层级…）。
- **快照/回滚**（文档 10 §4）：实体逐字段快照 **+ physics world blob**（Rapier serde）；`transient` 字段不参与（重建后重绑/重算）。
- 反序列化旧数据：按名匹配 + 迁移钩子（文档 06 §7）；`transient` 字段忽略。

### 0.6 共享 enum

```yaml
- enum: BodyType          # 刚体行为
    values: [ {name: Static,    value: 0},
              {name: Kinematic, value: 1},
              {name: Dynamic,   value: 2} ]

- enum: ShapeType         # 2D/3D 同名语义（见 §0.2）
    values: [ {name: Box,         value: 0},
              {name: Circle,      value: 1},
              {name: Capsule,     value: 2},
              {name: Cylinder,    value: 3},   # 仅 3D
              {name: Convex,      value: 4},   # 凸包
              {name: TriMesh,     value: 5},   # 仅 Static
              {name: HeightField, value: 6} ]  # 仅 Static/3D

- enum: JointType
    values: [ {name: Fixed,     value: 0},
              {name: Revolute,  value: 1},   # 铰链/2D+3D
              {name: Prismatic, value: 2},   # 滑轨
              {name: Ball,      value: 3},   # 3D 球窝
              {name: Rope,      value: 4},
              {name: Spring,    value: 5},
              {name: Generic,   value: 6} ]

- enum: CombineRule       # 摩擦/弹性组合
    values: [ {name: Average,  value: 0},
              {name: Min,      value: 1},
              {name: Multiply, value: 2},
              {name: Max,      value: 3} ]

- enum: SpaceOverride     # Area 对重力的覆盖方式
    values: [ {name: Disabled, value: 0},
              {name: Combine,  value: 1},
              {name: Replace,  value: 2} ]
```

---

## 1. RigidBody

- **归属模块 / 上游**：physics / `15 §3`、ADR-0028。
- **说明**：给实体一个**动力学/运动学/静态体**。挂 `Collider` 才有形状参与碰撞；不挂 Collider 的 RigidBody 只作运动载体（bake 时按需生成默认形状）。维度由同级 `Transform2D/3D` 决定（§0.2）。是 `Collider` / `CharacterBody` / `Area` / `Joint` 的**体宿主**。
- **依赖类型**：`BodyType`；`Transform2D/3D`（必需，维度来源）。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `body_type` | `BodyType` | `Static` | | 安全默认：需显式声明 `Dynamic` 才受力学演化 |
| `mass` | `f32` | `1.0` | `unit:kg, range:[0.001,]` | 仅 `Dynamic` 有效；可由 Collider 密度推导覆盖 |
| `linear_damping` | `f32` | `0.0` | `unit:1/s, range:[0,]` | 线性阻尼 |
| `angular_damping` | `f32` | `0.0` | `unit:1/s, range:[0,]` | 角阻尼 |
| `gravity_scale` | `f32` | `1.0` | | 世界重力缩放，0 = 无重力 |
| `ccd_enabled` | `bool` | `false` | | 连续碰撞检测（快速子弹） |
| `can_sleep` | `bool` | `true` | | 允许休眠（确定，按 sim time 判据） |
| `locked_translation` | `u8` | `0` | `range:[0,7]` | 位掩码：bit0=x,bit1=y,bit2=z |
| `locked_rotation` | `u8` | `0` | `range:[0,7]` | 同上 |
| `linear_velocity` | `Vec3` | `0` | `transient, readonly` | 从 world blob 读回；**不逐字段序列化** |
| `angular_velocity` | `Vec3` | `0` | `transient, readonly` | 同上 |
| `sleeping` | `bool` | `false` | `transient, readonly` | 运行时派生 |
| `body_handle` | `u64` | `0` | `transient` | Rapier `RigidBodyHandle`；重建重绑 |
| `world_slot` | `u32` | `0` | `transient` | 所属 physics world 绑定槽 |

- **句柄/引用**：`body_handle` / `world_slot` transient（§0.3）；无 `asset_ref`。
- **生命周期/确定性**：`Static`/`Kinematic` 不受力，位置由脚本/父级驱动；`Dynamic` 每 `physics_process` 由 Rapier 积分。步末变换只读回写 `Transform`（§0.4）。
- **序列化**：`body_handle`/`world_slot`/速度/`sleeping` 均 transient；其余字段进场景资产。

---

## 2. Collider

- **归属模块 / 上游**：physics / `15 §3`。
- **说明**：给实体（或同级 `RigidBody`）一个**碰撞形状**。挂 `is_sensor=false` 参与物理解算；挂 `is_sensor=true` 则为触发器，由 `Area` 解释（§4）。无同级 RigidBody 时按 **Static** 语义（静态环境几何）。一个实体可有**多个 Collider**（复合形状），各带局部 `offset_*`。
- **依赖类型**：`ShapeType`、`CombineRule`；`asset_ref<Mesh>`（Convex/TriMesh/HeightField）；`RigidBody`（可选，缺省即静态）。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `shape_type` | `ShapeType` | `Box` | | 2D/3D 语义映射见 §0.2 |
| `half_extents` | `Vec3` | `(0.5,0.5,0.5)` | | `Box` 半尺寸；2D 用 `x/y` |
| `radius` | `f32` | `0.5` | `range:[0.001,]` | `Circle`/`Capsule`/`Cylinder` |
| `half_height` | `f32` | `0.5` | `range:[0,]` | `Capsule`/`Cylinder` 圆柱段半高 |
| `convex_mesh` | `asset_ref<Mesh>` | `null` | `ref:Mesh` | `Convex` 凸包源网格 |
| `trimesh` | `asset_ref<Mesh>` | `null` | `ref:Mesh` | `TriMesh`/`HeightField` 源 |
| `height_scale` | `Vec2` | `(1,1)` | | `HeightField` 采样缩放 |
| `density` | `f32` | `1.0` | `unit:kg/m³, range:[0,]` | 由体积推导质量（覆盖 `mass`） |
| `friction` | `f32` | `0.5` | `range:[0,]` | 库仑摩擦系数 |
| `restitution` | `f32` | `0.0` | `range:[0,1]` | 弹性/回弹 |
| `friction_combine` | `CombineRule` | `Average` | | 与对方摩擦的组合规则 |
| `restitution_combine` | `CombineRule` | `Average` | | 与对方弹性的组合规则 |
| `is_sensor` | `bool` | `false` | | `true` = 触发器（配 `Area`） |
| `collision_layer` | `u32` | `1` | | 所属层位掩码 |
| `collision_mask` | `u32` | `0xFFFFFFFF` | | 参与的过滤位掩码 |
| `offset_position` | `Vec3` | `0` | | 相对宿主体的局部平移 |
| `offset_rotation` | `Quat` | `identity` | | 相对宿主体的局部旋转 |
| `collider_handle` | `u64` | `0` | `transient` | Rapier `ColliderHandle`；重建重绑 |
| `owner_body` | `u64` | `0` | `transient` | 归属 RigidBody 句柄（缺省=静态体） |

- **句柄/引用**：`collider_handle`/`owner_body` transient；形状资产用 `asset_ref<Mesh>`（序列化为路径）。`collision_layer/mask` 为位掩码，bake 时映射到 Rapier 的 collision groups。
- **生命周期/确定性**：形状参数变更是**结构性变更**，需在步界（非帧中）重建 collider；`TriMesh`/`HeightField` 仅允许 `Static`。接触求解按 id 稳定排序（§0.4）。
- **序列化**：`collider_handle`/`owner_body` transient；形状与材质字段进资产。

---

## 3. CharacterBody

- **归属模块 / 上游**：physics / `15 §3`。
- **说明**：**运动学角色控制器**（Rapier `KinematicCharacterController`）。用于玩家/敌人等**由脚本完全驱动位移**的角色：不受力，脚本每 `physics_process` 写 `desired_velocity` 并调用 `move_and_slide()`，控制器做**碰撞滑动 / 台阶 / 贴地**。形状取自同实体 `Collider`（推荐 `Capsule`）；内部持一个 kinematic 体。
- **依赖类型**：`Collider`（形状来源）、`Transform2D/3D`。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `desired_velocity` | `Vec3` | `0` | | 期望位移速度；**每步由脚本写**（含水平移动） |
| `up_direction` | `Vec3` | `(0,1,0)` | | 判定"地面/天花板"的上方向 |
| `floor_max_angle` | `f32` | `45` | `unit:deg, range:[0,90]` | 可站立的坡度上限 |
| `step_height` | `f32` | `0.25` | `range:[0,]` | 可自动迈上的台阶高度 |
| `snap_length` | `f32` | `0.0` | `range:[0,]` | 下坡贴地吸附距离 |
| `max_slides` | `u32` | `4` | `range:[0,16]` | 单步滑动迭代上限 |
| `auto_apply_gravity` | `bool` | `true` | | 是否自动把重力并入水平(y)速度 |
| `gravity_scale` | `f32` | `1.0` | | 重力缩放（`auto_apply_gravity` 时生效） |
| `velocity` | `Vec3` | `0` | `transient, readonly` | 上一步的实际速度（含滑动/重力结果） |
| `on_floor` | `bool` | `false` | `transient, readonly` | 结果标志 |
| `on_wall` | `bool` | `false` | `transient, readonly` | 结果标志 |
| `on_ceiling` | `bool` | `false` | `transient, readonly` | 结果标志 |
| `floor_normal` | `Vec3` | `0` | `transient, readonly` | 地面法线 |
| `controller_handle` | `u64` | `0` | `transient` | Rapier 角色控制器句柄；重建重绑 |

- **句柄/引用**：`controller_handle` transient；形状通过 `Collider` 引用，不内联。
- **生命周期/确定性**：`move_and_slide()` 只在 `physics_process` 调用（帧中禁止）；内部迭代固定（`max_slides`），结果确定。步末变换只读回写 `Transform`（§0.4）。
- **序列化**：所有结果类字段（`velocity`/`on_*`/`floor_normal`/`controller_handle`）transient；配置字段进资产。**“上一步结果”不是第二真相**——权威态在 world blob。

---

## 4. Area

- **归属模块 / 上游**：physics / `15 §3`。
- **说明**：**触发/场区域**（Godot `Area` 语义）。用于检测进入/离开（拾取、传送门、检查点）与**覆盖重力/阻尼**（水、重力场）。形状来自同实体 `Collider`（`is_sensor=true`）；Area 不产生物理响应，只做**查询与信号**。区域重叠是**高基数**，bake 后走 ECS（文档 12 §4）。
- **依赖类型**：`Collider`（形状来源，须 sensor）、`SpaceOverride`。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `monitoring` | `bool` | `true` | | 是否检测外部体进入/离开 |
| `monitorable` | `bool` | `true` | | 是否可被其他 Area 检测到 |
| `priority` | `i32` | `0` | | 多区域叠加时的生效优先级 |
| `gravity_space_override` | `SpaceOverride` | `Disabled` | | 区域如何影响重力 |
| `gravity_direction` | `Vec3` | `(0,-1,0)` | | `Replace` 时的重力方向 |
| `gravity` | `f32` | `9.81` | `range:[0,]` | 区域内重力大小 |
| `linear_damping_override` | `f32` | `0.0` | `range:[0,]` | 区域内线性阻尼 |
| `angular_damping_override` | `f32` | `0.0` | `range:[0,]` | 区域内角阻尼 |
| `collision_layer` | `u32` | `1` | | 检测层位掩码 |
| `collision_mask` | `u32` | `0xFFFFFFFF` | | 检测过滤位掩码 |
| `overlap_count` | `u32` | `0` | `transient, readonly` | 当前重叠体数量（每步派生） |
| `area_handle` | `u64` | `0` | `transient` | 模块侧 Area 句柄；重建重绑 |

- **句柄/引用**：`area_handle` transient；形状经 `Collider` 引用；重叠对象列表**不内联于组件**（高基数），经模块查询 API 按需取（句柄列表）。
- **生命周期/确定性**：进入/离开事件在 `physics_process` 内按 **entity id 稳定排序**产生（文档 10 D4），保证信号顺序确定；同一实体一步内进/出需定义合并规则（默认按最终状态）。
- **序列化**：`overlap_count`/`area_handle` transient；配置字段进资产。

---

## 5. Joint

- **归属模块 / 上游**：physics / `15 §3`。
- **说明**：连接**两个刚体**的约束（铰链、滑轨、球窝、绳、弹簧…）。`body_a` 缺省为**自身**的 RigidBody，`body_b` 为另一实体；两体各自可有 `Collider`。关节是**结构性对象**，创建/销毁在步界完成。
- **依赖类型**：`JointType`；`entity_ref`（`body_a`/`body_b`）；`RigidBody`（两端）。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `joint_type` | `JointType` | `Fixed` | | 约束类型 |
| `body_a` | `entity_ref` | `self` | `ref:RigidBody` | 默认自身体 |
| `body_b` | `entity_ref` | `null` | `ref:RigidBody` | 另一端；`null` = 连到世界（静态锚） |
| `anchor_a` | `Vec3` | `0` | | A 体局部锚点 |
| `anchor_b` | `Vec3` | `0` | | B 体局部锚点 |
| `axis` | `Vec3` | `(0,1,0)` | | `Revolute`/`Prismatic` 的轴 |
| `limits_enabled` | `bool` | `false` | | 启用角度/位移限位 |
| `limit_min` | `f32` | `0` | | 下限（角度 rad / 位移 m） |
| `limit_max` | `f32` | `0` | | 上限 |
| `motor_enabled` | `bool` | `false` | | 启用马达 |
| `motor_target_vel` | `f32` | `0` | | 目标速度 |
| `motor_max_force` | `f32` | `0` | `range:[0,]` | 马达最大力/矩 |
| `break_force` | `f32` | `0` | `range:[0,]` | 超过则断裂（0 = 不断） |
| `break_torque` | `f32` | `0` | `range:[0,]` | 超过则断裂（0 = 不断） |
| `joint_handle` | `u64` | `0` | `transient` | Rapier `ImpulseJointHandle`；重建重绑 |

- **句柄/引用**：`joint_handle` transient；`body_a`/`body_b` 用 `entity_ref`（序列化为稳定 ID，实例化后重绑，文档 12 §7）。锚点/轴为 A/B 体**局部坐标**，随体变换。
- **生命周期/确定性**：两端任一变化/失效需重建关节；创建顺序按 `entity id` 稳定。马达/限位确定（无随机）。断裂事件在步内按 id 排序产生。
- **序列化**：`joint_handle` transient；`body_a`/`body_b` 序列化为稳定 ID；其余约束字段进资产。

---

## 6. 状态

| 组件 | 设计 | 评审 | 落地 |
|------|------|------|------|
| RigidBody | ⏳ | ⬜ | ⬜ |
| Collider | ⏳ | ⬜ | ⬜ |
| CharacterBody | ⏳ | ⬜ | ⬜ |
| Area | ⏳ | ⬜ | ⬜ |
| Joint | ⏳ | ⬜ | ⬜ |
