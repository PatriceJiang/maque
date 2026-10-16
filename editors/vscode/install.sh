#!/usr/bin/env bash
# 把本扩展软链到 VSCode 扩展目录（远端 server 优先；改代码即时生效）。
# 用法：editors/vscode/install.sh [--uninstall]
set -eu
SRC="$(cd "$(dirname "$0")" && pwd)"
NAME="maque-scn-0.1.0"
UNINSTALL=""
[ "${1:-}" = "--uninstall" ] && UNINSTALL=1

found=0
for DIR in "$HOME/.vscode-server/extensions" "$HOME/.vscode/extensions"; do
    [ -d "$DIR" ] || continue
    found=1
    if [ -n "$UNINSTALL" ]; then
        rm -rf "$DIR/$NAME" && echo "removed: $DIR/$NAME"
    else
        ln -sfn "$SRC" "$DIR/$NAME" && echo "installed: $DIR/$NAME → $SRC"
    fi
done
[ "$found" = 1 ] || { echo "未找到 VSCode 扩展目录（~/.vscode-server/extensions 或 ~/.vscode/extensions）" >&2; exit 1; }
[ -n "$UNINSTALL" ] || echo "在 VSCode 里执行 “Developer: Reload Window” 后生效。"
