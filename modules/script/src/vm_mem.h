/* 脚本 VM 内存记账（docs/29 §10.2：内存域 `script`）。
 *
 * QuickJS / Lua 都提供**自定义分配器**钩子（JS_NewRuntime2 / lua_newstate），这里把两者
 * 统一接到 maque::tracy::memory_add/sub：
 *   · 账本 = malloc 的 usable size（不额外加分配头 → 内存零开销；`_msize`/`malloc_usable_size`
 *     只是读 malloc 头，成本与一次函数调用同级）；
 *   · `MAQUE_TRACY_MEM=0` 运行期关闭时只多一次 bool 检查，直接走 std::malloc/free；
 *   · 未编入内存记账（MQ_TRACY_MEMORY_ENABLED=0）时整个 helper 不存在，调用点走原来的
 *     JS_NewRuntime()/luaL_newstate() 路径 —— 行为与旧版逐字节一致。
 *
 * 只在本模块内部使用（不进公开 ABI）。 */
#pragma once

#include "core/tracy/mq_tracy.h"

#if MQ_TRACY_MEMORY_ENABLED

#include <cstdlib>
#include <cstddef>

#if defined(_WIN32)
#  include <malloc.h>
#elif defined(__APPLE__)
#  include <malloc/malloc.h>
#else
#  include <malloc.h>
#endif

namespace maque::script::mem {

/* malloc 实际块大小（与 QuickJS 自己的 malloc_size 记账口径一致）。 */
inline size_t usable_size(const void* p) {
#if defined(_WIN32)
    return p ? _msize(const_cast<void*>(p)) : 0;
#elif defined(__APPLE__)
    return p ? malloc_size(p) : 0;
#else
    return p ? malloc_usable_size(const_cast<void*>(p)) : 0;
#endif
}

inline void* tracked_malloc(size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (p && maque::tracy::memory_enabled())
        maque::tracy::memory_add(maque::tracy::Mem::script, usable_size(p));
    return p;
}

inline void* tracked_calloc(size_t count, size_t size) {
    void* p = std::calloc(count ? count : 1, size ? size : 1);
    if (p && maque::tracy::memory_enabled())
        maque::tracy::memory_add(maque::tracy::Mem::script, usable_size(p));
    return p;
}

inline void tracked_free(void* p) {
    if (!p) return;
    const bool on = maque::tracy::memory_enabled();
    const size_t n = on ? usable_size(p) : 0;
    std::free(p);
    if (on) maque::tracy::memory_sub(maque::tracy::Mem::script, n);
}

inline void* tracked_realloc(void* p, size_t n) {
    if (!p) return tracked_malloc(n);
    if (n == 0) {
        tracked_free(p);
        return nullptr;
    }
    const bool on = maque::tracy::memory_enabled();
    const size_t old_n = on ? usable_size(p) : 0;
    void* q = std::realloc(p, n);
    if (!q) return nullptr;                       /* 失败：原块仍然有效（未动账本） */
    if (on) {
        const size_t new_n = usable_size(q);
        if (new_n >= old_n) maque::tracy::memory_add(maque::tracy::Mem::script, new_n - old_n);
        else maque::tracy::memory_sub(maque::tracy::Mem::script, old_n - new_n);
    }
    return q;
}

} // namespace maque::script::mem

#endif  /* MQ_TRACY_MEMORY_ENABLED */
