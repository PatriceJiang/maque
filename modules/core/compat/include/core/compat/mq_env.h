/* 跨平台环境变量读写（极小兼容层）。
 *
 * 为什么需要：`setenv/unsetenv` 是 POSIX，MSVC 的 CRT 不提供；Windows 对应的是
 * `_putenv_s(name, value)`（传空串 = 删除该变量）。引擎里读环境变量的地方很多
 * （MAQUE_* 一堆开关），写的地方只有开关覆盖与测试置位，所以只做这三个函数。
 *
 * 约定：
 *   - 名字/值都是 UTF-8 C 字符串；Windows 下 CRT 用 ANSI 代码页，非 ASCII 值可能不保真，
 *     但本工程用它传的都是 ASCII 路径/开关。
 *   - 失败（非法入参）静默忽略：环境变量是「尽力而为」的配置通道，不该让调用点分叉错误分支。
 *   - 只实现为头文件内联，不引入新库、不改任何 C ABI。
 */
#ifndef MAQUE_CORE_COMPAT_MQ_ENV_H
#define MAQUE_CORE_COMPAT_MQ_ENV_H

#include <cstdlib>

/* 头文件内内联实现，不引入新库、不改任何 C ABI；命名空间避开全局符号污染。 */
namespace maque { namespace compat {

/* 设置环境变量（覆盖已有值）。 */
inline void set_env(const char* name, const char* value) {
    if (!name) return;
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    setenv(name, value ? value : "", 1);
#endif
}

/* 删除环境变量。 */
inline void unset_env(const char* name) {
    if (!name) return;
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* 读取环境变量；不存在返回 nullptr。 */
inline const char* get_env(const char* name) {
    if (!name) return nullptr;
    return std::getenv(name);
}

} // namespace compat
} // namespace maque

#endif // MAQUE_CORE_COMPAT_MQ_ENV_H
