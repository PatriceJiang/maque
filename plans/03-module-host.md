# 03 · module host（模块加载 / C ABI / registry）

> Owner agent：`agent-host`　上游文档：`docs/02`（主）、`docs/03`、`docs/05`、`docs/00`
> 状态：接口待冻结 → 测试红 → 实现绿 → 验收

## 1. 模块概述

- **职责**：
  1. 定义并冻结**唯一的跨 `.so` 边界形态**：`mq_module_query` 单入口 + C vtable + `struct_size`/`abi_version`（ADR-0013 / docs/02 §2–§3）。
  2. 实现模块**生命周期**：`discover → load(dlopen) → query → 版本/依赖校验 → init(host) → run → shutdown → unload`（docs/02 §4）。
  3. 实现 **registry**：按 `(name, version)` 查找接口、注册接口、服务定位（上层经 registry 取依赖，而非硬链接，docs/02 §4、docs/03 §2.2）。
  4. 实现**多实例**：每次实例化一个 host context（allocator / logger / registry / executor），零全局可变状态（ADR-0014、docs/02 §5）。
  5. 实现**热重载**：序列化状态 → 换 `.so` → 反序列化，且只在帧边界进行（docs/02 §6）。
  6. 提供**物理加载分平台**能力：原生 `dlopen` / 嵌入式静态链接 / WASM 实例化，**逻辑边界不变**（docs/05 §3），本模块只暴露统一的 `mq_module_host` 抽象。
- **所在层**：基础层（阶段 A 地基，`math → core → module host → platform → executor`，见 README §4）。
- **一句话**：module host 是"插件即 `.so`"的**装载器与注册表**，把跨模块的一切交互收敛为**版本化 C vtable**，并保证多实例隔离与热重载可行。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（docs/02）。本节为**逻辑边界**，三个平台（原生/嵌入式/WASM）共享同一组头文件；差异只在 `mq_module_host` 的**实现**里（docs/05 §3）。接口不完备不得进入实现。

### 2.1 公共头一：`mq_module.h`（模块侧契约，纯 C）

模块作者包含此头，导出查询入口，声明自己的 vtable 形态。

```c
/* mq_module.h —— 纯 C，-fvisibility=hidden 环境下唯一导出 */
#ifndef MQ_MODULE_H
#define MQ_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQ_MODULE_ABI_VERSION 1u          /* 边界协议主版本，不匹配即拒绝 */

/* 符号可见性：仅查询入口导出 */
#if defined(_WIN32)
#  define MQ_EXPORT __declspec(dllexport)
#else
#  define MQ_EXPORT __attribute__((visibility("default")))
#endif

/* ---------- 稳定服务原语（由 host 提供，随 host context 注入） ---------- */

typedef struct mq_allocator {
    uint32_t struct_size;
    uint32_t abi_version;
    void* (*alloc)(void* self, size_t size, size_t align);  /* 失败返回 NULL */
    void  (*free)(void* self, void* ptr);                   /* ptr==NULL 必须容忍 */
    void* (*realloc)(void* self, void* ptr, size_t size, size_t align);
} mq_allocator;                                              /* struct_size=sizeof, abi=1 */

typedef enum {                                            /* 日志级别，稳定枚举 */
    MQ_LOG_TRACE = 0, MQ_LOG_DEBUG = 1, MQ_LOG_INFO  = 2,
    MQ_LOG_WARN  = 3, MQ_LOG_ERROR = 4, MQ_LOG_OFF   = 5
} mq_log_level;

typedef struct mq_logger {
    uint32_t struct_size;
    uint32_t abi_version;
    void (*log)(void* self, mq_log_level lvl, const char* domain, const char* utf8_msg);
} mq_logger;

typedef struct mq_clock {           /* 单调时钟，供模块取时间；不保证跨实例一致 */
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t (*now_ticks)(void* self);
    uint64_t (*ticks_per_second)(void* self);
} mq_clock;

/* ---------- host context：init 时注入，模块全部状态挂在这里 ---------- */
/* 不透明：模块只经下述访问器使用，禁止解引用内部布局 */
typedef struct mq_host_context mq_host_context;

/* 访问器（host 实现）。所有返回的 vtable 指针生命周期 = host context 生命周期。 */
const mq_allocator* mq_host_allocator(const mq_host_context* ctx);
const mq_logger*    mq_host_logger(const mq_host_context* ctx);
const mq_clock*     mq_host_clock(const mq_host_context* ctx);

/* 服务定位：模块向上层请求依赖（替代硬链接其它 .so，docs/02 §4） */
const void* mq_host_get_interface(const mq_host_context* ctx,
                                  const char* iface_name, uint32_t min_version);

/* 模块自注册：在 init 中把自己的接口描述符注册进 host registry */
typedef struct mq_interface_desc {
    uint32_t struct_size;
    const char* name;          /* 稳定字符串 ID，如 "maque.renderer.v1" */
    uint32_t version;          /* 主版本 */
    const void* vtable;        /* 指向模块的 C vtable */
} mq_interface_desc;

int mq_host_register_interface(mq_host_context* ctx, const mq_interface_desc* desc);
/* 返回 MQ_OK=0 / MQ_ERR_DUP / MQ_ERR_INVAL / MQ_ERR_NOMEM；同一 (name,ver) 重复即 MQ_ERR_DUP */

/* ---------- 模块 vtable 形态（约定，非强制类型）：每个接口自带版本头 ---------- */
/* 例：
typedef struct mq_example_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    int  (*init)(mq_host_context* ctx, void** out_self);
    void (*tick)(void* self, double dt);
    void (*shutdown)(void* self);
} mq_example_v1;
*/

/* ---------- 唯一导出入口 ---------- */
/* 按接口名+最低版本返回 vtable 指针；不支持/版本不匹配返回 NULL。
 * 可被多次调用（幂等）：同一 name/version 返回同一指针。
 * 此函数无副作用、不分配、可安全跨线程只读调用。 */
MQ_EXPORT const void* mq_module_query(const char* iface_name, uint32_t min_version);

/* 可选：模块元信息（依赖声明），缺省视为无依赖 */
typedef struct mq_module_meta {
    uint32_t struct_size;
    const char* module_name;
    uint32_t module_version;
    const char* const* deps;       /* 依赖的接口名数组（min_version 见下） */
    const uint32_t* dep_min_versions;
    uint32_t dep_count;
} mq_module_meta;

/* 若模块提供，host 用它做依赖预校验；不提供则运行时按需 query。 */
MQ_EXPORT const mq_module_meta* mq_module_meta_query(void);

#ifdef __cplusplus
}
#endif
#endif /* MQ_MODULE_H */
```

### 2.2 公共头二：`mq_host.h`（宿主侧契约，纯 C）

引擎/宿主包含此头，创建实例、加载/卸载模块、查 registry。

```c
/* mq_host.h —— 纯 C */
#ifndef MQ_HOST_H
#define MQ_HOST_H

#include "mq_module.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQ_HOST_ABI_VERSION 1u

/* ---------- 错误模型：稳定错误码，无异常越界（R2） ---------- */
typedef enum {
    MQ_OK                 =  0,
    MQ_ERR_INVAL          = -1,   /* 参数非法（NULL / 空名 / align 非 2 的幂） */
    MQ_ERR_NOMEM          = -2,   /* 分配失败 */
    MQ_ERR_NOT_FOUND      = -3,   /* 模块文件 / 接口 不存在 */
    MQ_ERR_ABI_VERSION    = -4,   /* abi_version 主版本不兼容 */
    MQ_ERR_STRUCT_SIZE    = -5,   /* struct_size 小于 header 或畸形 */
    MQ_ERR_DEP_MISSING    = -6,   /* 依赖接口未满足 */
    MQ_ERR_DUP            = -7,   /* 重复注册 (name, version) */
    MQ_ERR_LOAD           = -8,   /* dlopen / 实例化 失败 */
    MQ_ERR_SYMBOL         = -9,   /* 缺少 mq_module_query 符号 */
    MQ_ERR_STATE          = -10,  /* 生命周期状态非法（如未 init 就 run） */
    MQ_ERR_RELOAD_BOUNDARY= -11,  /* 不在帧边界调用热重载 */
    MQ_ERR_PLATFORM       = -12,  /* 平台不支持（如嵌入式 dlopen 被禁用） */
    MQ_ERR_PANIC          = -13   /* 模块内 noexcept 违规 / 崩溃被捕获 */
} mq_result;

const char* mq_result_str(mq_result r);   /* 稳定英文串，供 A2 日志；不释放 */

/* ---------- 实例（多实例：一实例一全局状态容器） ---------- */
typedef struct mq_host mq_host;           /* 不透明，实例私有，绝不共享 */

typedef struct mq_host_desc {
    uint32_t struct_size;
    uint32_t abi_version;
    const mq_allocator* allocator;        /* 必填；host 全部分配经此 */
    const mq_logger*    logger;           /* 可空（用内置 sink） */
    const mq_clock*     clock;            /* 可空（用默认单调时钟） */
    uint32_t            instance_id;      /* 用于句柄跨实例校验（docs/02 §5） */
} mq_host_desc;

mq_result mq_host_create(const mq_host_desc* desc, mq_host** out);   /* out 归调用方 */
void      mq_host_destroy(mq_host* host);                            /* 停全部模块后释放 */

/* ---------- 加载 / 查询 / 卸载 ---------- */
typedef struct mq_module_desc {
    uint32_t struct_size;
    const char* name;        /* 逻辑名，如 "renderer" */
    const char* path;        /* 物理位置：原生=.so 路径；嵌入=编译期符号名；WASM=wasm url/tag */
    uint32_t    min_version; /* 请求的最低接口版本 */
} mq_module_desc;

/* 完整生命周期由 host 内部驱动；loaded_handle 是模块的 host 侧句柄（slot 索引）。 */
mq_result mq_host_load(mq_host* host, const mq_module_desc* desc,
                       uint64_t* out_module_id);

/* 已加载模块的运行期入口（帧驱动） */
mq_result mq_host_run(mq_host* host, uint64_t module_id, double dt);
mq_result mq_host_shutdown(mq_host* host, uint64_t module_id);   /* 调 shutdown 回调但不卸载 */
mq_result mq_host_unload(mq_host* host, uint64_t module_id);     /* shutdown(如未) + 卸载 .so */

/* ---------- registry：按 (name, version) 查找与枚举 ---------- */
mq_result mq_host_find_interface(mq_host* host, const char* iface_name,
                                 uint32_t min_version, const void** out_vtable);

typedef void (*mq_iface_visit_fn)(void* user,
                                  const char* name, uint32_t version,
                                  const void* vtable);
mq_result mq_host_visit_interfaces(mq_host* host, mq_iface_visit_fn fn, void* user);

/* ---------- 热重载（仅帧边界） ---------- */
typedef struct mq_reload_desc {
    uint32_t struct_size;
    uint64_t module_id;      /* 要重载的模块 */
    const char* new_path;    /* 新的 .so 路径 */
    uint32_t at_frame;       /* 调用方声明当前帧；host 校验为帧边界 */
} mq_reload_desc;

/* 内部：serialize → unload 旧 → load 新 → deserialize。失败则回滚到旧模块。 */
mq_result mq_host_reload(mq_host* host, const mq_reload_desc* desc);

#ifdef __cplusplus
}
#endif
#endif /* MQ_HOST_H */
```

### 2.3 类型清单

| 类型 | 归属 | 语义 | 所有权 |
|------|------|------|--------|
| `mq_allocator` | `mq_module.h` | 堆分配 vtable，`struct_size`+`abi_version` | host 拥有，生命周期≥实例 |
| `mq_logger` | `mq_module.h` | 结构化日志 sink | 同上 |
| `mq_clock` | `mq_module.h` | 单调时钟 | 同上 |
| `mq_host_context` | `mq_module.h` | 不透明实例上下文，模块持有其 const 视图 | host 拥有 |
| `mq_interface_desc` | `mq_module.h` | 自注册接口描述符 | 调用方在栈上传入，host 拷贝 name |
| `mq_module_meta` | `mq_module.h` | 模块依赖元信息 | 模块静态常量 |
| `mq_host` | `mq_host.h` | 引擎实例（不透明） | `mq_host_create` 分配，`mq_host_destroy` 释放 |
| `mq_host_desc` | `mq_host.h` | 实例构造参数 | 调用方栈 |
| `mq_module_desc` | `mq_host.h` | 单模块加载参数 | 调用方栈 |
| `mq_reload_desc` | `mq_host.h` | 热重载参数 | 调用方栈 |
| `mq_result` | `mq_host.h` | 错误码枚举 | 值语义 |
| `uint64_t module_id` | `mq_host.h` | 模块 host 句柄（slot + generation 打包） | 值语义，卸载后失效 |

### 2.4 操作面（全部函数语义）

**模块侧（`mq_module.h`）**

| 函数 | 参数 | 返回 | 语义 / 前置 | 副作用 |
|------|------|------|-------------|--------|
| `mq_module_query(name, min_version)` | 稳定接口名；请求最低主版本 | `const void*`（vtable）或 `NULL` | 幂等、无分配、可跨线程只读；`name==NULL` 或未知返回 `NULL` | 无 |
| `mq_module_meta_query()` | 无 | `const mq_module_meta*` 或 `NULL` | 可选导出；`NULL` 视为无依赖 | 无 |
| `mq_host_allocator/logger/clock(ctx)` | host context | 对应 vtable 或 `NULL` | ctx 非空；返回指针有效期=ctx | 无 |
| `mq_host_get_interface(ctx, name, ver)` | ctx；接口名；最低版本 | vtable 或 `NULL` | 服务定位；等价于向 registry 查询 | 无 |
| `mq_host_register_interface(ctx, desc)` | ctx；描述符 | `mq_result` | 在 init 内调用；重复 `(name,ver)` → `MQ_ERR_DUP` | 写 registry |

**宿主侧（`mq_host.h`）**

| 函数 | 参数 | 返回 | 语义 |
|------|------|------|------|
| `mq_host_create` | desc | `MQ_OK` + `*out` | 建立实例：构造 allocator 包装、logger、clock、空 registry、空模块表；分配 host 自身用 desc->allocator |
| `mq_host_destroy` | host | 无 | 逆序 `shutdown`+`unload` 所有模块，释放 registry 与 host |
| `mq_host_load` | host, desc | `MQ_OK` + `module_id` | 执行 discover→物理加载→`query`→校验→`init(host_ctx)`；任一失败回滚并释放已加载资源 |
| `mq_host_run` | host, id, dt | `MQ_OK` | 调用模块 `tick`（若 vtable 暴露）；状态必须为 RUNNING |
| `mq_host_shutdown` | host, id | `MQ_OK` | 调用模块 `shutdown`；模块转为 SHUTDOWN，仍可 unload |
| `mq_host_unload` | host, id | `MQ_OK` | 若未 shutdown 先调；`dlclose`；从 registry 摘除该模块注册的接口并**作废所有缓存指针** |
| `mq_host_find_interface` | host, name, min_version | `MQ_OK` + vtable | registry 查找；版本需 `>=`；找不到 `MQ_ERR_NOT_FOUND` |
| `mq_host_visit_interfaces` | host, fn, user | `MQ_OK` | 枚举全部已注册接口；用于 A1 自省 |
| `mq_host_reload` | host, desc | `MQ_OK` | 帧边界校验→序列化→卸载→加载新→反序列化；失败回滚旧模块 |
| `mq_result_str` | result | `const char*` | 稳定英文串，静态存储 |

### 2.5 错误模型

- 规则 **R2**：所有边界函数 `noexcept`，**无异常跨边界**；错误以 `mq_result` 返回码表达。
- 错误码在 `mq_result` 中**只增不改语义**（与 ABI 版本同理）。
- 模块内 `init/query` 若违反 `noexcept`（抛异常逃逸），host 以 `MQ_ERR_PANIC` 捕获并隔离该模块（`try/catch` 仅**只读包裹入口**，不用于正常控制流；被捕获记 ERROR 日志 + 卸载）。
- 可读性：每个错误码映射稳定英文串（`mq_result_str`），供 A2 结构化日志消费，**不依赖具体语言**。
- 无 `std::expected` 跨边界（R6/ADR-0013）；`std::expected` 仅限模块**内部**实现。

### 2.6 版本化规则（前向兼容）

- **两套版本**：① 协议版本 `MQ_MODULE_ABI_VERSION` / `MQ_HOST_ABI_VERSION`；② 每个 vtable 自带的 `abi_version`。
- `struct_size`：**只增不改**。新版本在表尾追加函数指针，旧调用方按自身 `struct_size` 截断使用；host 加载时校验 `struct_size >= offsetof(末尾必需字段)+sizeof(...)`，否则 `MQ_ERR_STRUCT_SIZE`。
- `abi_version`：**主版本不兼容即拒绝**（`MQ_ERR_ABI_VERSION`），不做静默降级。`mq_module_query(name, min_version)` 中 `min_version` 是**请求方**要求的最低主版本；模块实际主版本 `< min_version` 返回 `NULL`。
- 接口名 = 稳定字符串 + 版本后缀，如 `"maque.renderer.v1"`；主版本升级即换名（`v2`），旧名可并存（供渐进迁移）。
- 兼容矩阵：

  | 模块 abi_version | 请求 min_version | 结果 |
  |---|---|---|
  | 1 | 1 | 通过 |
  | 1 | 2 | `MQ_ERR_ABI_VERSION`（或 query 返回 NULL） |
  | 2 | 1 | 通过（高版本向后服务低请求） |
  | 0 / 畸形 | any | `MQ_ERR_ABI_VERSION` |

### 2.7 内存所有权

- 铁律 **R5**：**分配者释放**。
- `mq_host_create` 分配 `mq_host`（经 `desc->allocator`），`mq_host_destroy` 释放。
- `mq_module_query` 返回的 vtable 由**模块**拥有（静态/模块内分配），host **不释放**。
- `mq_host_load` 返回的 `module_id` 为值类型句柄（含 generation），无内存所有权。
- `mq_host_register_interface` **拷贝** `desc->name`（host 分配），调用方保留原始 buffer 所有权。
- 跨边界**禁止** `std::string`/`std::vector`/`shared_ptr`/裸 `new`（R6）；字符串一律 UTF-8 `const char*`，长度用 `size_t`。
- 无任何**跨 `.so` 的 `free`**：模块释放自己分配的内存，或全部经传入 allocator（R5）。

### 2.8 线程 / 确定性

- **默认单线程**（ADR-0018 / docs/05 §4）：host 的 load/run/unload/reload 均假定**调用方串行**；host 内部不加锁（零可变全局状态使其天然无需锁）。
- `mq_module_query` 可**跨线程只读**（无副作用、无分配）。
- `mq_host_register_interface`/registry 写仅在 `load` 期间发生，属串行生命周期操作。
- **确定性**：加载顺序、接口枚举顺序、release 顺序必须**确定**（按 name 字典序 + 稳定槽位），保证同一模块集合 → 同一行为，服务 A3（回放）。
- 热重载点**必须在帧边界**（`mq_host_reload` 校验 `at_frame`），与 GC / 快照对齐（docs/02 §6）。

## 3. 依赖与边界

- **可依赖**：
  - `core/data`（POD 契约、句柄 `{index,generation}` 的**布局定义**，编译期共享头）。
  - C 标准库（`dlopen`/`dlsym`/`dlclose`、`pthread`/等价物按需）、平台薄层接口（由 `platform` 模块提供 `mq_dynlib_*` 与 `mq_thread_*` 的**函数声明**，host 只依赖其头，不依赖实现）。
  - 本模块自有实现（registry 表、模块表、host context）。
- **禁止**（违反即接口审不通过）：
  - ❌ 依赖上层行为模块（scene/renderer/script…）的完整实现；对它们的依赖一律经 **fake / registry 查询**。
  - ❌ 持有**全局可变状态** / 单例 / 函数内 `static` 可变（ADR-0014）。允许 `static const` / `constexpr`。
  - ❌ 暴露裸指针跨边界（除 host 注入的 vtable 指针，其为只读且生命周期受控）。
  - ❌ 跨边界传 STL / 异常 / RTTI（R1–R3、R6）。
  - ❌ 在 registry 热路径上做分配 / 字符串比较以外的重活（`find_interface` 走**哈希表**，O(1)）。
  - ❌ 让模块缓存别的模块函数指针跨重载使用（docs/02 §6）——host 在 unload 后**作废**相关 vtable 引用并在 debug 构建下毒化 slot。
- **与其它模块的接缝**：
  - `platform`：host 通过 `mq_dynlib_open/sym/close` 抽象做物理加载；平台无关部分（registry、生命周期状态机）完全可单测。
  - `core/data`：`module_id`/句柄复用其 `{index,generation}` 打包约定。
  - `executor`：host 不内建线程；需要异步加载时由上层经 executor 组合（本期 host 提供同步接口，异步为后续扩展点）。
  - 所有行为模块（scene/renderer/…）：它们**只**通过 `mq_module.h` 与 registry 与 host 交互。

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**独立**：仅依赖本模块 + `mq_module.h`/`mq_host.h` + 平台薄层 stub，上游用 **fake 模块**。
> 测试框架建议 Catch2（若仓库已有则复用；否则用最小自写断言宏，保持零重型依赖）。测试目标目录：`modules/host/tests/`。

### 4.1 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| T01 | create/destroy 空实例 | 合法 desc | `MQ_OK`；无泄漏；`destroy` 幂等安全 | 正常 |
| T02 | create 非法 desc | allocator=NULL / struct_size=0 / abi_version=0 | `MQ_ERR_INVAL` / `MQ_ERR_STRUCT_SIZE` / `MQ_ERR_ABI_VERSION` | 边界 |
| T03 | 加载合法 fake 模块 | fake_a.so + init 正常 | `MQ_OK` + 有效 module_id；接口已注册 | 正常 |
| T04 | 加载不存在的路径 | path="/no/such.so" | `MQ_ERR_LOAD`；无残留 | 错误 |
| T05 | 加载缺少 `mq_module_query` 符号 | fake_nosym.so | `MQ_ERR_SYMBOL` | 错误 |
| T06 | `mq_module_query` 未知接口名 | name="no.such.iface" | `NULL`；`find_interface` → `MQ_ERR_NOT_FOUND` | 错误 |
| T07 | 版本不匹配拒绝 | fake 声明 abi_version=2，请求 min=1 通过；请求 min=3 拒绝 | 前者 `MQ_OK`，后者 `MQ_ERR_ABI_VERSION` | 版本兼容 |
| T08 | `struct_size` 过小 | fake 伪造过小 struct_size | `MQ_ERR_STRUCT_SIZE` | 版本兼容 |
| T09 | 依赖缺失 | fake 依赖 `"maque.foo.v1"` 未加载 | `MQ_ERR_DEP_MISSING`；模块不进入 RUNNING | 错误 |
| T10 | 依赖满足（拓扑/晚加载） | 先加载 foo，再加载依赖它的 fake | 均 `MQ_OK`；顺序无关（服务定位） | 正常 |
| T11 | 重复注册同一 (name,ver) | 两模块注册同名同版本；或同模块重复注册 | 第二个 `MQ_ERR_DUP` | 错误 |
| T12 | 不同版本注册同名接口 | 注册 `X.v1` 与 `X.v2` | 均 `MQ_OK`；`find_interface("X",1)` 命中 v1 | 版本兼容 |
| T13 | `find_interface` 版本选择 | 注册 v1、v2；查询 min=2 | 返回 v2 | 版本兼容 |
| T14 | 生命周期状态非法 | 未 load 就 run；shutdown 两次；unload 已卸载 id | `MQ_ERR_STATE` / 幂等 `MQ_OK` / `MQ_ERR_STATE` | 错误 |
| T15 | 卸载后接口摘除 | load 后 unload | `find_interface` 该接口 → `MQ_ERR_NOT_FOUND` | 正常 |
| T16 | 句柄失效防悬垂 | unload 后拿旧 module_id 调 run | `MQ_ERR_STATE`（generation 不匹配） | 边界 |
| T17 | 分配经 allocator | 计数 allocator；加载/注册/卸载 | alloc 次数 == free 次数；无泄漏 | 内存所有权 |
| T18 | `realloc` / `free(NULL)` 容忍 | 计数 allocator 走 free(NULL) | 不崩、不计数错误 | 边界 |
| T19 | `mq_result_str` 全覆盖 | 遍历全部错误码 | 均返回非空稳定串 | 正常 |
| T20 | `visit_interfaces` 枚举稳定序 | 注册多接口 | 按 name 字典序；数量正确 | 确定性 |
| T21 | `query` 可跨线程只读 | 多线程并发 query | 无数据竞争（TSan 干净） | 线程 |
| T22 | noexcept 违规隔离 | fake 在 init 抛异常 | host 捕获→`MQ_ERR_PANIC`→模块被卸载 | 错误 |
| T23 | 热重载成功 | fake_a.so → fake_a_v2.so，序列化状态 | `MQ_OK`；状态经反序列化保持一致 | 热重载 |
| T24 | 热重载非帧边界拒绝 | `at_frame` 与当前帧不一致 | `MQ_ERR_RELOAD_BOUNDARY`；旧模块仍可用 | 热重载 |
| T25 | 热重载失败回滚 | 新 .so 损坏 | 返回错误；**旧模块恢复可用**（状态未丢） | 热重载 |
| T26 | 多实例隔离：接口不串 | 实例A加载 fake_a，实例B未加载 | B 查 A 的接口 → `MQ_ERR_NOT_FOUND` | 多实例 |
| T27 | 多实例隔离：状态不串 | 两实例各自加载同一 fake，各改状态 | 互不可见；各自 shutdown 后对方不受影响 | 多实例 |
| T28 | 多实例隔离：registry 独立 | 两实例重复注册同名接口 | 两个实例**各自成功**（不触发跨实例 `MQ_ERR_DUP`） | 多实例 |
| T29 | 跨实例句柄拒绝 | 实例A 的 module_id 传给实例B | `MQ_ERR_STATE`（instance_id 校验） | 多实例 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S1 | 启动期加载渲染插件 | 创建实例 → `load("renderer")` → 场景 `init` 请求其接口 → 首次 `run(dt)` | `MQ_OK`；渲染接口在 registry 可见；一帧无错误 |
| S2 | 运行时加载/卸载渲染插件（热插拔） | 游戏运行中 `load("fx_plugin")` → `run` 数帧 → `unload` | 加载后效果出现；卸载后无残留接口、无泄漏、游戏继续 |
| S3 | 运行时热重载渲染插件（调参迭代） | 帧边界 `reload("renderer", new.so)` | 状态（如已绑定材质）保留；画面对应更新；失败则回滚不影响游戏 |
| S4 | 缺失依赖的关卡加载失败但可恢复 | 关卡声明需要 physics 插件但未随包提供 | `MQ_ERR_DEP_MISSING` + 明确日志；引擎不崩溃，回退到无物理模式 |
| S5 | 多开：同机两个游戏实例 | 两个 `mq_host` 各自加载自身插件集，独立运行 | 完全隔离；A 崩溃/卸载不影响 B；`visit_interfaces` 各自独立 |
| S6 | 版本回滚（降级运行） | 游戏包带 v2 插件，引擎只请求 min=v1 | 加载成功，走 v1 兼容子集；不因未来版本拒绝 |

### 4.3 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| D1 | 同模块集、同加载顺序 → `visit_interfaces` 输出一致 | 枚举序恒定（name 字典序） |
| D2 | 同模块集、不同加载顺序 → 最终 registry 状态一致（服务定位解耦顺序） | 接口集合与行为一致 |
| D3 | 崩溃/重放：记录加载序列 + RNG seed，重放 → 模块行为 checksum 一致 | 复现一致（A3） |

### 4.4 测试替身（fake / stub）

- **`fake_renderer.so`**：导出 `mq_module_query`，提供 `mq_example_v1` vtable（`init/tick/shutdown` 写入可观测标记：计数、状态 blob）。
- **`fake_renderer_v2.so`**：同接口、`abi_version=2`，`tick` 行为可区分（验证热重载换实现）。
- **`fake_nosym.so`**：故意**不**导出 `mq_module_query`。
- **`fake_badver.so`**：`abi_version=0` 或畸形 `struct_size`。
- **`fake_needs_dep.so`**：`mq_module_meta` 声明依赖 `"maque.foo.v1"`。
- **`fake_throws.so`**：`init` 抛异常，验证 `MQ_ERR_PANIC` 隔离。
- **`fake_foo.so`**：提供依赖目标的空接口。
- **平台 stub `mq_dynlib`**：内存内"假 `.so`"注册表，允许无真实文件系统跑单测（也可用真实 `dlopen` 编译 fake 为真 `.so` 双轨）。
- **计数 allocator**：验证分配/释放配平与所有权。

### 4.5 独立单测保障

- host 单测**只**链接：本模块目标 + `core/data` 头 + 平台 stub。**不链接**任何行为模块实现。
- 所有"上层模块"以 fake `.so` 或 stub vtable 出现；依赖关系经 registry 注入而非链接。
- 断言不依赖其它模块内部状态，只依赖 `mq_host.h` 暴露的接口（`find_interface`/`visit`/返回值）。
- CI 中在**同一进程**内跑多实例隔离用例（T26–T29、S5），确保无隐藏全局可变状态（配合 `.clang-tidy` 静态检查 + TSan）。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0（接口冻结）**：定稿 `mq_module.h` / `mq_host.h`；确定 `MQ_MODULE_ABI_VERSION=1`、全部错误码与所有权规则；`static_assert` 校验各 vtable 头布局；提交接口评审。**产出：两个头 + 编译期断言，无实现。**
2. **阶段 1（测试骨架，红）**：
   - 建 `modules/host/tests/` 与 CMake 测试目标；写 fake 模块源码与构建。
   - 写 T01–T29 的测试骨架与断言（此时全部失败/未链接）。
   - 写游戏场景 S1–S6 的驱动骨架（用 fake 渲染插件）。
   - **产出：可编译的失败测试集。**
3. **阶段 2（最小实现到绿）**：
   - 平台薄层 `mq_dynlib`（原生 `dlopen` 实现 + 内存 stub）。
   - `mq_host_create/destroy`、`load`（discover→open→query→校验→init）、`unload`、`find_interface`、`register_interface`。
   - 通过 T01–T16、T19、T26–T29。
   - **产出：T03 起加载/查询/注册/隔离用例绿。**
4. **阶段 3（边界 / 错误 / 确定性 / 热重载）**：
   - 补齐 T02、T05、T08、T17–T18、T20–T22、T23–T25。
   - 实现 `visit_interfaces` 稳定序（D1/D2）、句柄 generation 校验（T16）。
   - 实现热重载序列化→换库→反序列化 + 失败回滚 + 帧边界校验。
   - **产出：全部单测绿，TSan 干净。**
5. **阶段 4（场景集成 / 平台分支 / 达标）**：
   - 跑通 S1–S6 场景用例（运行时加载/卸载/热重载渲染插件）。
   - 接入 `platform` 抽象的嵌入式（静态模块集）与 WASM（实例化）分支**接口占位**，验证逻辑边界不变（用 stub 验证状态机一致）。
   - 性能/内存预算实测（见 §6），补文档。
   - **产出：DoD 全绿。**

## 6. 验收标准（DoD）

- 接口冻结：`mq_module.h` / `mq_host.h` 定稿，被上游 fake 模块成功依赖编译。
- 单测覆盖率 **≥ 85%**（host 实现），其中错误路径与版本兼容路径 **≥ 90%**。
- 全部单测用例 T01–T29 通过；游戏场景 S1–S6 通过；确定性 D1–D3 通过。
- **多实例隔离**用例（T26–T29、S5）通过；TSan 无数据竞争；`.clang-tidy` 无"全局可变状态"告警。
- 无跨模块实现依赖：host 测试二进制不链接任何行为模块。
- **性能预算**：`find_interface` 单次查询 p99 ≤ 200ns（哈希查表）；加载一个 fake 模块（含 `dlopen`）冷启动 ≤ 2ms；`load → unload` 全程分配释放配平（T17 零泄漏）。
- **内存预算**：空 host 实例 ≤ 4KiB；每加载模块的 host 侧元数据 ≤ 512B（不含模块自身）。
- 热重载：成功保留状态；失败**必然回滚**旧模块可用（T25）。
- 三平台逻辑边界一致：同一测试状态机在原生/嵌入式stub/WASM stub 下行为一致。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **ABI 脆弱**（布局变化、编译器差异） | `struct_size` 只增不改 + `abi_version` 主版本拒绝；`static_assert(offsetof/sizeof)` 冻结布局；CI 跨编译器（GCC/Clang）构建校验 |
| **符号重复**（两模块静态链接同一第三方库） | 强制 `-fvisibility=hidden` + 只导出 `mq_module_query`/`mq_module_meta_query`（R8）；链接作用域隔离；CI 检查导出符号白名单 |
| **嵌入式 `dlopen` 限制** | 物理加载降级为**静态链接 + 编译期模块集**（docs/05 §3b）；`mq_module_host` 抽象屏蔽差异；host 状态机与 registry 逻辑复用同一实现 |
| **WASM 无动态加载** | 优先"静态链接单 wasm"或"独立 `.wasm` 实例化 + 显式 imports"（docs/05 §3c）；SIDE_MODULE 作为可选、成熟度评估后再启用 |
| **热重载状态迁移不完整** | 状态必须全部可序列化（ADR-0011）；禁止模块隐藏指针状态；无法序列化即**拒绝重载**并回滚 |
| **跨重载缓存函数指针** | 约定 + debug 构建毒化失活 slot；unload 后作废引用；文档禁令（docs/02 §6） |
| **全局可变状态泄漏** | `.clang-tidy` 自定义规则 + 代码评审 + 同进程多实例测试（T26–T29）三重强制 |
| **noexcept 违规逃逸** | 入口只读 `try/catch` 包裹 → `MQ_ERR_PANIC` + 卸载 + ERROR 日志；不用于正常控制流 |
| **接口名拼写漂移** | 稳定字符串 ID 集中定义（`mq_interface_names.h`），CI 交叉检查模块与 host 使用一致 |
| **查询性能退化** | registry 用开放寻址哈希表，`find_interface` O(1)；基准测试设 p99 门限 |

## 8. Owner agent

- **角色标识**：`agent-host`
- **职责边界**：仅负责本模块（module host）的接口头（`mq_module.h` / `mq_host.h`）、测试与实现；**不修改**其它模块。
- **不必**实现 `platform` 的 `mq_dynlib_*` 真实后端——只依赖其声明；本模块自带内存 stub 用于独立单测。
- 跨模块接缝变更（如需要新平台原语）经接口评审，通知对应 owner（`agent-platform`、`agent-core`）。
