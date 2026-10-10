# 10 · control plane（CDP 式 JSON-RPC + MCP 上层包装）开发计划

> 复制自 [`_template.md`](_template.md)。接口与测试章节**先于**实现章节完成。
> 主参考：[`docs/11-控制平面与AI接口.md`](../docs/11-控制平面与AI接口.md)、[`docs/16-与浏览器架构对照.md`](../docs/16-与浏览器架构对照.md)；
> 上游 [`docs/00`](../docs/00-设计哲学与约束.md)（A4、ADR-0024）、[`docs/02`](../docs/02-模块系统-ABI与多实例.md)（C ABI / 多实例）、[`docs/09`](../docs/09-可观测性与诊断.md)（Log/Perf）、[`docs/10`](../docs/10-确定性快照与回放.md)（Replay）、[`docs/04`](../docs/04-虚拟文件系统与多协议.md)（VFS）、[`docs/05`](../docs/05-平台抽象与WASM.md)（WASM 传输）。

## 1. 模块概述

- **职责**：把引擎的**全部能力**暴露成**统一、稳定、可编程的驱动面**。底层是一套**对齐 CDP 模型（domain / method / event）的 JSON-RPC 2.0 服务**；上层是 **MCP 包装器**，把 domain/method 映射为 MCP `tools`、把 event 映射为 `notifications`。它自身**不实现任何引擎能力**，只负责**路由、鉴权、能力门控、事件分发、传输适配**——具体能力由各模块经**注册 vtable** 挂入（适配器模式）。
- **所在层**：诊断控制层（最上层；被 AI / CI / 编辑器 / 脚本工具消费，**不进入模拟热路径**）。
- **上游文档**：`docs/11`（协议栈 / domains / 语义 / 安全 / 多实例，本模块主定义）、`docs/16`（CDP 同构、origin→capability）、`docs/00`（A4 硬指标、ADR-0024 已落定 OD-1）、`docs/02`（C ABI vtable、零全局可变状态、instance 隔离、热重载）、`docs/09`（`Log` / `Perf` domain 数据源）、`docs/10`（`Replay` domain 与 checksum）。
- **一句话**：**maque 的 CDP**——AI / CI / 编辑器用同一套 `domain.method` 协议驱动**任意多个隔离实例**，并可持续订阅事件；破坏性操作默认 dry-run、远程内容受 capability 门控。

### 1.1 术语与不变式

| 术语 | 含义 |
|------|------|
| **domain** | 能力分组（如 `Scene` / `VFS` / `Renderer`）。对齐 CDP 的 domain。 |
| **method** | domain 内的一个调用面，线协议名 `"<Domain>.<method>"`（如 `Scene.load`）。 |
| **event** | domain 主动推送的通知，线协议名 `"<Domain>.<event>"`（如 `Log.appended`）。 |
| **handler** | 由能力模块注册的 C 回调，实现某 domain 的 `invoke`；控制平面只做路由。 |
| **call** | 一次已解析、已鉴权的调用上下文（含 `instance_id` / `dry_run` / 身份）。 |
| **transport** | 传输后端 vtable：`stdio` / `tcp` / `ws` / `js`（WASM 内 JS 桥）。 |
| **connection** | 一条传输连接 = 一个身份 + 一份 capability 授权 + 若干事件订阅。 |
| **capability** | 授权串（点分名，如 `Scene.write`、`VFS.mount.remote`、`Module.load`）。 |
| **MCP 包装** | 把 domain/method/event 映射为 MCP tools/resources/notifications 的适配层。 |

**硬不变式（贯穿全部实现与测试）：**

1. **协议即契约**：线协议是 JSON-RPC 2.0；method 名 `Domain.method` 一旦发布**只增不改**，变更走新 domain 或新 method（`docs/11 §10` 防协议膨胀）。
2. **控制平面不改模拟**：所有命令在**帧边界**经队列应用，控制平面运行在独立线程/传输；**无控制连接时行为完全一致**（对齐 `docs/09 §1` 与 ADR-0005 确定性）。
3. **能力门控默认拒绝**：未显式授予的 capability 一律拒绝（`docs/11 §7`）；远程/沙箱来源只能调用被授权子集。
4. **破坏性操作默认 dry-run**：`MQ_METHOD_DESTRUCTIVE` 的 method 在未显式确认时**只预览影响、不生效**（`docs/11 §4`）。
5. **每 method 带 `instance_id`**：多实例（ADR-0014）并行驱动；句柄不跨实例（`docs/02 §5`）。
6. **零全局可变状态**：全部状态在 `mq_control` 实例内；禁单例（ADR-0014）。
7. **边界仅 C ABI**：`extern "C"`、无异常逃逸、无 STL/`std::string` 跨边界、分配者释放（ADR-0013、`docs/02` R1–R8）。
8. **无 GUI 依赖**：headless 全功能；WASM/嵌入式同样可用（JS 桥 / WebSocket 双路径）。

## 2. 接口层设计（完备，先冻结）

> 公共头 `modules/diag/control/include/mq_control.h`（纯 C）。接口名 `"maque.control.v1"`。
> 跨边界仅 POD + 固定宽度整数 + UTF-8 `const char*` + **控制平面自持的 JSON 值/写入器句柄**（`docs/02` R6）。
> 为保持能力模块与传输后端可独立 `.so` 实现，**JSON 读写面、注册面、分发面、事件面、传输面**全部经本 vtable 暴露。

### 2.1 公共头（示意，冻结）

```c
/* mq_control.h —— 纯 C，跨 .so ABI；extern "C" */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MQ_CONTROL_ABI_VERSION 1u
#define MQ_CONTROL_IFACE_NAME  "maque.control.v1"

/* 线协议版本（JSON-RPC/CDP 语义版本）；服务端支持的闭区间 [MIN,MAX] */
#define MQ_CONTROL_PROTO_MIN 1u
#define MQ_CONTROL_PROTO_MAX 1u

/* 与 docs/02 一致的 allocator；本模块只经它分配。 */
typedef struct mq_allocator {
    uint32_t struct_size;
    void* (*alloc)(void* self, size_t size, size_t align);
    void  (*free)(void* self, void* ptr);
    void* self;
} mq_allocator;

/* 错误码：0 = 成功；负 = 错误（无异常，docs/02 R2）。 */
typedef enum mq_control_status {
    MQ_CONTROL_OK                =   0,
    MQ_CONTROL_E_INVALID_ARG     =  -1,
    MQ_CONTROL_E_ABI             =  -2,   /* abi_version 主版本不匹配 */
    MQ_CONTROL_E_ABI_SIZE        =  -3,   /* struct_size 过小无法安全使用 */
    MQ_CONTROL_E_STATE           =  -4,   /* 生命周期状态非法（未 start 等） */
    MQ_CONTROL_E_OOM             =  -5,
    MQ_CONTROL_E_DUPLICATE       =  -6,   /* domain/method 名冲突 */
    MQ_CONTROL_E_NOT_FOUND       =  -7,   /* domain/method/instance/sub/conn 不存在 */
    MQ_CONTROL_E_PARSE           =  -8,   /* JSON 解析失败 */
    MQ_CONTROL_E_INVALID_REQUEST =  -9,   /* JSON-RPC 帧非法 */
    MQ_CONTROL_E_BAD_PARAMS      = -10,   /* params 不符 schema */
    MQ_CONTROL_E_CAPABILITY      = -11,   /* capability 门控拒绝 */
    MQ_CONTROL_E_UNAUTHENTICATED = -12,   /* 缺/坏凭证 */
    MQ_CONTROL_E_DRY_RUN         = -13,   /* 破坏性操作未确认（默认 dry-run，非失败） */
    MQ_CONTROL_E_BACKPRESSURE    = -14,   /* 事件队列溢出 */
    MQ_CONTROL_E_TRANSPORT       = -15,   /* 传输层错误 */
    MQ_CONTROL_E_UNSUPPORTED     = -16,   /* 未实现的传输/特性/协议版本 */
    MQ_CONTROL_E_INTERNAL        = -17
} mq_control_status;

/* ---- 句柄（不透明；禁止裸指针跨边界，docs/02 R4） ---- */
typedef struct mq_control  mq_control;   /* 控制平面实例 */
typedef struct mq_domain   mq_domain;     /* 已注册 domain */
typedef struct mq_listener mq_listener;   /* 已启动的传输监听 */
typedef struct mq_json     mq_json;       /* 只读 JSON 节点（归控制平面） */
typedef struct mq_writer   mq_writer;     /* JSON 写入器 */

/* 世代句柄：连接 / 实例 / 订阅（防悬垂；跨实例不通用）。 */
typedef struct mq_conn_id { uint32_t index; uint32_t generation; } mq_conn_id;
typedef struct mq_sub_id  { uint32_t index; uint32_t generation; } mq_sub_id;

typedef struct mq_buffer { void* data; size_t size; } mq_buffer;

/* ---- method / domain 描述（注册面） ---- */
enum {
    MQ_METHOD_READONLY     = 1u << 0,  /* 查询类：幂等、无副作用 */
    MQ_METHOD_IDEMPOTENT   = 1u << 1,  /* 重复调用等价（配合 idempotency_key 去重） */
    MQ_METHOD_DESTRUCTIVE  = 1u << 2,  /* 破坏性：默认 dry-run，需确认 */
    MQ_METHOD_PER_INSTANCE = 1u << 3,  /* 必须带 instance_id，分发前校验实例存在 */
    MQ_METHOD_MCP_EXPOSE   = 1u << 4,  /* 暴露到 MCP tools/list（默认全暴露） */
    MQ_METHOD_HIDDEN       = 1u << 5   /* 内部方法：不进 MCP、不许远程 */
};

typedef struct mq_method {
    uint32_t    struct_size;
    const char* name;          /* 相对名："load"（线协议拼为 "Scene.load"） */
    uint32_t    flags;         /* MQ_METHOD_* 位组合 */
    const char* required_cap;  /* 需要的 capability（点分名）；NULL = 无要求 */
    const char* params_schema; /* JSON Schema 文本（UTF-8）；NULL = 不校验、MCP 用宽松 schema */
    const char* result_summary;/* 一句话结果说明，供 MCP tool description 与自省 */
} mq_method;

/* invoke 回调：只读 params + 只写 writer；返回码映射为 JSON-RPC error。
   params 与 call 生命周期仅本次调用内有效（不得留存）。 */
typedef mq_control_status (*mq_domain_invoke_fn)(
    void* user, mq_control* ctx, const struct mq_call* call, mq_writer* out);

typedef struct mq_domain_desc {
    uint32_t          struct_size;
    const char*       name;          /* "Scene"（稳定、大小写敏感、CDP 风格） */
    uint32_t          version;       /* domain 语义版本（只增） */
    uint32_t          method_count;
    const mq_method*  methods;       /* 常量数组，调用方须保证生命周期 ≥ destroy */
    const char*       required_cap;  /* 整个 domain 的门槛能力；NULL = 无 */
    mq_domain_invoke_fn invoke;
    void*             user;          /* 能力模块上下文；不透明回传 */
} mq_domain_desc;

/* 一次调用的解析后上下文。 */
typedef struct mq_call {
    uint32_t      struct_size;
    const char*   domain;        /* 已解析 */
    const char*   method;        /* 相对名 */
    const mq_json* params;       /* 只读；可为 NULL（无 params） */
    const char*   instance_id;   /* 可为 NULL（非 per-instance 方法） */
    uint32_t      dry_run;       /* 1 = 只预览不生效（见 §2.10） */
    const char*   idempotency_key; /* 可为 NULL；服务端据此去重 */
    mq_conn_id    conn;          /* 发起连接 */
    const char*   subject;       /* 鉴权后的身份（可读对象名）；可为 NULL */
    mq_writer*    out;           /* 便捷：等价于 invoke 的 out 参数 */
} mq_call;

/* ---- 事件（推送面） ---- */
typedef struct mq_event_desc {
    uint32_t    struct_size;
    const char* domain;
    const char* name;
    const char* params_schema;  /* 事件负载 schema；NULL = 不校验 */
} mq_event_desc;

/* 订阅过滤器（只读 JSON，字段见 §2.8）：{ "domains":[...], "events":[...],
   "instance_id":"...", "min_level":"warn", "sample":0.1 } */
typedef struct mq_sub_opts {
    uint32_t    struct_size;
    const char* filter_json;    /* NULL = 订阅全部（受 capability 限制） */
    uint32_t    max_queue;      /* 该订阅背压队列上限；0 = 用连接默认 */
    uint8_t     overflow_policy;/* 0=drop_oldest(默认),1=drop_newest,2=disconnect */
    uint8_t     _pad[3];
} mq_sub_opts;

/* ---- 传输（传输面，见 §2.7） ---- */
/* 传输侧不透明连接令牌；由传输实现持有，控制平面仅回传。 */
typedef void* mq_conn_token;

typedef struct mq_transport_cbs {
    uint32_t struct_size;
    void* user;
    /* 传输发现新连接：控制平面分配 mq_conn_id，令牌回传供 send/close 使用 */
    void (*on_open)(void* user, mq_conn_token tok, mq_conn_id* out_id);
    void (*on_message)(void* user, mq_conn_id id, const void* data, size_t n);
    void (*on_close)(void* user, mq_conn_id id);
} mq_transport_cbs;

typedef struct mq_transport_v1 {
    uint32_t    struct_size;
    uint32_t    abi_version;
    const char* scheme;      /* "stdio" "tcp" "ws" "js" */
    /* spec：连接/监听配置串（如 "127.0.0.1:9222"、"ws://..."）；本地实现可忽略 */
    mq_control_status (*start)(void* listener, const char* spec, const mq_transport_cbs* cbs);
    mq_control_status (*send)(void* listener, mq_conn_token tok, const void* data, size_t n);
    mq_control_status (*close)(void* listener, mq_conn_token tok);
    void (*stop)(void* listener);
} mq_transport_v1;

/* ---- 鉴权（主机注入，见 §2.9） ---- */
typedef struct mq_identity {
    uint32_t    struct_size;
    const char* subject;      /* 身份名（可读）；归主机，调用期有效 */
    const char* const* caps;  /* 授予的 capability 名数组 */
    uint32_t    cap_count;
} mq_identity;

/* 主机鉴权回调：token 为连接首帧携带的凭证；peer 为来源描述（如 IP/URL）。
   返回 OK 并填 identity；或 E_UNAUTHENTICATED。本地 stdio 可由主机无条件放行。 */
typedef mq_control_status (*mq_auth_fn)(
    void* host, const char* token, const char* peer, mq_identity* out);

/* ==== vtable：完整操作面 ==== */
typedef struct mq_control_v1 {
    uint32_t struct_size;      /* = sizeof(mq_control_v1) */
    uint32_t abi_version;      /* 主版本不匹配 → E_ABI */

    /* ---- 生命周期 ---- */
    mq_control_status (*create)(void* host, const mq_allocator* alloc,
                                mq_auth_fn auth, mq_control** out);
    void (*destroy)(mq_control* ctx);

    /* ---- 帧边界驱动（ADR-0005）：在模拟线程每帧调用一次。
       仅做两件事：① 应用已入队命令；② 冲刷出站事件。绝不阻塞。 ---- */
    void (*pump)(mq_control* ctx);

    /* ---- domain 注册（能力模块 / 适配器调用） ---- */
    mq_control_status (*register_domain)(mq_control* ctx, const mq_domain_desc* d,
                                         mq_domain* out);
    mq_control_status (*unregister_domain)(mq_control* ctx, mq_domain* d);
    mq_control_status (*register_event)(mq_control* ctx, const mq_event_desc* e);

    /* ---- 实例注册（多实例路由，ADR-0014） ---- */
    mq_control_status (*register_instance)(mq_control* ctx, const char* instance_id,
                                           void* engine_ctx);
    mq_control_status (*unregister_instance)(mq_control* ctx, const char* instance_id);
    mq_control_status (*resolve_instance)(mq_control* ctx, const char* instance_id,
                                          void** out_engine_ctx, uint32_t* out_gen);

    /* ---- 传输 ---- */
    mq_control_status (*register_transport)(mq_control* ctx, const mq_transport_v1* vt);
    mq_control_status (*listen)(mq_control* ctx, const char* scheme, const char* spec,
                                mq_listener** out);
    void (*stop_listener)(mq_control* ctx, mq_listener* l);

    /* ---- 分发 ---- */
    /* 喂入一帧原始字节（可含 JSON-RPC batch 数组）；同步完成请求并回写响应。
       响应字节经能力的传输 send 写出；本地/嵌套调用走 call_local。 */
    mq_control_status (*dispatch_bytes)(mq_control* ctx, mq_conn_id conn,
                                        const void* data, size_t n);
    /* 进程内调用（测试、脚本内嵌、CLI）；不走传输。 */
    mq_control_status (*call_local)(mq_control* ctx, const char* method,
                                    const char* params_json, const char* instance_id,
                                    uint32_t dry_run, mq_buffer* out);

    /* ---- 事件推送 ---- */
    mq_control_status (*event_begin)(mq_control* ctx, const char* domain, const char* name,
                                     const char* instance_id, mq_writer** out);
    mq_control_status (*event_commit)(mq_control* ctx, mq_writer* w);
    void (*event_abort)(mq_control* ctx, mq_writer* w);

    /* ---- 订阅 ---- */
    mq_control_status (*subscribe)(mq_control* ctx, mq_conn_id conn, const mq_sub_opts* o,
                                   mq_sub_id* out);
    mq_control_status (*unsubscribe)(mq_control* ctx, mq_sub_id s);
    mq_control_status (*set_backpressure)(mq_control* ctx, mq_conn_id conn,
                                          uint32_t max_queue, uint8_t policy);

    /* ---- 能力门控 ---- */
    mq_control_status (*grant)(mq_control* ctx, mq_conn_id conn, const char* cap);
    mq_control_status (*revoke)(mq_control* ctx, mq_conn_id conn, const char* cap);
    mq_control_status (*check_cap)(mq_control* ctx, mq_conn_id conn, const char* cap);

    /* ---- JSON 只读访问（handler 解析 params） ---- */
    int         (*json_type)(const mq_json* v);            /* mq_json_type，无 = -1 */
    int64_t     (*json_int)(const mq_json* v, int* ok);    /* 整数/布尔取整 */
    double      (*json_double)(const mq_json* v, int* ok);
    int         (*json_bool)(const mq_json* v, int* ok);
    const char* (*json_str)(const mq_json* v, size_t* out_len); /* UTF-8；无 = NULL */
    uint32_t    (*json_size)(const mq_json* v);            /* 数组/对象元素数 */
    const mq_json* (*json_get)(const mq_json* obj, const char* key); /* 对象取键 */
    const mq_json* (*json_at)(const mq_json* arr, uint32_t i);       /* 数组取元 */

    /* ---- JSON 写入（handler 产出 result / event payload） ---- */
    void (*w_null)(mq_writer* w);
    void (*w_bool)(mq_writer* w, int v);
    void (*w_int)(mq_writer* w, int64_t v);
    void (*w_double)(mq_writer* w, double v);
    void (*w_str)(mq_writer* w, const char* s, size_t n);  /* n=SIZE_MAX → strlen */
    void (*w_key)(mq_writer* w, const char* key);          /* 对象键 */
    void (*w_begin_array)(mq_writer* w);
    void (*w_begin_object)(mq_writer* w);
    void (*w_end)(mq_writer* w);                           /* 收尾当前数组/对象 */
    void (*w_raw)(mq_writer* w, const char* json, size_t n); /* 已校验的 JSON 片段 */

    /* ---- 内存释放（分配者释放） ---- */
    void (*buffer_free)(mq_control* ctx, mq_buffer* buf);
} mq_control_v1;

typedef enum mq_json_type {
    MQ_JSON_NULL = 0, MQ_JSON_BOOL = 1, MQ_JSON_INT = 2, MQ_JSON_DOUBLE = 3,
    MQ_JSON_STRING = 4, MQ_JSON_ARRAY = 5, MQ_JSON_OBJECT = 6
} mq_json_type;
```

### 2.2 类型清单

| 类型 | 类别 | 说明 |
|------|------|------|
| `mq_allocator` | POD vtable | 平台约定分配契约（`docs/02` R5）。 |
| `mq_control_status` | enum | 模块统一错误码（§2.4）。 |
| `mq_control` | 不透明句柄 | 控制平面实例（含注册表 / 连接表 / 订阅表 / 队列；零全局）。 |
| `mq_domain` | 不透明句柄 | 已注册 domain；`unregister` 时校验世代。 |
| `mq_listener` | 不透明句柄 | 一个已启动传输监听。 |
| `mq_conn_id` / `mq_sub_id` | POD `{index,generation}` | 连接 / 订阅世代句柄，防悬垂。 |
| `mq_method` / `mq_domain_desc` | POD 描述 | 注册面契约（常量数据，调用方保证生命周期）。 |
| `mq_call` | POD 视图 | 单次调用上下文；回调内有效，不得留存。 |
| `mq_event_desc` | POD 描述 | 事件注册（供自省与 MCP 映射）。 |
| `mq_sub_opts` | POD | 订阅过滤器 + 背压参数。 |
| `mq_transport_v1` / `mq_transport_cbs` | C vtable | 传输后端 + 连接回调（§2.7）。 |
| `mq_identity` / `mq_auth_fn` | POD / 函数指针 | 主机注入的鉴权与授权模型（§2.9）。 |
| `mq_json` / `mq_writer` | 不透明句柄 | JSON 只读视图 / 写入器；生命周期见 §2.6。 |
| `mq_json_type` | enum | JSON 值类型标签。 |
| `mq_buffer` | POD | 输出缓冲，调用方持有，须 `buffer_free`。 |

### 2.3 操作面语义（逐函数契约）

| 函数 | 语义 / 参数 / 返回 | 错误 |
|------|-------------------|------|
| `create` | 用 `host`+`alloc`+`auth` 建实例；全部状态在返回的 `mq_control` 内。`abi_version` 不符 → `E_ABI`。 | `E_INVALID_ARG`,`E_ABI`,`E_OOM` |
| `destroy` | 关闭全部监听、断开连接、清空订阅与队列；对 `NULL` 幂等。 | — |
| `pump` | **帧边界**调用：① 应用入站命令队列；② 冲刷出站事件队列。绝不阻塞、不分配大对象。 | — |
| `register_domain` | 校验名唯一、方法名唯一、vtable 版本；登记到路由表。返回 `mq_domain` 句柄。 | `E_DUPLICATE`,`E_INVALID_ARG`,`E_OOM` |
| `unregister_domain` | 移除 domain 及其订阅路由；其 `mq_domain` 失效。 | `E_NOT_FOUND` |
| `register_event` | 登记事件（供 `Types.describe` / MCP 通知映射 / schema 校验）。 | `E_DUPLICATE` |
| `register_instance` | 登记 `instance_id → engine_ctx`；`Instance.create` 成功后由主机调用。 | `E_DUPLICATE`,`E_OOM` |
| `unregister_instance` | 下线实例；随后指向它的调用返回 `E_NOT_FOUND`。 | `E_NOT_FOUND` |
| `resolve_instance` | 查实例；`MQ_METHOD_PER_INSTANCE` 方法分发前校验。 | `E_NOT_FOUND` |
| `register_transport` | 按 `scheme` 注册传输工厂 vtable（生命周期归调用方）。 | `E_DUPLICATE`,`E_ABI` |
| `listen` | 以 `spec` 启动监听；`start` 回调进入连接状态机（§2.7）。 | `E_NOT_FOUND`(scheme),`E_TRANSPORT` |
| `stop_listener` | 停止监听并关闭其在册连接。 | — |
| `dispatch_bytes` | 解析一帧（请求 / 通知 / batch）→ 校验 → 鉴权 → 门控 → 路由 → handler → 写响应。逐元素处理 batch。 | `E_PARSE`,`E_INVALID_REQUEST`,`E_CAPABILITY`,`E_UNAUTHENTICATED`,`E_NOT_FOUND`,`E_BAD_PARAMS` |
| `call_local` | 进程内直调，不经传输与鉴权（供测试 / CLI / 脚本内嵌）；仍走 capability？**否**（本地可信，见 §2.9）。 | 同上（除鉴权） |
| `event_begin/commit/abort` | 构造事件负载：begin 返回 writer，commit 推送，abort 丢弃（失败清理）。 | `E_NOT_FOUND`(未注册 event),`E_BACKPRESSURE` |
| `subscribe/unsubscribe` | 按过滤器登记推送；返回 `mq_sub_id`。 | `E_NOT_FOUND`,`E_CAPABILITY`,`E_OOM` |
| `set_backpressure` | 调整连接事件队列上限与溢出策略。 | `E_NOT_FOUND` |
| `grant/revoke/check_cap` | 运行时增删/查询连接 capability（如远程内容加载后动态授权）。 | `E_NOT_FOUND` |
| `json_*` | 只读访问 `params` / 事件 schema；指针仅调用期有效。 | 越界/类型不符返回 `NULL`/`ok=0` |
| `w_*` | 写入 result。键/值顺序即输出顺序（**确定性**）。 | 溢出 → 内部标记，`commit` 时 `E_OOM` |
| `buffer_free` | 释放本模块 `call_local` 分配的 `mq_buffer`。对空幂等。 | — |

### 2.4 错误模型（结构化）

- **无异常、无越界**：全部入口 `noexcept` 语义，错误以负 `mq_control_status` 返回（`docs/02` R2）。C++ 内部用 `std::expected<T, mq_control_status>`，**边界只传返回码**。
- **JSON-RPC 2.0 错误码映射**（服务端固定，写死并测试）：

| 内部状态 | JSON-RPC `code` | `message` | `data`（结构化） |
|----------|-----------------|-----------|------------------|
| `E_PARSE` | `-32700` | Parse error | `{ "at": <byte offset> }` |
| `E_INVALID_REQUEST` | `-32600` | Invalid Request | `{ "reason": "..." }` |
| `E_NOT_FOUND`（method） | `-32601` | Method not found | `{ "method": "Scene.nope" }` |
| `E_BAD_PARAMS` | `-32602` | Invalid params | `{ "schema_path": "...", "reason": "..." }` |
| `E_INTERNAL` | `-32603` | Internal error | `{ "detail": "..." }` |
| `E_UNAUTHENTICATED` | `-32000` | Unauthenticated | `{ "peer": "..." }` |
| `E_CAPABILITY` | `-32001` | Capability denied | `{ "required": "Module.load", "domain": "Module", "instance_id": "..." }` |
| `E_NOT_FOUND`（instance/domain/conn/sub） | `-32002` | Not found | `{ "kind": "instance", "id": "..." }` |
| `E_DRY_RUN` | `-32003` | Confirmation required | `{ "dry_run": true, "preview": {...} }`（**非失败**，见 §2.10） |
| `E_BACKPRESSURE` | `-32004` | Backpressure | `{ "dropped": N, "policy": "drop_oldest" }` |
| `E_STATE` | `-32005` | Bad state | `{ "state": "..." }` |
| `E_UNSUPPORTED` | `-32006` | Unsupported | `{ "feature": "..." }` |
| `E_TRANSPORT` / 其它 | `-32099` | Server error | `{ "detail": "..." }` |

- **domain 级错误**：handler 可经 `data` 携带 domain 自定义子错误码（`{ "domain_code": "SCENE_CYCLE", ... }`）；控制平面只透传、不改写 `code`。
- **不变量**：成功时 `result` 完整；错误时 `result` 缺席且 `error` 完整，二者互斥（JSON-RPC 2.0）。
- **首错即停 vs batch 继续**：batch 中每元素独立响应；单元素失败不影响其它元素。

### 2.5 版本化（ABI + 协议双轨）

- **ABI 版本化**：`mq_control_v1` 以 `struct_size`+`abi_version` 打头；`struct_size` **只增不改**，新函数追加表尾，旧调用方按自身 `struct_size` 截断（`docs/02 §3`）。`abi_version` 主版本不匹配 → `E_ABI`。
- **线协议版本化**：`MQ_CONTROL_PROTO_MIN..MAX` 封闭；客户端在 `Control.hello` / 首帧 `_meta.protocol_version` 声明，超出区间 → `E_UNSUPPORTED`（`-32006`）。语义变更走**新 domain / 新 method**，不破坏既有 method（不变式 1）。
- **自省即兼容**：`Control.manifest` 返回全部 domain / method / event / 版本 / schema，供客户端**能力协商**与 AI 自适应（A1）。
- **接口名**为稳定字符串 `"maque.control.v1"`，经 `mq_module_query` 获取（`docs/02 §3`）。

### 2.6 内存所有权

| 所有权 | 规则 |
|--------|------|
| **`mq_json` / `mq_writer`** | 由控制平面为单次调用分配，**仅在本次 `dispatch_bytes`/`call_local` 或 `event_begin..commit` 期间有效**；handler 不得缓存指针。事件 writer 由 `event_commit`/`event_abort` 释放。 |
| **注册描述**（`methods[]` / `name` / vtable） | 归注册方，须保证生命周期 ≥ `unregister_domain`；控制平面**不复制**（避免跨 `.so` 拷贝歧义）。 |
| **`mq_buffer`（`call_local`）** | 本模块经传入 `alloc` 分配 → 调用方 `buffer_free`。 |
| **传输令牌 `mq_conn_token`** | 归传输实现；控制平面只透传，不释放。 |
| **`mq_identity` 字符串 / caps** | 归主机；在连接生命周期内有效（连接关闭后主机可回收）。 |
| **发送字节** | `dispatch_bytes` 产生的响应归控制平面出站队列，由传输 `send` 消费；调用方不持有。 |
| **禁** | 跨 `.so` 传 `std::string/vector/shared_ptr`；A 分配 B 释放（`docs/02 §9`）。 |

### 2.7 传输抽象（可插拔）

- 传输实现 `mq_transport_v1`：`start/send/close/stop`。控制平面通过 `scheme` 选择。
- **连接状态机**：`start` 后传输调用 `on_open(tok)` → 控制平面分配 `mq_conn_id` 并回填 → 传输持有 `(tok → id)` 映射 → 每帧 `on_message(id,bytes)` 交给 `dispatch_bytes` → 断开调 `on_close(id)`。
- 出站：控制平面将响应/事件序列化后交 `send(tok,bytes)`；传输负责帧化（stdio 为 `\n` 分隔 / `Content-Length`，WS 为 text 帧，TCP 为 NDJSON，JS 桥为 `postMessage`）。
- **四类传输**：
  - `stdio`：本地、默认；一行一个 JSON-RPC（CDP 兼容用 `Content-Length` 头，可配置）。
  - `tcp`：`host:port`；默认必须鉴权。
  - `ws`：`ws://host:port/path`；浏览器 / 远程首选。
  - `js`：**WASM 内 JS 桥**（`docs/05`）；宿主 JS 注入 `send/onmessage`，控制平面经此收发；受限环境（无 socket）可用 WebSocket 双路径（`docs/11 §10`）。
- 传输**不解析协议**，只搬运字节；解析与路由全在控制平面（保证单一语义）。

### 2.8 事件订阅与背压

- **订阅过滤器**（`mq_sub_opts.filter_json`）：`domains[]`、`events[]`、`instance_id`、`min_level`（对 `Log`）、`sample`（0–1 采样）。空 = 全部（受 capability 限制）。
- **推送格式**：JSON-RPC 2.0 notification，`method` = `"<Domain>.<event>"`，`params` = `{ "instance_id", "seq", "ts_ns", ...payload }`。
- **确定性序**：同一实例内事件按 `seq` 单调递增（由 `event_commit` 分配），保证 AI/CI 可稳定比对。
- **背压**：每连接/订阅一条有界队列（默认 `max_queue=1024`）；溢出策略 `drop_oldest`（默认，保最新）/`drop_newest`/`disconnect`；丢弃计入 `dropped` 计数并经 `Control.backpressure` 事件告知。**背压绝不阻塞模拟线程**——`event_commit` 满则立即按策略处理并返回。
- **采样**：`sample<1` 用引擎 RNG？**否**——用**连接本地** PRNG（与模拟 RNG 隔离），避免订阅改变模拟（不变式 2）。

### 2.9 鉴权与 capability 门控

- **鉴权**：连接建立后首帧须为 `Control.hello { token, protocol_version }`；控制平面调 `mq_auth_fn` 得 `mq_identity`（`subject` + `caps`）。失败 → 关闭连接 / `E_UNAUTHENTICATED`。本地 `stdio` 由主机策略决定（可无条件放行）。
- **capability 命名**：点分名，**默认拒绝**。约定前缀：`<Domain>.read` / `<Domain>.write` / `<Domain>.admin`；特殊：`VFS.mount.remote`（挂远程）、`VFS.write`、`Scene.snapshot`、`Module.load`、`Runtime.eval`、`Renderer.capture`、`Replay.control`。
- **门控点**（分发前按序）：① 连接是否已鉴权；② method `required_cap`；③ domain `required_cap`；④ `MQ_METHOD_HIDDEN` 只允许本地（`call_local`）。
- **与浏览器 origin 对照**（`docs/16 §4`）：一个"来源"（本地游戏 / `https://microgames.org/xxx` 远程内容）映射为一组 capability；远程内容**只能**调用被授权子集（`docs/11 §7`、ADR-0030）。
- **动态授权**：`grant`/`revoke` 支持运行时调整（如加载远程内容前临时降权）。
- **`call_local` 例外**：进程内调用可信，跳过鉴权但与远程走**同一门控与 dry-run**逻辑（保证语义一致），仅 `HIDDEN` 方法放行。

### 2.10 dry-run 与幂等性

- **dry-run**：`MQ_METHOD_DESTRUCTIVE` 的 method：
  - 请求未带 `"confirm": true` → 控制平面注入 `dry_run=1` 调 handler；handler 计算**影响预览**（如"将删除 N 个节点、引用 M 个资产"）返回 `result.preview`，且**不产生副作用**；控制平面以**成功**返回（`result._applied=false`），**不**报错误（除非 handler 主动返回 `E_DRY_RUN` 携带 preview）。
  - 请求带 `"confirm": true` → `dry_run=0`，且需相应 capability（如 `Scene.write`）。
  - 该机制使 AI **安全试错**（`docs/11 §4`）。
- **幂等性**：
  - `MQ_METHOD_READONLY`：天然幂等，无 `idempotency_key` 需求。
  - `MQ_METHOD_IDEMPOTENT` 且带 `idempotency_key`：控制平面按 `(conn.subject, method, key)` 缓存上次结果（LRU，默认 256 条），重复调用**返回缓存 result**，不重复执行。
  - 变更类返回 `{ "handle": ..., "version": n, "applied": true }`，供回滚（`docs/11 §4`）。
- **确认令牌**：可选更强的两段式（`preview` 返回一次性 `confirm_token`，apply 时回传并校验未过期）。

### 2.11 MCP 映射约定

| MCP 概念 | maque 控制平面 | 约定 |
|----------|----------------|------|
| `tools/list` | 全部 `MQ_METHOD_MCP_EXPOSE` 的 domain×method | tool `name` = `"<Domain>_<method>"`（点号替换为 `_`，满足 MCP `[a-zA-Z0-9_-]{1,64}`）；`description` = `result_summary` + 线协议名；`inputSchema` = `params_schema`（缺省生成宽松 object schema）。 |
| `tools/call` | 转发为 `Domain.method` | `arguments` → `params`；`instance_id` 作为普通字段；`_meta.mcp` 保留原 `requestId`。 |
| `resources/list` | 自省对象集合 | 泛型：`mq://instances`、`mq://types`、`mq://types/{type_id}`、`mq://scene/{instance_id}`、`mq://vfs/{instance_id}`、`mq://perf/{instance_id}`；由 `Types`/`Scene`/`VFS`/`Perf` 的 read-only method 支撑。 |
| `resources/read` | 等价于对应 `dump`/自省 method | 唯一 uri → 调 `Types.schema` / `Scene.dump` / `VFS.dump_mounts` 等，返回 JSON。 |
| `notifications` | 控制平面 event | MCP `notifications/message`（或 `notifications/mq/<Domain>/<name>`）；订阅经 `tools/call control_subscribe`。 |
| `prompts` | **不提供** | 明确非目标（AI 提示词不属于引擎契约）。 |

- **MCP 传输**：`stdio`（本地工具/CLI）优先；远程用 Streamable HTTP / SSE，内部复用控制平面 `ws` 或专用 MCP transport（同样实现 `mq_transport_v1`，`scheme="mcp"`）。
- **无状态映射保证**：MCP 层**无独立状态**，纯翻译 domain/method/event（`docs/11 §2`：MCP 只是上层包装）。
- **失败映射**：JSON-RPC error → MCP `isError` result + 文本 `message` + 结构化 `data`。

### 2.12 内置域（自举）

控制平面自身注册一个 `Control` domain（`required_cap=NULL`，但敏感方法带 cap）：

| method | flags | 语义 |
|--------|-------|------|
| `Control.hello` | — | 握手：`{ token, protocol_version }`；建立身份。 |
| `Control.manifest` | READONLY | 返回全部 domain/method/event/版本/schema（能力协商 + A1）。 |
| `Control.subscribe` | — | `{ filter, max_queue, overflow_policy }` → `sub_id`。 |
| `Control.unsubscribe` | — | 取消订阅。 |
| `Control.version` | READONLY | 服务端 ABI/协议版本区间。 |
| `Control.backpressure` | READONLY | 查询连接的丢弃/积压统计。 |

其余 13 个 domain（`Instance` / `Runtime` / `Types` / `Scene` / `VFS` / `Asset` / `Renderer` / `Script` / `Perf` / `Log` / `Replay` / `Debugger` / `Module`）**由对应能力模块经 `register_domain` 挂入**；控制平面不硬编码它们（解耦；单测用 `fake_domain` 验证路由）。

## 3. 依赖与边界

- **可依赖（仅经 C ABI / 稳定接口）**：
  - **core/data**：POD、句柄、`std::expected`（仅模块内部）。
  - **platform**：socket、stdio、时钟（`ts_ns`）、模块加载（薄抽象，`docs/05`）。
  - **executor**：可选，用于把阻塞传输 IO 放到后台；**模拟线程只经 `pump`**。
  - **JSON 库**：模块**内部**选型（如 `yyjson`/自研），**不跨边界暴露**（对外仅 `mq_json`/`mq_writer`）。
  - **能力模块**：只经 `register_domain`/`register_event` 反向注入；控制平面**不**直接 `#include` 它们。
  - **host**：`allocator` + `mq_auth_fn` + 可选日志域。
- **禁止**：
  - ❌ 依赖 scene/renderer/vfs/asset 等**具体实现**（只认其注册的 domain）。
  - ❌ 持有**全局可变状态 / 单例**（ADR-0014）。
  - ❌ 跨边界异常 / RTTI / STL / 裸指针 ABI（`docs/02 §9`）。
  - ❌ 在 `pump` 或模拟线程内阻塞（等待 IO / 锁竞争 / 大分配）。
  - ❌ 让事件推送或订阅**改变模拟结果**（不变式 2）。
  - ❌ 传输层解析 / 改写协议语义（单一语义点）。
- **与其它模块的接缝**：

| 模块 | 接缝 |
|------|------|
| scene(11) | 注册 `Scene` domain；`types`/`dump` 供 `Types`/`Scene` resources。 |
| vfs(06) | 注册 `VFS` domain（`mount/list/read/write/resolve`）；`VFS.mount.remote` cap。 |
| asset(08) | 注册 `Asset` domain；字段与 `mq-asset` CLI 对齐（`docs/08 §2.7`）。 |
| renderer(15) | 注册 `Renderer` domain（`capture_frame` 离屏 readback）。 |
| script(14) | 注册 `Script` domain（eval/断点/单步）。 |
| observability(09) | 注册 `Log`/`Perf` domain；事件源（`Log.appended`、`Perf.sample`、`Crash.report`）。 |
| replay(10) | 注册 `Replay` domain（`load/step/checksum/replay`）。 |
| module host(03) | 注册 `Module` domain（`load/unload/reload`）；`Control` 自身经其加载。 |
| host | `Instance` domain（create/select/list）由主机引导层注册。 |

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**仅依赖本模块 + `mq_control.h`**；
> 传输、能力模块、鉴权、分配、时钟一律用 **fake**，**不得**依赖其它模块完整实现；**不开真实 socket**。

### 4.1 测试替身

| 替身 | 用途 |
|------|------|
| `fake_transport` | 内存传输：`start` 记录监听；测试用 `inject(id, bytes)` 模拟收，`drain(id, out)` 取发；可模拟断线/慢速/满队列。 |
| `fake_domain` | 可编程 domain：记录收到的 `mq_call`（含 `instance_id`/`dry_run`/`idempotency_key`），返回预设 result 或错误；可断言**路由**与**未执行副作用**。 |
| `fake_auth` | 固定 token→identity 映射；可返回 `E_UNAUTHENTICATED`；可预置 caps 集合。 |
| `fake_clock` | 提供确定 `ts_ns`，断言事件 `seq`/时间序。 |
| `fake_alloc` | 记账 allocator：统计分配/释放、注入 OOM、断言**零泄漏**。 |
| `fake_prng` | 订阅采样用确定性 PRNG（断言采样不触及模拟 RNG）。 |

### 4.2 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| U01 | create/destroy 生命周期 | 合法 alloc/auth | `OK`；销毁后 `fake_alloc` 零泄漏 | 正常/所有权 |
| U02 | 注册 domain | 合法 `mq_domain_desc` | `OK`，返回 `mq_domain` 句柄 | 正常 |
| U03 | 重复注册 domain | 同名二次 | `E_DUPLICATE`，原 domain 不受影响 | 错误 |
| U04 | 重复 method 名 | 同 domain 内重名 | `E_DUPLICATE` | 错误 |
| U05 | ABI 版本不匹配 | `abi_version=99` | `E_ABI`，拒绝 | 版本兼容 |
| U06 | struct_size 截断 | 传旧版较小 `struct_size` | 只调用已声明函数段，无越界 | 版本兼容 |
| U07 | 分发简单请求 | `Scene.load` 合法 params | 响应 `result` 完整、`id` 原样回填 | 正常 |
| U08 | 未注册 method | `Nope.x` | `-32601` Method not found | 错误码 |
| U09 | 非法 JSON | 截断字节 | `-32700`，`data.at` 报偏移 | 编解码 |
| U10 | 非法请求帧 | 缺 `jsonrpc`/`method` | `-32600` | 编解码 |
| U11 | params 不符 schema | 缺必填字段 | `-32602`，`data.schema_path` | 编解码 |
| U12 | notification（无 id） | 合法通知 | 执行、**不回响应** | 编解码 |
| U13 | batch 混合 | 2 请求 + 1 通知 + 1 坏元素 | 4 元素各自处理，坏元素 `-32600` | 编解码 |
| U14 | UTF-8 往返 | 含中文/emoji 的字符串 | 原样回传，无 mojibake | 编解码 |
| U15 | 数字边界 | `int64` 极值 / 大 double | 精确往返（int 不转 double） | 编解码 |
| U16 | JSON 转义 | 引号/反斜杠/控制字符 | 合法转义、可再解析 | 编解码 |
| U17 | 深层嵌套 | 64 层对象 | 正常；超限 → `-32600`（防栈溢出） | 边界/安全 |
| U18 | 超大帧 | 超过上限的帧 | 拒绝 `-32600`（防 DoS） | 安全 |
| U19 | 鉴权成功 | 有效 token | 建立身份，`subject` 进 `mq_call` | 安全 |
| U20 | 鉴权失败 | 坏 token | `-32000`，连接不进入可用态 | 安全 |
| U21 | capability 拒绝 | 无 `Module.load` 调 `Module.load` | `-32001`，`data.required` 正确 | capability |
| U22 | capability 放行 | 授予后同调用 | `OK`，handler 被调用 | capability |
| U23 | domain 级 cap | domain 需 `Scene.admin` | 缺则整体拒绝 | capability |
| U24 | HIDDEN 远程拒绝 | 经传输调 HIDDEN method | `-32601`；`call_local` 可调 | capability |
| U25 | 动态 grant/revoke | revoke 后调用 | 立即生效为拒绝 | capability |
| U26 | instance 路由 | 两实例分别 `step` | 各自 handler 收到**正确** `instance_id` | instance |
| U27 | per-instance 校验 | 指向不存在实例 | `-32002`，handler **未**被调用 | instance |
| U28 | 未注册实例注销 | unregister 不存在 | `E_NOT_FOUND` | instance |
| U29 | 订阅过滤 domains | 只订 `Log` | 收到 `Log.*`，不收到 `Scene.*` | 事件 |
| U30 | 订阅过滤 instance | 只订实例 A | 只收 A 的事件 | 事件 |
| U31 | 订阅 min_level | `min_level=warn` | info 被过滤，warn/error 通过 | 事件 |
| U32 | 事件 seq 单调 | 同实例连发 3 事件 | `seq` 1,2,3 递增 | 事件/确定性 |
| U33 | 采样确定性 | `sample=0.5` + `fake_prng` | 固定序列被选中，可复现 | 事件/确定性 |
| U34 | 背压 drop_oldest | 队列满继续 emit | 丢最旧、保最新，`dropped` 计数 | 背压 |
| U35 | 背压 drop_newest | 同上 | 丢最新 | 背压 |
| U36 | 背压 disconnect | 同上 | 连接被断开，`Control.backpressure` 告知 | 背压 |
| U37 | 背压不阻塞 | 满队列时 `event_commit` | 立即返回，**不阻塞** | 背压/确定性 |
| U38 | unsubscribe 后不再收 | 取消订阅 | 后续事件不投递 | 事件 |
| U39 | 失效 sub_id | 用旧句柄 unsubscribe | `E_NOT_FOUND` | 边界 |
| U40 | dry-run 默认 | 破坏性 method 无 `confirm` | `result._applied=false` + `preview`；**副作用未发生** | dry-run |
| U41 | dry-run 确认 | `confirm=true` + cap | `_applied=true`，副作用发生 | dry-run |
| U42 | dry-run 缺 cap | `confirm=true` 无写 cap | `-32001` | dry-run/capability |
| U43 | 幂等去重 | 同 `idempotency_key` 二次 | 返回缓存 result，handler 只执行一次 | 幂等 |
| U44 | 幂等 key 不同 | 不同 key | 各自执行 | 幂等 |
| U45 | 幂等 LRU 淘汰 | 超容量 | 最旧失效，重执行 | 幂等/边界 |
| U46 | 协议版本超范围 | `protocol_version=99` | `E_UNSUPPORTED`/`-32006` | 版本兼容 |
| U47 | 协议版本协商 | `Control.manifest` | 返回 `[MIN,MAX]` 与全部 domain/schema | 版本兼容 |
| U48 | `Control.manifest` 完整 | — | 列出的 domain/method 与注册一致 | 自省 |
| U49 | `call_local` 结果 | 进程内直调 | 返回 `mq_buffer`，`buffer_free` 释放 | 正常 |
| U50 | `call_local` 空参数 | `params_json=NULL` | 视作无 params，正常 | 边界 |
| U51 | OOM 注入 | alloc 第 N 次失败 | 返回 `E_OOM`，已分配全回收 | 内存所有权 |
| U52 | 空/零长帧 | `n=0` | `-32700` 或忽略（不崩溃） | 边界 |
| U53 | NULL 参数 | `ctx=NULL`/`out=NULL` | `E_INVALID_ARG`，不崩溃 | 边界 |
| U54 | 未注册 scheme | `listen("nope",...)` | `E_NOT_FOUND` | 传输 |
| U55 | 断线清理 | transport `on_close` | 订阅/连接资源释放，零泄漏 | 传输/所有权 |
| U56 | 传输关闭时在途响应 | 关闭后写响应 | 静默丢弃，不崩溃、不泄漏 | 传输 |
| U57 | 无模拟污染 | 全程下发命令 | 模拟侧仅经 `pump` 应用；无控制连接时行为一致 | 确定性 |

> **独立单测保障**：U01–U57 全部经 `fake_transport`/`fake_domain`/`fake_auth`/`fake_alloc` 运行，链接时**不链接**任何能力模块；`Scene`/`VFS` etc. 一律以 `fake_domain` 代替。

### 4.3 游戏场景用例（AI 工作流，真实玩法驱动）

- **S1 · AI 标准闭环（`docs/11 §5` 靶心）**
  以 `fake_domain` 扮演 13 个 domain，`fake_transport` 扮演 `ws`：
  步骤（全程 JSON-RPC）：
  1. `Control.hello {token, protocol_version:1}`
  2. `Instance.create {game:"demo"}` → 得 `instance_id="i1"`
  3. `VFS.mount {instance_id, mountpoint:"assets/", source:"https://microgames.org/demo/"}`（需 `VFS.mount.remote`）
  4. `Scene.load {instance_id, path:"assets/main.scn"}`
  5. `Replay.load {instance_id, snapshot:"boot", seed:42, inputs:[...]}`
  6. `Control.subscribe {filter:{domains:["Log","Perf"]}}`
  7. `Replay.step {instance_id, n:60}`
  8. `Renderer.capture_frame {instance_id, frame:60}` → 返回图像流引用
  9. `Types.dump {instance_id}` / `Scene.dump {instance_id}` → JSON
  10. `Perf.sample {instance_id}` / `Log.tail {instance_id}` → 指标 + 日志尾
  11. `Replay.checksum {instance_id}` → 记录 checksum0
  12. 修复后 `Replay.replay {instance_id}` → checksum 与 checksum0 比对
  断言：每步均为稳定结构化返回；事件 `Log.appended`/`Perf.sample` 按 `seq` 投递；`instance_id` 全程正确路由；checksum 一致。
- **S2 · 多实例并行实验（`docs/11 §8`）**
  同一控制平面创建 `i1`(seed=1)、`i2`(seed=2)；对两者并发 `Replay.step`/`checksum`；断言 handler 收到的 `instance_id` 与发起一致、结果互不串扰、事件按实例正确分发。
- **S3 · 远程内容受限（`docs/16 §4`、OD-9）**
  连接 B 身份为 `origin=https://untrusted.example`，仅授予 `Scene.read`/`Renderer.capture`：`VFS.mount.remote` → `-32001`；`Scene.dump` → 成功；`Module.load` → `-32001`；`Scene.destroy`（破坏性，无 `confirm`）→ 仅 dry-run 预览。
- **S4 · AI 安全试错（dry-run）**
  对运行中的 demo 实例发 `Scene.destroy {instance_id, node:"boss"}`（无 `confirm`）→ 返回 `preview:{subtree_nodes:12, external_refs:3, _applied:false}`；实例未变；带 `confirm=true` 且持 `Scene.write` 才真正删除。
- **S5 · 崩溃自省与复现（A2/A3）**
  订阅 `Crash`；触发 `fake_domain` 故意崩溃事件 → 收到 `Crash.report`（含 seed/帧号/checksum）；据其 `Replay.load+step` 复现，`checksum` 一致。
- **S6 · MCP 零适配接入**
  以 MCP `tools/list` 取到 `Scene_load`；`tools/call {name:"Scene_load", arguments:{instance_id:"i1", path:"assets/main.scn"}}` → 映射为 `Scene.load` 并回 success；订阅事件映射为 `notifications/message`。

### 4.4 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| D1 | 同输入同响应 | 同请求 ×3 经 `call_local`，返回字节相同（键序稳定） |
| D2 | 事件 seq 一致 | 同事件序列重放，`seq`/顺序一致 |
| D3 | 采样不污染模拟 | 开/关订阅、改 `sample`，模拟 checksum 不变 |
| D4 | 无控制连接一致 | 有/无活动连接，模拟帧 checksum 相同 |
| D5 | batch 顺序稳定 | batch 逐元素响应序 = 请求序 |
| D6 | 多实例隔离 | 两实例事件流不交叉，各自 seq 独立 |

### 4.5 覆盖率与性能目标

- 语句/分支覆盖率 **≥ 85%**（核心分发/门控/背压/编解码路径 ≥ 90%）。
- 性能预算（参考机）：`dispatch_bytes` 简单请求 **< 50 µs**（不含 handler）；`event_commit` 入队 **< 5 µs**；1k 订阅者扇出单事件 **< 1 ms**；`pump` 每帧 **< 100 µs**（队列空时近 0）。
- 内存：空闲连接 ≤ 8 KB；订阅队列默认 1024 条/连接，可配。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：定稿 `mq_control.h`（§2 全 vtable）、错误码映射表（§2.4）、协议版本区间、capability 命名、MCP 映射表（§2.11）；写 `static_assert(sizeof)` 与 ABI 布局检查。**冻结后签名只增不改。**
2. **阶段 1 · 测试骨架（红）**：实现 `fake_transport`/`fake_domain`/`fake_auth`/`fake_alloc`/`fake_clock`/`fake_prng`；落 U01–U18、U49–U53 骨架（先失败）。
3. **阶段 2 · 编解码 + 分发 + 注册（绿）**：JSON 解析/序列化（严格、无 STL 跨边界）、`register_domain`、`dispatch_bytes`、`call_local`、`Control.hello/manifest`；U01–U18、U48、U49–U53 转绿。
4. **阶段 3 · 鉴权 + capability + 多实例**：`mq_auth_fn` 桥接、门控点链、`register_instance`/`resolve_instance`、`HIDDEN` 规则；U19–U28 转绿；S3 场景。
5. **阶段 4 · 事件 + 背压 + 幂等 + dry-run**：`event_begin/commit`、订阅过滤、`seq`、背压三策略、`idempotency_key` LRU、dry-run 默认与确认；U29–U45、D1–D6 转绿；S1–S5 场景。
6. **阶段 5 · 传输 + 版本兼容 + MCP**：`register_transport`/`listen`/`stop_listener`，实现 `stdio`/`tcp`/`ws`/`js`（js 先桩后接 `docs/05`）；协议版本协商；MCP 包装（tools/resources/notifications）；U46–U47、U54–U57、S6 转绿。
7. **阶段 6 · 加固与预算**：帧边界 `pump` 确定性回归（D3/D4）、OOM/断线/超大帧/深嵌套安全、性能与内存达标、结构化错误 `data` 补全、文档与遗留项。

## 6. 验收标准（DoD）

- `mq_control.h` 接口**冻结**并被各能力模块的 `register_domain` **fake** 依赖编译通过。
- 单测覆盖率 **≥ 85%**；§4.2 全绿；`fake_alloc` 全程**零泄漏**；`fake_transport` 断线零泄漏。
- **JSON-RPC 编解码与错误码**：U09–U18、U46–U47 全绿，错误码映射表逐项验证。
- **capability 门控**：U21–U25、U42、S3 通过；**默认拒绝**语义验证。
- **instance_id 路由**：U26–U28、S2 通过；多实例隔离（D6）。
- **事件订阅/背压**：U29–U39、D2 通过；**背压不阻塞模拟线程**（U37）。
- **dry-run / 幂等**：U40–U45、S4 通过；破坏性操作默认不生效。
- **游戏场景 S1–S6 通过**（AI 工作流 create→mount→load→step→capture→dump→replay 全链）。
- **确定性 D1–D6 通过**：无控制连接时模拟 checksum 一致、订阅/采样不改变模拟。
- **协议版本兼容**：`Control.manifest` 可协商；超范围版本被拒；method 只增不改。
- 可 headless 运行（无 GUI）；WASM JS 桥与 WebSocket 双路径至少各自经 fake/桩验证；无跨模块实现依赖；无全局可变状态。
- 性能/内存预算达标（§4.5）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **协议膨胀**（`docs/11 §10`） | domain 分组 + method 只增不改 + `Control.manifest` 版本化 + 只暴露稳定子集；`HIDDEN` 隔离内部方法。 |
| **远程控制面安全**（`docs/11 §10`、OD-9） | 强制鉴权（远程默认）+ capability 门控**默认拒绝** + 破坏性默认 dry-run + 帧/深度上限防 DoS + 远程内容 origin→cap 映射。 |
| 控制平面对性能影响 | 独立传输/线程 + 有界队列 + `pump` 帧边界应用（O(队列)）+ 出站事件零阻塞；预算 §4.5。 |
| 事件风暴 | 订阅过滤 + 采样（连接本地 PRNG）+ 背压三策略 + `dropped` 计数；U29–U37。 |
| WASM 传输受限（`docs/11 §10`） | JS 桥 / WebSocket 双路径，同一 `mq_transport_v1` 抽象；`docs/05` 对接。 |
| 跨 `.so` JSON 值语义漂移 | JSON 读写**只经 vtable 句柄**，能力模块不得自行解析原始字节；编解码单测 U09–U18。 |
| 控制命令改变模拟（破坏 A3） | 变更命令**只在帧边界**经 `pump` 应用；订阅/采样隔离 PRNG；D3/D4/U57 回归门禁。 |
| dry-run 语义被绕过 | 破坏性 method **必须**标 `DESTRUCTIVE` 且经控制平面统一注入 `dry_run`；handler 不得自行放行（评审 + U40/U41）。 |
| 幂等缓存内存增长 | LRU 上限（默认 256），按 `subject+method+key` 键；U45。 |
| 鉴权模型过弱（本地全放行误用于远程） | `stdio` 放行由**主机显式策略**决定；`tcp`/`ws` 默认强制 token；CI 用独立只读身份。 |

## 8. Owner agent

- **角色标识**：`agent-control`
- **职责边界**：仅负责控制平面的接口（`mq_control.h`）、测试（编解码/门控/订阅/路由/dry-run 与 MCP 映射、确定性用例）与实现（分发、注册、事件、背压、传输抽象、鉴权桥、MCP 包装）；**不修改其它模块**，也不实现任何具体 domain（它们由各模块经 `register_domain` 注入）。
- **交付**：`plans/10-control-plane.md`（本文件）→ `modules/diag/control/`（`mq_control.h`、`mcp/`、`transports/`、测试）。
- **上游协作**：向 `agent-host` 约定 `Instance` domain 与实例注册时机；向 `agent-obs` 约定 `Log`/`Perf` 事件与 `Crash.report` schema；向 `agent-vfs`/`agent-asset` 对齐 `VFS`/`Asset` domain 字段与 `dump` 输出；向 `agent-module-host` 约定 `Module` domain 与 `mq_module_query` 加载路径；向 `agent-platform` 约定 `js` 桥与 socket 抽象。

