/* 模块宿主（docs/02）：动态库加载 + 版本化 vtable 查询（C ABI，extern "C" 边界不变）。
 *
 * 平台差异（模块后缀）：
 *   · Windows：模块是 **.dll**；LoadLibraryA(path) 在 path 不带扩展名时会由系统自动补
 *     ".dll"（Windows 既定行为，本文件不额外拼名）。
 *   · Linux：模块是 **.so**；dlopen(path) **不会**补扩展名 —— 调用方必须给完整文件名
 *     （如 "libmaque_gfx.so"）。
 *   两侧都按调用方给出的显式路径加载、不做平台兜底，语义等价。
 * 符号：两侧同名（MQ_MODULE_QUERY_SYMBOL = "mq_module_query"）；Windows 的 GetProcAddress
 *   大小写不敏感、Linux 的 dlsym 大小写敏感 —— 由模块侧的导出宏（MQ_MODULE_EXPORT）保证一致。 */
#include "core/module/mq_module.h"

#include <vector>
#include <cstring>

#if defined(_WIN32)
/* windows.h 会引入 min/max、ERROR 等宏并拖进 winsock1/ole 等无关头：
 * 用 WIN32_LEAN_AND_MEAN + NOMINMAX 收窄，且**只在本 .cpp 内**包含，不泄漏到任何头文件。 */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>              /* LoadLibraryA / GetProcAddress / FreeLibrary */
#else
#  include <dlfcn.h>                /* dlopen / dlsym / dlclose */
#endif

struct mq_loaded_module {
    void*              handle = nullptr;   /* Windows: HMODULE；Linux: dlopen 句柄 */
    mq_module_query_fn query = nullptr;
};

struct mq_host {
    std::vector<mq_loaded_module*> modules;
};

namespace {

/* 动态库句柄统一以 void* 保存（Windows 侧即 HMODULE）：三个薄封装让调用点平台无关 */
void* platform_open(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::LoadLibraryA(path));
#else
    return ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

mq_module_query_fn platform_query(void* handle, const char* symbol) {
#if defined(_WIN32)
    return reinterpret_cast<mq_module_query_fn>(
        ::GetProcAddress(static_cast<HMODULE>(handle), symbol));
#else
    return reinterpret_cast<mq_module_query_fn>(::dlsym(handle, symbol));
#endif
}

/* 卸载模块。
 *
 * ── 为什么 Windows 上默认**不**调用 FreeLibrary（重要，别"顺手修好"）──────────────
 * 实测（build-win，确定性复现，非竞态）：渲染插件 maque_gfx.dll 在引擎 teardown 里
 * `rapi->destroy()` 之后被 FreeLibrary，进程必然以 0xC0000005 崩在
 *    Faulting module: wgpu_native.dll_unloaded   Fault offset: 0xa79f0
 * 即 wgpu-native 自己的 DLL 退出路径踩到已卸载的内存（该 DLL 注册了 _onexit 处理器，
 * 并在内部起过线程）。Linux 的 dlclose 对这类库通常也不真正卸载（有线程/TLS 的库
 * 会被保留），所以两平台的实际行为反而一致。
 *
 * 语义影响（保持不漂移）：
 *   · `mq_host_unload` 仍然把模块移出列表 —— `mq_host_module_count` 减少、
 *     `mq_host_query` 不再命中（host_test 依赖这两点，且两平台一致）；
 *   · 只是"地址空间里的 .dll 映射"留到进程退出由 OS 回收（进程级资源，无泄漏风险）。
 * 需要强制卸载的场景（如测试 DLL 生命周期）用环境变量
 *   MAQUE_MODULE_FORCE_UNLOAD=1
 * 打开，并自行承担上述崩溃风险。 */
void platform_close(void* handle) {
#if defined(_WIN32)
    const char* force = std::getenv("MAQUE_MODULE_FORCE_UNLOAD");
    if (force && *force && *force != '0') {
        ::FreeLibrary(static_cast<HMODULE>(handle));
    } else {
        (void)handle;   /* 故意不 FreeLibrary：见上方说明 */
    }
#else
    ::dlclose(handle);
#endif
}

} // namespace

extern "C" {

mq_host* mq_host_create(void) { return new mq_host(); }

void mq_host_destroy(mq_host* host) {
    if (!host) return;
    for (auto* m : host->modules) {
        if (m->handle) platform_close(m->handle);
        delete m;
    }
    delete host;
}

int mq_host_load(mq_host* host, const char* path, mq_loaded_module** out_module) {
    if (out_module) *out_module = nullptr;
    if (!host || !path) return 1;
    void* handle = platform_open(path);
    if (!handle) return 2;
    mq_module_query_fn query = platform_query(handle, MQ_MODULE_QUERY_SYMBOL);
    if (!query) { platform_close(handle); return 3; }
    auto* m = new mq_loaded_module();
    m->handle = handle;
    m->query = query;
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
    if (module->handle) platform_close(module->handle);
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
