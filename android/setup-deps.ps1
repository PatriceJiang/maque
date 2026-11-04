param(
    [string]$SdlTag = "release-2.32.10",
    [string]$Abis = $env:MAQUE_ABIS
)

$ErrorActionPreference = "Stop"

# wgpu-native tag and archive pins must match .github/workflows/android.yml (bump them together).
$wgpuNativeTag = "v29.0.1.1"
$wgpuAarch64Sha256 = "721741f1b05a20c1738166bedf7a5efb2ba4b382da689526d3fc33de22bdd573"
$wgpuX86_64Sha256 = "ef16fc0644bf0e308a39ac4516742da8e22d8c201d3a542cc5baf533d272c491"
# sha256 of lib/libwgpu_native.so inside the matching release archive.
$wgpuAarch64LibSha256 = "8fa49645ac72c470c4bc386b3f28ad48df3d2d56cd67c01e259361c30d91614b"
$wgpuX86_64LibSha256 = "be7d19afe4ee8b303d5525f2c3ba4d50f24f6e063b121b9eaed43b1a865a2843"

if (-not $Abis) { $Abis = "arm64-v8a" }

$androidRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$deps = Join-Path $androidRoot ".deps"
$sdl = Join-Path $deps "SDL"
$jniLibs = Join-Path $androidRoot "app/src/main/jniLibs"

function Get-FileSha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Fetch-Sdl {
    if (Test-Path (Join-Path $sdl "CMakeLists.txt")) {
        $actual = (git -C $sdl describe --tags --exact-match 2>$null)
        if ($LASTEXITCODE -ne 0 -or $actual -ne $SdlTag) {
            throw "SDL2 checkout mismatch: expected $SdlTag at $sdl (found '$actual')"
        }
        Write-Host "SDL2 $SdlTag already present: $sdl"
        return
    }
    if (Test-Path $sdl) {
        throw "SDL directory exists but is incomplete: $sdl"
    }

    git clone --depth 1 --branch $SdlTag https://github.com/libsdl-org/SDL.git $sdl
    if ($LASTEXITCODE -ne 0) { throw "SDL2 clone failed" }
    Write-Host "SDL2 ready: $sdl"
}

# Gradle links this ABI-specific .so into libmain.so (see modules/render/gfx/CMakeLists.txt).
# The binary is a local dependency and is not committed, so fetch the pinned release, verify it and
# drop it into jniLibs/ — otherwise :app:assembleDebug stops at validateMaqueAndroidDeps.
function Fetch-WgpuNative([string]$Abi) {
    switch ($Abi) {
        "arm64-v8a" { $archiveAbi = "aarch64"; $archiveSha = $wgpuAarch64Sha256; $libSha = $wgpuAarch64LibSha256 }
        "x86_64"    { $archiveAbi = "x86_64";  $archiveSha = $wgpuX86_64Sha256;  $libSha = $wgpuX86_64LibSha256 }
        default {
            throw "unsupported ABI for wgpu-native: $Abi (supported: arm64-v8a, x86_64)"
        }
    }

    $dest = Join-Path (Join-Path $jniLibs $Abi) "libwgpu_native.so"
    if ((Test-Path $dest) -and (Get-FileSha256 $dest) -eq $libSha) {
        Write-Host "wgpu-native $wgpuNativeTag ($Abi) already present: $dest"
        return
    }

    $archiveRoot = Join-Path $deps "wgpu"
    $zip = Join-Path $archiveRoot "wgpu-android-$archiveAbi-release.zip"
    $out = Join-Path $archiveRoot $Abi
    New-Item -ItemType Directory -Force $archiveRoot | Out-Null
    if (-not (Test-Path $zip) -or (Get-FileSha256 $zip) -ne $archiveSha) {
        $url = "https://github.com/gfx-rs/wgpu-native/releases/download/$wgpuNativeTag/wgpu-android-$archiveAbi-release.zip"
        Write-Host "fetching $url"
        & curl.exe -fsSL --retry 3 --retry-delay 2 -o $zip $url
        if ($LASTEXITCODE -ne 0) { throw "wgpu-native download failed (curl exit $LASTEXITCODE): $url" }
    }
    $actual = Get-FileSha256 $zip
    if ($actual -ne $archiveSha) {
        Remove-Item -Force $zip
        throw "wgpu-native $archiveAbi checksum mismatch: expected $archiveSha, got $actual"
    }

    Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $out | Out-Null
    Expand-Archive -LiteralPath $zip -DestinationPath $out -Force
    $lib = Join-Path (Join-Path $out "lib") "libwgpu_native.so"
    if (-not (Test-Path $lib)) {
        throw "lib/libwgpu_native.so missing from $zip"
    }
    $actual = Get-FileSha256 $lib
    if ($actual -ne $libSha) {
        throw "wgpu-native ($Abi) library checksum mismatch: expected $libSha, got $actual"
    }

    New-Item -ItemType Directory -Force (Join-Path $jniLibs $Abi) | Out-Null
    Copy-Item -Force $lib $dest
    Write-Host "wgpu-native $wgpuNativeTag ready: $dest"
}

New-Item -ItemType Directory -Force $deps | Out-Null
Fetch-Sdl
foreach ($abi in $Abis.Split(",")) {
    $trimmed = $abi.Trim()
    if ($trimmed) { Fetch-WgpuNative $trimmed }
}
