# 05 · executor（任务调度 / reactor）

> 上游：`../docs/00`（ADR-0010/0018、A3）、`../docs/05 §4`（线程模型）、`../docs/10`（D6/D7）、`../docs/02`（C ABI/句柄）、`../docs/07`（allocator/确定性）。
> 本模块提供：**默认单线程**的执行器 + **显式 opt-in** 的 worker/job + **内部 reactor 接口** + **C++23 coroutine 作者层桥接**。
> 范式：**接口 + 测试先行**。接口冻结前不进入实现。

## 1. 模块概述

- **职责**：
  1. 管理任务执行的**事件循环**（reactor）：定时器、IO 完成、外部事件（文档 04 的异步 Provider 完成回调）。
  2. 提供 **coroutine 作者层**：C++23 `co_await` 的 `mq::task<T>`、定时器/IO awaitable、`spawn/fork` 桥接（ADR-0010）。
  3. 提供**显式并行**：`mq_worker_create()` / `mq_job_submit()` / 等待 / 取消；worker 间**不共享可变内存**，仅消息/句柄通信。
  4. 保证**模拟线程永不阻塞**：绝不在模拟线程上等待 job 或做阻塞 IO；`run_until_idle`/`drain` 只推进已就绪事件。
  5. **确定性契约**：默认单线程下任务顺序确定；一旦提交 job 到 worker，**确定性责任转移给调用方**（文档 05 §4、文档 10 D6）。
  6. 平台降级：**无线程 WASM** 下 worker 退化为主线程**排队执行** worker（API 不变，上层无需条件编译）。
- **所在层**：运行时（地基阶段 A；被 VFS / persistence / asset / script 等依赖）。
- **上游文档**：docs/05 §4、docs/10（D6/D7）、docs/02、docs/07、docs/00 ADR-0010/0018。
- **一句话**：**单线程确定性为默认，并行是显式申请；coroutine 是语法、reactor 是引擎、libuv 是首选实现。**

### 模块范围与明确边界

| 做 | 不做 |
|----|------|
| reactor 事件循环 + 定时器 + IO/外部完成投递 | 不实现具体 IO 协议（由 VFS Provider 负责，见 docs/04） |
| worker/job 的生命周期、提交、等待、取消 | 不做线程池以外的 SIMD/GPU 并行 |
| coroutine `task<T>` / awaitable 桥接到 reactor | 不把 coroutine 接入模拟帧内（帧内无异步，D7） |
| fake reactor 供单测与嵌入式 | 不绑定 libuv 于公共 ABI（实现可替换） |

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（docs/02）。公共头为**纯 C**，不含 `std::`。
> coroutine 作者层是 **C++ 头（编译期契约）**，通过 `mq_executor.h` 暴露的 C 回调桥接到 C ABI，绝不跨 `.so` 边界传 C++ 类型。

### 2.1 公共头：`include/maque/mq_executor.h`（纯 C）

```c
/* mq_executor.h —— 纯 C ABI，版本化 vtable */
#ifndef MAQUE_MQ_EXECUTOR_H
#define MAQUE_MQ_EXECUTOR_H

#include <stddef.h>
#include <stdint.h>

#define MQ_EXECUTOR_ABI_VERSION 1   /* 主版本不匹配即拒绝 */
#define MQ_EXECUTOR_IFACE_NAME   "maque.executor.v1"

/* ---- 句柄（不透明，带世代，见 docs/07 §3） ---- */
typedef struct mq_worker_h { uint32_t index; uint32_t generation; } mq_worker_h;
typedef struct mq_job_h    { uint32_t index; uint32_t generation; } mq_job_h;
typedef struct mq_reactor_h{ uint32_t index; uint32_t generation; } mq_reactor_h;
typedef struct mq_timer_h  { uint32_t index; uint32_t generation; } mq_timer_h;

#define MQ_HANDLE_NONE ((uint32_t)0xFFFFFFFFu)

/* ---- 错误模型：无异常越界；返回码 + 可选错误出参（A2） ---- */
typedef enum mq_executor_status {
    MQ_EXEC_OK              = 0,
    MQ_EXEC_ERR_INVALID     = 1,   /* 空指针/非法参数 */
    MQ_EXEC_ERR_STALE_HANDLE= 2,   /* 世代不匹配（悬垂） */
    MQ_EXEC_ERR_NO_WORKER   = 3,   /* 该平台不支持真线程，见 worker_mode */
    MQ_EXEC_ERR_WOULD_BLOCK = 4,   /* 在模拟线程上请求阻塞 → 违约（D6） */
    MQ_EXEC_ERR_CAPACITY    = 5,   /* 队列/池满 */
    MQ_EXEC_ERR_CANCELLED   = 6,
    MQ_EXEC_ERR_ABI_VERSION = 7,   /* 主版本不兼容 */
    MQ_EXEC_ERR_SHUTDOWN    = 8,
} mq_executor_status;

/* ---- 版本/模式 POD ---- */
typedef enum mq_worker_mode {
    MQ_WORKER_MODE_REAL          = 0, /* 原生真线程/线程池 */
    MQ_WORKER_MODE_MAIN_QUEUED   = 1, /* WASM 无线程：job 在主线程排队执行 */
    MQ_WORKER_MODE_UNAVAILABLE   = 2, /* 明确不支持（返回 _ERR_NO_WORKER） */
} mq_worker_mode;

typedef enum mq_job_flags {
    MQ_JOB_FLAG_NONE      = 0,
    MQ_JOB_FLAG_CANCELLABLE = 1u << 0, /* 允许 mq_job_cancel */
} mq_job_flags;

/* job 完成回调：在 reactor 线程（默认即模拟线程）被调用；必须轻量、不阻塞。 */
typedef void (*mq_job_fn)(void* user, mq_executor_status status, void* result);

/* job 必须封装为「输入 blob → 输出 blob」，worker 间不共享可变内存（docs/05 §4）。
   result 所有权：worker 分配由 job 归还；见 §2.5。 */
typedef struct mq_job_desc {
    uint32_t      struct_size;   /* = sizeof(mq_job_desc) */
    uint32_t      flags;
    mq_job_fn     on_done;       /* 可为 NULL（仅记状态） */
    const void*   args;          /* 只读输入，跨 worker 复制由提交方保证 */
    size_t        args_size;
    void*         user;
    const char*   debug_name;    /* 供 A2 指标/日志 */
} mq_job_desc;

typedef struct mq_executor_config {
    uint32_t      struct_size;    /* = sizeof(mq_executor_config) */
    uint32_t      abi_version;    /* 必须 = MQ_EXECUTOR_ABI_VERSION */
    mq_allocator* allocator;      /* 见 docs/07 §2，分配者释放 */
    int           worker_threads; /* 0 = 全部降级为 MAIN_QUEUED；>0 = 申请真线程数 */
    void*         host;           /* host context，用于 logger/registry（不透明） */
} mq_executor_config;

/* ---- vtable：完整操作面，只增不改 ---- */
typedef struct mq_executor_v1 {
    uint32_t struct_size;   /* = sizeof(mq_executor_v1) */
    uint32_t abi_version;   /* 主版本不匹配即拒绝加载 */

    /* --- 生命周期 --- */
    mq_executor_status (*create)(const mq_executor_config* cfg, void** out_exec);
    void               (*destroy)(void* exec);       /* 停止并释放；未完成 job 报 CANCELLED */

    /* --- worker / job --- */
    mq_worker_mode     (*worker_query_mode)(void* exec);
    mq_executor_status (*worker_create)(void* exec, int threads, mq_worker_h* out_w);
    void               (*worker_destroy)(void* exec, mq_worker_h w); /* 等待在跑 job 收尾 */
    mq_executor_status (*job_submit)(void* exec, mq_worker_h w,
                                     const mq_job_desc* desc, mq_job_h* out_j);
    mq_executor_status (*job_status)(void* exec, mq_job_h j, mq_executor_status* out);
    mq_executor_status (*job_cancel)(void* exec, mq_job_h j);  /* 尽力而为；见 §2.3 */
    mq_executor_status (*job_wait)(void* exec, mq_job_h j, void** out_result); /* 禁在模拟线程 */

    /* --- reactor：事件循环 / 定时器 / 外部完成投递 --- */
    mq_executor_status (*reactor_step)(void* exec, int max_events, int* out_ran);
    /* 推进至无就绪事件；在模拟线程上是 drain（非阻塞），只跑已就绪者 */
    mq_executor_status (*reactor_drain)(void* exec);
    mq_executor_status (*timer_add)(void* exec, uint64_t delay_ns, int repeat,
                                    mq_job_fn cb, void* user, mq_timer_h* out_t);
    mq_executor_status (*timer_cancel)(void* exec, mq_timer_h t);
    /* 供 VFS/外部线程投递完成事件到 reactor（线程安全入队，不执行） */
    mq_executor_status (*post)(void* exec, mq_job_fn cb, void* user);

    /* --- coroutine 桥接（C 侧原语；C++ 头在 §2.6 封装） --- */
    /* 注册一个「恢复协程」回调：当定时器/事件就绪时由 reactor 调用 */
    mq_executor_status (*set_resume_hook)(void* exec, void (*hook)(void* coro, void* ev));
    void*              (*current_thread_tag)(void* exec); /* 判断是否在模拟线程 */

    /* --- 观测（A2；关日志行为不变） --- */
    mq_executor_status (*stats)(void* exec, void* out_stats); /* 由 09 定义的结构 */
} mq_executor_v1;

/* 唯一导出的模块入口（docs/02 §3） */
void* mq_module_query(const char* interface_name, uint32_t min_version);

#endif /* MAQUE_MQ_EXECUTOR_H */
```

### 2.2 公开类型清单（全部 POD / 句柄）

| 类型 | 种类 | 说明 |
|------|------|------|
| `mq_worker_h` / `mq_job_h` / `mq_reactor_h` / `mq_timer_h` | 句柄 `{index,generation}` | 不透明、世代防悬垂；跨 `.so` 安全（docs/07 §3） |
| `mq_executor_status` | 枚举 | 错误码；等价 `std::expected` 的 C 表达 |
| `mq_worker_mode` | 枚举 | `REAL / MAIN_QUEUED / UNAVAILABLE`，WASM 降级语义 |
| `mq_job_flags` | 位标志 | 目前仅 `CANCELLABLE` |
| `mq_job_desc` / `mq_executor_config` | POD（含 `struct_size`） | 参数结构，前向兼容 |
| `mq_job_fn` | 函数指针 | 完成回调，**在 reactor 线程**执行 |
| `mq_executor_v1` | vtable | 唯一接口表，末尾追加式演进 |
| `mq_allocator` | POD | 复用 docs/02 的统一 allocator |

### 2.3 操作面语义（完整）

| 函数 | 语义 | 参数 / 返回 | 关键约束 |
|------|------|-------------|----------|
| `create` | 建 executor + 默认 reactor；建 `worker_threads` 个线程 | `cfg` 只读；`out_exec` 输出 | `abi_version` 不符 → `ERR_ABI_VERSION`；`worker_threads=0` 全降级 |
| `destroy` | 停循环、取消未完成 job、释放 | 幂等 | 未完成 job 的 `on_done` 以 `CANCELLED` 回调（若循环可跑） |
| `worker_query_mode` | 报告本平台能力 | 返回枚举 | **上层据此决定是否需要条件逻辑**（文档 05 §4） |
| `worker_create` | 显式建 worker | `threads` 建议值 | 无真线程 → 返回 `MAIN_QUEUED` worker（不是错误） |
| `worker_destroy` | 停止并回收 worker | — | 先排空再回收；期间不再接受 `job_submit` |
| `job_submit` | 提交 job（复制 `args` 进入 worker 私有区） | 输入 blob → 输出 blob | **worker 间不共享可变内存**；`MAIN_QUEUED` 时入主线程队列 |
| `job_status` | 查询状态 | 输出到 `out` | 已回收的 job → `OK` 表示已完成 |
| `job_cancel` | 取消（尽力） | — | 未开始 → 直接取消；已跑 → 置取消位，job 需自检 |
| `job_wait` | **阻塞**等待 | 输出 result | **在模拟线程调用返回 `ERR_WOULD_BLOCK`**（D6）；仅允许非模拟线程 |
| `reactor_step` | 推进至多 `max_events` 个就绪事件 | 输出实际数 | **非阻塞**；模拟帧边界调用 |
| `reactor_drain` | 推进所有**已就绪**事件 | — | 不等待未来事件（不阻塞模拟线程） |
| `timer_add/cancel` | 定时器 | 延迟/重复 | 定时器回调算一次「事件」，同样非阻塞 |
| `post` | 从外部线程投递完成事件 | — | **线程安全入队**，不在此执行；由 reactor 取出运行 |
| `set_resume_hook` | coroutine 恢复钩子 | — | 由 C++ 层 §2.6 使用 |
| `current_thread_tag` | 判定当前线程 | — | coroutine/上层据此防违约 |
| `stats` | 观测指标 | — | 关日志/指标行为不变（P0） |

### 2.4 错误模型

- 无 C++ 异常跨边界（docs/02 R2）；所有入口 `noexcept`，错误以 `mq_executor_status` 返回。
- C++ 作者层用 `std::expected<T, mq_executor_status>` 包装 C 调用（ADR-0012），**不抛出**。
- 句柄世代不匹配 → `ERR_STALE_HANDLE`；调试构建额外断言（docs/07 §11）。
- 模拟线程违约（阻塞/帧内等待）→ `ERR_WOULD_BLOCK`，并在 debug 构建打结构化日志（A2）。

### 2.5 内存所有权

- **分配者释放**（docs/02 R5 / docs/07 §2）：句柄槽表、worker 队列、job 私有 args 副本均用 `cfg->allocator` 分配，由 executor 在 `destroy`/完成时释放。
- `job_submit` 对 `args` 做**深拷贝**进 worker 私有区（`args_size` 字节），避免跨 worker 共享可变内存。
- `job_wait` 返回的 `result` 所有权归调用方，调用方用同一 allocator 释放（README 约定），或由 `mq_job_desc` 的归还钩子处理。
- 桥接 coroutine 时，帧状态由 executor 提供 arena（frame/scratch 类，docs/07 §2），避免逐帧 `new`。

### 2.6 coroutine 作者层（C++23 头，编译期契约）

> 位于 `include/maque/executor/task.hpp`，**仅编译期**，通过 §2.1 的 C 原语与 reactor 交互；**不跨 `.so` 边界**。

```cpp
namespace mq {

// 惰性任务：co_await 时挂到所属 executor 的 reactor
template <class T = void> struct task {
    struct promise_type {
        mq_executor_handle exec;               // 所属 executor
        std::expected<T, mq_executor_status> value;
        std::coroutine_handle<> continuation;  // 上层续体（对称转移，无栈分配续体）
        task get_return_object();
        std::suspend_always initial_suspend(); // 惰性
        auto final_suspend() noexcept;         // 挂起并对称恢复 continuation
        void return_value(T v);
        void unhandled_exception();            // 转 status，不逃逸
    };
};

// awaitable：定时器到点后在 reactor 上恢复
struct sleep_aw {
    uint64_t ns;
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> h); // 经 set_resume_hook 注册
    void await_resume() const;
};

// 到非模拟线程执行并回投结果（确定性责任转移给调用方）
template <class F> struct offload_aw { /* job_submit + 完成后 post 回 reactor */ };

} // namespace mq
```

桥接约定（冻结）：
- `task<T>` 为**惰性**，需 `exec.spawn(task)` 才被 reactor 调度；`spawn` 在模拟线程上注册协程帧，后续仅在事件就绪时恢复。
- **帧内无异步（D7）**：`spawn` 的协程**不得**在模拟帧内被恢复；恢复只发生在帧边界的 `reactor_drain` 或非模拟线程 `post` 回投之后的 `step`。
- `offload_aw` 走 `job_submit`，其 `on_done` 通过 `post` 把结果投回 reactor，再恢复协程 → **不阻塞模拟线程**。
- 协程帧分配经 executor 的 frame arena；无栈协程续体用对称转移，避免堆分配增长（风险见 §7）。

### 2.7 版本化

- `struct_size` **只增不改**：新版本在 `mq_executor_v1` 表尾追加函数指针；旧调用方按自身 `struct_size` 截断使用（docs/02 §3）。
- `abi_version` 主版本不匹配 → `mq_module_query` 拒绝并回可读错误（A2）。
- 接口名稳定字符串：`"maque.executor.v1"`。

### 2.8 线程 / 确定性约定（本模块核心契约）

| 场景 | 约定 |
|------|------|
| 默认（`worker_threads=0`） | 纯单线程事件循环；**顺序确定**：事件按入队序号（FIFO）+ 定时器按 `(deadline_ns, seq)` 稳定排序 |
| `job_submit` 到 `MAIN_QUEUED` | 入 FIFO 队列，由 `reactor_step/drain` 按序执行 → 仍确定 |
| `job_submit` 到 `REAL` | 完成顺序**不确定** → **确定性责任转移给调用方**（docs/05 §4、docs/10 D6）；本模块不保证 |
| 模拟线程 | **永不被本模块阻塞**；`job_wait` 在模拟线程返回 `ERR_WOULD_BLOCK` |
| 帧内 | 无异步：`reactor_step` 只跑当前就绪事件，不引入新的等待 |
| worker 数据 | 只能消息/句柄通信；**不得**读写模拟状态（docs/10 D6） |

## 3. 依赖与边界

- **可依赖**：
  - `platform`（docs/05）的**线程原语、单调时钟**（仅经 thin 抽象，供 libuv 后端使用）；
  - `core`（句柄世代、allocator、容器）；
  - 外部：**libuv**（首选 reactor 后端）；嵌入式可用内置 **fake reactor**。
- **禁止**：
  - 不得依赖上层（scene/renderer/script/vfs 实现）——VFS **反向**通过 `post` 投递完成事件，本模块不 `#include` VFS；
  - 热路径不过多虚函数（C ABI 边界已是粗粒度，见 docs/02 §7；reactor 内循环为具体实现，非多态）；
  - 不得持有**全局可变状态**（ADR-0014）：所有状态在 `create` 返回的 exec context 内；
  - 不暴露裸指针（对外仅句柄）。
- **与其它模块的接缝**：
  - **VFS（docs/04 §6）**：Provider 返回 awaitable → 完成时调用 `executor->post()` 投递到 reactor；**绝不**在模拟帧内触发 IO。
  - **coroutine 作者层（script/docs/14）**：脚本层复用 `task<T>`；GC 仅帧边界（ADR-0026），与本模块帧边界对齐。
  - **observability（docs/09）**：`stats` 输出 job 队列深度、事件延迟、取消数；关指标行为不变（P0）。
  - **确定性（docs/10）**：默认单线程保证 L1；CI 回放场景核对事件顺序 checksum。

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**独立**：仅依赖本模块 + 公共接口；IO 上游用 **fake reactor**（手动推进事件），不依赖 libuv、不依赖真实 VFS。

### 4.1 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| U01 | create/destroy 基本生命周期 | 合法 cfg | `OK`；destroy 幂等 | 正常 |
| U02 | abi_version 不匹配 | cfg.abi_version=99 | `ERR_ABI_VERSION` | 版本 |
| U03 | create 空指针 | cfg=NULL | `ERR_INVALID` | 错误 |
| U04 | struct_size 截断使用 | 旧 struct_size 的 vtable | 不越界读，正常返回 | 版本兼容 |
| U05 | worker_query_mode：无线程平台 | 模拟 `NO_THREADS` | 返回 `MAIN_QUEUED` | 边界 |
| U06 | worker_create 无真线程降级 | threads=4 但能力=无 | `MAIN_QUEUED` worker（非错误） | 边界/降级 |
| U07 | job_submit 后 job_status 反映完成 | 单 job | 完成后 `OK` | 正常 |
| U08 | job args 深拷贝隔离 | 提交后立即改原 buffer | worker 看到原始值 | 内存/隔离 |
| U09 | 悬垂 job 句柄 | destroy worker 后用旧句柄 | `ERR_STALE_HANDLE` | 错误/世代 |
| U10 | job_cancel 未开始 | 队列中 job | `CANCELLED` 回调 | 正常 |
| U11 | job_cancel 正在跑 | 已取出的 job | 尽力取消，状态一致 | 边界 |
| U12 | 队列满 | capacity 边界 | `ERR_CAPACITY` | 错误 |
| U13 | timer_add/delay 到点 | fake reactor 推进 | 恰在 deadline 触发一次 | 正常 |
| U14 | timer repeat | 多个周期 | 周期触发；cancel 后停止 | 正常 |
| U15 | timer 排序稳定性 | 同 deadline 多 timer | 按提交 seq 稳定顺序 | 确定性 |
| U16 | post 从外部投递 | 非 reactor 线程 post | 入队成功，不在该线程执行 | 线程安全 |
| U17 | reactor_step 限流 | max_events=2，5 个就绪 | 返回 ran=2 | 边界 |
| U18 | reactor_drain 不阻塞 | 无未来事件 | 立即返回 | 边界 |
| U19 | job_wait 在模拟线程 | 模拟线程调用 | `ERR_WOULD_BLOCK` | 错误/D6 |
| U20 | job_wait 在非模拟线程 | 后台线程调用 | 正常返回 result | 正常 |
| U21 | destroy 时未完成 job | 有在跑 job | 回调 `CANCELLED`，不泄漏 | 内存所有权 |
| U22 | stats 计数正确 | 提交/完成/取消混合 | 计数与状态匹配 | 观测 |
| U23 | 关 stats/日志行为不变 | 开/关观测 | 任务顺序与结果一致 | 确定性(P0) |
| U24 | 多实例隔离 | 两个 exec | 句柄/状态互不干扰 | 边界/ADR-0014 |
| U25 | allocator 配对 | 自定义计数 allocator | alloc==free，无泄漏 | 内存所有权 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 驱动 | 期望 |
|---|------|------|------|
| G01 | **后台加载资产不阻塞模拟帧** | 模拟线程每帧 `reactor_step`；离线 worker 加载 mesh 纹理，完成后 `post` 回投 | 帧时间稳定（无等待）；资产异步就绪后交回主线程；**帧内无 IO**（D7） |
| G02 | 玩家移动 + 异步音效解码 | 主线程跑移动；worker 解码音效 | 移动输入逐帧处理不受影响；音效在后续帧边界接入 |
| G03 | 关卡加载：world streaming | 玩家跨区 → offload 请求 | 未阻塞；区域就绪后于帧边界切换 |
| G04 | coroutine 任务链 | `spawn(move_then_wait_then_attack())` | 严格按事件序在帧边界推进；结果确定 |
| G05 | 断线重连（外部事件） | 网络线程 `post` | 事件在下一帧边界被处理，不打断当前帧 |

### 4.3 确定性用例（若适用）

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| D01 | 默认单线程事件顺序确定 | 同一提交序列 + fake reactor 固定推进 | 事件执行顺序逐项一致 |
| D02 | job 完成顺序隔离 | 同 args 序列，分别投 `MAIN_QUEUED` 与 `REAL` | `MAIN_QUEUED` 顺序稳定可复现；`REAL` **不作顺序断言**（责任转移，仅断言集合一致） |
| D03 | 回放一致性 | 记录事件执行 checksum（帧号→事件 id 序列） | 两次跑 checksum 相同（L1） |
| D04 | 定时器确定性 | 同 deadline/seq | 稳定顺序；`sim time` 而非墙钟（D3，时钟由调用方注入 fake） |

### 4.4 测试替身

- **fake_reactor**：手动 `advance(ns)` / `fire(timer)`，完全可控推进，替换 libuv；单测与嵌入式共用。
- **fake_clock**：单调时钟桩，注入固定时间戳（满足 D3）。
- **counting_allocator**：统计 alloc/free 配对，验证 §2.5 所有权。
- **fake_io_provider**：模拟 VFS 完成，通过 `post` 投递，验证 G01/G05。

### 4.5 独立单测保障

- 单测只 `#include <maque/mq_executor.h>` + `task.hpp`，不链接 libuv（用 fake_reactor 构建变体）。
- 不依赖 VFS/scene 等任何模块实现；跨模块场景用 fake_io_provider。
- CI 提供两种构建：`executor-fake`（单测）与 `executor-libuv`（集成），公共 ABI 一致。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 接口冻结**：提交 `mq_executor.h` + `task.hpp` 骨架，冻结 `MQ_EXECUTOR_ABI_VERSION=1`；评审 §2.8 线程/确定性契约。**此后接口只增不改**。
2. **阶段 1 · 测试骨架（红）**：写 U01–U25、G01–G05、D01–D04 骨架，配 fake_reactor/fake_clock/counting_allocator；全部先失败。
3. **阶段 2 · 最小实现（绿）**：
   - 2a. 句柄槽表 + create/destroy + 错误模型（U01–U04、U09、U24、U25）。
   - 2b. `MAIN_QUEUED` 执行路径（U07、U08、U17、U18）——**先做降级路径**，它是 WASM 与默认单线程的共同基础。
   - 2c. fake_reactor：定时器/事件/`post`（U13–U16）。
   - 2d. 真线程 worker + job 生命周期（U05、U06、U10–U12、U19–U21），仅在 `REAL` 路径加锁。
4. **阶段 3 · coroutine 桥接 + 边界/确定性**：实现 `task<T>`/`sleep_aw`/`offload_aw`（G04）；补 D01–D04、U23；refactor 去重。
5. **阶段 4 · libuv 后端 + 上游 fake 集成**：接 libuv reactor（集成构建）；跑 G01–G03、G05；性能/内存达标（§6）。
6. **阶段 5 · WASM 降级验证**：无 SAB 构建下 `worker_query_mode=MAIN_QUEUED`，G01 通过且行为一致。

## 6. 验收标准（DoD）

- 接口冻结且被上游 fake（VFS fake_io_provider、script 的 task 使用方）依赖通过；
- 单测覆盖率 ≥ 80%（核心路径 ≥ 90%）；U01–U25、G01–G05、D01–D04 全通过；
- **确定性**：D01/D03 在同一 seed/输入下 checksum 逐项一致（L1）；D02 明确区分确定/不确定路径；
- **性能/内存预算**：
  - 模拟帧内 `reactor_step`（无就绪事件）**零分配**、< 5µs（参考值，CI 记录回归）；
  - `job_submit` 深拷贝后无跨 worker 共享可变内存；
  - 协程帧经 arena，稳态无增长（long-run 迭代后帧分配数恒定）；
  - counting_allocator 显示 alloc==free，无泄漏；
- 无跨模块实现依赖；无全局可变状态（`.clang-tidy` 自定义规则 + 评审）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **coroutine 帧分配开销** | 帧经 executor frame arena；无栈续体用对称转移；避免 `co_await` 链上逐层堆分配；提供 `task` 小对象内联缓冲；基准测试防回归 |
| **libuv 完成顺序不确定** | 完成事件一律经 `post` 入 **FIFO 序号队列**，先排序再派发；确定性用例只用 fake_reactor 断言；文档化"REAL worker 确定性责任转移" |
| `job_wait` 被误用于模拟线程 | 运行时 `current_thread_tag` 检测 → `ERR_WOULD_BLOCK`；debug 断言 + 结构化日志 |
| WASM 降级行为分叉 | `worker_query_mode` 显式上报；`MAIN_QUEUED` 路径与单线程共用代码；CI 多目标（native/wasm）跑同套 G 用例 |
| 跨 worker 共享可变内存 | 提交深拷贝 args，仅经 `post`/句柄回传；禁用共享指针跨边界（docs/02 §9） |
| 帧内异步破坏回放（D7） | 恢复只在帧边界/`post` 之后；集成测试断言帧内无 IO 触发 |
| 句柄悬垂 | 世代校验（U09）+ 调试断言；`worker_destroy` 使旧句柄立即失效 |
| libuv 体积/嵌入式不适用 | reactor 后端可替换；fake/极简 reactor 为嵌入式默认，公共 ABI 不变 |

## 8. Owner agent

- **角色标识**：`agent-executor`
- **职责边界**：仅负责本模块的接口（`mq_executor.h`、`task.hpp`）、测试（fake_reactor 单测 + 场景/确定性用例）与实现（含 libuv 与极简 reactor 后端）；不修改其它模块。跨模块接缝（VFS `post`、script `task`、obs `stats`）只提供接口，不引入对其实实现的依赖。
