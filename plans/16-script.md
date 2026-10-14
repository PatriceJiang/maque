# 16 · script（quickjs 为主 + 可选 VM）开发计划

> Owner agent：`agent-script`　上游：`docs/14`（主）、`docs/06`（绑定生成）、`docs/12`（生命周期）、`docs/00`（ADR-0008/0026）、`docs/09`、`docs/02`。
> 铁律：**接口先冻结，测试先行（红→绿→重构）**；接口不完备不得进入实现。
> 主参考：[`docs/14-脚本.md`](../docs/14-脚本.md)；关联 [`docs/06`](../docs/06-反射与代码生成.md)、[`docs/12`](../docs/12-场景与世界.md)。

## 1. 模块概述

- **职责**：
  1. **统一后端 ABI**：定义 `mq_script_vm_v1` 版本化 C vtable（create/eval/call/register_host_fn/gc_step…），让
     quickjs / lua / python / v8 **只换实现**，上层绑定层与生命周期封装不变（docs/14 §1）。
  2. **主 VM = quickjs**：内部 refcount、无 JIT、启动快、确定性析构友好（ADR-0008）。lua/python/v8 为**后续可选后端**，
     **构建期裁剪**（CMake option），仅打包所选 VM。
  3. **反射生成绑定**：所有宿主类型/函数绑定的签名与编解码由**反射描述符生成**（docs/06 §6），**禁止手写**；
     参数/返回值经**序列化中枢**在脚本值与 POD 之间转换。
  4. **宿主 API 面 + capability 门控**：脚本只能调用**受控 API 表**（非裸指针），调用经**句柄**；远程内容只能访问被授权 API。
  5. **生命周期确定化**：脚本只**注册回调**（不自调度），引擎在固定点、深序调用：
     `on_enter_tree → ready → [process(dt)|physics_process(dt)]* → on_exit_tree → on_destroy`（对齐 docs/12 §6）。
  6. **确定性（OD-5 落点）**：**cycle GC 仅帧边界 `gc_step` 驱动，帧内禁隐式 GC**；脚本 RNG 用引擎 RNG；
     运行在**模拟线程**；析构外部副作用延迟到确定点。
  7. **沙箱 + 热重载**：远程内容受限沙箱（能力白名单 + 资源/时间预算）；源文本变更在**帧边界**重载并**迁移实例状态**。
- **所在层**：脚本层（呈现层尾，README §4 阶段 E）；依赖方向只向下（core/reflect、scene、persistence、executor）。
- **上游文档**：`docs/14`（主）、`docs/06`（反射=绑定唯一来源）、`docs/12`（生命周期与句柄）、`docs/02`（C ABI + struct_size 版本化）、
  `docs/09`（结构化异常）、`docs/00`（A1–A5、ADR-0008/0026）。
- **一句话**：**一个以 quickjs 为主、后端可换、绑定全自动生成、GC 时机确定、能力可门控的确定性脚本层，让新增引擎 API 自动出现在脚本且回放逐帧一致。**

### 1.1 术语与硬不变式

| 术语 | 含义 |
|------|------|
| **VM 后端** | 实现 `mq_script_vm_v1` 的一个脚本运行时（quickjs 为主）。 |
| **脚本绑定（binding）** | 由反射描述符生成的宿主类型/函数与脚本值的转换胶水；**禁止手写**。 |
| **宿主函数（host fn）** | 脚本可调用的引擎 API，经签名字符串注册、经受控表暴露。 |
| **capability** | 一次 VM 实例被授权的宿主能力集合（如 `fs.read`、`net`、`scene.write`）。 |
| **帧边界** | 模拟帧之间引擎可安全做 GC/重载/迁移的固定点（docs/12 §6）。 |
| **脚本实例** | 一个绑定到场景节点/实体的脚本上下文，含可见状态与回调集。 |

**硬不变式（贯穿全部实现与测试）：**

1. **帧内禁隐式 GC**：任何脚本执行（eval/call/host fn）期间**不得**触发 VM 的 cycle GC；GC 只在 `gc_step`（帧边界）驱动（ADR-0026）。
2. **绑定全生成**：宿主绑定代码 100% 由反射 codegen 产出；手写绑定视为 bug（CI 扫描 `gen/` 之外的绑定注册即失败）。
3. **确定性**：同 seed + 同脚本 + 同输入 → 同轨迹；脚本 RNG 走引擎 RNG；禁 `Math.random` 直连系统熵。
4. **零全局可变状态**：所有状态在 instance context（ADR-0014）；禁单例/可变 static。
5. **边界仅 C ABI**：`extern "C"`、无异常逃逸、无 STL 跨边界、分配者释放（ADR-0013、docs/02 R1–R8）。
6. **不暴露裸指针**：脚本侧一切引擎对象均为**句柄**（`{id,generation}`），不泄漏宿主地址。

## 2. 接口层设计（完备，先冻结）

> 公共头 `modules/script/include/mq_script.h`（纯 C）。接口名 `"maque.script.v1"`。
> 跨边界仅 POD + 固定宽度整数 + UTF-8 `const char*`。所有 `*_v1` 结构带 `struct_size` + `abi_version`。

### 2.1 公共头 `mq_script.h`（示意，冻结）

```c
/* mq_script.h —— 纯 C，跨 .so ABI */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MQ_SCRIPT_ABI_VERSION 1u
#define MQ_SCRIPT_IFACE_NAME  "maque.script.v1"

/* 与 docs/02 一致的 allocator；本模块只经它分配（分配者释放）。 */
typedef struct mq_allocator {
    uint32_t struct_size;
    void* (*alloc)(void* self, size_t size, size_t align);
    void  (*free )(void* self, void* ptr);
} mq_allocator;

/* 错误码：0=成功；负=错误（无异常，docs/02 R2）。 */
typedef enum mq_script_status {
    MQ_SCRIPT_OK                =  0,
    MQ_SCRIPT_E_INVALID_ARG     = -1,
    MQ_SCRIPT_E_ABI             = -2,   /* abi_version 不匹配 */
    MQ_SCRIPT_E_ABI_VERSION     = -3,   /* struct_size 过小无法安全使用 */
    MQ_SCRIPT_E_COMPILE         = -4,   /* 语法/编译错误 */
    MQ_SCRIPT_E_RUNTIME         = -5,   /* 运行期异常（未捕获） */
    MQ_SCRIPT_E_NOT_FOUND       = -6,   /* 函数/模块/符号不存在 */
    MQ_SCRIPT_E_CAP_DENIED      = -7,   /* capability 未授权，越权拒绝 */
    MQ_SCRIPT_E_BUDGET          = -8,   /* 超出时间/内存/指令预算（看门狗） */
    MQ_SCRIPT_E_IN_GC_FORBIDDEN = -9,   /* 帧内试图触发隐式 GC */
    MQ_SCRIPT_E_TYPE_MISMATCH   = -10,  /* 参数/返回值与签名不符 */
    MQ_SCRIPT_E_UNSUPPORTED     = -11,  /* 未编译的 VM 后端 / 未实现的特性 */
    MQ_SCRIPT_E_OOM             = -12,
    MQ_SCRIPT_E_BUSY            = -13   /* 重入/非帧边界调用被拒 */
} mq_script_status;

/* 后端种类（构建期裁剪；未编译的后端 query 返回 E_UNSUPPORTED）。 */
typedef enum mq_script_vm_kind {
    MQ_VM_QUICKJS = 1, MQ_VM_LUA = 2, MQ_VM_PYTHON = 3, MQ_VM_V8 = 4
} mq_script_vm_kind;

/* 脚本值种类：与 persistence/reflect 的 mq_value 一一映射（§2.6）。 */
typedef enum mq_script_val_kind {
    MQ_SV_NONE = 0, MQ_SV_BOOL, MQ_SV_I64, MQ_SV_F64, MQ_SV_STRING,
    MQ_SV_HANDLE, MQ_SV_ENUM, MQ_SV_LIST, MQ_SV_STRUCT
} mq_script_val_kind;

typedef struct mq_script_val {
    mq_script_val_kind kind;
    uint64_t           type_id;    /* ENUM/LIST/STRUCT 的反射 type_id；否则 0 */
    uint32_t           count;      /* LIST 元素数 */
    union {
        int      b;
        int64_t  i64;
        double   f64;
        struct { const char* ptr; uint32_t len; } str;   /* 不拥有 */
        struct { uint32_t index; uint32_t generation; } h; /* 句柄 */
        const struct mq_script_val* items;                /* 调用方拥有 */
    } as;
} mq_script_val;

/* 值编解码 vtable：由序列化中枢提供（docs/08）；绑定层按此转换 POD↔脚本值。 */
typedef struct mq_script_codec_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    /* 把反射类型实例编码为脚本值（READ）；WRITE 反向；由 codec 实现 visitor。 */
    int (*to_script)  (void* self, uint64_t type_id, const void* data, mq_allocator* a, mq_script_val* out);
    int (*from_script)(void* self, uint64_t type_id, const mq_script_val* in, void* out, mq_allocator* a);
} mq_script_codec_v1;

/* capability 位（受控 API 门控；见 §2.5）。 */
#define MQ_CAP_SCENE_READ    (1ull << 0)
#define MQ_CAP_SCENE_WRITE   (1ull << 1)
#define MQ_CAP_ASSET_READ    (1ull << 2)
#define MQ_CAP_FS_READ       (1ull << 3)
#define MQ_CAP_FS_WRITE      (1ull << 4)
#define MQ_CAP_NET           (1ull << 5)
#define MQ_CAP_LOG           (1ull << 6)
#define MQ_CAP_TIME          (1ull << 7)   /* 读引擎时钟，非系统墙钟 */
#define MQ_CAP_RNG           (1ull << 8)   /* 引擎 RNG（默认给；系统熵永不给） */
#define MQ_CAP_ALL           (~0ull)

/* VM 配置（POD；非透明）。 */
typedef struct mq_script_vm_opts {
    uint32_t    struct_size;
    mq_script_vm_kind kind;         /* 缺省 QUICKJS */
    uint64_t    capabilities;       /* 允许的 MQ_CAP_* 掩码 */
    uint32_t    sandbox;            /* 0=可信本地；1=受限沙箱（远程内容，§2.7） */
    uint64_t    seed;               /* 引擎 RNG 种子（确定性）；0 → 由 host 指定 */
    uint64_t    mem_budget;         /* 字节上限；0=不限（沙箱下必填，超限 E_BUDGET） */
    uint32_t    time_budget_us;     /* 单次 call 的墙钟/指令预算（看门狗）；0=不限 */
    uint32_t    _pad;
} mq_script_vm_opts;

/* 生命周期回调枚举（对齐 docs/12 §6；顺序即编号，固定）。 */
typedef enum mq_script_cb {
    MQ_CB_ON_ENTER_TREE = 0, MQ_CB_READY = 1, MQ_CB_PROCESS = 2,
    MQ_CB_PHYSICS_PROCESS = 3, MQ_CB_ON_EXIT_TREE = 4, MQ_CB_ON_DESTROY = 5,
    MQ_CB_COUNT = 6
} mq_script_cb;

/* 结构化脚本错误（供 docs/09 拼接 VM 栈 + 宿主栈）。 */
typedef struct mq_script_error {
    uint32_t     struct_size;
    mq_script_status status;
    const char*  message;    /* UTF-8，归 VM 实例，下一次错误前有效 */
    const char*  vm_stack;   /* VM 栈摘要（文件:行:列 列表） */
    const char*  source_name;/* 脚本名/路径 */
    uint32_t     line; uint32_t col;
} mq_script_error;

/* ==== 统一后端 ABI（docs/14 §1 扩展版）==== */
typedef struct mq_script_vm_v1 {
    uint32_t struct_size;    /* = sizeof(mq_script_vm_v1) */
    uint32_t abi_version;    /* 主版本不匹配 → E_ABI */

    /* —— 生命周期 —— */
    int  (*create)(void* host, const mq_script_vm_opts* opts,
                   const mq_allocator* a, void** out_vm);
    void (*destroy)(void* vm);

    /* —— 源与执行 —— */
    int  (*eval)(void* vm, const char* src, const char* name);          /* 编译+执行模块顶层 */
    int  (*call)(void* vm, const char* fn, const mq_script_val* args, uint32_t argc,
                 mq_script_val* out_rets, uint32_t retc);               /* 同步调用，禁阻塞 */
    int  (*call_async_step)(void* vm, uint32_t handle, uint32_t steps); /* 步进；帧边界驱动 */

    /* —— 宿主绑定（由反射生成器调用，禁止手写）—— */
    int  (*register_host_fn)(void* vm, const char* sig, void* fn, uint64_t cap_required);
    int  (*register_type)(void* vm, uint64_t type_id, const void* type_desc); /* 反射描述符 */
    int  (*register_instance)(void* vm, uint32_t handle, const char* script_path);

    /* —— 生命周期回调注册（引擎调用；脚本只声明）—— */
    int  (*bind_callback)(void* vm, uint32_t instance, mq_script_cb cb, const char* fn);

    /* —— GC：仅帧边界驱动，禁止帧内隐式 —— */
    int  (*gc_step)(void* vm, size_t budget);        /* 帧边界；帧内调用 → E_IN_GC_FORBIDDEN */
    void (*gc_set_frame_open)(void* vm, int open);   /* 引擎进入/离开模拟帧时切换闸门 */

    /* —— capability 门控 —— */
    int  (*set_capabilities)(void* vm, uint64_t mask);
    int  (*has_capability)(void* vm, uint64_t cap);

    /* —— 诊断 —— */
    const mq_script_error* (*last_error)(void* vm);
    int  (*tick_metrics)(void* vm, uint64_t* out_instructions, uint64_t* out_gc_pause_ns);

    /* —— 热重载：帧边界替换 + 状态迁移 —— */
    int  (*snapshot_state)(void* vm, uint32_t instance, mq_script_val* out_state, mq_allocator* a);
    int  (*restore_state) (void* vm, uint32_t instance, const mq_script_val* in);
} mq_script_vm_v1;

/* —— 宿主侧绑定注册入口（每个 VM 后端实现；由 codegen 产出的注册函数调用）—— */
typedef struct mq_script_host_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void*    host;                 /* 不透明宿主上下文（executor/scene/log） */
    const mq_script_codec_v1* codec;
    mq_allocator* alloc;
} mq_script_host_v1;

/* 模块查询入口（docs/02 §3）。 */
void* mq_module_query(const char* interface_name, uint32_t min_version);
/* interface_name == "maque.script.v1" → 返回 const mq_script_vm_v1*（当前后端实现）。 */
```

### 2.2 类型清单

| 类型 | 类别 | 说明 |
|------|------|------|
| `mq_allocator` | POD vtable | 跨 `.so` 分配契约；本模块只经它分配。 |
| `mq_script_status` | enum | 错误码（§2.4）。 |
| `mq_script_vm_kind` | enum | 后端种类；未编译者 `E_UNSUPPORTED`。 |
| `mq_script_val` | POD 视图 | 脚本标量/句柄/列表；字符串与 items 不拥有（调用方保证存活）。 |
| `mq_script_codec_v1` | POD vtable | `to_script`/`from_script`，由序列化中枢提供。 |
| `mq_script_vm_opts` | POD | 配置：后端/能力/沙箱/seed/预算。 |
| `mq_script_cb` | enum | 6 个生命周期回调，编号即确定顺序。 |
| `mq_script_error` | POD 只读视图 | 结构化错误；字符串归 VM。 |
| `mq_script_vm_v1` | POD vtable | **统一后端 ABI**（完整操作面）。 |
| `mq_script_host_v1` | POD | 宿主注入（host/codec/alloc）。 |
| `void* vm` | 不透明句柄 | VM 实例上下文；**禁裸指针泄漏**。 |

### 2.3 操作面语义（逐函数）

| 函数 | 语义 / 参数 / 返回 | 错误 |
|------|-------------------|------|
| `create` | 按 `opts` 建 VM 实例；绑定 `host`/`codec`/`alloc`；设置 capabilities/沙箱/seed/预算。 | `E_ABI`,`E_INVALID_ARG`,`E_UNSUPPORTED`,`E_OOM` |
| `destroy` | 释放 VM 及其全部脚本对象与实例；对 `NULL` 幂等。**不**在帧内调用。 | — |
| `eval` | 编译并执行模块顶层（`name` 为诊断用源名）。编译错误 → `E_COMPILE` 并填 `last_error`。 | `E_COMPILE`,`E_RUNTIME`,`E_BUDGET` |
| `call` | 同步调用 `fn`，入参 `args`，返回 `retc` 个值到 `out_rets`；**禁阻塞**、**禁异步**。 | `E_NOT_FOUND`,`E_TYPE_MISMATCH`,`E_RUNTIME`,`E_BUDGET` |
| `call_async_step` | 对挂起的异步/长任务按步推进（每帧固定 steps，保证确定）。 | `E_NOT_FOUND`,`E_BUSY` |
| `register_host_fn` | 注册一个宿主函数：`sig` 为规范化签名（§2.6），`fn` 为 C 函数，`cap_required` 门控。**由生成器调用**。 | `E_INVALID_ARG`,`E_UNSUPPORTED` |
| `register_type` | 注册反射类型描述符，供绑定层构造/读写实例（句柄化）。 | `E_TYPE_MISMATCH` |
| `register_instance` | 把 `script_path` 绑定到场景节点/实体 `handle`，建脚本实例。 | `E_NOT_FOUND`,`E_INVALID_ARG` |
| `bind_callback` | 为该实例的某生命周期回调绑定脚本函数名；引擎在固定点调用。 | `E_NOT_FOUND` |
| `gc_step` | **仅帧边界**运行 cycle GC，最多 `budget` 单位；帧内调用 → `E_IN_GC_FORBIDDEN`。 | `E_IN_GC_FORBIDDEN`,`E_BUSY` |
| `gc_set_frame_open` | 引擎进入模拟帧置 `1`（关 GC 闸），离开置 `0`（开闸）。 | `E_INVALID_ARG` |
| `set_capabilities`/`has_capability` | 设置/查询能力掩码；越权 API 调用返回 `E_CAP_DENIED`。 | `E_INVALID_ARG` |
| `last_error` | 返回上次错误只读视图（含 VM 栈 + 源名 + 行列）。 | — |
| `tick_metrics` | 读取指令数/GC 暂停，供 docs/09 指标。 | — |
| `snapshot_state`/`restore_state` | 帧边界序列化/恢复实例**脚本可见状态**（热重载状态迁移，§2.8）。 | `E_BUSY`,`E_RUNTIME` |

### 2.4 错误模型

- **无异常、无越界**：全入口 `noexcept` 语义，错误以负 `mq_script_status` 返回；**绝不让 VM 异常逃逸跨 `.so`**。
- **结构化上下文**：失败时填 `mq_script_error`（`message` + `vm_stack` + `source_name` + `line/col`），由 docs/09 与**宿主栈拼接**。
- **不变量**：成功（`0`）时 `out*` 完整填充；失败时 `out*` 内容未定义但**不泄漏**（内部回滚 VM 栈与临时值）。
- **看门狗**：`call`/`eval` 超 `time_budget_us` → 中断并返回 `E_BUDGET`（中断点确定为指令边界，保证可复现）。

### 2.5 capability 门控（沙箱）

- `register_host_fn` 携带 `cap_required`；调用时若 VM 未持该位 → `E_CAP_DENIED`，**不进入**宿主编函数体。
- 可信本地内容可 `MQ_CAP_ALL`；**远程内容**（`opts.sandbox=1`）默认仅 `SCENE_READ|ASSET_READ|LOG|RNG|TIME`，
  **永不含** `NET`、`FS_WRITE`；`FS_READ` 仅限只读挂载白名单（docs/14 §6、ADR-0030）。
- 系统熵**永不**授权：`Math.random`/`os.urandom` 等在沙箱下要么被禁用，要么重定向到引擎 RNG（§2.6）。
- 全程经**句柄**（docs/07），脚本无法构造裸指针；`register_type` 返回的也是句柄工厂。

### 2.6 反射生成绑定契约（禁止手写）

- **签名规范（冻结）**：`"<ret> ns.Class::method(<arg1>,<arg2>,...)"`，类型用反射 fqname 或标量记号
  （`i32/f64/bool/str/handle<world::Enemy>`）；例：`"i32 world::Enemy::damage(world::Entity,int32_t)"`。
- **生成物**：codegen 从 `schema/` + API 声明生成：
  1. `gen/script_bindings.c`：对每个 API 调 `register_host_fn(vm, sig, glue_fn, cap_required)`；
  2. `glue_fn`：用 `mq_script_codec_v1` 把 `mq_script_val[]` ↔ POD（经 `to_script/from_script`）；
  3. `gen/script_bindings.h`：注册入口 `void mq_script_register_generated(const mq_script_host_v1*, mq_script_vm_v1*)`。
- **不变量**：`glue_fn` 全部**生成**；仓库中 `modules/script/` 下**除 `gen/` 外不得出现 `register_host_fn` 调用**（CI grep 门禁）。
- **往返规则**：POD `memcpy` 直通；`entity_ref/asset_ref<T>` → `MQ_SV_HANDLE`（不暴露指针）；
  `T[]/map/string` → `MQ_SV_LIST`/`MQ_SV_STRING`（经 codec）；`transient` 字段不跨脚本边界。

### 2.7 沙箱与热重载

- **沙箱**：`opts.sandbox=1` 时，VM 禁止原生宿主调用（无 FFI/`requireNative`），仅能力白名单 API；
  叠加**预算看门狗**（时间/内存/指令）；浏览器平台再叠加 WASM 沙箱（docs/14 §9，双层隔离）。
- **热重载**：源文本变更监听 → **帧边界**（`gc_set_frame_open(0)` 后）执行：
  `snapshot_state(instance)` → 重建 VM 上下文/重编译 → `restore_state(instance)`。帧内**绝不**重载。
- **析构副作用延迟**：真正释放 GPU/文件等外部资源排在**确定点**（帧边界析构队列），不在 GC 回调内立即做。

### 2.8 内存所有权

| 所有权 | 规则 |
|--------|------|
| **VM 与脚本对象** | 归 VM 实例（quickjs refcount + 帧边界 GC）；`destroy` 统一释放。 |
| **`mq_script_val` 字符串 / `items`** | **不拥有**：指向调用方缓冲或 VM 内部；存活期内有效（调用返回后立即使用）。 |
| **`args`/`out_rets` 缓冲** | 调用方经 `alloc` 分配并释放（分配者释放）。 |
| **`last_error` 字符串** | 归 VM，下一次错误或 `destroy` 前有效。 |
| **host/codec/alloc** | 调用方持有，本模块只借用。 |
| **禁** | 跨 `.so` 传 `std::string/vector/shared_ptr`；A 分配 B 释放；脚本持裸指针。 |

### 2.9 版本化与前向兼容（docs/02 R7）

1. **`struct_size` 只增不改**：新版本在 `mq_script_vm_v1` 表尾追加函数；旧调用方按自身 `struct_size` 截断使用。
2. **`abi_version` 主版本不匹配即拒绝**，返回 `E_ABI`。
3. **接口名字符串稳定**：`"maque.script.v1"`；`mq_module_query` 以 `min_version` 协商。
4. **VM 语义版本**：每个后端声明其实现的主次版本；绑定层只依赖统一 ABI，**不依赖具体 VM**（换 VM 只换 `.so`）。

## 3. 依赖与边界

- **可依赖**：
  - **反射运行时 + codegen**（docs/06）：`mq_type_desc`/`mq_field_desc`/`mq_enum_desc`、`type_id`、`read_value/write_value`、visitor。
  - **序列化中枢 codec**（docs/08）：经 `mq_script_codec_v1` 做 POD↔脚本值转换。
  - **场景/世界门面**（docs/12）：按 `{entity, field_path}` 读写、生命周期固定点、句柄。
  - **allocator / 日志 host 接口**（docs/02/07/09）；**引擎 RNG**（docs/10/D2）；**executor 模拟线程**（docs/05）。
  - **quickjs**（第三方，vendored，编译进本模块 `.so`；可换后端为可选 CMake target）。
- **禁止**：
  - 依赖上层模块（renderer 等）实现；依赖具体 VM 的 ABI 泄漏到公共头。
  - **手写绑定**；热路径过虚函数（`call`/`gc_step` 之外的反射只走冷路径）。
  - **帧内隐式 GC**；脚本直连系统熵；脚本阻塞/发起异步/跨线程共享可变 JS 对象。
  - 跨边界异常/RTTI/STL/裸指针；可变全局/单例。
- **与其它模块的接缝**：

| 模块 | 接缝 |
|------|------|
| core/reflect(02/06) | 绑定**唯一来源**；`mq_type_desc` 驱动 codegen；脚本实例经句柄访问。 |
| persistence(08) | `codec_v1` 即序列化中枢 visitor；状态快照/热重载复用同一 codec。 |
| scene(12) | 生命周期固定点调用脚本回调；组件 `script` 属性携带脚本路径。 |
| executor(05) | 脚本运行在模拟线程；`gc_step`/重载只在帧边界。 |
| control plane(10/11) | `Script` domain：eval/断点/单步/热重载命令；事件 `Script.error`。 |
| observability(09) | 结构化异常（VM 栈 + 宿主栈）；脚本耗时/GC 暂停计入指标。 |

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。**独立单测**：仅依赖 `mq_script.h` + 一个 **fake VM**，**不依赖 quickjs**；
> 真实 quickjs 只在集成套件中。上游 scene/reflect/codec 一律用 fake。

### 4.1 测试替身

| 替身 | 用途 |
|------|------|
| `fake_vm` | 实现完整 `mq_script_vm_v1`：脚本值存内存树，`register_host_fn` 记录签名，`call` 按脚本化的伪指令执行；**可精确计数 GC 调用次数与时机**。 |
| `fake_codec` | 实现 `mq_script_codec_v1`，对固定 schema 做 POD↔`mq_script_val`。 |
| `fake_reflect` | 固定描述符（含 `entity_ref`、`transient`、枚举、`T[]`）。 |
| `fake_scene` | 假节点/句柄表 + 生命周期驱动器（按固定点与深序调用回调）。 |
| `fake_rng` | 引擎 RNG 桩（同 seed 同序列）。 |
| `fake_alloc` | 记账 allocator，最终断言零泄漏（`alloc==free`）。 |
| `fake_clock` | 控制预算/超时判定，验证看门狗确定性。 |

### 4.2 单元测试用例清单（fake VM）

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| U01 | create/destroy | 合法 opts | `0`；`fake_alloc` 零泄漏 | 正常 |
| U02 | ABI 版本拒绝 | `abi_version=99` | `E_ABI`，不进入实现 | 版本兼容 |
| U03 | struct_size 截断 | 旧较小 `struct_size` | 只用已声明函数段，无越界 | 版本兼容 |
| U04 | eval 编译错误 | 非法源码 | `E_COMPILE`，`last_error.line/col` 正确 | 错误 |
| U05 | eval 运行期错误 | 抛异常脚本 | `E_RUNTIME`，`vm_stack` 非空 | 错误 |
| U06 | call 正常 | 已知函数 + 参数 | 返回期望值，`out_rets` 填充 | 正常 |
| U07 | call 缺函数 | 不存在 fn | `E_NOT_FOUND` | 错误 |
| U08 | call 参数不符 | 签名 int32 传 string | `E_TYPE_MISMATCH` | 错误 |
| U09 | 未编译后端 | `kind=MQ_VM_V8`（未裁剪） | `create` 返回 `E_UNSUPPORTED` | 边界 |
| U10 | 空指针入口 | `vm=NULL`/`out=NULL` | `E_INVALID_ARG`，不崩溃 | 边界 |
| U11 | **帧内禁隐式 GC** | `gc_set_frame_open(1)` 后调 `call`/`eval`，脚本含大量分配 | fake_vm 记录 GC 调用次数 == 0 | 确定性 |
| U12 | 帧边界 GC 生效 | `gc_set_frame_open(0)` 后 `gc_step` | GC 执行 1 次，回收可回收对象 | 正常 |
| U13 | 帧内显式 GC 被拒 | 帧开时调 `gc_step` | `E_IN_GC_FORBIDDEN` | 错误 |
| U14 | 反射绑定往返（标量） | `i32/f64/bool` POD → `to_script` → `from_script` | POD 逐位相等 | 绑定往返 |
| U15 | 绑定往返（句柄） | `entity_ref` 字段 | `MQ_SV_HANDLE`，index/generation 保留；无裸指针 | 绑定往返 |
| U16 | 绑定往返（容器） | `T[]`/`string`/枚举 | LIST/STRING/ENUM 编码一致，往返相等 | 绑定往返 |
| U17 | transient 不跨界 | 读含 `_scratch`（transient）的实例 | 脚本值中**无**该字段 | 绑定往返 |
| U18 | 生成器注册 | 调 `mq_script_register_generated` | `register_host_fn` 被调用 N 次，签名与 schema 一致 | 正常 |
| U19 | 手写绑定门禁 | 扫描非 `gen/` 目录 | 无 `register_host_fn`/`register_type` 调用 | 规范 |
| U20 | **capability 拒绝** | VM 无 `FS_WRITE`，调写文件 API | `E_CAP_DENIED`，宿主编函数体未执行 | 沙箱 |
| U21 | **沙箱越权（net）** | 沙箱 VM 调网络 API | `E_CAP_DENIED` | 沙箱 |
| U22 | 沙箱默认能力 | `sandbox=1` 缺省 | 掩码 = SCENE_READ\|ASSET_READ\|LOG\|RNG\|TIME，无 NET/FS_WRITE | 沙箱 |
| U23 | set/has_capability | 动态授权 FS_READ | 查询一致；调用放行 | 沙箱 |
| U24 | 系统熵重定向 | 脚本调 `Math.random`（沙箱） | 走引擎 RNG；同 seed 同序列 | 确定性 |
| U25 | 时间预算 | `time_budget_us` 很小 + 长循环 | `E_BUDGET`，中断点在指令边界 | 边界 |
| U26 | 内存预算 | `mem_budget` 很小 + 大量分配 | `E_BUDGET`，无泄漏 | 边界 |
| U27 | 生命周期回调注册 | `bind_callback` 六个回调 | 记录；未绑定的回调为空 | 正常 |
| U28 | 热重载状态快照 | `snapshot_state` → 改脚本 → `restore_state` | 可见状态（HP/位置等）迁移后一致 | 正常 |
| U29 | 帧内重载被拒 | 帧开时重载 | `E_BUSY`（只允许帧边界） | 错误 |
| U30 | 错误结构化 | 触发运行期错误 | `last_error` 含 message/vm_stack/source/line | 诊断 |
| U31 | OOM 注入 | alloc 第 N 次失败 | `E_OOM`，已分配全回收 | 内存所有权 |
| U32 | destroy 幂等 | `destroy(NULL)` / 重复 destroy | 不崩溃 | 边界 |
| U33 | 指标读取 | 执行若干 call | `tick_metrics` 指令数 > 0 | 诊断 |

> **独立单测保障**：U01–U33 全经 `fake_vm`/`fake_codec`/`fake_reflect`/`fake_scene`/`fake_alloc` 运行；
> CMake `script_tests` target **不链接** quickjs、scene、persistence 实现；`-fvisibility=hidden` 只导出 `mq_module_query`。

### 4.3 真实 quickjs 集成用例

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| Q1 | eval + call 冒烟 | `function add(a,b){return a+b}`；call add(2,3) | 5 | 集成 |
| Q2 | 反射绑定实调 | 生成绑定 + quickjs 调 `world::Transform` setter/getter | POD 经序列化往返正确 | 集成 |
| Q3 | 游戏 API 绑定 | 脚本调 `Enemy::damage(handle,n)`（经生成 glue） | 假 scene 中目标 HP 减少 | 集成 |
| Q4 | 生命周期顺序 | 注册六回调 + fake_scene 驱动 | 调用序列 = enter→ready→process→physics_process→exit→destroy | 集成/确定性 |
| Q5 | 帧内 GC == 0 | quickjs 执行分配密集脚本（帧开） | 埋点断言 `gc_step` 未在帧内被隐式触发（用 JS_ 钩子计数） | 确定性 |
| Q6 | 帧边界 GC 回收 | 丢弃引用后帧边界 `gc_step` | 对象被回收（weakref 计数下降） | 集成 |
| Q7 | 沙箱越权 | 沙箱 quickjs 调未授权 API | `E_CAP_DENIED`，无副作用 | 沙箱 |
| Q8 | 热重载迁移 | 改脚本 + 帧边界重载 | 实例状态保留，新逻辑生效 | 集成 |
| Q9 | 结构化异常 | quickjs 抛错 | `last_error.vm_stack` 含脚本文件:行 | 诊断 |
| Q10 | 可选后端（若裁剪） | `kind=LUA`（编译时开启） | 同 U06/Q1 用例通过同一套绑定 | 集成 |

### 4.4 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S1 | **脚本驱动敌人 AI + 攻击** | 节点挂 `enemy.js`；`ready` 缓存玩家句柄；`process(dt)` 追逐（读 `Transform`）；进入攻击距离调 `Enemy::damage(player, dmg)`（经反射绑定） | 玩家 HP 按脚本确定递减；位置逼近玩家；无手写绑定参与 |
| S2 | **帧内无隐式 GC（回放一致）** | 连续 N 帧运行 S1，中途制造大量临时对象 | 每帧 `process` 期间 GC 计数==0；仅帧边界回收 |
| S3 | **沙箱远程游戏** | 以 `sandbox=1` 加载远程 `enemy.js`，尝试 `fs.write`/`net.post` | 两次均 `E_CAP_DENIED`；游戏逻辑（SCENE_READ/WRITE 白名单内）正常 |
| S4 | **确定性回放** | 同一 seed + 同输入序列 + 同脚本，记录两帧的 `(指令数, RNG 值, 伤害序列, checksum)` | 两次轨迹**逐帧字节一致** |
| S5 | **热重载迭代（A5）** | 运行中把敌人 dmg 从 10 改 20，帧边界重载 | 实例 HP/位置保留；后续攻击 dmg=20 |
| S6 | **反射新增 API 自动暴露** | 向 `schema/` 加 `Enemy::heal(i32)` → 重生成 → 脚本调用 | 无需手写代码即生效；CI 门禁通过 |
| S7 | **脚本异常 → 定位** | 脚本故意越界访问句柄 | `E_RUNTIME` + `vm_stack`+宿主栈拼接，指向源行 |
| S8 | **生命周期清理** | 销毁敌人节点 | `on_exit_tree`→`on_destroy` 依序调用；外部资源在帧边界析构；零泄漏 |

### 4.5 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| X1 | 同 seed 同脚本轨迹 | S1/S4 两次运行逐帧 checksum 相同 |
| X2 | RNG 一致 | 脚本随机数序列 == 引擎 RNG 序列（禁系统熵） |
| X3 | GC 时机一致 | 帧内 GC 计数恒为 0；帧边界回收顺序确定（按对象创建序/引用序） |
| X4 | 生命周期顺序一致 | 多次运行回调序列字节一致（深序 + 声明序） |
| X5 | 绑定往返无损 | POD→脚本值→POD 逐位相等（含边界值：NaN/±0/最大整数/空串） |
| X6 | 跨运行无地址/哈希依赖 | 强制不同分配地址，S4 checksum 不变 |

### 4.6 覆盖率与性能目标

- 语句/分支覆盖率 **≥ 85%**（`modules/script/` 非生成代码；生成 `gen/` 计入绑定往返覆盖）。
- 性能预算（参考机，quickjs）：纯脚本 `call`（无宿主调用）**< 2 µs/次**；单次反射绑定调用（标量）**< 5 µs**；
  帧边界 `gc_step` 对 10k 活对象 **< 5 ms**；脚本加载+编译 64 KB 源 **< 20 ms**。
- 内存：VM 空载常驻 **< 2 MB**（quickjs）；`mem_budget` 超限前可稳定触发 `E_BUDGET`；`fake_alloc` 全程零泄漏。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：定稿 `mq_script.h`（`mq_script_vm_v1`/`mq_script_val`/`codec_v1`/capability/错误码）、
   签名规范（§2.6）、生命周期回调枚举与固定顺序、`MQ_SCRIPT_ABI_VERSION=1`。**产出即冻结**。
2. **阶段 1 · 测试骨架（红）**：实现 `fake_vm`/`fake_codec`/`fake_reflect`/`fake_scene`/`fake_alloc`；
   落 U01–U33 骨架全部失败；CI 加入「非 `gen/` 不得含 `register_host_fn`」门禁。
3. **阶段 2 · 最小实现（绿，fake）**：以 `fake_vm` 把 U01–U10、U27、U30、U32 转绿：
   create/destroy、eval/call、register_host_fn/type、bind_callback、last_error、版本规则。
4. **阶段 3 · 绑定生成 + 往返**：扩展 codegen 产出 `gen/script_bindings.{c,h}` 与 `glue_fn`；
   落 U14–U19、X5（POD↔脚本值往返、transient 不跨界、句柄化、生成器注册、手写门禁）。
5. **阶段 4 · GC 确定性 + capability/沙箱**：`gc_set_frame_open`/`gc_step` 闸门语义（U11–U13、X3）；
   capability 门控与沙箱默认能力（U20–U24、S3）；看门狗预算（U25–U26）。
6. **阶段 5 · quickjs 集成**：接真实 quickjs 实现后端（凭 `mq_script_vm_v1`），落 Q1–Q9；生命周期与场景驱动（Q4、S1、Q8）。
7. **阶段 6 · 热重载 + 诊断 + 预算**：`snapshot/restore_state` 状态迁移（U28–U29、S5）；结构化异常拼接（S7）；
   指标（U33）；性能/内存达标；clang-tidy 清零。
8. **阶段 7 · 可选后端验证（可选）**：编译并冒烟 lua/python/v8 中至少一个，跑同套 U06/U08/Q1/绑定往返，验证「换 VM 不换绑定层」。

**里程碑**：M1 接口冻结 → M2 fake_vm 单测绿 → M3 绑定生成+往返绿 → M4 GC/capability/沙箱绿 →
M5 quickjs 集成+场景绿 → M6 热重载/诊断/确定性绿 → M7 可选后端冒烟。

## 6. 验收标准（DoD）

- 接口头 `mq_script.h` 冻结，被上游 fake（reflect/codec/scene）依赖编译通过；接口名 `"maque.script.v1"`。
- 单测覆盖率 **≥ 85%**；§4.2 全绿，`fake_alloc` 全程**零泄漏**。
- **绑定全生成**：CI 门禁确认非 `gen/` 无手写绑定；新增 API 经 codegen 自动暴露（S6）。
- **帧内禁隐式 GC 通过**：U11/Q5 断言帧内 GC 计数==0；U12/Q6 帧边界回收生效；U13 帧内显式 GC 被拒。
- **绑定往返无损**：U14–U17、X5 通过（含边界值与句柄/容器/transient）。
- **沙箱越权拒绝**：U20–U23、Q7、S3 通过；沙箱默认能力掩码正确，无 NET/FS_WRITE。
- **确定性**：X1–X6、S4 通过（同 seed 轨迹逐帧一致；RNG 走引擎；生命周期顺序稳定）。
- **游戏场景**：S1（脚本驱动敌人 AI + 攻击经反射绑定）端到端通过。
- 可选后端（lua/python/v8）至少冒烟通过同套绑定（阶段 7，可延后但接口须支持）。
- 无跨模块实现依赖；无全局可变状态；无跨边界异常/RTTI/STL/裸指针；性能/内存预算达标（§4.6）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **隐式 GC 破坏回放** | 闸门 `gc_set_frame_open` + 帧内 `gc_step` 拒绝；U11–U13/Q5 埋点断言；quickjs 保留字节码/延迟 GC 钩子 |
| **手写绑定漂移** | 绑定一律 codegen；CI grep 门禁（非 `gen/` 禁 `register_host_fn`）；S6 自动暴露验证 |
| **远程脚本越权** | capability 门控 + 沙箱默认白名单 + 预算看门狗；U20–U23/Q7/S3；系统熵永不授权 |
| **VM 语义分叉（多 VM）** | 统一 `mq_script_vm_v1` + 生成绑定层；只对其中间语义（`mq_script_val`）做保证；二期再评估可选后端 |
| **脚本阻塞模拟** | 禁阻塞/异步；`call` 同步；`call_async_step` 帧边界步进；看门狗 `E_BUDGET` |
| **脚本栈不完整** | `mq_script_error` 拼 VM 栈 + 宿主栈（docs/09）；U30/Q9/S7 |
| **热重载状态丢失** | 帧边界 `snapshot/restore_state`；U28/S5；帧内重载 → `E_BUSY` |
| **绑定往返丢失精度** | codec 用整数/最短往返浮点表示；X5 含 NaN/±0/极值；禁 locale |
| **VM 内存膨胀（嵌入式）** | 构建期裁剪 + `mem_budget` 强制；空载 < 2 MB；`E_BUDGET`/`E_OOM` 用例 |
| **quickjs 版本/补丁漂移** | vendored 固定版本，pin commit；换后端只替换 `.so`，绑定层不动 |

## 8. Owner agent

- **角色标识**：`agent-script`
- **职责边界**：仅负责 `modules/script/{include,src,gen,backends/quickjs}` 的接口（`mq_script.h`）、
  codegen 绑定模板、测试与实现；**不修改其它模块**。对 reflect/codec/scene/executor 仅经稳定接口消费，以其 fake 完成独立单测。
- **交付**：`plans/16-script.md`（本文件）→ `modules/script/`（`mq_script.h`、`backends/`、`gen/`、测试）。
- **上游协作**：向 `agent-core` 约定绑定描述符字段（签名来源）；向 `agent-persistence` 约定 `mq_script_codec_v1` visitor；
  向 `agent-scene` 约定生命周期固定点与句柄；向 `agent-executor` 约定帧边界驱动 `gc_step`/热重载；向 `agent-control` 对齐 `Script` domain。
