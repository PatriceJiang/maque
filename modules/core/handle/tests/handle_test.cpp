// maque 句柄槽表单元测试（独立：仅依赖 maque::handle）
#include "core/handle/mq_handle.h"

#include <cstdio>
#include <cstdint>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    mq_handle_table* t = mq_handle_table_create(sizeof(int), alignof(int));
    CHECK(t != nullptr);
    CHECK(mq_handle_table_count(t) == 0);

    // 分配两个，写入并读回
    void* p0 = nullptr; mq_handle h0 = mq_handle_table_alloc(t, &p0);
    void* p1 = nullptr; mq_handle h1 = mq_handle_table_alloc(t, &p1);
    CHECK(h0.index != h1.index);
    CHECK(mq_handle_table_count(t) == 2);
    *(int*)p0 = 42;
    *(int*)p1 = 7;
    CHECK(*(int*)mq_handle_table_get(t, h0) == 42);
    CHECK(*(int*)mq_handle_table_get(t, h1) == 7);
    CHECK(mq_handle_table_valid(t, h0));

    // 释放 h0 → 失效；旧句柄不可用
    CHECK(mq_handle_table_free(t, h0) == 0);
    CHECK(mq_handle_table_count(t) == 1);
    CHECK(mq_handle_table_get(t, h0) == nullptr);
    CHECK(!mq_handle_table_valid(t, h0));
    CHECK(mq_handle_table_free(t, h0) == 1);   // 重复释放失败

    // 再次分配：复用 index，但世代不同
    void* p2 = nullptr; mq_handle h2 = mq_handle_table_alloc(t, &p2);
    CHECK(h2.index == h0.index);
    CHECK(h2.generation != h0.generation);
    CHECK(*(int*)mq_handle_table_get(t, h2) == 0);   // 新槽已清零
    *(int*)p2 = 99;
    CHECK(*(int*)mq_handle_table_get(t, h2) == 99);

    // 越界/无效句柄
    CHECK(mq_handle_table_get(t, MQ_HANDLE_INVALID) == nullptr);
    mq_handle bogus{ 9999u, 0u };
    CHECK(mq_handle_table_get(t, bogus) == nullptr);

    // 遍历存活句柄
    int live = 0;
    for (uint32_t i = 0; i < mq_handle_table_capacity(t); ++i) {
        mq_handle h = mq_handle_table_handle_at(t, i);
        if (h.index != MQ_HANDLE_INDEX_NONE) ++live;
    }
    CHECK(live == (int)mq_handle_table_count(t));
    CHECK(mq_handle_table_count(t) == 2);

    mq_handle_table_destroy(t);

    if (g_failures == 0) { std::printf("handle_test: all checks passed\n"); return 0; }
    std::printf("handle_test: %d failure(s)\n", g_failures);
    return 1;
}
