/* 脚本 API · 键值存储（docs/25）：namespace storage，声明由 schema 生成
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）。 */
#include "../runtime_engine.h"

using namespace maque::rt;

namespace storage {

/* 返回值放进 Engine 的复用缓冲：生成的绑定会**立即**复制（ret_str → snprintf）→ 单线程安全 */
const char* get(void* ctx, const char* key) {
    auto* e = (Engine*)ctx;
    e->script_ret.clear();
    if (!e->storage || !key) return "";
    void* d = nullptr; size_t n = 0;
    if (mq_storage_get(e->storage, key, &d, &n) != MQ_STORAGE_OK) return "";
    e->script_ret.assign((const char*)d, n);
    mq_storage_free(d);
    return e->script_ret.c_str();
}

float set(void* ctx, const char* key, const char* value) {
    auto* e = (Engine*)ctx;
    if (!e->storage || !key || !value) return 0.0F;
    return mq_storage_put(e->storage, key, value, std::strlen(value)) == MQ_STORAGE_OK ? 1.0F : 0.0F;
}

float remove(void* ctx, const char* key) {
    auto* e = (Engine*)ctx;
    if (!e->storage || !key) return 0.0F;
    return mq_storage_remove(e->storage, key) == MQ_STORAGE_OK ? 1.0F : 0.0F;
}

float exists(void* ctx, const char* key) {
    auto* e = (Engine*)ctx;
    return (e->storage && key && mq_storage_exists(e->storage, key)) ? 1.0F : 0.0F;
}

const char* keys(void* ctx) {
    auto* e = (Engine*)ctx;
    e->script_ret.assign(65536, '\0');
    if (!e->storage || mq_storage_keys(e->storage, e->script_ret.data(), e->script_ret.size()) != MQ_STORAGE_OK) {
        e->script_ret = "[]";
        return e->script_ret.c_str();
    }
    e->script_ret.resize(std::strlen(e->script_ret.c_str()));
    return e->script_ret.c_str();
}

const char* info(void* ctx) {
    auto* e = (Engine*)ctx;
    std::string json = "{\"mode\":\"";
    json += e->storage ? mq_storage_mode(e->storage) : "";
    json += "\",\"root\":";
    { std::string q; write_json_string_pub(q, e->storage ? mq_storage_root(e->storage) : ""); json += q; }
    json += ",\"ns\":";
    { std::string q; write_json_string_pub(q, e->storage ? mq_storage_ns(e->storage) : ""); json += q; }
    json += "}";
    e->script_ret = std::move(json);
    return e->script_ret.c_str();
}

}  // namespace storage
