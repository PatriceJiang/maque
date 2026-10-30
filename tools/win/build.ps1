<#
.SYNOPSIS
  maque Windows 一键构建（clang-cl + Ninja，可选 ASan）。

.DESCRIPTION
  为什么需要这个脚本：
    1. 必须用**绝对路径**指定 clang-cl。若先跑 vcvars64.bat，PATH 会被指向 VS 自带的
       旧版 clang-cl（19.x），并导致 CMake 的 "Detecting CXX compiler ABI info" 永久挂起。
       故本脚本**不加载 vcvars**，让 CMake 自行定位 MSVC/Windows SDK。
    2. ASan 用的是 clang 自带的**动态**运行时（clang_rt.asan_dynamic-x86_64.dll）。
       CMake 侧已把它复制到各可执行文件旁，故运行测试无需手动改 PATH。

.EXAMPLE
  # 默认：Debug + ASan，配置并构建
  pwsh -File tools/win/build.ps1
  # 或（推荐，绕开 PowerShell 执行策略）
  tools\win\build.cmd

.EXAMPLE
  # 不带 sanitizer 的 Release 构建 + 跑测试
  pwsh -File tools/win/build.ps1 -BuildType Release -NoSanitize -RunTests

.EXAMPLE
  # 只跑测试
  pwsh -File tools/win/build.ps1 -RunTests -NoBuild
#>
[CmdletBinding()]
param(
    [string]   $BuildDir   = 'build-win',
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]   $BuildType  = 'Debug',
    # 空字符串 = 关闭 sanitizer；Windows 仅支持 address。
    # 从 build.cmd 调用时空参数会被 powershell.exe 吞掉，优先使用 -NoSanitize。
    [string]   $Sanitize   = 'address',
    [switch]   $NoSanitize,
    [switch]   $RunTests,
    [switch]   $NoConfigure,
    [switch]   $NoBuild,
    [string[]] $CTestArgs  = @('--output-on-failure')
)

$ErrorActionPreference = 'Stop'
if ($NoSanitize) { $Sanitize = '' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $repo

function Find-Tool([string]$name, [string[]]$candidates) {
    foreach ($c in $candidates) { if ($c -and (Test-Path $c)) { return $c } }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "找不到 $name（试过：$($candidates -join ', ')）"
}

$clangCl = Find-Tool 'clang-cl' @(
    "$env:ProgramFiles\LLVM\bin\clang-cl.exe",
    "${env:ProgramFiles(x86)}\LLVM\bin\clang-cl.exe"
)
$ninja = Find-Tool 'ninja' @(
    "$env:ProgramFiles\CMake\bin\ninja.exe",
    "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe"
)

Write-Host "maque Windows 构建" -ForegroundColor Cyan
Write-Host "  仓库      : $repo"
Write-Host "  clang-cl  : $clangCl"
Write-Host "  ninja     : $ninja"
Write-Host "  构建目录  : $BuildDir   ($BuildType)"

# 依赖预置（.deps/deps.cmake 由 tools/win/fetch-deps.ps1 生成；根 CMakeLists 会自动 include）
$depsCmake = Join-Path $repo '.deps\deps.cmake'
if (-not (Test-Path $depsCmake)) {
    Write-Host "  依赖      : 未找到 .deps/deps.cmake —— SDL2/curl/websockets 等将按降级路径关闭" -ForegroundColor Yellow
    Write-Host "              如需完整能力，先跑：pwsh -File tools/win/fetch-deps.ps1" -ForegroundColor Yellow
} else {
    Write-Host "  依赖      : $depsCmake"
}

$cmakeArgs = @(
    '-S', '.', '-B', $BuildDir, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DCMAKE_C_COMPILER=$clangCl",
    "-DCMAKE_CXX_COMPILER=$clangCl",
    "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DMAQUE_SANITIZE=$Sanitize"
)

if (-not $NoConfigure) {
    Write-Host "`n[1/3] 配置…" -ForegroundColor Cyan
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（exit $LASTEXITCODE）" }
} else {
    Write-Host "`n[1/3] 配置… 跳过（-NoConfigure）" -ForegroundColor DarkGray
}

if (-not $NoBuild) {
    Write-Host "`n[2/3] 构建…" -ForegroundColor Cyan
    & cmake --build $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "构建失败（exit $LASTEXITCODE）" }
} else {
    Write-Host "`n[2/3] 构建… 跳过（-NoBuild）" -ForegroundColor DarkGray
}

if ($RunTests) {
    Write-Host "`n[3/3] 测试…" -ForegroundColor Cyan
    # ASan（Windows 用 clang 自带的**动态**运行时）：clang_rt.asan_dynamic-x86_64.dll 不在 PATH 上，
    # 不加这一段所有可执行文件都会以 0xC0000135（DLL not found）失败。
    # CMake 侧已把该目录写进每个测试的 ENVIRONMENT（TEST_INCLUDE_FILES），
    # 这里同步一份是为了手工跑 exe / 排查时行为一致。
    if ($Sanitize) {
        $rtDir = $null
        try {
            $rd = (& $clangCl -print-resource-dir 2>$null | Select-Object -First 1).Trim()
            if ($rd) {
                $cand = Join-Path $rd 'lib\windows'
                if (Test-Path (Join-Path $cand 'clang_rt.asan_dynamic-x86_64.dll')) { $rtDir = $cand }
            }
        } catch { }
        if ($rtDir) {
            $env:PATH = "$rtDir;$env:PATH"
            Write-Host "  ASan 运行时: $rtDir" -ForegroundColor DarkGray
        } else {
            Write-Host "  警告：未找到 ASan 运行时 DLL 目录，测试可能以 0xC0000135 失败" -ForegroundColor Yellow
        }
    }
    Push-Location $BuildDir
    try {
        & ctest @CTestArgs
        $rc = $LASTEXITCODE
    } finally { Pop-Location }
    if ($rc -ne 0) { throw "测试失败（ctest exit $rc）" }
} else {
    Write-Host "`n[3/3] 测试… 跳过（加 -RunTests 运行）" -ForegroundColor DarkGray
}

Write-Host "`n完成。" -ForegroundColor Green
