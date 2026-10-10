# 07 · 脚本 / 音频 / 导航 内置组件

> 上游：`17 §2/§3`（组件约定与条目格式）、`14`（脚本）、`12`（场景/生命周期）、`06`（反射/资产引用）、`10`（确定性）、`15`（世界层模块）、`07`（句柄/内存）。
> 本类共 5 个组件：`Script` · `AudioSource` · `AudioListener` · `NavAgent` · `NavMesh`。
> 归属模块：`Script`→script；`AudioSource`/`AudioListener`→audio；`NavAgent`/`NavMesh`→navigation。
> 三者均为**世界层**，但**性质不同**：`Script` 与 `NavAgent` 影响**模拟**（参与回放）；
> **音频不回写模拟**（只读消费），其音频输出本身**排除出确定性**。

---

## 1. Script

- **归属模块 / 上游文档**：`script` 模块 / `14-脚本.md`、`12 §6`（生命周期）、`06 §6`（反射生成绑定）、`10 §2`（确定性）。
- **说明**：节点的**行为入口**。一个 `Script` 组件 = 节点上**一份脚本绑定 + 一个 VM 上下文**；
  同一节点可挂**多个** `Script`（按组件**声明序**执行）。`Script` 通过反射把暴露属性写回**同级/它级组件**（如改 `Transform3D`、驱动 `NavAgent`）。
  脚本源为**文本资产**（`14 §7`），热重载在**帧边界**替换并保留实例状态。
- **依赖类型 / enum**：
  - `ScriptLanguage`：`JS`（quickjs，默认）· `Lua` · `Python` · `V8`（后三者构建期裁剪，`14 §1`）。
  - `ScriptCallback`（位掩码，声明要注册的回调）：`OnEnterTree` · `Ready` · `Process` · `PhysicsProcess` · `OnExitTree` · `OnDestroy`。
  - 暴露属性值类型 `PropertyVariantType`：`Bool` · `Int` · `Float` · `Vec2` · `Vec3` · `Color` · `String` · `EntityRef` · `AssetRef`。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `source` | `asset_ref<ScriptAsset>` | 空 | `ref: ScriptAsset` | 脚本源资产；序列化为**稳定路径**（`14 §7`） |
| `language` | `ScriptLanguage` | `JS` | — | VM 后端；构建期裁剪，非法后端加载期报错 |
| `enabled` | `bool` | `true` | — | 关停则不注册回调、不推进（仍保留状态） |
| `callbacks` | `uint32_t` | `0x3E` | `range` | `ScriptCallback` 位掩码；未置位的回调不注册 |
| `exposed` | `map<string, PropertyVariant>` | 空 | — | **暴露属性值**（见下「序列化」）；键为**稳定属性 ID** |
| `_vm` | `vm_handle` | `0` | `transient` | 本实例 **VM 上下文句柄**；运行时派生，不序列化 |
| `_expose_defs` | `exposed_def_ref` | `0` | `transient`,`readonly` | 从 `source` 反射出的暴露属性定义表（运行时派生） |
| `_init_err` | `uint16_t` | `0` | `transient`,`readonly` | 最近一次求值/加载错误码（`14 §8`，`0`=无） |

- **句柄 / 引用**：
  - `_vm`：`vm_handle`（`07 §3` 句柄），**transient**——指向 `14 §1` 的 `mq_script_vm_v1` 实例上下文；实例销毁时释放。
  - `source`：`asset_ref<ScriptAsset>`，**强引用**（控制脚本源存活，`07 §6`），序列化为稳定路径。
  - `_expose_defs`：指向由 `source` + 反射描述符生成的**只读定义表**，运行时派生。
- **生命周期 / 确定性**（`12 §6` / `14 §4`）：
  - 回调在**固定点、确定顺序**调用：`on_enter_tree → ready → [process(dt) | physics_process(dt)]* → on_exit_tree → on_destroy`。
  - 顺序 = **深度优先（父先子后）+ 声明序（同节点多脚本按组件顺序）**，全局确定 → 回放一致（`10 §5`）。
  - **GC 确定性**：quickjs 用 refcount，**cycle GC 仅在帧边界 `gc_step`** 驱动；**模拟帧内禁止隐式 GC**（OD-5 / `14 §5`），否则析构时机漂移 → 回放失步。
  - 脚本 RNG 必须走**实例 RNG**（`10 D2`），禁 `Math.random` 直连系统熵；禁阻塞、禁发起异步（`14 §4`）。
  - 对象析构的**外部副作用**（释放 GPU/句柄资源）延迟到确定点执行。
- **序列化**：
  - **不序列化（transient）**：`_vm`、`_expose_defs`、`_init_err`——运行时按 `source` 重建。
  - **暴露属性值存储**：`exposed` 序列化为 `{ property_id : u64 (FNV-1a 稳定哈希), type_tag, payload }` 的**有序数组**（按 `property_id` 升序，保证可 diff / 确定）。
    - 加载时按 `property_id` + 属性名**双重匹配**：命中→注入；缺失→用默认值并告警（属性改名 = 稳定 ID 变化）。
    - **绝不**按序号存储（增删属性会漂移，`06 §5`）。
  - 热重载：序列化 `exposed` + 脚本可见状态 → 帧边界重建 VM → 反序列化回灌（`14 §7`）。

---

## 2. AudioSource

- **归属模块 / 上游文档**：`audio` 模块 / `15`（世界层）、`13 §8`（渲染只读消费）、`10 §2`（确定性边界）。
- **说明**：音频**发射源**。3D 空间化时需与 `Transform3D` 配对（提供位置/朝向）；单听者由 `AudioListener` 提供。
  音频是**只读消费模拟状态**的**输出通道**：**不写回**任何模拟字段，**不参与**模拟推进与回放 checksum。
- **依赖类型 / enum**：
  - `AttenuationModel`（音量衰减模型）：`InverseDistance`（默认）· `Linear` · `Exponential` · `CustomCurve`（引用衰减曲线资产）。
  - `PanningMode`（空间化模式）：`Stereo2D`（无视空间）· `Spatial3D`（位置空间化）· `Spatial3D_HRTF`（二期，头相关传输）。
  - `AudioPlayState`：`Stopped` · `Playing` · `Paused`。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `clip` | `asset_ref<AudioClip>` | 空 | `ref: AudioClip` | 音频裁剪资产（`asset_ref`） |
| `bus` | `asset_ref<AudioBus>` | 空 | `ref: AudioBus` | 混音总线；空 → 默认总线 |
| `autoplay` | `bool` | `false` | — | `on_enter_tree` 后自动播放 |
| `loop` | `bool` | `false` | — | 循环播放 |
| `volume` | `float` | `1.0` | `range:[0,4]`,`unit:linear` | 线性增益 |
| `pitch` | `float` | `1.0` | `range:[0.01,4]` | 音高（变速）；回放确定性见下 |
| `panning` | `PanningMode` | `Stereo2D` | — | 空间化模式 |
| `attenuation` | `AttenuationModel` | `InverseDistance` | — | 音量衰减模型（`Spatial3D` 时生效） |
| `curve` | `asset_ref<AudioCurve>` | 空 | `ref: AudioCurve` | `CustomCurve` 时的衰减曲线 |
| `min_distance` | `float` | `1.0` | `unit:m` | 衰减起点 |
| `max_distance` | `float` | `50.0` | `unit:m` | 衰减终点 |
| `doppler` | `float` | `0.0` | `range:[0,2]` | 多普勒系数（`0`=关） |
| `_state` | `AudioPlayState` | `Stopped` | `transient` | 运行期播放状态 |
| `_playhead` | `float` | `0.0` | `transient`,`unit:s` | 播放进度（**sim time** 推进，非墙钟） |
| `_voice` | `audio_voice_handle` | `0` | `transient` | 后端混音声部句柄 |

- **句柄 / 引用**：
  - `clip` / `bus` / `curve`：`asset_ref<T>`（强引用，控制资产存活），序列化为稳定路径。
  - `_voice`：`audio_voice_handle`，**transient**——后端（如 miniaudio）的声部/voice id，实例销毁或停止时释放，**不进序列化**。
- **生命周期 / 确定性**：
  - `autoplay` 在 `on_enter_tree` 后、且资产就绪时触发；播放进度 `_playhead` 以**模拟 dt 的 sim time** 推进（`10 D3`），**不读墙钟**。
  - **音频是否影响模拟**：**否**。音频只读消费 `Transform3D` 等模拟状态并输出采样；**不写回**组件，不进模拟 checksum，**排除出回放确定性**（`15 §4` 类比 GPU 粒子）。
  - 允许音频后端（重采样、HRTF、混音线程）**非确定**；但 `_playhead`/`_state` 的**逻辑推进确定**，保证同一输入下脚本可见的播放态一致。
- **序列化**：
  - **序列化**：`clip`/`bus`/`curve`（稳定路径）、`autoplay`…`doppler` 等配置、可选 `_playhead`（`loop` 时保存以便续播）。
  - **transient**：`_state`（可选重算）、`_voice`——运行时重建。

---

## 3. AudioListener

- **归属模块 / 上游文档**：`audio` 模块 / `15`、`17 §4`（全局/单例语义）。
- **说明**：音频**听者**，提供全局收听点位置/朝向（取本节点 `Transform3D`）。**全局/单例语义**：每实例（每 `view`）建议**唯一**，
  由**校验**保证（多余者报错/仅第一个生效，`17 §4`）。与 `AudioSource` 配对：源经衰减 + 空间化后混到听者。
- **依赖类型 / enum**：无专属 enum（复用 `AudioSource` 的空间化模式概念）；朝向由 `Transform3D` 提供。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `active` | `bool` | `true` | — | 关停则该实例无听者（静音空间化） |
| `master_volume` | `float` | `1.0` | `range:[0,2]`,`unit:linear` | 主音量 |
| `doppler_scale` | `float` | `1.0` | `range:[0,2]` | 全局多普勒缩放 |
| `_listener` | `audio_listener_handle` | `0` | `transient` | 后端听者句柄 |

- **句柄 / 引用**：
  - `_listener`：`audio_listener_handle`，**transient**——后端听者对象 id，运行时派生。
  - 位置/朝向来自**同节点** `Transform3D`（无自有引用）。
- **生命周期 / 确定性**：
  - 校验：加载/实例化后若同 `view` 存在**多个** `active` 听者 → 报错并**只取首个（按节点 id 稳定排序，`10 D4`）**。
  - **不影响模拟**：听者状态只用于音频输出，不回写、不参与 checksum。
- **序列化**：仅配置字段；`_listener` 为 transient。

---

## 4. NavAgent

- **归属模块 / 上游文档**：`navigation` 模块 / `15 §1/§5`（世界层、ECS）、`10 §2`（确定性）、`12 §6`（`physics_process`）。
- **说明**：导航**代理**，驱动节点沿 `NavMesh` 移动。挂 `Transform3D`；与 `NavMesh` 配对（提供可寻路表面）。
  寻路 + 避障在**物理相位 `physics_process(dt)`** 推进（`15 §3`），**参与模拟与回放**。
- **依赖类型 / enum**：
  - `NavAgentState`：`Idle` · `Moving` · `PathPending` · `Arrived` · `OffMesh` · `Failed`。
  - `NavAvoidance`：`None` · `RVO2`（局部避障，需确定实现）。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `navmesh` | `asset_ref<NavMeshData>` | 空 | `ref: NavMeshData` | 导航数据引用；用于查询 |
| `target_entity` | `entity_ref` | 空 | `ref` | 追踪目标实体（弱引用）；与 `target_position` 二选一 |
| `target_position` | `Vec3` | `{0,0,0}` | `unit:m` | 目标点（`target_entity` 为空时生效） |
| `speed` | `float` | `3.5` | `range:[0,100]`,`unit:m/s` | 最大移动速度 |
| `acceleration` | `float` | `8.0` | `unit:m/s²` | 加速度 |
| `radius` | `float` | `0.5` | `unit:m` | 代理半径（对齐烘焙时的 agent 参数） |
| `height` | `float` | `1.8` | `unit:m` | 代理高度 |
| `stopping_distance` | `float` | `0.5` | `unit:m` | 距目标多少即 `Arrived` |
| `max_slope` | `float` | `45.0` | `unit:deg` | 可行走最大坡度 |
| `step_height` | `float` | `0.4` | `unit:m` | 可跨越台阶高度 |
| `avoidance` | `NavAvoidance` | `None` | — | 局部避障模式 |
| `state` | `NavAgentState` | `Idle` | `readonly` | 运行期状态（确定性，随模拟推进） |
| `_path` | `Vec3[]` | 空 | `transient` | 平滑后的路径点（可重算，稳定 id 顺序） |
| `_path_index` | `uint16_t` | `0` | `transient` | 当前路径点索引 |
| `_desired_velocity` | `Vec3` | `{0,0,0}` | `transient` | 本帧期望速度（求解输出） |
| `_query` | `nav_query_handle` | `0` | `transient` | 导航查询上下文句柄 |

- **句柄 / 引用**：
  - `navmesh`：`asset_ref<NavMeshData>`，序列化为**稳定路径**。
  - `target_entity`：`entity_ref`（**弱引用**，`07 §6`）；目标销毁→世代失效→切 `Failed`。
  - `_query`：`nav_query_handle`，**transient**——运行时加速结构（如 Detour `dtNavMeshQuery`）句柄。
- **生命周期 / 确定性**：
  - 在 `physics_process(dt)` 内：`PathPending` →（异步剔除：烘焙好的 `NavMesh` 查询为**同步、帧内**，`10 D7`）→ 生成路径 → `Moving`→`Arrived`。
  - **寻路是否确定**：**是（必须）**。
    - 使用**确定性 A\***（固定启发式权重、**开放表按节点/多边形 id 稳定排序**，`10 D4`，禁指针/地址序）。
    - 路径平滑（漏斗算法）与避障（RVO2）需确定实现：代理处理顺序按 **entity id 升序**；随机扰动走**实例 RNG**（`10 D2`），禁系统熵。
    - 浮点遵循 `10 §6`（默认 L1 保证；RVO 若不可确定则**降级为 `None`** 并告警）。
  - 每帧推进按**固定步长 sim dt**（`10 D1`），结果写回 `Transform3D`（参与 checksum）。
- **序列化**：
  - **序列化**：`navmesh`（稳定路径）、`target_entity`（稳定 ID）、`target_position`、代理参数、`state`（续回放）。
  - **transient**：`_path`、`_path_index`、`_desired_velocity`、`_query`——帧边界/加载时按 `navmesh` + 目标**确定性重算**；不落盘以免路径冗余与版本漂移。

---

## 5. NavMesh

- **归属模块 / 上游文档**：`navigation` 模块 / `15 §5`、`10 §2`（烘焙确定）、`08`（资产生成）。
- **说明**：导航**数据源**组件，标记某节点子树为**可寻路表面**并提供**烘焙参数**；烘焙产物 `NavMeshData` 供 `NavAgent` 查询。
  编辑期承载烘焙输入（几何引用 + 参数），运行期可只保留产物句柄。
- **依赖类型 / enum**：
  - `NavMeshSource`：`Geometry`（引用 `MeshRenderer`/碰撞几何）· `Colliders`（从物理碰撞体烘焙）· `Explicit`（直接引用资产）。
  - `NavMeshBakeState`：`Dirty` · `Baking` · `Baked` · `Failed`（编辑器/工具期）。
- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `source` | `NavMeshSource` | `Geometry` | — | 烘焙输入来源 |
| `geometry` | `asset_ref<Mesh>` | 空 | `ref: Mesh` | `Explicit`/`Geometry` 时的几何引用 |
| `data` | `asset_ref<NavMeshData>` | 空 | `ref: NavMeshData` | **烘焙产物**；运行期主引用（稳定路径） |
| `cell_size` | `float` | `0.3` | `unit:m` | 体素单元尺寸（决定精度/内存） |
| `cell_height` | `float` | `0.2` | `unit:m` | 体素高度 |
| `agent_radius` | `float` | `0.5` | `unit:m` | 烘焙用代理半径（须匹配 `NavAgent.radius`） |
| `agent_height` | `float` | `1.8` | `unit:m` | 烘焙用代理高度 |
| `max_slope` | `float` | `45.0` | `unit:deg` | 可行走坡度 |
| `max_edge_length` | `float` | `12.0` | `unit:m` | 边界细化阈值 |
| `region_min_size` | `float` | `8.0` | `unit:m²` | 最小可行走区域 |
| `bake_state` | `NavMeshBakeState` | `Baked` | `readonly` | 编辑期烘焙状态 |
| `_accel` | `nav_data_handle` | `0` | `transient` | 运行时导航加速结构句柄 |

- **句柄 / 引用**：
  - `data`：`asset_ref<NavMeshData>`（强引用），序列化为**稳定路径**——这是**跨会话的导航数据句柄**。
  - `geometry`：`asset_ref<Mesh>`，序列化稳定路径。
  - `_accel`：`nav_data_handle`，**transient**——从 `data` 构建的运行时加速结构（tile graph）；加载时构建，销毁时释放。
- **生命周期 / 确定性**：
  - **烘焙确定**：给定输入几何 + 参数，烘焙产物**逐位一致**（保证 CI 可 diff、回放可重建）；输入几何遍历按 **entity id 稳定排序**（`10 D4`），内部体素化关闭随机。
  - 烘焙是**离线/编辑期**操作（重、可异步于**非模拟**阶段，`04`）；运行期只做**只读查询**、**帧内同步**（`10 D7`）。
  - 校验：同一导航域仅**一个** `NavMesh` 提供 `data` 于给定表面；重叠报错。
- **序列化**：
  - **序列化**：`data`（稳定路径）、烘焙参数、`source`；`bake_state` 只读。
  - **transient**：`_accel`——加载时从 `data` 重建；不落盘（可重建、平台相关）。

---

## 6. 依赖与配对速览

| 组件 | 必须配对 | 全局/单例 | 参与模拟 | 主要 transient |
|------|----------|-----------|----------|----------------|
| `Script` | 无（行为入口） | 否（可多个） | 是 | `_vm` / `_expose_defs` |
| `AudioSource` | `Transform3D`（时空化时） | 否 | **否**（只读消费） | `_voice` / `_playhead` |
| `AudioListener` | `Transform3D` | **是**（每 view 唯一） | **否** | `_listener` |
| `NavAgent` | `Transform3D` + `NavMesh` | 否 | 是（`physics_process`） | `_path` / `_query` |
| `NavMesh` | 几何源 / `data` 资产 | 否（每域唯一） | 烘焙离线，查询只读 | `_accel` |

> 统一定理：`Script` 暴露属性、`NavAgent` 路径均**确定性重算/回灌**，不落盘派生量；音频句柄全 transient 且**不参与 checksum**（`10 §2` / `14 §5`）。
