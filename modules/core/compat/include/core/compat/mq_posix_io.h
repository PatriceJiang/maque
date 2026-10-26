/* 跨平台低层文件 I/O（POSIX `open/write/pwrite/close` ↔ Windows `<io.h>`）。
 *
 * 为什么需要：崩溃路径（docs/09 §5）要在**信号处理器内**把预渲染的报告写盘，
 * 所以走的是不带缓冲的文件描述符接口而不是 iostream。POSIX 的这套接口 MSVC CRT
 * 没有同名函数（对应 `_open/_write/_lseeki64/_close`），故在此做一层薄封装。
 *
 * 语义差异（已按 POSIX 语义对齐）：
 *   - `open` 的权限位（0644）在 Windows 只影响只读位，`_S_IREAD|_S_IWRITE` 足够；
 *   - `pwrite(fd,buf,n,off)` 用 `_lseeki64` + `_write` 组合实现（Windows 无 pwrite）；
 *   - 失败返回 -1，与 POSIX 一致；`write_fd` 返回实际写入字节数。
 *
 * 只做头文件内联，不引入新库、不改任何 C ABI。
 */
#ifndef MAQUE_CORE_COMPAT_MQ_POSIX_IO_H
#define MAQUE_CORE_COMPAT_MQ_POSIX_IO_H

#include <cstddef>
#include <cstdio>
#include <cstdint>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace maque { namespace compat {

/* ssize_t 在 MSVC 里不存在；统一给一个（崩溃路径需要它承载 write 的返回值）。 */
#if defined(_WIN32)
using ssize_t = long long;
inline constexpr int kOpenReadOnly      = _O_RDONLY | _O_BINARY;
inline constexpr int kOpenWriteCreate   = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
#else
using ::ssize_t;                  /* POSIX 已有；引入本命名空间，使调用代码跨平台同一份 */
inline constexpr int kOpenReadOnly      = O_RDONLY;
inline constexpr int kOpenWriteCreate   = O_WRONLY | O_CREAT | O_TRUNC;
#endif

inline int open_fd(const char* path, int flags) {
    if (!path) return -1;
#if defined(_WIN32)
    return _open(path, flags, _S_IREAD | _S_IWRITE);
#else
    return open(path, flags, 0644);
#endif
}

inline ssize_t write_fd(int fd, const void* buf, size_t n) {
    if (fd < 0 || !buf) return -1;
#if defined(_WIN32)
    return static_cast<ssize_t>(_write(fd, buf, static_cast<unsigned>(n)));
#else
    return ::write(fd, buf, n);
#endif
}

/* 定位写：崩溃报告的"总是从头覆盖"语义（tmp+rename 之外的第二道保险）。 */
inline ssize_t pwrite_fd(int fd, const void* buf, size_t n, long long offset) {
    if (fd < 0 || !buf) return -1;
#if defined(_WIN32)
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return static_cast<ssize_t>(_write(fd, buf, static_cast<unsigned>(n)));
#else
    return ::pwrite(fd, buf, n, static_cast<off_t>(offset));
#endif
}

inline int close_fd(int fd) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    return _close(fd);
#else
    return ::close(fd);
#endif
}

} // namespace compat
} // namespace maque

#endif // MAQUE_CORE_COMPAT_MQ_POSIX_IO_H
