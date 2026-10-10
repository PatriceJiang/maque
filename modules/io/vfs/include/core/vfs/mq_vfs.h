/* maque 虚拟文件系统（最小切片，docs/04）：挂载点 + 协议后端。
 * 本切片实现 file:// 后端与最长前缀挂载；后续扩展 mem/http/zip。 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_vfs mq_vfs;

mq_vfs* mq_vfs_create(void);
void    mq_vfs_destroy(mq_vfs* vfs);

/* 挂载：mountpoint（如 "/" 或 "/assets"）绑定到 source（如 "file:///abs/dir" 或 "/abs/dir"）。
 * 解析用最长前缀匹配；后挂载者可覆盖同前缀。成功 0。 */
int mq_vfs_mount(mq_vfs* vfs, const char* mountpoint, const char* source);

int mq_vfs_exists(mq_vfs* vfs, const char* path);

/* 读整个文件到新分配的缓冲；*out 用 mq_vfs_free 释放。成功 0。 */
int mq_vfs_read(mq_vfs* vfs, const char* path, void** out, size_t* out_size);
void mq_vfs_free(void* p);

#ifdef __cplusplus
}
#endif
