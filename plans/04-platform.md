# 04 · platform（薄平台抽象）

> 上游：`docs/05-平台抽象与WASM.md`（主）、`docs/02-模块系统-ABI与多实例.md`、`docs/00-设计哲学与约束.md`（ADR-0016/0017/0018）。
> 本计划把 `docs/05` 落成可执行、可验收的模块开发计划：**接口 + 测试先行，后实现**（TDD）。

## 1. 模块概述

- **职责**：把**跨平台真正有差异**的少数原语收敛到一个薄接口后面——**单调时钟、线程原语、日志 sink、栈回溯（打桩点）、模块加载探测/原语**；向上给 executor(05)/observability(09)/module host(03) 提供**唯一**的 OS 接触面。
- **所在层**：基础（地基，与 math/core/host 同批交付）。
- **上游文档**：`docs/05`（平台矩阵、WASM 硬约束、线程模型）、`docs/02`（C ABI + struct_size 版本化、零全局可变状态）、`docs/00` ADR-0016（WASM 一等）、ADR-0017（薄抽象、不抽象热路径）、ADR-0018（默认单线程）。
- **一句话**：只抽象有平台差异处、绝不抽象热路径；默认单线程使确定性成为默认属性，并行必须显式 opt-in。

**范围界定（重要）**：

| 属于本模块 | 不属于本模块（见对应 owner） |
|---|---|
| 单调/墙钟时钟；线程/互斥/条件变量**原语**与能力探测 | worker/job 池与任务调度 → `agent-executor`（05） |
| 日志 **sink 注册**与原始记录投递（底层写） | 结构化日志事件/指标/崩溃管线 → `agent-obs`（09） |
| 栈回溯**采集/解析打桩点** | 崩溃上下文与回放输入拼接 → `agent-obs`（09） |
| 模块加载**能力探测 + 底层 open/sym/close 原语** | 加载策略/registry/依赖拓扑/热重载 → `agent-host`（03） |
| 文件 IO | → VFS（`agent-vfs`，docs/04） |
| GPU 同步/后端 | → WebGPU，由 `agent-renderer`（15）统一（ADR-0025） |

> 与 `docs/00` ADR-0017 的 fs 项对齐：**文件系统归 VFS**，本模块不提供裸 fs，避免与 docs/04 重复。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（docs/02）。接口不完备不得进入实现。
> 全部类型为固定宽度 POD 或不透明句柄；**边界上无 C++ 类型、无异常、无 RTTI**。

### 2.1 公共头 `mq_platform.h`

```c
/* mq_platform.h —— 纯 C，C ABI。唯一导出入口：mq_module_query */
#ifndef MQ_PLATFORM_H
#define MQ_PLATFORM_H

#include <stdint.h>
#include <stddef.h>
#include "mq_core.h"   /* mq_allocator（POD vtable，docs/02）——仅此一个跨头依赖 */

#define MQ_PLATFORM_ABI_VERSION   1
#define MQ_PLATFORM_INTERFACE     "maque.platform.v1" /* 稳定字符串 ID（docs/02 §3） */

/* ---------- 错误模型：int32 返回码，0 = 成功；无跨边界异常 ---------- */
typedef enum mq_status {
    MQ_OK                =  0,
    MQ_ERR_INVALID_ARG   = -1,  /* 空指针 / 非法入参 */
    MQ_ERR_UNSUPPORTED   = -2,  /* 该平台无此能力，如 WASM(no-SAB) 建线程 */
    MQ_ERR_OUT_OF_MEMORY = -3,
    MQ_ERR_AGAIN         = -4,  /* 非阻塞操作暂不可用 */
    MQ_ERR_TIMEOUT       = -5,
    MQ_ERR_ABI_VERSION   = -6,  /* 主版本不兼容，拒绝加载 */
    MQ_ERR_IO            = -7,  /* module_open 底层失败 */
    MQ_ERR_NOT_FOUND     = -8,  /* module_symbol 未找到 */
    MQ_ERR_DESTROYED     = -9   /* 句柄已销毁 */
} mq_status;

/* ---------- 能力位（get_caps 输出，只读，创建后不变） ---------- */
typedef enum mq_cap_flags {
    MQ_CAP_THREADS      = 1u << 0,  /* 可创建真线程 */
    MQ_CAP_THREAD_POOL  = 1u << 1,  /* 有原生线程池（executor 可用） */
    MQ_CAP_TLS          = 1u << 2,  /* 线程局部存储可用 */
    MQ_CAP_MUTEX        = 1u << 3,
    MQ_CAP_CONDVAR      = 1u << 4,
    MQ_CAP_STACKTRACE   = 1u << 5,  /* 栈回溯可用（DWARF/sourcemap 未 strip） */
    MQ_CAP_WALL_CLOCK   = 1u << 6,  /* 墙钟可用（非确定，仅供日志/展示） */
    MQ_CAP_MODULE_LOAD  = 1u << 7   /* 支持运行时模块加载 */
} mq_cap_flags;

typedef struct mq_caps {
    uint32_t struct_size;   /* = sizeof(mq_caps) */
    uint32_t abi_version;   /* = MQ_PLATFORM_ABI_VERSION */
    uint32_t flags;         /* 位或 mq_cap_flags */
    uint32_t clock_freq_hz; /* 单调时钟分辨率（ticks/秒），0 = 未知 */
    uint32_t loader_kind;   /* mq_loader_kind */
    uint32_t reserved0;
} mq_caps;

/* ---------- 模块加载物理形态（逻辑边界不变，docs/05 §3） ---------- */
typedef enum mq_loader_kind {
    MQ_LOADER_UNKNOWN          = 0,
    MQ_LOADER_DLOPEN           = 1, /* 原生 .so + dlopen */
    MQ_LOADER_STATIC           = 2, /* 嵌入式静态链接 + 编译期模块集 */
    MQ_LOADER_WASM_INSTANTIATE = 3, /* 独立 .wasm 实例化 + 显式 imports */
    MQ_LOADER_WASM_SIDE_MODULE = 4  /* Emscripten SIDE_MODULE（可选） */
} mq_loader_kind;

/* ---------- 日志（本模块只做原始投递 + sink 注册，结构化归 obs-09） ---------- */
typedef enum mq_log_level {
    MQ_LOG_TRACE = 0, MQ_LOG_DEBUG = 1, MQ_LOG_INFO = 2,
    MQ_LOG_WARN  = 3, MQ_LOG_ERROR = 4, MQ_LOG_FATAL = 5, MQ_LOG_OFF = 6
} mq_log_level;

typedef struct mq_log_record {
    uint32_t      struct_size;   /* = sizeof(mq_log_record) */
    uint32_t      level;         /* mq_log_level */
    uint64_t      timestamp_ns;  /* platform monotonic_ns 采样，单调 */
    uint64_t      thread_token;  /* 0 = 主/模拟线程；否则线程标识 */
    const char*   domain;        /* UTF-8，可为 NULL */
    const char*   message;       /* UTF-8，可为 NULL；调用期内有效 */
    const char*   file;          /* UTF-8，可为 NULL */
    uint32_t      line;
    uint32_t      reserved0;
} mq_log_record;

typedef void (*mq_log_sink_fn)(void* user, const mq_log_record* rec);

/* ---------- 线程原语（默认不使用；见 §2.5 约定） ---------- */
typedef void (*mq_thread_fn)(void* user);

typedef struct mq_thread_desc {
    uint32_t       struct_size;   /* = sizeof(mq_thread_desc) */
    mq_thread_fn   entry;         /* 必填 */
    void*          user;          /* 透传给 entry */
    const char*    name;          /* UTF-8，可 NULL，用于调试/回溯 */
    uint32_t       stack_size;    /* 0 = 平台默认 */
    uint32_t       priority_hint; /* 0 = 默认；越小越优先，平台可忽略 */
} mq_thread_desc;

typedef struct mq_thread mq_thread;   /* 不透明句柄 */
typedef struct mq_mutex  mq_mutex;    /* 不透明句柄 */
typedef struct mq_condvar mq_condvar; /* 不透明句柄 */

/* ---------- 栈回溯打桩点（docs/05 §8） ---------- */
typedef struct mq_stacktrace_frame {
    uint32_t    struct_size;  /* = sizeof(mq_stacktrace_frame) */
    uint32_t    reserved0;
    const char* module;       /* UTF-8，可 NULL */
    const char* symbol;       /* UTF-8，可 NULL（未解析时为空） */
    const char* file;         /* UTF-8，可 NULL（需 line tables） */
    uint32_t    line;
    uint32_t    reserved1;
} mq_stacktrace_frame;

/* ---------- 创建配置 ---------- */
typedef struct mq_platform_config {
    uint32_t struct_size;          /* = sizeof(mq_platform_config) */
    uint32_t abi_version;          /* = MQ_PLATFORM_ABI_VERSION */
    uint8_t  request_threads;      /* 显式要求线程；0 = 即使支持也降级单线程 */
    uint8_t  request_stacktrace;
    uint16_t reserved0;
    uint32_t log_default_level;    /* mq_log_level；低于此级别的 log_emit 直接丢弃 */
} mq_platform_config;

/* ---------- 唯一导出的 vtable ---------- */
typedef struct mq_platform_v1 {
    uint32_t struct_size;   /* = sizeof(mq_platform_v1)；只增不改 */
    uint32_t abi_version;   /* 主版本不匹配即拒绝 */

    /* 生命周期：create 由传入 allocator 分配 self；destroy 释放自身与该实例所有句柄 */
    mq_status (*create)(void* host, const mq_allocator* alloc,
                        const mq_platform_config* cfg, void** out_self);
    void      (*destroy)(void* self);

    /* 能力探测：创建后只读，任意线程可调用 */
    void      (*get_caps)(void* self, mq_caps* out);

    /* 单调时钟：单调不减；分辨率见 caps.clock_freq_hz；任意线程可调用 */
    uint64_t  (*monotonic_ns)(void* self);
    /* 墙钟（UTC epoch ns）：非确定，仅供日志时间戳/展示；不支持返回 UNSUPPORTED */
    mq_status (*wall_clock_ns)(void* self, uint64_t* out_ns);
    /* 让出当前模拟线程（协作式调度点）；不支持返回 UNSUPPORTED 且不视为错误 */
    mq_status (*yield)(void* self);

    /* 线程原语：无 MQ_CAP_THREADS 时一律返回 MQ_ERR_UNSUPPORTED（docs/05 §4 WASM 降级） */
    mq_status (*thread_create)(void* self, const mq_thread_desc* d, mq_thread** out);
    mq_status (*thread_join)(void* self, mq_thread* t);
    void      (*thread_destroy)(void* self, mq_thread* t);
    uint64_t  (*thread_current_token)(void* self); /* 0 = 主/模拟线程；否则唯一标识 */

    mq_status (*mutex_create)(void* self, mq_mutex** out);
    mq_status (*mutex_lock)(void* self, mq_mutex* m);
    mq_status (*mutex_try_lock)(void* self, mq_mutex* m); /* 忙返回 MQ_ERR_AGAIN */
    mq_status (*mutex_unlock)(void* self, mq_mutex* m);
    void      (*mutex_destroy)(void* self, mq_mutex* m);

    mq_status (*condvar_create)(void* self, mq_condvar** out);
    /* timeout_ns: UINT64_MAX = 无限等待；到点返回 MQ_ERR_TIMEOUT；销毁后返回 DESTROYED */
    mq_status (*condvar_wait)(void* self, mq_condvar* cv, mq_mutex* m, uint64_t timeout_ns);
    mq_status (*condvar_signal)(void* self, mq_condvar* cv);
    mq_status (*condvar_broadcast)(void* self, mq_condvar* cv);
    void      (*condvar_destroy)(void* self, mq_condvar* cv);

    /* 模块加载探测 + 底层原语：策略归 host(03)，此处只做物理操作与能力上报 */
    mq_loader_kind (*module_loader_kind)(void* self);
    mq_status (*module_open)(void* self, const char* path_or_uri, void** out_handle);
    mq_status (*module_symbol)(void* self, void* handle, const char* name, void** out_fn);
    void      (*module_close)(void* self, void* handle);

    /* 日志 sink：fn=NULL 且 user=NULL 表示清除 sink（回到默认 stderr/console） */
    mq_status (*log_set_sink)(void* self, mq_log_sink_fn fn, void* user);
    void      (*log_emit)(void* self, const mq_log_record* rec); /* 低于默认级别则静默丢弃 */

    /* 栈回溯打桩点：capture 返回写入帧数（0 = 不支持）；resolve 填充可读帧 */
    uint32_t  (*stacktrace_capture)(void* self, void** out_frames, uint32_t max_frames);
    mq_status (*stacktrace_resolve)(void* self, void* frame, mq_stacktrace_frame* out);
} mq_platform_v1;

#endif /* MQ_PLATFORM_H */
```

### 2.2 公共类型清单

| 类型 | 形态 | 说明 |
|---|---|---|
| `mq_status` | enum(int32) | 错误码；0 成功，负值失败 |
| `mq_cap_flags` / `mq_caps` | enum + POD | 能力探测结果，创建后只读 |
| `mq_loader_kind` | enum | 模块加载物理形态 |
| `mq_log_level` / `mq_log_record` / `mq_log_sink_fn` | POD + 函数指针 | 原始日志记录与 sink |
| `mq_thread_desc` / `mq_thread_fn` | POD + 函数指针 | 线程创建描述 |
| `mq_thread` / `mq_mutex` / `mq_condvar` | **不透明句柄** | 由 platform 分配/释放；上层不得解引用 |
| `mq_stacktrace_frame` | POD | 解析后的栈帧 |
| `mq_platform_config` | POD | create 配置 |
| `mq_platform_v1` | vtable | 唯一操作面 |
| `mq_allocator` | POD vtable | 来自 `mq_core.h`（docs/02），创建时注入 |

### 2.3 操作面（全部函数语义）

| 函数 | 语义 / 参数 / 返回 / 错误 |
|---|---|
| `create(host, alloc, cfg, out_self)` | 建 platform 实例。`host` 透传不解释；`alloc` 必填，self 与全部句柄经它分配。错误：`INVALID_ARG`（alloc/cfg/out_self 空）、`ABI_VERSION`（cfg.abi_version 主版本不匹配）、`OUT_OF_MEMORY`。 |
| `destroy(self)` | 释放 self 及实例内所有存活句柄（线程需已 join）。幂等（NULL 安全）。 |
| `get_caps(self,out)` | 填 `mq_caps`。创建后不变，任意线程可调。`out` 空→静默无操作（探测类，不返回错误）。 |
| `monotonic_ns(self)` | 单调不减纳秒。**不保证**跨实例一致、不保证严格递增。 |
| `wall_clock_ns(self,out_ns)` | UTC epoch 纳秒。非确定；不支持→`UNSUPPORTED`。 |
| `yield(self)` | 协作式让出模拟线程。单线程/WASM 下可为 `UNSUPPORTED`（不视为错误）。 |
| `thread_create(self,d,out)` | 建线程，`entry(user)` 在新线程运行。无 `MQ_CAP_THREADS`→`UNSUPPORTED`；`d`/`entry` 空→`INVALID_ARG`。 |
| `thread_join(self,t)` | 阻塞等待线程结束。**禁止在模拟线程调用**（§2.5）。 |
| `thread_destroy(self,t)` | 释放线程句柄（须已 join）。 |
| `thread_current_token(self)` | 0=主/模拟线程；其它值为当前线程唯一 token（用于日志/断言）。 |
| `mutex_*` | 标准互斥。`try_lock` 忙→`AGAIN`；对已销毁句柄→`DESTROYED`。无 `MQ_CAP_MUTEX`→`UNSUPPORTED`。 |
| `condvar_*` | 条件变量。`wait` 超时→`TIMEOUT`；需配合 mutex。无 `MQ_CAP_CONDVAR`→`UNSUPPORTED`。 |
| `module_loader_kind(self)` | 返回本平台加载形态（决定 host 分支，docs/05 §3）。 |
| `module_open(self,path_or_uri,out)` | 打开模块物理体（原生路径/`res://`/wasm uri）。失败→`IO`/`UNSUPPORTED`。 |
| `module_symbol(self,handle,name,out_fn)` | 解析符号（通常 `mq_module_query`）。未找到→`NOT_FOUND`。 |
| `module_close(self,handle)` | 关闭模块句柄并释放其资源。 |
| `log_set_sink(self,fn,user)` | 注册原始日志 sink；`fn=NULL,user=NULL` 清除。返回 `OK`/`INVALID_ARG`。 |
| `log_emit(self,rec)` | 投递记录到 sink。`rec.level < log_default_level` 丢弃；无 sink 走默认 sink。`rec` 内容调用期内有效，sink 不得长期持有。 |
| `stacktrace_capture(self,frames,max)` | 捕获当前调用栈帧指针到调用者缓冲，返回写入数（0=不支持）。`max=0`→0。 |
| `stacktrace_resolve(self,frame,out)` | 把帧解析为 module/symbol/file/line（打桩点）；不可解析字段留 NULL。 |

### 2.4 版本化

- `struct_size` 语义（docs/02 §3）：**只增不改**，新版本一律在 vtable / 结构体**尾部追加**字段；旧调用方按自身 `struct_size` 截断使用。
- `abi_version` 校验：`create` 时若 `cfg.abi_version` 的**主版本**与 `MQ_PLATFORM_ABI_VERSION` 不同 → 返回 `MQ_ERR_ABI_VERSION`，并（由 host）产出**可读错误**（服务 A2）。
- 每个随 vtable 传递的 POD（`mq_caps`/`mq_log_record`/`mq_thread_desc`/`mq_stacktrace_frame`/`mq_platform_config`）首字段均为 `struct_size`，调用方写自身大小，被调方据此判断可读范围。
- 接口名稳定为 `"maque.platform.v1"`；未来不兼容升级走 `"maque.platform.v2"`（并行共存，不隐式替换）。

### 2.5 内存所有权 / 线程 / 确定性约定

**内存所有权**：

- `create` 必须收到 `mq_allocator*`；platform 的 self、所有句柄、内部缓冲**全部**经它分配，并用**同一 allocator** 释放（R5 分配者释放）。
- 传入的 `cfg`/`rec`/`out` 缓冲、`mq_thread_desc.user`、log 字符串**均归调用方**；platform 不释放、调用返回后不引用（`mq_log_record` 尤须注意：sink 只在回调期内可读）。
- 跨边界不出现 `std::string`/`vector`/`shared_ptr`；字符串一律 UTF-8 `const char*`（R6）。
- `module_open` 返回的 handle 归 platform 所有，由 `module_close` 释放。

**线程约定**：

- `get_caps` / `monotonic_ns` / `wall_clock_ns` / `thread_current_token` / `log_emit`：**任意线程可调用**。
- `log_set_sink`：**仅在启动期（尚无其它线程）调用**；运行期变更需调用方自行同步，本模块不保证并发安全。
- `thread_*` / `mutex_*` / `condvar_*`：本身线程安全；但对**同一句柄**的 destroy 与其它操作不得并发。
- **模拟线程永不阻塞**（docs/05 §4）：`thread_join`、`condvar_wait`（无限超时）、阻塞 IO **禁止**在模拟线程调用；违反属调用方缺陷。

**确定性约定**：

- 默认单线程（ADR-0018）⇒ 默认确定。**一旦使用线程原语，确定性责任转移给调用方**（docs/10）。
- `monotonic_ns` 是**唯一**被认可的物理时间源，但**模拟逻辑不得直接依赖它**：模拟走**固定步长 + 逻辑 tick 计数**（回放/快照对齐 docs/10）；`monotonic_ns` 仅用于真实时间累加与帧预算。
- `wall_clock_ns` **禁止**进入模拟逻辑；只允许进入日志/展示字段。
- 允许 `static const`/`constexpr`；**禁止**可变全局/函数内 static（docs/02 §5，多实例前提）。

## 3. 依赖与边界

- **可依赖**：
  - `mq_core.h` 的 `mq_allocator`（POD vtable，编译期契约，docs/02 §10）与固定宽度整数。
  - 平台系统调用/C 运行库（`clock_gettime`、`pthread`/`std::thread`、`dlopen`）——**且仅在本模块的 `.cpp` 内部**，不泄漏到头文件。
- **禁止**：
  - ❌ 依赖任何上层模块（scene/renderer/script…）或 executor/obs 的实现。
  - ❌ 在头文件暴露 OS 类型（`pthread_t`、`HANDLE`、`FILE*`、JS 类型）；一律不透明句柄。
  - ❌ 热路径过虚函数：时钟/日志由上层**在初始化期缓存**函数指针后直接调用，模拟内层不逐帧查 vtable。
  - ❌ 持有全局可变状态 / 单例（多实例由 instance context 承载）。
  - ❌ 跨边界裸指针语义（除调用方自有缓冲经参数传入）。
- **与其它模块的接缝**：
  - `03 module host`：消费 `module_loader_kind` 选择加载分支，消费 `module_open/symbol/close` 完成物理加载；**策略（依赖/版本/registry）归 host**。
  - `05 executor`：消费 `get_caps` 决定是否有真线程；有则基于 `thread_*`/`mutex_*`/`condvar_*` 实现 worker，无则走**同步降级 worker**。
  - `09 observability`：`log_set_sink` 注册的 sink 即 obs 的落点；obs 在其上构造结构化事件与崩溃上下文（含 `stacktrace_*`）。
  - `06 VFS`：与本模块**无直接依赖**；文件 IO 不经 platform。

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**独立**：仅依赖本模块 + `mq_platform.h` + **fake 平台实现**，不依赖真实 OS、不依赖其它模块。
> 所有用例通过 **fake** 驱动；真实后端（native/wasm/嵌入式）只做阶段 4 的冒烟集成。

### 4.1 Fake 平台实现（测试替身核心）

`test/fake_platform.h/.cpp`：实现 `mq_platform_v1` 全部函数指针，行为**完全受测**、**无真实系统调用**：

- **可编程时钟**：内部 `uint64_t ticks`；`monotonic_ns` 返回 `ticks`；测试用 `fake_advance(ns)` 推进；`clock_freq_hz` 可设。
- **可配置能力**：`fake_set_caps(flags)` 决定 `MQ_CAP_THREADS`/`STACKTRACE`/`MUTEX`…；未置位的线程函数返回 `UNSUPPORTED`。
- **内存日志 sink**：`fake_log_records[]` 环形缓冲，记录每次 `log_emit` 的 level/domain/timestamp/token/message 拷贝；可查询计数与顺序。
- **确定性栈回溯**：`stacktrace_capture` 写入预置的 N 个伪帧指针（如 `(void*)0x1000+i`）；`stacktrace_resolve` 返回确定性 `"fake.module"`/`"fn_i"`/`"fake.c:LINE`。
- **可注入 OOM**：fake allocator 可设定在第 K 次 alloc 失败，返回 NULL。
- **加载器可切换**：`module_loader_kind` 返回 `DLOPEN`/`STATIC`/`WASM_*`；`module_symbol` 可按预置表命中/未命中。
- **句柄世代**：fake 内部维护 `destroyed` 标记，用于验证 `DESTROYED` 路径。
- **线程原语（可选真实现）**：仅用于验证 `thread_create` 成功路径与 token 非 0；不用于确定性用例。

辅助替身：`fake_allocator`（计数 + 可注入失败）、`fake_clock`、`fake_sink`（可作为独立于 platform 的观测点校验）。

### 4.2 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| 1 | create/destroy 正常 | 合法 alloc+cfg | `OK`，out_self 非空；destroy 后 alloc 计数 alloc==free | 正常 |
| 2 | create 空 allocator | alloc=NULL | `INVALID_ARG`，无分配 | 错误 |
| 3 | create 空 cfg | cfg=NULL | `INVALID_ARG` | 错误 |
| 4 | create 空 out_self | out_self=NULL | `INVALID_ARG` | 错误 |
| 5 | create 版本不匹配 | cfg.abi_version=2 | `ABI_VERSION` | 版本 |
| 6 | create OOM | fake alloc 第1次失败 | `OUT_OF_MEMORY`，无泄漏 | 错误/内存 |
| 7 | get_caps 反映配置 | threads+stacktrace 置位 | flags 含对应位，loader_kind 相符 | 正常 |
| 8 | get_caps out=NULL | out=NULL | 无崩溃、无操作 | 边界 |
| 9 | 单调性：多次调用不减 | 调 monotonic_ns×100 | 后值 ≥ 前值 | 正常 |
| 10 | 单调性：advance 后严格增 | advance(1000ns) | 新值 == 旧值+1000 | 正常 |
| 11 | 单调性：墙钟回拨不受影响 | wall_clock 人为回拨 | monotonic_ns 仍不减 | 边界 |
| 12 | wall_clock 不支持 | caps 去 WALL_CLOCK | `UNSUPPORTED` | 错误 |
| 13 | wall_clock 支持 | 正常 | `OK` 且 out_ns 非 0 | 正常 |
| 14 | yield 降级不报错 | no-thread fake | `UNSUPPORTED`（视为合法） | 边界 |
| 15 | thread_create 无线程 | caps 无 THREADS | `UNSUPPORTED` | 降级 |
| 16 | thread_create 空 entry | d.entry=NULL | `INVALID_ARG` | 错误 |
| 17 | thread token 主线程 | 未建线程 | `thread_current_token`==0 | 正常 |
| 18 | mutex 生命周期 | create/lock/unlock/destroy | 全部 `OK` | 正常 |
| 19 | mutex try_lock 忙 | 已 lock 再 try | `AGAIN` | 边界 |
| 20 | mutex destroy 后 lock | 已 destroy | `DESTROYED` | 错误 |
| 21 | mutex 无能力 | caps 无 MUTEX | `UNSUPPORTED` | 降级 |
| 22 | condvar 超时 | 无 signal，timeout=1ms | `TIMEOUT` | 边界 |
| 23 | log_set_sink+emit | 注册 sink，emit INFO | sink 收到 1 条，字段一致 | 正常 |
| 24 | log 低于默认级别 | default=WARN，emit DEBUG | sink 收 0 条 | 边界 |
| 25 | log 无 sink | 未注册，emit | 走默认 sink，不崩溃 | 边界 |
| 26 | log 清除 sink | set(NULL,NULL) 后 emit | 回默认，不复用旧 sink | 边界 |
| 27 | log 时间戳单调 | 连续 emit 并 advance | records 时间戳非减 | 正常 |
| 28 | log 空 rec | rec=NULL | 无崩溃（静默） | 边界 |
| 29 | stacktrace_capture 非空 | 支持栈回溯 | 返回 >0 帧 | 正常 |
| 30 | stacktrace_capture max=0 | max=0 | 返回 0 | 边界 |
| 31 | stacktrace 不支持 | caps 去 STACKTRACE | 返回 0 | 降级 |
| 32 | stacktrace_resolve | 有效帧 | module/symbol 非空，line>0 | 正常 |
| 33 | loader_kind 探测 | 分别设 3 种 | 逐字返回，无副作用 | 正常 |
| 34 | module_symbol 未命中 | 名字不存在 | `NOT_FOUND` | 错误 |
| 35 | module_open/close 配对 | 合法 uri | `OK`；close 后 alloc 平衡 | 正常 |
| 36 | struct_size 截断兼容 | 旧结构体（较小 size） | 只读前 N 字段，不越界 | 版本 |
| 37 | vtable 尾部追加兼容 | 截断 vtable 只取前 6 项 | 前 6 项可调用 | 版本 |
| 38 | 内存所有权 | 传入调用方缓冲 | platform 不释放、不改写缓冲外 | 内存 |
| 39 | 多实例隔离 | create 两个 platform | 各自 caps/时钟/句柄互不影响 | 正常 |
| 40 | destroy 幂等 | destroy(NULL) | 无崩溃 | 边界 |

### 4.3 游戏场景用例（真实玩法驱动）

| # | 场景 | 驱动 | 期望 |
|---|------|------|------|
| G1 | **无线程环境跑固定步长循环** | caps 无 THREADS（模拟 WASM no-SAB）；执行 600 帧、`dt=16666667ns` 固定步；每帧 `monotonic_ns += dt`；accumulate→tick | 恰好推进 600 个逻辑 tick；帧 checksum 与预期一致；不调用任何阻塞原语 |
| G2 | **崩溃采集闭环** | 模拟"玩家移动"逻辑中触发断言；`stacktrace_capture`+`resolve`，再 `log_emit(FATAL)` | 栈帧 ≥1 且可解析；log sink 收到 1 条 FATAL，含 RNG seed/帧号占位字段 |
| G3 | **模块加载分平台分支** | 分别用 DLOPEN/STATIC/WASM_INSTANTIATE 的 fake | `module_loader_kind` 驱动 host 分支；WASM 路径不尝试 `dlopen` |
| G4 | **exe 日志时间线可读** | 关卡加载→生成实体→击杀，各阶段 emit | sink 记录顺序与阶段一致、时间戳单调，可重建时间线 |
| G5 | **worker 显式 opt-in** | caps 有 THREADS，测试方显式建 worker 跑一段纯函数 job | job 结果正确；**同一模拟内容在单线程路径得到相同结果**（对照） |

### 4.4 确定性用例

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| D1 | 固定步长回放一致 | 同一 fake 时钟推进序列 + 同一输入 → 跑两遍 | 每帧 checksum 完全一致 |
| D2 | 时钟不污染模拟 | 模拟内**不读** `monotonic_ns`；改 fake 时钟起点 | 模拟输出不变 |
| D3 | 日志顺序确定 | 单线程顺序 emit N 条 | sink 记录顺序与调用顺序严格一致 |
| D4 | caps 不影响模拟结果 | 在 threads on/off 两配置跑同一单线程模拟 | 输出一致（证明默认单线程确定） |

### 4.5 测试替身与独立单测保障

- **测试替身**：`fake_platform`（核心）、`fake_allocator`（计数/OOM 注入）、`fake_clock`、`fake_sink`。
- **独立单测保障**：
  - 测试目标只 link `mq_platform.h` + `fake_platform.cpp` + 测试框架（如 Catch2/doctest）；**不 link** 真实 native/wasm 后端，**不 link** 任何其它模块。
  - 通过编译开关 `MQ_PLATFORM_TEST_FAKE=1` 保证无真实系统调用参与断言（真实后端仅冒烟测试，单列）。
  - CI 三配置矩阵（`threads on/off × stacktrace on/off × loader 三态`）全部跑 fake，确保**平台差异**在无真实 OS 下被覆盖。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 — 冻结接口头**：落地 `include/mq_platform.h`（§2.1 全文）+ `MQ_PLATFORM_ABI_VERSION=1`；写 `static_assert` 校验各 POD 大小与字段偏移；提交接口评审（对照 docs/02 R1–R8）。**接口不完备不得进入阶段 1。**
2. **阶段 1 — 写测试骨架（红）**：
   - 建 `test/fake_platform.*` 与 `fake_allocator`；
   - 按 §4.2 表逐条写断言，全部编译但失败（红）；先覆盖 1–8、9–11、23–27（生命周期/时钟/日志）。
3. **阶段 2 — 最小实现到绿**：
   - 实现 **fake 驱动下的接口**（先只做 fake 后端，验证语义）；
   - 实现 **native 后端**（`clock_gettime`/`std::thread`/`dlopen`）到 §4.2 核心用例全绿；
   - 明确 `create` 的 ABI 校验与 allocator 全路径。
4. **阶段 3 — 补边界/错误/确定性 + 重构**：
   - 补齐 §4.2 剩余（14–22 线程原语、29–38 栈回溯/版本/内存）+ §4.3 场景 + §4.4 确定性；
   - 实现 **WASM 后端**（no-thread 降级 `UNSUPPORTED`、`fetch`/JS console sink、JS 栈拼接打桩）；实现**嵌入式后端**（静态模块集 + `MQ_LOADER_STATIC`）；
   - 抽取平台无关的 `fake` 与真实后端共用核心（仅 OS 层分文件：`platform_native.cpp` / `platform_wasm.cpp` / `platform_embedded.cpp`），去重、检查零全局可变状态。
5. **阶段 4 — 三平台冒烟 + 性能/内存达标**：
   - 在 native / wasm（Node+浏览器）/ 嵌入式目标各跑一次**冒烟**（真实后端，非 fake）；
   - 校验 `monotonic_ns` 无系统调用级开销、`log_emit` 热路径零分配、create/destroy 内存平衡；
   - 对接上游 fake：以 `agent-host`/`agent-executor` 的 fake 替换真实模块，验证 platform 接口被正确消费（本模块只提供 fake consumer 断言，不实现对方）。

## 6. 验收标准（DoD）

- **接口冻结**：`mq_platform.h` 全函数（clock/thread/stacktrace/log/module）签名与语义冻结；`struct_size`+`abi_version` 版本化被截断用例（#36/#37）验证通过；被至少一个上游 fake（host/executor）依赖通过。
- **单测覆盖率 ≥ 80%**（行覆盖），§4.2 全部 40 条 + §4.3 5 条 + §4.4 4 条通过。
- **三平台降级正确**：WASM 无线程时 `thread_*` 返回 `UNSUPPORTED` 且 `MQ_CAP_THREADS` 清零（#15/G1）；栈回溯不可用时 `capture` 返回 0 不崩溃（#31）。
- **确定性用例 D1–D4 通过**：固定步长循环输出 checksum 一致，且 caps/时钟起点不影响模拟结果。
- **性能/内存预算**：`monotonic_ns` 单次 ≤ ~50ns（vDSO/`clock_gettime` 量级）；`log_emit` 在低于阈值时零分配、单次 ≤ ~100ns（不含 sink）；`create`→`destroy` 后 allocator `alloc==free`（无泄漏）。
- **边界纪律**：无跨模块实现依赖；无全局可变状态/单例（`.clang-tidy` 检查通过）；头文件不暴露 OS 类型；全部入口 `noexcept` 等价（纯 C）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **三平台行为分叉**（docs/05 §10） | 公共下限 + §4 平台配置矩阵在 fake 下全覆盖；CI 对 native/wasm/嵌入式三目标构建 + 冒烟；分叉点集中在 `platform_<os>.cpp`，接口层不含 `#ifdef` 行为差异 |
| WASM 无线程拖慢 executor | 本模块只上报 `MQ_CAP_THREADS=0` 并降级 `UNSUPPORTED`；同步 worker 由 executor(05) 实现，platform 不阻塞 |
| 单调时钟回拨/分辨率差异 | `monotonic_ns` 契约明确"单调不减"（#9–#11）；分辨率经 `caps.clock_freq_hz` 上报；模拟不直接依赖时钟（D2） |
| 栈回溯信息缺失（strip/sourcemap） | `stacktrace_capture` 可返回 0、`resolve` 允许字段为 NULL（打桩点）；docs/05 §8 保留 DWARF/JS 栈拼接，具体拼接归 obs(09) |
| struct_size 演进破坏兼容 | 只增不改（§2.4）+ 截断用例（#36/#37）防回归 |
| 线程原语被误用在模拟线程 | 契约显式禁止阻塞（§2.5）；D4 证明默认单线程确定；可选加运行期 debug 断言（"模拟线程调用 join/无限 wait"） |
| 多实例被共享状态破坏 | 全部状态在 instance 句柄；禁止可变 static（§2.5）；多实例隔离用例 #39 |
| 头文件泄漏 OS 类型导致 ABI 脆弱 | 头文件仅 POD + 不透明句柄（§2.2）；OS 类型仅存在于 `.cpp` |

## 8. Owner agent

- **角色标识**：`agent-platform`
- **职责边界**：仅负责本模块的接口（`mq_platform.h`）、测试（fake 平台 + 单测）、以及 native/wasm/嵌入式三个薄后端实现；不修改其它模块，不实现 worker 调度（归 `agent-executor`）、不实现结构化日志/崩溃管线（归 `agent-obs`）、不实现加载策略/registry（归 `agent-host`）、不实现文件 IO（归 `agent-vfs`）。