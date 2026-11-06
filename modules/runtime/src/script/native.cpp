/* 脚本宿主 · C/C++ native 脚本的装配与路径解析（plans/26-cpp-scripts.md）。
 *
 * 这里只做两件事：
 *   1) make_script_vm：统一创建/绑定 VM；native 额外注入 API 表 + Engine 上下文 + resolver。
 *   2) native_resolve：把场景里的源路径（如 /scripts/ball.cpp）解析成
 *      so:<绝对路径> / soname:<库名> / static:<key> 规格，并执行信任门。
 *
 * manifest 是**派生物**（native/<target>/manifest.json），不是手写真相：
 *   { "api": 1, "target": "linux-x86_64",
 *     "scripts": { "scripts/ball.cpp": { "artifact": "...", "kind": "shared" } } }
 * 也接受 "scripts/ball.cpp": "path/to/lib.so" 的简写。
 */
#include "../runtime_engine.h"
#include "maque_guest_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

namespace maque::rt {

namespace {

bool remote_scene_path(const std::string& p) {
    return p.rfind("http://", 0) == 0 || p.rfind("https://", 0) == 0;
}

std::string strip_leading_slash(std::string s) {
    size_t i = 0;
    while (i < s.size() && s[i] == '/') ++i;
    return s.substr(i);
}

const char* native_mode() {
    const char* m = std::getenv("MAQUE_NATIVE");
    return (m && *m) ? m : "local";
}

} // namespace

mq_script_vm* Engine::make_script_vm(const char* lang, const std::string& source_path) {
    (void)source_path;
    mq_script_vm* vm = mq_script_vm_create_lang(lang);
    if (!vm) return nullptr;
    if (std::strcmp(lang, "native") == 0) {
        mq_script_vm_native_config cfg{};
        cfg.struct_size = sizeof cfg;
        cfg.api = mq_script_api_v1_get();
        cfg.host_ctx = this;
        cfg.resolver = [](void* user, const char* source, char* out_spec, int cap) -> int {
            auto* e = static_cast<Engine*>(user);
            return e ? e->native_resolve(source ? source : "", out_spec, cap) : 1;
        };
        cfg.resolver_user = this;
        if (mq_script_vm_configure_native(vm, &cfg) != 0) {
            std::print(stderr, "runtime: native VM configure failed (API table unavailable?)\n");
            mq_script_vm_destroy(vm);
            return nullptr;
        }
    }
    mq_script_set_rng(vm, [](void* u) -> uint32_t { return static_cast<Engine*>(u)->next_rng(); }, this);
    mq_bind_generated(vm, this);   // native 的 register_* 是 no-op
    return vm;
}

int Engine::native_resolve(const std::string& source_path, char* out, int cap) {
    if (!out || cap <= 0) return 1;
    out[0] = '\0';

    const char* mode = native_mode();
    if (std::strcmp(mode, "0") == 0 || std::strcmp(mode, "off") == 0) {
        std::print(stderr, "runtime: native scripts disabled (MAQUE_NATIVE={})\n", mode);
        return 1;
    }
    const bool remote = remote_scene_path(scn_full_path);
    if (remote && std::strcmp(mode, "any") != 0) {
        std::print(stderr,
                   "runtime: refusing native script '{}' from a remote scene "
                   "(MAQUE_NATIVE=local; use wasm/static for untrusted content)\n",
                   source_path);
        return 1;
    }
    if (remote) {
        std::print(stderr,
                   "runtime: WARNING: loading native script '{}' from a remote scene "
                   "(MAQUE_NATIVE=any; native code is not sandboxed)\n",
                   source_path);
    }

    fs::path manifest;
    if (const char* env = std::getenv("MAQUE_NATIVE_MANIFEST"); env && *env) {
        manifest = fs::path(env);
    } else if (!module_pkg.root_dir.empty()) {
        manifest = fs::path(module_pkg.root_dir) / "native" / MQ_TARGET_KEY / "manifest.json";
        std::error_code ec;
        if (!fs::exists(manifest, ec) && !scn_dir.empty())
            manifest = fs::path(scn_dir) / "native" / MQ_TARGET_KEY / "manifest.json";
    } else if (!scn_dir.empty()) {
        manifest = fs::path(scn_dir) / "native" / MQ_TARGET_KEY / "manifest.json";
    }

    std::error_code ec;
    if (manifest.empty() || !fs::exists(manifest, ec)) {
        std::print(stderr,
                   "runtime: native script '{}' has no manifest for target '{}' "
                   "(expected native/{}/manifest.json or MAQUE_NATIVE_MANIFEST)\n",
                   source_path, MQ_TARGET_KEY, MQ_TARGET_KEY);
        return 1;
    }
    manifest = fs::absolute(manifest, ec);
    if (ec) {
        std::print(stderr, "runtime: native manifest path error: {}\n", ec.message());
        return 1;
    }

    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        std::print(stderr, "runtime: cannot read native manifest '{}'\n", manifest.string());
        return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    mq_json* dom = mq_json_parse(text.c_str(), text.size());
    if (!dom) {
        std::print(stderr, "runtime: malformed native manifest '{}'\n", manifest.string());
        return 1;
    }

    auto fail = [&](const std::string& msg) -> int {
        mq_json_free(dom);
        std::print(stderr, "runtime: native manifest '{}': {}\n", manifest.string(), msg);
        return 1;
    };

    const char* target = mq_json_str(mq_json_get(dom, "target"), "");
    if (*target && std::strcmp(target, MQ_TARGET_KEY) != 0)
        return fail(std::string("target mismatch: manifest='") + target + "', engine='" + MQ_TARGET_KEY + "'");
    const double api = mq_json_num(mq_json_get(dom, "api"), (double)MQ_SCRIPT_API_VERSION);
    if ((int)api != (int)MQ_SCRIPT_API_VERSION)
        return fail("API version mismatch (rebuild the guest against this engine)");

    const mq_json* scripts = mq_json_get(dom, "scripts");
    if (!scripts) return fail("missing 'scripts' object");

    const std::string rel = strip_leading_slash(source_path);
    if (rel.empty()) return fail("empty script path");

    std::string key = rel;
    const mq_json* entry = mq_json_get(scripts, key.c_str());
    if (!entry && module_pkg.active && !module_pkg.scene_rel.empty()) {
        key = module_pkg.scene_rel + "/" + rel;
        entry = mq_json_get(scripts, key.c_str());
    }
    if (!entry) return fail("no entry for '" + rel + "'");

    std::string artifact = mq_json_str(mq_json_get(entry, "artifact"), "");
    if (artifact.empty()) artifact = mq_json_str(entry, "");   // 简写：key -> "path"
    const char* kind = mq_json_str(mq_json_get(entry, "kind"), "shared");
    if (artifact.empty() && std::strcmp(kind, "static") != 0)
        return fail("entry '" + key + "' has no artifact");

    std::string spec;
    if (std::strcmp(kind, "static") == 0) {
        const char* k = mq_json_str(mq_json_get(entry, "key"), key.c_str());
        spec = std::string("static:") + k;
    } else if (std::strcmp(kind, "soname") == 0) {
        spec = std::string("soname:") + artifact;
    } else {
        fs::path p(artifact);
        if (p.is_relative()) p = manifest.parent_path() / p;
        p = p.lexically_normal();
        spec = "so:" + p.string();
    }

    mq_json_free(dom);
    if ((int)spec.size() >= cap) {
        std::print(stderr, "runtime: native spec too long for '{}'\n", source_path);
        return 1;
    }
    std::snprintf(out, (size_t)cap, "%s", spec.c_str());
    if (const char* info = std::getenv("MAQUE_NATIVE_INFO"); info && *info && *info != '0')
        std::print("runtime: native '{}' -> {}\n", rel, spec);
    return 0;
}

} // namespace maque::rt
