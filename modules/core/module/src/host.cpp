#include "core/module/mq_module.h"

#include <vector>
#include <cstring>
#include <dlfcn.h>

struct mq_loaded_module {
    void*              dl = nullptr;
    mq_module_query_fn query = nullptr;
};

struct mq_host {
    std::vector<mq_loaded_module*> modules;
};

extern "C" {

mq_host* mq_host_create(void) { return new mq_host(); }

void mq_host_destroy(mq_host* host) {
    if (!host) return;
    for (auto* m : host->modules) {
        if (m->dl) ::dlclose(m->dl);
        delete m;
    }
    delete host;
}

int mq_host_load(mq_host* host, const char* path, mq_loaded_module** out_module) {
    if (out_module) *out_module = nullptr;
    if (!host || !path) return 1;
    void* dl = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) return 2;
    auto* sym = ::dlsym(dl, MQ_MODULE_QUERY_SYMBOL);
    if (!sym) { ::dlclose(dl); return 3; }
    auto* m = new mq_loaded_module();
    m->dl = dl;
    m->query = reinterpret_cast<mq_module_query_fn>(sym);
    host->modules.push_back(m);
    if (out_module) *out_module = m;
    return 0;
}

void mq_host_unload(mq_host* host, mq_loaded_module* module) {
    if (!host || !module) return;
    for (size_t i = 0; i < host->modules.size(); ++i) {
        if (host->modules[i] == module) {
            host->modules.erase(host->modules.begin() + i);
            break;
        }
    }
    if (module->dl) ::dlclose(module->dl);
    delete module;
}

uint32_t mq_host_module_count(mq_host* host) {
    return host ? (uint32_t)host->modules.size() : 0u;
}

void* mq_host_query(mq_host* host, const char* interface_name, uint32_t min_version) {
    if (!host || !interface_name) return nullptr;
    for (auto* m : host->modules) {
        if (!m->query) continue;
        void* api = m->query(interface_name, min_version);
        if (!api) continue;
        auto* hdr = reinterpret_cast<mq_api_header*>(api);
        if (hdr->struct_size >= sizeof(mq_api_header) && hdr->abi_version >= min_version)
            return api;
    }
    return nullptr;
}

} // extern "C"
