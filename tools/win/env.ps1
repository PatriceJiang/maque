# 把 clang 的动态 ASan 运行时目录加进当前会话的 PATH（**只做 PATH，无其它副作用**）。
#
# 用法（三种任选，都不需要 .ps1 的执行策略放行）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\env.ps1   # 只在子进程生效，一般不是你要的
#   tools\win\env.cmd                                                       # cmd 会话里最省事
#   . .\tools\win\env.ps1                                                   # PowerShell 当前会话（需放行脚本）
#
# 背景：开了 MAQUE_SANITIZE=address 后，插桩过的可执行文件需要
#   clang_rt.asan_dynamic-x86_64.dll，而 Windows 只会在 ① exe 所在目录 ② PATH ③ 系统目录
#   里找（**不会**去调用者目录找）。正常构建后 CMake 已把 DLL 复制到各 exe 旁，所以
#   `ctest` / 直接跑构建目录里的 exe 都没问题；但把 exe 挪到别处、或手工构建后从别处运行，
#   就会以 0xC0000135 / -1073741515 失败 —— 用本脚本把 DLL 目录加进 PATH 即可。
#
# 输出：把探测到的目录写进 $env:ASAN_RUNTIME_DIR，并在 PATH 最前插入它。
$ErrorActionPreference = 'Stop'

$llvmRoot = Join-Path $env:ProgramFiles 'LLVM\lib\clang'
$dllName  = 'clang_rt.asan_dynamic-x86_64.dll'
$asanDir  = $null

if (Test-Path $llvmRoot) {
    # 取版本号最大的那个 lib\clang\<ver>\lib\windows
    $dirs = Get-ChildItem -Path $llvmRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending
    foreach ($d in $dirs) {
        $cand = Join-Path $d.FullName 'lib\windows'
        if (Test-Path (Join-Path $cand $dllName)) { $asanDir = $cand; break }
    }
}

if (-not $asanDir) {
    Write-Host "env.ps1: 在 $llvmRoot 下未找到 $dllName" -ForegroundColor Red
    Write-Host "         请确认已安装 LLVM（winget install LLVM.LLVM）" -ForegroundColor Red
    return
}

if ($env:PATH -split ';' -notcontains $asanDir) { $env:PATH = "$asanDir;$env:PATH" }
$env:ASAN_RUNTIME_DIR = $asanDir
Write-Host "ASan 运行时目录已在 PATH 最前：$asanDir" -ForegroundColor Green
Write-Host "（仅影响当前会话；新开终端需重新执行）" -ForegroundColor DarkGray
