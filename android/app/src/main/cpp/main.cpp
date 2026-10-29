#include <SDL.h>
#include <SDL_system.h>

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
