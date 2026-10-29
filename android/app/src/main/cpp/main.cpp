#include <SDL.h>
#include <SDL_system.h>
#include <sys/system_properties.h>

#include "core/compat/mq_env.h"
#include "runtime/mq_runtime.h"

#include <cstdlib>
#include <print>
#include <string>

namespace {

void configure_mobile_defaults() {
    if (!std::getenv("MAQUE_STORAGE")) {
        const char* internal = SDL_AndroidGetInternalStoragePath();
        if (internal && *internal) {
            const std::string storage = std::string("file://") + internal + "/maque";
            maque::compat::set_env("MAQUE_STORAGE", storage.c_str());
        }
    }
    if (!std::getenv("MAQUE_AUDIO")) maque::compat::set_env("MAQUE_AUDIO", "auto");
    char qemu[PROP_VALUE_MAX] = {};
    const bool emulator = __system_property_get("ro.kernel.qemu", qemu) > 0 && qemu[0] == '1';
    if (emulator) {
        maque::compat::set_env("MAQUE_PRESENT", "accelerated");
        maque::compat::set_env("MAQUE_ANDROID_PRESENT_EVERY", "4");
        SDL_Log("maque_android: present=ANativeWindow CPU fallback (emulator)");
    } else if (!std::getenv("MAQUE_PRESENT")) {
        maque::compat::set_env("MAQUE_PRESENT", "wgpu");
        SDL_Log("maque_android: present=wgpu GPU blit");
    }
}

} // namespace

int main(int argc, char** argv) {
    configure_mobile_defaults();
    if (argc < 2 || !argv[1] || !*argv[1]) {
        std::print(stderr, "maque_android: missing asset:// scene argument\n");
        return 2;
    }
    SDL_Log("maque_android: run %s", argv[1]);
    const int rc = mq_run_scene(argv[1]);
    SDL_Log("maque_android: scene finished rc=%d", rc);
    return rc;
}
