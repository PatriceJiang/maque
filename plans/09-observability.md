# 09 · observability（结构化日志 / 指标 / trace / 崩溃）

> 上游：`../docs/09-可观测性与诊断.md`（主）、`../docs/00-设计哲学与约束.md`（A2/A3、ADR-0022、ADR-0012、ADR-0014）、`../docs/05-平台抽象与WASM.md`（§8 WASM 崩溃采集、薄平台层）、`../docs/10-确定性快照与回放.md`（崩溃上下文=回放输入、D 规则）。
> 本计划把 `docs/09` 落成可执行、可验收的模块开发计划：**接口 + 测试先行，后实现**（TDD）。
> **硬约束 P0**：关掉观测/日志，模拟行为必须完全一致（docs/09 §6、docs/10 §2）。任何用例违反即一票否决。

## 1. 模块概述

- **职责**：
  1. **结构化日志**：`{ts, level, domain, event_id, fields...}` → JSON Lines；按 domain 分级开关；sink 可插拔（console/file/内存环形/远程经 VFS）。热路径**零格式化、零分配**（预分配环形缓冲 + 延迟格式化）。
  2. **指标**：counter / gauge / timer（帧时间、各系统耗时、内存分类——直接接 docs/07 分类计数）；全部**可编程读取**（A4）。
  3. **trace**：`scope("physics")` 作用域事件，导出 **Chrome Trace / Perfetto** 兼容 JSON；支持**单帧捕获**（AI/人分析）。
  4. **崩溃**：`std::stacktrace`（ADR-0012）+ DWARF/line tables 符号化；记录**上下文快照**（RNG seed / 帧号 / 输入日志引用 / 世界快照引用）→ 可回放；崩溃时环形缓冲落盘/上报，得到**事件尾部**；产物 = coredump/minidump + 事件尾部 + 指标快照。
  5. **确定性契约**：观测**不得**改变模拟行为；计数器不影响迭代顺序（docs/07 §9、docs/10 D4）。
- **所在层**：诊断控制（阶段 C；被 control plane(10) 消费，被几乎所有运行时/世界/呈现模块调用）。
- **上游文档**：docs/09、docs/00（A2/A3、ADR-0022/0012/0014）、docs/05 §8、docs/10、docs/07（内存计数）、docs/04（file/https sink）。
- **一句话**：观测数据是 AI 自调试/CI 判定/回放重放的**原始数据源**——结构化、低成本可关闭、确定性友好；关掉它行为必须逐帧一致。

### 范围界定（重要）

| 属于本模块 | 不属于本模块（见对应 owner） |
|---|---|
| 结构化事件、指标、trace、崩溃上下文的**语义与 C ABI** | 原始 sink 注册与底层写（`log_set_sink`/`log_emit`）→ platform(04) |
| 环形缓冲、延迟格式化、domain 分级、导出格式 | worker/job 调度（后台格式化线程）→ executor(05) |
| 崩溃上下文拼装（seed/帧号/输入日志引用/快照引用） | 栈回溯采集/解析打桩点 → platform(04) `stacktrace_*` |
| 指标读取 ABI、trace 落盘数据 | 指标/trace 的**对外端点**（HTTP/JSON-RPC/MCP）→ control plane(10) |
| 崩溃报告结构与产物 | coredump/minidump 的**平台产生** → platform(04)/OS |
| 内存分类计数的**读取** | 分类计数的**产生** → docs/07（各分配器 owner） |
| file/https sink 的数据 | 实际文件/网络 IO → VFS(06) |

> 与 platform(04) 的接缝：obs **不**直接触碰 OS；sink 写入走 `mq_platform_v1.log_set_sink`/`log_emit`，栈回溯走 `stacktrace_capture`/`stacktrace_resolve`，时钟走 `monotonic_ns`。obs 只在其上构造结构与编排。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（docs/02、ADR-0013）。公共头为**纯 C**，不含 `std::`，无异常、无 RTTI。
> 热路径 API 的**记录侧**不分配、不格式化；格式化/落盘异步到后台（经 executor `post`），或崩溃时在信号安全路径内完成。

### 2.1 公共头 `mq_obs.h`

```c
/* mq_obs.h —— 纯 C ABI，版本化 vtable。唯一导出入口：mq_module_query */
#ifndef MAQUE_MQ_OBS_H
#define MAQUE_MQ_OBS_H

#include <stdint.h>
#include <stddef.h>

#define MQ_OBS_ABI_VERSION  1
#define MQ_OBS_IFACE_NAME   "maque.observability.v1"

/* ---------- 错误模型：int32，0=成功；无跨边界异常（ADR-0013） ---------- */
typedef enum mq_obs_status {
    MQ_OBS_OK              =  0,
    MQ_OBS_ERR_INVALID     = -1,  /* 空指针/非法入参 */
    MQ_OBS_ERR_ABI_VERSION = -2,  /* 主版本不匹配 */
    MQ_OBS_ERR_NO_SPACE    = -3,  /* 环形已满且拒绝覆盖（保尾模式） */
    MQ_OBS_ERR_UNSUPPORTED = -4,  /* 平台无栈回溯 / 无信号处理 */
    MQ_OBS_ERR_NOT_FOUND   = -5,  /* 句柄/域未注册 */
    MQ_OBS_ERR_IO          = -6,  /* 导出/落盘失败 */
    MQ_OBS_ERR_DESTROYED   = -7
} mq_obs_status;

/* ---------- 级别 / 域 / 事件 id ---------- */
typedef enum mq_obs_level {
    MQ_OBS_TRACE = 0, MQ_OBS_DEBUG = 1, MQ_OBS_INFO = 2,
    MQ_OBS_WARN  = 3, MQ_OBS_ERROR = 4, MQ_OBS_FATAL = 5, MQ_OBS_OFF = 6
} mq_obs_level;

/* 域：稳定字符串 id（如 "render"、"scene"、"fs"），编译期哈希为 uint32。
   内置域枚举仅为便捷；第三方域用字符串注册。 */
typedef enum mq_obs_builtin_domain {
    MQ_OBS_DOMAIN_CORE = 0, MQ_OBS_DOMAIN_RENDER, MQ_OBS_DOMAIN_SCENE,
    MQ_OBS_DOMAIN_FS, MQ_OBS_DOMAIN_SCRIPT, MQ_OBS_DOMAIN_PHYSICS,
    MQ_OBS_DOMAIN_AUDIO, MQ_OBS_DOMAIN_VFS, MQ_OBS_DOMAIN_EXECUTOR,
    MQ_OBS_DOMAIN_COUNT
} mq_obs_builtin_domain;

/* 事件 id：等价于编译期 FNV-1a 稳定字符串哈希（如 mq_event_id("render.draw_call")）。
   稳定：同一字符串在任何平台/构建得到相同 id（支持跨进程比对）。 */
uint32_t mq_obs_event_id(const char* stable_name);

/* ---------- 结构化字段（POD，值内联，热路径零分配） ---------- */
typedef enum mq_obs_field_type {
    MQ_OBS_FIELD_I64 = 0, MQ_OBS_FIELD_U64, MQ_OBS_FIELD_F64,
    MQ_OBS_FIELD_BOOL, MQ_OBS_FIELD_STR  /* ptr+len，调用期内有效，不复制 */
} mq_obs_field_type;

typedef struct mq_obs_field {
    uint32_t            key;   /* mq_obs_event_id(...)，字段名稳定哈希 */
    uint8_t             type;  /* mq_obs_field_type */
    uint8_t             reserved0[3];
    union {
        int64_t         i64;
        uint64_t        u64;
        double          f64;
        uint32_t        b;     /* bool */
        struct { const char* p; uint32_t n; } str;  /* UTF-8，调用期有效 */
    } v;
} mq_obs_field;

/* ---------- 日志 sink（obs 层，与 platform 原始 sink 不同层） ---------- */
typedef struct mq_obs_sink_v1 {
    uint32_t struct_size;   /* = sizeof(mq_obs_sink_v1) */
    uint32_t abi_version;
    /* 收到已格式化的一行 JSON Lines（UTF-8，len 不含 NUL）。所有权归 sink 调用期。 */
    mq_obs_status (*write_line)(void* self, const char* json, uint32_t len);
    mq_obs_status (*flush)(void* self);
    void          (*destroy)(void* self);  /* 由 obs 在其生命周期管理下调用 */
} mq_obs_sink_v1;

/* ---------- 指标句柄（不透明，带世代） ---------- */
typedef struct mq_obs_metric_h { uint32_t index; uint32_t generation; } mq_obs_metric_h;
typedef enum mq_obs_metric_kind { MQ_OBS_METRIC_COUNTER = 0, MQ_OBS_METRIC_GAUGE } mq_obs_metric_kind;

/* ---------- trace：Chrome Trace / Perfetto 事件 ---------- */
typedef struct mq_obs_trace_scope { uint64_t begin_ns; uint32_t cat; const char* name; } mq_obs_trace_scope;

/* ---------- 崩溃上下文（= 回放输入，docs/10 §7） ---------- */
typedef struct mq_obs_crash_context {
    uint64_t      rng_seed;            /* 引擎 RNG seed */
    uint64_t      frame_number;        /* 逻辑帧号 */
    const char*   input_log_ref;       /* 输入日志引用（VFS 路径或 mem:// id） */
    const char*   world_snapshot_ref;  /* 最近世界快照引用（docs/10 §4） */
    uint64_t      sim_time_ns;         /* 逻辑时间（非墙钟） */
} mq_obs_crash_context;

/* ---------- 创建配置 ---------- */
typedef struct mq_obs_config {
    uint32_t       struct_size;          /* = sizeof(mq_obs_config) */
    uint32_t       abi_version;          /* 必须 = MQ_OBS_ABI_VERSION */
    mq_allocator*  allocator;            /* 分配者释放（docs/02 R5） */
    uint32_t       ring_capacity_records;/* 事件环形缓冲槽数，预分配 */
    uint32_t       ring_payload_bytes;   /* 每槽内联字节（字段的字符串可 spill 到 arena） */
    uint8_t        overwrite_oldest;     /* 1=满则覆盖最旧（默认）；0=保尾并拒绝 */
    uint8_t        async_format;         /* 1=后台格式化；0=同步格式化（测试/嵌入式） */
    uint8_t        capture_stacktrace;   /* 崩溃/断言是否采集栈 */
    uint8_t        reserved0;
    uint32_t       max_metrics;          /* 指标槽预分配数 */
    uint32_t       reserved1;
    void*          executor;             /* executor 实例（后台格式化/落盘用，可 NULL） */
    void*          host;                 /* host context，透传不解释 */
} mq_obs_config;

/* ---------- vtable：完整操作面，只增不改 ---------- */
typedef struct mq_obs_v1 {
    uint32_t struct_size;   /* = sizeof(mq_obs_v1) */
    uint32_t abi_version;   /* 主版本不匹配即拒绝 */

    /* --- 生命周期 --- */
    mq_obs_status (*create)(const mq_obs_config* cfg, void** out_self);
    void          (*destroy)(void* self);          /* flush + 卸载崩溃处理器；幂等 */

    /* --- 日志：热路径记录（零分配、零格式化） --- */
    /* domain_id 来自 mq_obs_event_id(域名字符串) 或内置；低于该域级开关则直接丢弃（零成本） */
    void (*log_record)(void* self, uint32_t domain_id, uint32_t event_id,
                       uint8_t level, const mq_obs_field* fields, uint32_t nfields);
    mq_obs_status (*set_domain_level)(void* self, uint32_t domain_id, uint8_t min_level);
    uint8_t       (*get_domain_level)(void* self, uint32_t domain_id);
    /* 全局默认级别：未单独设置域的兜底 */
    void          (*set_default_level)(void* self, uint8_t min_level);

    /* --- sink 管理（obs 层；底层写由 platform sink 或 file/vfs sink 实现） --- */
    mq_obs_status (*sink_add)(void* self, const mq_obs_sink_v1* sink);
    mq_obs_status (*sink_remove_all)(void* self);
    /* 手动排空环形并格式化投递 sink；异步模式下后台亦会调用 */
    mq_obs_status (*flush)(void* self);

    /* --- 尾部读取（AI/控制平面用：续读事件尾部，docs/09 §7） --- */
    /* 从最旧到最新拷出至多 max 条到调用方缓冲；返回实际条数。 */
    uint32_t (*tail_read)(void* self, uint64_t since_seq, void* out_records,
                          uint32_t max_records);

    /* --- 指标 --- */
    mq_obs_status (*metric_define)(void* self, uint32_t name_id, uint8_t kind,
                                   uint32_t unit_id, mq_obs_metric_h* out);
    void          (*counter_add)(void* self, mq_obs_metric_h m, int64_t delta);
    void          (*gauge_set)(void* self, mq_obs_metric_h m, double value);
    /* 返回当前值（counter 累加值 / gauge 瞬时值）；供编程读取（A4） */
    mq_obs_status (*metric_read)(void* self, mq_obs_metric_h m, double* out_value);
    /* 遍历全部指标（name_id/kind/value）供控制平面导出 */
    uint32_t      (*metric_enumerate)(void* self, void* out_items, uint32_t max_items);

    /* --- timer / scope（trace + 指标二合一） --- */
    mq_obs_status (*timer_begin)(void* self, uint32_t name_id, uint64_t* out_token);
    void          (*timer_end)(void* self, uint64_t token);   /* 记入 trace + timer 指标 */

    /* --- trace --- */
    mq_obs_status (*trace_begin)(void* self, uint32_t cat_id, const char* name,
                                 uint64_t* out_token);
    void          (*trace_end)(void* self, uint64_t token);
    /* 单帧捕获：开始/结束一帧 trace 采集；结束后可导出 */
    mq_obs_status (*trace_frame_capture_begin)(void* self, uint64_t frame_number);
    mq_obs_status (*trace_frame_capture_end)(void* self);
    /* 导出 Chrome Trace JSON 到 sink（经 VFS 落盘或内存 sink） */
    mq_obs_status (*trace_export)(void* self, const char* vfs_uri);
    mq_obs_status (*trace_reset)(void* self);

    /* --- 崩溃 --- */
    mq_obs_status (*crash_set_context)(void* self, const mq_obs_crash_context* ctx);
    /* 安装信号/terminate 处理器（信号安全：落环形尾 + 上下文 + 栈）；WASM 下用 JS onerror */
    mq_obs_status (*crash_install)(void* self);
    void          (*crash_uninstall)(void* self);
    /* 手动报告（断言/契约失败调用）：debug 断言失败 → 结构化报告（docs/09 §5） */
    void (*assert_fail)(void* self, const char* expr, const char* file,
                        uint32_t line, const char* msg);
    /* 生成崩溃报告（minidump/coredump + 事件尾部 + 指标快照）到 vfs_uri；可测试路径 */
    mq_obs_status (*crash_write_report)(void* self, const char* vfs_uri);
} mq_obs_v1;

/* 唯一导出的模块入口（docs/02 §3） */
void* mq_module_query(const char* interface_name, uint32_t min_version);

#endif /* MAQUE_MQ_OBS_H */
```

### 2.2 公开类型清单

| 类型 | 形态 | 说明 |
|---|---|---|
| `mq_obs_status` | enum(int32) | 错误码；0 成功，负值失败；等价 `std::expected` 的 C 表达 |
| `mq_obs_level` | enum | 6 级 + OFF；与 platform `mq_log_level` 语义对齐 |
| `mq_obs_builtin_domain` | enum | 内置域便捷索引；实际域一律以稳定字符串哈希 id 表示 |
| `mq_obs_field` / `mq_obs_field_type` | POD | 内联键值字段；字符串 ptr+len，调用期内有效 |
| `mq_obs_sink_v1` | vtable | obs 层 sink（write_line/flush/destroy），可插拔 |
| `mq_obs_metric_h` / `mq_obs_metric_kind` | 句柄 + enum | 指标句柄（世代防悬垂）、counter/gauge |
| `mq_obs_trace_scope` | POD | trace 作用域起止 |
| `mq_obs_crash_context` | POD | **回放输入**：seed/帧号/输入日志引用/快照引用 |
| `mq_obs_config` | POD（含 `struct_size`） | create 配置 |
| `mq_obs_v1` | vtable | 唯一操作面，末尾追加式演进 |
| `mq_allocator` | POD | 复用 docs/02 统一 allocator，创建时注入 |

### 2.3 操作面语义（完整）

| 函数 | 语义 / 参数 / 返回 / 错误 |
|---|---|
| `create(cfg,out)` | 建 obs 实例，预分配环形（`ring_capacity_records × ring_payload_bytes`）、指标槽。错误：`INVALID`（allocator/cfg/out 空）、`ABI_VERSION`、`OUT_OF_MEMORY`（—注意：错误码收敛在 `mq_obs_status` 内，OOM 复用 `MQ_OBS_ERR_NO_SPACE` 或由 allocator 空返回触发 `INVALID`）。 |
| `destroy(self)` | flush 环形 → sink；卸载崩溃处理器；释放环形/指标/字符串 arena；幂等（NULL 安全）。 |
| `log_record(self,domain,event,level,fields,n)` | **热路径**：若 `level < domain_level`（或全局级）立即零成本返回；否则把字段 POD **拷贝进环形槽**（值内联，字符串拷进 payload arena、超长截断）。不格式化、不调用 sink、不分配。 |
| `set_domain_level` / `get_domain_level` / `set_default_level` | 分级开关；运行期可调（仅影响是否记录，**不影响模拟**）。 |
| `sink_add` | 注册 obs sink；线程/生命周期由调用方保证（一般启动期）。 |
| `sink_remove_all` | 移除全部 sink（不 destroy 内存，由 sink 自管或 obs 生命周期约定）。 |
| `flush` | 排空环形：格式化 JSON Lines → 逐 sink `write_line`。异步模式下也在帧边界/失速时触发。 |
| `tail_read(self,since_seq,out,max)` | 续读事件尾部（AI/控制平面）；返回拷贝条数，未格式化则按记录形态返回。 |
| `metric_define(name_id,kind,unit_id,out)` | 定义指标（去重：同名同 kind 返回既有句柄）。槽满 → `NO_SPACE`。 |
| `counter_add(m,delta)` | 累加；**仅记录侧**，不读时零分配、不影响逻辑顺序。 |
| `gauge_set(m,value)` | 设瞬时值。 |
| `metric_read(m,out)` | 编程读取当前值（A4）。 |
| `metric_enumerate(out,max)` | 遍历导出全部指标（name/kind/value）。 |
| `timer_begin(name,out_token)` / `timer_end(token)` | 作用域耗时；同时产出 trace 事件与 timer 指标。 |
| `trace_begin/end` | 手动 trace 作用域（比 timer 更通用，带 category）。 |
| `trace_frame_capture_begin/end` | **单帧详细捕获**开关（docs/09 §4）。 |
| `trace_export(vfs_uri)` | 导出 Chrome Trace/Perfetto 兼容 JSON 到 VFS（https/file/mem 均可）。 |
| `trace_reset` | 清空 trace 缓冲（释放后单帧捕获可复用）。 |
| `crash_set_context(ctx)` | 设置崩溃上下文（不复制字符串内容，引用调用方生命周期；崩溃前须提交存活引用）。 |
| `crash_install` | 安装信号/`std::terminate` 处理器；不可用平台返回 `UNSUPPORTED`。信号安全路径只做：环形尾落盘 + 上下文 + `stacktrace_capture`。 |
| `crash_uninstall` | 卸载处理器。 |
| `assert_fail(expr,file,line,msg)` | 结构化断言报告（debug 契约失败）；debug 构建打日志，release 可 no-op。 |
| `crash_write_report(vfs_uri)` | 生成报告：minidump/coredump（平台能力，经 platform）+ 事件尾部 + 指标快照 + 上下文。 |

### 2.4 版本化

- `struct_size` 语义（docs/02 §3）：**只增不改**；`mq_obs_v1` 表尾追加函数指针，旧调用方按自身 `struct_size` 截断使用（用例 OV1/OV2）。
- `abi_version`：`create` 时 `cfg.abi_version` 主版本与 `MQ_OBS_ABI_VERSION` 不符 → `MQ_OBS_ERR_ABI_VERSION`（由 host 产出可读错误，服务 A2）。
- 随 vtable 传递的 POD（`mq_obs_field`/`mq_obs_config`/`mq_obs_sink_v1`/`mq_obs_crash_context`）首字段均为 `struct_size`/固定布局，支持截断读取。
- 接口名稳定为 `"maque.observability.v1"`；不兼容升级走 `.v2` 并行共存。
- **事件 id / 字段 key 稳定性**：`mq_obs_event_id` 的 FNV-1a 算法冻结（一个测试向量表锁定），保证跨进程/跨平台比对（控制平面按 id 直接聚合）。

### 2.5 内存所有权 / 线程 / 确定性约定

**内存所有权**：

- `create` 收到 `mq_allocator*`；obs 的 self、环形缓冲、指标槽、字符串 arena **全部**经它分配并同一 allocator 释放（R5）。
- `log_record` 的 `fields`/字符串 ptr **归调用方**：调用期内有效；obs **深拷贝**进环形 payload（值内联、字符串入 arena、超长截断），返回后不引用调用方内存。
- 传给 `sink_add` 的 `mq_obs_sink_v1` 结构由 obs **拷贝**（字段级），其 `self` 生命周期由调用方保证至 `sink_remove_all`/`destroy`。
- `tail_read`/`metric_enumerate`/`trace_export` 的输出缓冲归调用方。
- 跨边界不出现 `std::string`/`vector`/`shared_ptr`；字符串一律 UTF-8 `const char*` + len。

**线程约定**：

- `log_record` / `counter_add` / `gauge_set` / `timer_*` / `trace_*`：**默认仅模拟线程**调用（单写者，环形无锁）。worker 若需观测，须经 `executor->post` 回投到模拟线程（docs/05 §4：worker 不碰模拟状态）。
- 后台格式化（`async_format=1`）由 executor 上的 job 执行 `flush`；其读环形与模拟线程写环形通过**双缓冲/序号**交接（写者只写 tail，读者读取稳定段）。
- `crash_install` 的处理器异步信号安全：不分配、不加锁、不调用非 AS 安全函数；只写预分配的崩溃缓冲。

**确定性约定（核心契约）**：

- **P0**：`level` 低于开关时 `log_record` **立即返回**；无论开关与否，模拟逻辑不读观测结果，故输出逐帧一致（用例 D1–D4）。
- 计数器/计时器**只累加/记录**，不参与查询迭代顺序，不影响实体/系统遍历（docs/07 §9、D4）。
- 观测时间戳一律来自 platform `monotonic_ns`（物理）或 sim time（逻辑帧号）；**禁止**墙钟进入模拟逻辑（docs/10 D3）。
- 后果：`obs` 关闭（或 sink 全空、级别全 OFF）等价于"没有观测"，checksum 序列不变。

## 3. 依赖与边界

- **可依赖**：
  - `mq_core.h` 的 `mq_allocator`（POD vtable，docs/02 §10）、固定宽度整数。
  - `platform`(04) 的 `monotonic_ns`、`log_set_sink`/`log_emit`、`stacktrace_capture`/`stacktrace_resolve`——**仅经其 C ABI**，初始化期缓存函数指针。
  - `executor`(05) 的 `post`（后台格式化/落盘，异步模式）。
  - `VFS`(06) 的写路径（file/https/mem sink、trace/crash 落盘）——**经 C ABI**，不 `#include` 具体实现。
- **禁止**：
  - ❌ 依赖任何上层实现（scene/renderer/script/physics…）——上层只**调用** obs 的 C ABI。
  - ❌ 热路径过虚函数：`log_record`/`counter_add` 等由上层在初始化期**缓存函数指针**后直接调用，帧内不逐次查 vtable（docs/02 §7）。
  - ❌ 持有全局可变状态 / 单例（ADR-0014）；崩溃处理器用具名静态但仅指向**当前实例**的固定最小缓冲（见 §7 风险），不承载业务可变状态。
  - ❌ 热路径 `log_record` 内分配 / 格式化 / IO。
  - ❌ 在头文件暴露 OS/JS 类型。
- **与其它模块的接缝**：
  - `04 platform`：obs 的 sink 落点即 platform sink；栈回溯/时钟经其打桩点。
  - `05 executor`：异步 `flush`/trace 落盘经 `post` 到 worker；帧内不触发落盘（D7）。
  - `06 VFS`：`trace_export`/`crash_write_report`/file/https sink 的写经 VFS（`mem://` 供 AI 构造复现，docs/09 §7）。
  - `10 control plane`：消费 `metric_enumerate`/`tail_read`/`trace_export`/`crash_write_report` 暴露为端点（obs 只提供数据，不做协议）。
  - `07 内存`：obs 读取 frame/persistent/pool/virtual 分类计数填入 gauge（计数产生归各 allocator）。
  - `10 确定性`：`crash_set_context` 的 4 元组与回放输入成对（docs/10 §7）。

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**独立**：仅依赖本模块 + `mq_obs.h` + **mem sink + fake clock**，不依赖 platform/executor/VFS 真实实现，不依赖其它模块。

### 4.1 测试替身（核心）

- **`mem_sink`**：实现 `mq_obs_sink_v1`，把每条 `write_line` 的 JSON 行按序追加到预分配内存缓冲（容量可配置）。支持查询行数、按行号取文本、flush 计数；可注入"第 K 次 write 失败"验证错误路径。
- **`fake_clock`**：单调时钟桩，`uint64_t ns` + `advance(ns)`，注入固定时间戳（满足 D3；obs 时间戳走此桩，不读真实时钟）。
- **`counting_allocator`**：统计 alloc/free 配对与峰值，验证 §2.5；可注入第 K 次失败。
- **`fake_stacktrace`**：预置 N 个确定性帧指针 + `resolve` 返回确定性 `"fake.mod"/"fn_i"/"fake.c:LINE"`。
- **`fake_crash_platform`**：记录 `crash_install` 是否被调用、捕获手动触发的崩溃上下文。
- **`recording_vfs`**：记录 `trace_export`/`crash_write_report` 的目标 URI 与字节内容（不落真实文件）。

### 4.2 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| O01 | create/destroy 生命周期 | 合法 cfg+allocator | `OK`；destroy 幂等；alloc==free | 正常 |
| O02 | create 空 allocator | allocator=NULL | `INVALID`，无分配 | 错误 |
| O03 | create 空 out | out=NULL | `INVALID` | 错误 |
| O04 | abi_version 不匹配 | cfg.abi_version=2 | `ABI_VERSION` | 版本 |
| O05 | 环形预分配大小 | cap=256, payload=128 | 创建期一次大分配；稳态 0 分配 | 内存 |
| O06 | 单条记录进环形 | INFO 事件 | flush 后 mem_sink 收到 1 行合法 JSON | 正常 |
| O07 | 字段类型序列化 | i64/u64/f64/bool/str 各一 | JSON 中各字段类型/值正确 | 正常 |
| O08 | 字符串字段深拷贝 | 传入栈上 str，随后改写原串 | flush 后的 JSON 内容为**写入时**的值 | 内存所有权 |
| O09 | 字符串超长截断 | str.len > payload | 截断不越界，JSON 合法 | 边界 |
| O10 | domain 分级开关 | render=WARN，记 render/INFO | 0 行；记 render/ERROR → 1 行 | 正常 |
| O11 | 全局默认级兜底 | 未设域，default=ERROR | INFO 丢弃，ERROR 记录 | 正常 |
| O12 | 级别 OFF | domain=OFF | 任意事件 0 行 | 边界 |
| O13 | **环形覆盖最旧** | cap=8，写 20 条，overwrite=1 | flush 后恰 8 条且为**最后 8 条**；seq 连续 | 边界/覆盖 |
| O14 | **环形保尾拒绝** | cap=8，写 20 条，overwrite=0 | 仅前 8 条保留，第 9 条起丢弃并计 `NO_SPACE` | 边界/保尾 |
| O15 | 覆盖后 seq 不回绕 | 多次覆盖 | `tail_read(since_seq)` 顺序单调、无重复 | 边界 |
| O16 | tail_read 增量续读 | 写 N 条，记 seq，再写 M 条 | 只返回新增 M 条，顺序正确 | 正常 |
| O17 | sink_add/remove_all | 注册 mem_sink 后移除 | 移除后 flush 不再投递 | 正常 |
| O18 | flush 幂等/空环形 | 无记录 flush | `OK`，sink 零次 write | 边界 |
| O19 | 多 sink 广播 | 两个 mem_sink | 每行投递到两者，顺序一致 | 正常 |
| O20 | write_line 失败传播 | mem_sink 第 1 次失败 | `flush` 报错但环形不被破坏（可重试） | 错误 |
| O21 | sink struct_size 截断 | 旧 sink vtable（小 size） | 只调用存在的函数，不越界 | 版本 |
| O22 | metric_define 去重 | 同名同 kind 调 2 次 | 返回同一句柄，槽不增长 | 正常 |
| O23 | metric_define 槽满 | 超 max_metrics | `NO_SPACE` | 边界 |
| O24 | counter 累加 | +5,+3,-1 | metric_read==7 | 正常 |
| O25 | **gauge 覆盖** | set 3.14 后 set 2.71 | metric_read==2.71（非累加） | 正常 |
| O26 | metric_read 悬垂句柄 | destroy 后读旧句柄 | `DESTROYED`/`NOT_FOUND` | 错误/世代 |
| O27 | metric_enumerate 全量 | 定义 3 个指标 | 返回 3 项，name/kind/value 正确 | 正常 |
| O28 | timer 累加时长 | begin/end 夹 advance(1ms) | timer 指标 == 1ms；trace 含该 scope | 正常 |
| O29 | timer 嵌套 | 外层内嵌内层 | 两者时长独立、栈式配对 | 正常 |
| O30 | timer_end 无 begin | 伪造 token | 静默忽略/`NOT_FOUND`，不崩溃 | 错误 |
| O31 | trace_begin/end 序列化 | 两个 scope | Chrome Trace JSON 含 cat/name/ts/dur | 正常 |
| O32 | **trace 单帧捕获** | capture_begin/end 后 export | 仅含该帧事件；`recording_vfs` 收到合法 JSON | 正常 |
| O33 | trace_export 到 VFS | `mem://trace.json` | VFS 收到字节，格式可被 Chrome Trace 解析器接受 | 正常 |
| O34 | trace_reset | 写事件后 reset | 缓冲清空，后续 export 为空 | 边界 |
| O35 | 崩溃上下文设置 | 4 元组 seed/帧/输入/快照 | `crash_write_report` 报告中含该 4 元组 | 正常 |
| O36 | **崩溃报告含事件尾部** | 先记 3 条事件再报崩溃 | 报告含 3 条事件尾部 + 栈 + 指标快照 | 正常 |
| O37 | 断言失败结构化 | assert_fail("x>0",...） | 产生含 expr/file/line 的结构化事件；debug 下可见 | 正常 |
| O38 | crash_install 不支持 | fake 平台无信号 | 返回 `UNSUPPORTED`，不崩溃 | 降级 |
| O39 | assert_fail release no-op | release 构建 | 无事件、不崩溃 | 边界 |
| O40 | struct_size 截断兼容 | 旧 config（较小 size） | 只读前 N 字段，行为正常 | 版本 |
| O41 | 多实例隔离 | create 两个 obs | 各自环形/指标/级别互不影响 | 边界 |
| O42 | destroy 后调用 | 用已销毁 self | 返回 `DESTROYED`/无崩溃 | 错误 |
| O43 | **热路径零分配** | 循环 log_record 于开启级别 | counting_allocator 稳态 alloc 增量为 0 | 内存/性能 |
| O44 | 关级别零开销 | 级别 OFF 循环 log_record | 无分配、无 sink 调用、极低耗时 | 性能 |
| O45 | event_id 稳定向量 | 一组已知名字 | 与冻结的 FNV-1a 向量表逐位一致 | 版本/确定性 |

### 4.3 游戏场景用例（真实玩法驱动）

| # | 场景 | 驱动 | 期望 |
|---|------|------|------|
| G1 | **一帧内采集帧时间与 draw 数** | 一帧：`timer_begin("frame")`→ 渲染系统 `counter_add(draw_calls, N)` + `gauge_set(gpu_mem, x)` → `timer_end` | `metric_read(frame timer)`==该帧 fake 时长；draw_calls==N；gauge==x；trace 含 frame scope |
| G2 | **关卡加载事件时间线** | 加载→生成实体→玩家击杀，各阶段 `log_record` | flush 后 JSON Lines 顺序与阶段一致、时间戳单调；AI 可重建时间线 |
| G3 | **崩溃可重放闭环** | 模拟"玩家移动"中触发断言；设置 ctx(seed=42,frame=100,input=trace.bin,snap=snap://12)；`crash_write_report` | 报告含 seed/帧号/输入日志引用/快照引用 + 栈 + 事件尾部；据此刻重放（交确定性模块核对） |
| G4 | **热路径帧时间不退化** | 开启全域 TRACE 跑 1000 帧，对比关闭 | 帧逻辑输出 checksum 一致；观测开销在预算内（§6）；无帧内 IO |
| G5 | **异步格式化不阻塞帧** | async_format=1；模拟线程持续记录，后台 flush | 模拟帧不等待 sink；sink 最终收齐（顺序保持） |
| G6 | **AI 续读日志尾部** | 运行后 `tail_read(since_seq)` | 增量返回未读事件，控制平面可消费（无需重启） |

### 4.4 确定性用例

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| D1 | **关日志行为一致** | 同一输入跑两遍：一遍全 OFF、一遍全 TRACE | 两遍模拟输出 checksum 逐帧一致（P0） |
| D2 | 计数器不改顺序 | 干预迭代：开启/关闭 counter_add | 遍历顺序/结果不变（D4） |
| D3 | 时间戳不污染模拟 | 改 fake_clock 起点 | 模拟输出不变（模拟不读观测时间，D3） |
| D4 | sink 有无不影响结果 | 挂/不挂 sink 跑同一场景 | 模拟 checksum 一致；仅"是否产出观测数据"不同 |

### 4.5 独立单测保障

- 测试目标只 link `mq_obs.h` + `mem_sink` + `fake_clock` + `counting_allocator` + `fake_stacktrace` + `recording_vfs` + 测试框架；**不 link** platform/executor/VFS 真实实现。
- 编译开关 `MQ_OBS_TEST_INMEM=1`：obs 内部时间/落盘/栈回溯走注入的替身，无真实 IO/时钟。
- CI 双配置矩阵：`async_format on/off × overwrite_oldest on/off × stacktrace on/off` 全跑 mem sink；保证平台/策略差异在无真实依赖下被覆盖。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 接口冻结**：落地 `include/maque/mq_obs.h`（§2.1 全文）+ `MQ_OBS_ABI_VERSION=1`；冻结 `mq_obs_event_id` FNV-1a 算法与测试向量（O45）；`static_assert` 校验各 POD 大小/偏移。**接口不完备不得进入阶段 1。**
2. **阶段 1 · 测试骨架（红）**：建 `mem_sink`/`fake_clock`/`counting_allocator`/`fake_stacktrace`/`recording_vfs`；按 §4.2 逐条写断言全部编译但失败。先覆盖 O01–O09、O22–O25（生命周期/记录/指标）。
3. **阶段 2 · 最小实现（绿）**：
   - 2a. 生命周期 + 错误模型 + 版本校验（O01–O04、O40–O42）。
   - 2b. 环形缓冲 + 延迟格式化 + 分级开关（O05–O18、O43、O44）。
   - 2c. 指标（O22–O27）。
   - 2d. `flush` + sink 广播（O19–O21、G5）。
4. **阶段 3 · trace / 崩溃 + 边界/确定性**：
   - trace scope + 单帧捕获 + Chrome Trace 导出（O28–O34、G1）。
   - 崩溃上下文 + 报告（含事件尾部 + 指标快照 + 栈）（O35–O39、G3）。
   - 补 D1–D4、G2/G4/G6；refactor 去重；跑 `.clang-tidy` 零全局可变状态检查。
5. **阶段 4 · 上游 fake 集成 + 性能/内存达标**：
   - 接 platform（真实 `stacktrace_*`/`log_emit`）、executor（`post` 后台 flush）、VFS（file/https/mem sink）各跑一次**冒烟**（非替身）。
   - 校验热路径零分配/耗时预算（§6）；对接 `agent-control` 的 fake consumer 验证 `metric_enumerate`/`tail_read`/`trace_export` 被正确消费（只提供数据，不实现协议）。

## 6. 验收标准（DoD）

- **接口冻结**：`mq_obs.h` 全函数签名与语义冻结；`struct_size`+`abi_version` 版本化被截断用例（O21/O40）验证通过；被至少一个上游 fake（control plane）依赖通过。
- **单测覆盖率 ≥ 80%**（行覆盖），§4.2 全部 45 条 + §4.3 6 条 + §4.4 4 条通过。
- **硬约束 P0 通过**：D1（关日志行为逐帧 checksum 一致）为**门禁用例**，失败即整体不通过；D2–D4 通过。
- **环形语义正确**：O13（覆盖最旧、保留最后 N 条）与 O14（保尾拒绝）均通过；`tail_read` 增量无重复/无回绕（O15/O16）。
- **崩溃闭环**：G3 报告含 seed/帧号/输入日志引用/快照引用 + 事件尾部 + 栈 + 指标快照，可驱动回放（交确定性模块核对）。
- **性能/内存预算**：
  - `log_record`（级别开启）稳态**零分配**、单次 ≤ ~80ns（不含后台格式化）；级别关闭 ≤ ~5ns（一次比较即返回）。
  - `counter_add`/`gauge_set` 单次 ≤ ~10ns、零分配。
  - 异步格式化（G5）不阻塞模拟帧，帧耗时不受 sink 影响。
  - `create`→`destroy` 后 counting_allocator `alloc==free`（无泄漏）。
  - 环形内存 = `cap × payload` 一次性预分配，稳态不增长。
- **边界纪律**：无跨模块实现依赖；无全局可变状态（崩溃处理器的具名静态仅持最小固定缓冲，见 §7）；头文件不暴露 OS 类型；all 入口纯 C（无异常越界）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **观测拖慢热路径**（docs/09 §9） | 预分配环形 + 值内联 + 延迟格式化 + 分级即返；`log_record` 级别关闭一次比较返回；O43/O44 基准防回归 |
| **观测改变行为破坏回放** | **P0 门禁 D1**；观测只记录不参与逻辑；计数器不干预迭代顺序（D2/D4）；CI 校验 checksum |
| 日志爆炸 | domain 分级 + 环形覆盖最旧（O13）+ 可选采样；保尾模式（O14）供崩溃前关键日志 |
| 环形满丢尾部（崩溃时最需尾部） | overwrite=0 保尾模式 + 崩溃路径优先落盘稳定段；大容量环形兜底 |
| trace 体积 | 单帧捕获（O32）+ 采样 + 经 docs/08 codec 压缩落盘 |
| **崩溃处理器非信号安全** | 处理器只：写预分配缓冲 + `stacktrace_capture`（AS 安全）+ 标记，**不做**分配/锁/格式化；真正的格式化/落盘在下次启动或子进程完成 |
| WASM/嵌入式崩溃信息缺失（docs/05 §8） | 保留 DWARF/line tables（勿 strip）+ JS `onerror` 栈拼接；无信号平台 `crash_install` 返回 `UNSUPPORTED`，改由 `assert_fail`/JS 钩子产报告（O38） |
| 后台格式化与模拟线程数据竞争 | 单写者环形 + 序号交接，读者只读稳定段；worker 不写模拟状态（docs/05 §4） |
| event_id/字段 key 跨版本漂移 | 冻结 FNV-1a + 测试向量（O45）；控制平面按 id 聚合不受字符串改动影响 |
| 多实例共享崩溃处理器 | 具名静态仅存"当前实例"指针 + 固定缓冲；多实例时明确约定仅一个实例可 `crash_install`（或按注册序），其余复用同一处理器（不承载业务状态，ADR-0014 兼容） |
| text 字段生命周期误用 | `log_record` **深拷贝**字符串进 arena（O08/O09），返回后不引用调用方内存 |

## 8. Owner agent

- **角色标识**：`agent-obs`
- **职责边界**：仅负责本模块的接口（`mq_obs.h`）、测试（mem_sink/fake_clock 独立单测 + 场景/确定性用例）与实现（结构化日志/指标/trace/崩溃管线）。不修改其它模块：不实现原始 sink 落点与栈回溯（归 `agent-platform`）、不实现后台调度（归 `agent-executor`）、不实现文件/网络 IO（归 `agent-vfs`）、不实现对外协议端点（归 `agent-control`）、不产生内存分类计数（归 `agent-core`/docs/07）。
