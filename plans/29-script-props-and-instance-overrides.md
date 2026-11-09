# 29 · 脚本 props 与 SceneInstance 属性覆盖（实现方案）

> 上游：`docs/32`（场景实例）、`docs/34`（脚本模块与实例上下文）、`docs/27 §6`（组件引用用路径）、
> `plans/26`（native guest 的 `props` POD）。
> 本文回答并落地一个问题：**能否在 `scn.SceneInstance` 的属性里覆盖脚本定义的 attribute？**
> 结论：**现在不能**（两层缺口），本文给出从"零缺口"到"完整 props"的三段实现路径。

## 1. 结论与缺口

| # | 能力 | 现状 | 证据 |
|---|------|------|------|
| 1 | `scn.SceneInstance` 有属性覆盖字段 | **没有**。只有 `source/params/paused/active/detached`（+3 transient） | [scn.yaml:127-139](../modules/core/data/schema/world/scn.yaml#L127) |
| 2 | 脚本 attribute 的存储（`Script.exposed`） | **不可序列化**：`MQ_TK_MAP` 在 JSON/二进制两条 persist 路径都被跳过/写成 `0`，codegen 也不给 map 生成容器算子 | [persist.cpp:169-234](../modules/data/persist/src/persist.cpp#L169)、[persist.cpp:303-358](../modules/data/persist/src/persist.cpp#L303)、[persist_binary.cpp:70-75](../modules/data/persist/src/persist_binary.cpp#L70)、[parser.js:1431-1492](../modules/core/data/parser.js#L1431) |
| 3 | `script.Script.source` 能写路径 | **不能**：字段是 `asset_ref<ScriptAsset>`（u64），`.scn` 里写 `"enemy.js"` 会被 `strtoull(...,16)` 解析成 0 | [script.yaml:44](../modules/core/data/schema/script.yaml#L44)、[persist.cpp:377-390](../modules/data/persist/src/persist.cpp#L377) |
| 4 | `script.Script` 组件脚本会实例化 | **不会**：`modules/runtime` 里没有任何消费 `script.Script` 的代码；只加载场景级 `script`/`scripts`（`node = -1`） | grep `script.Script` 在 `modules/runtime` 无命中；[module.cpp:236-277](../modules/runtime/src/script/module.cpp#L236) |
| 5 | `this.props` 接到 `Script.exposed` | **没接**：JS 上下文里 `props` 是空普通对象；Lua/native 的 `module_instantiate = nullptr`；`mount.props = NULL` | [js_backend.cpp:663-681](../modules/script/src/js_backend.cpp#L663)、[script.cpp:60-114](../modules/script/src/script.cpp#L60)、[native_backend.cpp:164](../modules/script/src/native_backend.cpp#L164) |
| 6 | 官方立场 | **明确不要用**（P1） | [USER_GUIDE.md:45](../USER_GUIDE.md#L45)、[:109](../USER_GUIDE.md#L109)、[:665](../USER_GUIDE.md#L665) |

因此"在 `SceneInstance` 属性里覆盖脚本 attribute"缺的不只是一个字段，而是
**可序列化的 props 存储 + 组件脚本实例化 + props 访问器 + 实例覆盖入口**四件事。

## 2. 目标语义（先冻结）

1. **作用对象**：子场景里**实体上的** `script.Script` 组件（场景级 `script`/`scripts` 的 `node = -1`，
   docs/34 §5.2 规定其**没有 props**，不可被覆盖）。
2. **覆盖时机**：子场景实体加载完成 → **脚本实例化之前**写入 `Script.exposed`；
   脚本 `on_start` 第一次读 `this.props.x` 即得到覆盖值。
3. **覆盖优先级**：模块默认值 < 场景 `.scn` 里 `script.Script.exposed` < 父场景
   `SceneInstance.prop_overrides`（后者按名覆盖前者）。
4. **定位方式**：`target` = 子场景内**实体名**（`SceneNode.name`，在该实例子树内解析；重名取错误而非"第一个"）。
   `target` 为 `""` = 实例根实体（静态实例的挂载壳；动态实例没有壳，故必须写名字）。
5. **运行期修改**：`scene_set_instance_prop_overrides(inst, json)` + `scene_reload(inst)` 才重读
   （与 `params` 同语义；方案 B 的组件字段覆盖对应 `scene_set_instance_overrides`）。
6. **失败策略**：目标实体/组件/属性不存在、类型不匹配 → **告警 + 跳过该项**（不中止场景），
   计入 `SceneInstance.error`/日志；启用了 `MAQUE_STRICT=1` 时升级为构建失败。
7. **确定性**：覆盖按声明序遍历，值写入顺序固定；props 值参与检查和/快照（见 §8 未决项）。

## 3. 方案 B（最小可用，1 天）：通用组件字段覆盖

不碰脚本系统，直接把"覆盖"定义为**对子场景任意组件字段做部分 JSON 赋值**。
即使方案 A 未落地，它也能覆盖 `Transform2D.x`、`Sprite.texture`、`render.MeshRenderer.mesh_path` 等，
并且是方案 A3 的运行时骨架。

### 3.1 schema

```yaml
# modules/core/data/schema/world/scn.yaml
- struct: SceneOverride
  fields:
    - { name: target,    type: string }   # 子场景内实体名（"" = 实例根）
    - { name: component, type: string }   # 组件类型全名，如 "scn.Transform2D"
    - { name: json,      type: string }   # 部分对象，如 "{\"x\":100.0}"；按反射只写出现的键
- struct: SceneInstance
  fields:
    ...
    - { name: overrides, type: "SceneOverride[]" }   # 尾部追加，旧 .scn 零变化
```

`SceneOverride[]` 是"具名 struct 的动态数组"，现有 persist 直接支持（`apply_struct` 递归），
**不需要**给 map 补支持、不需要改 codegen。

### 3.2 运行时（约 150 行）

1. `load_entity`（[loader.cpp:151-197](../modules/runtime/src/scene/loader.cpp#L151)）读到静态 `SceneInstance` 时，
   把 `overrides`（以及 A3 的 `prop_overrides`）拷进 `SceneInstanceRuntime`
   （直接存 `std::vector<scn::SceneOverride>` / `std::vector<scn::PropOverride>`，与 `params` 同一处，
   [:171-175](../modules/runtime/src/scene/loader.cpp#L171)）。
2. `Engine::build_instance`（[instance.cpp:236-324](../modules/runtime/src/scene/instance.cpp#L236)）在
   **实体循环之后、`load_instance_scripts` 之前**（[:300-302](../modules/runtime/src/scene/instance.cpp#L300)）调用
   `apply_instance_overrides(inst)`：
   - 解析 target：在 `entity_instance[eid] == inst.id` 的实体里按 `name` 唯一匹配；
   - 找组件：`mq_reflect_type_by_name(reflect, component)` + `comp_by_name`（沿父链）；
   - 解析 json：`mq_json_parse` → 直接 `mq_persist_apply_json(reflect, comp_desc, dom, blob)`。
     **部分对象天然成立**：`apply_struct` 只遍历 JSON 里出现的键（[persist.cpp:160-164](../modules/data/persist/src/persist.cpp#L160)），未知键忽略。
3. 动态实例：新增脚本 API `scene_set_instance_overrides(instance, json)`（入队前写 `inst->overrides`，
   构建时生效；已构建实例需 `scene_reload`）。JSON 形如
   `[{"target":"EnemyA","component":"scn.Transform2D","json":"{\"x\":100}"}]`。
4. `ensure_instance_component`（[instance.cpp:539-565](../modules/runtime/src/scene/instance.cpp#L539)）
   把 `overrides` 回写到挂载壳组件，保证 `Scene.save` 可复原。

### 3.3 例子

```json
{ "name": "EnemySlot", "components": [
  { "type": "scn.SceneInstance", "source": "enemy.scn",
    "params": "{\"hp\":30}",
    "overrides": [
      { "target": "Body", "component": "scn.Transform2D", "json": "{\"x\":40.0,\"y\":12.0}" },
      { "target": "Spear", "component": "scn.Sprite", "json": "{\"texture\":\"builtin:/textures/checker\"}" }
    ] }
]}
```

### 3.4 测试

- 单测：`SceneOverride[]` JSON 往返 + 部分对象只改出现的键（persist_test）。
- e2e：`scenes/instance_override/`，父场景覆盖子场景实体的位置/贴图 → `--shot` 像素差 + `Scene.get` 断言。
- 错误：不存在 target / component / 非法 json → 告警、其余覆盖仍生效。
- 确定性：同场景跑两遍 checksum 一致。

## 4. 方案 A（真正回答"脚本 attribute"）：props 三段式

方案 B 只能覆盖**组件字段**。要让"脚本声明的 attribute"可覆盖，必须先把 props 变成**可序列化的组件状态**，
再把子场景实体上的脚本实例化出来，最后让 `SceneInstance.overrides` 指向 props。

### 4.1 A1：让 `Script.exposed` 可持久化（0.5–1 天）

**改动**：`Script.exposed` 由 `map<string, PropertyVariant>` 改为**有序数组**：

```yaml
# modules/core/data/schema/script.yaml
- struct: ScriptProp
  fields:
    - { name: name,  type: string }
    - { name: value, type: script.PropertyVariant }
- struct: Script
  fields:
    - { name: source, type: string }          # 由 asset_ref<ScriptAsset> 改为 VFS 路径（相对场景目录）
    ...
    - { name: exposed, type: "ScriptProp[]" } # 顺序 = 声明序（确定性；查找按 name 线性，N 很小）
```

**为什么不是 map**：codegen 对 `MQ_TK_MAP` 不生成容器算子（[parser.js:1461](../modules/core/data/parser.js#L1461)），
JSON persist 的 `apply_field`/`write_json_field` 都没有 map 分支（写出去是 `0`），
二进制快照也跳过 map（[persist_binary.cpp:70-75](../modules/data/persist/src/persist_binary.cpp#L70)）。
改成"具名 struct 动态数组"**零内核改动**即可获得 JSON 读写 + 二进制快照 + `Scene.get` dump。

**为什么 source 改 string**：`asset_ref<T>` 在 `.scn` 里只接受 `"0x…"`/数字（[persist.cpp:377-390](../modules/data/persist/src/persist.cpp#L377)），
路径写进去等于 0；`AudioSource.clip` 已确立"组件引用用路径而非 `asset_ref`"的先例（docs/27 §6）。
`ScriptAsset` 类型当前无任何消费方（`modules/asset` 无命中），改动安全。

**顺带**：更新 `docs/34 §5.4/§5.6`、`docs/components/07`、`USER_GUIDE.md` 的"map"表述；
`script.yaml` 的 `module_abi.version` 4 → 5（A2 一起做）。

### 4.2 A2：组件脚本 + props 访问器（2–3 天）

#### A2.1 脚本模块 ABI 追加（只增不改）

```c
/* script.yaml module_abi */
- { fn: mq_script_module_instantiate2, returns: "mq_script_instance*",
    args: [ { name: module, type: "mq_script_module*" }, { name: node, type: int32_t },
            { name: scene, type: int32_t }, { name: params, type: "const char*" },
            { name: package_key, type: "const char*" }, { name: props, type: "const char*" } ],
    doc: "props = 已解析的属性覆盖 JSON（[{name,type,value}]），与模块默认值合并" }
- { fn: mq_script_instance_props, returns: int,
    args: [ { name: instance, type: "mq_script_instance*" }, { name: out, type: char* }, { name: cap, type: int } ],
    doc: "把当前 this.props 序列化为 [{name,type,value}]；无 props 声明返回 1" }
```

旧 `mq_script_module_instantiate` 保留（等价于 `props = NULL`），`ScriptOps` 增加两个函数指针，
Lua/native 仍为 `nullptr` → **组件脚本只支持 JS + 包模式**（与 docs/34 P0 边界一致，加载期明确报错）。

#### A2.2 JS 后端（约 200 行）

1. `module_load_esm` 求值后读模块命名空间里的 `props` 导出（对象）：
   ```js
   export const props = { hp: 20, tint: [1, 0.5, 0.5, 1], title: 'slime' };
   ```
   类型推断：`bool → Bool`、`number → Float`、`string → String`、`[2]/[3]/[4] → Vec2/Vec3/Color`；
   需要 Int/EntityRef/AssetRef 时写显式形式 `{ type: 'int', default: 3 }`（同一份 defs 同时是默认值）。
2. `module_instantiate2`：先建实例 + 现有上下文（`node/scene/package/params`），
   再建 `this.props` = 默认值 ∪ `props` JSON 覆盖值（按名、按 defs 类型转换）。
3. `instance_call` 返回前（[js_backend.cpp:803-832](../modules/script/src/js_backend.cpp#L803)）：
   若模块声明了 props，`mq_script_instance_props` 按 defs 顺序把 `this.props` 序列化为
   `[{name,type,value}]`（值从 JS 读回，类型取 defs）。
4. 兼容：模块没有 `props` 导出 → 行为与现在完全一致（`props` 是空对象、序列化返回 1）。

> 备选（更"文档化"但更重）：给每个 prop 建 getter/setter，直连宿主 `props_get/props_set`。
> 需要 runtime 维护 `current_context` 栈（docs/34 §5.4）和逐名前缀；**建议先不做**，
> 采用"镜像对象 + 调用后回写"，语义差异只有"引擎侧改 `exposed` 帧内不可见"，可接受。

#### A2.3 runtime（约 250 行）

1. `Engine` 新增 `struct EntityScript { int entity; mq_script_instance* ci; std::string key; }`，
   `SceneInstanceRuntime` 增 `std::vector<EntityScript> entity_scripts`（与 `module_instances` 同一生命周期）。
2. `load_instance_scripts`（[module.cpp:236-277](../modules/runtime/src/script/module.cpp#L236)）之前新增
   `load_entity_scripts(inst)`：按**树前序**遍历 `entity_instance[eid] == inst.id` 的实体，
   取 `script.Script` 组件 → 读 `source`（`vfs_join(inst.base, rel)` + 包键归一化，逻辑与场景级相同）→
   `mq_script_module_load_esm`（同键缓存，多实体共享模块）→ `mq_script_module_instantiate2(..., props_json)`。
   `props_json` 来自组件 `exposed`（初始值 = 场景里写的覆盖）。
3. 实例化后立刻 `mq_script_instance_props` 回填组件 `exposed`（把默认值落成世界状态）。
4. `call_module_event`（[module.cpp:216-234](../modules/runtime/src/script/module.cpp#L216)）在调用返回后
   再调一次 `mq_script_instance_props`，把结果写回组件（按 name 更新 `ScriptProp.value`，不重排序）。
5. 派发：在 `call_instance_event`（[instance.cpp:76-95](../modules/runtime/src/scene/instance.cpp#L76)）里，
   先按前序调 entity scripts，再调场景级 `module_instances`（docs/34 §5.3 的"节点先、场景级最后"）；
   `on_destroy` 用后序。
6. 销毁：`destroy_instance_content` 里先派发 `on_destroy` → 销毁 `entity_scripts` → 现有逻辑。
   实体运行期被 `node.destroy`/`Scene.remove` 时（[frame.cpp:518](../modules/runtime/src/engine/frame.cpp#L518)）
   也要销毁其脚本实例（在 `flush_pending_destroy` 前按 id 查表）。
7. `enabled=false`：实例照建、不派发（复用现有 `Script.enabled` 语义）；`callbacks` 位掩码过滤回调。

### 4.3 A3：`SceneInstance.prop_overrides` 指向 props（1–2 天）

```yaml
# scn.yaml 需追加 `- imports: [../script.yaml]`（引用 script.PropertyVariant，无循环）
- struct: PropOverride
  fields:
    - { name: target, type: string }            # 子场景实体名（"" = 实例根）
    - { name: prop,   type: string }            # props 名
    - { name: value,  type: script.PropertyVariant }
- struct: SceneInstance
  fields:
    ...
    - { name: prop_overrides, type: "PropOverride[]" }   # 与 B 的 overrides 分列，语义不混
```

> 为什么分开两个数组：B 的 `overrides` 目标是"组件字段 + JSON 片段"，A3 的是"脚本 prop + 类型化值"；
> 合成一个字段要靠"component 是否为空"分支，容易误用。两者共用同一个 apply 函数与插入点即可。

时序（**关键**）：`build_instance` 的实体循环结束后、`load_entity_scripts` 之前，
把 `inst.prop_overrides` 逐条写入目标实体 `script.Script.exposed`（不存在则追加，已存在则按名替换）；
随后 `load_entity_scripts` 用这份 `exposed` 作为 `props` 传给 `instantiate2`——
`on_start` 读到的就是覆盖值，**不需要**在脚本里手动解析 `params`。

例子：

```json
{ "name": "EnemySlot", "components": [
  { "type": "scn.SceneInstance", "source": "enemy.scn",
    "prop_overrides": [
      { "target": "Body", "prop": "hp",
        "value": { "type": 1, "i": 30 } },
      { "target": "Body", "prop": "tint",
        "value": { "type": 5, "v": { "x": 1.0, "y": 0.2, "z": 0.2, "w": 1.0 } } }
    ] }
]}
```

```js
// enemy/body.js —— 声明即可被覆盖，无需读 params
export const props = { hp: 10, tint: [1, 1, 1, 1] };
export default {
  on_start() { /* this.props.hp === 30 */ },
  update(dt) { this.props.hp -= dt; }   // 调用后由引擎回写 Script.exposed
};
```

动态组合：`scene_set_instance_prop_overrides(id, json)`（入队前，构建时生效）或构建后
`scene_reload(id)`；`ensure_instance_component` 把 `prop_overrides` 回写到挂载壳组件，
`Scene.save` 可复原（与 `params` 同路径）。

## 5. 时序总览

```
父 .scn 反序列化
  └─ load_entity(SceneInstance 组件)
       ├─ 读 source/params/overrides/prop_overrides → SceneInstanceRuntime
       └─ build_instance(child)
            ├─ load_entity(...)              # 子场景实体（含 script.Script 组件）
            ├─ apply_instance_overrides      # B：写组件字段
            ├─ apply_instance_prop_overrides # A3：写 Script.exposed
            ├─ load_entity_scripts           # A2：instantiate2(props=exposed)
            ├─ load_instance_scripts         # 场景级脚本（node=-1）
            └─ rebase_instance_assets
每帧 update： 场景实例前序 → 实例内节点前序 → 场景级 → global → overlay
每次调用返回：mq_script_instance_props → 回写 Script.exposed（同步、同帧）
卸载：       on_destroy 后序 → 销毁节点脚本 → 销毁场景级脚本
快照/恢复：  Script.exposed 随组件入快照（A1 后）；恢复后重建实例 → props 从 exposed 初始化
```

## 6. 错误、诊断、确定性

| 场景 | 行为 |
|------|------|
| `target` 找不到 / 重名 | 告警 `instance-override: target 'X' not found/ambiguous`，跳过该项；实例仍正常构建 |
| `component` 找不到 / json 非法 | 告警并跳过该项 |
| `prop` 未在模块 defs 声明 | 告警并跳过（不静默新增字段） |
| 类型不匹配 | 按 defs 类型转换；不可转换 → 告警 + 保留默认值 |
| `script.Script` 用了 Lua/native | 加载期 `inst.error = "component script requires js package"`，脚本跳过 |
| 诊断开关 | `MAQUE_INSTANCE_INFO=1` 打印每条覆盖的 target/component/result |
| 确定性 | 覆盖按数组序、props 按 defs 序写回；字符串不参与 checksum（沿用现值策略，见 §8） |

## 7. 测试计划

| # | 用例 | 断言 | 层 |
|---|------|------|----|
| 1 | `ScriptProp[]` / `SceneOverride[]` JSON 往返 | 逐字节一致；缺键不覆盖 | persist_test |
| 2 | 部分对象只改出现的键 | 未出现的字段保持原值 | persist_test |
| 3 | props defs 解析 + 类型推断 | Float/String/Color 正确 | script_test |
| 4 | `instantiate2` 合并默认值+覆盖 | `this.props.hp == 30` | script_test |
| 5 | 调用后回写 | 改 `this.props.hp` → 组件 `exposed` 变化 | script_test |
| 6 | 无 props 导出零变化 | 行为与今天一致 | script_test |
| 7 | 场景 e2e：父覆盖子属性 | `--shot` 像素差 + `Scene.get` 断言 | `ctest -R instance_override_e2e` |
| 8 | 重名/缺失/类型错误 | 告警 + 其余覆盖生效 | e2e |
| 9 | 确定性 | 同场景两次 checksum 一致 | e2e |
| 10 | 动态实例 + reload | 改 `prop_overrides` 后 `scene_reload` 生效 | e2e |
| 11 | 快照往返 | props 值恢复 | snapshot_test |

新增/修改的 ctest：`persist`、`script`、`instance_override_e2e`（新）、`snapshot`、`bindings_e2e`（新 API）、
既有 `nested_e2e`/`td_e2e` 零回归。

## 8. 风险与未决

| 风险 / 未决 | 处理 |
|-------------|------|
| `exposed` 从 map 改数组：与 `docs/34 §5.4` 的 map 表述不一致 | 同步改文档；无消费代码，实际不破坏任何场景 |
| `Script.source` 改类型 | 无消费方；`docs/components/07` 的 "asset_ref 序列化为路径" 说法一并修正 |
| 一个实体只能有一个同类型组件（`comps` 按 type_id 存） | 与 docs/34 §5.6 "多 Script" 冲突；P1 只支持**一个** `script.Script`/实体，文档写清 |
| 组件脚本的输入事件作用域 | 与场景级一致（实例内每脚本都收到，脚本自查 `this.node == target`）；按节点过滤留 P2 |
| 运行期 `node.destroy` 的实体脚本 | A2 先按 id 查表销毁；"实体级热重载/重挂"留后续 |
| props 是否进 checksum | 容器/字符串当前跳过；建议对 `Script.exposed` 的**数值成员**按 (entity,name) 排序显式混入（新增，老场景无 Script 组件 → 零回归） |
| Inspector 编辑 `exposed` | `Scene.get/set` 只支持标量/字符串（[control/scene.cpp:91-178](../modules/runtime/src/control/scene.cpp#L91)）；数组编辑留后续 |
| 每次调用序列化 props 的开销 | props 数量小；先做"结果与上次相同则跳过写回"的廉价优化，必要时再加 dirty 标记 |
| Lua/native 组件脚本 | 明确报错；native 的 `mount.props` 已预留 POD，P2 再接 |

## 9. 工作量与里程碑

| 阶段 | 内容 | 估时 | 可独立交付 |
|------|------|------|-----------|
| B | 通用组件字段覆盖（schema + runtime + 动态 API + e2e） | 0.5–1 天 | ✅ 立刻解决"实例属性覆盖组件字段" |
| A1 | `exposed` 数组化 + `source` 路径化 + 文档/生成物 | 0.5–1 天 | ✅ 数据面就绪（脚本仍不生效） |
| A2 | 组件脚本实例化 + props ABI + JS 后端 + 回写 | 2–3 天 | ✅ `this.props` 可用（场景内可写 `.scn` 初值） |
| A3 | `SceneInstance.overrides` → props + 动态 API + 快照/回写 | 1–2 天 | ✅ 完整回答原始需求 |
| T | 测试/文档/回归 | 1 天 | — |

顺序：**B → A1 → A2 → A3**。B 的 `apply_instance_overrides` 与 A3 的 `apply_instance_prop_overrides`
共用同一个插入点（`build_instance` 实体循环之后、加载脚本之前）与同一套 target 解析；
两者字段分列（`overrides` / `prop_overrides`），互不干扰。

## 10. 验收（DoD）

- `scenes/instance_override/`（B，组件字段）与 `scenes/instance_prop_override/`（A3，脚本 props）：
  父场景覆盖子场景实体，`on_start`/首次提取即读到覆盖值，`--shot` 有像素差；
- 未声明 prop / 未知 target → 告警且不影响其余覆盖；
- `ctest --test-dir build --output-on-failure` 全绿（含 `persist`/`script`/`snapshot`/新 e2e）；
- 同一场景两次运行 checksum 一致；
- `docs/32`、`docs/34`、`docs/components/07`、`USER_GUIDE.md` 同步更新，`USER_GUIDE` 的"不要用 props"改为正式用法；
- 生成物（`generated/`）与 schema 一致（`ctest -R codegen_drift`）。
