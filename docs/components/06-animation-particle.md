# 06 · 动画与粒子组件

> 上游：`17`（约定 §2/§3）、`15`（动画/物理/粒子）、`10`（确定性快照）、`06`（反射）。
> 本文件涵盖 4 个组件：**AnimationPlayer · Animator · Skeleton · ParticleEmitter**。
> 全部为**世界层模块**的反射组件：跑在**模拟线程、固定 dt** 下，权威数据在 **ECS（bake 后实体）**，
> 节点树仅用于**编辑/编排**（`12 §4`）。所有随机**只走引擎 RNG**，禁系统熵（`10 D2`）。

## 0. 速览

| 组件 | 归属模块 | 语义 | 关键配对 | 确定性 |
|------|----------|------|----------|--------|
| `AnimationPlayer` | animation | 播放**单个剪辑**，写属性轨道 | `Transform2D/3D` 或任意反射字段 | ✅ 参与回放 |
| `Animator` | animation | **状态机 + 参数**，选剪辑并混合 | `Skeleton` / `AnimationPlayer` | ✅ 求值确定 |
| `Skeleton` | animation | 骨骼层级 + 蒙皮，输出姿态 | `MeshRenderer` / `Animator` | ✅ 姿态确定 |
| `ParticleEmitter` | particle | 发射粒子（**CPU 确定 / GPU 仅视觉**） | `Transform3D` / `Material` | ✅CPU / ❌GPU |

**通用约定**：动画剪辑/骨架/贴图/材质一律用 `asset_ref<T>`；渐变与曲线用**内联定长 POD 小结构**（§0.1），
以避免额外资产引用、保持确定性、便于文本 diff（`08 §4`）。引用类型序列化为稳定 ID/路径（`06 §3`）。

### 0.1 内联小结构（供多个组件共享）

```yaml
- struct: GradientStop   # 颜色渐变的单个色标
  size: 20
  fields:
    - { name: t,     type: "float" }        # [0,1]
    - { name: color, type: "Rgba" }         # {f32 r,g,b,a}

- struct: Gradient       # 定长 8 色标颜色渐变
  size: 164
  fields:
    - { name: count, type: "uint8_t", attrs: { range: [0, 8] } }
    - { name: stops, type: "GradientStop[8]" }

- struct: CurveKey       # 曲线关键帧（含切线，Cubic 用）
  size: 16
  fields:
    - { name: t,           type: "float" }
    - { name: value,       type: "float" }
    - { name: in_tangent,  type: "float" }
    - { name: out_tangent, type: "float" }

- struct: Curve          # 定长 8 关键帧数值曲线
  size: 132
  fields:
    - { name: interp, type: "InterpMode", default: "InterpMode::Linear" }
    - { name: count,  type: "uint8_t", attrs: { range: [0, 8] } }
    - { name: keys,   type: "CurveKey[8]" }
```

- `Rgba = {f32 r,g,b,a}`（线性空间，`13`）。
- **求值**：采样由 animation/particle 系统在**模拟步内按 dt** 完成，纯函数、确定（`15 §2`）。
- 定长上限（8）在 schema 里硬约束；超出由**导入期烘焙裁剪**（`08 §6`），不引入动态容器。

---

## 1. AnimationPlayer

- **归属模块 / 上游文档**：`animation` 模块；`15 §2`、`17 §3`。
- **说明**：在实体上播放**单个动画剪辑（AnimationClip）**并按时间推进，把关键帧**写回**目标反射字段
  （位置/旋转/颜色/自定义字段，`15 §2` *属性轨道*）。相对 `Animator`，它是**无状态机的直接播放**
  ——挂最少的组件、最直观，适合门/风车/一次性演出。与 `Transform2D/3D` 或其他字段持有者配对；
  与 `Animator` **互斥使用**于同一目标字段（避免双写，由校验保证）。
- **依赖类型 / enum**：

  | 类型 | 取值 | 说明 |
  |------|------|------|
  | `LoopMode` | `Once=0` `Loop=1` `PingPong=2` `ClampForever=3` | 到端点行为 |
  | `UpdateMode` | `Process=0` `Physics=1` | 用 `process(dt)` 还是 `physics_process(dt)` 驱动（`12 §6`） |

  资产：`asset_ref<AnimationClip>`（属性轨道关键帧，文本入库可 diff，`08 §4`）。

- **字段表**：

  | 字段 | 类型 | 默认 | attrs | 说明 |
  |------|------|------|-------|------|
  | `clip` | `asset_ref<AnimationClip>` | `null` | `ref: AnimationClip` | 播放的剪辑（稳定路径） |
  | `autoplay` | `bool` | `true` | | 进入世界即自动播放 |
  | `loop` | `LoopMode` | `Loop` | | 循环模式 |
  | `update_mode` | `UpdateMode` | `Process` | | 驱动相位 |
  | `speed` | `float` | `1.0` | `range:[0,100]` | 播放速率倍率 |
  | `playing` | `bool` | `false` | | 运行态（模拟态，见 §1.4） |
  | `time` | `float` | `0.0` | `readonly, unit:s` | 当前播放时间（模拟态） |
  | `track_mask` | `uint64_t` | `~0` | | 轨道启用位掩码（≤64 轨） |
  | `event_sink` | `entity_ref` | `null` | `ref: Script` | 接收动画事件回调的脚本实体 |

- **句柄 / 引用**：`clip`（`asset_ref`，强引用保持剪辑存活，`07 §6`）；`event_sink`（弱 `entity_ref`）。
  写回目标为**同级组件的反射字段路径**（由剪辑内的 `field_path` 描述，`06 §5`）。
- **生命周期 / 确定性**：
  - 求值在**模拟步内**按固定 `dt` 采样，**纯函数**：`(time, clip) → 字段值`，结果确定（`15 §2`）。
  - `autoplay` 在 `ready` 之后启动；`playing`/`time` 属**模拟态**，纳入快照，回放逐帧一致（`10 D1/D3`）。
  - 不读墙钟；**总时长**来自剪辑资产，非运行时熵。
  - 渲染帧率 ≠ 模拟帧率时，对**只读快照**插值显示，**不回写**（`13 §8`）。
- **序列化**：
  - `clip` → 稳定路径（`08 §5`）。
  - 场景资产中 `playing`/`time` 标 `transient`（编辑期重置为默认）；**快照**（`10 §4`）按字段包含它们。
  - `update_mode`/`speed`/`loop` 为设计参数，正常入库。

---

## 2. Animator

- **归属模块 / 上游文档**：`animation` 模块；`15 §2`（状态机 / blend tree）、`17 §3`。
- **说明**：**动画状态机驱动**。持有控制器资产（状态/转移/混合树）与一组**参数**；每模拟步求值
  状态与转移，选出剪辑与权重，驱动 `AnimationPlayer` 或 `Skeleton`。参数由脚本经反射写入
  （`SetParameter`，`06 §6` / `14`）。与 `AnimationPlayer`/`Skeleton` 配对；**求值确定**
  （禁时间相关的随机混合，`15 §2`）。
- **依赖类型 / enum**：

  | 类型 | 取值 | 说明 |
  |------|------|------|
  | `AnimParamType` | `Float=0` `Int=1` `Bool=2` `Trigger=3` | 参数种类 |
  | `TransitionMode` | `Interrupt=0` `WhenDone=1` | 转移触发（在控制器资产内） |

  ```yaml
  - struct: AnimParam       # 一个参数槽（定长，随组件内联）
    size: 16
    fields:
      - { name: name_hash, type: "uint32_t" }   # interned 名稳定哈希（06 §5）
      - { name: type,      type: "AnimParamType" }
      - { name: value,     type: "float" }      # Float；Bool/Trigger 用 0/1
      - { name: ivalue,    type: "int32_t" }    # Int
  ```

  资产：`asset_ref<AnimatorController>`（状态、转移、混合树，文本入库，`08 §4`）。

- **字段表**：

  | 字段 | 类型 | 默认 | attrs | 说明 |
  |------|------|------|-------|------|
  | `controller` | `asset_ref<AnimatorController>` | `null` | `ref: AnimatorController` | 状态机资产（稳定路径） |
  | `parameters` | `AnimParam[16]` | `{}` | `range:[0,16]` | 参数槽（定长上限，超出导入期裁剪） |
  | `param_count` | `uint8_t` | `0` | `readonly` | 有效参数数 |
  | `current_state` | `uint32_t` | `0` | `transient, readonly` | 当前状态哈希（派生，不入场景） |
  | `normalized_time` | `float` | `0.0` | `transient, readonly, range:[0,1]` | 当前状态归一化进度 |
  | `speed` | `float` | `1.0` | `range:[0,100]` | 状态机时钟倍率 |
  | `apply_physics` | `bool` | `false` | | 用 `physics_process(dt)` 驱动 |
  | `target` | `entity_ref` | `null` | `ref: AnimationPlayer` | 输出驱动的播放器（可空，直接用混合结果） |

- **句柄 / 引用**：`controller`（`asset_ref` 强引用）；`target`（弱 `entity_ref` 指向同实体的
  `AnimationPlayer`）。参数名用**interned 稳定哈希**（禁字符串比较进热路径，`07 §7`）。
- **生命周期 / 确定性**：
  - 每模拟步：更新参数时钟 → 求值转移（**按声明序+id 稳定排序**，`10 D4`）→ 求值混合树权重 → 输出。
  - **无时间相关随机混合**：clamp/normalize 后权重确定（`15 §2`）；blend tree 的同步用规范化时间，非墙钟。
  - `Trigger` 参数**消费后自动清除**，清除发生在**固定步边界**，保证回放一致。
  - `current_state`/`normalized_time` 为**派生**，由快照重算/校验（`10 §5`）。
- **序列化**：
  - `controller` → 稳定路径；`parameters` 值入库（脚本/AI 可直接读写，`06 §6`）。
  - `current_state`/`normalized_time` 标 `transient`（场景资产不写）；**快照**包含状态以供回滚。
  - 参数槽 `range` 上限固定；增删参数由控制器资产定义、导入期映射。

---

## 3. Skeleton

- **归属模块 / 上游文档**：`animation` 模块；`15 §2`（*骨骼动画*：层级变换 + 蒙皮）、`17 §3`。
- **说明**：**蒙皮骨架**。持有骨架绑定资产（骨骼层级 + 逆绑定矩阵），骨骼为**带 `Transform3D` 的
  子实体/节点**（由 `Animator`/`AnimationPlayer` 驱动其变换）；本组件在每个模拟步把骨骼局部变换
  合成世界变换并产出**蒙皮矩阵**（`skinning_matrices`），供 `MeshRenderer` 提取到 GPU 蒙皮
  （`13 §5`）。与 `MeshRenderer`（蒙皮网格）、`Animator` 配对。
- **依赖类型 / enum**：

  | 类型 | 取值 | 说明 |
  |------|------|------|
  | `SkinningMode` | `Linear=0` `DualQuat=1` | 蒙皮算法（按平台评估，`15 §7`） |

  ```yaml
  - struct: BonePose        # 单骨骼局部 TRS（姿态缓冲元素）
    size: 40
    fields:
      - { name: position, type: "Vec3" }
      - { name: rotation, type: "Quat" }
      - { name: scale,    type: "Vec3" }
  ```

  资产：`asset_ref<SkeletonAsset>`（骨骼名、父索引、绑定/逆绑定矩阵，文本入库，`08 §4`）。

- **字段表**：

  | 字段 | 类型 | 默认 | attrs | 说明 |
  |------|------|------|-------|------|
  | `skeleton` | `asset_ref<SkeletonAsset>` | `null` | `ref: SkeletonAsset` | 骨架绑定资产（稳定路径） |
  | `bone_root` | `entity_ref` | `null` | `ref: Node` | 骨骼层级根（子实体带 `Transform3D`） |
  | `skinning_mode` | `SkinningMode` | `Linear` | | 蒙皮算法 |
  | `poses` | `BonePose[]` | `[]` | `transient` | 局部姿态缓冲（每步派生） |
  | `skinning_matrices` | `Mat4[]` | `[]` | `transient` | 输出蒙皮矩阵（上传 GPU，`13`） |
  | `bounds` | `Aabb` | `{}` | `transient, readonly` | 蒙皮后包围盒（剔除用，`13 §5`） |

- **句柄 / 引用**：`skeleton`（`asset_ref` 强引用）；`bone_root`（弱 `entity_ref`，指向骨骼子树根）。
  骨骼**按骨架资产内的稳定顺序**索引，绝不用指针地址排序（`10 D4` / `07 §9`）。
- **生命周期 / 确定性**：
  - 每模拟步：读各骨骼 `Transform3D` → 层序合成世界矩阵 → `world · inverse_bind` → 蒙皮矩阵。**纯确定**。
  - 姿态求值顺序 = 骨架资产的**层级序**（父先于子），与声明顺序一致，回放逐帧一致（`15 §2`）。
  - `poses`/`skinning_matrices`/`bounds` 是**派生缓冲**，每步重算，不参与逻辑决策。
  - 高基数骨骼实例走 ECS + 实例化，避占节点树（`12 §4` 硬边界、`15 §7`）。
- **序列化**：
  - `skeleton` → 稳定路径；`bone_root` → 实体稳定 ID（实例化时统一重绑，`12 §7`）。
  - `poses`/`skinning_matrices`/`bounds` 标 `transient`（场景与**快照均不序列化**，由骨骼变换重算）。
  - 骨骼自身变换由其 `Transform3D` 组件（或动画驱动）承载，各自序列化。

---

## 4. ParticleEmitter

- **归属模块 / 上游文档**：`particle` 模块；`15 §4`、`10`（GPU 粒子排除确定性）、`17 §3`。
- **说明**：**粒子发射器**。按发射率/形状生成粒子并模拟（生命周期、速度、重力、大小/颜色随生命变化）。
  支持 **CPU 模式（确定，参与回放）** 与 **GPU 模式（compute，高性能，仅视觉）**。可由节点树 **bake**
  为 ECS 实体；高基数粒子走 ECS 池化（`15 §5`、`07 §8`）。与 `Transform3D`、`Material` 配对。
- **依赖类型 / enum**：

  | 类型 | 取值 | 说明 |
  |------|------|------|
  | `ShapeType` | `Point=0` `Circle=1` `Box=2` `Sphere=3` `Hemisphere=4` `Cone=5` `Edge=6` | 发射形状（2D/3D 各取子集） |
  | `SimSpace` | `Local=0` `World=1` | 粒子模拟空间：随发射器移动 / 世界固定 |
  | `ParticleMode` | `CPU=0` `GPU=1` | 模拟模式（决定确定性） |
  | `BlendMode` | `Alpha=0` `Additive=1` `Multiply=2` | 渲染混合（透传给材质/管线，`13`） |
  | `SortMode` | `None=0` `ByDepth=1` `ByAge=2` | 绘制排序（GPU 模式可忽略） |

  资产：`asset_ref<Texture>`（粒子贴图，可用 spritesheet）、`asset_ref<Material>`（着色/混合）。
  渐变/曲线用 §0.1 的内联 `Gradient` / `Curve`。

- **字段表**：

  | 字段 | 类型 | 默认 | attrs | 说明 |
  |------|------|------|-------|------|
  | `mode` | `ParticleMode` | `CPU` | | **CPU=确定；GPU=仅视觉**（见 §4.4） |
  | `sim_space` | `SimSpace` | `Local` | | 模拟空间 |
  | `texture` | `asset_ref<Texture>` | `null` | `ref: Texture` | 粒子贴图（稳定路径） |
  | `material` | `asset_ref<Material>` | `null` | `ref: Material` | 材质（含混合/着色） |
  | `blend` | `BlendMode` | `Additive` | | 混合模式 |
  | `shape` | `ShapeType` | `Point` | | 发射形状 |
  | `shape_size` | `Vec3` | `{0,0,0}` | `unit:m` | Box 半尺寸 / Sphere 半径在 x 分量 |
  | `shape_angle` | `float` | `0.0` | `unit:deg, range:[0,180]` | Cone 半角 / Edge 方向 |
  | `rate` | `float` | `10.0` | `range:[0,100000], unit:1/s` | 持续发射率（每秒） |
  | `burst_count` | `uint16_t` | `0` | | 单次爆发数量 |
  | `lifetime` | `Curve` | `{1.0}` | `unit:s` | 粒子寿命（可随机区间，用 curve t 采样） |
  | `start_speed` | `Curve` | `{1.0}` | `unit:m/s` | 初速度分布 |
  | `gravity` | `Vec3` | `{0,-9.8,0}` | `unit:m/s²` | 重力（World 空间） |
  | `start_size` | `Curve` | `{1.0}` | | 初始大小 |
  | `size_over_life` | `Curve` | `{1.0}` | | 大小随生命（乘子） |
  | `color_over_life` | `Gradient` | 白→透明白 | | 颜色/透明度随生命 |
  | `max_particles` | `uint32_t` | `1000` | `range:[0,1000000]` | 粒子预算上限（池化，`07 §8`） |
  | `seed` | `uint32_t` | `0` | | **引擎 RNG 种子（仅 CPU）** |
  | `prewarm` | `float` | `0.0` | `unit:s` | 预热时长（固定步离线推进） |
  | `emit` | `bool` | `true` | | 运行态发射开关 |
  | `rng_state` | `uint64_t` | `0` | `transient` | CPU 粒子 RNG 状态（模拟态，入快照） |

- **句柄 / 引用**：`texture`/`material`（`asset_ref` 强引用，控制资源存活，`07 §6`）；
  粒子实例池由 `particle` 系统管理，经 `max_particles` 上限 + 池化避免运行期分配（`07 §8`）。
- **生命周期 / 确定性**：
  - **固定步长**：发射与模拟均按模拟 `dt` 推进（`10 D1`）；时间累加器吸收渲染帧波动。
  - **CPU 模式 = 确定**：随机只走**引擎 RNG**（由 `seed` 初始化、状态存 `rng_state`），**禁系统熵**
    （`10 D2`）；粒子迭代按**稳定 id 序**（`10 D4`）；参与快照/回放与 checksum（`10 §5`）。
  - **GPU 模式 = 仅视觉**：compute 粒子**可能不确定**（`15 §4`）→ **排除出模拟确定性**：不写任何模拟字段、
    不进 checksum、不做逻辑输入；`rng_state` 不参与；相关字段标 **`transient`**，快照与回放**跳过**
    （GPU 代价：回放时该发射器外观不保证逐帧一致，但**不影响世界状态**）。
  - **`prewarm`**：用固定 `dt` 离线推进整数步，确保回放一致（不读墙钟）。
  - 渲染只读消费粒子缓冲；GPU 缓冲**不回写**模拟（`13 §8`）。
- **序列化**：
  - 设计参数（`mode`/`shape`/`rate`/`lifetime`/`gravity`/`color_over_life`…）正常入库 → 稳定路径/内联值。
  - `emit` 入库（可被 AI/脚本改，`06 §6`）。
  - **CPU**：`seed` 入库；`rng_state` + 粒子池状态标**模拟态**，场景资产 `transient`、**快照包含**。
  - **GPU**：`rng_state` 及任何粒子缓冲**永久 `transient`**，场景与快照均不写（不可回放，符合 `15 §4`）。
