# 资产详设 · 动画剪辑（AnimationClip）

> 上游：`08`（资源/序列化）、`06`（反射 = 字段路径真相）、`15 §2`（动画两类轨道）、`10`（确定性）、`components/06-animation-particle.md`、`components/09-skeletal-2d.md`。
> 说明：动画剪辑是**资产（Asset）**，不是组件。`AnimationPlayer.clip` 通过 `asset_ref<AnimationClip>` 引用它
> （`components/06-animation-particle.md §1`）；`AnimatorController` 在其状态/混合树里也引用剪辑（§2）。
> 本节补齐"动画组件已引用、但从未详设"的缺口。

## 1. 定位与两类轨道

`AnimationClip` 是**一段按时间轴组织的关键帧数据**，分两类轨道（`15 §2`）：

| 轨道 | 目标 | 写入对象 | 示例 |
|------|------|----------|------|
| **属性轨道（PropertyTrack）** | 任意**反射字段**的字段路径 | `AnimationPlayer` 写回同级组件字段 | 位置、颜色、`CustomField` |
| **骨骼轨道（BoneTrack）** | 骨架资产内的**骨骼**（按稳定索引/名） | `Skeleton`/`Animator` 驱动骨骼 `Transform3D` | 关节旋转、位移 |

- 属性轨道对**反射字段路径**做关键帧（`06 §5`），目标由剪辑内的 `field_path` 描述，与 `AnimationPlayer`
  的写回契约一致（`components/06-animation-particle.md §1`）。
- 骨骼轨道对**骨骼**做关键帧；骨骼按骨架资产的**稳定顺序**索引（`10 D4`，见 `assets/06-skeleton-spine.md`）。
- 两类轨道共存于同一剪辑，由 `AnimationPlayer`（或 `Animator`）统一按时间采样。

## 2. 源文本 vs 烘焙二进制

- **源文本（dev source）**：剪辑描述符 + 关键帧。文本入库，**可 diff/AI 读**（`08 §3/§4`）。
- **烘焙二进制（cooked）**：压缩/对齐后的关键帧缓冲 + 采样加速索引（时间前缀表），运行时加载用。
- 硬约束：**文本是开发期唯一真相**；二进制是可再生产物（内容哈希判定失效，`08 §6`）。

## 3. 结构 / 字段

### 3.1 剪辑头

```yaml
animation_clip:
  name: player_run
  length: 1.25              # 总时长（秒），省略则由关键帧末时间推导并在烘焙期固化
  loop: Loop                # LoopMode：Once | Loop | PingPong | ClampForever
  interpolation: Linear     # 轨道默认插值（可被单关键帧覆盖）
  fps: 30                   # 源关键帧的采样基准（仅编辑提示，采样按时间而非帧）
  tracks:
    - property: { field_path: "Transform2D.position" }
      keys: [ ... ]
    - bone: { index: 3 }
      keys: [ ... ]
```

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `name` | `string` | `""` | | 剪辑名（稳定标识，供状态机按名引用） |
| `length` | `float` | `0` | `unit:s, readonly(cooked)` | 总时长；源可省，烘焙期由末关键帧固化 |
| `loop` | `LoopMode` | `Loop` | | 端点行为（对齐 `components/06 §1 LoopMode`） |
| `interpolation` | `InterpMode` | `Linear` | | 轨道默认插值 |
| `tracks` | `Track[]` | `[]` | | 轨道列表（属性/骨骼混合） |

### 3.2 轨道与关键帧

```yaml
- struct: Track
  fields:
    - { name: kind,        type: "TrackKind" }        # Property | Bone
    - { name: field_path,  type: "uint32_t" }         # Property：interned 字段路径哈希（06 §5）
    - { name: bone_index,  type: "uint16_t" }         # Bone：骨架资产内稳定索引
    - { name: value_type,  type: "ValueType" }        # Float | Vec2 | Vec3 | Quat | Color | Bool
    - { name: interpolation, type: "InterpMode", default: "InterpMode::Inherit" }
    - { name: key_count,   type: "uint16_t" }
    - { name: keys,        type: "Key[]" }            # 按 time 升序（稳定序，10 D4）

- struct: Key                                     # 单关键帧（变体值定长存储）
  fields:
    - { name: time,   type: "float", unit: "s" }
    - { name: value,  type: "Value" }             # 依 value_type 解释的定长联合
    - { name: in_tangent,  type: "float" }        # Cubic 用（对齐 components/06 §0.1 CurveKey）
    - { name: out_tangent, type: "float" }
```

| 类型 | 取值 | 说明 |
|------|------|------|
| `TrackKind` | `Property=0` `Bone=1` | 轨道类型 |
| `InterpMode` | `Inherit=-1` `Step=0` `Linear=1` `Cubic=2` | `Inherit` = 用剪辑默认；`Step` 无插值（离散/布尔） |
| `ValueType` | `Float` `Vec2` `Vec3` `Quat` `Color` `Bool` | 值种类；决定 `Key.value` 布局 |

- **插值语义**：`Step`（前值保持）→ `Linear`（线性）→ `Cubic`（Hermite，用 `in/out_tangent`）。
  四元数轨道用 **slerp**（短弧、符号归一），避免翻转抖动。
- **稳定序**：`keys` 按 `time` 升序、`tracks` 按 `(kind, field_path/bone_index)` 排序，
  禁哈希序（`08 §4`、`10 D4`）。
- **值联合**：`Value` 为定长 POD 联合（最大成员决定 `sizeof`）；`value_type` 决定解释 → 便于 codegen 与文本化。

## 4. 引用与运行时句柄

| 引用方（组件/资产） | 字段 | 说明 |
|---------------------|------|------|
| `AnimationPlayer` | `clip : asset_ref<AnimationClip>` | 直接播放的剪辑（`components/06 §1`） |
| `AnimatorController` | 状态/混合树内的剪辑引用 | 状态机按名/路径选剪辑（`components/06 §2`） |
| `Skeleton` / `Animator` | 骨骼轨道驱动骨骼 | 通过 `Skeleton.bone_root` 的 `Transform3D`（`components/06 §3`） |

- `asset_ref<AnimationClip>` 序列化为**稳定路径**；强引用计数保持剪辑存活（`07 §6`）。
- **运行时句柄**：加载后产出轻量只读**剪辑实例**（关键帧缓冲指针 + 采样游标上下文），标 `transient`、
  不序列化；`AnimationPlayer.time` / `playing` 才是**模拟态**（纳入快照，`components/06 §1`、`10 §4`）。
- `AnimationPlayer.track_mask` 按轨道**索引位**启用/禁用轨道；索引基于剪辑内稳定序（`components/06 §1`）。

## 5. 生命周期 / 确定性

- **采样在模拟步内**按固定 `dt` 完成：`(time, clip) → 字段值`，**纯函数、确定**（`15 §2`）。
- 总时长来自 `clip.length`（烘焙固化），**不读墙钟、不用运行时熵**。
- **端点行为**由 `loop` 决定：`Loop` 用整数模回绕，`PingPong` 反射，`ClampForever` 钳制到端点。
  回绕/反射用整数与钳制，避免浮点累计漂移（对齐 `AnimatedSprite` 的处理，`components/09 §1`）。
- 骨骼轨道采样顺序 = **骨架资产层级序**（父先于子），回放逐帧一致（`10 D4`）。
- 渲染帧率 ≠ 模拟帧率时，对**只读快照**插值显示，**不回写**（`13 §8`）。

## 6. 序列化 / diff

- **剪辑描述符 + 关键帧文本入库**：字段按 schema 声明序、省略默认、`keys` 一行一元素（`08 §4`）。
- `length` 在源可省略，烘焙期写入 cooked 头（`readonly`），避免手工维护不一致。
- 浮点值统一最短往返表示，避免无意义 diff（`08 §4`）。
- 剪辑为**纯数据**，不影响模拟校验和本身；参与校验的是组件侧的 `time`/字段写回结果（`10 §5`）。

## 7. 与其它系统

| 系统 | 关系 |
|------|------|
| 反射（`06`） | `field_path` 指向反射字段；值类型由 codegen 校验 |
| 场景（`12`） | `AnimationPlayer`/`Animator` 引用剪辑；剪辑不嵌入场景 |
| 持久化（`08`） | 文本入库、稳定路径、烘焙去默认 |
| 渲染（`13`） | 采样结果写字段，渲染只读消费；插值不回写 |
| 确定性（`10`） | 固定步采样、稳定序、整数回绕，回放逐帧一致 |

## 8. 风险

| 风险 | 处理 |
|------|------|
| 关键帧数与图集/内存 | 烘焙压缩 + 时间前缀索引；稀疏关键帧 |
| 四元数翻转抖动 | slerp 短弧 + 符号归一；导入期检查连续性 |
| `field_path` 失效（字段重命名） | 稳定哈希 + schema `deprecated` 迁移（`06 §7`）；导入期校验报错 |
| 骨骼轨道索引漂移 | 骨骼按骨架资产稳定序；重导骨骼时重映射并校验 |
| 双写同一字段（Player + Animator） | 校验保证互斥；`track_mask` 明确归属（`components/06 §1`） |
| 浮点漂移 | 固定步 + 最短往返表示；回绕用整数 |
| 文本/二进制不一致 | 单一模型 + 往返一致性测试（`08 §9`） |
