/* mem:// 后端：进程内键值（**默认模式**：不落盘，测试/临时数据）。 */
#include "../storage_internal.h"

#include <algorithm>

namespace maque::storage {

int mem_apply(MemStore& store, const Request& req, Response& out) {
    const std::string k = rel_path(req.ns, req.key);
    switch (req.op) {
        case Op::Get: {
            auto it = store.items.find(k);
            if (it == store.items.end()) return MQ_STORAGE_ERR_NOTFOUND;
            out.value = it->second;
            return MQ_STORAGE_OK;
        }
        case Op::Put: {
            store.items[k].assign((const uint8_t*)req.data, (const uint8_t*)req.data + req.size);
            return MQ_STORAGE_OK;
        }
        case Op::Remove: {
            auto it = store.items.find(k);
            if (it == store.items.end()) return MQ_STORAGE_ERR_NOTFOUND;
            store.items.erase(it);
            return MQ_STORAGE_OK;
        }
        case Op::Keys: {
            const std::string prefix = req.ns + "/";
            std::vector<std::string> keys;
            for (const auto& [key, _] : store.items) {
                if (key.rfind(prefix, 0) == 0) keys.push_back(key.substr(prefix.size()));
            }
            std::sort(keys.begin(), keys.end());          // 确定性输出
            out.json = keys_json(keys);
            return MQ_STORAGE_OK;
        }
    }
    return MQ_STORAGE_ERR_ARG;
}

} // namespace maque::storage
