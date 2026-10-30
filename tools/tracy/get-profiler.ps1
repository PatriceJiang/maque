# 下载 Tracy Profiler 的官方可执行文件（GUI `tracy-profiler.exe` + CLI `tracy-capture.exe`）。
#
# 为什么二进制不入库：
#   · 仓库只 vendored 客户端源码（third_party/tracy/public，见 docs/29）；
#   · profiler GUI / capture 是约 12MB 的发行包，不适合进 git；
#   · `.deps/` 已在 .gitignore 里，且 F3 会自动在 `.deps/tracy/` 找 `tracy-profiler.exe`。
#
# 用法（仓库根目录）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\tracy\get-profiler.ps1
#   powershell ... -File tools\tracy\get-profiler.ps1 -Version v0.11.1 -Force   # 重新下载
param(
    [string]$Version = 'v0.11.1',
    [switch]$Force
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$depsDir  = Join-Path $repoRoot '.deps/tracy'
$profiler = Join-Path $depsDir 'tracy-profiler.exe'
$capture  = Join-Path $depsDir 'tracy-capture.exe'

if (-not $Force -and (Test-Path $profiler) -and (Test-Path $capture)) {
    Write-Host "tracy: 已存在 $profiler / $capture（要重下加 -Force）"
    exit 0
}

if (-not ($env:OS -eq 'Windows_NT')) {
    Write-Error @"
tools/tracy/get-profiler.ps1 只处理 Windows 官方二进制（v0.11.1 发行包只提供 windows-*.zip）。
其它平台请自行从源码构建 Tracy profiler（https://github.com/wolfpld/tracy），
或把可执行文件放到 $depsDir/ 并设 MAQUE_TRACY_PROFILER=<path>。
"@
    exit 1
}

$ver    = $Version.TrimStart('v')
$asset  = "windows-$ver.zip"
$url    = "https://github.com/wolfpld/tracy/releases/download/$Version/$asset"
$zip    = Join-Path $depsDir $asset
$staged = Join-Path $depsDir "extract-$ver"

New-Item -ItemType Directory -Force $depsDir | Out-Null
if (-not (Test-Path $zip)) {
    Write-Host "tracy: 下载 $url ..."
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
}
if (Test-Path $staged) { Remove-Item -Recurse -Force $staged }
Expand-Archive -Path $zip -DestinationPath $staged -Force

# 只留本次需要的两个：profiler（F3 拉起）+ capture（CI/命令行抓 trace）
Copy-Item (Join-Path $staged 'tracy-profiler.exe') $profiler -Force
Copy-Item (Join-Path $staged 'tracy-capture.exe')  $capture  -Force
if (Test-Path (Join-Path $staged 'tracy-csvexport.exe')) {
    Copy-Item (Join-Path $staged 'tracy-csvexport.exe') (Join-Path $depsDir 'tracy-csvexport.exe') -Force
}
Remove-Item -Recurse -Force $staged
Remove-Item -Force $zip

Write-Host "tracy: 完成"
Write-Host "  profiler: $profiler"
Write-Host "  capture : $capture"
Write-Host "F3 会在 .deps/tracy/ 自动找到 profiler；也可用 MAQUE_TRACY_PROFILER 覆盖路径。"
