#include <SDL.h>
#include <SDL_system.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <sys/system_properties.h>

#include "core/compat/mq_env.h"
#include "runtime/mq_runtime.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

namespace {

/* ImGui（F2/F4 面板）用 stb_truetype 自己解析字体，不能直接读 APK asset；
 * Android 系统自带的 CJK 字体是 CFF/TTC（stb 不支持），所以把仓库里的
 * fonts/*.ttf（Gradle 把 scenes/ 作为 assets 根）复制到内部存储，并设置 MAQUE_FONT 指向它。
 * 不复制的话 F2/F4 首开会因字体加载失败触发 ImGui 断言。 */
void ensure_android_font() {
    if (std::getenv("MAQUE_FONT")) return;
    const char* internal = SDL_AndroidGetInternalStoragePath();
    if (!internal || !*internal) return;
    const std::string dir = std::string(internal) + "/fonts";
    const std::string dest = dir + "/AlibabaPuHuiTi-3-55-Regular.ttf";
    std::error_code ec;
    if (std::filesystem::is_regular_file(dest, ec) &&
        std::filesystem::file_size(dest, ec) > 4096) {
        maque::compat::set_env("MAQUE_FONT", dest.c_str());
        return;
    }

    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID get_assets = cls
        ? env->GetMethodID(cls, "getAssets", "()Landroid/content/res/AssetManager;") : nullptr;
    jobject assets = get_assets ? env->CallObjectMethod(activity, get_assets) : nullptr;
    AAssetManager* mgr = assets ? AAssetManager_fromJava(env, assets) : nullptr;
    AAsset* asset = mgr
        ? AAssetManager_open(mgr, "fonts/AlibabaPuHuiTi-3-55-Regular.ttf",
                             AASSET_MODE_BUFFER) : nullptr;

    if (asset) {
        const int64_t len = AAsset_getLength(asset);
        const void* data = AAsset_getBuffer(asset);
        if (data && len > 0) {
            std::filesystem::create_directories(dir, ec);
            std::ofstream out(dest, std::ios::binary);
            out.write(static_cast<const char*>(data), (std::streamsize)len);
            out.close();
            if (out) {
                maque::compat::set_env("MAQUE_FONT", dest.c_str());
                SDL_Log("maque_android: font -> %s (%lld bytes)", dest.c_str(),
                        (long long)len);
            } else {
                SDL_Log("maque_android: font copy failed: %s", dest.c_str());
            }
        }
        AAsset_close(asset);
    }
    if (assets) env->DeleteLocalRef(assets);
    if (cls) env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

void configure_mobile_defaults() {
    ensure_android_font();
    if (!std::getenv("MAQUE_STORAGE")) {
        const char* internal = SDL_AndroidGetInternalStoragePath();
        if (internal && *internal) {
            const std::string storage = std::string("file://") + internal + "/maque";
            maque::compat::set_env("MAQUE_STORAGE", storage.c_str());
        }
    }
    if (!std::getenv("MAQUE_AUDIO")) maque::compat::set_env("MAQUE_AUDIO", "auto");
    if (!std::getenv("MAQUE_IMGUI_INI")) {
        const char* internal = SDL_AndroidGetInternalStoragePath();
        if (internal && *internal) {
            const std::string ini = std::string(internal) + "/imgui.ini";
            maque::compat::set_env("MAQUE_IMGUI_INI", ini.c_str());
        }
    }
    if (!std::getenv("MAQUE_ENGINE_OPTIONS_FILE")) {
        const char* internal = SDL_AndroidGetInternalStoragePath();
        if (internal && *internal) {
            const std::string opt = std::string(internal) + "/engine_options.ini";
            maque::compat::set_env("MAQUE_ENGINE_OPTIONS_FILE", opt.c_str());
        }
    }
    char present_prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.maque.present", present_prop) > 0 && present_prop[0]) {
        maque::compat::set_env("MAQUE_PRESENT", present_prop);
        SDL_Log("maque_android: present=%s (debug.maque.present)", present_prop);
    }
    char qemu[PROP_VALUE_MAX] = {};
    const bool emulator = __system_property_get("ro.kernel.qemu", qemu) > 0 && qemu[0] == '1';
    const bool force_present = present_prop[0] != '\0';
    if (emulator && !force_present) {
        maque::compat::set_env("MAQUE_PRESENT", "accelerated");
        maque::compat::set_env("MAQUE_ANDROID_PRESENT_EVERY", "4");
        SDL_Log("maque_android: present=ANativeWindow CPU fallback (emulator)");
    } else if (!std::getenv("MAQUE_PRESENT")) {
        /* 真机默认 wgpu 直呈：不把整帧从 GPU 回读到 CPU，RenderFactor/UI 才真正省时间。
         * 直呈 attach/configure 失败时 setup 会回退离屏 CPU 路径；可用
         * `setprop debug.maque.present software` 临时强制 CPU。 */
        maque::compat::set_env("MAQUE_PRESENT", "wgpu");
        SDL_Log("maque_android: present=wgpu direct (default)");
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

/* Android 触摸 JNI：MaqueActivity 用 renderSurface 的归一化坐标直接入队，
 * 不经过 SDL 的 Java Touch 转换（旋转/多窗口下它的本地坐标会错）。 */
extern "C" void mq_platform_android_touch(int id, int action, float nx, float ny);

extern "C" JNIEXPORT void JNICALL
Java_org_maque_player_MaqueActivity_nativeTouch(JNIEnv*, jclass, jint id, jint action,
                                                jfloat nx, jfloat ny) {
    mq_platform_android_touch((int)id, (int)action, (float)nx, (float)ny);
}
