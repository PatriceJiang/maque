# Windows 第三方依赖构建说明

> 入库版本：`.deps/` 被 .gitignore 忽略，故这份说明随仓库提交；
> 实际的源码/构建/安装产物在 `.deps/`，由 `tools/win/fetch-deps.ps1` 重建（幂等）。

---

# maque Windows 第三方原生依赖 —— 构建说明与自检证据

> 由 `tools/win/fetch-deps.ps1` 一条命令产生。依赖不入库（`.gitignore` 忽略 `/.deps/`）。
> 本文件是**交付证据**：每个库的版本、配置选项、产物绝对路径，以及「编译 + 链接 + 运行」
> 的自检命令与输出。

---

## 0. 成功 / 失败清单

| 库 | 版本 | 状态 | 产物（`.deps/dist/`） |
|---|---|---|---|
| SDL2 | 2.32.10 | ✅ 成功（静态 + 动态都构建） | `lib/SDL2-static-full.lib`（使用）· `lib/SDL2.lib` · `lib/SDL2main.lib` · `bin/SDL2.dll` |
| zlib-ng | 2.2.4（`ZLIB_COMPAT=ON`） | ✅ 成功 | `lib/zlibstatic.lib` + `include/zlib.h` |
| zstd | 1.5.7 | ✅ 成功 | `lib/zstd.lib`（=`zstd_static.lib` 副本）+ `include/zstd.h` |
| freetype | 2.13.3（VER-2-13-3） | ✅ 成功 | `lib/freetype.lib` + `include/freetype2/` |
| curl（libcurl） | 8.23.0-DEV（tag `curl-8_23_0`） | ✅ 成功（静态 + Schannel） | `lib/libcurl.lib` + `include/curl/` |
| libwebsockets | 4.4.5 | ✅ 成功（静态，**TLS 关闭**，见 §6） | `lib/websockets_static.lib` + `include/libwebsockets.h` |

**6/6 全部成功**，无失败项。`.deps/dist` 合计约 28.6 MB。
`/.deps/selfcheck` 的 6 个独立自检程序 **全部通过**（CMake 路径 6/6；raw clang-cl 6/6）。

---

## 1. 工具链与统一约定

| 项 | 值 |
|---|---|
| 编译器 | `C:\Program Files\LLVM\bin\clang-cl.exe` —— clang version **20.1.8**，Target `x86_64-pc-windows-msvc` |
| CMake | **4.0.3** |
| Ninja | `C:\Users\ADMIN\miniconda3\envs\Py311\Scripts\ninja.exe`（1.13.0.git.kitware.jobserver-pipe-1） |
| 下载 | `curl.exe` 8.22.0（Schannel） |
| 生成器 / 构建类型 | `Ninja` / `Release` |
| 运行时库 | `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`（**/MD**，与主工程一致） |
| 安装前缀 | `E:\Github\maque\.deps\dist`（`include/` `lib/` `bin/`） |

### ⚠️ 铁律 1：不要加载 `vcvars64.bat`

加载后 PATH 会先命中 VS 自带的 clang-cl **19.1.5**（实测：`.deps/probe/mini` 里 CMake 选中的
就是 `VC/Tools/Llvm/x64/bin/clang-cl.exe`），而且 CMake 的 *Detecting CXX compiler ABI info*
会**永久挂起**。正确做法：把 clang-cl 的**绝对路径**交给 `CMAKE_C_COMPILER/CXX_COMPILER`，
CMake 自己定位 MSVC 头与 Windows SDK。本机已验证可行，raw clang-cl 亦可在**没有**
`INCLUDE`/`LIB` 环境变量时自行定位工具链（见 §5 的裸命令）。

### ⚠️ 铁律 2：`github.com` 不可达

`git clone` / `github.com` 443 超时。可用的是 `codeload.github.com`（源码 tarball）、
`api.github.com`（查版本）。下载一律：

```
https://codeload.github.com/<owner>/<repo>/tar.gz/refs/tags/<tag>
```

解压用 Windows 自带 `tar.exe`（bsdtar）。tarball 里的**符号链接**会报
`Can't create '...': Invalid argument` —— 只出现在 libwebsockets 的 embedded 示例和 zstd 的
测试脚本里，**与构建无关**，脚本已过滤该噪声。

### ⚠️ 铁律 3：`.ps1` 必须带 UTF-8 BOM

PowerShell 5.1 对**无 BOM** 的 `.ps1` 按系统 ANSI 代码页解码，中文注释会乱码乃至**语法错误**
（本项目已实测：`FATAL: 找不到 ninja` 一行的中文尾部把引号吃掉，导致 `'<' 运算符保留` 报错）。
`tools/win/fetch-deps.ps1` 已写为 UTF-8 **with BOM**；后续编辑请保持。

执行策略默认禁止运行脚本文件，所以必须：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1
```

---

## 2. 一条命令的复现方式

```powershell
# 全量（幂等：已安装的库自动跳过）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1

# 只做某几个库 / 全部重建 / 跳过自检
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1 -Only sdl2,curl
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1 -Force
powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1 -SkipSelfCheck
```

脚本做六件事：**下载 → 解压 → （必要的可移植性补丁）→ 构建 → 安装 → 后处理 → 生成
`.deps/deps.cmake` → 构建并运行 6 个独立自检**。日志在 `.deps/logs/<lib>.log`。

主工程侧（根 `CMakeLists.txt` 已接线，无需改任何模块）：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMAQUE_WINDOWS_DEPS=ON `
      -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
      -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe"
```

---

## 3. 产物清单（绝对路径）

```
E:\Github\maque\.deps\dist\
├─ bin\
│   ├─ SDL2.dll                        1,659,904   （动态变体，实际未使用：我们用静态）
│   └─ curl-config
├─ include\
│   ├─ SDL2\SDL.h …                                （SDL2 2.32.10）
│   ├─ zlib.h  zconf.h  zlib_name_mangling.h       （zlib-ng 2.2.4 compat）
│   ├─ zstd.h  zdict.h  zstd_errors.h              （zstd 1.5.7）
│   ├─ freetype2\ft2build.h + freetype\**          （freetype 2.13.3）
│   ├─ curl\curl.h …                               （libcurl 8.23.0-DEV）
│   └─ libwebsockets.h  lws_config.h  libwebsockets\**  （lws 4.4.5）
└─ lib\
    ├─ SDL2-static-full.lib   7,920,238   ← SDL2-static.lib + SDL2main.lib 合并（deps.cmake 使用）
    ├─ SDL2-static.lib        7,916,820
    ├─ SDL2main.lib               3,750
    ├─ SDL2.lib                 183,464   （SDL2.dll 的导入库，未使用）
    ├─ zlibstatic.lib           198,090
    ├─ zstd.lib               1,000,098   ← zstd_static.lib 的副本
    ├─ zstd_static.lib        1,000,098
    ├─ freetype.lib           1,336,738
    ├─ libcurl.lib            2,510,588
    └─ websockets_static.lib  1,293,120
```

---

## 4. `.deps/deps.cmake` 设置的变量

函数：`maque_find_windows_deps()`（根 `CMakeLists.txt` 已 `include()` + 调用）。
除下表外还做三件事：① 把 `dist` 前置进 `CMAKE_PREFIX_PATH`；② `include_directories(<dist>/include)`
（让 `#include <zstd.h>` / `<curl/curl.h>` 这类裸 include 在 Windows 上也能命中）；
③ 预建 `CURL::libcurl` 与顶层 `link_libraries()`（理由见 §5 的坑 1/2）。

| 变量 | 值 |
|---|---|
| `SDL2_INCLUDE_DIR` | `E:/Github/maque/.deps/dist/include/SDL2` |
| `SDL2_LIBRARY` | `E:/Github/maque/.deps/dist/lib/SDL2-static-full.lib` |
| `SDL2_DLL` / `SDL2_SHARED_LIBRARY` | `…/dist/bin/SDL2.dll` / `…/dist/lib/SDL2.lib`（未使用，备查） |
| `ZLIB_INCLUDE_DIR` | `E:/Github/maque/.deps/dist/include` |
| `ZLIB_LIBRARY` / `ZLIB_LIBRARY_RELEASE` | `E:/Github/maque/.deps/dist/lib/zlibstatic.lib` |
| `ZSTD_LIBRARY` | `E:/Github/maque/.deps/dist/lib/zstd.lib` |
| `ZSTD_INCLUDE_DIR` | `E:/Github/maque/.deps/dist/include` |
| `FREETYPE_LIBRARY` | `E:/Github/maque/.deps/dist/lib/freetype.lib` |
| `FREETYPE_INCLUDE_DIR_ft2build` / `_freetype2` | `E:/Github/maque/.deps/dist/include/freetype2` |
| `CURL_INCLUDE_DIR` | `E:/Github/maque/.deps/dist/include` |
| `CURL_LIBRARY` | `E:/Github/maque/.deps/dist/lib/libcurl.lib` |
| `CURL_USE_STATIC_LIBS` | `ON` |
| `CURL::libcurl`（预建 imported target） | `IMPORTED_LOCATION=…/libcurl.lib`，`INTERFACE_COMPILE_DEFINITIONS=CURL_STATICLIB`，`INTERFACE_LINK_LIBRARIES=ws2_32;iphlpapi;advapi32;crypt32;secur32;bcrypt` |
| `LWS_FOUND` | `TRUE` |
| `LWS_INCLUDE_DIRS` / `LWS_INCLUDE_DIR` | `E:/Github/maque/.deps/dist/include` |
| `LWS_LIBRARY_DIRS` | `E:/Github/maque/.deps/dist/lib` |
| `LWS_LIBRARY` | `E:/Github/maque/.deps/dist/lib/websockets_static.lib` |
| `LWS_LIBRARIES` | `…/websockets_static.lib;ws2_32;userenv;psapi;iphlpapi;crypt32` |
| `LWS_VERSION` / `LWS_PREFIX` / `LWS_CFLAGS` | `4.4.5` / `…/dist` / `-I…/dist/include` |
| `MAQUE_DEPS_ROOT` / `MAQUE_DEPS_DIST` | `E:/Github/maque/.deps` / `E:/Github/maque/.deps/dist` |

`SDL2_LIBRARY`、`ZLIB_*`、`FREETYPE_*`、`CURL_*` 都写成 **cache 变量**，因此各模块里原来的
`find_path` / `find_library` / `find_package` 会直接命中（`find_*` 见到已置位的变量就不再搜索）；
`LWS_*` 是 pkg-config 风格变量（Windows 无 pkg-config，`pkg_check_modules` 不会跑）。

---

## 5. 逐库配置与踩坑记录

### 5.1 SDL2 2.32.10

```powershell
cmake -S .deps/src/SDL-release-2.32.10 -B .deps/build/sdl2 -G Ninja `
  -DCMAKE_C_COMPILER=<clang-cl> -DCMAKE_CXX_COMPILER=<clang-cl> `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL `
  -DCMAKE_INSTALL_PREFIX=E:/Github/maque/.deps/dist `
  -DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_DISABLE_INSTALL_DOCS=ON
```

* **坑：静态 SDL2 需要 `SDL2main` 垫片。** Windows 上 `SDL_main.h` 会把应用的 `main`
  改名为 `SDL_main`（`#if defined(SDL_MAIN_NEEDED) || defined(SDL_MAIN_AVAILABLE)` → `#define main SDL_main`），
  由 `SDL2main` 提供真正的 `main`/`WinMain`（`src/main/windows/SDL_windows_main.c`，在 `_MSC_VER` 下
  `#define console_ansi_main main`）。而 maque 模块是
  `target_link_libraries(maque_platform PUBLIC "${SDL2_LIBRARY}")` —— **裸路径**，拿不到
  `SDL2-static` 目标的 `INTERFACE_LINK_LIBRARIES`，链接单个 `.lib` 会缺 `main`。
  → 后处理把 `SDL2-static.lib + SDL2main.lib` 合并成 **`SDL2-static-full.lib`**（`llvm-lib` 直接合并
  两个归档即可），`SDL2_LIBRARY` 指向它，一个路径就够。
* **坑：SDL2 静态库的系统库必须显式补。** 取自 `dist/lib/pkgconfig/sdl2.pc` 的 `Libs.private`：
  `kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32 setupapi shell32 dinput8`
  —— 在 `deps.cmake` 里用**顶层 `link_libraries()`** 补（`target_link_libraries(裸路径)` 无法传递）。
* **坑：自检程序的签名必须是 `main(int, char**)`。** 写成 `main(void)` 会变成另一个 C++ 重载
  （`SDL_main` 声明是 `extern "C" int SDL_main(int, char**)`），链接期表现为
  `undefined symbol: SDL_main`。**主工程的 `main` 本来就是标准签名，不受影响。**
* **坑：裸 clang-cl 链接需要 `/link /subsystem:console`。** 不加时 lld-link 在「尚未从归档拉入
  `main` 成员」时就判定入口点缺失 → `LNK1561: 必须定义入口点`。CMake 对控制台可执行文件**会**
  显式传 `/subsystem:console`，所以 CMake 路径（selfcheck 与主工程）不受影响；只有手写裸命令要注意。
* 动态变体（`SDL2.dll` + 导入库 `SDL2.lib`）也一并装好备用，但**默认不用**，因此不需要分发 DLL、
  不需要改 CTest 的 PATH。

### 5.2 zlib-ng 2.2.4（`find_package(ZLIB)` 用）

```powershell
-DBUILD_SHARED_LIBS=OFF -DZLIB_COMPAT=ON -DWITH_OPTIM=OFF
-DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF -DINSTALL_UTILS=OFF -DWITH_BENCHMARKS=OFF
```

* `ZLIB_COMPAT=ON` → 装出 `zlib.h`/`zconf.h`，`zlibVersion()` 报 `1.3.1.zlib-ng`（ABI/源码兼容 zlib 1.3.1）。
* `WITH_OPTIM=OFF`：避免 x86 SIMD 汇编对 nasm/ML 的依赖，纯 C 路径，够用且可复现。
* 静态库名 `zlibstatic.lib` —— CMake 4.0 `FindZLIB.cmake` 在 WIN32 的搜索名单
  `z zlib zdll zlib1 zlibstatic zlibwapi zlibvc zlibstat` 里**包含**它，所以 `find_package(ZLIB)` 直接命中。

### 5.3 zstd 1.5.7（内建 shader 压缩）

```powershell
cmake -S .deps/src/zstd-1.5.7/build/cmake -B .deps/build/zstd …   # 注意源目录在 build/cmake
-DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF
-DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF -DZSTD_LEGACY_SUPPORT=OFF
```

* **坑：静态库名是 `zstd_static.lib`**（zstd 的 CMake 在 MSVC/Clang 下故意改名，避免与导入库
  `zstd.lib` 冲突），而 `cmake/shader_pack.cmake` 用的是 `find_library(ZSTD_LIBRARY NAMES zstd)`。
  → 后处理复制一份 **`zstd.lib`**。`deps.cmake` 两个名字都认。
* zstd 静态库无额外系统依赖。

### 5.4 freetype 2.13.3（文本渲染）

```powershell
-DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE
-DFT_DISABLE_PNG=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -DFT_DISABLE_BROTLI=TRUE
```

* 关掉所有可选依赖 → 自包含静态库，无额外系统库、无 DLL。
* 安装布局 `include/freetype2/…`；`FindFreetype` 在 `FREETYPE_LIBRARY` 已置位时不再搜索，
  用 `FREETYPE_INCLUDE_DIR_ft2build` / `_freetype2` 拼 `FREETYPE_INCLUDE_DIRS`（并据此读版本号）。

### 5.5 curl 8.23.0（VFS http(s)、storage http(s)）

```powershell
-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF
-DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_DISABLE_LDAP=ON
-DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DUSE_WIN32_IDN=OFF
-DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DHTTP_ONLY=OFF
-DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DCURL_WERROR=OFF -DPICKY_COMPILER=OFF -DCMAKE_DEBUG_POSTFIX=
```

* `HTTP_ONLY=OFF` → 保留全部协议；TLS 用 **Schannel**（系统自带，离线可用，无需 OpenSSL）。
* **坑 1：静态 libcurl 的系统依赖必须显式传递。** `FindCURL.cmake`（CMake 4.0）在
  `CURL_USE_STATIC_LIBS AND MSVC` 时只硬编码 `normaliz.lib;ws2_32.lib;wldap32.lib`
  —— 那是**官方预编译包**的假设，和我们的构建不符（我们关了 IDN/LDAP，改用 Schannel）。
  → `deps.cmake` **预建** `CURL::libcurl`（`FindCURL` 的 `if(NOT TARGET CURL::libcurl)` 分支会跳过），
  接口库取 curl 自己 `CMakeLists.txt` 里 `CURL_LIBS` 的口径：
  `ws2_32 iphlpapi advapi32 crypt32 secur32 bcrypt`
  （`BCryptGenRandom` 在 `winapi.c` 里，实测缺 `bcrypt` 就会 `undefined symbol: BCryptGenRandom`）。
* **坑 2：消费者必须定义 `CURL_STATICLIB`**，否则 `curl/curl.h` 按 `__declspec(dllimport)` 声明
  → 链接期全是 `__imp_curl_*` 未解析。已写进预建 target 的 `INTERFACE_COMPILE_DEFINITIONS`。
* tag `curl-8_23_0` 里 `curlver.h` 的版本串是 `8.23.0-DEV`（上游 tag 内容如此），不影响功能。

### 5.6 libwebsockets 4.4.5（net 模块、storage ws(s)）

```powershell
-DLWS_WITH_STATIC=ON -DLWS_WITH_SHARED=OFF -DLWS_WITH_SSL=OFF -DDISABLE_WERROR=ON
-DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITHOUT_TEST_SERVER=ON -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON
-DLWS_WITHOUT_TEST_PING=ON -DLWS_WITHOUT_TEST_CLIENT=ON -DLWS_WITH_MINIMAL_EXAMPLES=OFF
-DLWS_WITHOUT_EXTENSIONS=ON -DLWS_WITH_PLUGINS=OFF -DLWS_WITHOUT_DAEMONIZE=ON
-DLWS_WITH_LEJP=ON -DLWS_WITH_JSONRPC=OFF -DLWS_WITH_LHP=OFF -DLWS_WITH_DIR=OFF
-DLWS_WITH_LEJP_CONF=OFF -DLWS_WITH_NO_LOGS=OFF
```

* 静态库名 **`websockets_static.lib`**（不是 `websockets.lib`）。头装到 `include/libwebsockets.h`
  + `include/libwebsockets/`，因此 `LWS_INCLUDE_DIRS=<dist>/include`。
* 系统库取自 lws 自己的 `LIB_LIST_AT_END`：`ws2_32 userenv psapi iphlpapi crypt32`。
* **坑：clang-cl 被 lws 的 `/WX` + `-Werror` 打死。** libwebsockets 在 `if (MSVC)` 下无条件加
  `/Zc:preprocessor /wd5105` 和 `/W3 /WX`，并在 `CMAKE_C_FLAGS` 追加 `-Werror`。clang-cl 同时定义
  `_MSC_VER` 与 `__clang__`，于是：
  1. `/Zc:preprocessor`（clang 20 未实现）→ 驱动报 `argument unused`，被 `/WX` 升级为**硬错误**；
  2. clang 独有诊断 `-Wdocumentation`（`\param len` 对不上）、`-Wreserved-identifier`
     （`_lws_log`、`_LWS_LINIT`）在 MSVC 里根本不存在，全部变成错误。
  → `fetch-deps.ps1` 对**解压副本**做两处幂等补丁（把这两组选项按 `CMAKE_C_COMPILER_ID` 收窄为
  「非 Clang 才加」），并加官方开关 `-DDISABLE_WERROR=ON`。补丁只动 `.deps/src/`，不碰上游仓库；
  重跑脚本会检查标记避免重复打补丁。

---

## 6. 已知限制（明确写出来，避免误判）

1. **libwebsockets 没有 TLS → `wss://` 不可用（`ws://` 正常）。**
   lws 4.4.5 的 TLS 后端只有 OpenSSL / mbedTLS / BoringSSL / wolfSSL / CyaSSL，**没有 Schannel 分支**，
   离线环境拿不到 OpenSSL，所以取 `LWS_WITH_SSL=OFF` 保证可构建。
   storage / net 模块的 `ws://` 与「测试内建 lws 服务端」都不受影响；
   若要 `wss://`：额外构建 mbedTLS，再用 `-DLWS_WITH_MBEDTLS=ON -DLWS_MBEDTLS_LIBRARIES=…
   -DLWS_MBEDTLS_INCLUDE_DIRS=…` 重配 lws 即可（脚本里加一个库条目 + 两个变量）。
   **`https://` 不受影响**（curl 走 Schannel，已自检证明 `ssl=Schannel`）。
2. **SDL2 走静态**，所以不需要分发 `SDL2.dll`；`bin/SDL2.dll` 与 `lib/SDL2.lib` 只是顺带装好的备用变体。
   若要切回动态：把 `SDL2_LIBRARY` 指向 `dist/lib/SDL2.lib` 并让 `dist/bin` 进 PATH。
3. **zstd/freetype/curl/lws 全静态** → 主工程不需要任何 DLL 部署步骤。
4. `dist/lib/cmake/`、`dist/lib/pkgconfig/` 是上游安装的 config/pc 文件（未使用，保留备查）。

---

## 7. 自检证据

自检工程由 `fetch-deps.ps1` 生成于 `E:\Github\maque\.deps\selfcheck\`，每个程序都走
**与 maque 模块完全相同的查找路径**（`include deps.cmake` → `maque_find_windows_deps()` →
`find_package`/`find_library` → 链 `.deps/dist` 里的库），编译 + 链接 + 运行。

### 7.1 CMake 路径（推荐复现方式）

```powershell
cmake -S .deps\selfcheck -B .deps\selfcheck\build -G Ninja `
  -DCMAKE_MAKE_PROGRAM=C:/Users/ADMIN/miniconda3/envs/Py311/Scripts/ninja.exe `
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build .deps\selfcheck\build --parallel
ctest --test-dir .deps\selfcheck\build --output-on-failure
```

配置期解析结果（证明 `deps.cmake` 生效、变量与 `find_*` 命中）：

```
-- selfcheck: SDL2_INCLUDE_DIR=E:/Github/maque/.deps/dist/include/SDL2
-- selfcheck: SDL2_LIBRARY=E:/Github/maque/.deps/dist/lib/SDL2-static-full.lib
-- selfcheck: ZLIB_FOUND=TRUE ZLIB_VERSION_STRING=1.3.1 ZLIB_LIBRARY=E:/Github/maque/.deps/dist/lib/zlibstatic.lib
-- selfcheck: ZSTD_LIBRARY=E:/Github/maque/.deps/dist/lib/zstd.lib
-- selfcheck: Freetype_FOUND=TRUE FREETYPE_LIBRARY=E:/Github/maque/.deps/dist/lib/freetype.lib FREETYPE_INCLUDE_DIRS=E:/Github/maque/.deps/dist/include/freetype2
-- selfcheck: CURL_FOUND=TRUE CURL_VERSION_STRING=8.23.0-DEV CURL_LIBRARY=E:/Github/maque/.deps/dist/lib/libcurl.lib
-- selfcheck: LWS_FOUND=TRUE LWS_INCLUDE_DIRS=E:/Github/maque/.deps/dist/include LWS_LIBRARIES=E:/Github/maque/.deps/dist/lib/websockets_static.lib;ws2_32;userenv;psapi;iphlpapi;crypt32
-- selfcheck: enabled = sdl2;zlib;zstd;freetype;curl;lws
```

测试结果：

```
1/6 Test #1: sdl2 ........... Passed
2/6 Test #2: zlib ........... Passed
3/6 Test #3: zstd ........... Passed
4/6 Test #4: freetype ....... Passed
5/6 Test #5: curl ........... Passed
6/6 Test #6: lws ............ Passed
100% tests passed, 0 tests failed out of 6
```

各程序输出（功能性证据，不只是「版本号打印」）：

```
OK sdl2: compiled=2.32.10 linked=2.32.10 ticks=0
OK zlib: header=1.3.1.zlib-ng runtime=1.3.1.zlib-ng raw=133 compressed=85 crc32=ff049913
OK zstd: header=10507 runtime=1.5.7 raw=73 compressed=77
OK freetype: header=2.13.3 runtime=2.13.3 face=Alibaba PuHuiTi 3.0 glyphs=29296 'A'=21x23 pitch=21 coverage=0
OK curl: header=8.23.0-DEV runtime=libcurl/8.23.0-DEV Schannel ssl=Schannel zlib=(none) nghttp2=no
     url parse: host=cdn.example.com path=/assets/a%20b.scn
     loopback:1 probe rc=7 (Could not connect to server)
OK lws: version=4.4.5-0bc79f4 service_rc=0
```

* SDL2：`SDL_GetVersion` 运行期版本 = 2.32.10，`SDL_Init(SDL_INIT_TIMER)` 成功。
* zlib：`compress2`/`uncompress` 往返一致 + `crc32`，证明真的调到了库代码。
* zstd：19 级压缩 + 解压往返一致。
* freetype：用仓库自带中文字体 `scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf` **真的栅格化**了 `'A'`
  （32px → 21×23 位图，29296 字形），不是只打版本号。
* curl：Schannel 后端存在（`ssl=Schannel`）、URL 解析（VFS 的 percent-encode 路径）、
  对 `127.0.0.1:1` 的离线探测走通 DNS/connect 错误路径（`rc=7`，不依赖外网）。
* lws：创建 context → `lws_service` → 销毁，全部真调用。

### 7.2 裸 clang-cl 路径（无 vcvars、无 INCLUDE/LIB 环境变量）

`$env:INCLUDE`/`$env:LIB` 都为空（clang-cl 自行定位 MSVC 工具链），逐库「编译 + 链接 + 运行」：

```bat
:: 工作目录 E:\Github\maque\.deps\selfcheck
clang-cl /nologo /O2 /MD -IE:\Github\maque\.deps\dist\include zlib_check.c ^
         E:\Github\maque\.deps\dist\lib\zlibstatic.lib /Fe:raw_zlib.exe
→ OK zlib: header=1.3.1.zlib-ng runtime=1.3.1.zlib-ng raw=133 compressed=85 crc32=ff049913   (rc=0)

clang-cl /nologo /O2 /MD -IE:\Github\maque\.deps\dist\include zstd_check.c ^
         E:\Github\maque\.deps\dist\lib\zstd.lib /Fe:raw_zstd.exe
→ OK zstd: header=10507 runtime=1.5.7 raw=73 compressed=77                                    (rc=0)

clang-cl /nologo /O2 /MD -IE:\Github\maque\.deps\dist\include\freetype2 freetype_check.c ^
         E:\Github\maque\.deps\dist\lib\freetype.lib /Fe:raw_freetype.exe ^
         && raw_freetype.exe E:\Github\maque\scenes\fonts\AlibabaPuHuiTi-3-55-Regular.ttf
→ OK freetype: header=2.13.3 runtime=2.13.3 face=Alibaba PuHuiTi 3.0 glyphs=29296 'A'=21x23 pitch=21 coverage=0  (rc=0)

clang-cl /nologo /O2 /MD /DCURL_STATICLIB -IE:\Github\maque\.deps\dist\include curl_check.c ^
         E:\Github\maque\.deps\dist\lib\libcurl.lib ws2_32.lib iphlpapi.lib advapi32.lib crypt32.lib secur32.lib bcrypt.lib ^
         /Fe:raw_curl.exe
→ OK curl: header=8.23.0-DEV runtime=libcurl/8.23.0-DEV Schannel ssl=Schannel …              (rc=0)

clang-cl /nologo /O2 /MD -IE:\Github\maque\.deps\dist\include\SDL2 sdl2_check.cpp ^
         E:\Github\maque\.deps\dist\lib\SDL2-static-full.lib kernel32.lib user32.lib gdi32.lib winmm.lib imm32.lib ^
         ole32.lib oleaut32.lib version.lib uuid.lib advapi32.lib setupapi.lib shell32.lib dinput8.lib ^
         /Fe:raw_sdl2.exe /link /subsystem:console
→ OK sdl2: compiled=2.32.10 linked=2.32.10 ticks=0                                             (rc=0)
   ⚠️ 少了 /link /subsystem:console 会 LNK1561，见 §5.1

clang-cl /nologo /O2 /MD -IE:\Github\maque\.deps\dist\include lws_check.c ^
         E:\Github\maque\.deps\dist\lib\websockets_static.lib ws2_32.lib userenv.lib psapi.lib iphlpapi.lib crypt32.lib ^
         /Fe:raw_lws.exe
→ OK lws: version=4.4.5-0bc79f4 service_rc=0                                                   (rc=0)
```

### 7.3 幂等性与全量重建

* `fetch-deps.ps1`（不加参数）：6 个库全部 `SKIP (已安装: …)`，后处理、`deps.cmake` 生成、
  自检照常执行 → 自检 `PASS`。
* `fetch-deps.ps1 -Force`：6 个库全部重新 configure/build/install，随后自检 `PASS`。

---

## 8. 给主工程的提醒（不在我的写入范围，留给 Lead 决定）

1. 根 `CMakeLists.txt:28` 的 `add_compile_options(/permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /W3)`
   在 `if(MSVC)` 下对 clang-cl **也**生效，clang-cl 会对 `/Zc:preprocessor` 报
   `argument unused during compilation`。当前没有 `/WX`，只是**警告**、不影响构建；
   如果想消掉，可加 `-Qunused-arguments` 或按 `CMAKE_CXX_COMPILER_ID` 收窄。
2. 配置主工程时请勿加载 `vcvars64.bat`（§1 铁律 1），并确认 `MAQUE_WINDOWS_DEPS=ON`（默认 ON）。

---

生成时间：2026-10-16 · 生成者：deps-builder（task-1）
