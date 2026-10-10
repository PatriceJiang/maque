# 06 · VFS（虚拟文件系统 / 多协议 / 挂载）

> 复制自 `_template.md`。接口与测试章节先于实现冻结。
> 上游主参考：`docs/04-虚拟文件系统与多协议.md`；边界规则：`docs/02-模块系统-ABI与多实例.md`；总纲：`docs/00-设计哲学与约束.md`（ADR-0015 / ADR-0013 / ADR-0010 / ADR-0030）。

## 1. 模块概述

- **职责**：提供**统一路径空间**，把多种协议（`file/res/mem/zip/http(s)/ftp/data`）的后端以 **Provider `.so`（C ABI 版本化 vtable）** 挂载进同一棵挂载点树；对上层（persistence、资产管线、脚本、控制平面）屏蔽协议与来源差异，统一 `stat/open/read/readdir` 语义与错误码。VFS 位于 persistence / 资产管线**之下**，是它们的 IO 底座。
- **所在层**：运行时层（runtime / IO）。被数据层（persistence、asset）依赖，不依赖它们。
- **上游文档**：`docs/04`（主）、`docs/02`（provider 为 C ABI 模块）、`docs/00`（ADR-0015 引入 VFS、ADR-0010 异步、ADR-0030 内容哈希、ADR-0014 无全局可变状态）。
- **一句话**：让引擎像浏览器一样"按路径/URL 加载游戏"，而所有调用方只看到一条稳定路径。

## 2. 接口层设计（完备，先冻结）

> 一律 **C ABI + 版本化 vtable**（docs/02 R1–R8）。接口冻结前不得进入实现。
> 命名空间：接口名 `"maque.vfs.v1"` 与 `"maque.fs_provider.v1"`（稳定字符串 ID）。

### 2.1 公共头 `mq_vfs.h`（纯 C，示意但可编译的完整面）

```c
/* mq_vfs.h —— 纯 C，extern "C"；边界上无 C++ / 无异常 / 无 STL */
#include <stddef.h>
#include <stdint.h>

#define MQ_VFS_ABI_VERSION      1u
#define MQ_FS_PROVIDER_ABI_VERSION 1u

/* ---- 结果码：所有入口 noexcept，用返回码表达错误（docs/02 R2） ---- */
typedef enum mq_result {
    MQ_OK               =  0,
    MQ_ERR_NOT_FOUND    = -1,   /* 路径不存在 */
    MQ_ERR_ACCESS       = -2,   /* 权限 / 能力不足 */
    MQ_ERR_TRAVERSAL    = -3,   /* 路径穿越（挂载根之外，§9）被拒绝 */
    MQ_ERR_SCHEME       = -4,   /* 未知 scheme / provider 未注册 */
    MQ_ERR_MOUNT        = -5,   /* 挂载冲突或挂载点非法 */
    MQ_ERR_VERSION      = -6,   /* abi_version 主版本不匹配 */
    MQ_ERR_READONLY     = -7,   /* 只读挂载上的写操作 */
    MQ_ERR_INVALID_ARG  = -8,
    MQ_ERR_NO_MEMORY    = -9,
    MQ_ERR_IO           = -10,  /* 底层 IO 失败（含网络） */
    MQ_ERR_EOF          = -11,
    MQ_ERR_HASH_MISMATCH= -12,  /* 内容哈希校验失败（ADR-0030） */
    MQ_ERR_CANCELED     = -13,
    MQ_ERR_PENDING      = -14,  /* 异步任务未完成（轮询语义，非错误） */
    MQ_ERR_UNSUPPORTED  = -15,  /* provider 未实现该操作 */
} mq_result;

/* ---- 基础 POD 类型（固定宽度、UTF-8 char*） ---- */
typedef enum mq_open_mode {
    MQ_OPEN_READ  = 1u << 0,
    MQ_OPEN_WRITE = 1u << 1,
    MQ_OPEN_APPEND= 1u << 2,
    MQ_OPEN_CREATE= 1u << 3,
} mq_open_mode;

typedef enum mq_file_kind {
    MQ_FILE_REGULAR = 0,
    MQ_FILE_DIR     = 1,
    MQ_FILE_ARCHIVE = 2,
    MQ_FILE_OTHER   = 3,
} mq_file_kind;

typedef struct mq_stat {
    uint32_t     struct_size;   /* = sizeof(mq_stat) */
    mq_file_kind kind;
    uint64_t     size;          /* 字节数；目录为 0 */
    uint64_t     mtime_ns;      /* UNIX epoch 纳秒；未知为 0 */
    uint32_t     readonly;      /* 1 = 只读 */
    uint32_t     _pad;
} mq_stat;

/* 目录项回调：路径为 provider 相对路径的 UTF-8；buffer 生命周期仅在回调内有效 */
typedef void (*mq_dir_cb)(void* ud, const char* name, const mq_stat* st);

/* ---- 句柄（docs/02 R4：不透明 + 世代，禁止裸指针跨边界） ---- */
typedef struct mq_vfs         mq_vfs;         /* 实例上下文 */
typedef struct mq_provider    mq_provider;    /* provider 实例 */
typedef struct mq_stream      mq_stream;      /* 打开的流 */
typedef struct mq_io_task     mq_io_task;     /* 异步任务句柄 */

typedef struct mq_mount_id {   /* 挂载世代句柄 */
    uint32_t index;
    uint32_t generation;
} mq_mount_id;

/* ---- 异步任务（对接 executor，ADR-0010） ---- */
typedef enum mq_task_state { MQ_TASK_PENDING=0, MQ_TASK_DONE=1, MQ_TASK_FAILED=2, MQ_TASK_CANCELED=3 } mq_task_state;
typedef void (*mq_io_done_cb)(void* ud, mq_result r, size_t bytes);

/* ---- 分配器：宿主注入，分配者释放（docs/02 R5） ---- */
typedef struct mq_allocator {
    uint32_t struct_size;
    void* (*alloc)(void* self, size_t size, size_t align);
    void  (*free)(void* self, void* ptr);
    void* self;
} mq_allocator;

/* =========================================================
 *  Provider vtable（由各协议 .so 实现并注册）
 *  宿主经 mq_module_query("maque.fs_provider.v1") 取得注册入口
 * ========================================================= */
typedef struct mq_fs_provider_v1 {
    uint32_t    struct_size;    /* = sizeof(mq_fs_provider_v1)，只增不改 */
    uint32_t    abi_version;    /* = MQ_FS_PROVIDER_ABI_VERSION，主版本不匹配拒绝 */

    const char* scheme;         /* "file" "res" "mem" "zip" "http" "ftp" "data" ... */

    /* mount: spec 为该挂载的配置串（如 base URL、zip 路径）；out_prov 返回 provider 实例 */
    mq_result (*mount)   (mq_vfs* host, mq_allocator* a, const char* spec, mq_provider** out_prov);
    void      (*unmount) (mq_provider* prov);

    mq_result (*stat)    (mq_provider* prov, const char* relpath, mq_stat* out);
    mq_result (*open)    (mq_provider* prov, const char* relpath, uint32_t mode, mq_stream** out_stream);
    void      (*close)   (mq_provider* prov, mq_stream* s);
    mq_result (*seek)    (mq_provider* prov, mq_stream* s, uint64_t offset);
    uint64_t  (*tell)    (mq_provider* prov, mq_stream* s);

    /* 异步读：提交后立即返回，完成经 executor 回调；out_task 归 VFS 持有 */
    mq_result (*read_async)(mq_provider* prov, mq_stream* s, void* buf, size_t n,
                            mq_io_done_cb cb, void* ud, mq_io_task** out_task);
    /* 同步读：仅供冷路径封装，热/模拟线程禁用；返回实际字节数（0=EOF） */
    mq_result (*read)      (mq_provider* prov, mq_stream* s, void* buf, size_t n, size_t* out_n);
    mq_result (*write)     (mq_provider* prov, mq_stream* s, const void* buf, size_t n, size_t* out_n);

    /* 目录枚举：cb 在此调用内同步触发；异步版返回任务 */
    mq_result (*readdir)      (mq_provider* prov, const char* relpath, mq_dir_cb cb, void* ud);
    mq_result (*readdir_async)(mq_provider* prov, const char* relpath, mq_io_done_cb cb, void* ud, mq_io_task** out_task);

    /* 可选：返回内容的期望哈希用于校验（ADR-0030）；可为 NULL */
    mq_result (*content_hash)(mq_provider* prov, const char* relpath, uint8_t out_sha256[32]);
} mq_fs_provider_v1;

/* =========================================================
 *  VFS 公共操作面（宿主调用，全部 noexcept）
 * ========================================================= */
typedef struct mq_vfs_v1 {
    uint32_t struct_size;
    uint32_t abi_version;

    /* 生命周期：host 注入 allocator 与 executor；零全局可变状态（ADR-0014） */
    mq_result (*create) (void* host, mq_allocator* a, void* executor, mq_vfs** out);
    void      (*destroy)(mq_vfs* vfs);

    /* 注册 provider 工厂（按 scheme 索引；同 scheme 后注册覆盖先注册） */
    mq_result (*register_provider)(mq_vfs* vfs, const mq_fs_provider_v1* vt);

    /* 挂载：mountpoint 如 "assets/"；source 如 "https://cdn/x/" 或 "./a.zip/inner"；
       priority 越大越优先（同前缀并列时）；flags 见 mq_mount_flags */
    mq_result (*mount)  (mq_vfs* vfs, const char* mountpoint, const char* source,
                         uint32_t flags, int32_t priority, mq_mount_id* out_id);
    mq_result (*unmount)(mq_vfs* vfs, mq_mount_id id);

    /* 路径操作（相对统一路径空间的逻辑路径） */
    mq_result (*stat) (mq_vfs* vfs, const char* path, mq_stat* out);
    mq_result (*open) (mq_vfs* vfs, const char* path, uint32_t mode, mq_stream** out_stream);
    void      (*close)(mq_vfs* vfs, mq_stream* s);
    mq_result (*seek) (mq_vfs* vfs, mq_stream* s, uint64_t offset);

    mq_result (*read_async)(mq_vfs* vfs, mq_stream* s, void* buf, size_t n,
                            mq_io_done_cb cb, void* ud, mq_io_task** out_task);
    mq_result (*read_sync) (mq_vfs* vfs, mq_stream* s, void* buf, size_t n, size_t* out_n); /* 冷路径封装 */
    mq_result (*write)     (mq_vfs* vfs, mq_stream* s, const void* buf, size_t n, size_t* out_n);
    mq_result (*readdir)      (mq_vfs* vfs, const char* path, mq_dir_cb cb, void* ud);
    mq_result (*readdir_async)(mq_vfs* vfs, const char* path, mq_io_done_cb cb, void* ud, mq_io_task** out_task);

    /* 任务查询/取消（帧边界驱动完成检查） */
    mq_task_state (*task_state)(mq_vfs* vfs, mq_io_task* t);
    mq_result     (*task_result)(mq_vfs* vfs, mq_io_task* t, size_t* out_bytes);
    void          (*task_release)(mq_vfs* vfs, mq_io_task* t);

    /* 可观测 / 自省（A1/A2）：解析结果、挂载树 dump（JSON 写入调用方缓冲区） */
    mq_result (*resolve)(mq_vfs* vfs, const char* path, char* out_scheme, size_t scheme_cap,
                         char* out_relpath, size_t rel_cap, mq_mount_id* out_mount);
    mq_result (*dump_mounts)(mq_vfs* vfs, char* out_json, size_t cap, size_t* out_n);
} mq_vfs_v1;

typedef enum mq_mount_flags {
    MQ_MOUNT_NONE      = 0,
    MQ_MOUNT_READONLY  = 1u << 0,
    MQ_MOUNT_PREFETCH  = 1u << 1,   /* 允许远程预取 */
    MQ_MOUNT_CACHE     = 1u << 2,   /* 启用内容寻址本地缓存 */
    MQ_MOUNT_SHADOW    = 1u << 3,   /* 显式声明遮蔽同名子树 */
} mq_mount_flags;
```

### 2.2 类型清单

| 类型 | 性质 | 说明 |
|------|------|------|
| `mq_result` | POD 枚举 | 统一错误码，全模块唯一错误语言 |
| `mq_stat` / `mq_file_kind` | POD | 元数据；固定布局 + `struct_size` |
| `mq_open_mode` / `mq_mount_flags` | POD 位标志 | 位组合，未知位忽略 |
| `mq_vfs` / `mq_provider` / `mq_stream` / `mq_io_task` | 不透明句柄 | 绝不暴露内部结构 |
| `mq_mount_id` | POD `{index,generation}` | 防悬垂；跨实例不通用 |
| `mq_mount_flags` | POD 位标志 | 只读/预取/缓存/遮蔽 |
| `mq_allocator` | POD vtable | 宿主注入，分配者释放 |
| `mq_fs_provider_v1` | C vtable | Provider 协议后端接口 |
| `mq_vfs_v1` | C vtable | VFS 实例操作面 |
| `mq_dir_cb` / `mq_io_done_cb` | 函数指针 | 目录项 / 异步完成回调 |

### 2.3 操作面语义（逐函数契约）

- `create`：创建独立实例上下文（allocator、executor、注册表、挂载树全在 `mq_vfs` 内）。失败返回 `MQ_ERR_*` 且 `*out` 不变。
- `register_provider`：按 `scheme` 注册 vtable 指针；VFS **不复制** vtable 生命周期，调用方须保证在 `destroy` 前有效；`abi_version` 主版本不匹配返回 `MQ_ERR_VERSION`（服务 A2，给出可读诊断）。
- `mount`：规范化 `mountpoint`（去重斜杠、补尾斜杠、拒绝 `../`）；解析 `source` 的 scheme 找 provider；调 `provider->mount` 得实例；插入挂载树；返回世代 `mq_mount_id`。同路径重复挂载按 `priority`（大者优先），相等则**后挂载优先**（遮蔽）；`MQ_MOUNT_SHADOW` 未设但同前缀已存在且 `priority` 相等时返回 `MQ_ERR_MOUNT`（强制显式，防误遮蔽）。
- `unmount`：按世代校验，失效则 `MQ_ERR_INVALID_ARG`；释放 provider、从树移除；悬挂流由 provider `close` 决定行为（约定调用后流不可再用）。
- `stat/open/seek/readdir`：先 `resolve` 得 `(provider, relpath)`，再转发；路径穿越在 `resolve` 阶段拦截（§2.7）。
- `read_async`：同步提交、立即返回（`MQ_OK` 且已触发，或 `MQ_ERR_PENDING` 表进行中）；完成经 executor 回调 `cb(ud, r, bytes)`；任务句柄归 VFS，用后必 `task_release`。
- `read_sync`：`read_async` + 阻塞等待的**冷路径封装**；`executor == NULL` 或模拟线程调用返回 `MQ_ERR_UNSUPPORTED`（防破坏确定性）。
- `write`：仅可写挂载可用，否则 `MQ_ERR_READONLY`。
- `resolve`/`dump_mounts`：纯查询，用于观测与 AI 自省；不产生 IO 副作用（远程 stat 除外，见风险）。

### 2.4 挂载解析算法（最长前缀 + 嵌套 + 显式优先级）

```
resolve(path):
  1. 规范化：折叠 ""/"."/"//"，拒绝任何 ".." 越界分量 → 否则 MQ_ERR_TRAVERSAL
  2. 在挂载表中找所有满足 path 以 mountpoint 为前缀的挂载（前缀按路径分量边界匹配，非字节前缀）
  3. 选择键 = (mountpoint 分量长度 DESC, priority DESC, registration_seq DESC)
     —— 最长前缀优先；并列时高优先级优先；再并列时后挂载优先
  4. relpath = path 去掉 mountpoint 后的剩余部分
  5. 若 relpath 为空 → 指向挂载根目录
  6. 返回 (provider, relpath, mount_id)；无匹配 → 隐式内置 mem:// 空挂载或 MQ_ERR_NOT_FOUND
```

- **字节前缀陷阱**：`assets` 不应匹配 `assets2/x`；必须按 `/` 分量边界匹配。
- **嵌套**：更长的 `mountpoint` 天然遮蔽父挂载的子树（如 `assets/musics/` 覆盖 `assets/`）。

### 2.5 错误模型

- 全模块统一使用 `mq_result`（因 C ABI 禁异常，docs/02 R2）。C++ 内部实现用 `std::expected<T, mq_result>`，边界处**只**传返回码（ADR-0012 允许 `expected`，但不跨边界）。
- 约定：负值为错误，`MQ_OK=0`；`MQ_ERR_PENDING`/`MQ_ERR_EOF` 是"非失败"的状态返回，调用方需区分。

### 2.6 版本化

- 两个 vtable 均以 `struct_size` + `abi_version` 打头。**`struct_size` 只增不改**：新版本在表尾追加函数指针，旧调用方按自身 `struct_size` 截断使用（前向兼容）。
- `abi_version` 主版本不同即 `MQ_ERR_VERSION` 拒绝。接口名（`"maque.vfs.v1"`）与版本号绑定。
- Provider 侧：VFS 按 `MQ_FS_PROVIDER_ABI_VERSION` 校验；provider 能力差异用 `MQ_ERR_UNSUPPORTED` 而非崩溃。

### 2.7 内存所有权

- **分配者释放**：所有输出缓冲、provider 实例、流、任务均由 VFS/宿主经注入的 `mq_allocator` 分配与释放；调用方不得 `free` 非自己分配之物。
- **`char*` 输入**：调用方拥有、UTF-8、`create`/`mount`/`open` 调用期间有效；VFS 可复制进自身 arena。
- **`mq_dir_cb` 的 `name`**：仅回调内有效，调用方需复制以留存。
- **`dump_mounts`/`resolve`** 输出写入调用方提供的缓冲区，返回需要字节数（cap 不足时 `MQ_ERR_INVALID_ARG` 且 `*out_n` 给出所需大小）。
- **回调不重入**：`read_async` 完成回调可能在 executor 线程触发，调用方须保证 `ud` 线程安全或经帧边界同步。

### 2.8 线程 / 确定性

- 异步 IO 允许在 executor 的 reactor 线程完成；**模拟线程永不阻塞**。
- **帧内禁异步**：模拟帧内不得调用 `read_async` 触发新 IO；资产加载在帧间/加载阶段进行（docs/04 §6，ADR-0005）。帧内**不得**依赖异步完成顺序。
- `read_sync` 仅冷路径；实现内断言"非模拟线程"。
- 所有状态在 `mq_vfs` 上下文，禁止全局可变 / 单例（ADR-0014，docs/02 §5）。

## 3. 依赖与边界

- **可依赖**：
  - `core/data`：POD 类型、句柄、`std::expected`（仅模块内部）。
  - `platform`：本地 fs、网络 socket、模块加载（薄抽象，docs/05）。
  - `executor`：coroutine 提交与完成回调（唯一异步出口）。
  - `module host`：`mq_module_query` 发现 provider `.so`。
  - allocate 注入：宿主 `mq_allocator`。
- **禁止**：
  - ❌ 依赖 persistence / asset / scene 等上层模块（VFS 在它们**之下**）。
  - ❌ 跨边界传 `std::string/vector/shared_ptr/异常/RTTI`（docs/02 §9）。
  - ❌ 持有全局可变状态 / 单例（破坏多实例）。
  - ❌ 暴露裸指针内部结构；热路径（逐字节读）不得反复过 vtable。
  - ❌ 在模拟帧内触发异步 IO 或同步阻塞 IO。
  - ❌ 自行实现本地寻址而绕过 `platform` 的 fs 抽象。
- **接缝**：
  - **向下**：各 protocol provider 实现 `mq_fs_provider_v1` 并注册；VFS 只认 vtable，不认协议细节。
  - **向上**：persistence backend（file/memory/network/zip）与 asset pipeline **建立在 VFS 之上**（`serialize(data, codec, vfs_path)`，scheme 选 provider）；脚本 `fs.mount(...)`/`fs.read` 映射到 `mq_vfs_v1`。
  - **观测**：控制平面通过 `resolve`/`dump_mounts` + executor 事件读取 IO 记录（A2）。

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**独立**：仅依赖本模块 + `mq_vfs.h`；`file`/网络 provider 全部用 **`mem://` + fake provider** 替代，不碰真实磁盘/网络。

### 4.1 测试替身（fixtures）

| 替身 | 作用 |
|------|------|
| `mem_provider` | 内存字典 `{path → bytes/stat}`，P0 内置实现，覆盖全部单测 |
| `fake_provider` | 可编程返回任意 `mq_result`、可注入延迟/失败/只读，用于错误与异步路径 |
| `fake_executor` | 手动脉冲的 executor：测试精确控制异步完成时机（不依赖真实线程） |
| `fake_allocator` | 统计分配/释放次数，验证"分配者释放"与无泄漏 |
| `fault_injector` | 在 provider 指定调用点返回 `MQ_ERR_IO` / 哈希不匹配 |

### 4.2 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| 1 | 创建/销毁 | `create`→`destroy` | `MQ_OK`，`fake_allocator` 净分配为 0 | 正常/所有权 |
| 2 | 注册 provider | `register_provider(mem)` | `MQ_OK`；重复注册同 scheme 后者覆盖 | 正常 |
| 3 | 注册版本不匹配 | `abi_version=99` | `MQ_ERR_VERSION`，可读诊断 | 版本 |
| 4 | vtable 前向兼容 | 构造 `struct_size` 更小的旧表 | 正常调用旧面，不读越界 | 版本 |
| 5 | 基本挂载+stat | `mount("a/","mem://",...)` + `stat("a/f")` | `MQ_OK` 且 size/kind 正确 | 正常 |
| 6 | 简单读 | `open`+`read_async`+脉冲+`read_sync` | 字节与源一致 | 正常 |
| 7 | 最长前缀：父遮蔽 | 挂 `a/` 与 `a/b/`，读 `a/b/f` | 命中 `a/b/` 的 provider | 核心 |
| 8 | 字节前缀陷阱 | 挂 `assets/`，读 `assets2/x` | **不**命中，`MQ_ERR_NOT_FOUND` | 边界 |
| 9 | 嵌套覆盖 | `a/` 下有 `x`，`a/b/` 也定义 `x` | `a/b/x` 取内层，`a/x` 取外层 | 核心 |
| 10 | 同名优先级 | 同 `a/` 两次挂载 priority 5/10 | 高优先级命中，`resolve` 返回对应 mount | 核心 |
| 11 | 同优先级后挂优先 | 同 `a/` 两次挂载同 priority | 后挂载遮蔽先挂载 | 语义 |
| 12 | 未显式 shadow 冲突 | 同前缀同 priority 且无 `SHADOW` | `MQ_ERR_MOUNT` | 错误 |
| 13 | 卸载后回退 | 挂 `a/`+`a/b/`，卸载 `a/b/` | `a/b/x` 回退到 `a/` | 核心 |
| 14 | 卸载失效句柄 | 用旧 `mq_mount_id` 再次 unmount | `MQ_ERR_INVALID_ARG` | 边界 |
| 15 | 挂载点规范化 | `"a//b//"` / `"a/b"` | 归一为 `a/b/`，解析一致 | 边界 |
| 16 | 非法挂载点 | `"../evil/"` / `"a/../b/"` | `MQ_ERR_TRAVERSAL`/`MQ_ERR_MOUNT` | 错误 |
| 17 | **路径穿越拒绝** | 读 `"a/../../etc/passwd"` | `MQ_ERR_TRAVERSAL`，**不**触达 provider | 安全 |
| 18 | 编码穿越拒绝 | `"%2e%2e/"` 及 `..%2f` | `MQ_ERR_TRAVERSAL`（规范化后再校验） | 安全 |
| 19 | 未知 scheme | `mount("a/","nope://")` | `MQ_ERR_SCHEME` | 错误 |
| 20 | 只读写拒绝 | 只读挂载上 `write` | `MQ_ERR_READONLY` | 错误 |
| 21 | 异步读时序 | `read_async` 后不脉冲 | `task_state==PENDING`；脉冲后 `DONE` | 异步 |
| 22 | 异步读成功 | 脉冲 executor | 回调收到 `MQ_OK`+正确字节 | 异步 |
| 23 | 异步读失败 | `fault_injector` 注入 `MQ_ERR_IO` | 回调 `MQ_ERR_IO`，无泄漏 | 异步 |
| 24 | 异步取消 | `read_async` 后取消 | `task_state==CANCELED`，回调不重复 | 异步 |
| 25 | EOF 语义 | 读到文件尾 | `read_sync` 返回 0 且 `MQ_ERR_EOF` | 边界 |
| 26 | 零长读 | `n=0` | `MQ_OK`，`out_n=0` | 边界 |
| 27 | 大读分块 | 读 > 单次 cap | 分块正确，字节连续 | 边界 |
| 28 | readdir 正常 | mem 挂载含 3 文件 | 回调收到 3 项且 name/stat 正确 | 正常 |
| 29 | readdir 异步 | `readdir_async`+脉冲 | 回调项集合一致（顺序不保证→集合比较） | 异步 |
| 30 | 空目录 | 空挂载根 readdir | `MQ_OK`，0 回调 | 边界 |
| 31 | 内容哈希校验 | 源哈希不匹配 | `MQ_ERR_HASH_MISMATCH`（ADR-0030） | 安全 |
| 32 | content_hash 缺失 | provider 未实现该函数 | 跳过校验，正常读 | 兼容 |
| 33 | dump_mounts | 两级挂载 | JSON 含全部挂载点/优先级/scheme | 自省 |
| 34 | resolve 输出 | 解析 `a/b/f` | scheme/relpath/mount_id 正确 | 自省 |
| 35 | 缓冲区不足 | `dump_mounts` cap 过小 | `MQ_ERR_INVALID_ARG` 且 `out_n` 报所需 | 边界 |
| 36 | 无全局状态/多实例 | 两 `mq_vfs` 同 scheme 不同数据 | 互不干扰，句柄不跨实例 | 多实例 |
| 37 | 内存所有权 | 所有路径走 `fake_allocator` | 退出时净分配 0，无二次释放 | 所有权 |
| 38 | 能力门控 | 沙箱 provider 请求越权 | `MQ_ERR_ACCESS`（§9 能力限制） | 安全 |

### 4.3 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S1 | **远程资产加载** | `fs.mount("assets/","https://cdn.example.com/assets/")`（fake http provider）；加载 `assets/tex/hero.png` | 异步取回、经内容哈希校验、二次读取命中缓存 |
| S2 | **zip 挂载** | `fs.mount("assets/","game.zip/assets")`（zip provider，仅读中央目录）；读随机文件 | 随机访问正确，内存不随归档大小线性增长 |
| S3 | **本地开发覆盖** | 先挂远程 `assets/`，再挂 `./assets/`（`SHADOW`） | 本地遮蔽远程（开发态热改立即可见） |
| S4 | **运行期热挂载** | 游戏运行时 `mount("mods/","mod.zip/mods")` | 新路径立即可用，不改已打开流 |
| S5 | **离线可玩** | 断网（http provider 返回 `MQ_ERR_IO`）后重复加载 | 命中内容寻址缓存，仍成功 |
| S6 | **坏包拒绝** | zip 中央目录损坏 / 哈希不符 | `MQ_ERR_HASH_MISMATCH`/`MQ_ERR_IO`，游戏优雅降级 |
| S7 | **资产管线对接** | asset 加载器 + persistence backend 均走 VFS 路径 | 一条路径空间处处可达，无重复 IO 实现 |
| S8 | **存档读写** | `mem://`/`file://` 挂载，persistence 经 VFS 写存档再读回 | 无损往返（配合 docs/08） |
| S9 | **AI 自省** | 控制平面调 `dump_mounts`/枚举任意路径 | 可序列化输出，AI 能读懂挂载树（A1） |

### 4.4 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| D1 | 帧内不触发异步 | 在模拟帧内调用受限 IO 被断言/拒绝；帧循环 checksum 与无 IO 版本一致 |
| D2 | 完成顺序不改变模拟 | 用 `fake_executor` 以不同顺序脉冲完成，模拟结果 checksum 一致（加载时序不进模拟状态） |
| D3 | 回放一致 | 记录加载完成顺序后重放，结果 checksum 一致 |
| D4 | 挂载树确定性 | 相同 `mount` 序列 → 相同 `dump_mounts` 输出（稳定排序） |

### 4.5 独立单测保障

- 单测仅链接 `vfs` 静态/动态库 + `mq_vfs.h`，**不链接** persistence/asset/scene。
- 所有协议交互经 `mem_provider`/`fake_provider`，**零**真实磁盘/网络访问（CI 可在无网环境跑）。
- executor 用 `fake_executor` 手动脉冲，避免线程与真实时序依赖。
- 用 `fake_allocator` 断言所有权与无泄漏。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：定稿 `mq_vfs.h`（`mq_vfs_v1` + `mq_fs_provider_v1`）、结果码、版本化、所有权约定；写 `static_assert(sizeof)`/布局检查。产出：头 + ABI 校验测试。**冻结后签名只增不改。**
2. **阶段 1 · 测试骨架（红）**：实现 `mem_provider`/`fake_provider`/`fake_executor`/`fake_allocator`；写 4.2 全表用例（先失败）；实现 `resolve` 算法与挂载树（用例 5–19 转绿）。
3. **阶段 2 · 最小实现（绿）**：`create/destroy/register/mount/unmount/stat/open/close/seek` + `mem://` 读写；打通 `read_async`/`readdir` 与 executor 回调；用例 20–30、36–37 转绿。
4. **阶段 3 · 边界/安全/确定性**：路径穿越（含编码变体）、优先级/遮蔽冲突、版本兼容、哈希校验、`dump_mounts/resolve`；落地 D1–D4；用例 31–35、38 转绿。
5. **阶段 4 · 对接与预算**：实现/接入 `file://`、`res://`、`zip://` provider（zip 只读中央目录 + 按需解压）、`http(s)://`（Range + 内容寻址缓存）；接 asset/persistence fake 集成；性能/内存达标。
6. **阶段 5 · 重构与观测**：抽公共路径规范化与错误映射；接入结构化 IO 事件（对接 observability，A2）；整理文档与遗留项。

## 6. 验收标准（DoD）

- `mq_vfs.h` 接口**冻结**并被上游 fake（asset/persistence/script）依赖编译通过；
- 4.2 全部用例 + 4.3 场景（S1–S9，其中远程/zip 用 fake 或本地样本）+ 4.4 确定性用例通过；
- 单测覆盖率 ≥ 80%（核心 `resolve`/mount/async 路径 ≥ 90%）；
- 无全局可变状态；两实例隔离用例（#36）通过；
- 路径穿越拒绝（#17/#18）与哈希校验（#31）通过；
- 性能预算：`resolve` 为纯内存 O(挂载数)，命中 < 1µs；帧内无阻塞 IO、无异步触发（D1）；
- 内存：`fake_allocator` 净分配为 0，无泄漏/二次释放；
- 不依赖任何上层模块实现。

## 7. 风险

| 风险 | 处理 |
|------|------|
| 远程加载延迟影响帧时间 | 一律异步 + executor；帧内禁 IO（D1）；远程预取 + 内容寻址缓存（S5）；模拟帧绝不阻塞 |
| 挂载遮蔽语义易错 | 严格 `(前缀分量长度, priority, seq)` 排序；同前缀同优先级无显式 `SHADOW` 即报错；用例 7–14 覆盖 |
| 字节前缀误匹配 | 按 `/` 分量边界匹配（用例 #8） |
| 归档随机访问内存开销 | 只读中央目录，按需解压单条目，不整包驻留（S2） |
| 多协议语义漂移 | 统一 `stat/open/read/readdir` 语义与 `mq_result` 错误码；provider 能力用 `MQ_ERR_UNSUPPORTED` 而非崩溃 |
| 远程内容不可信 | 内容哈希校验（ADR-0030）+ 能力沙箱 + 路径穿越拦截 + 脚本侧无原生 exec（§9） |
| 跨 WASM/JS 边界（OD-8） | 接口只含 POD + 函数指针（docs/02），天然符合；provider 可由 JS 实现 |
| provider vtable 生命周期 | 约定 vtable 与 provider 实例由注册/挂载方管理，VFS 不缓存跨重载函数指针（docs/02 §6） |

## 8. Owner agent

- **角色标识**：`agent-vfs`
- **职责边界**：仅负责 VFS 模块的接口（`mq_vfs.h`）、测试（mem/fake provider 单测、场景与确定性用例）与实现（挂载树、解析、异步桥接）；实现/接入各 protocol provider 但不修改其它模块。任何跨模块接口变更需先修订本计划与 `docs/04`。
