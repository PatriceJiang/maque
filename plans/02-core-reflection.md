# 02 · core（data / 句柄 / 反射）

> Owner agent：`agent-core`　上游：`docs/06`（主）、`docs/07`、`docs/02`、`docs/00`。
> 铁律：**接口先冻结，测试先行（红→绿→重构）**；接口不完备不得进入实现。

## 1. 模块概述

- **职责**：
  1. **句柄**：定义 `mq_handle{index,generation}` 与通用槽表（slot table），提供世代校验、稳定性迭代。
  2. **反射运行时（中枢）**：定义 `type_id`（全限定名稳定哈希）、类型/枚举/字段描述符（数据，可跨 `.so`）、
     类型注册表、按 `field_path` 的读写、`visit(data, visitor)`、构造/销毁/复制。
  3. **codegen 契约**：一次 `schema.yaml` → 三种产出（C++ POD 头 / C ABI 描述符 / JSON schema），扩展 `parser.js`。
- **所在层**：基础层（`modules/core/{data,handles,reflect}`）。依赖方向只能向下（不依赖任何上层模块）。
- **上游文档**：`docs/06-反射与代码生成.md`（主）、`docs/07-内存与数据模型.md`、`docs/02-模块系统-ABI与多实例.md`、
  `docs/00-设计哲学与约束.md`（A1 可自省、ADR-0001/0002/0013/0014）、`docs/08`（codec=visitor）、`docs/12`（组件=反射类型）。
- **一句话**：把「引擎里每一份有意义的数据」描述成可跨 `.so`、跨语言、可确定序列化的**数据化反射**，
  作为序列化、脚本、场景、编辑器、AI 的**共同语言**。

### 1.1 形态与分层

| 子模块 | 目录 | 形态 | 理由 |
|--------|------|------|------|
| `data` | `modules/core/data` | **INTERFACE 头**（已有） | 编译期 POD 契约，固定布局 + `static_assert(size)`（docs/02 §10） |
| `handles` | `modules/core/handles` | INTERFACE 头（`mq_handle.h`）+ 可选 header-only 内联实现 | 句柄是纯数据 + 无状态算法 |
| `reflect` | `modules/core/reflect` | 头（`mq_reflect.h` 纯 C）+ 运行库 `maque_reflect`（可静态链接或 `.so`） | 注册表是运行时行为，经 C ABI vtable 暴露 |

> `reflect` 的跨实例状态全在 instance context（ADR-0014），**零全局可变状态**。字段描述符是**数据**，
> 模块 `init` 时经 C ABI 注册进 registry（docs/02 §8）。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（docs/02）。接口名用稳定字符串 ID：`"maque.handles.v1"`、`"maque.reflect.v1"`。

### 2.1 公共头 `mq_handles.h`（纯 C）

```c
/* mq_handles.h */
#define MQ_HANDLES_ABI_VERSION 1
#define MQ_HANDLE_INVALID_INDEX 0xFFFFFFFFu

typedef struct mq_handle {
    uint32_t index;       /* 槽下标；无效 = 0xFFFFFFFF */
    uint32_t generation;  /* 释放即自增，悬垂可检测 */
} mq_handle;              /* 8 字节，POD，可序列化 */

static inline int mq_handle_is_valid(mq_handle h) { return h.index != MQ_HANDLE_INVALID_INDEX; }
static inline int mq_handle_eq(mq_handle a, mq_handle b) {
    return a.index == b.index && a.generation == b.generation;
}
static inline mq_handle mq_handle_invalid(void) {
    mq_handle h = { MQ_HANDLE_INVALID_INDEX, 0u }; return h;
}

typedef mq_result (*mq_slot_iter_fn)(void* user, mq_handle h, void* payload);

typedef struct mq_slot_table_v1 {
    uint32_t struct_size;   /* = sizeof(mq_slot_table_v1) */
    uint32_t abi_version;   /* 主版本不匹配即拒绝 */
    /* element_size 为每个负载字节数；payload 由本表拥有、归零 */
    mq_result (*init)(void* self, const mq_allocator* a, uint32_t element_size, uint32_t capacity);
    void      (*deinit)(void* self);
    mq_result (*acquire)(void* self, mq_handle* out_handle, void** out_payload);
    mq_result (*lookup)(void* self, mq_handle h, void** out_payload);   /* 悬垂/越界 → NOT_FOUND，不返回野指针 */
    mq_result (*release)(void* self, mq_handle h);                     /* generation++；重复释放报错 */
    uint32_t  (*capacity)(void* self);
    uint32_t  (*live_count)(void* self);
    /* 按 index 升序迭代（确定性；禁指针哈希/地址序） */
    mq_result (*iterate)(void* self, mq_slot_iter_fn fn, void* user);
} mq_slot_table_v1;
```

- **操作面语义**：
  - `acquire`：分配新槽（或复用空闲槽），`generation` 保持该槽历史代次，负载清零；失败返回 `MQ_ERR_NO_MEMORY`。
  - `lookup`：`h.generation != slot.generation` 或 `index >= capacity` → `MQ_ERR_NOT_FOUND`（杜绝悬垂读取）。
  - `release`：有效句柄 → `generation++`，槽入空闲表；对已失效句柄 → `MQ_ERR_STALE_HANDLE`。
  - `iterate`：严格按 `index` 升序；回调返回非 `MQ_OK` 时中断并透传该错误。
- **内存所有权**：槽表内存由传入 `mq_allocator` 分配，`deinit` 负责经**同一** allocator 释放（分配者释放）。
- **线程/确定性**：单线程使用；`iterate` 顺序仅取决于 index，**不依赖地址**。

### 2.2 公共头 `mq_reflect.h`（纯 C）——类型数据模型

```c
/* mq_reflect.h */
#define MQ_REFLECT_ABI_VERSION 1

typedef uint64_t mq_type_id;              /* FNV-1a 64(全限定名)，见 §2.5 */
typedef uint64_t mq_schema_hash;          /* 类型 shape 稳定哈希，见 §2.5 */

/* 字段类型种类 */
typedef enum mq_field_kind {
    MQ_FIELD_SCALAR       = 0,
    MQ_FIELD_ENUM         = 1,
    MQ_FIELD_STRUCT       = 2,
    MQ_FIELD_FIXED_ARRAY  = 3,   /* T[N] */
    MQ_FIELD_DYN_ARRAY    = 4,   /* T[]  */
    MQ_FIELD_MAP          = 5,   /* map<K,V> */
    MQ_FIELD_STRING       = 6,   /* UTF-8 */
    MQ_FIELD_ENTITY_REF   = 7,   /* mq_handle（弱引用） */
    MQ_FIELD_ASSET_REF    = 8,   /* mq_handle + 稳定 id（强引用） */
    MQ_FIELD_OPTIONAL     = 9    /* optional<T> */
} mq_field_kind;

typedef enum mq_scalar_kind {
    MQ_SCALAR_NONE = 0,
    MQ_SCALAR_I8, MQ_SCALAR_I16, MQ_SCALAR_I32, MQ_SCALAR_I64,
    MQ_SCALAR_U8, MQ_SCALAR_U16, MQ_SCALAR_U32, MQ_SCALAR_U64,
    MQ_SCALAR_F32, MQ_SCALAR_F64, MQ_SCALAR_BOOL, MQ_SCALAR_CHAR,
    MQ_SCALAR_HANDLE           /* 内联 mq_handle 字段 */
} mq_scalar_kind;

/* 类型标志位 */
#define MQ_TYPE_FLAG_POD        0x1u  /* 可 memcpy */
#define MQ_TYPE_FLAG_TRANSIENT  0x2u  /* 整体不参与序列化/visit */
#define MQ_TYPE_FLAG_OPAQUE     0x4u  /* 不透明：无字段描述（如外部句柄） */

typedef struct mq_attr { const char* key; const char* value; } mq_attr;   /* UTF-8；value 可空 */

typedef struct mq_field_desc {
    const char*       name;        /* UTF-8，字段名 */
    uint32_t          offset;      /* 相对结构体起始的字节偏移 */
    uint32_t          size;        /* 字段自身 sizeof（FIXED_ARRAY 为整块） */
    mq_field_kind     kind;
    mq_scalar_kind    scalar;      /* kind==SCALAR/HANDLE 时有效，否则 NONE */
    mq_type_id        type_id;     /* 元素/内嵌结构/键/枚举的类型 id；无则 0 */
    mq_type_id        type_id2;    /* MAP 的值类型 id；否则 0 */
    uint32_t          count;       /* FIXED_ARRAY 长度；其余 0 */
    uint32_t          flags;       /* 字段标志：TRANSIENT 等 */
    const mq_attr*    attrs;       /* range/unit/readonly/ref/deprecated… */
    uint32_t          attr_count;
} mq_field_desc;

typedef struct mq_enum_value { const char* name; int64_t value; } mq_enum_value;

typedef struct mq_enum_desc {
    uint32_t             struct_size;   /* = sizeof(mq_enum_desc) */
    uint32_t             abi_version;
    mq_type_id           id;
    const char*          name;          /* "EnemyState" */
    const char*          ns;            /* "world" */
    uint32_t             size;          /* 底层整型字节数 */
    const mq_enum_value* values;
    uint32_t             value_count;
} mq_enum_desc;

/* 生命周期钩子：由类型所属模块提供（数据；重载时由 registry 重新灌入） */
typedef void* (*mq_ctor_fn) (void* data, const mq_allocator* a);
typedef void  (*mq_dtor_fn) (void* data, const mq_allocator* a);
typedef void* (*mq_copy_fn) (void* dst, const void* src, const mq_allocator* a);

typedef struct mq_type_desc {
    uint32_t            struct_size;    /* = sizeof(mq_type_desc)，前向兼容 */
    uint32_t            abi_version;
    mq_type_id          id;
    const char*         name;           /* "Transform" */
    const char*         ns;             /* "world" */
    uint32_t            size;           /* sizeof(POD) */
    uint32_t            align;
    uint32_t            flags;          /* MQ_TYPE_FLAG_* */
    const mq_field_desc* fields;
    uint32_t            field_count;
    const mq_enum_desc*  enums;         /* 内嵌枚举（可选） */
    uint32_t            enum_count;
    mq_schema_hash      shape_hash;     /* codegen 预计算，见 §2.5 */
    mq_ctor_fn          construct;      /* 可空：POD 无动态成员时为空 */
    mq_dtor_fn          destroy;
    mq_copy_fn          copy;
} mq_type_desc;
```

### 2.3 公共头 `mq_reflect.h`——值类型（供 AI/编辑器/命令行读写）

```c
typedef enum mq_value_kind {
    MQ_VAL_NONE = 0,
    MQ_VAL_BOOL, MQ_VAL_I64, MQ_VAL_U64, MQ_VAL_F64,
    MQ_VAL_STRING, MQ_VAL_HANDLE, MQ_VAL_ENUM, MQ_VAL_LIST, MQ_VAL_STRUCT
} mq_value_kind;

typedef struct mq_value {
    mq_value_kind kind;
    mq_type_id    type;    /* ENUM/LIST/STRUCT 的元素类型 id */
    uint32_t      count;   /* LIST 元素数 */
    union {
        int      b;
        int64_t  i64;
        uint64_t u64;
        double   f64;
        struct { const char* ptr; uint32_t len; } str;  /* 不拥有：指向调用方缓冲 */
        mq_handle h;
        const struct mq_value* items;                   /* LIST：调用方拥有 */
    } as;
} mq_value;
```

### 2.4 公共头 `mq_reflect.h`——错误模型与 visitor

```c
typedef enum mq_result {
    MQ_OK = 0,
    MQ_ERR_INVALID_ARG,       /* 空指针 / 尺寸不符 */
    MQ_ERR_NOT_FOUND,         /* 类型或字段不存在 / 悬垂句柄 */
    MQ_ERR_DUPLICATE_TYPE,    /* 重复注册同一 type_id */
    MQ_ERR_TYPE_MISMATCH,     /* 值类型与字段不符 */
    MQ_ERR_ABI_MISMATCH,      /* 主版本不匹配 */
    MQ_ERR_PATH_SYNTAX,       /* field_path 语法错误 */
    MQ_ERR_OUT_OF_RANGE,      /* 数组下标越界 / range 校验失败 */
    MQ_ERR_NO_MEMORY,
    MQ_ERR_UNSUPPORTED,       /* 该 kind 不支持此操作（如未注册子类型） */
    MQ_ERR_TRANSIENT_FIELD,   /* 试图读写 transient 字段（visit 默认跳过） */
    MQ_ERR_STALE_HANDLE,
    MQ_ERR_NOT_POD
} mq_result;

/* 可读错误名（跨 .so 安全：返回静态字符串） */
const char* mq_result_str(mq_result r);

/* visitor：序列化 codec / dump / 校验都是 visitor；读写双向 */
typedef enum mq_visit_dir { MQ_VISIT_READ = 0, MQ_VISIT_WRITE = 1 } mq_visit_dir;

typedef struct mq_visitor {
    uint32_t      struct_size;   /* = sizeof(mq_visitor) */
    mq_visit_dir  dir;
    void*         user;
    /* 结构进出 */
    mq_result (*on_struct_begin)(void* u, const mq_type_desc* t, void* data);
    mq_result (*on_struct_end)  (void* u, const mq_type_desc* t);
    mq_result (*on_field_begin) (void* u, const mq_field_desc* f, const char* path);
    mq_result (*on_field_end)   (void* u, const mq_field_desc* f);
    /* 叶子：value 指向 data 内该字段的可写内存（READ 视图中视为只读） */
    mq_result (*on_scalar)      (void* u, const mq_field_desc* f, void* value);
    mq_result (*on_string)      (void* u, const mq_field_desc* f, char** str, uint32_t* len);
    mq_result (*on_enum)        (void* u, const mq_field_desc* f, const mq_enum_desc* e, void* value);
    mq_result (*on_ref)         (void* u, const mq_field_desc* f, mq_handle* h);
    /* 容器 */
    mq_result (*on_array_begin) (void* u, const mq_field_desc* f, uint32_t* count);
    mq_result (*on_array_end)   (void* u, const mq_field_desc* f);
} mq_visitor;
```

> `visit` 语义：遍历时**跳过** `MQ_TYPE_FLAG_TRANSIENT` 类型与 `TRANSIENT` 字段；
> 字段顺序 = 描述符声明顺序（稳定）；容器按 index 升序。visitor 回调返回非 `MQ_OK` 立即中断并透传。

### 2.5 `type_id` 与 `shape_hash` 规格（稳定哈希，禁序号）

- **`type_id = FNV-1a64(UTF-8("ns::Name"))`**：offset basis `0xcbf29ce484222325`，prime `0x100000001b3`。
  - 运行时 `mq_type_id_of(const char* fqname)` 与 codegen 必须**逐字节一致**。
  - 测试向量（冻结）：`"math::Vec3"` → `0x...`、`"world::Transform"` → `0x...`、`"asset::Texture"` → `0x...`
    （向量值在阶段 0 由实现与 codegen 同时生成、写入测试基线，二者不一致即红）。
- **`shape_hash`**：对 `(size, align, flags, 字段序列(name,offset,kind,scalar,type_id,count))` 做 FNV-1a64，
  用于「同 id 不同 shape」检测（加字段=hash 变，旧数据反序列化按名匹配 + 迁移，docs/06 §7）。
- **禁止**任何基于序号/地址/指针的类型标识或排序。

### 2.6 Registry / 操作面 vtable `mq_reflect_v1`

```c
typedef mq_result (*mq_type_iter_fn)(void* user, const mq_type_desc* t);

typedef struct mq_reflect_v1 {
    uint32_t struct_size;     /* = sizeof(mq_reflect_v1) */
    uint32_t abi_version;     /* 主版本不匹配即拒绝 */

    /* —— 注册（仅 init 期、单线程；描述符深拷贝进 registry 的 allocator）—— */
    mq_result (*register_type) (void* self, const mq_type_desc* t);
    mq_result (*register_enum) (void* self, const mq_enum_desc* e);
    mq_result (*unregister_module)(void* self, const char* module_iface_name);

    /* —— 查询 —— */
    const mq_type_desc* (*find_type)        (void* self, mq_type_id id);
    const mq_type_desc* (*find_type_by_name)(void* self, const char* fqname);
    const mq_enum_desc* (*find_enum)        (void* self, mq_type_id id);
    uint32_t            (*type_count)       (void* self);
    mq_schema_hash      (*shape_hash)       (void* self, mq_type_id id);
    /* 迭代：按 type_id 升序（确定性） */
    mq_result (*iterate_types)(void* self, mq_type_iter_fn fn, void* user);

    /* —— 构造 / 销毁 / 复制（分配者释放，经传入 allocator）—— */
    mq_result (*construct)(void* self, mq_type_id t, void* out, const mq_allocator* a);
    mq_result (*destroy)  (void* self, mq_type_id t, void* data, const mq_allocator* a);
    mq_result (*copy)     (void* self, mq_type_id t, void* dst, const void* src, const mq_allocator* a);

    /* —— 按 field_path 读写（值视图；AI/编辑器/命令行）—— */
    mq_result (*read_value) (void* self, const mq_type_desc* t, const void* data,
                             const char* field_path, mq_value* out);
    mq_result (*write_value)(void* self, const mq_type_desc* t, void* data,
                             const char* field_path, const mq_value* in);

    /* —— 遍历（codec/dump = visitor）—— */
    mq_result (*visit)(void* self, mq_type_id t, void* data, const mq_visitor* v);

    /* —— 反射层句柄便捷（委托 mq_slot_table，句柄与负载配对）—— */
    mq_result (*handle_lookup)(void* self, mq_handle h, mq_type_id t, void** out_data);
} mq_reflect_v1;

/* 模块查询入口（每个 .so 导出唯一入口，docs/02 §3） */
void* mq_module_query(const char* interface_name, uint32_t min_version);
/* interface_name == "maque.reflect.v1" → 返回 const mq_reflect_v1* */
```

- **`field_path` 语法（冻结）**：
  ```
  path    := segment ( '.' segment | '[' index ']' )*
  segment := identifier            ; ASCII [A-Za-z_][A-Za-z0-9_]*
  index   := decimal u32
  例： "position.x"、"path[3].x"、"uvs[0]"、"state"
  ```
  语法错误 → `MQ_ERR_PATH_SYNTAX`；非法下标 → `MQ_ERR_OUT_OF_RANGE`；不存在字段 → `MQ_ERR_NOT_FOUND`；
  transient 字段 → `MQ_ERR_TRANSIENT_FIELD`（写操作强制报错；读操作默认拒绝，`visit` 路径静默跳过）。
- **`read_value`/`write_value` 所有权**：`mq_value` 的 `str.ptr`/`as.items` **不拥有**——指向 `data` 内部或调用方缓冲，
  调用方保证缓冲存活期内有效。`write_value` 对 `DYN_ARRAY`/`MAP`/`STRING` 的扩容经类型 `construct/destroy/copy` 钩子，
  使用注册时携带的 allocator（若类型无钩子且含动态成员 → `MQ_ERR_NOT_POD`）。

### 2.7 版本化 / 前向兼容规则（docs/02 R7）

1. **`struct_size` 只增不改**：新版本在结构/表尾追加成员；旧调用方按自身 `struct_size` **截断使用**。
2. **`abi_version` 主版本不匹配即拒绝**，返回 `MQ_ERR_ABI_MISMATCH` + 可读字符串（服务 A2）。
3. **接口名字符串稳定**：`maque.handles.v1`、`maque.reflect.v1`；`mq_module_query` 以 `min_version` 协商。
4. **描述符数据兼容**：`type_id` 由名字稳定哈希保证不漂移；`shape_hash` 变化 = 需迁移（docs/06 §7）。

### 2.8 内存所有权总则（docs/02 R5 / docs/07）

- **分配者释放**：任何经 `mq_allocator` 分配的内存，由**同一** allocator 释放；跨 `.so` 绝不 `free` 他方内存。
- **registry 深拷贝**：`register_type/register_enum` 将描述符（含 `name`/`ns`/`fields`/`attrs` 字符串）深拷贝进
  registry 自己的 allocator；因此模块描述符可以是 `static const`，也**不依赖模块内存存活**，热重载安全。
- **不可变读**：`init` 完成后 registry 只读；`find_type` 返回的指针由 registry 拥有，生命周期到 `unregister/deinit`。

### 2.9 codegen 契约（一次 schema → 三种产出）

**输入**：`modules/core/data/schema.yaml`（在现有 `namespace/class/enum/fields` 上扩展
`struct`、`attrs`、引用类型 `entity_ref`/`asset_ref<T>`、动态容器 `T[]`/`map<K,V>`/`string`/`optional<T>`）。

**CLI（冻结）**：
```
node modules/core/data/parser.js \
    --schema schema.yaml \
    --out-cpp  <dir>      # gen/<ns>.h：C++ POD 头（static_assert(sizeof==size) + default）
    --out-abi  <dir>      # gen/<schema>_reflect.h/.cpp：C ABI 描述符 + 注册函数
    --out-json <dir>      # gen/<schema>.schema.json：机器可读 schema（AI/校验）
    [--check]             # 仅校验，输出与现有产物一致则 exit 0
```
> 三种产出由**同一** parser，保证 C++ 布局、运行时描述、对外文档**永不漂移**（docs/06 §4）。
> 输出**按名字排序**（确定性，可 diff），并**内联 `shape_hash` 常量**。

**C ABI 产出**必须提供：
```c
extern "C" void mq_register_generated_types(const mq_reflect_v1* r);  /* 幂等；重复注册返回 DUPLICATE 时可忽略 */
```

| 产出 | 文件（示例） | 关键内容 |
|------|--------------|----------|
| C++ POD 头 | `gen/math.h`、`gen/world.h` | `namespace world { struct Transform {...}; static_assert(sizeof(Transform)==48); }` |
| C ABI 描述符 | `gen/reflect_all.h/.cpp` | `static const mq_field_desc world_Transform_fields[] = {...};` + `mq_type_id_of` 预计算常量 + 注册函数 |
| JSON schema | `gen/schema.json` | 每个类型的 `id/name/size/fields[{name,offset,kind,type,attrs}]` |

## 3. 依赖与边界

- **可依赖**：`mq_allocator`（来自 ABI 头，参数注入，不是链接依赖）。无其它模块依赖；不链接 `math` 实现（描述符里 `Vec3` 只按 `type_id` 引用）。
- **禁止**：
  - 上层 `#include`（scene/renderer/…）；反射**不引用**任何上层模块。
  - 热路径过虚函数：`reflect_*` 仅冷路径；热循环走生成 C++ 直接访问（docs/06 §5）。
  - 跨边界异常/RTTI/STL/裸指针（docs/02 R1–R6）；越过 `.so` 的 `std::string`/`vector`/`shared_ptr`。
  - 可变全局 / 单例 / 函数内 `static`（ADR-0014，多实例）。
  - 用序号/地址做类型标识或迭代排序。
- **与其它模块的接缝**：
  - `module host`（03）：反射 registry 由其作为 host 服务持有与注入；`mq_module_query` 注册入口。
  - `persistence`（08）：codec 是 `mq_visitor` 的实现；canonical IR = schema。
  - `scene`（12）：组件/节点 = `mq_type_desc`；按 `{type_id, field_path}` 读写。
  - `script`（14）：绑定由描述符**生成**，禁止手写。
  - `control plane`（10/11）：dump/读/写任意 path，喂 JSON schema。

## 4. 测试计划（从接口推导，先行）

> 单测**独立**：仅依赖 `core` 公共头 + 测试用 fake allocator / 手写描述符；上游用 fake。
> 跨 `.so` 用例编译两个最小测试 `.so`（`fake_domain_a.so`、`fake_domain_b.so`），各自注册类型。

### 4.1 单元测试用例清单

**A. 句柄 / 槽表（`test_handles.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| A1 | acquire 连续 | init(elem=16,cap=4)；acquire ×4 | 4 个 index=0..3、generation=0、payload 已清零 | 正常 |
| A2 | lookup 命中 | 对 A1 句柄 lookup | 非空 payload，指针稳定 | 正常 |
| A3 | 悬垂检测 | release(h0) 后 lookup(h0) | `MQ_ERR_NOT_FOUND`，不返回野指针 | 边界 |
| A4 | 世代自增 | release 后重新 acquire 同一槽 | 同 index、generation=旧+1 | 正常 |
| A5 | 重复释放 | release(h0) ×2 | 第二次 `MQ_ERR_STALE_HANDLE` | 错误 |
| A6 | 越界句柄 | lookup({999,0}) | `MQ_ERR_NOT_FOUND` | 错误 |
| A7 | 容量增长 | 超过 cap 后 acquire | 成功扩容（经 allocator），旧句柄仍有效 | 边界 |
| A8 | 迭代顺序 | acquire/release 交错后 iterate | 严格按 index 升序，回调次数=live_count | 确定性 |
| A9 | 分配者释放 | 用记账 fake allocator 全程 | alloc 次数 == free 次数，无泄漏 | 所有权 |
| A10 | 无效句柄常量 | `mq_handle_invalid()` | `!is_valid`，`eq` 自反 | 边界 |

**B. type_id / shape_hash（`test_type_id.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| B1 | 冻结向量 | `"math::Vec3"` 等 | 等于基线常量（实现与 codegen 一致） | 正常 |
| B2 | 稳定性 | 增删其它类型后再算 | 同名字 type_id **不变** | 确定性 |
| B3 | 无序号依赖 | 调换注册顺序 | `find_type(id)` 结果与顺序无关 | 确定性 |
| B4 | 碰撞探测 | 一批真实类型名 | 无重复 id；重复即测试失败 | 边界 |
| B5 | shape_hash 敏感 | 加/删字段、改 offset | shape_hash 变化 | 正常 |
| B6 | shape_hash 稳定 | 相同 shape 两次计算 | 一致 | 确定性 |

**C. 注册与查询（`test_registry.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| C1 | 注册+查找 | register_type(Transform) | find_type/find_type_by_name 命中，字段数正确 | 正常 |
| C2 | 重复注册 | 同 id 注册两次 | 第二次 `MQ_ERR_DUPLICATE_TYPE` | 错误 |
| C3 | 迭代有序 | 注册乱序的多类型 | iterate_types 按 type_id 升序 | 确定性 |
| C4 | 未注册查找 | find_type(0xDEAD) | 返回 NULL | 错误 |
| C5 | 深拷贝独立性 | 注册后覆写原描述符缓冲 | registry 内描述符不受影响 | 所有权 |
| C6 | 反注册模块 | unregister_module("fake.a") | 该模块类型全部消失，其它保留 | 正常 |
| C7 | ABI 版本 | abi_version=999 的 vtable | 调用入口返回 `MQ_ERR_ABI_MISMATCH` | 版本兼容 |
| C8 | struct_size 截断 | 以旧（更小）struct_size 的 vtable | 旧的函数仍可用，不越界读 | 版本兼容 |

**D. field_path 读写（`test_field_path.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| D1 | 标量读 | Transform，"position.x" | f32 == 值 | 正常 |
| D2 | 标量写 | 写 "position.y"=7 | 内存偏移处 = 7 | 正常 |
| D3 | 定长数组 | Mat4，"m[7]" | 下标 7 元素 | 正常 |
| D4 | 数组越界 | "m[16]" | `MQ_ERR_OUT_OF_RANGE` | 错误 |
| D5 | 语法错误 | "position..x"、".x"、"[0]" | `MQ_ERR_PATH_SYNTAX` | 错误 |
| D6 | 不存在字段 | "nope" | `MQ_ERR_NOT_FOUND` | 错误 |
| D7 | 类型不符写 | 向 f32 写 STRING | `MQ_ERR_TYPE_MISMATCH` | 错误 |
| D8 | 嵌套 struct | "position.x" 遍历子结构 | 正确解析 | 正常 |
| D9 | transient 字段 | 读 "Enemy._scratch" | `MQ_ERR_TRANSIENT_FIELD` | 错误 |
| D10 | range 属性写 | hp=200（range[0,100]） | `MQ_ERR_OUT_OF_RANGE` | 错误 |
| D11 | 动态数组 | Mesh.vertices[2].x | 越界/存在按 count 判定 | 边界 |
| D12 | 句柄字段 | 读 entity_ref | 返回 MQ_VAL_HANDLE，generation 保留 | 正常 |

**E. visit 往返（`test_visit.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| E1 | visit 计数 | Enemy 实例 | on_struct/field 回调按声明序、次数正确 | 正常 |
| E2 | 跳过 transient | Enemy（含 _scratch、transient 子类型） | 该字段无回调 | 正常 |
| E3 | 写 visitor 往返 | 用 READ 导出 mq_value 流，WRITE 回填新实例 | 逐字段相等（memcmp POD 部分） | 正常 |
| E4 | 中断传播 | 某回调返回 `MQ_ERR_OUT_OF_RANGE` | visit 立即返回该错误，后续无回调 | 错误 |
| E5 | 顺序稳定 | 同实例两次 visit | 回调序列完全一致 | 确定性 |
| E6 | 枚举回调 | EnemyState 字段 | on_enum 给 name="Chase", value=1 | 正常 |
| E7 | 引用回调 | entity_ref/asset_ref | on_ref 收到 mq_handle | 正常 |

**F. 构造/销毁/复制（`test_lifecycle.cpp`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| F1 | POD construct | Vec3 | 归零，返回 out | 正常 |
| F2 | 动态 construct | 含 `T[]` 的类型（fake 钩子） | 经 allocator 分配，dtor 释放 | 所有权 |
| F3 | copy 深拷贝 | 含动态成员实例 | 源改后目标不变；两 allocator 记账平衡 | 所有权 |
| F4 | NOT_POD | 含动态成员但无钩子 | construct 返回 `MQ_ERR_NOT_POD` | 错误 |

**G. 跨 `.so` 注册（`test_cross_so.cpp` + `fake_domain_a.so`/`fake_domain_b.so`）**

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| G1 | A 注册 B 查询 | A 注册 `fake::CompA`，宿主 find | 命中，字段/offset 正确 | 集成 |
| G2 | 双模块同 schema | A、B 各自注册同名类型 | 二者 type_id 相同；第二次 DUPLICATE | 正常 |
| G3 | 描述符数据跨界 | 宿主按描述符读写 A 的实例内存 | 偏移正确、无越界 | 集成 |
| G4 | 分配者释放 | A 分配的实例经宿主 destroy（A 的钩子） | A allocator 记账平衡 | 所有权 |
| G5 | 热重载 | dlclose A → dlopen A' 重新注册 | registry 类型刷新，旧指针不引用 | 边界 |
| G6 | 不缓存函数指针 | 重载后旧描述符指针置空 | 宿主不崩溃（约定验证） | 边界 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S1 | **玩家移动** | 构造 `world::Transform` → `write_value("position.x", 5.0f)` → `read_value("position.x")` | 读回 5.0f；`scale` 保持默认 `{1,1,1}` |
| S2 | **敌人受击** | read "hp"=100 → write "hp"=40 → 越界写 "hp"=200 | 40 生效；200 → `MQ_ERR_OUT_OF_RANGE`（range 属性） |
| S3 | **敌人状态机** | write "state" = EnemyState::Chase（枚举按名/值） | on_enum 得到 name/值；读回一致 |
| S4 | **目标引用** | write entity_ref "target" = 有效句柄 → 销毁目标 → lookup | 读回旧 generation；lookup → NOT_FOUND（弱引用） |
| S5 | **资源引用序列化** | `asset_ref<Texture>` "sprite" 经 READ visitor dump | 输出稳定 id/路径（非指针），可回填 |
| S6 | **场景 dump** | 对包含 Transform+Enemy 的节点 visit → JSON codec（fake） | JSON 字段按声明序；省略默认值；无 transient |
| S7 | **场景 load 往返** | 将 S6 JSON 经 WRITE visitor 回填新节点 | 与原节点 memcmp（POD 部分）相等；引用重绑 |
| S8 | **AI 路径编辑** | 命令行风格：对 `path[2].x` 读、改、dump | 精确到元素，其余不动 |
| S9 | **组件挂载** | 用 slot table 挂 Transform 句柄到节点 | acquire 得句柄，dump 该句柄负载正确 |

### 4.3 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| X1 | 迭代顺序 | `iterate_types` 只按 type_id 升序；同一组类型在任意注册顺序下结果字节一致 |
| X2 | 字段顺序 | visit/JSON 字段恒按描述符声明序，绝不按哈希序 |
| X3 | 无地址依赖 | 强制不同分配地址（多次运行）→ dump checksum 一致 |
| X4 | codegen 幂等 | 同一 schema 连跑两次 `--out-*` → 产物逐字节相同（含排序、无时间戳） |
| X5 | codegen ↔ 运行时一致 | codegen 预计算的 type_id/shape_hash == 运行时 `mq_type_id_of`/registry 计算值 |

### 4.4 测试替身

- `fake_allocator`：记账（bytes/次数/峰值），确定性地址可选（固定基址）以验证无地址依赖。
- `fake_module_a.so` / `fake_module_b.so`：仅含 `mq_module_query` + 注册函数 + 2~3 个手写描述符。
- `fake_codec`：实现 `mq_visitor` 的 JSON / mem 两个 codec，验证 E3/S6/S7。
- 上游 fake：`scene` 的节点/组件用最简 struct + 手写描述符，不引入 scene 实现。

### 4.5 独立单测保障

- 测试仅编译 `modules/core` 出头 + `libmaque_reflect` + fake 替身；CMake 用独立 `core_tests` target，**不链接**任何上层模块。
- CI 断言：`core_tests` 的 `target_link_libraries` 只含 `maque::core-data`、`maque::core-reflect`、gtest。
- `-fvisibility=hidden` + 只导出 `mq_module_query`，防止意外符号依赖。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 — 冻结接口**：定稿 `mq_handles.h`、`mq_reflect.h`、`mq_result` 表、`field_path` 语法、
   `type_id`/`shape_hash` 规格与测试向量；`mq_register_generated_types` 签名；CLI 参数。**产出即冻结**。
2. **阶段 1 — 测试骨架（红）**：写 `test_handles/test_type_id/test_registry/test_field_path/test_visit/test_lifecycle`
   全部用例表；跨 `.so` 的 fake 模块 target；`--check` 基线（含未生成产物的红）。
3. **阶段 2 — 最小实现（绿）**：
   - `handles`：slot table（acquire/lookup/release/iterate）。
   - `reflect`：FNV-1a `mq_type_id_of`、registry（register/find/iterate 有序）、`read_value`/`write_value`、`visit`、构造/复制（先 POD）。
   - `codegen`：parser 扩展属性/引用/容器；产出三种文件；内联 id/hash 常量。逐组用例转绿。
4. **阶段 3 — 边界/错误/确定性**：错误码全覆盖、ABI 截断与版本拒绝、range/transient、动态容器 + 钩子、
   X1–X5 确定性、跨 `.so` G1–G6。
5. **阶段 4 — 集成与预算**：对接 `module host` fake（registry 服务注入）、`persistence` fake codec 往返；
   性能/内存达标（见 §6）；clang-tidy 清零。

**里程碑**：M1 接口冻结 → M2 handlers+registry 绿 → M3 field_path+visit 绿 → M4 codegen 三产出+ `--check` 绿 → M5 跨 `.so`/确定性/预算 绿。

## 6. 验收标准（DoD）

- 接口头 `mq_handles.h`/`mq_reflect.h` 冻结，被 `module host` 与 `persistence` fake 依赖编译通过。
- 单测覆盖率 **≥ 85%**（核心公共 API 行/分支）；§4 用例全绿。
- **确定性**：X1–X5 通过；两次 codegen 产物字节一致；跨分配地址 dump checksum 一致。
- **跨 `.so`**：G1–G6 通过；`-fvisibility=hidden`；无跨边界异常/RTTI/STL。
- **内存**：所有 allocator 用例零泄漏，`alloc==free`；无裸 `new`/`malloc`；无全局可变状态（clang-tidy 规则）。
- **性能预算**：`read_value`/`write_value` 简单标量路径 ≤ ~100ns；`visit` 单字段 ≤ ~200ns（冷路径，参考值）；
  registry `find_type` O(1)（id 哈希）或 O(log n)（有序）。
- 无跨模块实现依赖；`type_id` 无序号/地址依赖。

## 7. 风险

| 风险 | 处理 |
|------|------|
| `type_id` 哈希碰撞 | 用 FNV-1a64；CI 对全量已注册类型名做碰撞探测（B4）；碰撞即构建失败 |
| 运行时与 codegen 哈希不一致 | X5 一致性用例统一基线；codegen 从同一常量表产出 |
| 描述符跨 `.so`/热重载悬垂 | registry **深拷贝**描述符；约定不缓存函数指针跨重载（G5/G6，docs/02 §6） |
| 反射元数据膨胀（嵌入式） | 编译期裁剪开关：P0 只留 name/offset/kind，属性和 JSON schema 可关（docs/06 §9） |
| 动态容器跨边界 `free` | 强制经类型钩子 + 注入 allocator；无钩子含动态成员返回 `MQ_ERR_NOT_POD`（F2/F4） |
| 热路径误用反射 | API 前缀 `reflect_*`/`mq_*`；评审规则；生成 C++ 走直接访问 |
| `field_path` 语法漂移 | §2.6 冻结语法 + D5 用例；语法变更须改版本 |
| ABI 结构被误扩展 | `struct_size` 只增不改；C8 截断用例守住 |

## 8. Owner agent

- **角色标识**：`agent-core`
- **职责边界**：仅负责 `modules/core/{data,handles,reflect}` 的接口、测试与实现，以及
  `modules/core/data/parser.js` 的 codegen 扩展。**不修改**其它模块；与 `agent-host`（registry 服务注入）、
  `agent-persistence`（visitor codec）、`agent-scene`（组件类型定义）通过本计划 §2 的冻结接口对接。
