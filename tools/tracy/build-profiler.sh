#!/usr/bin/env bash
# 构建 Tracy profiler GUI（Linux）—— F3 拉起它，看时间线 / 打开 .tracy 文件。
#
# Windows 用 tools/tracy/get-profiler.ps1 下载官方 exe；Linux 官方发行包**没有**预编译二进制，
# 这里从同一版本源码构建（协议版本必须与 vendored 客户端一致，否则连不上）。
#
# 产物：`.deps/tracy/tracy-profiler`（`.deps/` 不入库）；引擎 F3 会自动在 `.deps/tracy/` 找到，
#       也可用 `MAQUE_TRACY_PROFILER=<path>` 覆盖。
#
# 用法：
#   tools/tracy/build-profiler.sh              # 自动：Wayland 会话且依赖齐全 → Wayland 后端，否则 X11
#   tools/tracy/build-profiler.sh --wayland    # 强制 Wayland 后端（需 wayland/egl 开发包）
#   tools/tracy/build-profiler.sh --legacy     # 强制 X11 后端（GLFW；缺则自动下载源码构建）
#   tools/tracy/build-profiler.sh --force      # 已存在也重建
#
# 依赖：curl、unzip、cmake >= 3.16、ninja、C++20 编译器。
#   · Wayland：wayland-client / egl / wayland-egl / wayland-cursor / xkbcommon / wayland-protocols
#               + wayland-scanner（Ubuntu: libwayland-dev libegl1-mesa-dev libxkbcommon-dev wayland-protocols）
#   · X11：GLFW（Ubuntu: libglfw3-dev）；没有就下载 glfw 3.4 源码一起编。
#   · 文件选择框：xdg-portal（dbus-1）或 GTK3（-DGTK_FILESELECTOR=ON）。
# 打开 trace 也可用命令行 `tracy-profiler out.tracy`，或启动后 `-a 127.0.0.1 -p 8086` 直连。
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$ROOT/.deps/tracy"
CACHE="$DEPS/cache"
SRC="$DEPS/src"
OUT="$DEPS/tracy-profiler"
HDR="$ROOT/third_party/tracy/public/common/TracyVersion.hpp"

CAPSTONE_VER="5.0.3"
GLFW_VER="3.4"

FORCE=0
MODE="auto"
for arg in "$@"; do
  case "$arg" in
    --force|-f) FORCE=1 ;;
    --wayland)  MODE="wayland" ;;
    --legacy)   MODE="legacy" ;;
    -h|--help)  sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "build-profiler.sh: 未知参数 $arg" >&2; exit 2 ;;
  esac
done

[ -f "$HDR" ] || { echo "build-profiler.sh: 缺 $HDR（third_party/tracy 未 vendored？）" >&2; exit 1; }
VER_NUM="$(sed -nE 's/^enum \{ (Major|Minor|Patch) = ([0-9]+) \};/\2/p' "$HDR" | paste -sd. -)"
[ -n "$VER_NUM" ] || { echo "build-profiler.sh: 无法从 $HDR 解析版本" >&2; exit 1; }
VER="v$VER_NUM"

if [ "$FORCE" -eq 0 ] && [ -x "$OUT" ]; then
  echo "tracy: 已存在 $OUT（要重建加 --force）"
  exit 0
fi

for tool in curl unzip cmake ninja pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || { echo "tracy: 缺少 $tool" >&2; exit 1; }
done

have_wayland() {
  command -v wayland-scanner >/dev/null 2>&1 &&
  pkg-config --exists wayland-client egl wayland-egl wayland-cursor xkbcommon wayland-protocols
}

if [ "$MODE" = "auto" ]; then
  if [ -n "${WAYLAND_DISPLAY:-}" ] && have_wayland; then MODE="wayland"; else MODE="legacy"; fi
fi
if [ "$MODE" = "wayland" ] && ! have_wayland; then
  echo "tracy: Wayland 开发依赖不全（见脚本头部 apt 包名），改用 --legacy 或先装依赖" >&2
  exit 1
fi

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

# 上游 v0.11.1 的已知 UB（GCC -O2/-O3 构建必现）：TimeToStringExact() 对 INT64_MIN 取负溢出，
# 随后 PrintSmallInt0 用巨值索引数字表 → 打开 trace / 连接客户端时段错误（Debug 构建侥幸不崩）。
# 这里打一行最小修复（只在 .deps 的下载源码里，不改仓库）。
if grep -q 'ns = -_ns;' "$TRACY_SRC/server/TracyPrint.cpp"; then
  sed -i 's/ns = -_ns;/ns = uint64_t( -( _ns + 1 ) ) + 1;/' "$TRACY_SRC/server/TracyPrint.cpp"
  echo "tracy: 已打补丁 TimeToStringExact（INT64_MIN 取负 UB）"
fi
# Release 的 LTO 会放大这类跨 TU UB（且构建慢）；GUI 无热点，保守关闭 IPO。
sed -i 's/set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)/set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)/' \
  "$TRACY_SRC/cmake/config.cmake"

BUILD="$TRACY_SRC/build-profiler"
EXTRA=()
if [ "$MODE" = "legacy" ]; then
  EXTRA+=("-DLEGACY=ON")
  if ! pkg-config --exists glfw3; then
    GLFW_SRC="$SRC/glfw-$GLFW_VER"
    if [ ! -d "$GLFW_SRC" ]; then
      GLFW_ZIP="$CACHE/glfw-$GLFW_VER.zip"
      fetch "https://github.com/glfw/glfw/archive/refs/tags/$GLFW_VER.zip" "$GLFW_ZIP"
      unzip -q -o "$GLFW_ZIP" -d "$SRC"
    fi
    EXTRA+=("-DCPM_glfw_SOURCE=$GLFW_SRC")
  fi
else
  echo "tracy: GUI 后端 = Wayland"
fi

echo "tracy: 配置 $BUILD（后端 $MODE）..."
# ⚠️ 不要加 -DNO_STATISTICS=ON：那是给 capture CLI 的；GUI 需要 Worker 的统计接口。
# ⚠️ 也不要加 -DNO_FILESELECTOR=ON：v0.11.1 上游在该开关下不创建 TracyNfd、却仍链接
#    `-lTracyNfd` → 链接失败（实测）。保持默认（xdg-portal/DBus 或 GTK）。
cmake -S "$TRACY_SRC/profiler" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPM_capstone_SOURCE="$CAP_SRC" \
  -DNO_FILESELECTOR=OFF -DNO_PARALLEL_STL=ON \
  "${EXTRA[@]}" >/dev/null

echo "tracy: 构建 tracy-profiler（首次较慢）..."
cmake --build "$BUILD" --target tracy-profiler

install -m 0755 "$BUILD/tracy-profiler" "$OUT"
echo "tracy: 完成 → $OUT"
echo "  打开 trace：$OUT out.tracy"
echo "  直连客户端：$OUT -a 127.0.0.1 -p 8086（游戏里 F3 / MAQUE_TRACY=1）"
