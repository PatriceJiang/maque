/* mq_asset.h —— maque 资产管线（docs/08、plans/08）：纯 C ABI（C11 干净）。
 *
 * 导入(import) → 依赖图(deps) → 烘焙(cook) → 打包(pack) → dump(JSON)。
 *
 * 边界约定（docs/02 R1–R8）：只暴露 extern "C" + POD + 固定宽度整数；
 * 无异常/RTTI/STL 跨边界；不透明句柄 mq_asset_db；全部状态在实例内（无全局可变状态）。
 *
 * 确定性契约：同输入 → 逐字节相同的 cook/pack/dump 产物；遍历一律按 source 路径字典序；
 * 不使用时间戳、指针地址、locale 或随机数。
 *
 * 所有权：db 由 mq_asset_create 建立、mq_asset_destroy 释放；
 * mq_asset_dump_json 返回的缓冲归 db（下次变更或 destroy 前有效），调用方不得释放。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQ_ASSET_ABI_VERSION 1u
#define MQ_ASSET_IFACE_NAME  "maque.asset.v1"

/* mq_vfs 的完整定义在 core/vfs/mq_vfs.h；此处重复 typedef 为同一 struct 类型
 * （C11 6.7/3、C++ 均允许），使本头可独立用于只读依赖图场景。 */
typedef struct mq_vfs mq_vfs;

typedef struct mq_asset_db mq_asset_db;   /* 不透明，模块分配，逐实例 */

/* 错误码（0 = OK）。 */
enum {
    MQ_ASSET_OK            = 0,
    MQ_ASSET_ERR_ARG       = 1,
    MQ_ASSET_ERR_NOT_FOUND = 2,
    MQ_ASSET_ERR_IO        = 3,
    MQ_ASSET_ERR_FORMAT    = 4,
    MQ_ASSET_ERR_NO_MEM    = 5,
    MQ_ASSET_ERR_DEPENDENCY= 6,
    MQ_ASSET_ERR_DUPLICATE = 7
};

/* 资源种类（与 schema 的 asset 命名空间对应；未知类型用 Other）。 */
typedef enum mq_asset_kind {
    MQ_ASSET_KIND_OTHER = 0,
    MQ_ASSET_KIND_TEXTURE,
    MQ_ASSET_KIND_MATERIAL,
    MQ_ASSET_KIND_MESH,
    MQ_ASSET_KIND_FONT,
    MQ_ASSET_KIND_ANIMATION,
    MQ_ASSET_KIND_SCENE,
    MQ_ASSET_KIND_SCRIPT
} mq_asset_kind;

/* mq_asset_info.flags 位定义。 */
#define MQ_ASSET_FLAG_COOKED      1u   /* bit0：已有烘焙产物 */
#define MQ_ASSET_FLAG_MISSING_DEP 2u   /* bit1：存在缺失依赖（查询时实时计算） */

/* 资源记录（稳定 ID / 源路径 / 烘焙产物 / 内容哈希）。 */
typedef struct mq_asset_info {
    uint64_t      id;             /* 稳定 ID（内容哈希派生；同内容同 ID） */
    mq_asset_kind kind;
    char          source[256];    /* 源路径（相对 VFS 挂载根） */
    char          cooked[256];    /* 烘焙产物路径（未烘焙为空） */
    uint64_t      source_hash;    /* 源内容哈希（FNV-1a 64） */
    uint64_t      cooked_hash;    /* 产物哈希（未烘焙为 0） */
    uint32_t      dep_count;      /* 依赖数 */
    uint32_t      flags;          /* bit0 已烘焙, bit1 有缺失依赖 */
} mq_asset_info;

/* 生命周期：vfs 由调用方持有（本模块只借用，不 destroy）。
 * vfs == NULL = 纯内存模式：无 IO 后端，源内容视为空，import/cook/pack 仍可运行
 * （体积为零的清单/依赖图工具），source_hash = FNV-1a64("")。 */
mq_asset_db* mq_asset_create(mq_vfs* vfs);
void         mq_asset_destroy(mq_asset_db* db);   /* 对 NULL 安全幂等 */

/* 导入：把 VFS 路径登记为资源（读内容 → 哈希 → 依扩展名判定 kind）。
 * 重复路径返回 MQ_ASSET_ERR_DUPLICATE。out 必须非空。 */
int mq_asset_import(mq_asset_db* db, const char* path, mq_asset_info* out);
/* 按源路径/稳定 ID 查询。 */
int mq_asset_find(mq_asset_db* db, const char* path, mq_asset_info* out);
int mq_asset_find_by_id(mq_asset_db* db, uint64_t id, mq_asset_info* out);
uint32_t mq_asset_count(mq_asset_db* db);
int mq_asset_info_at(mq_asset_db* db, uint32_t index, mq_asset_info* out);

/* 依赖：声明 path 依赖 dep_path（导入期可多次调用；重复声明幂等）。
 * dep_path 尚未导入也允许登记（是否缺失由 mq_asset_validate 判定）。 */
int      mq_asset_add_dependency(mq_asset_db* db, const char* path, const char* dep_path);
uint32_t mq_asset_dependency_count(mq_asset_db* db, const char* path);
/* 写入第 index 条依赖（NUL 结尾，超长截断）；out 必须非空且 out_cap > 0。 */
int      mq_asset_dependency_at(mq_asset_db* db, const char* path, uint32_t index,
                                char* out, size_t out_cap);

/* 依赖图：拓扑序（被依赖者在前；确定序：同层按路径字典序）。返回写入 out 的路径数。
 * out/out_cap 不足时只写入前 out_cap 项；处于环上的节点不输出（数量固定）。 */
uint32_t mq_asset_toposort(mq_asset_db* db, char (*out)[256], uint32_t out_cap);
/* 依赖闭包检查：任何依赖缺失 → MQ_ASSET_ERR_DEPENDENCY，并把缺失者写进 out（首条，
 * 截断到 missing_cap）。missing_out 可为 NULL（只取返回码）。 */
int mq_asset_validate(mq_asset_db* db, char* missing_out, size_t missing_cap);

/* 烘焙：源 → 产物（本切片：二进制打包，确定性且可读回）。
 * 格式（小端）：magic "MQAK" + u32 version + u64 source_hash + u32 kind +
 *               u32 payload_len + payload 字节。
 * 产物经本地文件系统写入 out_path（mq_vfs 只提供读，无 write 接口；docs/08 §2 backend file:// 形态）。
 * 成功后登记 cooked 路径与 cooked_hash。 */
int mq_asset_cook(mq_asset_db* db, const char* path, const char* out_path);
/* 打包：把若干资源（含依赖闭包）打成归档文件（自制格式，含清单与内容哈希）。
 * 格式（小端）：magic "MQAP" + u32 version + u32 entry_count +
 *               每项 { u64 id, u64 hash, u32 kind, u32 path_len, path,
 *                      u32 data_len, data }（按 path 字典序）+
 *               u64 manifest_hash（覆盖其前全部字节）。
 * 条目 data = 该资源的源内容。同样写本地文件系统。 */
int mq_asset_pack(mq_asset_db* db, const char* const* paths, uint32_t count, const char* out_path);

/* dump：整库 JSON（清单/哈希/依赖/烘焙状态；供 AI/CI 读取，docs/11）。
 * 返回的缓冲归 db 所有（下次变更或 destroy 前有效）；失败返回 NULL。 */
const char* mq_asset_dump_json(mq_asset_db* db);
/* 自省：按 kind 计数（dump/统计用）。 */
uint32_t mq_asset_count_kind(mq_asset_db* db, mq_asset_kind kind);

#ifdef __cplusplus
}
#endif
