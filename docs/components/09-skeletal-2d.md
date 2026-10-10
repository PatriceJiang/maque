# 09 · 2D 骨骼与帧动画组件

> 上游：`17`（约定 §2/§3）、`15`（动画/物理/粒子）、`10`（确定性快照）、`06`（反射）、
> `03`（渲染 2D/3D）、`13`（渲染管线）。
> 本文件涵盖 2 个组件：**AnimatedSprite · SpineSkeleton**。
> 二者均归 **2D 图层**（`13 §4`），需 `Transform2D` 配对，**跑在模拟线程、固定 dt** 下（`10 D1`）；
> 权威数据在 **ECS（bake 后实体）**，节点树仅用于**编辑/编排**（`12 §4`）。
> 渲染为**只读消费**，绝不回写模拟字段（`03 §0`、`13 §8`）。

## 0. 速览

| 组件 | 归属模块 | 语义 | 关键配对 | 确定性 |
|------|----------|------|----------|--------|
| `AnimatedSprite` | renderer | **自足帧动画**：精灵表按帧序播放 | `Transform2D` + `asset_ref<SpriteFrames>` | ✅ 时间线确定 |
| `SpineSkeleton` | spine2d | **外部骨骼动画**：驱动 Spine 2D 运行时 | `Transform2D` + `asset_ref<SpineAsset>` | ✅ 播放确定 / 运行时不回写 |

**与相邻组件的区别**：

- 与 `Sprite`（`03 §Sprite`）：`Sprite` 是**单帧静态** 2D 贴图基元，`frame` 由外部（脚本/动画轨道）驱动；
  `AnimatedSprite` 内建**帧时间线**，按 `fps` 自动推进，无需旁路组件。
- 与 `Skeleton`（`06 §3`，**3D**）：`Skeleton` 是 **3D 蒙皮骨架**（`Transform3D` 层级 + 逆绑定矩阵 +
  GPU 蒙皮，输出 `skinning_matrices`）；`SpineSkeleton` 是 **2D 分层变形**（网格顶点形变，无 3D 蒙皮），
  力学与相位完全不同，故单列。
- 与 `AnimationPlayer/Animator`（`06 §1/§2`）：那两者驱动**反射字段轨道/状态机**；本类**自带播放时钟**，
  不与属性轨道双写同一字段（校验保证，`06 §1`）。

**通用约定**：贴图/精灵表/骨架资产一律 `asset_ref<T>`，序列化为稳定 ID/路径（`06 §3`、`08 §5`）；
运行时实例句柄标 `transient`，反序列化后重建时**重绑**（§2.4）。

---

## 1. AnimatedSprite

- **归属模块 / 上游文档**：`renderer`；`03 §Sprite`、`13 §4/§5`、`15 §2`、`17 §3`。
- **说明**：**自足帧动画播放器**。引用一份 `SpriteFrames` 帧序资产（帧 → 图集区域 + 时长），
  在模拟步内按 `fps × speed` 推进时钟，选出 `frame`，交由 2D 精灵批处理绘制（`13 §5`）。
  适合角色行走/爆炸序列/UI 逐帧等**无骨骼**场景。挂在带 `Transform2D` 的节点上；与 `Sprite`
  **互斥使用**于同一节点（避免重复绘制，校验保证）。
- **依赖类型 / enum**：

  | 类型 | 取值 | 说明 |
  |------|------|------|
  | `PlayMode` | `Once=0` `Loop=1` `PingPong=2` `ClampForever=3` | 到端点行为（对齐 `06 §1 LoopMode`） |
  | `FlipMode` | `None=0` `Horizontal=1` `Vertical=2` `Both=3` | 翻转（复用 `03 §Sprite`） |

  资产：`asset_ref<SpriteFrames>`（每帧 `region` + `duration`，可对应一张精灵表按 `hframes×vframes`
  切成多帧，文本入库可 diff，`08 §4`）。

  ```yaml
  - struct: SpriteFrame       # SpriteFrames 资产内的单帧（定长 POD）
    size: 24
    fields:
      - { name: region,   type: "Rect" }      # 归一化 UV 子矩形
      - { name: duration, type: "float", unit: "s", range: [0, 100] }
  ```

- **字段表**：

  | 字段 | 类型 | 默认 | attrs | 说明 |
  |------|------|------|-------|------|
  | `frames` | `asset_ref<SpriteFrames>` | `null` | `ref: SpriteFrames` | 帧序资产（稳定路径） |
  | `animation` | `uint32_t` | `0` | | 多动画时选定的动画索引/稳定哈希 |
  | `autoplay` | `bool` | `true` | | 进入世界即自动播放 |
  | `play_mode` | `PlayMode` | `Loop` | | 端点行为 |
  | `fps` | `float` | `10.0` | `range:(0,240]` | 基准帧率（每秒帧数） |
  | `speed` | `float` | `1.0` | `range:[0,100]` | 播放速率倍率（负值倒放） |
  | `frame` | `uint16_t` | `0` | `range:[0,65535]` | 当前帧索引（模拟态，见下） |
  | `time` | `float` | `0.0` | `readonly, unit:s` | 动画内当前时间（模拟态） |
  | `playing` | `bool` | `false` | | 运行态播放开关（模拟态） |
  | `flip` | `FlipMode` | `None` | | 水平/垂直翻转 |
  | `color` | `Color` | `{1,1,1,1}` | | 顶点色（tint × alpha） |
  | `pivot` | `Vec2` | `{0.5,0.5}` | `unit: normalized` | 锚点（对齐 `03 §Sprite`） |
  | `z_index` | `int32_t` | `0` | | 层内排序；同 `RenderLayer` 内越小越先画 |
  | `material` | `asset_ref<Material>` | `null` | `ref: Material` | 可选自定义 2D 材质；空用默认 |

- **句柄 / 引用**：`frames` / `material`（`asset_ref` 强引用，控制资源存活，`07 §6`）。
  帧序在该资产内**按稳定顺序**索引，绝不用指针地址排序（`10 D4`）。
- **生命周期 / 确定性**：
  - 时钟推进在**模拟步内**按固定 `dt` 完成：`time += dt·speed`，`帧序 = 前缀和(time·fps)`，**纯函数、确定**
    （`15 §2`）。`time`/`frame`/`playing` 属**模拟态**，纳入快照，回放逐帧一致（`10 D1/D3`）。
  - 不读墙钟；帧时长来自 `SpriteFrames` 资产，非运行时熵。**循环/回绕**用整数模与钳制，避免浮点累计漂移。
  - 渲染帧率 ≠ 模拟帧率时，对**只读快照**插值显示，**不回写**（`13 §8`）。
- **序列化**：
  - `frames` → 稳定路径；`animation`/`play_mode`/`fps`/`speed`/`flip`/`color`/`pivot`/`z_index`/`material`
    为设计参数，正常入库。
  - 场景资产中 `playing`/`time`/`frame` 标 `transient`（编辑期重置默认）；**快照**（`10 §4`）按字段包含它们。

---

## 2. SpineSkeleton

- **归属模块 / 上游文档**：`spine2d` 模块（第三方运行时封装）；`15 §2`、`10`、`03 §0`、`17 §3`。
- **说明**：**集成 Spine 2D 骨骼运行时**。引用一份导出后的 Spine 资产（骨架 + 皮肤 + 动画 + 图集），
  每模拟步推进运行时、求值当前姿势，再把结果作为**只读绘制输入**交给 2D 批处理。挂在带 `Transform2D`
  的节点上。运行时以**句柄**形式持有（§2.4），**不进反射布局、不参与序列化**。
  与 `Skeleton`（3D，`06 §3`）不同：此处不做 3D 蒙皮、不产出 `skinning_matrices`，仅输出 2D 网格/贴图区域。
- **依赖类型 / enum**：

  | 类型 | 取值 | 说明 |
  |------|------|------|
  | `SpineBlend` | `Setup=0` `First=1` `Replace=2` `Add=3` `Multiply=4` `Screen=5` | 动画混合模式（对齐 Spine `MixBlend`） |
  | `SpinePhysTimestep` | `Inherit=0` `Fixed=1` `NoPhysics=2` | 运行时内置物理/IK 时间步策略 |
  | `SpineUpdate` | `Process=0` `Physics=1` | 用 `process(dt)` 还是 `physics_process(dt)` 驱动 |

  资产：`asset_ref<SpineAsset>`（骨架 JSON/Binary + 图集 `SpineAtlas`；文本骨架可 diff，二进制按稳定路径
  引用，`08 §4`）。

  ```yaml
  - struct: SpineAnimState   # 运行时缓冲（transient，不入场景）
    size: 16
    fields:
      - { name: time,           type: "float", unit: "s" }   # 当前动画时间
      - { name: track,          type: "int32_t" }            # 主轨道号
      - { name: loop,           type: "bool" }
  ```

- **字段表**：

  | 字段 | 类型 | 默认 | attrs | 说明 |
  |------|------|------|-------|------|
  | `skeleton` | `asset_ref<SpineAsset>` | `null` | `ref: SpineAsset` | 骨架+图集资产（稳定路径） |
  | `skin` | `uint32_t` | `0` | | 皮肤稳定哈希/索引 |
  | `animation` | `uint32_t` | `0` | | 默认动画稳定哈希/索引 |
  | `autoplay` | `bool` | `true` | | 进入世界即播放 |
  | `play_mode` | `PlayMode` | `Loop` | `ref: PlayMode` | 端点行为（复用 §1） |
  | `speed` | `float` | `1.0` | `range:[0,100]` | 全局时间倍率（含负放） |
  | `blend` | `SpineBlend` | `Replace` | | 主轨道混合模式 |
  | `update_mode` | `SpineUpdate` | `Process` | | 驱动相位 |
  | `phys_timestep` | `SpinePhysTimestep` | `Inherit` | | 运行时内置物理步策略 |
  | `flip` | `FlipMode` | `None` | `ref: FlipMode` | 水平/垂直翻转（复用 §1） |
  | `color` | `Color` | `{1,1,1,1}` | | 顶点色（tint × alpha） |
  | `z_index` | `int32_t` | `0` | | 层内排序 |
  | `material` | `asset_ref<Material>` | `null` | `ref: Material` | 可选 2D 材质；空用默认 |
  | `state` | `SpineAnimState` | `{}` | `transient, readonly` | 运行时播放状态（派生） |
  | `instance` | `SpineInstanceHandle` | `0` | `transient, readonly` | **运行时实例句柄**（见 §2.4） |

- **句柄 / 引用**：
  - `skeleton` / `material`（`asset_ref` 强引用，`07 §6`）。
  - `instance` 为 **`SpineInstanceHandle`**（不透明 `uint64_t`），指向第三方运行时分配的对象，
    由 `spine2d` 模块经 C ABI 持有与释放（`02` FFI 规则：只暴露 C ABI + 句柄，分配者释放）。
    **标 `transient`、绝不序列化**；反序列化后由运行时**重建并重绑**（§2.4）。
- **生命周期 / 确定性**：
  - 每模拟步：以固定 `dt·speed` 推进运行时 → 求值姿势 → 产出**只读绘制输入**（网格顶点/贴图区域/混合）。
    **播放求值确定**（`15 §2`）；避免运行时内置依赖系统熵的行为（如随机换肤），随机效果统一走引擎 RNG。
  - `phys_timestep = Inherit` 时，运行时物理/IK 用与主循环一致的固定步（`10 D1`）；**禁**运行时建内部非固定
    线程/计时器（`10 D6/D7`）。
  - **渲染只读**：姿势/绘制结果不回写任何模拟字段；渲染帧率 ≠ 模拟帧率时对只读快照插值显示（`13 §8`）。
  - 求值顺序 = 资产内**轨道/骨架声明序**，与 runtime 内部稳定顺序一致，回放逐帧一致（`10 D4`）。
- **序列化**：
  - `skeleton` → 稳定路径；`skin`/`animation`/`play_mode`/`speed`/`blend`/`update_mode`/`phys_timestep`/
    `flip`/`color`/`z_index`/`material` 为设计参数，正常入库。
  - `instance` 与 `state` **永久 `transient`**：场景与快照**均不写**（运行时对象不可序列化）；重建时按
    上述同名动画/皮肤/时间**重放绑定**。若需回放精确到帧，快照记 `state.time` 作为**重建提示**，
    由引擎在反序列化后 `SetTime` 恢复（不落组件裸句柄）。

---

## 3. 与已有 2D 渲染组件的对接

| 组件 | 图层/分支 | 输入（模拟态） | 输出中性项 | 回写模拟 |
|------|-----------|----------------|------------|:--------:|
| `AnimatedSprite` | 2D 图层 | 帧序资产 + `frame` + Transform2D | 精灵（region + tint） | 否 |
| `SpineSkeleton` | 2D 图层 | 运行时姿势 + Transform2D | 网格/图集批（region+顶点） | 否 |

> 二者均由对应模块解释、**布局中立**、可被反射统一读写（`17 §2.6`）；渲染器仅消费**可序列化的绘制描述**
> （`13 §1`），后端不可用或渲染失败**不回写**任何模拟字段（`13 §8`）。

## 4. 未来扩展（同类 2D 运行时）

- **DragonBones**：与 `SpineSkeleton` 同构——新增独立组件（如 `DragonBonesSkeleton`）与独立模块，
  复用相同字段骨架（`skin`/`animation`/`play_mode`/`speed`/`blend`/`instance`），差异仅在运行时封装与
  混合枚举取值。**不合并进** `SpineSkeleton`（单一职责，`17 §2.5`）。
- **通用抽象**：若多个 2D 骨骼运行时字段趋同，可抽出 `Skeleton2DBase` 的**字段约定/接口**（非组件继承），
  由各运行时模块实现；组件仍按运行时各自单列，保证反射布局稳定（`06`）。
- **帧动画与骨骼混合**：允许同一节点挂多个 2D 绘制组件（如 `AnimatedSprite` 作背景层、`SpineSkeleton`
  作角色层），按 `z_index` 排序，互不干扰。

## 5. 状态

| 组件 | 设计 | 评审 | 落地 |
|------|------|------|------|
| `AnimatedSprite` | ✅ | ⬜ | ⬜ |
| `SpineSkeleton` | ✅ | ⬜ | ⬜ |
