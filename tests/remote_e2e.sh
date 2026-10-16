#!/usr/bin/env bash
# 远程加载端到端（docs/04 §1/§4/§5）：VFS 挂载 http 目录 —— 场景 + 脚本经 HTTP 抓取。
#   ① 直接跑 http://127.0.0.1:PORT/ball.scn：远程游戏即资源（场景/脚本/资源全走 VFS）
#   ② 与本地同帧数运行 → world_trail 完全一致（来源不影响确定性）
#   ③ MAQUE_MOUNT 本地覆盖目录：后挂载优先遮蔽远程文件（远程游戏的开发迭代路径）
#   ④ 远程 404 → 明确失败（HTTP 状态码 → MQ_VFS_ERR_NOTFOUND，而不是静默）
# 仅用 127.0.0.1，不依赖外部网络。
set -u
. "$(dirname "$0")/maque_python.sh"                 # tests/ 下的脚本：本仓库内直接跑
[ -f "${MAQUE_REPO_ROOT:-}/tests/maque_python.sh" ] && . "$MAQUE_REPO_ROOT/tests/maque_python.sh"   # 经构建树路径调用时兜底（MAQUE_REPO_ROOT 可能未设，故用 :- 兜住 set -u）
PLAYER="$1"; SCENES="$2"; WORK="$3"

[ "$HAVE_PY" = 1 ] || { echo "remote_e2e: skip（无 "$PY"，无法起本地 HTTP 服务器）"; exit 77; }

rm -rf "$WORK"; mkdir -p "$WORK"
export MAQUE_HEADLESS=1

SRV=""
cleanup() { [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; return 0; }
trap cleanup EXIT
fail() { echo "remote_e2e: FAIL $*" >&2; exit 1; }

# ---- 本地 HTTP 服务器：127.0.0.1 随机端口，只服务 scenes/ 目录（沿子目录下钻）----
"$PY" - "$SCENES" "$WORK/port" <<'PY' &
import http.server, os, socket, sys
root, portfile = sys.argv[1], sys.argv[2]
os.chdir(root)
s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
with open(portfile, "w") as f:
    f.write(str(port))

class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass                                  # 静默（测试输出整洁）

http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
PY
SRV=$!
for _ in $(seq 1 100); do [ -s "$WORK/port" ] && break; sleep 0.05; done
PORT=$(cat "$WORK/port" 2>/dev/null || true)
[ -n "$PORT" ] || fail "本地 HTTP 服务器未就绪"

BASE="http://127.0.0.1:$PORT"
FRAMES=180        # MAQUE_MAX_FRAMES（第 2 个位置参数是帧输出目录，勿误用）

# ---- ① 远程场景：场景 + 脚本 + 资源全部经 VFS(http) 抓取 ----
MAQUE_MAX_FRAMES=$FRAMES timeout 60 "$PLAYER" "$BASE/ball.scn" > "$WORK/remote.log" 2>&1 || true
grep -q "script='ball.js'" "$WORK/remote.log" || fail "远程场景未解析出脚本声明"
grep -q "script backend 'js'" "$WORK/remote.log" || fail "远程脚本未装载（HTTP 抓取失败？）"
TRAIL_REMOTE=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/remote.log" | tail -1)
[ -n "$TRAIL_REMOTE" ] || fail "远程运行未产生 world_trail"

# ---- ② 本地同帧数运行：轨迹必须逐位一致（网络来源不改变模拟）----
MAQUE_MAX_FRAMES=$FRAMES timeout 60 "$PLAYER" "$SCENES/ball.scn" > "$WORK/local.log" 2>&1 || true
TRAIL_LOCAL=$(grep -o 'world_trail=0x[0-9a-f]*' "$WORK/local.log" | tail -1)
[ -n "$TRAIL_LOCAL" ] || fail "本地运行未产生 world_trail"
[ "$TRAIL_REMOTE" = "$TRAIL_LOCAL" ] || fail "远程/本地轨迹不一致（$TRAIL_REMOTE vs $TRAIL_LOCAL）"

# ---- ③ MAQUE_MOUNT：本地覆盖目录后挂载 → 遮蔽远程文件（改本地脚本即生效）----
mkdir -p "$WORK/override"
cp "$SCENES/ball.scn" "$WORK/override/"
{ echo "log('remote_e2e: local override active');"; cat "$SCENES/ball.js"; } > "$WORK/override/ball.js"
# ⚠ Windows（Git Bash / MSYS）会把**像路径**的环境变量值做 POSIX→Windows 转换：
#   MAQUE_MOUNT="/=/tmp/x" 这种"挂载点=路径"的复合串会被拆坏成
#   `mount 'C:\Program Files\Git\' <- E;C:\Program Files\Git\...`
#   → 挂载点与源全错，断言必然失败。用 MSYS_NO_PATHCONV=1 关掉该转换（Linux 上无副作用）。
MAQUE_MOUNT="/=$WORK/override" MSYS_NO_PATHCONV=1 MAQUE_MAX_FRAMES=30 timeout 60 "$PLAYER" "$BASE/ball.scn" > "$WORK/override.log" 2>&1 || true
grep -q "runtime: mount '/'" "$WORK/override.log" || fail "MAQUE_MOUNT 未生效"
grep -q "local override active" "$WORK/override.log" || fail "本地覆盖未遮蔽远程脚本"

# ---- ④ 远程 404 → 明确报错 ----
MAQUE_MAX_FRAMES=1 timeout 60 "$PLAYER" "$BASE/nope.scn" > "$WORK/404.log" 2>&1 || true
grep -q "cannot read scene" "$WORK/404.log" || fail "远程 404 未报错（应为 cannot read scene）"

TRAIL_REMOTE=${TRAIL_REMOTE#world_trail=}
echo "remote_e2e: ok (trail=$TRAIL_REMOTE frames=$FRAMES)"
