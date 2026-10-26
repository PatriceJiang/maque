#!/usr/bin/env bash
# 构建 Tracy capture CLI（Linux/macOS）—— CI / 无 GUI 抓 trace 用的 `tracy-capture`。
#
# 为什么和 get-profiler.ps1 分开：
#   · v0.11.1 官方发行包**只有 windows-*.zip**（Linux/macOS 没有预编译二进制）；
#   · capture 是纯 CLI，可以直接从源码构建，不需要 GLFW/freetype 等 GUI 依赖；
#   · GUI 的 tracy-profiler 需要桌面依赖，按需自行构建，再把路径给 `MAQUE_TRACY_PROFILER`。
#
# 产物：`.deps/tracy/tracy-capture`（`.deps/` 不入库）；`tests/tracy_e2e.sh` 会自动发现。
#
# 用法（仓库根目录）：
#   tools/tracy/build-capture.sh            # 已存在则跳过
#   tools/tracy/build-capture.sh --force    # 重新构建
#
# 依赖：curl、unzip、cmake >= 3.16、ninja、C++20 编译器。
# 版本：默认跟随 vendored 客户端（third_party/tracy/public/common/TracyVersion.hpp），
#       协议版本必须一致，否则 capture 连不上客户端。
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$ROOT/.deps/tracy"
CACHE="$DEPS/cache"
SRC="$DEPS/src"
OUT="$DEPS/tracy-capture"
HDR="$ROOT/third_party/tracy/public/common/TracyVersion.hpp"

CAPSTONE_VER="5.0.3"

FORCE=0
for arg in "$@"; do
  case "$arg" in
    --force|-f) FORCE=1 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "build-capture.sh: 未知参数 $arg" >&2; exit 2 ;;
  esac
done

[ -f "$HDR" ] || { echo "build-capture.sh: 缺 $HDR（third_party/tracy 未 vendored？）" >&2; exit 1; }
VER_NUM="$(sed -nE 's/^enum \{ (Major|Minor|Patch) = ([0-9]+) \};/\2/p' "$HDR" | paste -sd. -)"
[ -n "$VER_NUM" ] || { echo "build-capture.sh: 无法从 $HDR 解析版本" >&2; exit 1; }
VER="v$VER_NUM"

if [ "$FORCE" -eq 0 ] && [ -x "$OUT" ]; then
  echo "tracy: 已存在 $OUT（要重建加 --force）"
  exit 0
fi

for tool in curl unzip cmake ninja; do
  command -v "$tool" >/dev/null 2>&1 || { echo "tracy: 缺少 $tool（cmake/ninja 见 AGENTS.md 构建依赖）" >&2; exit 1; }
done

mkdir -p "$CACHE" "$SRC"

fetch() { # $1 = url，$2 = 目标 zip
  if [ -f "$2" ]; then return 0; fi
  echo "tracy: 下载 $1 ..."
  curl -L --fail --retry 2 -o "$2.part" "$1"
  mv "$2.part" "$2"
}

TRACY_ZIP="$CACHE/tracy-$VER_NUM.zip"
CAP_ZIP="$CACHE/capstone-$CAPSTONE_VER.zip"
fetch "https://github.com/wolfpld/tracy/archive/refs/tags/$VER.zip" "$TRACY_ZIP"
fetch "https://github.com/capstone-engine/capstone/archive/refs/tags/$CAPSTONE_VER.zip" "$CAP_ZIP"

TRACY_SRC="$SRC/tracy-$VER_NUM"
CAP_SRC="$SRC/capstone-$CAPSTONE_VER"
[ -d "$TRACY_SRC" ] || unzip -q -o "$TRACY_ZIP" -d "$SRC"
[ -d "$CAP_SRC" ]   || unzip -q -o "$CAP_ZIP" -d "$SRC"

BUILD="$TRACY_SRC/build-capture"
# CPM 默认会 `git clone` capstone / oneTBB；这里改成：
#   · CPM_capstone_SOURCE 指向已解压的发行源码 → 不 clone；
#   · NO_PARALLEL_STL=ON → 不引入 TBB（capture 不需要并行 STL 加速）。
echo "tracy: 配置 $BUILD ..."
cmake -S "$TRACY_SRC/capture" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPM_capstone_SOURCE="$CAP_SRC" \
  -DNO_STATISTICS=ON -DNO_PARALLEL_STL=ON >/dev/null

echo "tracy: 构建 tracy-capture（首次较慢，capstone 要编译一会）..."
cmake --build "$BUILD" --target tracy-capture

install -m 0755 "$BUILD/tracy-capture" "$OUT"
echo "tracy: 完成 → $OUT"
echo "  CI/命令行：$OUT -o out.tracy -a 127.0.0.1 -p 8086 -s 2"
echo "  F3 拉起 GUI 需自行构建 tracy-profiler 并设 MAQUE_TRACY_PROFILER=<path>。"
