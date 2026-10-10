#include "core/handle/mq_handle.h"

#include <vector>
#include <cstddef>

namespace {

struct Slot {
    uint32_t generation = 0;
    bool     alive = false;
    void*    storage = nullptr;
};

struct Table {
    size_t elem_size = 0;
    size_t elem_align = 0;
    std::vector<Slot> slots;
    std::vector<uint32_t> free_list;  /* 可复用槽 index */
    uint32_t live = 0;

    void* make_storage() const {
        /* 对齐分配（C++17 aligned new）。 */
        return ::operator new(elem_size, std::align_val_t(elem_align));
    }
    void drop_storage(void* p) const {
        ::operator delete(p, std::align_val_t(elem_align));
    }
};

} // namespace

extern "C" {

mq_handle_table* mq_handle_table_create(size_t elem_size, size_t elem_align) {
    if (elem_size == 0) elem_size = 1;
    if (elem_align == 0) elem_align = 1;
    if (elem_align > 16) elem_align = 16;
    auto* t = new Table();
    t->elem_size = elem_size;
    t->elem_align = elem_align;
    return reinterpret_cast<mq_handle_table*>(t);
}

void mq_handle_table_destroy(mq_handle_table* table) {
    if (!table) return;
    auto* t = reinterpret_cast<Table*>(table);
    for (auto& s : t->slots)
        if (s.storage) t->drop_storage(s.storage);
    delete t;
}

mq_handle mq_handle_table_alloc(mq_handle_table* table, void** out_storage) {
    if (out_storage) *out_storage = nullptr;
    if (!table) return MQ_HANDLE_INVALID;
    auto* t = reinterpret_cast<Table*>(table);

    uint32_t idx;
    if (!t->free_list.empty()) {
        idx = t->free_list.back();
        t->free_list.pop_back();
    } else {
        idx = static_cast<uint32_t>(t->slots.size());
        t->slots.emplace_back();
        t->slots[idx].storage = t->make_storage();
    }
    Slot& s = t->slots[idx];
    s.alive = true;
    ++t->live;

    /* 清零存储（确定、可复现；docs/07 §9）。 */
    for (size_t i = 0; i < t->elem_size; ++i)
        reinterpret_cast<unsigned char*>(s.storage)[i] = 0;

    if (out_storage) *out_storage = s.storage;
    return mq_handle{ idx, s.generation };
}

int mq_handle_table_free(mq_handle_table* table, mq_handle h) {
    if (!table) return 1;
    auto* t = reinterpret_cast<Table*>(table);
    if (h.index >= t->slots.size()) return 1;
    Slot& s = t->slots[h.index];
    if (!s.alive || s.generation != h.generation) return 1;
    s.alive = false;
    ++s.generation;                 /* 世代自增 → 旧句柄失效 */
    t->free_list.push_back(h.index);
    --t->live;
    return 0;
}

void* mq_handle_table_get(mq_handle_table* table, mq_handle h) {
    if (!table) return nullptr;
    auto* t = reinterpret_cast<Table*>(table);
    if (h.index >= t->slots.size()) return nullptr;
    Slot& s = t->slots[h.index];
    if (!s.alive || s.generation != h.generation) return nullptr;
    return s.storage;
}

int mq_handle_table_valid(mq_handle_table* table, mq_handle h) {
    return mq_handle_table_get(table, h) != nullptr;
}

uint32_t mq_handle_table_count(mq_handle_table* table) {
    return table ? reinterpret_cast<Table*>(table)->live : 0u;
}

uint32_t mq_handle_table_capacity(mq_handle_table* table) {
    return table ? static_cast<uint32_t>(reinterpret_cast<Table*>(table)->slots.size()) : 0u;
}

mq_handle mq_handle_table_handle_at(mq_handle_table* table, uint32_t index) {
    if (!table) return MQ_HANDLE_INVALID;
    auto* t = reinterpret_cast<Table*>(table);
    if (index >= t->slots.size()) return MQ_HANDLE_INVALID;
    Slot& s = t->slots[index];
    if (!s.alive) return MQ_HANDLE_INVALID;
    return mq_handle{ index, s.generation };
}

} // extern "C"
