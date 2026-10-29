#!/usr/bin/env sh
set -eu

SDL_TAG="${SDL_TAG:-release-2.32.10}"
ANDROID_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEPS="$ANDROID_ROOT/.deps"
SDL="$DEPS/SDL"

mkdir -p "$DEPS"
if [ -f "$SDL/CMakeLists.txt" ]; then
    actual=$(git -C "$SDL" describe --tags --exact-match 2>/dev/null || true)
    if [ "$actual" != "$SDL_TAG" ]; then
        printf "SDL2 checkout mismatch: expected %s at %s (found '%s')\n" "$SDL_TAG" "$SDL" "$actual" >&2
        exit 1
    fi
    printf 'SDL2 %s already present: %s\n' "$SDL_TAG" "$SDL"
    exit 0
fi
if [ -e "$SDL" ]; then
    printf 'SDL directory exists but is incomplete: %s\n' "$SDL" >&2
    exit 1
fi

git clone --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$SDL"
printf 'SDL2 ready: %s\n' "$SDL"
