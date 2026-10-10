# 07 · persistence（序列化中枢：codec × backend）

> 主参考：`docs/08-资源持久化与资产管线.md`（序列化中枢、双形态一模型、文本 diff/AI 友好规范）；
> 上游：`docs/06-反射与代码生成.md`（canonical IR / visitor）、`docs/04-虚拟文件系统与多协议.md`（backend 建立在 VFS 之上）、
> `docs/00`（ADR-0011 / ADR-0021 / ADR-0013）。

## 1. 模块概述

- **职责**：提供**唯一内存表示（反射描述符）到字节**的双向转换，并把字节落/取到任意 backend。
  - 两轴正交：**codec**（决定字节表示：`text/json`、`text/yaml`、`binary`、`binary+compress`）×
    **backend**（决定字节去哪：VFS 路径 `file://`/`mem://`/`zip://`/`https://`）。
  - 所有 codec 都是对**反射描述符**的 **visitor**（docs/06 §5）→ 天然覆盖全部类型。
  - 硬保证：**binary ⇄ text 无损往返**（同一 canonical IR 派生）。
- **所在层**：**数据层**（位于 VFS 之上、asset/world 之下）。
- **上游文档**：docs/08（主）、docs/06、docs/04、docs/00（ADR-0011/0021/0013）。
- **一句话**：`serialize(value, descriptor) → codec(visitor) → backend(VFS)`，文本是开发期唯一真相，二进制是可再生派生。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（ADR-0013）。所有公开类型为 POD / 句柄 / 函数指针；**禁**跨边界异常 / RTTI / STL。
> 前置依赖：反射描述符 `mq_type_desc` / `mq_field_desc` / `mq_type_id` 由 `core` 模块提供（docs/06 §4），本模块只消费其 **只读** C ABI。

### 2.1 公共头（`include/mq_persist.h`）

```c
/* mq_persist.h —— 纯 C，可跨 .so / WASM */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mq_reflect.h"   /* core：mq_type_desc / mq_field_desc / mq_type_id */

#define MQ_PERSIST_ABI_VERSION 1u
#define MQ_PERSIST_BIN_MAGIC   0x4650514Du  /* "MQPF" little-endian */

/* ---------- 错误模型：std::expected 风格的 C 映射，无异常越界 ---------- */
typedef enum mq_result {
    MQ_OK                 = 0,
    MQ_E_IO               = 1,   /* backend 读/写失败 */
    MQ_E_EOF              = 2,   /* 输入提前结束 */
    MQ_E_CODEC            = 3,   /* codec 语法/结构非法 */
    MQ_E_UTF8             = 4,   /* 非法 UTF-8 */
    MQ_E_TRAILING         = 5,   /* 解析后仍有残留字节 */
    MQ_E_TYPE_MISMATCH    = 6,   /* 事件类型与描述符不符 */
    MQ_E_UNKNOWN_FIELD    = 7,   /* 输入含 schema 未声明字段 */
    MQ_E_MISSING_REQUIRED = 8,   /* 缺少必填(无默认值)字段 */
    MQ_E_RANGE            = 9,   /* 数值越界(attrs.range) / 溢出 */
    MQ_E_ENUM_UNKNOWN     = 10,  /* 枚举符号未知 */
    MQ_E_REF_UNRESOLVED   = 11,  /* 引用稳定 ID 无法解析 */
    MQ_E_VERSION          = 12,  /* format_version / abi_version 不兼容 */
    MQ_E_SCHEMA_MISMATCH  = 13,  /* 输入 schema_hash 与当前描述符不符 */
    MQ_E_UNSUPPORTED      = 14,  /* 未知 codec / 未实现类型种类 */
    MQ_E_OOM              = 15,  /* 分配失败 */
    MQ_E_ABI              = 16,  /* struct_size/abi_version 校验失败 */
} mq_result;

typedef struct mq_error {
    mq_result code;
    const char* msg;     /* 静态或所属 ctx 生命周期内有效，不转移所有权 */
    uint32_t    line;    /* 文本 codec：出错行，0=未知 */
    uint32_t    column;  /* 文本 codec：出错列，0=未知 */
} mq_error;

/* ---------- 分配器：所有权契约核心 ---------- */
typedef struct mq_allocator_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* (*alloc)(void* ud, size_t n, size_t align);
    void* (*realloc)(void* ud, void* p, size_t old_n, size_t n, size_t align);
    void  (*free)(void* ud, void* p, size_t n, size_t align);
    void* ud;                     /* 用户上下文 */
} mq_allocator_v1;

/* ---------- 字节流：backend 与 codec 的唯一接缝（POD） ---------- */
typedef struct mq_byte_sink_v1 {          /* 序列化：codec 写字节 */
    uint32_t struct_size; uint32_t abi_version;
    void* self;
    mq_result (*write)(void* self, const void* data, size_t n);
    mq_result (*flush)(void* self);
} mq_byte_sink_v1;

typedef struct mq_byte_source_v1 {        /* 反序列化：codec 读字节 */
    uint32_t struct_size; uint32_t abi_version;
    void* self;
    /* 返回 MQ_OK 且 *got==0 表示 EOF */
    mq_result (*read)(void* self, void* buf, size_t cap, size_t* got);
    /* 可选：零拷贝窥视，返回 0 表示不支持 */
    mq_result (*peek)(void* self, const void** data, size_t* len);
} mq_byte_source_v1;

/* ---------- canonical IR visitor 事件（反射描摹的统一词汇） ---------- */
typedef enum mq_evt_kind {
    MQ_EVT_OBJ_BEGIN, MQ_EVT_OBJ_END,   /* 结构体/聚合 */
    MQ_EVT_FIELD,                        /* 字段名 + 字段描述符 */
    MQ_EVT_ARR_BEGIN, MQ_EVT_ARR_END,    /* 定长/动态数组 */
    MQ_EVT_MAP_BEGIN, MQ_EVT_MAP_END,    /* map<K,V> */
    MQ_EVT_I64, MQ_EVT_U64, MQ_EVT_F64, MQ_EVT_BOOL,
    MQ_EVT_STR,                          /* ptr+len，非 NUL 结尾 */
    MQ_EVT_ENUM,                         /* 符号名 + 数值 */
    MQ_EVT_REF,                          /* entity_ref / asset_ref< T > → 稳定 ID */
    MQ_EVT_NULL,                         /* optional / 空引用 */
} mq_evt_kind;

typedef enum mq_arr_kind { MQ_ARR_FIXED, MQ_ARR_DYNAMIC } mq_arr_kind;

typedef struct mq_evt {
    mq_evt_kind kind;
    union {
        struct { const mq_type_desc* type; uint32_t field_count; } obj_begin;
        struct { const char* name; const mq_field_desc* field; }   field;
        struct { mq_arr_kind arr; uint64_t count; const mq_type_desc* elem; } arr_begin;
        struct { const mq_type_desc* key; const mq_type_desc* value; uint64_t count; } map_begin;
        int64_t  i64;
        uint64_t u64;
        double   f64;
        int      b;                       /* 0/1，避免 C99 _Bool ABI 疑虑 */
        struct { const char* ptr; uint32_t len; int is_utf8; } str;
        struct { const mq_type_desc* type; const char* symbol; int64_t value; } enum_v;
        struct { mq_type_id target; const char* id; } ref;
    };
} mq_evt;

/* ---------- codec：对 canonical IR 的 visitor，双向 ---------- */
#define MQ_CODEC_F_TEXT        (1u<<0)
#define MQ_CODEC_F_COMPRESSED  (1u<<1)
#define MQ_CODEC_F_CANONICAL   (1u<<2)   /* 输出对同一值逐字节确定 */

typedef struct mq_codec_v1 {
    uint32_t struct_size; uint32_t abi_version;
    const char* name;      /* 稳定名："text/json" "text/yaml" "binary" "binary+zstd" */
    const char* mime;      /* "application/json" ... 可用于 VFS 内容协商 */
    uint32_t    flags;

    /* 生命周期：每个 (codec, 一次操作) 一个 ctx */
    void*      (*create)(const mq_allocator_v1* alloc, const struct mq_persist_opts* opts,
                         mq_error* err);
    void       (*destroy)(void* ctx);

    /* 序列化：驱动方(反射)依次投喂 mq_evt，codec 写入 out */
    mq_result  (*encode_begin)(void* ctx, const mq_byte_sink_v1* out,
                               const mq_type_desc* root, const struct mq_persist_opts* opts,
                               mq_error* err);
    mq_result  (*encode_event)(void* ctx, const mq_evt* e, mq_error* err);
    mq_result  (*encode_end)(void* ctx, mq_error* err);

    /* 反序列化：codec 从 in 解析，逐条产出 mq_evt（返回 MQ_E_EOF 表示正常结束） */
    mq_result  (*decode_begin)(void* ctx, const mq_byte_source_v1* in,
                               const mq_type_desc* root, const struct mq_persist_opts* opts,
                               mq_error* err);
    mq_result  (*decode_next)(void* ctx, mq_evt* out, mq_error* err);
    mq_result  (*decode_end)(void* ctx, mq_error* err);
} mq_codec_v1;

/* ---------- backend：把字节流打开在某 URI（VFS）上 ---------- */
typedef struct mq_backend_v1 {
    uint32_t struct_size; uint32_t abi_version;
    const char* name;      /* "vfs"（默认，经 VFS provider） / "mem"（独立单测） */
    void* host;            /* 例如 VFS 实例句柄、mem 存储 */

    mq_result (*open_write)(void* host, const char* uri, const mq_allocator_v1* alloc,
                            const struct mq_persist_opts* opts,
                            mq_byte_sink_v1** out, mq_error* err);
    mq_result (*open_read)(void* host, const char* uri, const mq_allocator_v1* alloc,
                           const struct mq_persist_opts* opts,
                           mq_byte_source_v1** out, mq_error* err);
    void      (*close_write)(void* host, mq_byte_sink_v1* s);
    void      (*close_read)(void* host, mq_byte_source_v1* s);
} mq_backend_v1;

/* ---------- 选项 ---------- */
typedef struct mq_persist_opts {
    uint32_t struct_size; uint32_t abi_version;
    uint32_t format_version;      /* 数据格式版本；0=使用描述符默认 */
    uint64_t expected_schema_hash;/* 0=不校验；否则与描述符 hash 比对 */
    int      omit_defaults;       /* 文本：等于默认值的字段不写出（默认 1） */
    int      stable_key_order;    /* 文本：按 schema 声明序（默认 1；强制） */
    int      pretty;              /* 文本：缩进换行（默认 1） */
    int      array_one_per_line;  /* 文本：数组一行一元素（默认 1） */
    int      float_shortest;      /* 浮点最短往返表示（默认 1） */
    int      allow_unknown_fields;/* 反序列化：未知字段报错还是忽略（默认 0=报错） */
    const mq_allocator_v1* alloc; /* 反序列化动态容器的分配器；NULL=用 host 默认 */
    const char* ref_resolver;     /* 预留：引用解析策略名 */
} mq_persist_opts;

typedef struct mq_persist_stats {
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint32_t fields_visited;
    uint32_t fields_default_omitted;
    uint32_t refs_written;
} mq_persist_stats;

/* ---------- 顶层门面：反射驱动的编排器 ---------- */
typedef struct mq_persist_v1 {
    uint32_t struct_size; uint32_t abi_version;

    /* 内存往返（独立单测核心；不碰 backend） */
    mq_result (*encode_mem)(void* host, const mq_codec_v1* codec,
                            const mq_type_desc* type, const void* value,
                            const mq_persist_opts* opts,
                            void** out_bytes, size_t* out_len,
                            mq_persist_stats* stats, mq_error* err);
    mq_result (*decode_mem)(void* host, const mq_codec_v1* codec,
                            const mq_type_desc* type, const void* bytes, size_t len,
                            void* out_value, const mq_persist_opts* opts,
                            mq_persist_stats* stats, mq_error* err);

    /* 端到端：反射 walk → codec → backend(URI) */
    mq_result (*serialize)(void* host, const mq_codec_v1* codec, const mq_backend_v1* backend,
                           const char* uri, const mq_type_desc* type, const void* value,
                           const mq_persist_opts* opts, mq_persist_stats* stats, mq_error* err);
    mq_result (*deserialize)(void* host, const mq_codec_v1* codec, const mq_backend_v1* backend,
                             const char* uri, const mq_type_desc* type, void* out_value,
                             const mq_persist_opts* opts, mq_persist_stats* stats, mq_error* err);

    /* codec/backend 注册表（冷路径；无全局可变状态，挂在 host 实例上） */
    mq_result (*register_codec)(void* host, const mq_codec_v1* codec);
    const mq_codec_v1* (*find_codec)(void* host, const char* name);
    mq_result (*register_backend)(void* host, const mq_backend_v1* backend);
    const mq_backend_v1* (*find_backend)(void* host, const char* name);
} mq_persist_v1;
```

### 2.2 类型清单

| 类型 | 形态 | 说明 |
|------|------|------|
| `mq_result` / `mq_error` | POD / 句柄 | 错误码 + 静态消息 + 行列 |
| `mq_allocator_v1` | POD vtable | 分配者释放契约；反序列化容器用 |
| `mq_byte_sink_v1` / `mq_byte_source_v1` | POD vtable | codec 与 backend 唯一接缝 |
| `mq_evt` / `mq_evt_kind` | POD | canonical IR 事件（序列化/反序列化共用词汇） |
| `mq_codec_v1` | POD vtable | 名字 + 双向 visitor 回调 |
| `mq_backend_v1` | POD vtable | URI → 字节流 |
| `mq_persist_opts` | POD | 全文规范开关 |
| `mq_persist_stats` | POD | 字节/字段计数，供观测 |
| `mq_persist_v1` | POD vtable | 顶层门面 |

### 2.3 操作面（全部函数语义）

- `encode_mem`：从 `value`（按 `type` 描述符）反射 walk，投喂 `mq_evt` 给 codec，产出 `out_bytes`（调用方用 `opts->alloc` 释放）。**纯内存**，不依赖 VFS，供独立单测。
- `decode_mem`：codec 解析 `bytes` 产出事件，写入 `out_value`（**调用方预分配** 顶层结构体，POD 定长可栈分配；动态容器从 `opts->alloc` 分配）。
- `serialize` / `deserialize`：`encode_mem`/`decode_mem` 的 IO 版；backend 按 `uri` 打开字节流。
- `register_codec` / `find_codec` / `register_backend` / `find_backend`：实例本地注册表（**禁全局可变状态**，ADR-0014）。
- codec 回调顺序契约：`encode_begin` → N×`encode_event` → `encode_end`；`decode_begin` → `decode_next`(至 `MQ_E_EOF`) → `decode_end`。

### 2.4 binary 规范（canonical，可逐字节比对）

小端；头 `{magic u32, format_version u16, codec_tag u16, schema_hash u64}`；其后为**字段流**（OBJ/ARR/标量按 schema 声明序）；默认值**不写出**（与文本一致，另以位图标记 optional/默认？——**no**：二进制为派生缓存，采用**省略默认值 + 显式 presence 位图**）。无时间戳 / 无 uuid / 无指针；`ref` 写 `type_id u64 + 稳定 ID`（长度前缀字符串）。

### 2.5 版本化规则（前向兼容）

1. **ABI 版本化**：所有 vtable 首部 `struct_size + abi_version`。调用方填自己编译期 `struct_size`；被调方校验 `struct_size >= 自身最小需求`，不足→`MQ_E_ABI`；新增回调只允许**追加到尾部**并抬升 `abi_version`，旧调用方因 `struct_size` 较小被识别。
2. **数据格式版本化**：`opts->format_version` 写入二进制头；反序列化时 `> MQ_PERSIST_FORMAT_MAX` → `MQ_E_VERSION`；缺字段按 schema `default` 补齐（docs/06 §7 加字段兼容）。
3. **schema 漂移防护**：`expected_schema_hash` 非 0 时与描述符 hash 比对，不符 → `MQ_E_SCHEMA_MISMATCH`。

### 2.6 内存所有权

- **分配者释放**：`mq_allocator_v1` 显式传入；`encode_mem` 输出的字节缓冲、`decode_mem` 分配的动态容器由**调用方**用同一 allocator 释放。
- codec ctx 由 `codec->create` 分配，`codec->destroy` 释放，配对。
- backend 打开的 `byte_sink/source` 由 `backend->close_write/close_read` 关闭并释放，**禁止**调用方 `free`。
- 跨 `.so`：仅传 POD/句柄，**禁**跨边界 `new`/`delete`/STL。

### 2.7 线程 / 确定性

- codec/backend ctx **非线程安全**，一次操作一个 ctx；注册表在 init 期只读。
- **不在模拟帧内做 IO**（ADR-0018）；异步由上层（VFS/executor）包裹。
- 确定性：对同一值 + 同 `opts`，`text` 与 `binary` 输出**逐字节一致**（稳定键序、最短浮点、无时间戳）；`binary+compress` 的压缩器须确定性（固定 level、无并行）。

## 3. 依赖与边界

- **可依赖**：`core` 反射运行时（只读 `mq_type_desc` / `mq_field_desc` / `mq_type_id`）、`VFS`（backend 的默认实现经其 provider）。
- **禁止**：
  - 不得依赖上层模块（scene/asset/world/renderer…）；
  - 热路径（逐实体）**不得**过本模块虚函数（反射仅供冷路径，docs/06 §5）；
  - **不得**持有全局可变状态；注册表挂在实例上；
  - **不得**暴露裸指针（对外是 URI + 描述符 + POD 值）。
- **接缝**：
  - 上游 `core`：`mq_type_desc` 提供 `type_id`、字段名/偏移/类型种类/`default`/`attrs.transient`；codec 即其 visitor。
  - 下游 `asset pipeline`（`ai`）：调用 `serialize`(text) 与 `serialize`(binary) 做 `cook`；调用 `decode` 做 `dump`。
  - `VFS`：`mq_backend_v1{ name:"vfs" }` 把 `open_read/open_write` 映射到 `res/open`。

## 4. 测试计划（从接口推导，先行）

> 单测只依赖本模块 + `core` 的 **fake 描述符** 与 **mem backend**；不用真实 VFS / 真实游戏模块。

### 4.1 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| T01 | 空结构体→json | `struct{}` | `{}` | 正常 |
| T02 | 省略默认值 | `Vec3{0,0,0}`（默认 0） | `{}`；非默认 `Vec3{1,0,0}` → `{"x":1}` | 规范 |
| T03 | 稳定键序 | 字段声明序 `z,a,m`，值各异 | 输出顺序恒为 `z,a,m`（非字典序） | 规范 |
| T04 | 数组一行一元素 | `int[]= {1,2,3}` | 三行，各一个元素 | 规范 |
| T05 | 浮点最短往返 | `0.1`,`1/3`,`1e-7`,`-0.0`,`1.5` | `0.1`,`0.3333333333333333`,… 解析回原 bit | 规范 |
| T06 | 枚举文本用符号 | `EnemyState::Chase=1` | json 写 `"Chase"`，读回=1 | 正常 |
| T07 | 引用稳定 ID | `asset_ref<Texture>="tex/hero.png"` | 写路径字符串，非序号 | 规范 |
| T08 | transient 不序列化 | 字段 `_scratch` transient | 输出中不存在 | 正常 |
| T09 | 动态容器往返 | `vector<string>`/`map<string,int>`/`optional` | 逐值一致 | 正常 |
| T10 | binary 往返逐位 | 任意值→bin→value | `memcmp==0` | 往返 |
| T11 | bin→text→bin 一致 | 值→bin1；bin1→text；text→bin2 | `bin1==bin2`（逐字节） | 往返 |
| T12 | text→bin→text 一致 | 值→text1；→bin；→text2 | `text1==text2` | 往返 |
| T13 | 确定性 | 同值同 opts 两次 encode | 字节完全一致 | 确定性 |
| T14 | 无易变字段 | 任意输出 | 正则不含时间戳/uuid/绝对路径 | 规范 |
| T15 | 未知字段 | json 含未声明字段 | `MQ_E_UNKNOWN_FIELD`（`allow_unknown=1` 时忽略） | 错误 |
| T16 | 缺必填字段 | 缺无默认值字段 | `MQ_E_MISSING_REQUIRED` | 错误 |
| T17 | 类型不符 | 字段值类型错 | `MQ_E_TYPE_MISMATCH` | 错误 |
| T18 | 非法 JSON | `{x:}` / 截断 | `MQ_E_CODEC`（带 line/col） | 错误 |
| T19 | 残留字节 | `{}{}` | `MQ_E_TRAILING` | 错误 |
| T20 | 非法 UTF-8 字符串 | 无效字节序列 | `MQ_E_UTF8` | 错误 |
| T21 | 数值越界 | `hp=999`（range[0,100]） | `MQ_E_RANGE` | 错误 |
| T22 | 未知枚举符号 | `"Fly"` | `MQ_E_ENUM_UNKNOWN` | 错误 |
| T23 | 引用未解析 | 稳定 ID 无对应资产 | `MQ_E_REF_UNRESOLVED`（策略可配） | 错误 |
| T24 | format 版本过高 | 头 version=999 | `MQ_E_VERSION` | 版本兼容 |
| T25 | 加字段兼容 | 旧数据缺新字段（有 default） | 用 default 补齐，`MQ_OK` | 版本兼容 |
| T26 | schema_hash 不符 | `expected_schema_hash` 错误 | `MQ_E_SCHEMA_MISMATCH` | 版本兼容 |
| T27 | ABI 尺寸过小 | `struct_size` 不足 | `MQ_E_ABI` | 版本兼容 |
| T28 | 分配平衡 | 计数 allocator 跑 encode/decode | alloc==free，无泄漏 | 内存所有权 |
| T29 | 未知 codec | `find_codec("text/toml")` | NULL / `MQ_E_UNSUPPORTED` | 错误 |
| T30 | mem backend 读写 | `mem://scene` 写后读 | 字节一致 | 正常 |
| T31 | 空输入 | `len==0` | `MQ_E_EOF` | 边界 |
| T32 | 定长数组长度不符 | `float[9]` 给 8 个 | `MQ_E_CODEC` | 边界 |

### 4.2 游戏场景用例

- **S1 保存/加载场景**：构造含 2 个节点的场景——`Node{ name, Transform{pos,rot,scale} }`，
  节点 1 带 `Enemy{ hp=30, state=Chase, path=[3×Vec3], sprite="tex/zombie.png" }`，节点 2 为静态 `Prop`。
  1. `serialize(text/json, mem://scene.scn)` → 得文本；
  2. `deserialize(text/json, mem://scene.scn)` → 逐字段比对（节点数、Transform、Enemy.hp/state/path/sprite）；
  3. 断言文本按 schema 声明序、数组一行一元素、默认字段省略。
- **S2 烘焙与一致性**：S1 场景 `serialize(binary, mem://scene.bin)`；从 binary 反序列化后再次 `serialize(text)`；断言与 S1 文本**逐字节一致**（bin→text→bin 与 text→bin→text 均通过）。
- **S3 AI 编辑后重载**：把 `scene.scn` 中 `hp` 手改为 `45`（模拟 AI/编辑器），反序列化 → 断言 `hp==45`，其余不变。
- **S4 敌人属性边界**：`hp=0`（死亡默认省略成 `0` 场景）与 `hp=100`；`hp=101` → `MQ_E_RANGE`。
- **S5 引用稳定性**：新增/删除一个节点后重存，断言 `sprite` 引用路径**不变**（无顺序号漂移）。

### 4.3 确定性用例

- 同 seed / 同值重复 100 次 `encode_mem`，所有字节流 SHA-256 相同；
- `binary` 与 `binary+zstd`（固定 level=3、单线程）同输入 → 同产物；
- 跨编译单元/跨进程（写文件再读）→ 断言一致。

### 4.4 测试替身

- **fake 描述符**：手工构造 `mq_type_desc`（Vec3/Enemy/Node），不依赖 codegen 产物；
- **mem backend**：`name:"mem"`，`host` 为 `map<uri, bytes>`，供 T10–T32 独立单测；
- **计数 allocator**：记录 alloc/free 次数与字节，验证所有权；
- **fake ref resolver**：把稳定 ID 映射到固定表，验证 `MQ_E_REF_UNRESOLVED`。

### 4.5 独立单测保障

- 用例只 `#include "mq_persist.h"` + `"mq_reflect.h"`（fake 描述符）；**不链接** VFS/asset/scene；
- backend 全部走 `mem`；IO 版 `serialize` 仅在 S1/S2 用 mem backend 覆盖；
- CI 中断言链接符号中**不含**其它模块实现。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：落地 `mq_persist.h`（本节全部类型/vtable/错误码）+ `MQ_PERSIST_ABI_VERSION=1`；冻结 binary 头布局与文本规范；定义 `mem` backend 头。评审通过才进入实现。
2. **阶段 1 · 测试骨架（红）**：先写 T01–T32 与 S1–S5 的断言（`text/json` + `binary` + `mem` backend），全部失败。
3. **阶段 2 · 最小实现到绿**：实现反射 walk 编排器、`text/json` codec、`binary` codec、`mem` backend；打通 T01–T13、T28–T31。
4. **阶段 3 · 边界/错误/确定性**：补齐 T14–T27、T32 与确定性用例；引入 `attrs.range`/transient/枚举/引用处理；重构事件流，消除重复。
5. **阶段 4 · 文本规范固化**：省略默认值、稳定键序、一行一元素、最短浮点全绿；加入 `text/yaml` codec（同 visitor 复用）与 `binary+compress`（可选编译）。
6. **阶段 5 · 集成与预算**：`vfs` backend 对接（上层 fake VFS）；接入 `ai` 的 cook/dump 最小路径；跑内存/吞吐预算与泄漏检测。

## 6. 验收标准（DoD）

- 接口 `mq_persist.h` 冻结且被 mem backend / fake 描述符依赖通过；
- T01–T32、S1–S5、确定性用例全绿，单测覆盖率 ≥ 80%；
- **往返一致性成立**：bin→text→bin 与 text→bin→text 均**逐字节 / 逐值一致**；
- 文本规范全部满足（省略默认 / 稳定键序 / 一行一元素 / 最短浮点 / 无时间戳 uuid）；
- 内存所有权无泄漏（计数 allocator 平衡）；无跨模块实现依赖；无全局可变状态；
- 性能预算：单次 `encode/decode` 1e4 字段结构体 < 5 ms（Debug 关闭优化除外），峰值内存 ≈ 输出 × 常数倍。

## 7. 风险

| 风险 | 处理 |
|------|------|
| 文本/二进制漂移 | 单一描述符驱动同一 visitor；T11/T12 往返逐字节用例常驻 CI |
| 浮点格式不稳定的 diff | 统一最短往返格式化（T05）；禁平台相关 `%f` |
| 稳定键序被字典序 Codec 打乱 | codec 按 schema 声明序输出，测试 T03 锁死 |
| 默认值判定歧义（NaN/空串） | 逐字段 bit 比较默认值；`-0.0` 视为非默认，T05 覆盖 |
| 引用顺序号漂移 | 引用一律稳定 ID/路径，T07/S5 防回归 |
| 版本迁移破坏旧数据 | format_version + schema_hash + 加字段默认补齐，T24–T26 |
| 压缩器引入不确定性 | 固定 level、禁并行、禁时间戳；确定性用例覆盖 |
| ABI 变更破坏 `.so` | struct_size + abi_version 追加式演进，T27 守护 |
| 慢（文本体积/解析） | 文本仅 dev；发布烘 binary；`binary+compress` 可选 |

## 8. Owner agent

- **角色标识**：`agent-persistence`
- **职责边界**：仅负责 `persistence` 模块的接口、测试与实现（`mq_persist.h`、codec、backend 抽象、mem backend）；
  不修改 `core`/`VFS`/`asset` 等其它模块；与上游通过冻结 C ABI 交互。
