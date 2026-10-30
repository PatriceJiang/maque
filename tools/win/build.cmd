@echo off
rem maque Windows 构建入口（.cmd 包装：绕开 PowerShell 执行策略对 .ps1 的签名要求）
rem   用法： tools\win\build.cmd [传给 build.ps1 的参数]
rem   例：   tools\win\build.cmd -RunTests
rem          tools\win\build.cmd -BuildType Release -NoSanitize -RunTests
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
