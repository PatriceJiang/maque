# 01 · 内置组件设计：结构与元数据

> 上游：`17`（索引与设计约定，本文严格遵守 §2/§3）、`06`（反射 = 组件形状唯一真相）、
> `12`（Scene=Node=Prefab / 双视图 / 生命周期）、`07`（句柄/内存/引用生命周期）、`08`（资产稳定 ID）。
>
> 本类共 5 个组件：**Name · Active · Tags · Meta · PrefabInstance(二期)**。
> 它们不产生绘制/物理/脚本行为，只描述**节点是谁、是否生效、如何归类、如何被外部引用、如何从 Prefab 派生**。
> 所有字段均为反射 POD：`schema.yaml` 描述，`static_assert(sizeof==size)`；只放数据 + 句柄，禁裸指针与 `std::`。

---

## 1. Name

**归属模块 / 上游文档**
- 模块：`world`（场景/节点树）。上游 `12 §3`、`06 §3`。

**说明**
- 节点的人类可读名。用于编辑器大纲（Outliner）、日志、AI 定位、调试与脚本按名查找。
- **语义名 ≠ 类型**：`type_id`（反射类型）决定"它是什么"，`Name` 只决定"它叫什么"，
  两者互不推导。名**不保证全局唯一**（唯一性由编辑器/AI 按需校验，不作运行时约束）。
- 何时挂：几乎每个 authored 节点都挂；纯 ECS 派生实体（bake 产物）通常**不带** `Name`（避免高基数开销）。
- 与谁配对：无强制配对。脚本按名查找时依赖它与脚本 VM 的查找接口（`14`）。

**依赖类型 / enum**：无。

**字段表**

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `value` | `fixed_string<64>` | `""` | `tooltip: 节点显示名` | UTF-8 显示名。定长上限 64 字节，超长截断并告警 |
| `hash` | `uint32_t` | `0` | `transient readonly` | `value` 的稳定哈希，运行时派生，用于 O(1) 按名查找比较；不序列化 |

**句柄/引用**：无引用字段。

**生命周期 / 确定性**
- 纯数据，无生命周期回调；节点在 `on_enter_tree` 前即应有值。
- 按名查找必须**先比 `hash` 再比 `value`**，命中顺序按节点 id 排序（`07 §9`），禁按地址。

**序列化**
- `value` 序列化（字符串明文）。
- `hash` 标 `transient`，不入库，加载后重算。

---

## 2. Active

**归属模块 / 上游文档**
- 模块：`world`（场景/节点树）。上游 `12 §6`（生命周期）、`13`（渲染消费"是否生效"）。

**说明**
- 控制节点**是否生效**，并沿子树向下传播（Unity 语义的 `activeSelf` / `activeInHierarchy`）。
- 语义：`enabled=false` 的节点及其**所有后代**在运行时被跳过（不参与 process/physics/render/脚本回调），
  但不改变节点树结构、不销毁、不重绑引用。隔离子树调试依赖此组件（`12 §4`）。
- 何时挂：需要开关能力的节点挂；未挂时按"默认生效"处理（缺省即 `enabled=true`）。
- 与谁配对：可挂于任意节点；与 `Script` 的 `process` 回调、`13` 渲染收集、物理步进共同决定"是否被消费"。

**依赖类型 / enum**：无（用 `bool`）。

**字段表**

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `enabled` | `bool` | `true` | `tooltip: 本节点是否生效` | authored 开关（等价 `activeSelf`）；false 时本节点及后代整体跳过 |
| `active_in_hierarchy` | `bool` | `true` | `transient readonly` | 本节点 ∧ 全部祖先 `enabled` 的合成结果（`activeInHierarchy`）；运行时按确定顺序自根向下重算，不序列化 |

**句柄/引用**：无引用字段（祖先关系走节点记录的 `parent`，不在此组件内）。

**生命周期 / 确定性**
- `active_in_hierarchy` 在每帧生命周期起点、按**根→叶深度优先 + 声明顺序**一致重算（`12 §6`），与 `process` 顺序对齐。
- 传播是**纯函数**（父 × 子），无墙钟/熵，结果确定。

**序列化**
- `enabled` 序列化。
- `active_in_hierarchy` 标 `transient`，加载后由重算派生。

---

## 3. Tags

**归属模块 / 上游文档**
- 模块：`world`（场景/节点树）。上游 `12 §3`（组合式能力）、`07 §7`（interned string）。

**说明**
- 节点上的**轻量分类标记集合**（Godot group / Unity tag 的数据化合并）。用于批量查询、系统筛选、
  脚本归类（`FindNodesWithTag`）、AI 语义标注。
- **与标签语义的边界**：`Tags` 是"可多重、可增删、运行时用于筛选"的字符串集合；
  若需要**编译期已知的类型级分类**，应新增自定义组件或用 `type_id`，**不要**把类型编码进 Tags。
- 何时挂：需要被系统或脚本按标签筛选的节点挂；无归类需求的不挂（省内存）。
- 与谁配对：无强制配对；脚本查询接口（`14`）、系统筛选器是消费者。

**依赖类型 / enum**：依赖 interned string 机制（`07 §7`）。

**字段表**

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `values` | `string[]` | `[]` | `ref: interned_string` | interned 标签集合，UTF-8，无重复、**按字典序去重排序存储**以保证确定性 |
| `count` | `uint32_t` | `0` | `transient readonly` | 当前标签数，运行时派生缓存；不序列化 |

**句柄/引用**：`values` 的元素是 interned string 句柄（`07 §7`），非裸 `char*`。

**生命周期 / 确定性**
- 增删标签后须重排并重算 `count`；迭代按字典序（id 序），禁按地址（`07 §9`）。
- 无生命周期回调。

**序列化**
- `values` 序列化为**标签字符串列表**（interned 表在加载期重建）。
- `count` 标 `transient`。

---

## 4. Meta

**归属模块 / 上游文档**
- 模块：`world`（场景/节点树）。上游 `12 §2/§7`（稳定 ID、引用重绑）、`08 §3`（稳定 ID/path）、`11`（AI 读写任意路径）。

**说明**
- 节点的**自由元数据 + 稳定引用标识**。存放不适合固定 schema 的编辑器/AI 注解，以及序列化时供跨引用解析的
  **稳定节点 ID**（`12 §7`：实例化后引用重绑以稳定 ID 为锚，句柄为运行时表示）。
- 典型用途：来源溯源（`source_scene` / `source_prefab`）、分类路径、AI 语义提示、作者备注。
- 何时挂：需要稳定引用锚点或自定义注解的节点挂；纯临时节点可不挂。
- 与谁配对：`PrefabInstance` 的 override 定位依赖本组件的 `stable_id`；资产管线（`08`）用 `source_*` 溯源。

**依赖类型 / enum**：依赖 `map<K,V>` 容器类型（`06 §3`）；`asset_ref<T>` 引用类型。

**字段表**

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `stable_id` | `uint64_t` | `0` | `readonly tooltip: 稳定节点 ID` | 编辑期分配、跨序列化稳定的引用锚点；`0=未分配`（无跨引用需求时合法） |
| `entries` | `map<string,string>` | `{}` | `ref: interned_string` | 任意键值注解（UTF-8）；键按字典序存储，值明文 |
| `comment` | `fixed_string<256>` | `""` | `tooltip: 作者备注` | 编辑器大纲/属性面板展示的备注 |
| `source_scene` | `asset_ref<Scene>` | `null` | `ref: Scene` | 溯源：节点来源场景资产；无则 `null` |
| `source_prefab` | `asset_ref<Prefab>` | `null` | `ref: Prefab` | 溯源：节点来源 Prefab 资产；无则 `null` |

**句柄/引用**
- `stable_id`：稳定 ID（序列化为整数），反序列化后建立 `stable_id → 节点句柄` 映射供引用重绑。
- `source_scene` / `source_prefab`：均为 `asset_ref<T>`（存句柄、强引用计数，`07 §6`/`08 §3`），
  序列化为资产**稳定 ID / 路径**，加载期解析。

**生命周期 / 确定性**
- `stable_id` 在编辑期分配后不再变化；运行期只读。
- `entries` 迭代按字典序（`07 §9`）。

**序列化**
- `stable_id`、`comment`、`entries`、`source_scene`、`source_prefab` 全部序列化。
- 引用字段以**稳定 ID / 路径**入库（`08 §3`），禁顺序号。
- 无 transient 字段。

---

## 5. PrefabInstance（二期，设计草案）

> **状态：二期。** 一期默认"实例化 + 引用重绑"（`12 §7`），本组件在一期**不落地**；
> 下列为局部覆盖（override 列表 / variant）的**设计草案**，供二期评审。

**归属模块 / 上游文档**
- 模块：`world`（场景/节点树）。上游 `12 §2/§7`、`08 §3`（稳定 ID/path）、`06 §3`（引用类型）。

**说明**
- 表达一个节点子树是某 `Prefab` 的**实例**，并记录相对源 Prefab 的**局部覆盖列表**（Godot `PackedScene` 实例 +
  Unity `PrefabInstance` 的数据化合并）。
- **设计要点**：覆盖采用"**值就地 + 路径标记**"模型——被覆盖字段的**实际值已写在实例节点的对应组件里**，
  本组件的 `overrides` 只记录**哪些字段偏离了源**（path + type_id 标记），从而：
  - 避免在本组件内嵌套任意变体值（保持 POD、避免动态 union）；
  - 支持"重置为源值"= 删除标记 + 从源拷贝值；
  - variant（Prefab 变体）可在此基础上叠加一层 override。
- 何时挂：作为 Prefab 实例根的节点挂且**仅根挂一处**；后代不重复挂。
- 与谁配对：与 `Meta.stable_id`（override 定位锚点）、`asset_ref<Prefab>`（源资产）配对。

**依赖类型 / enum**
- 依赖 `asset_ref<T>`、`entity_ref` 引用类型（`06 §3`）。
- 新增辅助 POD：`PrefabOverride`（本组件内联的子记录，非独立组件）：

```yaml
- namespace: world
  type_list:
    - struct: PrefabOverride
      fields:
        - { name: node,         type: "entity_ref" }        # 实例内被覆盖节点（重绑后）
        - { name: source_stable_id, type: "uint64_t" }      # 对应源 Prefab 内节点稳定 ID
        - { name: component_type_id, type: "uint32_t" }     # 被覆盖组件类型（稳定哈希）
        - { name: field_path,   type: "fixed_string<128>" } # 字段路径，如 "position" / "entries.hp"
```

**字段表**

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `source` | `asset_ref<Prefab>` | `null` | `ref: Prefab` | 源 Prefab 资产；强引用计数（`07 §6`），控制其存活 |
| `overrides` | `PrefabOverride[]` | `[]` | | 局部覆盖标记列表；**按 `(source_stable_id, component_type_id, field_path)` 字典序**存储以保证确定性 |
| `is_variant` | `bool` | `false` | | 是否为 Prefab 变体（源本身是一个 Prefab 实例） |
| `override_count` | `uint32_t` | `0` | `transient readonly` | 覆盖项计数缓存；不序列化 |
| `dirty` | `bool` | `false` | `transient readonly` | 运行时：实例相对源是否有待重建/待合并变更；不序列化 |

**句柄/引用**
- `source`：`asset_ref<Prefab>`（存句柄），序列化为 Prefab 的**稳定 ID / 路径**。
- `PrefabOverride.node`：`entity_ref`（弱引用，指向本实例内的节点句柄，`07 §6`）；序列化时用
  `source_stable_id` 表示（映射回源 Prefab，实例重建后重绑为本地句柄），**不**序列化运行时句柄。
- `PrefabOverride.component_type_id` / `field_path`：类型与字段的稳定标识（`06 §5` 稳定哈希），
  跨版本按名匹配 + 迁移（`06 §7`）。

**生命周期 / 确定性**
- 实例化流程：加载 `source` → 拷贝子树 → 重生成 id → **按 `source_stable_id` 重绑内部引用**（`12 §7`）
  → 对本组件 `overrides` 中的字段应用实例节点上的就地值 → 清除 `dirty`。
- 禁止循环实例化（A 含 B 含 A）：加载期检测并报错（`12 §7`）。
- `overrides` 迭代/重建顺序按三元组字典序（`07 §9`），结果确定。
- `dirty` 为运行时派生，参与本帧生命周期点的重建判定，不入序列化。

**序列化**
- `source`、`overrides`（仅存 `source_stable_id` / `component_type_id` / `field_path`，丢弃运行时 `node` 句柄）、
  `is_variant` 序列化。
- `override_count`、`dirty` 标 `transient`。
- 被覆盖字段的**值**存储在实例节点各自的组件记录中并随场景一同序列化（"值就地"原则）。

---

## 附：本类约定小结

| 组件 | 引用 | transient 字段 | 稳定 ID / path | 阶段 |
|------|------|----------------|----------------|------|
| Name | — | `hash` | — | 一期 |
| Active | — | `active_in_hierarchy` | — | 一期 |
| Tags | `string[]`(interned) | `count` | — | 一期 |
| Meta | `asset_ref<Scene>`, `asset_ref<Prefab>` | — | `stable_id` | 一期 |
| PrefabInstance | `asset_ref<Prefab>`, `entity_ref` | `override_count`, `dirty` | `source_stable_id` | 二期 |
