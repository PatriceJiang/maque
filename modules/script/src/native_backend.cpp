/* native 脚本后端（plans/26-cpp-scripts.md）：C/C++ 编译型 guest。
 *
 * 加载复用 module host（与渲染插件同一机制）：
 *   mq_host_load(path) → mq_host_query("maque.script.guest.v1") → guest vtable
 * guest 的 create 收到 Host API 表（mq_script_api_v1）与 Engine*（不透明 host_ctx），
 * 之后全部调用走 C vtable；不链接引擎库、不跨边界传 STL/异常。
 *
 * 规格（resolver 返回）：
 *   so:<路径> / soname:<库名>   → dlopen / LoadLibrary
 *   static:<key>                → 构建期注册表（Android/iOS/WASM/测试）
 * 无 resolver 时把 filename 直接当路径（单测方便）。
 *
 * 热重载：新 VM 成功后才由总线换入；旧 VM 的 destroy 只销毁实例/模块，
 * Windows 上 mq_host 默认不 FreeLibrary（见 modules/core/module/src/host.cpp）。 */
#include "vm_base.h"
#include "abi/script_guest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef MAQUE_HAVE_NATIVE

#include "maque_guest_api.h"
#include "core/module/mq_module.h"
#include "core/tracy/mq_tracy.h"

namespace {

struct NativeVm : mq_script_vm {
    mq_script_vm_native_config cfg{};
    bool configured = false;
    mq_host* host = nullptr;
    const mq_script_guest_vt* vt = nullptr;
    mq_script_guest* guest = nullptr;
    mq_script_guest_instance* inst = nullptr;
    std::string last_error;
    mq_script_rng_fn rng = nullptr;
    void* rng_user = nullptr;
};

struct StaticEntry {
    std::string key;
    const mq_script_guest_vt* vt = nullptr;
};

/* 静态注册表：进程级、启动期写入、之后只读。静态 guest 本来就是进程级代码；
 * 动态 guest 不走这里，因此不违反“实例之间不共享可变状态”。 */
std::vector<StaticEntry>& static_registry() {
    static std::vector<StaticEntry> entries;
    return entries;
}

NativeVm* S(mq_script_vm* vm) { return static_cast<NativeVm*>(vm); }
const NativeVm* S(const mq_script_vm* vm) { return static_cast<const NativeVm*>(vm); }

void set_error(NativeVm* v, const char* msg) {
    if (v) v->last_error = msg ? msg : "";
}

bool valid_vt(const mq_script_guest_vt* vt) {
    return vt && vt->header.struct_size >= sizeof(mq_api_header) &&
           vt->header.abi_version >= MQ_SCRIPT_GUEST_ABI_VERSION && vt->create && vt->destroy &&
           vt->instantiate && vt->instance_destroy && vt->instance_has && vt->instance_call;
}

void release_guest(NativeVm* v) {
    if (!v) return;
    if (v->inst && v->vt && v->vt->instance_destroy) v->vt->instance_destroy(v->inst);
    v->inst = nullptr;
    if (v->guest && v->vt && v->vt->destroy) v->vt->destroy(v->guest);
    v->guest = nullptr;
    v->vt = nullptr;
}

void release_host(NativeVm* v) {
    if (v && v->host) {
        mq_host_destroy(v->host);
        v->host = nullptr;
    }
}

/* 解析 resolver 规格并加载 guest。返回 0 成功。 */
int load_guest(NativeVm* v, const char* filename) {
    if (!v) return 1;
    if (!v->configured || !v->cfg.api) {
        set_error(v, "native: VM is not configured (call mq_script_vm_configure_native first)");
        return 1;
    }
    if (!filename || !*filename) {
        set_error(v, "native: empty script path");
        return 1;
    }

    char spec[1024] = { 0 };
    if (v->cfg.resolver) {
        if (v->cfg.resolver(v->cfg.resolver_user, filename, spec, (int)sizeof spec) != 0 || !spec[0]) {
            set_error(v, "native: resolver failed (no artifact for this target?)");
            return 1;
        }
    } else {
        std::snprintf(spec, sizeof spec, "so:%s", filename);
    }

    release_guest(v);
    release_host(v);

    const mq_script_guest_vt* vt = nullptr;
    if (std::strncmp(spec, "static:", 7) == 0) {
        const char* key = spec + 7;
        for (const StaticEntry& e : static_registry()) {
            if (e.key == key) { vt = e.vt; break; }
        }
        if (!vt) {
            v->last_error = std::string("native: static guest not registered: ") + key;
            return 1;
        }
    } else {
        const char* path = spec;
        if (std::strncmp(spec, "so:", 3) == 0) path = spec + 3;
        else if (std::strncmp(spec, "soname:", 7) == 0) path = spec + 7;
        if (!*path) {
            set_error(v, "native: empty artifact path");
            return 1;
        }
        v->host = mq_host_create();
        mq_loaded_module* mod = nullptr;
        const int rc = mq_host_load(v->host, path, &mod);
        if (rc != 0) {
            char msg[1200];
            std::snprintf(msg, sizeof msg, "native: cannot load guest '%s' (mq_host_load=%d)", path, rc);
            set_error(v, msg);
            release_host(v);
            return 1;
        }
        vt = static_cast<const mq_script_guest_vt*>(
            mq_host_query(v->host, MQ_SCRIPT_GUEST_IFACE_NAME, MQ_SCRIPT_GUEST_ABI_VERSION));
        if (!valid_vt(vt)) {
            char msg[1200];
            std::snprintf(msg, sizeof msg,
                          "native: '%s' does not export %s v%u", path,
                          MQ_SCRIPT_GUEST_IFACE_NAME, (unsigned)MQ_SCRIPT_GUEST_ABI_VERSION);
            set_error(v, msg);
            release_host(v);
            return 1;
        }
    }

    char err[512] = { 0 };
    mq_script_guest* guest = vt->create(v->cfg.api, v->cfg.host_ctx, err, (int)sizeof err);
    if (!guest) {
        v->last_error = std::string("native: guest create failed: ") + (err[0] ? err : "unknown");
        release_host(v);
        return 1;
    }

    mq_script_guest_mount mount{};
    mount.struct_size = sizeof mount;
    mount.node = -1;
    mount.scene = 0;
    mount.module_key = filename;
    mount.params = nullptr;
    mount.props = nullptr;
    mount.prop_count = 0;
    err[0] = 0;
    mq_script_guest_instance* inst = vt->instantiate(guest, &mount, err, (int)sizeof err);
    if (!inst) {
        v->last_error = std::string("native: guest instantiate failed: ") + (err[0] ? err : "unknown");
        vt->destroy(guest);
        release_host(v);
        return 1;
    }

    v->vt = vt;
    v->guest = guest;
    v->inst = inst;
    v->last_error.clear();
    return 0;
}

int call_impl(mq_script_vm* vm, const char* fn, int nnum, const double* nums, const char* text,
              double* ret) {
    NativeVm* v = S(vm);
    if (!v || !v->vt || !v->inst) {
        if (v) set_error(v, "native: no guest instance");
        return 1;
    }
    if (ret) *ret = 0.0;
    char err[512] = { 0 };
    const int rc = v->vt->instance_call(v->inst, fn, nnum, nums, text, ret, err, (int)sizeof err);
    if (rc == 0) {
        v->last_error.clear();
    } else if (rc == 1) {
        v->last_error = std::string("native: handler not found: ") + (fn ? fn : "?");
    } else {
        v->last_error = std::string("native: handler error: ") + (err[0] ? err : "unknown");
    }
    return rc;
}

} // namespace

extern "C" int mq_script_guest_register_static(const char* key, const mq_script_guest_vt* vt) {
    if (!key || !*key || !valid_vt(vt)) return 1;
    auto& reg = static_registry();
    for (const StaticEntry& e : reg) {
        if (e.key == key) return 1;
    }
    reg.push_back(StaticEntry{ key, vt });
    return 0;
}

namespace maque::script::native {

mq_script_vm* vm_create(void) {
    auto* v = new NativeVm();
    v->lang = "native";
    return v;
}

void vm_destroy(mq_script_vm* vm) {
    NativeVm* v = S(vm);
    if (!v) return;
    release_guest(v);
    release_host(v);
    delete v;
}

int configure(mq_script_vm* vm, const mq_script_vm_native_config* cfg) {
    NativeVm* v = S(vm);
    if (!v || !cfg) return 1;
    if (cfg->struct_size < sizeof(mq_script_vm_native_config)) return 1;
    if (!cfg->api) return 1;
    v->cfg = *cfg;
    v->configured = true;
    return 0;
}

int eval(mq_script_vm* vm, const char* src, const char* filename) {
    (void)src;   // native 的“源码”是编译产物；src 仅用于宿主侧哈希/诊断
    MQ_TRACY_ZONE_NAMED(mq_zone, script, "native.eval");
    MQ_TRACY_ZONE_TEXT(mq_zone, filename);
    NativeVm* v = S(vm);
    if (!v) return 1;
    return load_guest(v, filename);
}

int has(mq_script_vm* vm, const char* fn) {
    NativeVm* v = S(vm);
    if (!v || !v->vt || !v->inst || !fn) return 0;
    return v->vt->instance_has(v->inst, fn);
}

int call0(mq_script_vm* vm, const char* fn, double* ret) { return call_impl(vm, fn, 0, nullptr, nullptr, ret); }
int call1(mq_script_vm* vm, const char* fn, double a, double* ret) {
    const double nums[1] = { a };
    return call_impl(vm, fn, 1, nums, nullptr, ret);
}
int call2(mq_script_vm* vm, const char* fn, double a, double b, double* ret) {
    const double nums[2] = { a, b };
    return call_impl(vm, fn, 2, nums, nullptr, ret);
}
int call3(mq_script_vm* vm, const char* fn, double a, double b, double c, double* ret) {
    const double nums[3] = { a, b, c };
    return call_impl(vm, fn, 3, nums, nullptr, ret);
}
int call0s(mq_script_vm* vm, const char* fn, const char* s, double* ret) {
    return call_impl(vm, fn, 0, nullptr, s, ret);
}
int call1s(mq_script_vm* vm, const char* fn, double a, const char* s, double* ret) {
    const double nums[1] = { a };
    return call_impl(vm, fn, 1, nums, s, ret);
}
int call2s(mq_script_vm* vm, const char* fn, double a, double b, const char* s, double* ret) {
    const double nums[2] = { a, b };
    return call_impl(vm, fn, 2, nums, s, ret);
}

int last_error(mq_script_vm* vm, char* out, int cap) {
    NativeVm* v = S(vm);
    if (!v || !out || cap <= 0) return 1;
    if (v->last_error.empty()) { out[0] = '\0'; return 1; }
    std::snprintf(out, (size_t)cap, "%s", v->last_error.c_str());
    return 0;
}

void set_rng(mq_script_vm* vm, mq_script_rng_fn fn, void* user) {
    NativeVm* v = S(vm);
    if (!v) return;
    v->rng = fn;
    v->rng_user = user;
}

void gc(mq_script_vm* vm) { (void)vm; }              // C++ 无 VM GC；实例由 RAII 管理
void frame_boundary(mq_script_vm* vm) { (void)vm; }  // 无 deferred GC

int register_num(mq_script_vm*, const char*, mq_script_num_fn, void*) { return 0; }
int register_str(mq_script_vm*, const char*, mq_script_str_fn, void*) { return 0; }
int register_ret_str(mq_script_vm*, const char*, mq_script_ret_str_fn, void*) { return 0; }
int register_mixed(mq_script_vm*, const char*, mq_script_mixed_fn, void*) { return 0; }
int register_multi(mq_script_vm*, const char*, const char*, mq_script_multi_fn, void*) { return 0; }
int register_multi_ret_str(mq_script_vm*, const char*, const char*, mq_script_multi_ret_str_fn, void*) { return 0; }
int register_bytes(mq_script_vm*, const char*, const char*, mq_script_bytes_fn, void*) { return 0; }

} // namespace maque::script::native

#else  /* !MAQUE_HAVE_NATIVE */

extern "C" int mq_script_guest_register_static(const char*, const mq_script_guest_vt*) { return 1; }

namespace maque::script::native {
mq_script_vm* vm_create(void) { return nullptr; }
void vm_destroy(mq_script_vm*) {}
int configure(mq_script_vm*, const mq_script_vm_native_config*) { return 1; }
int eval(mq_script_vm*, const char*, const char*) { return 1; }
int has(mq_script_vm*, const char*) { return 0; }
int call0(mq_script_vm*, const char*, double*) { return 1; }
int call1(mq_script_vm*, const char*, double, double*) { return 1; }
int call2(mq_script_vm*, const char*, double, double, double*) { return 1; }
int call3(mq_script_vm*, const char*, double, double, double, double*) { return 1; }
int call0s(mq_script_vm*, const char*, const char*, double*) { return 1; }
int call1s(mq_script_vm*, const char*, double, const char*, double*) { return 1; }
int call2s(mq_script_vm*, const char*, double, double, const char*, double*) { return 1; }
int last_error(mq_script_vm*, char* out, int cap) { if (out && cap > 0) out[0] = '\0'; return 1; }
void set_rng(mq_script_vm*, mq_script_rng_fn, void*) {}
void gc(mq_script_vm*) {}
void frame_boundary(mq_script_vm*) {}
int register_num(mq_script_vm*, const char*, mq_script_num_fn, void*) { return 1; }
int register_str(mq_script_vm*, const char*, mq_script_str_fn, void*) { return 1; }
int register_ret_str(mq_script_vm*, const char*, mq_script_ret_str_fn, void*) { return 1; }
int register_mixed(mq_script_vm*, const char*, mq_script_mixed_fn, void*) { return 1; }
int register_multi(mq_script_vm*, const char*, const char*, mq_script_multi_fn, void*) { return 1; }
int register_multi_ret_str(mq_script_vm*, const char*, const char*, mq_script_multi_ret_str_fn, void*) { return 1; }
int register_bytes(mq_script_vm*, const char*, const char*, mq_script_bytes_fn, void*) { return 1; }
} // namespace maque::script::native

#endif
