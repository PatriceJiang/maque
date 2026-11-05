#!/usr/bin/env bash
# Launcher for samples/tank with sound enabled.
# Engine default audio backend is null (CI/headless); tank needs SDL audio.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLAYER="${MAQUE_PLAYER:-}"
if [ -z "$PLAYER" ]; then
    for cand in "$ROOT/build/samples/player/maque_player" \
                "$ROOT/build-win/samples/player/maque_player.exe" \
                "$ROOT/build-dbg/samples/player/maque_player"; do
        if [ -x "$cand" ]; then PLAYER="$cand"; break; fi
    done
fi
if [ -z "$PLAYER" ] || [ ! -x "$PLAYER" ]; then
    echo "tank: maque_player not found; build first or set MAQUE_PLAYER" >&2
    exit 1
fi
export MAQUE_AUDIO="${MAQUE_AUDIO:-sdl}"
exec "$PLAYER" "$ROOT/samples/tank/tank.scn" "$@"
