# 资产详设 · 骨架与 Spine 2D 数据（SkeletonAsset / SpineAsset）

> 上游：`08`（资源/序列化）、`06`（反射）、`15 §2`（骨骼动画）、`10`（确定性）、`13`（GPU 蒙皮/2D 批处理）、
> `components/06-animation-particle.md §3`（`Skeleton` 3D）、`components/09-skeletal-2d.md §2`（`SpineSkeleton` 2D）。
> 说明：本节分两节描述两类**骨骼资产**——**3D 骨架数据（SkeletonAsset）**与 **Spine 2D 数据（SpineAsset）**。
> 二者机制不同（3D 蒙皮 vs 2D 分层网格形变），故分节；共同点是被组件以 `asset_ref<T>` 引用并驱动运行时姿势。

---

## 第一节 · 3D 骨架数据（SkeletonAsset）

### 1.1 定位与形态

- **内容**：骨骼层级（名、父索引）、**绑定姿态（bind pose）**、**逆绑定矩阵**、可选命名/分组/IK 约束元数据。
  **蒙皮权重**通常挂在**蒙皮网格资产**（顶点属性）上，本节只描述骨架侧的绑定数据（见 §1.6）。
- **源文本（dev source）**：骨骼层级 + 绑定姿态 + 逆绑定矩阵，文本入库、可 diff/AI 读（`08 §3/§4`）。
- **烘焙二进制（cooked）**：并行数组（骨骼名/父索引/绑定 TRS/逆绑定矩阵），按层级序排布，便于逐帧零拷贝遍历。

```
SkeletonAsset ──引用──▶ Skeleton.skeleton (asset_ref, components/06 §3)
     │                          │
   绑定数据(层级/逆绑定)     每模拟步：读骨骼 Transform3D → 合成世界 → world·inverse_bind → skinning_matrices
     │                          └──▶ MeshRenderer 提取到 GPU 蒙皮（13 §5）
```

### 1.2 结构 / 字段

```yaml
skeleton_asset:
  name: humanoid
  bind_pose:               # 绑定姿态（骨架默认局部 TRS）
    roots: [0]
  bones:                   # 按层级序排列（父先于子，稳定序 10 D4）
    - { name: "root",     parent: -1, bind: { pos: "...", rot: "...", scale: "..." },
        inverse_bind: "..." }            # 逆绑定矩阵（world_bind⁻¹）
    - { name: "spine",    parent: 0,  ... }
    - { name: "head",     parent: 1,  ... }
  skinning: Linear                       # SkinGrid 默认；见 components/06 §3 SkinningMode
  constraints: [ ... ]                   # 可选：IK/命名分组元数据（导入期固化）
```

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `name` | `string` | `""` | | 骨架名（稳定标识） |
| `bones` | `BoneB[-1]` | `[]` | `readonly(cooked)` | 骨骼数组，**层级序**（父索引 < 自身索引） |
| `skinning` | `SkinningMode` | `Linear` | | 默认蒙皮算法（`components/06 §3`） |
| `constraints` | `Constraint[]` | `[]` | | IK/命名分组等（导入期固化，运行期只读） |

```yaml
- struct: BoneB                                  # 单骨骼绑定数据
  fields:
    - { name: name_hash,      type: "uint32_t" } # interned 名稳定哈希（06 §5）
    - { name: parent,         type: "int16_t" }  # 父骨骼索引；-1 = 根
    - { name: bind_position,  type: "Vec3" }
    - { name: bind_rotation,  type: "Quat" }
    - { name: bind_scale,     type: "Vec3" }
    - { name: inverse_bind,   type: "Mat4" }     # world_bind⁻¹，蒙皮用
```

- **不变量**：`bones[i].parent < i`（层级序）；父索引用 `int16_t`，`-1` 表根（对齐 `components/06 §3` 的骨架稳定序约定）。

### 1.3 引用与运行时句柄

| 引用方（组件） | 字段 | 类型 |
|----------------|------|------|
| `Skeleton`（3D） | `skeleton` | `asset_ref<SkeletonAsset>`（`components/06 §3`） |
| `Animator`（骨骼轨道） | 经 `Skeleton` 驱动 | 见 `assets/05-animation-clip.md §1` |

- `asset_ref<SkeletonAsset>` 序列化为**稳定路径**；强引用计数保持存活（`07 §6`）。
- **运行时句柄**：加载后产出**只读绑定缓冲**（骨骼层级 + 逆绑定矩阵）句柄，标 `transient`、不序列化；
  反序列化后按稳定路径重建并**重绑**。
- **骨骼实例**：`Skeleton.bone_root` 指向的**骨骼子树**（子实体/节点带 `Transform3D`），按骨架资产内的
  **稳定顺序**索引，**绝不用指针地址排序**（`components/06 §3`、`10 D4`）。
- **输出缓冲**（均 transient）：`Skeleton.poses`（局部姿态）、`Skeleton.skinning_matrices`（上传 GPU）、
  `Skeleton.bounds`（剔除用）。渲染**只读消费**，不回写（`13 §8`）。

### 1.4 生命周期 / 确定性

- 每模拟步：读各骨骼 `Transform3D` → **按层级序**合成世界矩阵 → `world · inverse_bind` → 蒙皮矩阵。**纯确定**。
- 姿态求值顺序 = 骨架资产的**层级序**（父先于子），回放逐帧一致（`15 §2`、`10 D4`）。
- `poses`/`skinning_matrices`/`bounds` 为**派生缓冲**，每步重算，**不进快照、不参与 checksum**（`components/06 §3`）。
- 资源经 VFS **预加载**，帧内不触发异步（`04 §6`、`15 §1`）；高基数骨骼实例走 ECS + 实例化（`12 §4`）。

### 1.5 序列化 / diff

- **资产描述符文本入库**：骨骼按层级序、数组一行一元素、省略默认、稳定路径（`08 §4`）。
- **cooked**：并行数组 + 逆绑定矩阵，按层级序排布，内容哈希命名、可 gitignore（`08 §3`）。
- **组件侧**：仅序列化 `skeleton`（稳定路径）与 `skinning_mode`；`bone_root` 为实体稳定 ID（实例化重绑，`12 §7`）；
  `poses`/`skinning_matrices`/`bounds` 标 `transient`（场景与快照均不写）。

### 1.6 蒙皮权重归属

- **蒙皮权重（每顶点骨骼索引 + 权重）属于蒙皮网格资产**（顶点属性），不属骨架资产：
  骨架提供层级与逆绑定，网格提供权重，二者在 `Skeleton` 处合成蒙皮矩阵（`components/06 §3`、`13 §5`）。
- 需保证：**骨骼索引在骨架资产稳定序内对齐网格权重**；重导骨架时重映射并校验（见 §1.7 风险）。

### 1.7 风险

| 风险 | 处理 |
|------|------|
| 骨骼索引与网格权重错位 | 稳定索引 + 导入期校验；重导时重映射 |
| 逆绑定矩阵精度/漂移 | 烘焙期定点化 + 往返测试（`08 §9`） |
| 骨骼名重命名 | interned 稳定哈希 + schema 迁移（`06 §7`） |
| 高基数骨骼开销 | 实例化 + GPU 蒙皮（`13 §5`、`15 §7`） |
| 骨架/网格不同步 | 内容哈希 + 依赖图增量重烘（`08 §6`） |

---

## 第二节 · Spine 2D 数据（SpineAsset）

### 2.1 定位与形态

- **内容**：导出后的 **Spine 骨架（JSON 或二进制）+ 图集（`SpineAtlas`）+ 动画 + 皮肤 + 附件**，
  由第三方 **Spine 2D 运行时**消费（`components/09 §2`）。
- **与 3D 骨架的区别**：Spine 是 **2D 分层网格形变**（顶点形变 + 附件绘制），**不做 3D 蒙皮、不产出
  `skinning_matrices`**；力学与相位完全不同，故单列（`components/09 §0`）。
- **源文本 vs 二进制**：
  - **文本骨架（Spine JSON）可 diff/AI 读**（`08 §4`）；
  - **二进制骨架 + 图集**按**稳定路径**引用（第三方导出物，作为外部源资产）。
- **第三方资产边界**：Spine 数据由外部工具导出，引擎**不重写其格式**；仅在 `ai` 导入阶段登记稳定 ID、
  建依赖图、按内容哈希缓存（`08 §6`）。**不改动 Spine 内部结构**（保持运行时兼容）。

### 2.2 结构 / 引用

```yaml
spine_asset:
  name: hero
  skeleton: "res://spine/hero.json"      # 或 hero.skel（二进制）；稳定路径
  atlas:    "res://spine/hero.atlas"     # SpineAtlas 文本描述
  textures: ["res://spine/hero.png"]     # 图集纹理页（稳定路径）
  skins:    ["default", "red", "blue"]   # 皮肤名（稳定序）
  animations: ["idle", "run", "attack"]  # 动画名（稳定序，供运行时按名选择）
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `skeleton` | `asset_ref` / 路径 | Spine 骨架文件（JSON 文本或 `.skel` 二进制），稳定路径 |
| `atlas` | `asset_ref<SpineAtlas>` | 图集描述（区域 → 纹理页 + UV），文本可 diff |
| `textures` | `asset_ref<Texture>[]` | 图集纹理页，稳定路径 |
| `skins` | `string[]` | 皮肤名列表（稳定序，供 UI/AI 枚举） |
| `animations` | `string[]` | 动画名列表（稳定序） |

- Spine 内部结构（骨架/动画/皮肤）**由运行时解析**，引擎侧只登记**引用与清单**，不展开为反射字段。

### 2.3 引用与运行时句柄

| 引用方（组件） | 字段 | 类型 |
|----------------|------|------|
| `SpineSkeleton`（2D） | `skeleton` | `asset_ref<SpineAsset>`（`components/09 §2`） |
| `SpineSkeleton`（2D） | `material` | `asset_ref<Material>`（可选 2D 材质） |

- `asset_ref<SpineAsset>` / `asset_ref<Texture>` 为**强引用**，控制资源存活（`07 §6`）。
- **运行时实例句柄 `SpineInstanceHandle`**（不透明 `uint64_t`）：指向第三方运行时分配的对象，
  由 `spine2d` 模块经 **C ABI** 持有与释放（`02` FFI 规则：只暴露 C ABI + 句柄，**分配者释放**）。
  **标 `transient`、绝不序列化**；反序列化后由运行时**重建并重绑**（`components/09 §2.4`）。
- 组件字段 `SpineSkeleton.instance : SpineInstanceHandle`、`SpineSkeleton.state : SpineAnimState`
  均为运行时派生（`transient, readonly`）。

### 2.4 生命周期 / 确定性

- 每模拟步：以固定 `dt·speed` 推进运行时 → 求值姿势 → 产出**只读绘制输入**（网格顶点/贴图区域/混合）。
  **播放求值确定**（`15 §2`）；避免运行时内置依赖系统熵的行为（如随机换肤），随机效果统一走引擎 RNG（`10 D2`）。
- `phys_timestep = Inherit` 时用与主循环一致的固定步（`10 D1`）；**禁**运行时建内部非固定线程/计时器（`10 D6/D7`）。
- 求值顺序 = 资产内**轨道/骨架声明序**，与 runtime 内部稳定顺序一致，回放逐帧一致（`10 D4`）。
- **渲染只读**：姿势/绘制结果不回写任何模拟字段（`03 §0`、`13 §8`）。
- 快照重建：若需精确到帧，快照记 `state.time` 作为**重建提示**，反序列化后由引擎 `SetTime` 恢复
  （**不落组件裸句柄**）（`components/09 §2.4`）。

### 2.5 序列化 / diff

- **SpineAsset**：登记骨架/图集/纹理的**稳定路径**与皮肤/动画清单；文本骨架本身可 diff。
- **组件侧**：`skeleton`/`material` → 稳定路径；`skin`/`animation`/`play_mode`/`speed`/`blend`/`update_mode`/
  `phys_timestep`/`flip`/`color`/`z_index` 为设计参数，正常入库（`components/09 §2`）。
- `instance` 与 `state` **永久 `transient`**：场景与快照**均不写**（运行时对象不可序列化），重建时按同名
  动画/皮肤/时间**重放绑定**。

### 2.6 风险

| 风险 | 处理 |
|------|------|
| 第三方运行时不确定性 | 播放求值确定；随机走引擎 RNG；固定步物理（`10 D1/D2`） |
| FFI 边界（Rust/C++ ↔ Spine） | 只走 C ABI + 句柄；分配者释放；异常不越界（`02`） |
| 运行时对象不可序列化 | `instance`/`state` 永久 transient；重建 + `SetTime` 恢复 |
| Spine 版本漂移 | 记录导出器/格式版本；导入期校验；缓存按内容哈希（`08 §6`） |
| 图集/纹理路径失效 | 稳定路径 + 加载期校验（`08 §5`） |
| 多运行时（DragonBones 等） | 各自单列组件与模块，字段骨架趋同但**不合并**（`components/09 §4`） |

---

## 状态

| 资产 | 设计 | 评审 | 落地 |
|------|------|------|------|
| `SkeletonAsset`（3D 骨架） | ✅ | ⬜ | ⬜ |
| `SpineAsset`（Spine 2D） | ✅ | ⬜ | ⬜ |
