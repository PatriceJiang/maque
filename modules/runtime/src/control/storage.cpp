/* 控制面 · 键值存储（docs/25；变更类受 control_mutating 限制）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

/* 控制面（docs/11 §3、docs/25）：只读 info/list/get，变更 put/remove（只读会话拒绝）。 */
extern "C" int mq_control_Storage_info(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->storage) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no storage\"}"); return 0; }
    std::string root;
    write_json_string_pub(root, mq_storage_root(e->storage));
    std::string ns;
    write_json_string_pub(ns, mq_storage_ns(e->storage));
    std::snprintf(out, cap, "{\"ok\":true,\"mode\":\"%s\",\"root\":%s,\"ns\":%s}",
                  mq_storage_mode(e->storage), root.c_str(), ns.c_str());
    return 0;
}

extern "C" int mq_control_Storage_list(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    if (!e->storage) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no storage\"}"); return 0; }
    std::vector<char> buf(65536, '\0');
    const int rc = mq_storage_keys(e->storage, buf.data(), buf.size());
    if (rc != MQ_STORAGE_OK) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"keys failed (%d)\"}", rc); return 0; }
    std::snprintf(out, cap, "{\"ok\":true,\"keys\":%s}", buf.data());
    return 0;
}

extern "C" int mq_control_Storage_get(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* key = mq_json_str(mq_json_get(params, "key"), nullptr);
    if (!e->storage || !key) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing key\"}"); return 0; }
    void* d = nullptr; size_t n = 0;
    const int rc = mq_storage_get(e->storage, key, &d, &n);
    if (rc != MQ_STORAGE_OK) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"notfound\"}"); return 0; }
    std::string value;
    write_json_string_pub(value, std::string((char*)d, n));
    mq_storage_free(d);
    std::snprintf(out, cap, "{\"ok\":true,\"value\":%s}", value.c_str());
    return 0;
}

extern "C" int mq_control_Storage_put(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* key = mq_json_str(mq_json_get(params, "key"), nullptr);
    const char* value = mq_json_str(mq_json_get(params, "value"), nullptr);
    if (!e->storage || !key || !value) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing key/value\"}"); return 0; }
    const int rc = mq_storage_put(e->storage, key, value, std::strlen(value));
    std::snprintf(out, cap, "{\"ok\":%s,\"error\":\"%s\"}", rc == MQ_STORAGE_OK ? "true" : "false",
                  rc == MQ_STORAGE_OK ? "" : "put failed");
    return 0;
}

extern "C" int mq_control_Storage_remove(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* key = mq_json_str(mq_json_get(params, "key"), nullptr);
    if (!e->storage || !key) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing key\"}"); return 0; }
    const int rc = mq_storage_remove(e->storage, key);
    std::snprintf(out, cap, "{\"ok\":%s,\"error\":\"%s\"}", rc == MQ_STORAGE_OK ? "true" : "false",
                  rc == MQ_STORAGE_OK ? "" : (rc == MQ_STORAGE_ERR_NOTFOUND ? "notfound" : "remove failed"));
    return 0;
}
