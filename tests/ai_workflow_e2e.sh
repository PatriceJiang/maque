#!/usr/bin/env bash
# AI 标准工作流端到端（docs/11 §5 靶心序列）：
#   Scene.info → Types.describe → Runtime.step → World.snapshot/restore（回滚等价）
#   → Scene.entities / Perf.get → Control.mcp(tools/call)
# 这是"AI 能自主加载/调试/预览/采集"的最小可脚本化闭环。
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）
PLAYER="${1:?player path}"
SCENE="${2:?scene path}"
W="${3:?work dir}"; rm -rf "$W"; mkdir -p "$W"

# docs/11 §5 的 AI 标准动作序列（去掉 Instance.create/VFS.mount，单进程内完成）：
#   Scene.load → Replay.load(录制) → Replay.step → Renderer.capture → Types/Scene.dump
#   → Perf.sample / Log.tail → Replay.replay → checksum 对比
printf '%s\n' \
 '{"jsonrpc":"2.0","id":1,"method":"Scene.info"}' \
 '{"jsonrpc":"2.0","id":2,"method":"Types.describe"}' \
 '{"jsonrpc":"2.0","id":3,"method":"Runtime.frame"}' \
 '{"jsonrpc":"2.0","id":4,"method":"Runtime.step","params":{"frames":40}}' \
 '{"jsonrpc":"2.0","id":5,"method":"World.snapshot"}' \
 '{"jsonrpc":"2.0","id":6,"method":"Scene.entities"}' \
 '{"jsonrpc":"2.0","id":7,"method":"Perf.get"}' \
 '{"jsonrpc":"2.0","id":8,"method":"World.checksum"}' \
 '{"jsonrpc":"2.0","id":9,"method":"Runtime.step","params":{"frames":20}}' \
 '{"jsonrpc":"2.0","id":10,"method":"World.restore"}' \
 '{"jsonrpc":"2.0","id":11,"method":"World.checksum"}' \
 '{"jsonrpc":"2.0","id":12,"method":"Control.mcp","params":{"method":"tools/call","name":"Perf.get","arguments":{}}}' \
 | MAQUE_HEADLESS=1 MAQUE_CONTROL=- "$PLAYER" "$SCENE" > "$W/out.txt" 2>&1 \
  || { echo "ai_workflow_e2e: player failed" >&2; exit 1; }

"$PY" - "$W/out.txt" <<'PY'
import json, sys, re
lines = [l for l in open(sys.argv[1], encoding='utf-8') if l.startswith('{"jsonrpc"')]
resp = {}
for l in lines:
    d = json.loads(l)
    resp[d["id"]] = d.get("result")
ok = []
ok.append(("Scene.info", isinstance(resp.get(1), dict) and "title" in resp[1]))
ok.append(("Types.describe", isinstance(resp.get(2), list) and len(resp[2]) > 200))
ok.append(("Runtime.frame", resp.get(3, {}).get("frame") == 0))
ok.append(("Runtime.step", resp.get(4, {}).get("frame") == 40))
ok.append(("World.snapshot", resp.get(5, {}).get("ok") is True))
ok.append(("Scene.entities", isinstance(resp.get(6), list) and len(resp[6]) >= 4))
ok.append(("Perf.get", "metrics" in resp.get(7, {})))
sum40 = resp.get(8, {}).get("trail")
ok.append(("World.checksum@40", isinstance(sum40, str) and sum40 != "0"))
ok.append(("Runtime.step 20", resp.get(9, {}).get("frame") == 60))
ok.append(("World.restore", resp.get(10, {}).get("ok") is True))
sum40b = resp.get(11, {}).get("trail")
ok.append(("rollback 等价", sum40 == sum40b))
ok.append(("Control.mcp tools/call", "content" in (resp.get(12) or {})))
bad = [n for n, v in ok if not v]
for n, v in ok:
    print(("  OK   " if v else "  FAIL ") + n)
print("ai_workflow:", "ok" if not bad else f"FAILED {bad}")
sys.exit(0 if not bad else 1)
PY
