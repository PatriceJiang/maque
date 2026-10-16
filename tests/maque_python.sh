#!/usr/bin/env bash
# 解析一个**可用的** Python 3 解释器，供 tests/*.sh 使用。
#
# 为什么不能直接写 `python3`（Windows 实测踩坑）：
#   Windows 上 `C:\Users\<user>\AppData\Local\Microsoft\WindowsApps\python3.exe` 是
#   **Microsoft Store 占位程序**（0 字节）。不带参数执行它会弹出安装提示并**挂住**，
#   带参数则打印 "Python was not found; run without arguments to install from the
#   Microsoft Store..."。于是依赖 python3 解析 JSON 的 e2e 用例会：要么卡死、
#   要么直接失败——而真实解释器其实是 `python`（本机为 miniconda 3.11）。
#
# 用法（在脚本靠前处）：
#   . "$(dirname "$0")/maque_python.sh"
#   "$PY" -c '...' 或 "$PY" - <<'PY' ...
# 若确实找不到可用解释器，脚本应自行 `exit 77`（CTest SKIP_RETURN_CODE）或明确失败；
# 本文件只负责把 `PY` 与 `HAVE_PY` 设好，不做退出决定。
#
# 探测方式：`"$cand" -c 'pass'` —— 对 Store 占位程序会立刻失败（不挂起），
# 从而安全地筛掉它；同时 `timeout` 兜住极端情况（无 timeout 时退化为直接调用）。

PY=""
HAVE_PY=0

_maque_try_py() {
    local cand="$1"
    [ -n "$cand" ] || return 1
    command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ] || return 1
    if command -v timeout >/dev/null 2>&1; then
        timeout 10 "$cand" -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1 && return 0
    fi
    "$cand" -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1
}

# 顺序：显式覆盖 → python3 → python → py -3（Windows 启动器）
for _cand in "${MAQUE_PYTHON:-}" python3 python py; do
    if [ "$_cand" = "py" ]; then
        if command -v py >/dev/null 2>&1 && py -3 -c 'pass' >/dev/null 2>&1; then
            PY="py -3"; HAVE_PY=1; break
        fi
        continue
    fi
    if _maque_try_py "$_cand"; then PY="$_cand"; HAVE_PY=1; break; fi
done

unset -f _maque_try_py
export PY HAVE_PY
