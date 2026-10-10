# 11 · scene（节点树 / ECS 双视图）

> Owner agent：`agent-scene`　上游：`docs/12`（主）、`docs/06`、`docs/07`、`docs/02`、`docs/10`、`docs/00`。
> 铁律：**接口先冻结，测试先行（红→绿→重构）**；接口不完备不得进入实现。
> 本模块所有权：`Scene = Node = Prefab` 统一、节点树 ⇄ ECS 双视图、实例化与引用重绑、确定化生命周期。

## 1. 模块概述

- **职责**：
  1. **节点树（authoring 源）**：`Scene = Node = Prefab` 同一数据形态；节点是**反射记录**
     `{id, type_id, properties, parent, children[]}` + **组合组件（句柄挂载）**，禁止 OOP 继承（ADR-0019）。
  2. **双视图**：节点树为**编辑期唯一真相**，经**单向 bake** 派生 **ECS**（archetype/SoA），运行期热区走 ECS（ADR-0003）。
  3. **实例化 + 引用重绑**：拷贝子树 → 重生成 `id` → 内部引用重绑到本实例 → 外部引用走句柄；**禁循环实例化**（一期）。
  4. **确定化生命周期**：`on_enter_tree → ready → [process | physics_process]* → on_exit_tree → on_destroy`，
     **深序 + 声明序**固定调度；脚本 VM 只注册回调、不自调度（docs/12 §6）。
  5. **接缝供给**：为渲染提供分层 / 相机挂点（渲染本身不属本模块），为动画/物理/粒子提供 bake 后的 ECS 句柄。
- **所在层**：世界层（`modules/world/scene`），运行时行为模块（`.so` + C vtable，docs/02 §10）。
- **上游文档**：`docs/12-场景与世界.md`（主）、`docs/06-反射与代码生成.md`、`docs/07-内存与数据模型.md`、
  `docs/02-模块系统-ABI与多实例.md`、`docs/10-确定性快照与回放.md`。
- **一句话**：把「一棵被 AI/编辑器读写、可实例化、可确定调度的节点树」作为编辑期唯一真相，
  并以**单向 bake** 派生 cache 友好的 ECS，调和「可观测」与「高性能」。

### 1.1 形态与分层

| 子模块 | 目录 | 形态 | 理由 |
|--------|------|------|------|
| `scene` | `modules/world/scene` | 头 `mq_scene.h`（纯 C）+ 运行库 `maque_scene`（`.so`） | 世界层行为模块，冷路径 C ABI（docs/02 §10） |
| `ecs` | `modules/world/ecs` | 头 `mq_ecs.h`（纯 C）+ 运行库 `maque_ecs` | bake 目标；被动画/物理/粒子/渲染消费；**可被 fake 替换** |

> 状态全在 scene instance context（ADR-0014），**零全局可变状态**；节点池 / 组件存储 / bake 映射表均由注入 allocator 分配。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（docs/02）。稳定接口名：`"maque.scene.v1"`、`"maque.ecs.v1"`。
> 所有入口 `noexcept`；错误用返回码；**禁跨边界异常/RTTI/STL/裸指针**。

### 2.1 公共头 `mq_scene.h`——句柄与稳定 ID

```c
/* mq_scene.h —— 纯 C */
#define MQ_SCENE_ABI_VERSION 1
#define MQ_SCENE_MAX_INSTANCE_DEPTH 64      /* 嵌套实例化深度上限，防爆栈 */
#define MQ_NODE_INVALID_INDEX 0xFFFFFFFFu

/* 稳定编辑期 id：由资产显式给定或运行时单调分配；可序列化、跨 reorder 稳定；禁序号语义 */
typedef uint64_t mq_node_id;
#define MQ_NODE_ID_INVALID 0ull

/* 运行期节点句柄：槽 + 世代 + 稳定 id（悬垂可检测，docs/07 §3） */
typedef struct mq_node_ref {
    uint32_t  index;       /* 槽下标；无效 = MQ_NODE_INVALID_INDEX */
    uint32_t  generation;  /* 释放即自增 */
    mq_node_id id;         /* 稳定 id，供序列化/路径/引用 */
} mq_node_ref;             /* 16 字节，POD，可序列化 */

static inline int mq_node_ref_is_valid(mq_node_ref r) { return r.index != MQ_NODE_INVALID_INDEX; }
static inline mq_node_ref mq_node_ref_invalid(void) {
    mq_node_ref r = { MQ_NODE_INVALID_INDEX, 0u, MQ_NODE_ID_INVALID }; return r;
}

/* ECS 实体句柄：与 mq_handle 同构（docs/07 §5） */
typedef struct mq_entity_ref { uint32_t index; uint32_t generation; } mq_entity_ref;

/* 组件句柄 = 通用槽句柄（core/handles），承载该组件的反射 POD 负载 */
typedef mq_handle mq_component_handle;

/* Prefab = 带来源标记的子树引用（实例化输入；一期无 override 列表） */
typedef struct mq_prefab {
    uint64_t   source_scene;   /* 源场景稳定 id，用于循环实例化检测 */
    mq_node_ref root;          /* 源场景内的子树根 */
} mq_prefab;
```

### 2.2 `mq_scene.h`——节点描述与遍历枚举

```c
typedef struct mq_node_desc {
    uint32_t      struct_size;      /* = sizeof(mq_node_desc) */
    uint32_t      abi_version;
    mq_node_id    id;
    mq_type_id    type_id;          /* 节点记录自身的反射类型（properties 的 schema） */
    mq_node_ref   parent;           /* 根节点为 invalid */
    uint32_t      child_count;
    uint32_t      component_count;
    const char*   name;             /* interned UTF-8，可空；路径寻址用 */
    uint64_t      flags;            /* 见 MQ_NODE_FLAG_* */
} mq_node_desc;

#define MQ_NODE_FLAG_INSTANCE_ROOT 0x1ull   /* 本节点是某次 instantiate 的根（用于卸载） */
#define MQ_NODE_FLAG_PREFAB_SRC    0x2ull   /* 来自场景资产（只读源） */
#define MQ_NODE_FLAG_LAYER         0x4ull   /* 渲染分层/相机挂点（docs/12 §5） */

typedef enum mq_order { MQ_ORDER_PRE = 0, MQ_ORDER_POST = 1 } mq_order;
typedef enum mq_phase { MQ_PHASE_PROCESS = 0, MQ_PHASE_PHYSICS = 1 } mq_phase;

typedef mq_result (*mq_node_iter_fn)(void* user, mq_node_ref n);
typedef mq_result (*mq_comp_iter_fn)(void* user, mq_node_ref n, mq_type_id t, mq_component_handle h);
```

### 2.3 `mq_scene.h`——确定化生命周期回调

```c
/* 引擎在固定点、固定顺序调用；脚本 VM（14）只注册、不自调度（docs/12 §6） */
typedef struct mq_lifecycle_v1 {
    uint32_t struct_size;   /* = sizeof(mq_lifecycle_v1) */
    uint32_t abi_version;
    void*    user;          /* 回调上下文（脚本 VM 实例等） */
    mq_result (*on_enter_tree)     (void* u, void* scene, mq_node_ref n);
    mq_result (*on_ready)          (void* u, void* scene, mq_node_ref n);
    mq_result (*on_process)        (void* u, void* scene, mq_node_ref n, float dt);
    mq_result (*on_physics_process)(void* u, void* scene, mq_node_ref n, float dt);
    mq_result (*on_exit_tree)      (void* u, void* scene, mq_node_ref n);
    mq_result (*on_destroy)        (void* u, void* scene, mq_node_ref n);
} mq_lifecycle_v1;
```

> **调用顺序（冻结）**：
> - `on_enter_tree`：前序（parent → children），兄弟按**声明序**；
> - `ready`：整棵子树 enter 完成后，再前序调用；
> - `process` / `physics_process`：前序，兄弟声明序，单节点上组件按**挂载声明序**；
> - `on_exit_tree` / `on_destroy`：后序（children → parent）。
> 任何回调返回非 `MQ_OK` 立即中止该阶段并透传错误（确定性短路）。

### 2.4 `mq_scene.h`——bake 选项与结果

```c
typedef struct mq_bake_opts {
    uint32_t          struct_size;   /* = sizeof(mq_bake_opts) */
    uint32_t          abi_version;
    uint32_t          flags;         /* 见 MQ_BAKE_* */
    const mq_type_id* filter_components;  /* 空 + count=0 = bake 全部组件 */
    uint32_t          filter_count;
    uint32_t          high_cardinality_threshold;  /* 超阈值返回 MQ_ERR_NODE_CAP_EXCEEDED（硬边界，docs/12 §4） */
} mq_bake_opts;

#define MQ_BAKE_INCLUDE_ROOT   0x1u   /* 子树根也生成实体（默认否，根作为 parent 锚） */
#define MQ_BAKE_HIERARCHY      0x2u   /* 写入 Parent 组件以保持层级语义（docs/00 §6） */
#define MQ_BAKE_REBIND_REFS    0x4u   /* 把 entity_ref 字段重绑到本批实体（默认开） */

typedef struct mq_bake_result mq_bake_result;
struct mq_bake_result {
    uint32_t struct_size;   /* = sizeof(mq_bake_result) */
    uint32_t abi_version;
    uint32_t entity_count;
    uint64_t checksum;      /* 派生数据的确定性校验和（docs/10 §5） */
    /* 映射表：所有权归 scene，有效期到下次 bake / scene destroy */
    mq_result (*map_node)  (mq_bake_result* self, mq_node_ref n, mq_entity_ref* out);
    mq_result (*map_entity)(mq_bake_result* self, mq_entity_ref e, mq_node_ref* out);
};

typedef struct mq_snapshot {
    uint32_t    struct_size;   /* = sizeof(mq_snapshot) */
    uint32_t    abi_version;
    const void* data;          /* 不拥有；经 restore 使用；由传入 allocator 释放 */
    size_t      size;
} mq_snapshot;
```

### 2.5 公共头 `mq_ecs.h`——bake 目标（可被 fake 替换）

```c
/* mq_ecs.h —— 纯 C；运行期派生视图，被动画/物理/粒子/渲染消费（docs/15） */
#define MQ_ECS_ABI_VERSION 1

typedef struct mq_ecs_world_v1 {
    uint32_t struct_size;      /* = sizeof(mq_ecs_world_v1) */
    uint32_t abi_version;
    /* 实体生命周期 */
    mq_result (*create_entity)(void* self, mq_entity_ref* out);
    mq_result (*destroy_entity)(void* self, mq_entity_ref e);   /* generation++ */
    /* 层级（Parent 组件语义，docs/00 §6） */
    mq_result (*set_parent)(void* self, mq_entity_ref e, mq_entity_ref parent);
    /* 组件（POD blob；SoA/archetype 布局由实现决定） */
    mq_result (*add_component)(void* self, mq_entity_ref e, mq_type_id t, const void* pod, uint32_t size);
    mq_result (*remove_component)(void* self, mq_entity_ref e, mq_type_id t);
    mq_result (*get_component)(void* self, mq_entity_ref e, mq_type_id t, void** out, uint32_t* size);
    mq_result (*has_component)(void* self, mq_entity_ref e, mq_type_id t);
    /* 迭代：按 {index,generation} 升序（确定性，禁指针序，docs/07 §9） */
    mq_result (*entity_count)(void* self, uint32_t* out);
} mq_ecs_world_v1;
```

### 2.6 操作面 vtable `mq_scene_v1`

```c
typedef struct mq_scene_v1 {
    uint32_t struct_size;   /* = sizeof(mq_scene_v1)，只增不改 */
    uint32_t abi_version;   /* 主版本不匹配即拒绝 */

    /* —— 实例 —— */
    mq_result (*create)(void* host, const mq_allocator* a,
                        const mq_reflect_v1* reflect,   /* 反射（06）：字段读写/描述符 */
                        const mq_ecs_world_v1* ecs,     /* bake 目标（可 fake） */
                        void** out_scene);
    void      (*destroy)(void* self);

    /* —— 节点创建 / 销毁 / 重挂 —— */
    mq_result (*create_node)(void* self, mq_node_ref parent, mq_type_id type_id,
                             const char* name, mq_node_ref* out);
    mq_result (*destroy_node)(void* self, mq_node_ref n);           /* 后序递归销毁，触发 destroy 回调 */
    mq_result (*move_node)(void* self, mq_node_ref n, mq_node_ref new_parent, uint32_t index);
    mq_result (*get_node)(void* self, mq_node_ref n, mq_node_desc* out);
    mq_result (*root)(void* self, mq_node_ref* out);

    /* —— 节点属性（节点记录自身 type_id 的字段，经反射冷路径）—— */
    mq_result (*read_field) (void* self, mq_node_ref n, const char* path, mq_value* out);
    mq_result (*write_field)(void* self, mq_node_ref n, const char* path, const mq_value* in);

    /* —— 组件（句柄挂载、非内联，docs/12 §3）—— */
    mq_result (*attach_component)(void* self, mq_node_ref n, mq_type_id t, mq_component_handle* out);
    mq_result (*detach_component)(void* self, mq_node_ref n, mq_type_id t);
    mq_result (*get_component)   (void* self, mq_node_ref n, mq_type_id t, mq_component_handle* out);
    mq_result (*component_read)  (void* self, mq_component_handle h, const char* path, mq_value* out);
    mq_result (*component_write) (void* self, mq_component_handle h, const char* path, const mq_value* in);

    /* —— 遍历 / 查询（确定性：深序 + 声明序）—— */
    mq_result (*iterate_children)  (void* self, mq_node_ref n, mq_node_iter_fn fn, void* user);
    mq_result (*iterate_subtree)   (void* self, mq_node_ref root, mq_order o, mq_node_iter_fn fn, void* user);
    mq_result (*iterate_components)(void* self, mq_node_ref n, mq_comp_iter_fn fn, void* user);
    mq_result (*find_by_id)        (void* self, mq_node_id id, mq_node_ref* out);
    mq_result (*find_by_path)      (void* self, const char* path, mq_node_ref* out);

    /* —— 实例化 / 重绑（一期）；override 预留接口（二期）—— */
    mq_result (*instantiate)(void* self, const mq_prefab* p, mq_node_ref parent, mq_node_ref* out_root);
    mq_result (*replace_by_prefab)(void* self, mq_node_ref n, const mq_prefab* p);  /* 二期 */

    /* —— 生命周期 —— */
    mq_result (*register_lifecycle)  (void* self, mq_type_id t, const mq_lifecycle_v1* cb);
    mq_result (*unregister_lifecycle)(void* self, mq_type_id t);
    mq_result (*enter_tree)(void* self, mq_node_ref root);
    mq_result (*exit_tree) (void* self, mq_node_ref root);
    mq_result (*step)      (void* self, mq_phase phase, float dt);   /* 全树确定调度 */

    /* —— bake（单向：节点树 → ECS）—— */
    mq_result (*bake)(void* self, mq_node_ref root, const mq_bake_opts* opts,
                      mq_bake_result** out_result);   /* 结果由 scene 拥有，下次 bake/destroy 失效 */

    /* —— 快照 / 序列化（经 mq_visitor，08）—— */
    mq_result (*snapshot)   (void* self, mq_node_ref root, const mq_allocator* a, mq_snapshot* out);
    mq_result (*restore)    (void* self, const mq_snapshot* snap, mq_node_ref parent, mq_node_ref* out);
    mq_result (*serialize)  (void* self, mq_node_ref root, const mq_visitor* codec);
    mq_result (*deserialize)(void* self, const mq_visitor* codec, mq_node_ref parent, mq_node_ref* out);
} mq_scene_v1;

void* mq_module_query(const char* interface_name, uint32_t min_version);
/* interface_name == "maque.scene.v1" → const mq_scene_v1*； "maque.ecs.v1" → const mq_ecs_world_v1* */
```

- **`find_by_path` 语法（冻结）**：`path := ( '/' name | '/' '@' id-hex )+`；`name` 为 `[A-Za-z0-9_-]+`。
  例：`/Player/Sprite`、`/Player/@a1b2c3d4`。语法错误 → `MQ_ERR_PATH_SYNTAX`；不存在 → `MQ_ERR_NOT_FOUND`。
- **`instantiate` 语义**：前序拷贝 `p.root` 子树；每节点 `id'`：
  源内引用 = 由源 `id` 经重绑表映射到新 `id`；外部引用（跨场景）保持句柄原样。
  内部 `entity_ref`/`node_ref` 字段一律经重绑表重写，**未命中即 `MQ_ERR_NOT_FOUND`（不得静默保留）**。
- **循环检测**：实例化栈记录正在解析的 `source_scene` 链；`p.source_scene` 命中栈或深度 > `MQ_SCENE_MAX_INSTANCE_DEPTH`
  → `MQ_ERR_CYCLIC_INSTANCE`。
- **bake 语义**：前序遍历，每节点（按 `opts`）在 `ecs` 建实体，写组件 POD（`reflect` 描述符给出 size）；
  `MQ_BAKE_HIERARCHY` 时写 `Parent` 组件保持层级；`MQ_BAKE_REBIND_REFS` 时把 `entity_ref` 字段按映射重绑。
  映射表由 scene 持有；`bake` **不读回** ECS（单向）。

### 2.7 错误模型

```c
typedef enum mq_result {
    MQ_OK = 0,
    MQ_ERR_INVALID_ARG,
    MQ_ERR_NOT_FOUND,           /* 节点/组件/路径不存在；悬垂句柄 */
    MQ_ERR_STALE_HANDLE,
    MQ_ERR_NODE_HAS_PARENT,     /* 重挂造成自身环或目标非法 */
    MQ_ERR_COMPONENT_EXISTS,    /* 同类型组件重复挂载 */
    MQ_ERR_COMPONENT_MISSING,
    MQ_ERR_CYCLIC_INSTANCE,     /* 循环实例化（含深度超限） */
    MQ_ERR_NODE_CAP_EXCEEDED,   /* 高基数硬边界（docs/12 §4） */
    MQ_ERR_TYPE_MISMATCH,       /* 组件 POD size 与反射描述符不符 */
    MQ_ERR_BAKE_UNSUPPORTED,    /* filter 含不可 bake 组件 / 缺 ECS */
    MQ_ERR_PATH_SYNTAX,
    MQ_ERR_ABI_MISMATCH,
    MQ_ERR_NO_MEMORY
} mq_result;

const char* mq_result_str(mq_result r);   /* 返回静态字符串，跨 .so 安全 */
```

### 2.8 版本化 / 前向兼容（docs/02 R7）

1. `struct_size` **只增不改**：新版本在表尾追加函数；旧调用方按自身 `struct_size` 截断使用。
2. `abi_version` 主版本不匹配即拒绝，返回 `MQ_ERR_ABI_MISMATCH` + 可读串。
3. 接口名字符串稳定：`"maque.scene.v1"`、`"maque.ecs.v1"`；`mq_module_query` 以 `min_version` 协商。
4. 节点/组件 schema 版本由 `reflect` 的 `type_id`/`shape_hash` 保证（docs/06 §7），scene 不另立类型标识。

### 2.9 内存所有权（docs/02 R5 / docs/07）

- **分配者释放**：scene 内所有内存（节点池、组件存储、bake 映射、`snapshot.data`）经 `create` 注入的 `mq_allocator` 分配，
  由 `destroy`/下一次 `bake` 经**同一** allocator 释放；跨 `.so` 绝不 `free` 他方内存。
- **不暴露裸指针**：对外只给 `mq_node_ref`/`mq_component_handle`/`mq_entity_ref`；`component_data` 内部热访问不跨边界暴露。
- `read_field` 返回的 `mq_value.str.ptr`/`items` **不拥有**（指向节点/组件内部或调用方缓冲），语义同 `reflect`。
- **池化**：节点/组件走 pool（docs/07 §2）；高频 create/destroy 不触发通用堆碎片。

### 2.10 线程 / 确定性

- **单线程使用**（ADR-0018）；`step`/`bake`/生命周期回调均在模拟线程。
- 遍历顺序仅取决于**声明序 + 深序**，**绝不按地址/指针/hash 序**（docs/07 §9、docs/10 D4）。
- `bake_result.checksum` 只由字段值与稳定 id 派生；同输入 → 同 checksum（docs/10 §5）。

## 3. 依赖与边界

- **可依赖**：
  - `core/handles`（`mq_handle`、slot table）；`core/reflect`（`mq_reflect_v1`、`mq_visitor`、`mq_value`）——均经参数注入，非硬链接。
  - `mq_ecs_world_v1`（bake 目标，注入）。
- **禁止**：
  - 依赖渲染/控制平面等上层；scene **不认识**渲染存储细节（docs/12 §8）。
  - 热路径过虚函数：逐实体迭代发生在**模块内部**；`.so` 边界只放粗粒度（create/instantiate/bake/step）。
  - 跨边界异常/RTTI/STL/裸指针（docs/02 R1–R6）。
  - 可变全局 / 单例 / 函数内 `static`（ADR-0014）。
  - **继承节点基类**（ADR-0019）：节点一律 `反射记录 + 组合`。
  - 节点树承载高基数（硬边界，docs/12 §4）；高基数一律 bake 到 ECS。
- **与其它模块的接缝**：
  - `reflect`（06）：节点/组件即反射类型；属性面板/绑定/dump 全自动。
  - `persistence`（08）：`snapshot/serialize` 用 `mq_visitor`；稳定 id/路径表示引用。
  - `script`（14）：经 `register_lifecycle` 注册回调，绑定由反射生成。
  - `renderer`（13）：消费 bake 后 ECS 绘制描述；`MQ_NODE_FLAG_LAYER` 节点提供分层/相机挂点。
  - `animation/physics/particle`（15）：消费 ECS 实体与组件；`physics_process` 相位与之对齐。
  - `control plane`（09/11）：dump/编辑任意子树（孤立子树即调试单元）。

## 4. 测试计划（从接口推导，先行）

> 单测**独立**：仅依赖 `mq_scene.h`/`mq_ecs.h` + **fake reflect** + **fake storage（fake ECS）** + 记账 fake allocator；
> **不链接**任何其它模块实现。

### 4.1 单元测试用例清单

**A. 节点树结构（`test_node_tree.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| A1 | 建根 | create_node(invalid, T, "Root") | 有效 ref，parent=invalid，child_count=0 | 正常 |
| A2 | 加子 | 连续向 Root 加 3 子 | 声明序 == 插入序；child_count=3 | 正常 |
| A3 | 取描述 | get_node(child) | type_id/name/parent 正确 | 正常 |
| A4 | 重挂 | move_node(c0, c1, 0) | c0 成为 c1 首子；原父 child_count-1 | 正常 |
| A5 | 自环拒绝 | move_node(n, n, 0) | `MQ_ERR_NODE_HAS_PARENT` | 错误 |
| A6 | 祖先环拒绝 | move_node(root, descendant, 0) | `MQ_ERR_NODE_HAS_PARENT` | 错误 |
| A7 | 深序销毁 | destroy_node(root) | 全校销毁；每 ref lookup → NOT_FOUND | 正常 |
| A8 | 悬垂句柄 | 销毁后 get_node(旧 ref) | `MQ_ERR_NOT_FOUND`（generation 不符） | 边界 |
| A9 | 无效父 | create_node({999,0,0}, T) | `MQ_ERR_NOT_FOUND` | 错误 |
| A10 | 分配者释放 | 全程记账 allocator，create/destroy 交替 | alloc==free，无泄漏 | 所有权 |

**B. 组件挂载（`test_components.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| B1 | 挂载 | attach_component(n, Transform) | 返回有效句柄，get 命中 | 正常 |
| B2 | 重复挂载 | 二次 attach 同类型 | `MQ_ERR_COMPONENT_EXISTS` | 错误 |
| B3 | 卸载 | detach_component | get → MISSING；句柄 lookup → NOT_FOUND | 正常 |
| B4 | 多组件序 | attach 3 种组件后 iterate_components | 按**挂载声明序** | 确定性 |
| B5 | 反射读写 | component_write("position.x",5) → read | 值 == 5；其余保持默认 | 正常 |
| B6 | size 不符 | add 组件 POD 与描述符 size 不符 | `MQ_ERR_TYPE_MISMATCH` | 错误 |
| B7 | 跨节点独立 | 同类型组件挂两个节点 | 负载互不影响 | 正常 |
| B8 | 销毁清理 | destroy_node 含 2 组件 | 组件句柄全失效 | 边界 |

**C. 遍历顺序（`test_traversal.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| C1 | 前序 | 树 R(A(a1,a2),B)，iterate_subtree(PRE) | 序列 == R,A,a1,a2,B | 确定性 |
| C2 | 后序 | 同上，POST | 序列 == a1,a2,A,B,R | 确定性 |
| C3 | 兄弟声明序 | 兄弟按插入序 a1,a2,a3 | 恒为 a1,a2,a3（非地址序） | 确定性 |
| C4 | 中断传播 | 回调在某节点返回错误 | iterate 立即返回该错误，后续无回调 | 错误 |
| C5 | find_by_path | "/A/a2" | 命中 a2 | 正常 |
| C6 | 路径语法错 | "A//a2"、"/@zz" | `MQ_ERR_PATH_SYNTAX` | 错误 |
| C7 | find_by_id | 已知 id | 命中；未知 id → NOT_FOUND | 边界 |
| C8 | 迭代次序稳定 | 同树两次 iterate | 回调序列逐项一致 | 确定性 |

**D. 实例化 + 引用重绑（`test_instantiate.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| D1 | 基本拷贝 | 实例化 3 层 prefab | 结构、type_id、属性值一致；id 全部新生成 | 正常 |
| D2 | id 重生成 | 对比源/实例 id 集 | 交集为空；实例内 id 唯一 | 正常 |
| D3 | 内部引用重绑 | 源中 node_ref 指向子树内节点 | 实例中指向**本实例对应节点**（非源） | 正常 |
| D4 | 外部引用保持 | 源中 entity_ref 指向外部实体 | 实例中句柄**原样保留**（走句柄） | 正常 |
| D5 | 嵌套实例化 | A 内含实例化的 B，再把 A 实例化 | 两层重绑都正确，id 无冲突 | 正常 |
| D6 | 重绑遗漏即错 | 源中内部引用指向源内但重绑表无映射 | `MQ_ERR_NOT_FOUND`（不得静默保留） | 错误 |
| D7 | 多实例独立 | 同 prefab 实例化两次 | 两实例属性互不影响（深拷贝） | 正常 |
| D8 | 实例根标记 | 实例化返回根 | flags 含 `INSTANCE_ROOT` | 正常 |
| D9 | 实例化到子节点 | parent=A/a1 | a1 的 children 追加实例根 | 正常 |

**E. 循环实例化检测（`test_cycle.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| E1 | 直接自含 | scene A 的 prefab 指向 A 自身 | `MQ_ERR_CYCLIC_INSTANCE` | 错误 |
| E2 | 间接环 | A→B→A | 加载/实例化期 `MQ_ERR_CYCLIC_INSTANCE` | 错误 |
| E3 | 深度超限 | 链式嵌套 > `MQ_SCENE_MAX_INSTANCE_DEPTH` | `MQ_ERR_CYCLIC_INSTANCE` | 边界 |
| E4 | 合法嵌套 | A→B→C（无环） | 成功，深度=3 | 正常 |
| E5 | 检测报错点 | 记录触发的 source_scene 链 | 错误信息含环上 scene id（供 A2/诊断） | 正常 |
| E6 | 失败原子性 | E1 失败后 | 目标树无半成品节点（回滚） | 边界 |

**F. 生命周期顺序（`test_lifecycle.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| F1 | enter 顺序 | 树 R(A,B)，注册回调记录 | `enter: R,A,B`（前序） | 确定性 |
| F2 | ready 顺序 | enter_tree 后 | `ready: R,A,B`（前序，全部 enter 完） | 确定性 |
| F3 | process 顺序 | step(PROCESS,dt) | 前序；每帧记录一致 | 确定性 |
| F4 | physics 独立 | step(PHYSICS,dt) | 只调 physics 回调，不调 process | 正常 |
| F5 | exit 顺序 | exit_tree(root) | `exit: A,B,R`（后序） | 确定性 |
| F6 | destroy 顺序 | destroy_node(root) | `destroy: A,B,R`（后序） | 确定性 |
| F7 | 组件声明序 | 单节点 2 个带回调组件 | 回调按**挂载声明序** | 确定性 |
| F8 | 未注册类型 | 无回调类型的节点 | 该节点无回调，不报错 | 边界 |
| F9 | 回调错误短路 | process 中某节点 return ERROR | step 立即返回该错误，后续节点不调 | 错误 |
| F10 | 重入 enter | 对已在树节点再 enter_tree | `MQ_ERR_INVALID_ARG`（不重复 enter） | 错误 |

**G. bake 语义一致性（`test_bake.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| G1 | 节点→实体 | bake 3 层树 | entity_count == 节点数（按 opts）；映射表双向可用 | 正常 |
| G2 | 属性一致 | 节点属性写 5.0 → bake → fake ECS 读组件 | 组件值 == 5.0（**属性一致**） | 正常 |
| G3 | 层级语义 | `MQ_BAKE_HIERARCHY` | ECS 中 Parent 组件 == 树层级 | 正常 |
| G4 | ref 重绑 | 组件含 entity_ref 指向同批节点 | bake 后 entity_ref 指向本批对应实体 | 正常 |
| G5 | 过滤组件 | `filter_components=[Transform]` | 仅 Transform 进入 ECS | 正常 |
| G6 | 高基数拒绝 | 节点数 > threshold | `MQ_ERR_NODE_CAP_EXCEEDED`（硬边界） | 错误 |
| G7 | 单向性 | bake 后改 ECS 组件再读节点树 | 节点树**不变**（无回写） | 正常 |
| G8 | 重复 bake | 同树 bake 两次 | 两次 checksum 相同；映射表刷新 | 确定性 |
| G9 | 属性默认 | 未写字段的节点 bake | 组件为该 schema 默认值 | 边界 |
| G10 | 缺 ECS | ecs=NULL 调 bake | `MQ_ERR_BAKE_UNSUPPORTED` | 错误 |

**H. 快照 / 序列化（`test_snapshot.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| H1 | 快照往返 | snapshot → restore 到新父 | 树结构/属性/组件逐项一致 | 正常 |
| H2 | 稳定 id | 往返后 id | 保留稳定 id（非槽号） | 正常 |
| H3 | 引用往返 | 含内部 node_ref + 外部 entity_ref | 内部重绑、外部句柄原样 | 正常 |
| H4 | 组件悬挂 | snapshot 后首次 serialize | 所有组件随节点写出（无遗漏） | 正常 |
| H5 | 默认省略 | 序列化经 fake codec | 等于 schema 默认的字段不写出（docs/08 §4） | 正常 |
| H6 | 分配者释放 | snapshot.data 经 allocator 分配 | restore 后经同 allocator 释放，记账平衡 | 所有权 |
| H7 | checksum 稳定 | snapshot 两次 | 字节一致 | 确定性 |
| H8 | 空树 | snapshot 叶子 | 合法，restore 得单节点 | 边界 |

**I. 错误 / 版本 / 内存（`test_errors.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| I1 | ABI 版本 | abi_version=999 的 vtable | create → `MQ_ERR_ABI_MISMATCH` | 版本兼容 |
| I2 | struct_size 截断 | 旧（更小）struct_size 调用 | 旧函数可用，不越界读 | 版本兼容 |
| I3 | 错误串 | `mq_result_str` 全枚举 | 均有非空静态串 | 正常 |
| I4 | NULL 参数 | create(NULL,...) / bake(NULL) | `MQ_ERR_INVALID_ARG`，不崩溃 | 错误 |
| I5 | 无裸 new | 构建期符号扫描 | scene `.so` 无 `operator new`/`malloc` 未解析符号 | 所有权 |
| I6 | 零全局态 | 两实例并行 | 互不影响（多实例 ADR-0014） | 边界 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S1 | **游戏根树** | 建 `World` 根，挂 3 个 `Layer` 子（2D `Gameplay` / 3D `World3D` / `UI`），各带相机挂点组件 | 前序 `World,Gameplay,World3D,UI`；各层独立子树可独立调试 |
| S2 | **玩家 + 相机** | `Player` 节点挂 Transform+Sprite；`Camera` 节点作为 Gameplay 子，`target` 引 Player | 相机 entity_ref 指向 Player 节点 id；实例化后仍指向本实例 Player |
| S3 | **NPC 对话引用** | `NPC` 挂 Dialogue 组件，字段 `listener` 引 Player 节点 | 内部引用重绑正确；跨层引用走句柄 |
| S4 | **UI 独立层** | UI 子树挂 `HUD`，含血条 `HpBar` 引 Player.hp 路径 | 路径解析命中；读 Player.hp == 100 |
| S5 | **实例化敌人 prefab** | 加载 `Enemy.scn`（含 Enemy+AI+Collider），对 3 个生成点各 instantiate | 3 只敌人结构一致、id 互异；AI 的 `target` 内部引用指向各自自身 |
| S6 | **敌人 AI 决策** | 敌人 AI 组件 `target` 指 Player → bake → 物理/动画读 ECS | bake 后 AI 的 entity_ref 指向 Player 实体；属性一致 |
| S7 | **玩家死亡** | destroy_node(Player) | 后序 destroy 回调；NPC/相机中悬挂 entity_ref 经世代检测失效（弱引用，docs/07 §6） |
| S8 | **进入关卡** | enter_tree(World) → ready → step(PROCESS) | 全树回调顺序确定；与回放重跑一致 |
| S9 | **子弹高基数** | 生成 5000 子弹 → 超过节点阈值 | bake 到 ECS；节点树不承载（`MQ_ERR_NODE_CAP_EXCEEDED` 提示改走 ECS） |
| S10 | **关卡快照** | snapshot(World) → 修改 → restore | 恢复后结构与属性逐项一致，可作 AI undo（A5） |

### 4.3 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| X1 | 遍历确定 | 同树 iterate_subtree 的 PRE/POST 序列跨运行逐项一致，不随分配地址变化 |
| X2 | 生命周期确定 | enter/ready/process/exit/destroy 序列跨运行一致 |
| X3 | 实例化确定 | 同 prefab/同操作序 → 新 id 分配序与结构一致；重绑结果一致 |
| X4 | bake 确定 | 同输入 → `bake_result.checksum` 一致；ECS 实体生成序一致 |
| X5 | 快照确定 | 同树 snapshot 两次字节一致；不含时间戳/地址 |
| X6 | 回放对齐 | 在固定 dt + 输入下重跑 scene 序列，节点树 dump checksum 与首跑一致（docs/10 §5） |

### 4.4 测试替身

- `fake_reflect`：手写 `mq_type_desc`（Transform/Enemy/Transform2D/UI 等）实现最小 `mq_reflect_v1`
  （`find_type`、`read_value`/`write_value`、`visit`、`construct/destroy`），不引入 `core/reflect` 实现。
- `fake_ecs`：实现 `mq_ecs_world_v1`，用数组 + 句柄记录实体/组件，记录调用序以验证 G1–G10；**故意慢**以便断言顺序。
- `fake_allocator`：记账（bytes/次数/峰值），可固定基址以验证无地址依赖。
- `fake_codec`：实现 `mq_visitor` 的 JSON / mem 两个 codec，验证 H5/H7。
- `fake_script_lifecycle`：实现 `mq_lifecycle_v1`，把回调追加到日志数组，验证 F1–F9。

### 4.5 独立单测保障

- CMake 独立 target `scene_tests`，`target_link_libraries` 仅含 `maque::scene`（或直接源码）、`maque::core-handles`（头）、gtest，
  **不含** `core-reflect`/`persistence`/`renderer` 实现。
- CI 断言：`scene_tests` 无对其它模块实现的链接；`-fvisibility=hidden` + 只导出 `mq_module_query`。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 — 冻结接口**：定稿 `mq_scene.h`、`mq_ecs.h`、`mq_result` 表、`find_by_path` 语法、
   生命周期顺序规则、bake 选项语义、循环检测规则。**产出即冻结**。
2. **阶段 1 — 测试骨架（红）**：写 `test_node_tree/test_components/test_traversal/test_instantiate/test_cycle/
   test_lifecycle/test_bake/test_snapshot/test_errors` 用例表与 fake 替身；全部编译失败/断言失败（红）。
3. **阶段 2 — 最小实现（绿）**：
   - 节点池（slot table）+ create/destroy/move/get；
   - 组件存储（per-type POD blob，句柄挂载）+ attach/detach/读写；
   - 遍历（深序 + 声明序）、find_by_id/path；
   - 生命周期注册与 enter/ready/step/exit/destroy 调度。
   逐组 A–F 转绿。
4. **阶段 3 — 实例化 / bake / 快照**：D（重绑 + id 重生成）、E（循环检测）、G（bake 一致性 + 高基数边界）、
   H（快照往返）、I（错误/版本/所有权）。逐组转绿。
5. **阶段 4 — 集成与预算**：对接 `fake_reflect`/`fake_ecs` 场景驱动（S1–S10）；确定性 X1–X6；
   性能/内存达标（§6）；clang-tidy 清零。

**里程碑**：M1 接口冻结 → M2 节点树+组件+遍历 绿 → M3 生命周期 绿 → M4 实例化+循环+bake 绿 →
M5 快照+确定性+预算 绿。

## 6. 验收标准（DoD）

- 接口头 `mq_scene.h`/`mq_ecs.h` 冻结，被 `script`（生命周期注册）、`physics/animation/particle`（ECS 消费）、
  `renderer`（绘制描述）fake 依赖编译通过。
- 单测覆盖率 **≥ 85%**（公共 API 行/分支）；§4 用例全绿。
- **确定性**：X1–X6 通过；`bake_result.checksum` 与 snapshot 字节跨运行一致；无地址依赖。
- **生命周期**：F1–F10 通过；脚本绑定零自调度（仅引擎调度）。
- **bake 一致性**：G1–G10 通过；节点树 → ECS 后关键属性逐项一致；无 ECS→节点树回写。
- **内存**：所有 allocator 用例零泄漏（alloc==free）；无裸 `new`/`malloc`；无全局可变状态（clang-tidy）。
- **性能预算**（参考值，冷路径 cm）：
  - `create_node`/`attach_component` ≤ ~500ns；
  - `iterate_subtree` 每节点 ≤ ~200ns；
  - `instantiate` 每节点 ≤ ~2µs（含重绑）；
  - `bake` 每节点 ≤ ~1µs（写 fake/真实 ECS 前）；
  - 节点池 `get_node` O(1)，`find_by_path` O(depth)。
- 无跨模块实现依赖；节点树高基数被硬边界挡在模块外。

## 7. 风险

| 风险 | 处理 |
|------|------|
| 节点树高基数性能 | 硬边界 + `high_cardinality_threshold` 拒绝 + 引导 bake 到 ECS（G6/S9，docs/12 §4） |
| 引用重绑遗漏 | 实例化统一走重绑表，未命中即 `MQ_ERR_NOT_FOUND`（D3/D6）；外部引用走句柄 |
| 循环实例化 | 实例化栈记录 source_scene 链 + 深度上限，加载期报错并回滚（E1–E6） |
| OOP 习惯回潮（继承节点） | 组件组合 + 评审禁止继承节点基类；接口层无节点基类概念（ADR-0019） |
| 双视图语义漂移 | 单一编辑期真相（节点树）+ 单向 bake；G7 单向性用例守住 |
| 生命周期顺序漂移 | §2.3 冻结顺序 + F1–F9 用例；脚本只能注册，不自调度 |
| 悬垂引用/句柄 | `{index,generation}` 世代校验；`entity_ref` 默认弱引用（S7，docs/07 §6） |
| prefab override 复杂度 | 一期只做实例化 + 重绑；`replace_by_prefab` 预留、override 二期（docs/12 §7） |
| 快照确定性被破坏 | 无时间戳/地址；按稳定 id 排序；X5/H7 用例守住（docs/10） |
| 边界无内联影响性能 | 边界粗粒度（instantiate/bake/step），逐实体迭代留模块内（docs/02 §7） |

## 8. Owner agent

- **角色标识**：`agent-scene`
- **职责边界**：仅负责 `modules/world/scene`（`mq_scene`）与 `modules/world/ecs`（`mq_ecs`）的接口、测试与实现。
  **不修改**其它模块；与 `agent-core`（reflect/handles 注入）、`agent-persistence`（visitor codec）、
  `agent-script`（生命周期注册）、`agent-physics/animation/particle`（ECS 消费）、`agent-renderer`（分层/相机挂点）
  通过本计划 §2 的冻结接口对接。
