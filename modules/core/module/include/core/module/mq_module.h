/* maque 模块系统 —— 插件即 .so，接口为带版本的 C vtable (docs/02)。
 * 每个模块仅导出 `mq_module_query`；宿主按 (name, version) 查找接口。
 * 跨边界只用 C ABI：无异常、无 RTTI、无 STL、分配者释放。 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 当前 ABI 主版本：不兼容即拒绝（docs/02 §3）。 */
#define MQ_ABI_VERSION 1u

/* 模块导出符号名与入口签名。 */
#define MQ_MODULE_QUERY_SYMBOL "mq_module_query"

/* 接口结构体头部：所有接口 struct 首字段必须是它。 */
typedef struct mq_api_header {
    uint32_t struct_size;   /* = sizeof(接口结构体)；前向兼容，只增不改 */
    uint32_t abi_version;
} mq_api_header;

/* 模块查询入口：返回接口指针（不匹配返回 NULL）。 */
typedef void* (*mq_module_query_fn)(const char* interface_name, uint32_t min_version);

/* 导出宏（保证符号可见）。 */
#if defined(_WIN32)
#  define MQ_MODULE_EXPORT __declspec(dllexport)
#else
#  define MQ_MODULE_EXPORT __attribute__((visibility("default")))
#endif

typedef struct mq_host mq_host;
typedef struct mq_loaded_module mq_loaded_module;

/* 宿主（每实例一个；零全局可变状态，docs/02 §5）。 */
mq_host* mq_host_create(void);
void     mq_host_destroy(mq_host* host);

/* 加载模块（dlopen + 解析 mq_module_query）。成功 0。 */
int  mq_host_load(mq_host* host, const char* path, mq_loaded_module** out_module);
/* 卸载模块（dlclose）。 */
void mq_host_unload(mq_host* host, mq_loaded_module* module);
uint32_t mq_host_module_count(mq_host* host);

/* 在已加载模块中按 (name, min_version) 查找接口。
 * 校验：header.struct_size >= sizeof(mq_api_header) 且 header.abi_version >= min_version。 */
void* mq_host_query(mq_host* host, const char* interface_name, uint32_t min_version);

#ifdef __cplusplus
}
#endif
