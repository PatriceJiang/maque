#!/usr/bin/env sh
set -eu

SDL_TAG="${SDL_TAG:-release-2.32.10}"
# wgpu-native tag and archive pins must match .github/workflows/android.yml (bump them together).
WGPU_NATIVE_TAG="v29.0.1.1"
WGPU_AARCH64_SHA256="721741f1b05a20c1738166bedf7a5efb2ba4b382da689526d3fc33de22bdd573"
WGPU_X86_64_SHA256="ef16fc0644bf0e308a39ac4516742da8e22d8c201d3a542cc5baf533d272c491"
# sha256 of lib/libwgpu_native.so inside the matching release archive.
WGPU_AARCH64_LIB_SHA256="8fa49645ac72c470c4bc386b3f28ad48df3d2d56cd67c01e259361c30d91614b"
WGPU_X86_64_LIB_SHA256="be7d19afe4ee8b303d5525f2c3ba4d50f24f6e063b121b9eaed43b1a865a2843"

ANDROID_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEPS="$ANDROID_ROOT/.deps"
SDL="$DEPS/SDL"
JNI_LIBS="$ANDROID_ROOT/app/src/main/jniLibs"
ABIS="${MAQUE_ABIS:-arm64-v8a}"

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        printf 'sha256sum/shasum not found; cannot verify the wgpu-native download\n' >&2
        exit 1
    fi
}

fetch_sdl() {
    if [ -f "$SDL/CMakeLists.txt" ]; then
        actual=$(git -C "$SDL" describe --tags --exact-match 2>/dev/null || true)
        if [ "$actual" != "$SDL_TAG" ]; then
            printf "SDL2 checkout mismatch: expected %s at %s (found '%s')\n" "$SDL_TAG" "$SDL" "$actual" >&2
            exit 1
        fi
        printf 'SDL2 %s already present: %s\n' "$SDL_TAG" "$SDL"
        return 0
    fi
    if [ -e "$SDL" ]; then
        printf 'SDL directory exists but is incomplete: %s\n' "$SDL" >&2
        exit 1
    fi

    git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$SDL"
    printf 'SDL2 ready: %s\n' "$SDL"
}

# Gradle links this ABI-specific .so into libmain.so (see modules/render/gfx/CMakeLists.txt).
# The binary is a local dependency and is not committed, so fetch the pinned release, verify it and
# drop it into jniLibs/ — otherwise :app:assembleDebug stops at validateMaqueAndroidDeps.
fetch_wgpu_native() {
    abi=$1
    case "$abi" in
        arm64-v8a)
            archive_abi="aarch64"
            archive_sha=$WGPU_AARCH64_SHA256
            lib_sha=$WGPU_AARCH64_LIB_SHA256
            ;;
        x86_64)
            archive_abi="x86_64"
            archive_sha=$WGPU_X86_64_SHA256
            lib_sha=$WGPU_X86_64_LIB_SHA256
            ;;
        *)
            printf 'unsupported ABI for wgpu-native: %s (supported: arm64-v8a, x86_64)\n' "$abi" >&2
            exit 1
            ;;
    esac

    dest="$JNI_LIBS/$abi/libwgpu_native.so"
    if [ -f "$dest" ] && [ "$(sha256_file "$dest")" = "$lib_sha" ]; then
        printf 'wgpu-native %s (%s) already present: %s\n' "$WGPU_NATIVE_TAG" "$abi" "$dest"
        return 0
    fi

    zip="$DEPS/wgpu/wgpu-android-$archive_abi-release.zip"
    out="$DEPS/wgpu/$abi"
    mkdir -p "$DEPS/wgpu"
    if [ ! -f "$zip" ] || [ "$(sha256_file "$zip")" != "$archive_sha" ]; then
        url="https://github.com/gfx-rs/wgpu-native/releases/download/$WGPU_NATIVE_TAG/wgpu-android-$archive_abi-release.zip"
        printf 'fetching %s\n' "$url"
        curl -fsSL --retry 3 --retry-delay 2 "$url" -o "$zip"
    fi
    actual=$(sha256_file "$zip")
    if [ "$actual" != "$archive_sha" ]; then
        printf 'wgpu-native %s checksum mismatch: expected %s, got %s\n' "$archive_abi" "$archive_sha" "$actual" >&2
        rm -f "$zip"
        exit 1
    fi

    rm -rf "$out"
    mkdir -p "$out"
    unzip -q -o "$zip" -d "$out"
    lib="$out/lib/libwgpu_native.so"
    if [ ! -f "$lib" ]; then
        printf 'lib/libwgpu_native.so missing from %s\n' "$zip" >&2
        exit 1
    fi
    actual=$(sha256_file "$lib")
    if [ "$actual" != "$lib_sha" ]; then
        printf 'wgpu-native (%s) library checksum mismatch: expected %s, got %s\n' "$abi" "$lib_sha" "$actual" >&2
        exit 1
    fi

    mkdir -p "$JNI_LIBS/$abi"
    cp "$lib" "$dest"
    printf 'wgpu-native %s ready: %s\n' "$WGPU_NATIVE_TAG" "$dest"
}

mkdir -p "$DEPS"
fetch_sdl
for abi in $(printf '%s' "$ABIS" | tr ',' ' '); do
    [ -n "$abi" ] || continue
    fetch_wgpu_native "$abi"
done
