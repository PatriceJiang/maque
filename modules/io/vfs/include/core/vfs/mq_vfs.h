/* maque 虚拟文件系统（docs/04）：挂载点 + 多协议后端。
 * 后端：file://（本地目录）/ mem://（内存挂载）/ zip://（归档，未压缩项）/
 *       builtin:（引擎内建，区分于 assets）/ 直接 URL（libcurl）。
 * 解析：最长前缀匹配（后挂载者优先）；内容哈希用于远程资产校验（OD-9/ADR-0030）。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_vfs mq_vfs;

/* 错误码（结构化；docs/11 §4）。 */
enum {
    MQ_VFS_OK          = 0,
    MQ_VFS_ERR_ARG     = 1,   /* 空指针/非法参数 */
    MQ_VFS_ERR_NO_MOUNT= 2,   /* 无匹配挂载点 */
    MQ_VFS_ERR_NOTFOUND= 3,   /* 挂载内不存在 */
    MQ_VFS_ERR_NOMEM   = 4,
    MQ_VFS_ERR_FORMAT  = 5,   /* 容器格式损坏（如 zip 头） */
    MQ_VFS_ERR_IO      = 6,   /* 网络/磁盘 IO */
    MQ_VFS_ERR_HASH    = 7    /* 内容哈希不匹配（远程资产校验） */
};

mq_vfs* mq_vfs_create(void);
void    mq_vfs_destroy(mq_vfs* vfs);

/* 挂载：mountpoint（如 "/" 或 "/assets"）绑定到 source。
 * source 形式："/abs/dir" | "file:///abs/dir" | "mem://name" | "zip:///abs/a.zip" | URL。
 * 解析用最长前缀匹配；后挂载者覆盖同前缀。成功 MQ_VFS_OK。 */
int mq_vfs_mount(mq_vfs* vfs, const char* mountpoint, const char* source);

/* 内存挂载：把字节注册为 "mem://name"，可被挂载点引用（docs/04 内存后端）。 */
int  mq_vfs_add_mem(mq_vfs* vfs, const char* name, const void* data, size_t size);
/* 归档挂载（zip，未压缩项）：直接挂载归档文件。 */
int  mq_vfs_mount_zip(mq_vfs* vfs, const char* mountpoint, const char* zip_path);

int mq_vfs_exists(mq_vfs* vfs, const char* path);

/* 注册 builtin 资源（引擎内建，区分于 assets；scheme "builtin:"）。 */
void mq_vfs_register_builtin(mq_vfs* vfs, const char* logical, const void* data, size_t size);

/* 读整个文件到新分配的缓冲；*out 用 mq_vfs_free 释放。成功 MQ_VFS_OK。
 * path 可为 "builtin:..."（内建）、"res://..."（等价于挂载内路径）或挂载路径。 */
int mq_vfs_read(mq_vfs* vfs, const char* path, void** out, size_t* out_size);
void mq_vfs_free(void* p);

/* 内容哈希（FNV-1a 64；确定性、无加密承诺——用于缓存键与远程资产校验）。 */
uint64_t mq_vfs_hash(const void* data, size_t size);
uint64_t mq_vfs_hash_path(mq_vfs* vfs, const char* path);

/* 读并校验内容哈希（远程资产：OD-9 内容哈希校验；不匹配 → MQ_VFS_ERR_HASH）。 */
int mq_vfs_read_verified(mq_vfs* vfs, const char* path, uint64_t expect, void** out, size_t* out_size);

/* 解析路径到"最终来源"（诊断/AI：docs/04 §挂载矩阵）。写入 out（可为 NULL 探测）。 */
int mq_vfs_resolve(mq_vfs* vfs, const char* path, char* out, size_t out_cap);

#ifdef __cplusplus
}
#endif
