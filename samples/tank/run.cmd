@echo off
rem Launcher for samples/tank with sound enabled (engine default audio is null).
setlocal
set "ROOT=%~dp0..\.."
if not defined MAQUE_AUDIO set "MAQUE_AUDIO=sdl"
if not defined MAQUE_PLAYER set "MAQUE_PLAYER=%ROOT%\build-win\samples\player\maque_player.exe"
if not exist "%MAQUE_PLAYER%" (
    echo tank: maque_player not found: %MAQUE_PLAYER% 1>&2
    exit /b 1
)
"%MAQUE_PLAYER%" "%ROOT%\samples\tank\tank.scn" %*
