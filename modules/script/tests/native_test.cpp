// native 脚本后端单元测试（plans/26-cpp-scripts.md）：
// 加载上面编出的 guest、校验 API 表、事件派发、错误、热重载与 ABI 门。
#include "core/script/mq_script.h"
#include "maque_guest_api.h"

#include <cstdio>
#include <cstring>
#include <print>
#include <string>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

namespace {

struct HostState {
    int logs = 0;
    int quits = 0;
    std::string last;
};

void host_log(void* ctx, const char* msg) {
    auto* s = static_cast<HostState*>(ctx);
    ++s->logs;
    s->last = msg ? msg : "";
}

void host_quit(void* ctx) {
    ++static_cast<HostState*>(ctx)->quits;
}

float math_clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

mq_script_api_v1 make_api(HostState& st) {
    (void)st;
    mq_script_api_v1 api{};
    api.header.struct_size = (uint32_t)sizeof api;
    api.header.abi_version = MQ_SCRIPT_API_VERSION;
    api.api_hash = MQ_SCRIPT_API_HASH;
    api.target = MQ_TARGET_KEY;
    api.engine_version = "test";
    api.host_log = &host_log;
    api.host_quit = &host_quit;
    api.math_clamp = &math_clamp;
    return api;
}

} // namespace

int main() {
    HostState st;
    mq_script_api_v1 api = make_api(st);

    mq_script_vm* vm = mq_script_vm_create_lang("native");
    CHECK(vm != nullptr);
    CHECK(std::strcmp(mq_script_lang(vm), "native") == 0);

    mq_script_vm_native_config cfg{};
    cfg.struct_size = sizeof cfg;
    cfg.api = &api;
    cfg.host_ctx = &st;
    CHECK(mq_script_vm_configure_native(vm, &cfg) == 0);

    char err[512] = { 0 };
    CHECK(mq_script_eval(vm, "", MQ_NATIVE_TEST_GUEST) == 0);

    CHECK(mq_script_has(vm, "on_start") == 1);
    CHECK(mq_script_has(vm, "update") == 1);
    CHECK(mq_script_has(vm, "on_text") == 1);
    CHECK(mq_script_has(vm, "nope") == 0);

    CHECK(mq_script_call0(vm, "on_start", nullptr) == 0);
    CHECK(st.logs == 1 && st.last == "on_start");

    CHECK(mq_script_call1(vm, "update", 1.0 / 60.0, nullptr) == 0);
    CHECK(mq_script_call1(vm, "update", 1.0 / 60.0, nullptr) == 0);
    CHECK(mq_script_call1(vm, "update", 1.0 / 60.0, nullptr) == 0);
    CHECK(st.logs == 4);

    CHECK(mq_script_call1s(vm, "on_text", 0.0, "hello", nullptr) == 0);
    CHECK(st.last == "hello");
    CHECK(st.logs == 5);

    /* 未知处理器：rc=1，last_error 非空（错误隔离不等于静默）。 */
    double ret = 0.0;
    CHECK(mq_script_call0(vm, "nope", &ret) == 1);
    CHECK(mq_script_last_error(vm, err, (int)sizeof err) == 0 && err[0] != '\0');

    /* 热重载：同一 VM 重新 eval（旧 guest 销毁、新 guest 创建）。 */
    CHECK(mq_script_eval(vm, "", MQ_NATIVE_TEST_GUEST) == 0);
    CHECK(mq_script_has(vm, "on_start") == 1);
    CHECK(mq_script_call0(vm, "on_start", nullptr) == 0);
    CHECK(st.last == "on_start");

    /* 坏产物：明确失败，且 last_error 有信息。 */
    CHECK(mq_script_eval(vm, "", "/nonexistent/maque_native_missing.so") == 1);
    CHECK(mq_script_last_error(vm, err, (int)sizeof err) == 0 && err[0] != '\0');

    /* ABI 门：宿主 API 哈希不匹配 —— guest create 失败，不能加载。 */
    mq_script_api_v1 bad = api;
    bad.api_hash ^= 0xffffffffu;
    mq_script_vm_native_config bad_cfg = cfg;
    bad_cfg.api = &bad;
    mq_script_vm* bad_vm = mq_script_vm_create_lang("native");
    CHECK(bad_vm != nullptr);
    CHECK(mq_script_vm_configure_native(bad_vm, &bad_cfg) == 0);
    CHECK(mq_script_eval(bad_vm, "", MQ_NATIVE_TEST_GUEST) == 1);
    CHECK(mq_script_last_error(bad_vm, err, (int)sizeof err) == 0 &&
          std::strstr(err, "hash") != nullptr);
    mq_script_vm_destroy(bad_vm);

    /* target 门。 */
    mq_script_api_v1 wrong_target = api;
    wrong_target.target = "not-this-target";
    mq_script_vm_native_config wrong_cfg = cfg;
    wrong_cfg.api = &wrong_target;
    mq_script_vm* wrong_vm = mq_script_vm_create_lang("native");
    CHECK(wrong_vm != nullptr);
    CHECK(mq_script_vm_configure_native(wrong_vm, &wrong_cfg) == 0);
    CHECK(mq_script_eval(wrong_vm, "", MQ_NATIVE_TEST_GUEST) == 1);
    CHECK(mq_script_last_error(wrong_vm, err, (int)sizeof err) == 0 &&
          std::strstr(err, "target") != nullptr);
    mq_script_vm_destroy(wrong_vm);

    /* configure 只对 native 生效。 */
    mq_script_vm* js = mq_script_vm_create_lang("js");
    CHECK(js != nullptr);
    CHECK(mq_script_vm_configure_native(js, &cfg) == 1);
    mq_script_vm_destroy(js);

    /* 后端列表包含 native。 */
    char list[128] = { 0 };
    CHECK(mq_script_backends(list, (int)sizeof list) == 0);
    CHECK(std::strstr(list, "native") != nullptr);

    mq_script_vm_destroy(vm);

    if (g_failures == 0) {
        std::print("native_test: all checks passed\n");
        return 0;
    }
    std::print("native_test: {} failure(s)\n", g_failures);
    return 1;
}
