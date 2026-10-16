# 把 MSVC 环境（INCLUDE/LIB/PATH）导入当前 PowerShell 会话。
#   clang-cl / cl.exe 不读注册表，只认环境变量；少了 INCLUDE/LIB 会卡在
#   CMake 的 "Detecting CXX compiler ABI info" 阶段（链接测试程序找不到库）。
# 用法（必须先 dot-source，否则变量不会留在当前会话）：
#   . tools/win/vcvars.ps1
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere 未找到：$vswhere" }

$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw '未找到带 VC 工具集的 Visual Studio 安装' }
$vsRoot = $vsRoot.Trim()

$arch = if ($env:MAQUE_WIN_ARCH) { $env:MAQUE_WIN_ARCH } else { 'x64' }
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars$arch.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars 未找到：$vcvars" }

# cmd 无法直接输出到 PS 变量，先落到临时文件再导入（避免管道编码问题）
$tmp = Join-Path $env:TEMP ("maque-vcvars-{0}.txt" -f $PID)
cmd.exe /c "`"$vcvars`" >nul 2>&1 && set > `"$tmp`""
if ($LASTEXITCODE -ne 0) { throw "vcvars 执行失败：$vcvars" }

foreach ($line in Get-Content $tmp) {
    $i = $line.IndexOf('=')
    if ($i -le 0) { continue }
    $name = $line.Substring(0, $i)
    $value = $line.Substring($i + 1)
    Set-Item -Path "env:$name" -Value $value
}
Remove-Item $tmp -ErrorAction SilentlyContinue

Write-Host ("vcvars: 已导入 MSVC 环境（{0}）" -f $arch)
Write-Host ("  VCToolsInstallDir = {0}" -f $env:VCToolsInstallDir)
Write-Host ("  WindowsSdkDir     = {0}" -f $env:WindowsSdkDir)
