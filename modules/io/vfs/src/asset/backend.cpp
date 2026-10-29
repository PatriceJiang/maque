/* asset:// backend: Android APK assets via SDL2 RWops.
 * SDLActivity installs Android's AssetManager before SDL_main runs, and SDL_RWFromFile
 * transparently reads both compressed and uncompressed entries from the APK. */
#include "../vfs_internal.h"

#if defined(__ANDROID__)
#include <SDL.h>
#endif

namespace maque::vfs {

int asset_read(const std::string& dir, const std::string& rel, void** out, size_t* out_size) {
#if defined(__ANDROID__)
    std::string path;
    if (!dir.empty()) {
        path = dir;
        while (!path.empty() && path.back() == '/') path.pop_back();
        if (!rel.empty()) path += "/";
    }
    path += rel;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.empty()) return MQ_VFS_ERR_ARG;

    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw) return MQ_VFS_ERR_NOTFOUND;
    const Sint64 length = SDL_RWsize(rw);
    if (length < 0) {
        SDL_RWclose(rw);
        return MQ_VFS_ERR_IO;
    }

    Blob data((size_t)length);
    const size_t read = data.empty() ? 0 : SDL_RWread(rw, data.data(), 1, data.size());
    SDL_RWclose(rw);
    if (read != data.size()) return MQ_VFS_ERR_IO;
    return deliver(data, out, out_size);
#else
    (void)dir;
    (void)rel;
    (void)out;
    (void)out_size;
    return MQ_VFS_ERR_UNSUPPORTED;
#endif
}

} // namespace maque::vfs
