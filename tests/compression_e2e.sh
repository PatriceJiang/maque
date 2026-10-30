#!/usr/bin/env bash
# zstd/zlib script API: JS Uint8Array + Lua binary string round trips.
set -u
PLAYER="$1"; DATA="$2"; WORK="$3"
rm -rf "$WORK"; mkdir -p "$WORK"

run_one() {
    local scene="$1" log="$2"
    env MAQUE_HEADLESS=1 MAQUE_MAX_FRAMES=5 timeout 60 "$PLAYER" "$scene" >"$log" 2>&1 || true
}

run_one "$DATA/compression_js.scn" "$WORK/js.log"
run_one "$DATA/compression_lua.scn" "$WORK/lua.log"

if grep -q 'compression_e2e unavailable' "$WORK/js.log" "$WORK/lua.log"; then
    echo "compression_e2e: SKIP zstd/zlib dependency unavailable"
    exit 77
fi

grep -q 'compression_e2e ok lang=js' "$WORK/js.log" || {
    echo "compression_e2e: JS failed" >&2
    tail -80 "$WORK/js.log" >&2
    exit 1
}
grep -q 'compression_e2e ok lang=lua' "$WORK/lua.log" || {
    echo "compression_e2e: Lua failed" >&2
    tail -80 "$WORK/lua.log" >&2
    exit 1
}
if grep -q 'compression_e2e FAIL' "$WORK/js.log" "$WORK/lua.log"; then
    echo "compression_e2e: round trip mismatch" >&2
    exit 1
fi

echo "compression_e2e: ok (JS Uint8Array + Lua binary string)"
