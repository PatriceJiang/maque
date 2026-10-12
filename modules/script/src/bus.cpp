/* 事件总线（多 VM / 跨语言协作；docs/14 §多 VM、docs/19 §事件、plans/18）
 *
 * 语义（刻意保持极简，以便确定性可证）：
 *   · **顺序 = 加入顺序**（= 场景里 scripts 的声明顺序）→ 事件投递顺序完全确定，可回放；
 *   · **只投给订阅者**：定义了该处理器的 VM 才收到调用（`mq_script_has` 判定）；
 *   · **只传标量与字符串**（现有 payload 语义）：跨语言不共享对象/表，事件即消息；
 *   · **错误隔离**：每个 VM 的调用各自受保护（JS 异常 / Lua pcall），一个脚本出错不影响其它脚本；
 *   · **GC 逐 VM 在帧边界推进**（docs/10 D7），不因多 VM 改变 GC 时机；
 *   · 总线**拥有**其 VM：销毁总线即销毁全部 VM（生命周期单点，避免悬空）。
 */
#include "abi/script.h"

#include <cstring>
#include <string>
#include <vector>

struct mq_script_bus {
    struct Entry {
        mq_script_vm* vm;
        std::string name;      /* 脚本路径（自省/诊断；不参与语义） */
    };
    std::vector<Entry> vms;
    mq_script_vm* last_error_vm = nullptr;
    uint32_t errors = 0;      /* 累计出错次数（单调；调用方比较增量判定"本次派发是否出错"） */
};

extern "C" mq_script_bus* mq_script_bus_create(void) {
    return new mq_script_bus();
}

extern "C" int mq_script_bus_add(mq_script_bus* bus, mq_script_vm* vm, const char* name) {
    if (!bus || !vm) return -1;
    bus->vms.push_back({ vm, name ? name : "" });
    return (int)bus->vms.size() - 1;
}

/* 把已准备好的 VM 换入槽位（热重载原子替换）：销毁旧 VM、**顺序不变**。
 * 由调用方先完成"创建 + eval"，成功后再换入 —— 编译/加载失败时旧 VM 原样保留（热更出错不致命）。 */
extern "C" mq_script_vm* mq_script_bus_swap(mq_script_bus* bus, uint32_t index, mq_script_vm* vm, const char* name) {
    if (!bus || index >= bus->vms.size() || !vm) return nullptr;
    mq_script_vm_destroy(bus->vms[index].vm);
    bus->vms[index].vm = vm;
    if (name) bus->vms[index].name = name;
    return vm;
}

extern "C" uint32_t mq_script_bus_count(mq_script_bus* bus) {
    return bus ? (uint32_t)bus->vms.size() : 0;
}

extern "C" mq_script_vm* mq_script_bus_vm_at(mq_script_bus* bus, uint32_t index) {
    if (!bus || index >= bus->vms.size()) return nullptr;
    return bus->vms[index].vm;
}

extern "C" const char* mq_script_bus_name_at(mq_script_bus* bus, uint32_t index) {
    if (!bus || index >= bus->vms.size()) return "";
    return bus->vms[index].name.c_str();
}

extern "C" int mq_script_bus_has(mq_script_bus* bus, const char* fn) {
    if (!bus || !fn || !*fn) return 0;
    for (const auto& e : bus->vms)
        if (mq_script_has(e.vm, fn)) return 1;
    return 0;
}

extern "C" int mq_script_bus_call(mq_script_bus* bus, const char* fn, double a, double b, const char* text) {
    if (!bus || !fn || !*fn) return 0;
    int hits = 0;
    for (const auto& e : bus->vms) {
        if (!mq_script_has(e.vm, fn)) continue;          /* 只投给订阅者 */
        ++hits;
        /* 带字符串参数用 call2s，纯数值用 call2（两者都是受保护调用：出错只记录，不中断） */
        const int rc = text ? mq_script_call2s(e.vm, fn, a, b, text, nullptr)
                            : mq_script_call2(e.vm, fn, a, b, nullptr);
        if (rc != 0) { ++bus->errors; bus->last_error_vm = e.vm; }
    }
    return hits;
}

extern "C" int mq_script_bus_call3(mq_script_bus* bus, const char* fn, double a, double b, double c,
                                   const char* text) {
    if (!bus || !fn || !*fn) return 0;
    int hits = 0;
    for (const auto& e : bus->vms) {
        if (!mq_script_has(e.vm, fn)) continue;
        ++hits;
        const int rc = text ? mq_script_call3(e.vm, fn, a, b, c, nullptr)   /* 3 参数 + 文本：先按 3 数值调用 */
                            : mq_script_call3(e.vm, fn, a, b, c, nullptr);
        (void)text;
        if (rc != 0) { ++bus->errors; bus->last_error_vm = e.vm; }
    }
    return hits;
}

extern "C" uint32_t mq_script_bus_error_count(mq_script_bus* bus) {
    return bus ? bus->errors : 0;
}

extern "C" void mq_script_bus_gc(mq_script_bus* bus) {
    if (!bus) return;
    for (const auto& e : bus->vms) mq_script_frame_boundary(e.vm);   /* 逐 VM，帧边界一次 */
}

extern "C" int mq_script_bus_last_error(mq_script_bus* bus, char* out, int cap) {
    if (!bus || !out || cap <= 0) return -1;
    out[0] = '\0';
    if (!bus->last_error_vm) return -1;
    return mq_script_last_error(bus->last_error_vm, out, cap);
}

extern "C" void mq_script_bus_set_rng(mq_script_bus* bus, mq_script_rng_fn fn, void* user) {
    if (!bus) return;
    for (const auto& e : bus->vms) mq_script_set_rng(e.vm, fn, user);   /* 同一 RNG → 跨语言同序列 */
}

extern "C" void mq_script_bus_destroy(mq_script_bus* bus) {
    if (!bus) return;
    for (const auto& e : bus->vms) mq_script_vm_destroy(e.vm);          /* 总线拥有 VM */
    delete bus;
}
