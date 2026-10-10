# 08 · asset pipeline（ai：导入 / 依赖 / 烘焙 / 打包 / 压缩 / dump）开发计划

> 复制自 [`_template.md`](_template.md)。接口与测试章节**先于**实现章节完成。
> 主参考：[`docs/08-资源持久化与资产管线.md`](../docs/08-资源持久化与资产管线.md)；上游 [`docs/04`](../docs/04-虚拟文件系统与多协议.md)、[`docs/06`](../docs/06-反射与代码生成.md)、[`docs/02`](../docs/02-模块系统-ABI与多实例.md)。

## 1. 模块概述

- **职责**：把**开发期文本资源**（`.scn` / 纹理 / 网格 / 材质 / 脚本 …）经 **导入 → 依赖图 → 烘焙（文本→二进制）→ 打包 → 压缩 → dump** 转成**确定、可复现**的发布产物，并产出**机器可读 dump** 供 AI/CI 检查与比对。可**脱离完整引擎运行**（ADR-0009）。
- **所在层**：数据层（VFS 之上，persistence 相邻；见 README §4 阶段 B）。
- **上游文档**：`docs/08`（管线定义）、`docs/04`（VFS 为 IO 底座、`archive://` 挂载）、`docs/06`（反射 = canonical IR、schema codegen）、`docs/07`（allocator / 句柄 / 稳定 ID）、`docs/02`（C ABI vtable + struct-size 版本化）、`docs/11`（控制平面 `Asset` domain）。
- **一句话**：**一个可 headless、可被 AI/CI 驱动的确定性资产烘焙工具链，输入文本真相、输出内容寻址的二进制包与 JSON dump。**

### 1.1 术语与不变式

| 术语 | 含义 |
|------|------|
| **source** | 开发期文本资源（唯一真相，入 VCS；docs/08 §3）。 |
| **import** | 读取 source → 建立内存 IR（反射模型）+ 记录其静态/动态依赖边。 |
| **deps** | 资产依赖图（有向图）：边 `A → B` 表示 A 引用了 B（`asset_ref` / 场景节点引用 / 脚本 require）。 |
| **cook** | 把 import 后的 IR 烘焙为**二进制产物**（`text→binary`，docs/08 §2 的 codec 切换）。 |
| **pack** | 把一组 cook 产物 + 目录清单压成**归档**（`archive://` 可直接挂载；docs/04 §8）。 |
| **dump** | 输出**机器可读 JSON**（依赖、类型、内容哈希），供 AI diff / 校验（docs/08 §6）。 |

**硬不变式（贯穿全部实现与测试）：**

1. **内容哈希判定失效**：任何产物以输入**内容哈希**（非 mtime / 非大小）判定是否需重烘（docs/08 §6）。
2. **确定性产物**：同一输入 → **逐字节相同**的产物（排序输出、无时间戳、无绝对路径、无环境/locale 依赖；docs/08 §6、§9）。
3. **文本是唯一真相**：二进制是**可再生派生**，删除后可从 source 重建（docs/08 §3）。
4. **零全局可变状态**：全部状态在 `mq_asset_db` 实例内（ADR-0014）。
5. **边界仅 C ABI**：`extern "C"`、无异常逃逸、无 STL 跨边界、分配者释放（ADR-0013、docs/02 R1–R8）。

## 2. 接口层设计（完备，先冻结）

> 公共头 `modules/data/asset/include/mq_asset.h`（纯 C）。接口名 `"maque.asset.v1"`。
> 跨边界仅 POD + 固定宽度整数 + UTF-8 `const char*`（docs/02 R6）。

### 2.1 公共头（示意，冻结）

```c
/* mq_asset.h —— 纯 C，跨 .so ABI */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define MQ_ASSET_ABI_VERSION 1u
#define MQ_ASSET_IFACE_NAME  "maque.asset.v1"

/* 与 docs/02 一致的 allocator；本模块只经它分配。 */
typedef struct mq_allocator {
    uint32_t struct_size;
    void* (*alloc)(void* self, size_t size, size_t align);
    void  (*free)(void* self, void* ptr);
} mq_allocator;

/* 内容哈希：固定 128-bit，算法 pin 为 BLAKE3-128（可复现、跨平台、无环境依赖）。 */
typedef struct mq_asset_hash { uint64_t lo; uint64_t hi; } mq_asset_hash;

/* 错误码：0 = 成功；负 = 错误（无异常，docs/02 R2）。 */
typedef enum mq_asset_status {
    MQ_ASSET_OK               =  0,
    MQ_ASSET_E_INVALID_ARG    = -1,
    MQ_ASSET_E_NOT_FOUND      = -2,   /* 资产 id / 路径不存在 */
    MQ_ASSET_E_IO             = -3,   /* VFS 读/写失败 */
    MQ_ASSET_E_PARSE          = -4,   /* source 文本解析失败 */
    MQ_ASSET_E_CYCLE          = -5,   /* 依赖图存在环 */
    MQ_ASSET_E_MISSING_DEP    = -6,   /* 引用的依赖不存在 */
    MQ_ASSET_E_ABI            = -7,   /* abi_version 不匹配 */
    MQ_ASSET_E_CORRUPT        = -8,   /* 归档/产物损坏或哈希不匹配 */
    MQ_ASSET_E_ABI_VERSION    = -9,   /* struct_size 过小无法安全使用 */
    MQ_ASSET_E_OOM            = -10,
    MQ_ASSET_E_UNSUPPORTED    = -11   /* 未知格式 / 未编译的 feature */
} mq_asset_status;

typedef enum mq_asset_kind {
    MQ_ASSET_KIND_UNKNOWN = 0, MQ_ASSET_KIND_SCN = 1, MQ_ASSET_KIND_TEXTURE = 2,
    MQ_ASSET_KIND_MESH = 3, MQ_ASSET_KIND_MATERIAL = 4, MQ_ASSET_KIND_SCRIPT = 5,
    MQ_ASSET_KIND_AUDIO = 6, MQ_ASSET_KIND_RAW = 7
} mq_asset_kind;

typedef enum mq_asset_dump_format {
    MQ_ASSET_DUMP_JSON = 0,     /* 主推：AI 最易解析（docs/08 §4）*/
    MQ_ASSET_DUMP_JSONL = 1,    /* 大图流式：一行一边/一节点 */
    MQ_ASSET_DUMP_DOT   = 2     /* 依赖图可视化，可选 */
} mq_asset_dump_format;

/* 只读视图，内存由 db 的 intern 表持有，destroy(db) 前有效。 */
typedef struct mq_asset_ref_view {
    const char*   id;       /* 稳定 ID（UTF-8；规范化 VFS 路径或命名 ID）*/
    mq_asset_kind kind;
    mq_asset_hash content;  /* source 内容哈希 */
} mq_asset_ref_view;

typedef struct mq_dep_edge {
    const char* from;       /* 稳定 ID */
    const char* to;         /* 稳定 ID */
    uint32_t    ref_kind;   /* 0=asset_ref, 1=scn-node, 2=script-require */
} mq_dep_edge;

/* 调用方经 allocator 分配 edges 数组；用 buffer_free 释放（见 §2.5）。 */
typedef struct mq_deps_view {
    uint32_t          struct_size;
    uint32_t          count;
    const mq_dep_edge* edges;   /* 按 (from,to) 字典序排序 —— 确定性 */
} mq_deps_view;

/* 一次烘焙/打包/导入的产物：内容寻址、可 dump、可写入 VFS。 */
typedef struct mq_asset_artifact {
    uint32_t      struct_size;
    const char*   id;          /* 产物对应的资产稳定 ID */
    mq_asset_kind kind;
    mq_asset_hash content;     /* 产物内容哈希（确定性身份）*/
    const void*   bytes;       /* 只读；归 db 所有，buffer_free 前有效 */
    size_t        size;
} mq_asset_artifact;

/* 增量烘焙报告：哪些被重烘、哪些命中缓存。 */
typedef struct mq_cook_report {
    uint32_t struct_size;
    uint32_t cooked;    /* 本次实际重烘数量 */
    uint32_t reused;    /* 命中内容哈希、未重烘数量 */
    uint32_t failed;
} mq_cook_report;

typedef struct mq_buffer { void* data; size_t size; } mq_buffer;

typedef struct mq_asset_opts {
    uint32_t    struct_size;
    const char* root;          /* 根 VFS 路径（挂载点前缀），如 "assets/" */
    uint32_t    flags;         /* MQ_ASSET_F_STRICT=1（缺依赖即失败）、MQ_ASSET_F_NO_MMAP=2 */
    const char* manifest_path; /* 可选：预载清单 */
} mq_asset_opts;

/* ==== vtable：完整操作面 ==== */
typedef struct mq_asset_v1 {
    uint32_t struct_size;      /* = sizeof(mq_asset_v1) */
    uint32_t abi_version;      /* 主版本不匹配 → E_ABI */

    /* 生命周期 */
    int  (*create)(void* host, const mq_allocator* alloc,
                   const mq_asset_opts* opts, void** out_db);
    void (*destroy)(void* db);

    /* import：读取 source → IR，登记到 db 并记录依赖边 */
    int  (*import)(void* db, const char* src_path, mq_asset_artifact* out);
    /* 一键 import 整个根目录（确定性遍历顺序：按 id 字典序）*/
    int  (*import_all)(void* db, mq_asset_artifact* out);

    /* 依赖图查询 */
    int  (*deps_of)(void* db, const char* id, mq_deps_view* out);       /* 直接后继 */
    int  (*deps_reverse)(void* db, const char* id, mq_deps_view* out);  /* 反向（谁引用我）*/
    int  (*deps_closure)(void* db, const char* id, mq_deps_view* out);  /* 传递闭包 */
    int  (*topo_order)(void* db, mq_deps_view* out);                    /* 拓扑序，成环 → E_CYCLE */

    /* cook：文本 → 二进制；opts 决定 codec/压缩 */
    int  (*cook)(void* db, const char* id, const mq_cook_opts* o, mq_asset_artifact* out);
    int  (*cook_dirty)(void* db, const mq_cook_opts* o, mq_cook_report* out); /* 增量：只烘脏项 */

    /* pack：manifest 引用的子图 → 归档（archive:// 可挂载）*/
    int  (*pack)(void* db, const mq_pack_opts* o, mq_asset_artifact* out);

    /* dump：机器可读文本 */
    int  (*dump)(void* db, const char* id, mq_asset_dump_format fmt, mq_buffer* out);
    int  (*dump_graph)(void* db, mq_asset_dump_format fmt, mq_buffer* out);

    /* 独立加载 .scn（不启动运行时）*/
    int  (*scn_open)(void* db, const char* path, void** out_doc);
    int  (*scn_deps)(void* db, void* doc, mq_deps_view* out);
    void (*scn_close)(void* db, void* doc);

    /* 内存释放（分配者释放；见 §2.5）*/
    void (*buffer_free)(void* db, mq_buffer* buf);   /* 释放 deps_view.edges 或 dump 输出 */
} mq_asset_v1;
```

`mq_cook_opts` / `mq_pack_opts`（同头，POD）：

```c
typedef struct mq_cook_opts {
    uint32_t    struct_size;
    uint8_t     codec;      /* 0=binary, 1=binary+compress(zstd), 2=json（dev）*/
    uint8_t     compress_level; /* zstd level，默认 3，固定即确定 */
    uint16_t    _pad;
    mq_asset_hash* prev;    /* 可选：上次产物哈希，用于跳过 */
} mq_cook_opts;

typedef struct mq_pack_opts {
    uint32_t    struct_size;
    const char* manifest_id; /* 打包入口（其依赖闭包即内容集）*/
    const char* out_path;    /* 归档写入的 VFS 路径，如 "out/game.pak" */
    uint8_t     codec;       /* 归档内条目 codec；0=store,1=zstd */
    uint8_t     compress_level;
    uint16_t    _pad;
} mq_pack_opts;
```

### 2.2 类型清单

| 类型 | 类别 | 说明 |
|------|------|------|
| `mq_allocator` | POD vtable | 跨 `.so` 分配契约（docs/02 R5）。 |
| `mq_asset_hash` | POD | 128-bit 内容哈希（pin BLAKE3-128）。 |
| `mq_asset_status` | enum | 错误码（§2.4）。 |
| `mq_asset_kind` | enum | 资产种类。 |
| `mq_asset_ref_view` | POD 只读视图 | id/kind/content；字符串归 db。 |
| `mq_dep_edge` / `mq_deps_view` | POD + 视图 | 依赖边数组，已排序。 |
| `mq_asset_artifact` | POD 只读视图 | 产物字节；归 db，`buffer_free` 前有效。 |
| `mq_cook_report` | POD | 增量报告。 |
| `mq_buffer` | POD | 输出缓冲，调用方持有，须 `buffer_free`。 |
| `mq_asset_opts` / `mq_cook_opts` / `mq_pack_opts` | POD | 非透明配置。 |
| `void* db` / `void* doc` | 不透明句柄 | 实例上下文与场景文档；**禁裸指针泄漏**。 |

### 2.3 操作面语义（逐函数）

| 函数 | 语义 / 参数 / 返回 | 错误 |
|------|-------------------|------|
| `create` | 用 `host` + `alloc` 建 `db`，绑定 `opts.root`。返回 `E_ABI` 若 `abi_version` 不符。 | `E_INVALID_ARG`, `E_ABI`, `E_OOM` |
| `destroy` | 释放 db 及全部 intern/缓存/产物；对 `NULL` 安全幂等。 | — |
| `import` | 经 VFS 读 `src_path`，按 kind 解析文本 → IR，登记 id、`content`，抽出引用边。**不写盘**。 | `E_NOT_FOUND`, `E_IO`, `E_PARSE`, `E_MISSING_DEP`(strict) |
| `import_all` | 确定性（字典序）遍历根，逐个 import；单文件错误累计，整体返回首个错误码。 | 同上 |
| `deps_of` | 写 `out`：直接后继，`edges` 按 `(from,to)` 排序。 | `E_NOT_FOUND`, `E_OOM` |
| `deps_reverse` | 反向边：回答"谁依赖我"（热重载影响面；docs/08 §7）。 | 同上 |
| `deps_closure` | 传递闭包（去重、排序）。 | 同上 |
| `topo_order` | 全图拓扑序；成环返回 `E_CYCLE`（含环上 id 上下文，见 §2.4）。 | `E_CYCLE`, `E_OOM` |
| `cook` | 单资产 IR → 二进制（`codec`）。同输入 → 同 `content`。 | `E_NOT_FOUND`, `E_UNSUPPORTED`, `E_OOM` |
| `cook_dirty` | 增量：对每个资产比对源内容哈希与缓存产物哈希，仅重烘变更者 + **反向依赖闭包**；填 `mq_cook_report`。 | `E_IO`, `E_OOM` |
| `pack` | 取 `manifest_id` 的依赖闭包，按 id 字典序写归档条目；条目含内容哈希；输出可被 `archive://` 挂载（docs/04 §8）。 | `E_NOT_FOUND`, `E_CYCLE`, `E_IO`, `E_CORRUPT` |
| `dump` | 单资产 → JSON/JSONL/DOT，字段按 schema 声明序（docs/08 §4）；无时间戳/绝对路径。 | `E_NOT_FOUND`, `E_UNSUPPORTED` |
| `dump_graph` | 整图 dump（节点 + 边），排序稳定。 | `E_CYCLE`(可由 DOT 表达环), `E_OOM` |
| `scn_open` | **不启动引擎**解析 `.scn`（docs/08 §6）；建立 `doc`。 | `E_PARSE`, `E_IO`, `E_NOT_FOUND` |
| `scn_deps` | 从 `doc` 抽出该场景引用的资产 id 集合。 | `E_INVALID_ARG` |
| `scn_close` | 释放 `doc`。 | — |
| `buffer_free` | 释放由本模块经 `alloc` 分配的 `edges`/`dump` 缓冲；对空缓冲幂等。 | — |

### 2.4 错误模型

- **无异常、无越界**：全部入口 `noexcept` 语义，错误以负 `mq_asset_status` 返回（docs/02 R2）。
- **结构化上下文**：返回首个错误码；细节（出错的 id / 路径 / 行号 / 环成员）经 `dump` 或 host 日志域输出，**不跨边界传 STL**。环成员以稳定 id 列表写入一次 `dump_graph` 的 `errors[]`。
- **不变量**：成功（`0`）时所有 `out` 参数被完整填充；失败时 `out` 内容未定义但**不泄漏**（内部回滚）。
- **首错即停 vs 累计**：`import_all` / `cook_dirty` 累计并在 `report.failed` 计数，返回**首个**错误码；`--strict`（`MQ_ASSET_F_STRICT`）下 `cook_dirty` 一处失败即中止。

### 2.5 内存所有权

| 所有权 | 规则 |
|--------|------|
| **intern 字符串**（id / kind 名） | 归 db，`destroy` 前有效；不单独释放。 |
| **`artifact.bytes`** | 归 db 内部缓存，`buffer_free`/`destroy` 前有效；只读。 |
| **`deps_view.edges` / `dump` 输出 `mq_buffer`** | 由本模块经传入 `alloc` 分配 → 调用方用 `buffer_free` 释放。 |
| **`host`** | 由调用方持有，本模块只借用其 VFS/日志接口。 |
| **`doc`** | 归 db，`scn_close` 释放。 |
| **禁** | 跨 `.so` 传 `std::string/vector/shared_ptr`；A 分配 B 释放（docs/02 §9）。 |

### 2.6 版本化与确定性

- **`struct_size` 只增不改**：新版本在 `mq_asset_v1` 表尾追加函数；旧调用方按自身 `struct_size` 截断使用（docs/02 §3）。
- **`abi_version` 主版本不匹配即拒绝**，返回 `E_ABI`（可读信息进日志，服务 A2）。
- **接口名**为稳定字符串 `"maque.asset.v1"`，经 `mq_module_query` 获取（docs/02 §3）。
- **确定性契约（写死并测试）**：
  1. 哈希算法 pin 为 **BLAKE3-128**，禁运行时切换；
  2. 所有遍历/输出**按稳定 id 字典序**（禁指针、禁哈希序、禁文件系统顺序）；
  3. **禁时间戳、禁绝对路径、禁主机名、禁 locale**（浮点用最短往返表示，docs/08 §4）；
  4. 压缩参数（level/窗口）默认固定；同一输入 → **同一 `content` 哈希**。
- **线程**：`db` 非线程安全；跨线程须外部串行化（与 ADR-0018 默认单线程一致）。`cook_dirty` 内部**可**用 worker 并行，但**输出必须与串行逐字节一致**（按 id 归并）。

### 2.7 CLI 契约（headless，AI/CI 驱动）

工具名 `mq-asset`（薄封装于同一 ABI，供无宿主场景与流水线调用）。退出码：`0` 成功、`1` 一般错误、`2` 用法错误、`3` 存在环、`4` 哈希/校验失败（与状态码映射固定）。

```
mq-asset import   --root <vfs-root> --src <path>            [--json]
mq-asset deps     --root <vfs-root> --id <id> [--reverse|--closure] [--format json]
mq-asset cook     --root <vfs-root> (--id <id> | --dirty)   [--codec binary|zstd|json]
mq-asset pack     --root <vfs-root> --manifest <id> --out <vfs-path> [--codec store|zstd]
mq-asset dump     --root <vfs-root> (--id <id> | --graph)   [--format json|jsonl|dot]
mq-asset scn      --root <vfs-root> load <path> --dump-deps  [--format json]
```

约定：
- 所有输出**默认机器可读 JSON**（AI 优先，docs/00 A1/A4）；`--json` 关闭人类摘要。
- **dry-run**：`cook` / `pack` 支持 `--dry-run`，只报告影响面不落盘（对齐 docs/11 §4）。
- 通过环境变量 `MQ_VFS_MOUNT` 或 `--root` 指定挂载；CLI 内部走与库**同一** `mq_asset_v1` 操作面，保证二者不漂移。
- CLI 与 docs/11 的 `Asset` domain（`Asset.import/deps/cook/pack/dump`）字段一一对应；JSON-RPC 结果即 `dump` 输出。

## 3. 依赖与边界

- **可依赖（仅经 C ABI / 稳定接口）**：
  - **VFS**（docs/04）：`stat/open/read/write/readdir` 与挂载树；`file://`/`mem://` 读写，`archive://` 挂载本模块产物。
  - **反射/schema codegen**（docs/06）：canonical IR 与描述符（type_id、字段偏移、`asset_ref<T>` 解析规则）。**只消费描述符数据**，不依赖代码生成器实现。
  - **allocator / 日志 host 接口**（docs/02、docs/07、docs/09）。
  - **持久化 codec**（docs/07 模块 `persistence`）：复用其 codec visitor 做文本⇄二进制；缺省时本模块内置 JSON/binary visitor 作回退。
- **禁止**：
  - 依赖**运行时世界/渲染/脚本/场景**模块实现（须能脱离完整引擎运行，ADR-0009）。
  - **持有全局可变状态**（ADR-0014）；**跨边界异常/RTTI/STL**（docs/02 §9）；**裸指针泄漏 ABI**。
  - 依赖**文件系统顺序 / mtime / 绝对路径 / locale / 指针地址**做任何判定或排序（确定性）。
  - 把二进制产物当**唯一真相**（须能从 source 重建）。
- **与其它模块的接缝**：

| 模块 | 接缝 |
|------|------|
| VFS(06) | 本模块**只经 VFS 路径**读写，不直接碰磁盘；产物包被 `archive://` Provider 挂载。 |
| persistence(07) | 共享 codec visitor 与反射 IR；`cook` 即"换 codec 重编码"。 |
| scene(11) | `scn_open/scn_deps` 独立解析 `.scn`；场景模块运行时复用同一解析器（同一 schema）。 |
| renderer(15) | 纹理/网格 cook 产物被运行时按 `content` 哈希加载。 |
| control plane(10) | `Asset` domain 直接映射 §2.7；事件（`Asset.cooked`）推送。 |

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**仅依赖本模块 + `mq_asset.h`**；
> VFS/反射/日志一律用 **fake**（`mem://` / temp VFS），**不得**依赖其它模块完整实现。

### 4.1 测试替身

| 替身 | 用途 |
|------|------|
| `fake_vfs` | 内存 VFS：预置文本资源、捕获写入；实现 `stat/open/read/write/readdir` 语义。 |
| `fake_alloc` | 记账 allocator：统计 alloc/free 次数与字节，注入 OOM，最终断言**零泄漏**。 |
| `fake_reflect` | 固定 schema 描述符（含 `asset_ref<T>`、`transient`、默认值），供 import/cook 解析。 |
| `fake_clock` / `fake_env` | 断言产物**不含时间戳 / 不受 TZ/locale 影响**（改动 env 后哈希不变）。 |
| 语料 | `fixtures/`：`player.scn`、`hero.png`、`wall.mat`、`main.js`、`cycle_a.scn/cycle_b.scn`（造环）、含 `../` 的恶意归档。 |

### 4.2 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| U01 | create/destroy 生命周期 | 合法 opts | 返回 `0`；销毁后 `fake_alloc` 零泄漏 | 正常 |
| U02 | import 文本→IR | `player.scn` | `artifact.kind=SCN`，登记 id，`content` 稳定 | 正常 |
| U03 | import 缺文件 | `nope.scn` | `E_NOT_FOUND`，无副作用 | 错误 |
| U04 | import 解析失败 | 非法 JSON | `E_PARSE`，含行号上下文 | 错误 |
| U05 | deps_of 直接后继 | `player.scn`→`hero.png`,`wall.mat` | 2 条边，`(from,to)` 有序 | 正常 |
| U06 | deps_reverse | `hero.png` | 反向列出引用者 | 正常 |
| U07 | deps_closure 去重排序 | 多层引用 | 传递闭包无重复、字典序 | 正常 |
| U08 | topo_order 合法图 | 无环 | 拓扑序满足所有边方向 | 正常 |
| U09 | **循环依赖检测** | `cycle_a↔cycle_b` | `E_CYCLE`，环成员 id 写入 dump | 错误 |
| U10 | deps 空集 | 叶子资产 | `count=0`，`edges` 可空且 `buffer_free` 安全 | 边界 |
| U11 | missing_dep | `asset_ref` 指向不存在 id | 非 strict：登记并记 warning；strict：`E_MISSING_DEP` | 错误 |
| U12 | cook 文本→二进制 | `hero.png` | 产物 `content` 稳定、非空 | 正常 |
| U13 | **确定性：同输入同产物哈希** | 同一 source 连续 cook ×3 | 三次 `content` 逐字节相等 | 确定性 |
| U14 | 环境无关 | 变更 TZ/locale/工作目录后重 cook | 哈希不变 | 确定性 |
| U15 | 无时间戳 | dump 产物 JSON | 不含任何时间字段 | 确定性 |
| U16 | **增量：改一文件只重烘相关** | 改动 `wall.mat` | `cook_dirty` 仅重烘 `wall.mat` 及其反向依赖；`report.reused>0` | 增量 |
| U17 | 增量：未变则全复用 | 不修改任何源 | `report.cooked==0` | 增量 |
| U18 | 内容哈希 vs mtime | 只改 mtime 不改内容 | **不重烘**（`cooked==0`） | 增量/边界 |
| U19 | pack 单入口闭包 | `--manifest game` | 归档含闭包全集，按 id 排序，每条目带内容哈希 | 正常 |
| U20 | pack 产物可挂载 | 归档路径给 `archive://` fake | fake_vfs 能 `stat/open` 归档内条目 | 集成(fake) |
| U21 | pack 环路入口 | manifest 处在环上 | `E_CYCLE` | 错误 |
| U22 | 归档校验 | 篡改一个字节后读取 | `E_CORRUPT`（内容哈希不符） | 错误 |
| U23 | 路径穿越防护 | 归档含 `../evil` 条目 | 拒绝（`E_CORRUPT`/`E_INVALID_ARG`，docs/04 §9） | 安全 |
| U24 | dump 单资产 JSON | `player.scn` | 合法 JSON，字段按 schema 序，无默认值字段省略 | 正常 |
| U25 | dump 幂等 | 同图 dump ×2 | 字节相同 | 确定性 |
| U26 | 独立加载 .scn | `scn_open("player.scn")` | 成功；不触发任何运行时模块 | 正常 |
| U27 | scn_deps | 上述 doc | 返回引用资产 id 集合，与 U05 一致 | 正常 |
| U28 | scn_close 释放 | — | 零泄漏 | 内存所有权 |
| U29 | ABI 版本不匹配 | `abi_version=99` | `E_ABI`，拒绝使用 | 版本兼容 |
| U30 | struct_size 截断 | 传旧版较小 `struct_size` | 只调用已声明函数段，无越界 | 版本兼容 |
| U31 | OOM 注入 | alloc 在第 N 次失败 | 返回 `E_OOM`，已分配全回收 | 内存所有权 |
| U32 | 空/零长缓冲 | `buffer_free` 空缓冲、`deps` 空 | 幂等、无崩溃 | 边界 |
| U33 | 入口 NULL 参数 | `db=NULL` / `out=NULL` | `E_INVALID_ARG`，不崩溃 | 边界 |
| U34 | dump_graph DOT | 含环图 | 合法 DOT，环显式表达 | 正常 |

> **独立单测保障**：U01–U34 全部经 `fake_vfs`/`fake_reflect`/`fake_alloc` 运行，链接时**不链接** scene/renderer/persistence 实现；`scn_open` 走本模块内建解析器而非 scene 模块。

### 4.3 游戏场景用例（真实玩法驱动）

- **S1 · 打包一个完整游戏并 dump 依赖（端到端，fake VFS）**
  预置：`games/demo/` 下 `main.scn`（引用 `hero.png`、`wall.mat`、`player.js`）、`wall.mat` 引用 `brick.png`、`player.js` require `math.js`。
  步骤：`import_all` → `topo_order` → `cook_dirty`（全量）→ `pack --manifest main.scn --out out/demo.pak` → `dump_graph --format json`。
  断言：归档条目集 = 闭包 6 项；`dump` 中 `deps` 与预期邻接表一致；二次 `cook_dirty` 全 `reused`；将 `out/demo.pak` 交 `archive://` fake 挂载后能读 `main.scn`。
- **S2 · 改数值触发最小重烘（迭代，A5）**
  仅把 `wall.mat` 的 `roughness` 从 0.5 改 0.6 → `cook_dirty`：重烘 `wall.mat` + 其反向依赖（`main.scn` 打包层），`brick.png`/`hero.png`/`*.js` 全部 `reused`；重 `dump_graph`，仅相关 `content` 变化，其余哈希不变。
- **S3 · AI 修复验证回环（A3/A1）**
  用 dump 读出一个坏引用（`asset_ref` 指向缺失 id）→ AI 修正 source → 重 `import`/`dump` 验证 `E_MISSING_DEP` 消失；全程 headless、`mem://` 无副作用。

### 4.4 确定性用例

| # | 场景 | 期望 |
|---|------|------|
| D1 | 同语料 cook ×3 | 三次产物哈希相同 |
| D2 | 串行 vs worker 并行 `cook_dirty` | 产物**逐字节一致**（按 id 归并） |
| D3 | 乱序 import（打乱文件枚举顺序） | 最终 dump/归档一致（排序稳定） |
| D4 | 变更 TZ / locale / `$PWD` / 主机名 | 哈希不变 |
| D5 | 只改 mtime | 不重烘、哈希不变 |

### 4.5 覆盖率与性能目标

- 语句/分支覆盖率 **≥ 85%**（本模块工具链，非热路径）。
- 性能预算（参考机）：`cook_dirty` 对 1k 资产无变化时 **< 200 ms**（纯哈希比对，无重烘）；打包 64 MB 归档 **< 3 s**；单资产 dump **< 5 ms**。
- 内存：`db` 常驻 ≤ 语料大小的 2×；`cook` 单资产峰值 ≤ 源 + 产物 + 固定 arena。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：落地 `mq_asset.h`（§2）+ `MQ_ASSET_ABI_VERSION=1` + 接口名；评审通过后才实现。CLI 用法与 `Asset` domain 字段一并冻结。
2. **阶段 1 · 测试骨架（红）**：搭 `fake_vfs`/`fake_alloc`/`fake_reflect`，落 U01–U10、U24–U28、U33 骨架，全部失败。
3. **阶段 2 · 最小实现到绿**：`create/destroy` → `import`（解析 + intern + 边抽取）→ `deps_of/reverse/closure/topo_order` → `dump`（JSON）→ `scn_open/deps/close`。补齐 U01–U10、U24–U28。
4. **阶段 3 · cook / pack / 增量 / 确定性**：接入 codec（JSON⇄binary、zstd）；实现 `cook`/`cook_dirty`（内容哈希缓存 + 反向闭包）/`pack`（闭包 + 排序 + 归档 + 校验），落 U12–U23、U25、U34、D1–D5。
5. **阶段 4 · 版本/内存/安全 + 集成**：`struct_size` 截断、ABI 校验、OOM 注入、路径穿越、损坏归档（U29–U32）；CLI 与 `Asset` domain 对接；S1–S3 场景用例；性能/内存达标。
6. **阶段 5 · 加固**：环成员 dump 上下文、dry-run、`import_all` 累计错误、worker 并行一致性回归；接通 `archive://` 挂载联测（fake）。

## 6. 验收标准（DoD）

- 接口冻结且被上游 **fake**（VFS/反射/日志）依赖通过；CLI 与 `Asset` domain 字段一致。
- 单测覆盖率 **≥ 85%**；§4.2 全绿；`fake_alloc` 全程**零泄漏**。
- **确定性用例 D1–D5 通过**：同输入同产物哈希、串并行一致、环境无关。
- **增量用例通过**：改一文件只重烘相关；未变则全复用；只改 mtime 不重烘（U16–U18）。
- **循环依赖检测**通过（U09、U21）且环上下文可 dump。
- **独立加载 `.scn`**（U26–U28）在**不加载任何运行时模块**下通过。
- 打包产物可被 `archive://` 挂载（U20/S1）；篡改/穿越被拒（U22/U23）。
- 可脱离完整引擎编译运行（`mq-asset` 单二进制 + 本模块 `.so`）；无跨模块实现依赖；无全局可变状态。
- 性能/内存预算达标（§4.5）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **烘焙不确定性**（顺序/指针/时间/locale 渗入） | 全链按稳定 id 排序；哈希 pin BLAKE3-128；禁时间戳/绝对路径；D1–D5 + CI 门禁（产物哈希入库比对）。 |
| 文本/二进制漂移（docs/08 §9） | 单一反射 IR + codec visitor；加入往返一致性测试（text→binary→text 等值）。 |
| 内容哈希误判失效（漏烘） | 哈希覆盖**源内容 + 依赖内容 + cook 参数**（含 codec/level），任一变更即失效；U16–U18 覆盖。 |
| 大量资产 hash 慢 | 内容寻址缓存 + 只 stat/读取需要项；D 类用批量哈希；预算 §4.5。 |
| 归档随机访问内存开销（docs/04 §12） | 只读中央目录、按需解压；`pack` 条目表存偏移+哈希。 |
| 远端/恶意归档（OD-9） | 内容哈希校验 + 路径穿越防护 + capability 门控（U22/U23；docs/04 §9）。 |
| 与 persistence codec 重复实现 | 优先复用 persistence codec；仅保留内置 JSON/binary 回退并对其做**同等**确定性测试。 |
| worker 并行破坏确定性 | 输出按 id 归并；D2 回归；默认串行，仅 `db` 内部可控并行。 |
| `.scn` 解析与 scene 模块漂移 | 两者共用同一 schema 与解析器（docs/06）；发布前跑同一语料交叉比对。 |

## 8. Owner agent

- **角色标识**：`agent-asset`
- **职责边界**：仅负责本模块的接口（`mq_asset.h` + CLI 契约）、测试与实现；**不修改其它模块**。对 VFS/反射/日志仅经稳定接口消费，并以其 fake 完成独立单测。
- **交付**：`plans/08-asset-ai.md`（本文件）→ `modules/data/asset/`（`mq_asset.h`、`cli/`、测试）。
- **上游协作**：向 `agent-vfs` 约定 `archive://` 挂载与 `readdir` 语义；向 `agent-persistence` 约定 codec visitor 复用；向 `agent-control` 对齐 `Asset` domain 字段。
