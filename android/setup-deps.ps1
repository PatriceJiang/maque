param(
    [string]$SdlTag = "release-2.32.10"
)

$ErrorActionPreference = "Stop"
$androidRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$deps = Join-Path $androidRoot ".deps"
$sdl = Join-Path $deps "SDL"

New-Item -ItemType Directory -Force $deps | Out-Null
if (Test-Path (Join-Path $sdl "CMakeLists.txt")) {
    $actual = (git -C $sdl describe --tags --exact-match 2>$null)
    if ($LASTEXITCODE -ne 0 -or $actual -ne $SdlTag) {
        throw "SDL2 checkout mismatch: expected $SdlTag at $sdl (found '$actual')"
    }
    Write-Host "SDL2 $SdlTag already present: $sdl"
    exit 0
}
if (Test-Path $sdl) {
    throw "SDL directory exists but is incomplete: $sdl"
}

git clone --depth 1 --branch $SdlTag https://github.com/libsdl-org/SDL.git $sdl
if ($LASTEXITCODE -ne 0) { throw "SDL2 clone failed" }
Write-Host "SDL2 ready: $sdl"
