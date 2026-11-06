@echo off
rem Launcher for samples/tank-native (native C++ gameplay, Lua HUD) with sound enabled.
setlocal
set "ROOT=%~dp0..\.."
if not defined MAQUE_AUDIO set "MAQUE_AUDIO=sdl"
if not defined MAQUE_PLAYER set "MAQUE_PLAYER=%ROOT%\build-win\samples\player\maque_player.exe"
if not exist "%MAQUE_PLAYER%" (
    echo tank-native: maque_player not found: %MAQUE_PLAYER% 1>&2
    exit /b 1
)
if not defined MAQUE_NATIVE_MANIFEST set "MAQUE_NATIVE_MANIFEST=%ROOT%\build-win\samples\tank-native\manifest.json"
if not exist "%MAQUE_NATIVE_MANIFEST%" (
    echo tank-native: manifest not found: %MAQUE_NATIVE_MANIFEST% 1>&2
    echo   build first: cmake --build build-win --target tank_native_game maque_player 1>&2
    exit /b 1
)
"%MAQUE_PLAYER%" "%ROOT%\samples\tank-native\tank-nativel.scn" %*
