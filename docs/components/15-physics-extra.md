# 15 · 物理扩展类内置组件

> 归属模块：**physics**（Rapier via C ABI FFI，ADR-0028）。
> 上游：`17`（组件约定 §2/§3）、`05-physics.md`（本类共享约定 §0）、`15 §3`（物理集成）、`10`（确定性/快照/回放）、`06`（反射 = 组件形状唯一真相）、`12`（双视图 / bake）、`07`（句柄 / 内存）。
> 组件清单：**ConstantForce · LinearVelocityLimit · DampingOverride**（后两者为可选补充）。
> 本文件**完全复用** `05-physics.md` §0 的通用约定（模块归属、2D/3D 表达、权威状态与句柄、生命周期与确定性、序列化总则、共享 enum），下文不再重复展开。

---

## 0. 本类定位

- 这三个组件是 `RigidBody` 的**行为修饰器**：不新增形状、不新增体，只改变 **Dynamic 刚体每步所受的力 / 速度上限 / 阻尼**。
- 它们**必须与同实体 `RigidBody`（`body_type == Dynamic`）配对**；挂在 `Static`/`Kinematic` 上为无效配置（bake 期告警，运行期忽略）。
- 同实体维度仍由 `Transform2D/3D` 决定；字段统一 `Vec3`，2D 仅用 `x/y`、`z` 恒 0（`05 §0.2`）。
- 每步施加顺序固定：**先 ConstantForce → 再 DampingOverride 覆写阻尼 → 最后 LinearVelocityLimit 夹紧**（`05 §0.4` 的 id 稳定排序之内），保证同场景重放一致。

### 共享 enum（本类新增）

```yaml
- enum: ForceApplyMode      # 力的施加方式
    values: [ {name: Continuous, value: 0},   # 每物理步持续施加
              {name: Impulse,    value: 1} ]   # 单步冲量，施加一次后置零

- enum: ForceSpace           # 力/扭矩的参考系
    values: [ {name: World, value: 0},        # 世界坐标（默认）
              {name: Local, value: 1} ]        # 刚体局部坐标，随体旋转
```

> `ForceSpace::Local` 用于"沿箭身向前推""绕固有轴自旋"等随体朝向变化的场景；bake 时按刚体当前旋转换算为世界系后再提交 Rapier。

---

## 1. ConstantForce

- **归属模块 / 上游**：physics / `15 §3`、`05 §0`。
- **说明**：给 Dynamic 刚体施加**恒定的外力与扭矩**（推进器、持续重力偏移、风力场、旋转马达）。与 `RigidBody::gravity_scale` 互补：后者只缩放重力，本组件提供**任意方向、任意时机的持续外力**。需要 RigidBody（必需）。
- **依赖类型**：`ForceApplyMode`、`ForceSpace`；`RigidBody`（必需，且 `Dynamic`）。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `force` | `Vec3` | `0` | `unit:N` | 施加在质心的力；按 `space` 解释 |
| `torque` | `Vec3` | `0` | `unit:N·m` | 施加的扭矩；按 `space` 解释 |
| `space` | `ForceSpace` | `World` | | 力/扭矩的参考系 |
| `apply_mode` | `ForceApplyMode` | `Continuous` | | `Continuous` 每步施加；`Impulse` 施加一次后自清 |
| `enabled` | `bool` | `true` | | 总开关；`false` 时不施加（可运行时切换） |
| `only_when_awake` | `bool` | `true` | | 体休眠时不施加，避免唤醒抖动 |
| `impulse_consumed` | `bool` | `false` | `transient, readonly` | `Impulse` 模式下本次冲量是否已提交 |

- **句柄/引用**：不持有 Rapier 句柄——力在步内由 physics 系统经 `RigidBody::body_handle`（transient）提交给 Rapier，本组件本身无句柄。无 `asset_ref`。
- **生命周期/确定性**：
  - 仅在 `physics_process(dt)` 固定步内施加（`05 §0.4`），**绝不**在 `process(dt)` 或帧中提交——保证与模拟步一一对应。
  - `Continuous`：每步重新施加 `force`/`torque`，与步长无关地为恒力语义；`Impulse`：在本步施加后把本组件的 `impulse_consumed` 置真并在步界清零 `apply_mode` 效果，**不依赖墙钟计时**。
  - 力的换算只用 sim time 与刚体确定性旋转，**不引入系统熵 / RNG**（`10 D2/D3`）；顺序按 `entity id` 稳定（`10 D4`）。
- **序列化**：`impulse_consumed` 标 `transient`（每步重算）；`force`/`torque`/`space`/`apply_mode`/`enabled`/`only_when_awake` 进场景资产（作者意图）。快照/回滚时权威态仍在 physics world blob（`05 §0.3`），本组件只存意图与只读回读量。

---

## 2. LinearVelocityLimit（可选补充）

- **归属模块 / 上游**：physics / `15 §3`、`05 §0`。
- **说明**：给 Dynamic 刚体的**线性 / 角速度设上限**（防高速穿透、限制载具极速、稳定堆叠）。每步积分后**夹紧**，不改施加的力。需要 RigidBody（必需，且 `Dynamic`）。
- **依赖类型**：`RigidBody`（必需，且 `Dynamic`）。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `max_linear_speed` | `f32` | `0` | `unit:m/s, range:[0,]` | 线性速度模长上限；`0` = 不限制 |
| `max_angular_speed` | `f32` | `0` | `unit:rad/s, range:[0,]` | 角速度模长上限；`0` = 不限制 |
| `enabled` | `bool` | `true` | | 总开关 |
| `linear_speed` | `f32` | `0` | `transient, readonly` | 上一步夹紧后的实际线速度模长 |
| `angular_speed` | `f32` | `0` | `transient, readonly` | 上一步夹紧后的实际角速度模长 |

- **句柄/引用**：无（作用于 `RigidBody::body_handle`，见 §1）。
- **生命周期/确定性**：在 `physics_process` 步末、变换回写**之前**夹紧；夹紧为纯确定性运算（模长比较 + 缩放），无 RNG。结果类字段从 world blob 只读回读（`05 §0.3`）。
- **序列化**：`linear_speed`/`angular_speed` transient；`max_*`/`enabled` 进资产。

---

## 3. DampingOverride（可选补充）

- **归属模块 / 上游**：physics / `15 §3`、`05 §0`。
- **说明**：**按实体覆写** `RigidBody` 的线性 / 角阻尼（`RigidBody` 字段仍是缺省基线）。用于"落地急停""水中减速"等不想改全局层级的场景；效果类似 `Area::*_damping_override`，但作用于**单个体且无需区域重叠查询**。需要 RigidBody（必需，且 `Dynamic`）。
- **依赖类型**：`RigidBody`（必需，且 `Dynamic`）。

### 字段表

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `linear_damping` | `f32` | `0.0` | `unit:1/s, range:[0,]` | 覆写线性阻尼（覆盖 `RigidBody` 同名缺省） |
| `angular_damping` | `f32` | `0.0` | `unit:1/s, range:[0,]` | 覆写角阻尼 |
| `enabled` | `bool` | `true` | | `false` 时回落到 `RigidBody` 缺省值 |
| `priority` | `i32` | `0` | | 与 `Area` 阻尼覆写并存时的仲裁优先级 |

- **句柄/引用**：无（作用于 `RigidBody::body_handle`，见 §1）。
- **生命周期/确定性**：在 `physics_process` 内于 ConstantForce 之后、速度夹紧之前写入 Rapier 体阻尼；生效值确定、无 RNG。与 `Area` 覆写冲突时按 `priority` 稳定裁决（同优先级按 `entity id`）。
- **序列化**：`enabled`/`priority`/阻尼值进资产；无 transient 结果字段。

---

## 4. 状态

| 组件 | 设计 | 评审 | 落地 |
|------|------|------|------|
| ConstantForce | ✅ | ⬜ | ⬜ |
| LinearVelocityLimit | ✅ | ⬜ | ⬜ |
| DampingOverride | ✅ | ⬜ | ⬜ |
