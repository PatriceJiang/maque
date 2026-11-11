@echo off
rem Launch the racing game. Must run with the repo root as CWD:
rem the engine resolves its default font relative to the working directory,
rem and falling back to a system font costs ~20 ms/frame here.
cd /d "%~dp0..\.."
if not exist "build-win-release\samples\player\maque_player.exe" (
  echo [run] building...
  cmake --build build-win-release || goto :fail
)
build-win-release\samples\player\maque_player.exe games\race\menu.scn %*
goto :eof
:fail
echo [run] build failed
exit /b 1
