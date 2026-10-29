<#
    tools/win/fetch-deps.ps1 —— maque Windows 第三方原生依赖：一条命令搞定的完整流程
    ────────────────────────────────────────────────────────────────────────────
    下载 → 解压 → （必要的可移植性补丁）→ CMake/Ninja/clang-cl 构建 → 安装到统一前缀
    → 后处理 → 生成 .deps/deps.cmake → 构建并运行 6 个独立自检程序。

    用法（PowerShell 5.1 即可，脚本文件默认被执行策略拦住，必须带 Bypass）：

        powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\fetch-deps.ps1

        -Only sdl2,curl     只处理部分库（默认全部 6 个）
        -Force              忽略“已安装”标记，全部重建
        -SkipSelfCheck      跳过最后的独立自检
        -ClangCl <path>     覆盖 clang-cl 路径（默认 C:\Program Files\LLVM\bin\clang-cl.exe）
        -Ninja <path>       覆盖 ninja 路径（默认：PATH 或 conda 里的 ninja.exe）

    产物（全部在 .deps/ 下，仓库不入库，见 .gitignore 的 /.deps/）：

        .deps/dist/include|lib|bin    统一安装前缀（6 个库）
        .deps/deps.cmake              根 CMakeLists 用 include() + maque_find_windows_deps()
        .deps/logs/*.log              每个库的 configure/build/install 日志
        .deps/selfcheck/              独立自检工程（源码由本脚本生成）
        .deps/BUILD-NOTES.md          构建说明与自检证据（若已在则保留）

    ⚠️ 两个必须遵守的约束（都已实测，改脚本前先看这里）：

      1) **不要加载 vcvars64.bat**。加载后 PATH 会先命中 VS 自带的 clang-cl 19.1.5，
         而且 CMake 的 “Detecting CXX compiler ABI info” 会**永久挂起**。
         本脚本把 clang-cl 的绝对路径直接交给 CMAKE_C_COMPILER/CXX_COMPILER，
         Windows SDK / MSVC 头由 CMake 自己定位（已验证）。
      2) **github.com 不可达**（git clone 443 超时）。源码一律走 codeload.github.com 的
         tar.gz，curl.exe 下载，Windows 自带 tar.exe 解压（tarball 里的 symlink 会解压失败，
         那只是 libwebsockets 的 embedded 示例与 zstd 的测试脚本，与构建无关）。

    统一约定：Release + Ninja + clang-cl + 动态 MSVC 运行时
    （-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL，即 /MD），与主工程一致，
    避免 CRT 不匹配。
#>
[CmdletBinding()]
param(
    [string[]]$Only = @(),
    [switch]$Force,
    [switch]$SkipSelfCheck,
    [string]$ClangCl = 'C:\Program Files\LLVM\bin\clang-cl.exe',
    [string]$Ninja = ''
)

# 原生工具往 stderr 写警告时，'Stop' 会把它们变成终止性错误；统一用退出码判断。
$ErrorActionPreference = 'Continue'

$Repo    = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)      # tools/win -> tools -> <repo>
$Deps    = Join-Path $Repo '.deps'
$Cache   = Join-Path $Deps 'cache'
$SrcRoot = Join-Path $Deps 'src'
$BuildRoot = Join-Path $Deps 'build'
$Dist    = Join-Path $Deps 'dist'
$LogRoot = Join-Path $Deps 'logs'
$SelfDir = Join-Path $Deps 'selfcheck'

New-Item -ItemType Directory -Force -Path $Cache, $SrcRoot, $BuildRoot, $Dist, $LogRoot | Out-Null

# ---------------------------------------------------------------- 工具解析
if (-not (Test-Path $ClangCl)) {
    Write-Host "FATAL: clang-cl 不存在：$ClangCl（用 -ClangCl <path> 指定）"
    exit 1
}
if (-not $Ninja) {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { $Ninja = $cmd.Source }
}
if (-not $Ninja) {
    foreach ($cand in @(
        'C:\Users\ADMIN\miniconda3\envs\Py311\Scripts\ninja.exe',
        "$env:USERPROFILE\miniconda3\envs\Py311\Scripts\ninja.exe",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe")) {
        if (Test-Path $cand) { $Ninja = $cand; break }
    }
}
if (-not $Ninja -or -not (Test-Path $Ninja)) {
    Write-Host "FATAL: 找不到 ninja（用 -Ninja <path> 指定）"
    exit 1
}
# CMake 需要 clang-cl 的正斜杠形式
$ClangClArg = $ClangCl -replace '\\', '/'
$NinjaArg   = $Ninja   -replace '\\', '/'
$env:PATH = (Split-Path $Ninja) + ';' + $env:PATH

Write-Host "=== maque Windows deps ==="
Write-Host "  repo    : $Repo"
Write-Host "  prefix  : $Dist"
Write-Host "  clang-cl: $ClangCl"
Write-Host "  ninja   : $Ninja"

# ---------------------------------------------------------------- 依赖清单
# Name / Url / File / 解压后目录 / cmake 源目录（相对 src/<dir>）/ 额外配置 / 安装标记
$Libraries = @(
    @{ Name = 'zlibng'; Url = 'https://codeload.github.com/zlib-ng/zlib-ng/tar.gz/refs/tags/2.2.4'
       File = 'zlib-ng-2.2.4.tar.gz'; Dir = 'zlib-ng-2.2.4'; Sub = ''
       Args = @('-DBUILD_SHARED_LIBS=OFF', '-DZLIB_COMPAT=ON', '-DWITH_OPTIM=OFF',
                '-DZLIB_ENABLE_TESTS=OFF', '-DWITH_GTEST=OFF', '-DINSTALL_UTILS=OFF', '-DWITH_BENCHMARKS=OFF')
       Stamp = 'lib\zlibstatic.lib' }
    @{ Name = 'freetype'; Url = 'https://codeload.github.com/freetype/freetype/tar.gz/refs/tags/VER-2-13-3'
       File = 'freetype-VER-2-13-3.tar.gz'; Dir = 'freetype-VER-2-13-3'; Sub = ''
       Args = @('-DBUILD_SHARED_LIBS=OFF', '-DFT_DISABLE_ZLIB=TRUE', '-DFT_DISABLE_BZIP2=TRUE',
                '-DFT_DISABLE_PNG=TRUE', '-DFT_DISABLE_HARFBUZZ=TRUE', '-DFT_DISABLE_BROTLI=TRUE')
       Stamp = 'lib\freetype.lib' }
    @{ Name = 'harfbuzz'; Url = 'https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/refs/tags/10.2.0'
       File = 'harfbuzz-10.2.0.tar.gz'; Dir = 'harfbuzz-10.2.0'; Sub = ''
       Args = @('-DHB_HAVE_FREETYPE=ON', '-DHB_BUILD_UTILS=OFF', '-DHB_BUILD_SUBSET=OFF',
                '-DHB_HAVE_GLIB=OFF', '-DHB_HAVE_ICU=OFF')
       Stamp = 'lib\harfbuzz.lib' }
    @{ Name = 'zstd'; Url = 'https://codeload.github.com/facebook/zstd/tar.gz/refs/tags/v1.5.7'
       File = 'zstd-1.5.7.tar.gz'; Dir = 'zstd-1.5.7'; Sub = 'build\cmake'
       Args = @('-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_STATIC=ON', '-DZSTD_BUILD_PROGRAMS=OFF',
                '-DZSTD_BUILD_TESTS=OFF', '-DZSTD_BUILD_CONTRIB=OFF', '-DZSTD_LEGACY_SUPPORT=OFF')
       Stamp = 'lib\zstd_static.lib' }
    @{ Name = 'curl'; Url = 'https://codeload.github.com/curl/curl/tar.gz/refs/tags/curl-8_23_0'
       File = 'curl-curl-8_23_0.tar.gz'; Dir = 'curl-curl-8_23_0'; Sub = ''
       Args = @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON', '-DBUILD_CURL_EXE=OFF',
                '-DCURL_USE_SCHANNEL=ON', '-DCURL_USE_OPENSSL=OFF', '-DCURL_DISABLE_LDAP=ON',
                '-DCURL_USE_LIBPSL=OFF', '-DCURL_USE_LIBSSH2=OFF', '-DUSE_NGHTTP2=OFF',
                '-DUSE_WIN32_IDN=OFF', '-DCURL_ZLIB=OFF', '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF',
                '-DHTTP_ONLY=OFF', '-DBUILD_TESTING=OFF', '-DBUILD_EXAMPLES=OFF',
                '-DCURL_WERROR=OFF', '-DPICKY_COMPILER=OFF', '-DCMAKE_DEBUG_POSTFIX=')
       Stamp = 'lib\libcurl.lib' }
    @{ Name = 'sdl2'; Url = 'https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-2.32.10'
       File = 'SDL-release-2.32.10.tar.gz'; Dir = 'SDL-release-2.32.10'; Sub = ''
       Args = @('-DSDL_SHARED=ON', '-DSDL_STATIC=ON', '-DSDL_TEST=OFF', '-DSDL_TESTS=OFF',
                '-DSDL_DISABLE_INSTALL_DOCS=ON')
       Stamp = 'lib\SDL2-static.lib' }
    @{ Name = 'lws'; Url = 'https://codeload.github.com/warmcat/libwebsockets/tar.gz/refs/tags/v4.4.5'
       File = 'libwebsockets-4.4.5.tar.gz'; Dir = 'libwebsockets-4.4.5'; Sub = ''
       # TLS：lws 4.4.5 的 TLS 后端只有 OpenSSL/mbedTLS(要额外依赖)，没有 Schannel 分支，
       # 离线环境下用 LWS_WITH_SSL=OFF 保证可构建 —— 结果：ws:// 可用，wss:// 不可用
       # （curl 那侧走 Schannel，https:// 不受影响）。见 BUILD-NOTES.md「已知限制」。
       # DISABLE_WERROR：lws 默认 /WX + -Werror，而 clang 有 MSVC 没有的诊断
       # （-Wdocumentation / -Wreserved-identifier …），见 Apply-Patches。
       Args = @('-DLWS_WITH_STATIC=ON', '-DLWS_WITH_SHARED=OFF', '-DLWS_WITH_SSL=OFF',
                '-DLWS_WITHOUT_TESTAPPS=ON', '-DLWS_WITHOUT_TEST_SERVER=ON',
                '-DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON', '-DLWS_WITHOUT_TEST_PING=ON',
                '-DLWS_WITHOUT_TEST_CLIENT=ON', '-DLWS_WITH_MINIMAL_EXAMPLES=OFF',
                '-DLWS_WITHOUT_EXTENSIONS=ON', '-DLWS_WITH_PLUGINS=OFF',
                '-DLWS_WITHOUT_DAEMONIZE=ON', '-DLWS_WITH_STATIC_PIC=OFF',
                '-DLWS_WITH_LEJP=ON', '-DLWS_WITH_JSONRPC=OFF', '-DLWS_WITH_LHP=OFF',
                '-DLWS_WITH_DIR=OFF', '-DLWS_WITH_LEJP_CONF=OFF', '-DLWS_WITH_NO_LOGS=OFF',
                '-DDISABLE_WERROR=ON')
       Stamp = 'lib\websockets_static.lib' }
)

function Get-Lib($name) { $Libraries | Where-Object { $_.Name -eq $name } | Select-Object -First 1 }

# ---------------------------------------------------------------- 下载 / 解压
function Invoke-Download($lib) {
    $tgz = Join-Path $Cache $lib.File
    if ((Test-Path $tgz) -and ((Get-Item $tgz).Length -gt 20000)) {
        Write-Host "  cached: $($lib.File) ($((Get-Item $tgz).Length) bytes)"
        return $true
    }
    $code = (& curl.exe -sS -L --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 900 `
                -o $tgz -w '%{http_code}' $lib.Url)
    $len = if (Test-Path $tgz) { (Get-Item $tgz).Length } else { 0 }
    if ($code -ne '200' -or $len -lt 20000) {
        Write-Host "  FAIL download http=$code bytes=$len  $($lib.Url)"
        Remove-Item $tgz -ErrorAction SilentlyContinue
        return $false
    }
    Write-Host "  downloaded: $($lib.File) ($len bytes)"
    return $true
}

function Invoke-Extract($lib) {
    $dest = Join-Path $SrcRoot $lib.Dir
    # 注意：zstd 的 CMakeLists.txt 在 build/cmake 下（源码根目录没有），所以用
    # 「CMake 源目录」是否存在来判断是否已解压，而不是 <dir>/CMakeLists.txt。
    $cmSrc = $dest
    if ($lib.Sub) { $cmSrc = Join-Path $dest $lib.Sub }
    if (Test-Path (Join-Path $cmSrc 'CMakeLists.txt')) {
        Write-Host "  source already extracted: $dest"
        return $true
    }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    $tgz = Join-Path $Cache $lib.File
    # tarball 里的符号链接在 Windows 上会报 "Invalid argument" —— 与构建无关，忽略。
    & tar.exe -xzf $tgz -C $SrcRoot 2>&1 | ForEach-Object { if ($_ -notmatch 'Invalid argument|Error exit delayed') { Write-Host "     $_" } }
    if (Test-Path (Join-Path $cmSrc 'CMakeLists.txt')) { Write-Host "  extracted: $dest"; return $true }
    Write-Host "  FAIL extract -> $dest"
    return $false
}

# ---------------------------------------------------------------- clang-cl 可移植性补丁
# libwebsockets 4.4.5 在 `if (MSVC)` 下无条件加 /Zc:preprocessor + /W3 /WX（外加
# CMAKE_C_FLAGS 里的 -Werror）。clang-cl 同时定义 _MSC_VER 与 __clang__，于是：
#   * /Zc:preprocessor（clang 未实现）→ “argument unused”，被 /WX 升级为硬错误；
#   * clang 独有的 -Wdocumentation / -Wreserved-identifier 等诊断也全部变错误。
# 两处都按编译器 id 收窄（幂等；只动 .deps/src 下的解压副本，不碰上游仓库）。
function Apply-Patches($lib, $src) {
    if ($lib.Name -ne 'lws') { return }
    $cm = Join-Path $src 'CMakeLists.txt'
    if (-not (Test-Path $cm)) { return }
    $txt = [System.IO.File]::ReadAllText($cm)
    $changed = $false

    if (-not $txt.Contains('maque: preprocessor flags are MSVC-only')) {
        $pat = '(?s)# Unbreak MSVC broken preprocessor __VA_ARGS__ behaviour.*?\r?\n(\s*)endif\(\)'
        $rep = "`t# maque: preprocessor flags are MSVC-only (clang-cl implements neither flag, and lws adds /WX)`r`n" +
               "`tif (NOT `"`${CMAKE_C_COMPILER_ID}`" STREQUAL `"Clang`")`r`n" +
               "`t`tif (MSVC_VERSION GREATER 1925)`r`n" +
               "`t`t`tadd_compile_options(/Zc:preprocessor /wd5105)`r`n" +
               "`t`telse()`r`n" +
               "`t`t`tadd_compile_options(/experimental:preprocessor /wd5105)`r`n" +
               "`t`tendif()`r`n" +
               "`tendif()"
        if ($txt -match $pat) { $txt = $txt -replace $pat, $rep; $changed = $true; Write-Host "  patch: preprocessor flags -> MSVC only" }
        else { Write-Host "  WARN patch: preprocessor anchor not found" }
    }
    if (-not $txt.Contains('maque: no /WX for clang')) {
        $pat2 = '(?m)^([ \t]*)add_compile_options\(/W3 /WX\)'
        $rep2 = "`$1# maque: no /WX for clang (clang-only diagnostics would fail the build)`r`n" +
                "`$1if (NOT `"`${CMAKE_C_COMPILER_ID}`" STREQUAL `"Clang`")`r`n" +
                "`$1`tadd_compile_options(/W3 /WX)`r`n" +
                "`$1else()`r`n" +
                "`$1`tadd_compile_options(/W3)`r`n" +
                "`$1endif()"
        if ($txt -match $pat2) { $txt = $txt -replace $pat2, $rep2; $changed = $true; Write-Host "  patch: /WX dropped for clang" }
        else { Write-Host "  WARN patch: /WX anchor not found" }
    }
    if ($changed) {
        [System.IO.File]::WriteAllText($cm, $txt, (New-Object System.Text.UTF8Encoding($false)))
    }
}

# ---------------------------------------------------------------- 构建 / 安装
function Invoke-Build($lib) {
    $src   = Join-Path $SrcRoot $lib.Dir
    if ($lib.Sub) { $src = Join-Path $src $lib.Sub }
    $bdir  = Join-Path $BuildRoot $lib.Name
    $stamp = Join-Path $Dist $lib.Stamp
    $log   = Join-Path $LogRoot "$($lib.Name).log"

    if ((Test-Path $stamp) -and -not $Force) {
        Write-Host "  SKIP (已安装: $stamp)"
        return 'skipped(installed)'
    }

    Apply-Patches $lib $src

    $common = @('-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$NinjaArg",
                "-DCMAKE_C_COMPILER=$ClangClArg", "-DCMAKE_CXX_COMPILER=$ClangClArg",
                '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
                "-DCMAKE_INSTALL_PREFIX=$($Dist -replace '\\','/')",
                "-DCMAKE_PREFIX_PATH=$($Dist -replace '\\','/')")

    $t0 = Get-Date
    "=== configure $($lib.Name) $(Get-Date -Format o) ===" | Set-Content -Path $log -Encoding UTF8
    & cmake -S $src -B $bdir @common @($lib.Args) *>&1 | Tee-Object -FilePath $log -Append | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL configure rc=$LASTEXITCODE  (log: $log)"
        Get-Content $log -Tail 25 | ForEach-Object { "   | $_" }
        return "fail(configure rc=$LASTEXITCODE)"
    }
    "=== build $($lib.Name) ===" | Add-Content -Path $log
    & cmake --build $bdir --parallel *>&1 | Tee-Object -FilePath $log -Append | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL build rc=$LASTEXITCODE  (log: $log)"
        Get-Content $log -Tail 25 | ForEach-Object { "   | $_" }
        return "fail(build rc=$LASTEXITCODE)"
    }
    "=== install $($lib.Name) ===" | Add-Content -Path $log
    & cmake --install $bdir *>&1 | Tee-Object -FilePath $log -Append | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL install rc=$LASTEXITCODE  (log: $log)"
        return "fail(install rc=$LASTEXITCODE)"
    }
    $dt = [int]((Get-Date) - $t0).TotalSeconds
    if (Test-Path $stamp) { Write-Host "  OK (${dt}s) -> $stamp"; return "ok(${dt}s)" }
    Write-Host "  WARN built+installed but stamp missing: $stamp"
    return 'warn(no stamp)'
}

# ---------------------------------------------------------------- 后处理
# 1) zstd：静态库在 MSVC/clang-cl 下叫 zstd_static.lib，而 cmake/shader_pack.cmake 用
#    find_library(ZSTD_LIBRARY NAMES zstd) → 复制一份 zstd.lib。
# 2) SDL2：Windows 上 SDL_main.h 把应用 main 改名为 SDL_main，需要 SDL2main 的
#    main/WinMain 垫片；maque 用裸路径链接（没有 imported target），所以把
#    SDL2-static.lib + SDL2main.lib 合并成单个 SDL2-static-full.lib。
function Invoke-Fixups {
    Write-Host "--- 后处理 ---"
    $lib = Join-Path $Dist 'lib'
    $zstdStatic = Join-Path $lib 'zstd_static.lib'
    if (Test-Path $zstdStatic) {
        Copy-Item $zstdStatic (Join-Path $lib 'zstd.lib') -Force
        Write-Host "  zstd.lib <- zstd_static.lib"
    }
    $sdlStatic = Join-Path $lib 'SDL2-static.lib'
    $sdlMain   = Join-Path $lib 'SDL2main.lib'
    $sdlFull   = Join-Path $lib 'SDL2-static-full.lib'
    if ((Test-Path $sdlStatic) -and (Test-Path $sdlMain)) {
        $llvmLib = Join-Path (Split-Path $ClangCl) 'llvm-lib.exe'
        if (-not (Test-Path $llvmLib)) { $llvmLib = 'llvm-lib.exe' }
        Remove-Item $sdlFull -ErrorAction SilentlyContinue
        & $llvmLib /nologo /out:$sdlFull $sdlStatic $sdlMain 2>&1 | Out-Null
        if (Test-Path $sdlFull) { Write-Host "  SDL2-static-full.lib <- SDL2-static.lib + SDL2main.lib" }
        else { Write-Host "  FAIL SDL2 merge" }
    }
}

# ---------------------------------------------------------------- 生成 deps.cmake
function Write-DepsCMake {
    $path = Join-Path $Deps 'deps.cmake'
    $content = @'
# =============================================================================
#  maque Windows 第三方依赖预置（由 tools/win/fetch-deps.ps1 生成）
# -----------------------------------------------------------------------------
#  用法（根 CMakeLists.txt 已接线，无需改动各模块）：
#
#      include("<repo>/.deps/deps.cmake")
#      if(COMMAND maque_find_windows_deps)
#          maque_find_windows_deps()
#      endif()
#
#  作用：把 .deps/dist 里已构建安装的 6 个原生依赖
#      SDL2 · zlib-ng · zstd · freetype · libcurl · libwebsockets
#  以 **缓存变量 + 预建 imported target** 的形式喂给后续的
#  find_package / find_library，使 maque 各模块**零改动**命中：
#
#      模块                    查找方式（现状）                      本文件提供的变量
#      modules/platform        find_path(SDL.h) find_library(SDL2)   SDL2_INCLUDE_DIR / SDL2_LIBRARY
#      samples/inspector       同上                                  同上
#      modules/image           find_package(ZLIB)                    ZLIB_INCLUDE_DIR / ZLIB_LIBRARY
#      cmake/shader_pack       find_library(ZSTD_LIBRARY NAMES zstd) ZSTD_LIBRARY
#      modules/render/text     find_package(Freetype)                FREETYPE_*
#      modules/io/vfs|storage  find_package(CURL) → CURL::libcurl    CURL_* + 预建 target
#      modules/net|storage     pkg_check_modules(LWS libwebsockets)  LWS_* （Windows 无 pkg-config）
#
#  两个**必须**预建/额外的处理，写在注释里以免后人踩坑：
#   1) CURL::libcurl —— 静态 libcurl 的系统依赖（Schannel 后端：ws2_32/iphlpapi/
#      advapi32/crypt32/secur32/bcrypt）无法从实现里自动得到；FindCURL.cmake 在 Windows
#      上只会硬编码 normaliz/ws2_32/wldap32（那是官方预编译包的假设）。预建后
#      FindCURL 的 `if(NOT TARGET CURL::libcurl)` 分支会跳过，链接行才正确。
#      同时必须给消费者加 CURL_STATICLIB（否则头里按 dllimport 声明 → 链接失败）。
#   2) SDL2 静态库的 Windows 系统库 —— 模块用裸路径
#      `target_link_libraries(maque_platform PUBLIC "${SDL2_LIBRARY}")`，拿不到
#      SDL2-static 目标的 INTERFACE 传递依赖，故用 link_libraries() 在顶层补齐
#      （列表取自 dist/lib/pkgconfig/sdl2.pc 的 Libs.private）。
#
#  依赖本身不入库（.gitignore 忽略 /.deps/）；重建见 tools/win/fetch-deps.ps1。
# =============================================================================

set(_MAQUE_DEPS_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(_MAQUE_DEPS_DIST "${_MAQUE_DEPS_DIR}/dist")

function(maque_find_windows_deps)
    set(_dist "${_MAQUE_DEPS_DIST}")
    set(_inc "${_dist}/include")
    set(_lib "${_dist}/lib")

    if(NOT EXISTS "${_dist}")
        message(WARNING "maque: 未找到 ${_dist} —— 先运行 tools/win/fetch-deps.ps1；"
                        "缺失的依赖会按各模块的降级路径关闭")
        return()
    endif()

    set(MAQUE_DEPS_ROOT "${_MAQUE_DEPS_DIR}" CACHE PATH "maque Windows 依赖根目录" FORCE)
    set(MAQUE_DEPS_DIST "${_dist}" CACHE PATH "maque Windows 依赖安装前缀" FORCE)

    # 让裸 find_path/find_library 也能命中 dist（顶层 scope → 所有子目录继承）
    if(NOT "${CMAKE_PREFIX_PATH}" MATCHES "\\.deps[/\\\\]dist")
        set(CMAKE_PREFIX_PATH "${_dist};${CMAKE_PREFIX_PATH}"
            CACHE PATH "CMake 搜索前缀（含 maque .deps/dist）" FORCE)
    endif()

    # 头文件搜索路径：Windows 上没有系统级 /usr/include，而 maque 有按裸名 include 的地方
    # （cmake/shader_pack.cmake 生成的 zstd 解压实现 `#include <zstd.h>`、各模块的
    #  <curl/curl.h> / <libwebsockets.h> 等）。目录级 include_directories 对顶层调用
    # → 全工程（含子目录）继承，等价于 Linux 上这些头在系统路径里。
    include_directories("${_inc}")

    set(_missing "")

    # ---------------------------------------------------------------- SDL2（窗口/输入）
    # 静态链接：优先用 SDL2-static-full.lib —— 它是 SDL2-static.lib + SDL2main.lib 的
    # 合并归档。Windows 上 SDL_main.h 会把应用的 main 改名为 SDL_main，SDL2main 提供
    # `main`/`WinMain` 垫片；合并成单个 .lib 后，模块只链一个路径也能过。
    set(_sdl2 "${_lib}/SDL2-static-full.lib")
    if(NOT EXISTS "${_sdl2}")
        set(_sdl2 "${_lib}/SDL2-static.lib")
    endif()
    if(EXISTS "${_sdl2}")
        set(SDL2_INCLUDE_DIR "${_inc}/SDL2" CACHE PATH "SDL2 头文件目录" FORCE)
        set(SDL2_LIBRARY "${_sdl2}" CACHE FILEPATH "SDL2 静态库（含 SDL2main 垫片）" FORCE)
        if(EXISTS "${_dist}/bin/SDL2.dll")
            set(SDL2_DLL "${_dist}/bin/SDL2.dll" CACHE FILEPATH "SDL2 动态库（可选，未使用）" FORCE)
            set(SDL2_SHARED_LIBRARY "${_lib}/SDL2.lib" CACHE FILEPATH "SDL2 导入库（可选，未使用）" FORCE)
        endif()
        # 静态 SDL2 需要的 Windows 系统库（SDL2 目标的 INTERFACE 依赖拿不到）
        link_libraries(kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid
                       advapi32 setupapi shell32 dinput8)
        message(STATUS "maque deps: SDL2 静态 = ${SDL2_LIBRARY}（SDL2-static + SDL2main）+ 系统库")
    else()
        list(APPEND _missing "SDL2")
    endif()

    # ---------------------------------------------------------------- zlib-ng（PNG 写出）
    # zlib-ng 2.2.4 以 ZLIB_COMPAT=ON 构建 → 装出 zlib.h/zconf.h + zlibstatic.lib，
    # FindZLIB 的 WIN32 搜索名单里含 zlibstatic，因此 find_package(ZLIB) 直接命中。
    if(EXISTS "${_lib}/zlibstatic.lib")
        set(ZLIB_INCLUDE_DIR "${_inc}" CACHE PATH "zlib 头文件目录" FORCE)
        set(ZLIB_LIBRARY "${_lib}/zlibstatic.lib" CACHE FILEPATH "zlib 静态库（zlib-ng compat）" FORCE)
        set(ZLIB_LIBRARY_RELEASE "${_lib}/zlibstatic.lib" CACHE FILEPATH "zlib 静态库（Release）" FORCE)
        message(STATUS "maque deps: zlib = ${ZLIB_LIBRARY}（zlib-ng 2.2.4 ZLIB_COMPAT）")
    else()
        list(APPEND _missing "zlib")
    endif()

    # ---------------------------------------------------------------- zstd（内建 shader 压缩）
    # cmake/shader_pack.cmake: find_library(ZSTD_LIBRARY NAMES zstd)
    # zstd 在 MSVC/Clang 上把静态库叫 zstd_static.lib（避免与导入库同名），
    # 构建流程会额外复制一份 zstd.lib，两个名字都认。
    set(_zstd "${_lib}/zstd.lib")
    if(NOT EXISTS "${_zstd}")
        set(_zstd "${_lib}/zstd_static.lib")
    endif()
    if(EXISTS "${_zstd}")
        set(ZSTD_LIBRARY "${_zstd}" CACHE FILEPATH "libzstd 静态库" FORCE)
        set(ZSTD_INCLUDE_DIR "${_inc}" CACHE PATH "libzstd 头文件目录" FORCE)
        message(STATUS "maque deps: zstd = ${ZSTD_LIBRARY}")
    else()
        list(APPEND _missing "zstd")
    endif()

    # ---------------------------------------------------------------- freetype（文本渲染）
    # 安装布局：<dist>/include/freetype2/ft2build.h + <dist>/lib/freetype.lib
    # FindFreetype 在 FREETYPE_LIBRARY 已置位时不再搜索，并用这两个 include 变量拼
    # FREETYPE_INCLUDE_DIRS（还会据此读版本号）。
    if(EXISTS "${_lib}/freetype.lib")
        set(FREETYPE_INCLUDE_DIR_ft2build "${_inc}/freetype2" CACHE PATH "freetype ft2build.h 目录" FORCE)
        set(FREETYPE_INCLUDE_DIR_freetype2 "${_inc}/freetype2" CACHE PATH "freetype 头目录" FORCE)
        set(FREETYPE_LIBRARY "${_lib}/freetype.lib" CACHE FILEPATH "freetype 静态库" FORCE)
        message(STATUS "maque deps: freetype = ${FREETYPE_LIBRARY}（自带 zlib/png/harfbuzz 均关闭）")
    else()
        list(APPEND _missing "freetype")
    endif()

    # ---------------------------------------------------------------- libcurl（VFS/storage http）
    if(EXISTS "${_lib}/libcurl.lib")
        set(CURL_INCLUDE_DIR "${_inc}" CACHE PATH "libcurl 头文件目录" FORCE)
        set(CURL_LIBRARY "${_lib}/libcurl.lib" CACHE FILEPATH "libcurl 静态库（Schannel）" FORCE)
        set(CURL_USE_STATIC_LIBS ON CACHE BOOL "libcurl 静态链接" FORCE)
        # 预建 target（理由见文件头注释 1）
        if(NOT TARGET CURL::libcurl)
            add_library(CURL::libcurl UNKNOWN IMPORTED)
            set_target_properties(CURL::libcurl PROPERTIES
                IMPORTED_LOCATION "${_lib}/libcurl.lib"
                INTERFACE_INCLUDE_DIRECTORIES "${_inc}"
                INTERFACE_COMPILE_DEFINITIONS "CURL_STATICLIB"
                INTERFACE_LINK_LIBRARIES "ws2_32;iphlpapi;advapi32;crypt32;secur32;bcrypt")
        endif()
        message(STATUS "maque deps: curl = ${CURL_LIBRARY}（静态 + Schannel；预建 CURL::libcurl）")
    else()
        list(APPEND _missing "curl")
    endif()

    # ---------------------------------------------------------------- libwebsockets（net/ws）
    # maque 用 pkg_check_modules(LWS libwebsockets)；Windows 无 pkg-config，所以这里
    # 直接预置 pkg-config 风格变量（LWS_LIBRARIES 允许是「路径 + 系统库名」的列表）。
    # lws 静态库在 MSVC/clang-cl 下叫 websockets_static.lib。
    set(_lws "${_lib}/websockets_static.lib")
    if(NOT EXISTS "${_lws}")
        set(_lws "${_lib}/websockets.lib")
    endif()
    if(EXISTS "${_lws}")
        set(LWS_FOUND TRUE CACHE BOOL "libwebsockets 可用" FORCE)
        set(LWS_INCLUDE_DIRS "${_inc}" CACHE STRING "libwebsockets 头目录" FORCE)
        set(LWS_LIBRARY_DIRS "${_lib}" CACHE STRING "libwebsockets 库目录" FORCE)
        # 系统库取自 libwebsockets CMakeLists 的 LIB_LIST_AT_END
        set(LWS_LIBRARIES "${_lws};ws2_32;userenv;psapi;iphlpapi;crypt32"
            CACHE STRING "libwebsockets 链接列表" FORCE)
        set(LWS_INCLUDE_DIR "${_inc}" CACHE PATH "libwebsockets 头目录" FORCE)
        set(LWS_LIBRARY "${_lws}" CACHE FILEPATH "libwebsockets 静态库" FORCE)
        set(LWS_VERSION "4.4.5" CACHE STRING "libwebsockets 版本" FORCE)
        set(LWS_PREFIX "${_dist}" CACHE PATH "libwebsockets 前缀" FORCE)
        set(LWS_CFLAGS "-I${_inc}" CACHE STRING "libwebsockets CFLAGS" FORCE)
        message(STATUS "maque deps: libwebsockets = ${LWS_LIBRARY}（静态；TLS 关闭 → 仅 ws://）")
    else()
        list(APPEND _missing "libwebsockets")
    endif()

    if(_missing)
        message(WARNING "maque deps: ${_dist} 中缺少：${_missing}（对应模块会走降级/关闭路径）")
    else()
        message(STATUS "maque deps: 6/6 依赖就绪（${_dist}）")
    endif()
endfunction()
'@
    [System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "--- 已生成 $path ---"
}

# ---------------------------------------------------------------- 独立自检工程
# 每个程序都走**和 maque 模块完全一样**的查找路径（include deps.cmake →
# maque_find_windows_deps() → find_package/find_library → 链 .deps/dist），
# 编译 + 链接 + 运行都过，才算这个依赖真的可用。
function Write-SelfCheck {
    New-Item -ItemType Directory -Force -Path $SelfDir | Out-Null

    $cmake = @'
cmake_minimum_required(VERSION 3.20)
project(maque_deps_selfcheck C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../deps.cmake")
maque_find_windows_deps()
enable_testing()
set(_found "")

find_path(SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
find_library(SDL2_LIBRARY NAMES SDL2)
message(STATUS "selfcheck: SDL2_INCLUDE_DIR=${SDL2_INCLUDE_DIR}")
message(STATUS "selfcheck: SDL2_LIBRARY=${SDL2_LIBRARY}")
if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    add_executable(check_sdl2 sdl2_check.cpp)
    target_include_directories(check_sdl2 PRIVATE "${SDL2_INCLUDE_DIR}")
    target_link_libraries(check_sdl2 PRIVATE "${SDL2_LIBRARY}")
    add_test(NAME sdl2 COMMAND check_sdl2)
    list(APPEND _found sdl2)
endif()

find_package(ZLIB QUIET)
message(STATUS "selfcheck: ZLIB_FOUND=${ZLIB_FOUND} ZLIB_VERSION_STRING=${ZLIB_VERSION_STRING} ZLIB_LIBRARY=${ZLIB_LIBRARY}")
if(ZLIB_FOUND)
    add_executable(check_zlib zlib_check.c)
    target_link_libraries(check_zlib PRIVATE ZLIB::ZLIB)
    add_test(NAME zlib COMMAND check_zlib)
    list(APPEND _found zlib)
endif()

find_library(ZSTD_LIBRARY NAMES zstd)
message(STATUS "selfcheck: ZSTD_LIBRARY=${ZSTD_LIBRARY}")
if(ZSTD_LIBRARY)
    add_executable(check_zstd zstd_check.c)
    target_include_directories(check_zstd PRIVATE ${ZSTD_INCLUDE_DIR})
    target_link_libraries(check_zstd PRIVATE "${ZSTD_LIBRARY}")
    add_test(NAME zstd COMMAND check_zstd)
    list(APPEND _found zstd)
endif()

find_package(Freetype QUIET)
message(STATUS "selfcheck: Freetype_FOUND=${Freetype_FOUND} FREETYPE_LIBRARY=${FREETYPE_LIBRARY} FREETYPE_INCLUDE_DIRS=${FREETYPE_INCLUDE_DIRS}")
if(Freetype_FOUND)
    add_executable(check_freetype freetype_check.c)
    target_link_libraries(check_freetype PRIVATE Freetype::Freetype)
    set(_font "${CMAKE_CURRENT_SOURCE_DIR}/../../scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf")
    if(EXISTS "${_font}")
        add_test(NAME freetype COMMAND check_freetype "${_font}")
    else()
        add_test(NAME freetype COMMAND check_freetype)
    endif()
    list(APPEND _found freetype)
endif()

find_package(CURL QUIET)
message(STATUS "selfcheck: CURL_FOUND=${CURL_FOUND} CURL_VERSION_STRING=${CURL_VERSION_STRING} CURL_LIBRARY=${CURL_LIBRARY}")
if(CURL_FOUND)
    add_executable(check_curl curl_check.c)
    target_link_libraries(check_curl PRIVATE CURL::libcurl)
    add_test(NAME curl COMMAND check_curl)
    list(APPEND _found curl)
endif()

message(STATUS "selfcheck: LWS_FOUND=${LWS_FOUND} LWS_LIBRARIES=${LWS_LIBRARIES}")
if(LWS_FOUND)
    add_executable(check_lws lws_check.c)
    target_include_directories(check_lws PRIVATE ${LWS_INCLUDE_DIRS})
    target_link_directories(check_lws PRIVATE ${LWS_LIBRARY_DIRS})
    target_link_libraries(check_lws PRIVATE ${LWS_LIBRARIES})
    add_test(NAME lws COMMAND check_lws)
    list(APPEND _found lws)
endif()

message(STATUS "selfcheck: enabled = ${_found}")
'@
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'CMakeLists.txt'), $cmake, (New-Object System.Text.UTF8Encoding($false)))

    $sdl2 = @'
/* SDL2 self-check: compile + link + run against .deps/dist only.
 * NOTE: the signature must be main(int, char**) -- SDL_main.h renames main to
 * SDL_main on Windows and declares it `extern "C" int SDL_main(int, char**)`,
 * so a main(void) definition would become a different (C++) function. */
#include <SDL.h>
#include <stdio.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_version linked;
    SDL_GetVersion(&linked);
    if (SDL_Init(SDL_INIT_TIMER) != 0) {
        printf("FAIL sdl2: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    Uint32 t = SDL_GetTicks();
    SDL_Quit();
    printf("OK sdl2: compiled=%d.%d.%d linked=%d.%d.%d ticks=%u\n",
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL,
           linked.major, linked.minor, linked.patch, (unsigned)t);
    return (linked.major == 2) ? 0 : 1;
}
'@
    $zlib = @'
/* zlib self-check: compress2/uncompress round-trip against .deps/dist. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

int main(void) {
    const char* src = "maque zlib self-check: "
                      "the quick brown fox jumps over the lazy dog 0123456789 "
                      "the quick brown fox jumps over the lazy dog 0123456789";
    uLong srclen = (uLong)strlen(src) + 1;
    uLongf clen = compressBound(srclen);
    unsigned char* cbuf = (unsigned char*)malloc(clen);
    unsigned char* ubuf = (unsigned char*)malloc(srclen);
    if (!cbuf || !ubuf) { printf("FAIL zlib: oom\n"); return 1; }
    int rc = compress2(cbuf, &clen, (const Bytef*)src, srclen, 9);
    if (rc != Z_OK) { printf("FAIL zlib: compress2 rc=%d\n", rc); return 1; }
    uLongf ulen = srclen;
    rc = uncompress(ubuf, &ulen, cbuf, clen);
    if (rc != Z_OK) { printf("FAIL zlib: uncompress rc=%d\n", rc); return 1; }
    if (ulen != srclen || memcmp(ubuf, src, srclen) != 0) { printf("FAIL zlib: round-trip mismatch\n"); return 1; }
    printf("OK zlib: header=%s runtime=%s raw=%lu compressed=%lu crc32=%08lx\n",
           ZLIB_VERSION, zlibVersion(), (unsigned long)srclen,
           (unsigned long)clen, (unsigned long)crc32(0L, (const Bytef*)src, srclen));
    free(cbuf); free(ubuf);
    return 0;
}
'@
    $zstd = @'
/* zstd self-check: ZSTD_compress/ZSTD_decompress round-trip against .deps/dist. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

int main(void) {
    const char* src = "maque zstd self-check: builtin shader pack (zstd -19) round-trip payload";
    size_t srclen = strlen(src) + 1;
    size_t bound = ZSTD_compressBound(srclen);
    char* cbuf = (char*)malloc(bound);
    char* ubuf = (char*)malloc(srclen);
    if (!cbuf || !ubuf) { printf("FAIL zstd: oom\n"); return 1; }
    size_t clen = ZSTD_compress(cbuf, bound, src, srclen, 19);
    if (ZSTD_isError(clen)) { printf("FAIL zstd: compress: %s\n", ZSTD_getErrorName(clen)); return 1; }
    size_t ulen = ZSTD_decompress(ubuf, srclen, cbuf, clen);
    if (ZSTD_isError(ulen)) { printf("FAIL zstd: decompress: %s\n", ZSTD_getErrorName(ulen)); return 1; }
    if (ulen != srclen || memcmp(ubuf, src, srclen) != 0) { printf("FAIL zstd: round-trip mismatch\n"); return 1; }
    printf("OK zstd: header=%d runtime=%s raw=%zu compressed=%zu\n",
           ZSTD_VERSION_NUMBER, ZSTD_versionString(), srclen, clen);
    free(cbuf); free(ubuf);
    return 0;
}
'@
    $freetype = @'
/* freetype self-check: init + real glyph rasterization against .deps/dist.
 * argv[1] (optional) = a .ttf path; when given, the check also loads the face,
 * sets a char size, loads a glyph and rasterizes it (proves the real code path
 * used by the text module, not just a version print). */
#include <stdio.h>
#include <ft2build.h>
#include FT_FREETYPE_H

int main(int argc, char** argv) {
    FT_Library lib;
    FT_Error err = FT_Init_FreeType(&lib);
    if (err) { printf("FAIL freetype: FT_Init_FreeType err=%d\n", err); return 1; }
    FT_Int major = 0, minor = 0, patch = 0;
    FT_Library_Version(lib, &major, &minor, &patch);
    printf("OK freetype: header=%d.%d.%d runtime=%d.%d.%d",
           FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH, major, minor, patch);
    int rc = 0;
    if (argc > 1) {
        FT_Face face = NULL;
        err = FT_New_Face(lib, argv[1], 0, &face);
        if (err) { printf("\nFAIL freetype: FT_New_Face(%s) err=%d\n", argv[1], err); FT_Done_FreeType(lib); return 1; }
        err = FT_Set_Pixel_Sizes(face, 0, 32);
        if (err) { printf("\nFAIL freetype: FT_Set_Pixel_Sizes err=%d\n", err); rc = 1; }
        err = FT_Load_Char(face, 'A', FT_LOAD_RENDER);
        if (err) { printf("\nFAIL freetype: FT_Load_Char err=%d\n", err); rc = 1; }
        if (!rc) {
            FT_GlyphSlot g = face->glyph;
            printf(" face=%s glyphs=%ld 'A'=%dx%d pitch=%d coverage=%u",
                   face->family_name, (long)face->num_glyphs,
                   (int)g->bitmap.width, (int)g->bitmap.rows, g->bitmap.pitch,
                   (unsigned)g->bitmap.buffer[0]);
            if (g->bitmap.width == 0 || g->bitmap.rows == 0) { printf("\nFAIL freetype: empty bitmap\n"); rc = 1; }
        }
        FT_Done_Face(face);
    }
    printf("\n");
    FT_Done_FreeType(lib);
    return rc;
}
'@
    $curl = @'
/* curl self-check: static libcurl (Schannel) link + run.
 * No external network is used: the functional probe targets a closed loopback
 * port, so a clean CURLE_COULDNT_CONNECT proves the connect error path works. */
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

int main(void) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) { printf("FAIL curl: curl_global_init\n"); return 1; }
    CURL* h = curl_easy_init();
    if (!h) { printf("FAIL curl: curl_easy_init\n"); return 1; }
    curl_version_info_data* vi = curl_version_info(CURLVERSION_NOW);
    printf("OK curl: header=%s runtime=%s ssl=%s zlib=%s nghttp2=%s\n",
           LIBCURL_VERSION, curl_version(),
           vi->ssl_version ? vi->ssl_version : "(none)",
           vi->libz_version ? vi->libz_version : "(none)",
           (vi->features & CURL_VERSION_HTTP2) ? "yes" : "no");
    if (!vi->ssl_version) { printf("FAIL curl: no TLS backend compiled in\n"); return 1; }
    CURLU* u = curl_url();
    if (!u || curl_url_set(u, CURLUPART_URL, "https://cdn.example.com/assets/a%20b.scn", 0) != CURLUE_OK) {
        printf("FAIL curl: curl_url_set\n");
        return 1;
    }
    char* host = NULL; char* path = NULL;
    curl_url_get(u, CURLUPART_HOST, &host, 0);
    curl_url_get(u, CURLUPART_PATH, &path, 0);
    printf("     url parse: host=%s path=%s\n", host ? host : "?", path ? path : "?");
    curl_free(host); curl_free(path); curl_url_cleanup(u);
    curl_easy_setopt(h, CURLOPT_URL, "http://127.0.0.1:1/");
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(h);
    printf("     loopback:1 probe rc=%d (%s)\n", (int)rc, curl_easy_strerror(rc));
    curl_easy_cleanup(h);
    curl_global_cleanup();
    if (rc != CURLE_COULDNT_CONNECT && rc != CURLE_OPERATION_TIMEDOUT) {
        printf("FAIL curl: unexpected probe result\n");
        return 1;
    }
    return 0;
}
'@
    $lws = @'
/* libwebsockets self-check: link + create/serve/destroy a real context. */
#include <stdio.h>
#include <string.h>
#include <libwebsockets.h>

static int cb(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
    (void)wsi; (void)reason; (void)user; (void)in; (void)len;
    return 0;
}
static const struct lws_protocols protos[] = {
    { .name = "maque.selfcheck", .callback = cb, .per_session_data_size = 0, .rx_buffer_size = 0 },
    LWS_PROTOCOL_LIST_TERM
};
int main(void) {
    lws_set_log_level(LLL_ERR, NULL);
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protos;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    struct lws_context* ctx = lws_create_context(&info);
    if (!ctx) { printf("FAIL lws: lws_create_context\n"); return 1; }
    int rc = lws_service(ctx, 0);
    printf("OK lws: version=%s service_rc=%d\n", lws_get_library_version(), rc);
    lws_context_destroy(ctx);
    return 0;
}
'@
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'sdl2_check.cpp'), $sdl2, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'zlib_check.c'), $zlib, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'zstd_check.c'), $zstd, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'freetype_check.c'), $freetype, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'curl_check.c'), $curl, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText((Join-Path $SelfDir 'lws_check.c'), $lws, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "--- 已生成自检工程 $SelfDir ---"
}

function Invoke-SelfCheck {
    Write-Host "--- 独立自检（编译 + 链接 + 运行）---"
    $bdir = Join-Path $SelfDir 'build'
    Remove-Item -Recurse -Force $bdir -ErrorAction SilentlyContinue
    & cmake -S $SelfDir -B $bdir -G Ninja "-DCMAKE_MAKE_PROGRAM=$NinjaArg" `
        "-DCMAKE_C_COMPILER=$ClangClArg" "-DCMAKE_CXX_COMPILER=$ClangClArg" `
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL *>&1 |
        Select-String -Pattern 'selfcheck:' | ForEach-Object { Write-Host "  $($_.Line)" }
    & cmake --build $bdir --parallel *>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { Write-Host "  自检构建失败"; return $false }
    & ctest --test-dir $bdir --output-on-failure *>&1 | Select-Object -Last 10 | ForEach-Object { Write-Host "  $_" }
    $ok = ($LASTEXITCODE -eq 0)
    Write-Host ("  自检结果: " + $(if ($ok) { 'PASS' } else { 'FAIL' }))
    return $ok
}

# ---------------------------------------------------------------- 主流程
$results = [ordered]@{}
foreach ($lib in $Libraries) {
    if ($Only.Count -gt 0 -and ($Only -notcontains $lib.Name)) { continue }
    Write-Output ""
    Write-Output "=================== $($lib.Name) ==================="
    if (-not (Invoke-Download $lib)) { $results[$lib.Name] = 'fail(download)'; continue }
    if (-not (Invoke-Extract  $lib)) { $results[$lib.Name] = 'fail(extract)';  continue }
    $results[$lib.Name] = Invoke-Build $lib
}

Invoke-Fixups
Write-DepsCMake

$selfOk = $null
if (-not $SkipSelfCheck) { $selfOk = Invoke-SelfCheck }

Write-Output ""
Write-Output "=================== 汇总 ==================="
foreach ($k in $results.Keys) { Write-Output ("  {0,-10} {1}" -f $k, $results[$k]) }
if ($selfOk -ne $null) { Write-Output ("  {0,-10} {1}" -f 'selfcheck', $(if ($selfOk) { 'PASS' } else { 'FAIL' })) }
Write-Output "  prefix     $Dist"
Write-Output "  deps.cmake $(Join-Path $Deps 'deps.cmake')"
Write-Output ""
Write-Output "下一步：cmake -S `"$Repo`" -B build -G Ninja -DMAQUE_WINDOWS_DEPS=ON -DCMAKE_BUILD_TYPE=Debug"
