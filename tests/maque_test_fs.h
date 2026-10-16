/* 单测临时目录与文件助手（跨平台、**失败可见而非崩溃**）。
 *
 * 为什么需要（实测缺陷）：
 *   多个单测直接用 `fs::temp_directory_path() / "xxx"` 再调 `fs::create_directories()`
 *   与 `std::ofstream`。这些**抛异常**的重载在目录不可写时抛出未捕获的
 *   `std::filesystem::filesystem_error` → 进程被 std::terminate / __fastfail 干掉，
 *   表现是"测试无任何输出、退出码 0xC0000409"（Windows）或 SIGABRT（Linux）——
 *   完全看不出真实原因。实测本机 `%TEMP%` 不可写（Access is denied），
 *   于是 vfs/asset/watcher 三个单测齐齐硬崩。
 *
 * 本头的策略：
 *   ① 优先用 `%TEMP%/maque-test-<name>`；**探测可写**，不可写则回退到
 *      `MAQUE_TEST_TMPDIR`（环境变量，可为相对路径）→ `./maque-test-tmp/<name>`（CWD 下，CTest 的
 *      WORKING_DIRECTORY 就在构建树内，通常可写）；
 *   ② 提供**不抛异常**的 put/read/remove 助手（用 error_code 重载），失败返回 false；
 *   ③ `maque_test_setup_dir()` 返回失败时测试自己报错退出，而不是崩。
 *
 * 用法：
 *   #include "maque_test_fs.h"
 *   fs::path root;
 *   if (!maque_test_setup_dir("vfs", root)) { std::print(stderr, "vfs_test: 无法创建临时目录\n"); return 77; }
 */
#ifndef MAQUE_TEST_FS_H
#define MAQUE_TEST_FS_H

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

namespace fs = std::filesystem;

/* 目录是否可写（真正建一个文件再删）。 */
inline bool maque_dir_writable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    const fs::path probe = dir / ".maque_write_probe";
    { std::ofstream f(probe, std::ios::binary); if (!f.is_open()) return false; f << "x"; }
    std::error_code e2;
    fs::remove(probe, e2);
    return true;
}

/* 建出一个可写的测试根目录（清理旧内容，但不抛异常）。 */
inline bool maque_test_setup_dir(const char* name, fs::path& out) {
    std::error_code ec;

    fs::path base;
    if (const char* env = std::getenv("MAQUE_TEST_TMPDIR"); env && *env) {
        base = env;
    } else {
        fs::path tmp = fs::temp_directory_path(ec);
        if (!ec) {
            const fs::path cand = tmp / (std::string("maque-test-") + name);
            if (maque_dir_writable(cand)) base = cand;
        }
        if (base.empty()) base = fs::path("maque-test-tmp") / name;   // 回退：CWD 下（构建树内）
    }

    if (!maque_dir_writable(base)) {
        std::print(stderr, "maque_test_fs: 临时目录不可写 '{}'\n", base.string());
        return false;
    }
    fs::remove_all(base, ec);          // 清掉上次残留（失败也无妨）
    fs::create_directories(base, ec);
    if (ec) { std::print(stderr, "maque_test_fs: create_directories 失败 '{}': {}\n", base.string(), ec.message()); return false; }
    out = base;
    return true;
}

/* 写文件（自动建父目录）。失败返回 false，不抛异常。 */
inline bool maque_put_file(const fs::path& p, const std::string& s) {
    std::error_code ec;
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    return f.good();
}

/* 读文件；失败返回 fallback（默认 "<err>"，与既有测试的约定一致）。 */
inline std::string maque_read_file(const fs::path& p, const char* fallback = "<err>") {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return fallback;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

/* 递归删除（不抛异常）。 */
inline void maque_remove_all(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

#endif // MAQUE_TEST_FS_H
