/* 全局 operator new/delete 拦截（**可选**：-DMAQUE_TRACY_ALLOC=ON，docs/29 §10.3）。
 *
 * 目的：把引擎里**所有** C++ 堆分配计入 `Mem::heap`，并作为 `TracyAlloc/TracyFree` 事件
 * 送进 Tracy 的 Memory 视图（按指针配对，能看存活块与调用点）。
 *
 * 代价：每块多 16B 头（对齐分配 24B）+ 每分配两次原子操作 —— 这是"性能剖析构建"选项，
 * 默认 OFF，不适合作长期 Release。关闭时本文件不参与编译（CMake 只按需加入源文件）。
 *
 * 实现要点：
 *   · 账本放在**分配头**里（domain 固定为 heap），free 时按头回冲，跨阶段释放不会记错域；
 *   · `accounted` 标记：跟踪期间 Tracy 自己用 new 分配的内部块不计账，free 时也就不会回冲；
 *   · 对齐分配（align_val_t > 16）用 malloc + 手工对齐，头里存原始指针；
 *   · 全部是 noexcept/POD，可在 main 之前（静态初始化）安全使用。
 */
#include "core/tracy/mq_tracy.h"

#if MQ_TRACY_ALLOC_ENABLED

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace {

/* 分配头：16B（默认 new 返回的地址仍然 16 对齐）。`accounted` 见文件头。 */
constexpr std::size_t kHdrBytes = 16;
struct Hdr {
    std::size_t size;
    std::uint32_t accounted;
    std::uint32_t pad;
};
static_assert(sizeof(Hdr) == kHdrBytes, "分配头必须是 16B（保持 malloc 的 16 对齐）");

/* 对齐分配头：24B（raw 指针 + size + accounted）。 */
struct AlignedHdr {
    void* raw;
    std::size_t size;
    std::uint32_t accounted;
    std::uint32_t pad;
};
static_assert(sizeof(AlignedHdr) == 24, "对齐分配头尺寸固定");

/* 重入护栏：跟踪期间（Tracy 内部）的分配不记账，避免污染 heap 与无限递归。 */
thread_local bool t_tracking = false;

bool begin_track() noexcept {
    if (t_tracking) return false;
    t_tracking = true;
    return true;
}
void end_track() noexcept { t_tracking = false; }

/* 只做"记账 + Memory 事件"，不分配。 */
void track_alloc(void* p, std::size_t n) noexcept {
    maque::tracy::memory_add(maque::tracy::Mem::heap, n);
    maque::tracy::memory_note_alloc(p, n, maque::tracy::Mem::heap);
}

void track_free(void* p, std::size_t n) noexcept {
    maque::tracy::memory_sub(maque::tracy::Mem::heap, n);
    maque::tracy::memory_note_free(p, maque::tracy::Mem::heap);
}

void* alloc_plain(std::size_t n) noexcept {
    void* raw = std::malloc(n + kHdrBytes);
    if (!raw) return nullptr;
    auto* h = static_cast<Hdr*>(raw);
    h->size = n;
    h->pad = 0;
    void* p = static_cast<char*>(raw) + kHdrBytes;
    if (maque::tracy::memory_enabled() && begin_track()) {
        track_alloc(p, n);
        end_track();
        h->accounted = 1;
    } else {
        h->accounted = 0;
    }
    return p;
}

void free_plain(void* p) noexcept {
    if (!p) return;
    auto* h = reinterpret_cast<Hdr*>(static_cast<char*>(p) - kHdrBytes);
    if (h->accounted) track_free(p, h->size);
    std::free(h);
}

void* alloc_aligned(std::size_t n, std::size_t align) noexcept {
    if (align <= kHdrBytes) return alloc_plain(n);   /* 16B 头天然满足 <=16 对齐 */
    const std::size_t total = n + sizeof(AlignedHdr) + align;
    void* raw = std::malloc(total);
    if (!raw) return nullptr;
    const std::uintptr_t base =
        (reinterpret_cast<std::uintptr_t>(raw) + sizeof(AlignedHdr) + align - 1) & ~(std::uintptr_t)(align - 1);
    void* p = reinterpret_cast<void*>(base);
    auto* h = reinterpret_cast<AlignedHdr*>(base - sizeof(AlignedHdr));
    h->raw = raw;
    h->size = n;
    h->pad = 0;
    if (maque::tracy::memory_enabled() && begin_track()) {
        track_alloc(p, n);
        end_track();
        h->accounted = 1;
    } else {
        h->accounted = 0;
    }
    return p;
}

void free_aligned(void* p) noexcept {
    if (!p) return;
    auto* h = reinterpret_cast<AlignedHdr*>(static_cast<char*>(p) - sizeof(AlignedHdr));
    if (h->accounted) track_free(p, h->size);
    std::free(h->raw);
}

} // namespace

/* ---- 全局替换（C++ 保证 new/delete 成对重载被调用；数组/定位/aligned 变体齐全）---- */

void* operator new(std::size_t n) {
    void* p = alloc_plain(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n) {
    void* p = alloc_plain(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return alloc_plain(n ? n : 1);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return alloc_plain(n ? n : 1);
}

void* operator new(std::size_t n, std::align_val_t al) {
    void* p = alloc_aligned(n ? n : 1, static_cast<std::size_t>(al));
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t al) {
    void* p = alloc_aligned(n ? n : 1, static_cast<std::size_t>(al));
    if (!p) throw std::bad_alloc();
    return p;
}

void* operator new(std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return alloc_aligned(n ? n : 1, static_cast<std::size_t>(al));
}

void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept {
    return alloc_aligned(n ? n : 1, static_cast<std::size_t>(al));
}

void operator delete(void* p) noexcept { free_plain(p); }
void operator delete[](void* p) noexcept { free_plain(p); }
void operator delete(void* p, std::size_t) noexcept { free_plain(p); }
void operator delete[](void* p, std::size_t) noexcept { free_plain(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { free_plain(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { free_plain(p); }
void operator delete(void* p, std::align_val_t al) noexcept {
    if (static_cast<std::size_t>(al) <= kHdrBytes) free_plain(p);
    else free_aligned(p);
}
void operator delete[](void* p, std::align_val_t al) noexcept {
    if (static_cast<std::size_t>(al) <= kHdrBytes) free_plain(p);
    else free_aligned(p);
}
void operator delete(void* p, std::size_t, std::align_val_t al) noexcept {
    if (static_cast<std::size_t>(al) <= kHdrBytes) free_plain(p);
    else free_aligned(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t al) noexcept {
    if (static_cast<std::size_t>(al) <= kHdrBytes) free_plain(p);
    else free_aligned(p);
}
void operator delete(void* p, std::align_val_t al, const std::nothrow_t&) noexcept {
    if (static_cast<std::size_t>(al) <= kHdrBytes) free_plain(p);
    else free_aligned(p);
}
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept {
    if (static_cast<std::size_t>(al) <= kHdrBytes) free_plain(p);
    else free_aligned(p);
}

namespace maque::tracy {
/* 链接锚点（tracy.cpp 引用）：保证本目标文件被链进可执行文件，从而 operator new 生效。 */
void mem_alloc_link_anchor() {}
} // namespace maque::tracy

#endif  /* MQ_TRACY_ALLOC_ENABLED */
