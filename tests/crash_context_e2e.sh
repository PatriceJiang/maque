#!/usr/bin/env bash
# 崩溃/异常上下文端到端（docs/09 §5、docs/10 §7）：
#   脚本抛错 → 引擎写结构化报告（seed/帧号/检查点/重放指引）→ 校验内容并可据此重放。
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）

PLAYER="$1"
SCENE_DIR="$2"      # 场景所在目录（含 anim.scn / anim.js / fade.anim.json）
WORK="$3"

rm -rf "$WORK"
mkdir -p "$WORK/run"
cp "$SCENE_DIR/anim.scn" "$SCENE_DIR/anim.js" "$SCENE_DIR/fade.anim.json" "$WORK/run/"
export MAQUE_HEADLESS=1

fail() { echo "crash_context_e2e: FAIL $*" >&2; exit 1; }

# 故意在第 5 帧抛错的脚本（替换 run/anim.js）
cat > "$WORK/run/anim.js" <<'JS'
(function () {
  'use strict';
  var S = globalThis.__crash || (globalThis.__crash = { n: 0 });
  globalThis.on_start = function () { log('crash-e2e ready'); };
  globalThis.update = function () {
    S.n++;
    if (S.n === 5) throw new Error('intentional crash at frame 5');
    if (input_key(1) > 0) quit();
  };
})();
JS

REPORT="$WORK/crash.json"
MAQUE_CRASH_REPORT="$REPORT" MAQUE_CHECKPOINTS=1 \
  MAQUE_MAX_FRAMES=20 "$PLAYER" "$WORK/run/anim.scn" > "$WORK/out1.txt" 2>&1

[ -f "$REPORT" ] || fail "crash report not written"
grep -q "crash context" "$WORK/out1.txt" || fail "no crash context log line"

"$PY" - "$REPORT" > "$WORK/parsed.txt" <<'PY' || fail "crash report content invalid"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["reason"].startswith("Error: intentional crash"), d["reason"]
assert d["frame"] == 4, d["frame"]   # update 第 5 次调用发生在第 4 帧（首帧为 0）
assert "seed" in d and "world_trail" in d, d
assert d["replay"]["command"].startswith("MAQUE_REPLAY="), d["replay"]
assert any(e["event"] == "update_failed" for e in d["metrics"]["events"]), d["metrics"]
print(f'{d["frame"]} {d["world_trail"]}')
PY
FRAME=$(cut -d' ' -f1 < "$WORK/parsed.txt")
TRAIL=$(cut -d' ' -f2 < "$WORK/parsed.txt")
[ -n "$FRAME" ] && [ -n "$TRAIL" ] || fail "cannot parse report fields"

# 检查点暴露（分歧定位锚点）
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"Runtime.step","params":{"frames":6}}' \
              '{"jsonrpc":"2.0","id":2,"method":"Replay.checkpoints"}' > "$WORK/req.txt"
MAQUE_CHECKPOINTS=1 MAQUE_CONTROL=- "$PLAYER" "$WORK/run/anim.scn" < "$WORK/req.txt" > "$WORK/out3.txt" 2>&1
grep -q '"checkpoints"' "$WORK/out3.txt" || fail "checkpoints not exposed"

echo "crash_context_e2e: ok (frame=$FRAME trail=$TRAIL)"
exit 0
