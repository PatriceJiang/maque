#!/usr/bin/env bash
# Launcher for samples/tank-native (native C++ gameplay, Lua HUD) with sound enabled.
# Build first: cmake --build build --target tank_native_game maque_player
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLAYER="${MAQUE_PLAYER:-}"
BUILD="${MAQUE_BUILD_DIR:-}"
if [ -z "$PLAYER" ]; then
    for cand in "$ROOT/build/samples/player/maque_player" \
                "$ROOT/build-win/samples/player/maque_player.exe" \
                "$ROOT/build-dbg/samples/player/maque_player"; do
        if [ -x "$cand" ]; then PLAYER="$cand"; break; fi
    done
fi
if [ -z "$PLAYER" ] || [ ! -x "$PLAYER" ]; then
    echo "tank-native: maque_player not found; build first or set MAQUE_PLAYER" >&2
    exit 1
fi
if [ -z "$BUILD" ]; then
    # <build>/samples/player/maque_player → <build>
    BUILD="$(cd "$(dirname "$PLAYER")/../.." && pwd)"
fi
MANIFEST="${MAQUE_NATIVE_MANIFEST:-$BUILD/samples/tank-native/manifest.json}"
if [ ! -f "$MANIFEST" ]; then
    echo "tank-native: manifest not found: $MANIFEST" >&2
    echo "  build first: cmake --build build --target tank_native_game maque_player" >&2
    exit 1
fi
export MAQUE_AUDIO="${MAQUE_AUDIO:-sdl}"
export MAQUE_UI_ATLAS=1
export MAQUE_NATIVE_MANIFEST="$MANIFEST"
exec "$PLAYER" --frames 1000000 "$ROOT/samples/tank-native/tank-nativel.scn" "$@"
