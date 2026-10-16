@echo off
rem 把 clang 的动态 ASan 运行时目录加进当前 cmd 会话的 PATH。
rem   用法： tools\win\env.cmd
rem 说明：开了 MAQUE_SANITIZE=address 后，把可执行文件挪到别处运行（或手工构建后从
rem       别处运行）会因找不到 clang_rt.asan_dynamic-x86_64.dll 而以 0xC0000135 失败；
rem       本脚本修那种情况。正常构建目录内运行不需要它（CMake 已把 DLL 复制到 exe 旁）。
setlocal EnableDelayedExpansion
set "ASAN_DIR="
set "LLVMROOT=%ProgramFiles%\LLVM\lib\clang"
if exist "%LLVMROOT%" (
  for /f "delims=" %%d in ('dir /b /ad /o-n "%LLVMROOT%" 2^>nul') do (
    if not defined ASAN_DIR (
      if exist "%LLVMROOT%\%%d\lib\windows\clang_rt.asan_dynamic-x86_64.dll" set "ASAN_DIR=%LLVMROOT%\%%d\lib\windows"
    )
  )
)
if not defined ASAN_DIR (
  echo env.cmd: 未找到 clang_rt.asan_dynamic-x86_64.dll（请检查 LLVM 安装） 1>&2
  exit /b 1
)
endlocal & set "PATH=%ASAN_DIR%;%PATH%"
echo ASan 运行时目录已在 PATH 最前：%ASAN_DIR%
echo （仅影响当前 cmd 会话；新开终端需重新执行）
