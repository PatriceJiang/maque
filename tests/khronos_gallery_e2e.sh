#!/usr/bin/env bash
# Khronos 画廊端到端（scenes/khronos/index.scn + gallery.js）：
#   入口 149 个模型标题的分页 list → 点条目进入 demo（3D 模型场景）→ 上一个/下一个在排序列表内
#   循环（首项 prev → 末项、末项 next → 首项）→ 返回列表（列表重建）→ 翻页改 page_label。
#   同时抓"入口帧 / demo 帧"两张截图（Capture.screen 分块 base64 → PPM→PNG）做像素级断言：
#   两图显著不同、demo 画面非背景占比明显、入口图有文字像素、尺寸 = 设计分辨率 1280x720。
#
# 驱动方式（确定性的帧推进，参考 tests/ui_e2e.sh）：
#   MAQUE_HEADLESS=1 MAQUE_CONTROL=-  → stdin 逐行 JSON-RPC，不进主循环，由 Runtime.step 推进；
#   MAQUE_INSPECT=<随机端口>           → 引擎每帧保留最近一帧像素（Capture.screen 才可用；不必真连）。
#   MAQUE_WINDOW=0x0 MAQUE_PRESENT=off → 软件渲染离屏路径（无显示环境可跑）。
#
# 用法（两种参数形式都支持，便于手工跑与 CTest 注册）：
#   khronos_gallery_e2e.sh <maque_player> <work-dir>
#   khronos_gallery_e2e.sh <maque_player> <repo-root> <work-dir>
set -u

PLAYER="${1:-}"
ARG2="${2:-}"
ARG3="${3:-}"
if [ -z "$PLAYER" ] || [ -z "$ARG2" ]; then
    echo "usage: khronos_gallery_e2e.sh <maque_player> [<repo-root>] <work-dir>" >&2
    exit 2
fi
SELF_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "$ARG3" ]; then
    ROOT="$ARG2"
    WORK="$ARG3"
else
    ROOT="$SELF_ROOT"
    WORK="$ARG2"
fi
[ -d "$ROOT/scenes/khronos" ] || { echo "khronos_gallery_e2e: FAIL 仓库根不对: $ROOT" >&2; exit 1; }
[ -x "$PLAYER" ] || [ -f "$PLAYER" ] || { echo "khronos_gallery_e2e: FAIL 找不到 player: $PLAYER" >&2; exit 1; }
# work-dir 会被 rm -rf：拒绝空/根/仓库根本身等危险值（CTest 传的是 build 下的独立目录）
case "$WORK" in
    ""|"/"|"."|"..") echo "khronos_gallery_e2e: FAIL 非法 work-dir: '$WORK'" >&2; exit 1 ;;
esac
[ "$WORK" != "$ROOT" ] || { echo "khronos_gallery_e2e: FAIL work-dir 不能等于仓库根" >&2; exit 1; }

. "$(dirname "$0")/maque_python.sh"
if [ "${HAVE_PY:-0}" != "1" ]; then
    echo "khronos_gallery_e2e: skip（无可用 python3 解析 JSON；见 tests/maque_python.sh）" >&2
    exit 77
fi

# 内容依赖：画廊内容由 scenes/khronos/index.scn + gallery.js 提供；缺失时 SKIP（不阻塞构建）。
if [ ! -f "$ROOT/scenes/khronos/index.scn" ] || [ ! -f "$ROOT/scenes/khronos/gallery.js" ]; then
    echo "khronos_gallery_e2e: skip（缺 scenes/khronos/index.scn 或 gallery.js）" >&2
    exit 77
fi

rm -rf "$WORK"
mkdir -p "$WORK"

fail() { echo "khronos_gallery_e2e: FAIL $*" >&2; exit 1; }

"$PY" - "$PLAYER" "$ROOT" "$WORK" <<'PY'
# -*- coding: utf-8 -*-
"""Khronos 画廊 e2e 驱动（由 khronos_gallery_e2e.sh 以 heredoc 调用）。

只通过控制面（stdin JSON-RPC + Runtime.step 确定性推进）驱动，不依赖真实窗口/鼠标：
  Scene.info / Runtime.step / Scene.entities / Scene.get / Input.inject / Capture.screen
输入坐标 = 世界坐标（y 向上）；命中矩形来自 Scene.get 读组件字段，不硬编码布局。
"""
import base64
import json
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time

PLAYER, ROOT, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(WORK, exist_ok=True)
SCENE_REL = "scenes/khronos/index.scn"
ENTRY_TITLE = "Khronos glTF Sample Assets"

# 控制台/管道编码因机器而异（Windows 中文控制台是 GBK）→ 统一 UTF-8 输出，避免诊断信息里的
# 非 ASCII 箭头/符号让测试在"只差一行日志"时崩掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

TOTAL_TIMEOUT = 240.0          # 子进程总超时（秒）：超时 kill + 失败（真机要装载 5 个 3D 模型场景）
REQ_TIMEOUT = 120.0            # 单条请求超时（大场景 Runtime.step / 模型场景装载可能偏慢）
PROBE_TIMEOUT = 5.0            # 首次读取等待；不 flush 的旧构建在此之后用"挤缓冲"请求兜底

_deadline = time.time() + TOTAL_TIMEOUT
_lines = []                    # player 的全部 stdout/stderr（诊断用；reader 线程追加）


def out(msg):
    print("khronos_gallery_e2e: " + msg, flush=True)


def fail(msg):
    print("khronos_gallery_e2e: FAIL " + msg, file=sys.stderr)
    tail = "".join(_lines[-10:])[-3000:]
    if tail:
        print("---- player 输出末尾 ----\n" + tail, file=sys.stderr)
    sys.stderr.flush()
    raise SystemExit(1)


def remaining():
    return _deadline - time.time()


def free_port():
    """挑一个空闲回环端口：MAQUE_INSPECT 只需绑定成功（让引擎保留最近一帧）。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


# ---- 期望的模型列表：优先解析 gallery.js 的 MODELS 块（内容真值），退化到目录名码点序 ----
def load_models():
    js_path = os.path.join(ROOT, "scenes", "khronos", "gallery.js")
    try:
        js = open(js_path, encoding="utf-8").read()
    except OSError:
        js = ""
    m = re.search(r"/\*\s*MODELS:BEGIN.*?\*/\s*(?:const|var|let)\s+MODELS\s*=\s*\[(.*?)\]\s*;",
                  js, re.S)
    names = re.findall(r'"([^"\\]+)"', m.group(1)) if m else []
    if not names:
        base = os.path.join(ROOT, "scenes", "khronos")
        names = sorted(d for d in os.listdir(base)
                       if os.path.isfile(os.path.join(base, d, d + ".scn")))
    per_page = 20
    pm = re.search(r"\bPER_PAGE\s*=\s*(\d+)", js)
    if pm:
        per_page = max(1, int(pm.group(1)))
    return names, per_page


MODELS, PER_PAGE = load_models()
TOTAL = len(MODELS)
PAGES = max(1, (TOTAL + PER_PAGE - 1) // PER_PAGE)


# ---- 控制面客户端（stdin 逐行 JSON-RPC，按 id 匹配响应）----
class Control:
    def __init__(self, player, root, work):
        self.work = work
        self.log_path = os.path.join(work, "player.log")
        self.logf = open(self.log_path, "w", encoding="utf-8")
        self.q = queue.Queue()
        self.pending = {}
        self.uid = 0

        env = dict(os.environ)
        # 清掉可能从外部环境继承的引擎开关（CI/IDE 里 MAQUE_SHOT/MAQUE_SIM 之类会改写行为）
        for k in list(env):
            if k.startswith("MAQUE_"):
                del env[k]
        env.update({
            "MAQUE_HEADLESS": "1",
            "MAQUE_CONTROL": "-",
            "MAQUE_INSPECT": str(free_port()),   # 仅让引擎保留最近一帧（Capture.screen）
            "MAQUE_PRESENT": "off",
            "MAQUE_WINDOW": "0x0",
            "MAQUE_PACE": "0",
            "MAQUE_STORAGE": "mem://",           # 测试不落盘（默认 file://<场景目录>/.maque）
        })
        try:
            self.proc = subprocess.Popen(
                [player, SCENE_REL], cwd=root, env=env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, encoding="utf-8", errors="replace", bufsize=1)
        except OSError as e:
            fail("player 启动失败: %s" % e)

        def reader():
            try:
                for line in self.proc.stdout:
                    _lines.append(line)
                    try:
                        self.logf.write(line)
                        self.logf.flush()
                    except Exception:
                        pass
                    self.q.put(line)
            finally:
                self.q.put(None)

        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()

        # 进程级看门狗：总超时到了就 kill，让阻塞的读立刻返回 EOF
        def watchdog():
            while remaining() > 0:
                if self.proc.poll() is not None:
                    return
                time.sleep(0.2)
            try:
                self.proc.kill()
            except Exception:
                pass

        threading.Thread(target=watchdog, daemon=True).start()

    def _send(self, rid, method, params):
        req = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            req["params"] = params
        try:
            self.proc.stdin.write(json.dumps(req) + "\n")
            self.proc.stdin.flush()
        except (OSError, ValueError):
            return False
        return True

    def _wait(self, rid, timeout):
        deadline = time.time() + max(0.1, min(timeout, max(0.1, remaining())))
        while True:
            if rid in self.pending:
                return self.pending.pop(rid)
            left = deadline - time.time()
            if left <= 0:
                return None
            try:
                line = self.q.get(timeout=left)
            except queue.Empty:
                return None
            if line is None:
                return "EOF"
            s = line.strip()
            if not s.startswith("{"):
                continue                       # 脚本日志/引擎日志行（非 JSON-RPC 响应）
            try:
                obj = json.loads(s)
            except ValueError:
                continue
            if not isinstance(obj, dict):
                continue
            if "id" in obj:
                if obj["id"] == rid:
                    return obj
                self.pending[obj["id"]] = obj

    def request(self, method, params=None, timeout=REQ_TIMEOUT):
        if remaining() <= 0:
            fail("总超时 %.0fs（卡在 %s）" % (TOTAL_TIMEOUT, method))
        self.uid += 1
        rid = self.uid
        if not self._send(rid, method, params):
            fail("写 stdin 失败（%s，player 已退出？rc=%r）" % (method, self.proc.poll()))
        r = self._wait(rid, min(PROBE_TIMEOUT, timeout))
        if r is None:
            # 兼容旧构建（stdin 控制面每条响应后不 flush）：stdout 接管道时是块缓冲，
            # 需要写出超过缓冲大小的内容才会刷出。发一条**大响应**请求（Types.describe ~15KB）
            # 把已缓冲的响应挤出来。当前引擎每条响应后 fflush，正常路径不会走到这里。
            end = time.time() + timeout
            while r is None and time.time() < end and remaining() > 0:
                self.uid += 1
                self._send(self.uid, "Types.describe", None)
                r = self._wait(rid, 1.0)
        if r is None:
            fail("请求超时（%s）" % method)
        if r == "EOF":
            fail("player 提前退出（%s，rc=%r）" % (method, self.proc.poll()))
        if "error" in r:
            fail("%s 返回错误: %s" % (method, json.dumps(r["error"], ensure_ascii=False)))
        return r.get("result")

    def close(self, kill=False):
        try:
            if kill:
                self.proc.kill()
            else:
                self.proc.stdin.close()
                try:
                    self.proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=30)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        try:
            self.logf.close()
        except Exception:
            pass


# ---- 高层动作 ----
def info(ctl):
    r = ctl.request("Scene.info")
    if not isinstance(r, dict) or "title" not in r:
        fail("Scene.info 返回异常: %r" % (r,))
    return r


def title(ctl):
    return info(ctl)["title"]


def step(ctl, n=1):
    r = ctl.request("Runtime.step", {"frames": n})
    if not isinstance(r, dict):
        fail("Runtime.step 返回异常: %r" % (r,))
    return r


def entities(ctl):
    r = ctl.request("Scene.entities")
    return r if isinstance(r, list) else []


def names(ctl):
    return [e.get("name", "") for e in entities(ctl)]


def entity(ctl, name):
    for e in entities(ctl):
        if e.get("name") == name:
            return e
    return None


def require_entity(ctl, name):
    e = entity(ctl, name)
    if e is None:
        fail("找不到实体 '%s'；当前实体: %s" % (name, ", ".join(names(ctl))[:500]))
    return e


def get_value(ctl, name, path):
    """Scene.get 读字段；实体/字段不存在返回 None（不失败，便于探测组件类型）。"""
    e = entity(ctl, name)
    if e is None:
        return None
    r = ctl.request("Scene.get", {"id": e["id"], "path": path})
    if isinstance(r, dict) and r.get("ok"):
        return r.get("value")
    return None


def get_text(ctl, name):
    for path in ("scn.Label.text", "ui.TextLabel.text", "render.RichText.text"):
        v = get_value(ctl, name, path)
        if isinstance(v, str):
            return v
    fail("实体 '%s' 不是可读文本（scn.Label/ui.TextLabel/render.RichText）" % name)


def button_rect(ctl, name):
    """Button 命中矩形：组件 x/y = 左下角（世界 y 向上），width/height = 尺寸。"""
    vals = {}
    for f in ("x", "y", "width", "height"):
        v = get_value(ctl, name, "scn.Button." + f)
        if not isinstance(v, (int, float)):
            fail("实体 '%s' 没有 scn.Button.%s（不是 Button？）" % (name, f))
        vals[f] = float(v)
    return vals["x"], vals["y"], vals["width"], vals["height"]


def inject(ctl, typ, x, y):
    ctl.request("Input.inject", {"type": typ, "x": x, "y": y,
                                 "button": 0, "pointer_type": 0})


def click(ctl, name):
    """真实输入三连（move → down → up，世界坐标；同帧内按下/抬起合成 click）。"""
    x, y, w, h = button_rect(ctl, name)
    cx, cy = x + w / 2.0, y + h / 2.0
    inject(ctl, 2, cx, cy)
    step(ctl, 1)
    inject(ctl, 0, cx, cy)
    step(ctl, 1)
    inject(ctl, 1, cx, cy)
    step(ctl, 3)


def wait_title(ctl, expected, frames=20):
    t = title(ctl)
    for _ in range(frames):
        if t == expected:
            return t
        step(ctl, 1)
        t = title(ctl)
    fail("等不到标题 '%s'（当前 '%s'）" % (expected, t))


def wait_entity(ctl, name, frames=20):
    e = entity(ctl, name)
    for _ in range(frames):
        if e is not None:
            return e
        step(ctl, 1)
        e = entity(ctl, name)
    fail("等不到实体 '%s'（当前实体: %s）" % (name, ", ".join(names(ctl))[:500]))


def wait_text(ctl, name, expect=None, not_equal=None, frames=20):
    t = get_text(ctl, name) if entity(ctl, name) is not None else None
    for _ in range(frames):
        if t is not None:
            if expect is not None and t == expect:
                return t
            if not_equal is not None and t != not_equal:
                return t
        step(ctl, 1)
        t = get_text(ctl, name) if entity(ctl, name) is not None else None
    fail("等不到 '%s' 的目标文本（expect=%r not_equal=%r，实际 %r）"
         % (name, expect, not_equal, t))


def capture(ctl, tag):
    """Capture.screen 分块拼接 → PPM；存 WORK/<tag>.ppm（有 PIL 再存 .png）。"""
    chunks = []
    offset = 0
    total = 0
    w = h = 0
    while True:
        r = ctl.request("Capture.screen", {"offset": offset, "max": 48 * 1024, "max_dim": 0})
        if not isinstance(r, dict) or not r.get("ok"):
            fail("Capture.screen 失败（%s）: %r —— MAQUE_INSPECT 端点可能没建起来" % (tag, r))
        w, h, total = int(r["w"]), int(r["h"]), int(r["total"])
        size = int(r["size"])
        if size <= 0:
            fail("Capture.screen 分块 size=0（offset=%d total=%d）" % (offset, total))
        chunks.append(base64.b64decode(r["data"]))
        offset += size
        if offset >= total:
            break
    ppm = b"".join(chunks)
    if len(ppm) != total:
        fail("Capture.screen 拼接大小不符：got=%d total=%d（%s）" % (len(ppm), total, tag))
    if not ppm.startswith(b"P6\n"):
        fail("Capture.screen 不是 PPM(P6)（%s）: %r" % (tag, ppm[:8]))
    head = ppm.split(b"\n", 3)
    if len(head) != 4:
        fail("PPM 头解析失败（%s）" % tag)
    pw, ph = [int(v) for v in head[1].split()]
    if head[2].strip() != b"255":
        fail("PPM 最大色阶不是 255（%s）" % tag)
    if (pw, ph) != (w, h):
        fail("PPM 尺寸(%d,%d) 与控制面返回(%d,%d) 不一致（%s）" % (pw, ph, w, h, tag))
    rgb = head[3]
    if len(rgb) != pw * ph * 3:
        fail("PPM 像素数据长度不符（%s）: %d != %d" % (tag, len(rgb), pw * ph * 3))
    ppm_path = os.path.join(WORK, tag + ".ppm")
    with open(ppm_path, "wb") as f:
        f.write(ppm)
    png_path = ""
    try:
        from PIL import Image  # noqa: F401  (可选依赖)
        img = Image.frombytes("RGB", (pw, ph), rgb)
        png_path = os.path.join(WORK, tag + ".png")
        img.save(png_path)
    except Exception:
        pass                                  # 无 PIL 时保留 PPM（测试不依赖 PIL）
    out("截图 %s: %dx%d → %s%s" % (tag, pw, ph, ppm_path,
                                  (" / " + png_path) if png_path else "（无 PIL，仅 PPM）"))
    return pw, ph, rgb


def image_stats(w, h, rgb, step_px=2):
    """亮度/非背景占比/颜色数（抽样统计，够用且快）。bg = 抽样里出现最多的颜色。"""
    from collections import Counter
    n = w * h
    samples = [rgb[i * 3:i * 3 + 3] for i in range(0, n, step_px)]
    cnt = Counter(samples)
    bg = cnt.most_common(1)[0][0] if cnt else b"\x00\x00\x00"
    nonbg = sum(1 for p in samples if p != bg)
    bright = sum(1 for p in samples if p[0] + p[1] + p[2] >= 384)   # 平均通道 ≥128
    return {
        "w": w, "h": h, "bg": tuple(bg), "colors": len(cnt),
        "nonbg": nonbg / float(len(samples)),
        "bright": bright / float(len(samples)),
    }


def mean_abs_diff(w, h, a, b, step_px=2):
    n = w * h
    total = 0
    count = 0
    for i in range(0, n, step_px):
        o = i * 3
        total += abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1]) + abs(a[o + 2] - b[o + 2])
        count += 3
    return total / float(count)


def center_stats(w, h, rgb):
    """画面中央区域（排除上下 demo 操作栏）的非背景占比 + 亮度占比。"""
    x0, x1 = int(w * 0.08), int(w * 0.92)
    y0, y1 = int(h * 0.22), int(h * 0.78)
    from collections import Counter
    samples = []
    for y in range(y0, y1, 2):
        base = y * w
        for x in range(x0, x1, 2):
            i = (base + x) * 3
            samples.append(rgb[i:i + 3])
    cnt = Counter(samples)
    bg = cnt.most_common(1)[0][0]
    nonbg = sum(1 for p in samples if p != bg) / float(len(samples))
    bright = sum(1 for p in samples if p[0] + p[1] + p[2] >= 384) / float(len(samples))
    return nonbg, bright


def check(cond, msg):
    if not cond:
        fail(msg)


# ---- 断言流程 ----
def run(ctl):
    check(TOTAL > 0, "gallery.js MODELS 为空 / 无法解析")
    check(TOTAL == 149, "MODELS 应含 149 个模型（实际 %d）" % TOTAL)
    check(PAGES >= 2, "149 个模型应分多页（PER_PAGE=%d → %d 页）" % (PER_PAGE, PAGES))
    missing = [m for m in MODELS if not os.path.isfile(
        os.path.join(ROOT, "scenes", "khronos", m, m + ".scn"))]
    check(not missing, "MODELS 里有 %d 个模型找不到场景文件: %s" % (len(missing), missing[:5]))

    # ---- ① 入口：标题 + list/分页控件存在 ----
    t = title(ctl)
    check(t == ENTRY_TITLE, "入口标题应为 '%s'（实际 '%s'）" % (ENTRY_TITLE, t))
    step(ctl, 2)
    for name in ("gallery_title", "item_0", "page_next", "page_prev", "page_label"):
        require_entity(ctl, name)
    gt = get_text(ctl, "gallery_title")
    check(ENTRY_TITLE in gt, "gallery_title 文本应含入口标题（实际 %r）" % gt)
    check(str(TOTAL) in gt, "gallery_title 应显示模型总数 %d（实际 %r）" % (TOTAL, gt))
    label1 = get_text(ctl, "page_label")
    check(label1 == "1 / %d" % PAGES, "首页 page_label 应为 '1 / %d'（实际 %r）" % (PAGES, label1))
    item_idx = sorted(int(m.group(1)) for m in
                      (re.match(r"^item_(\d+)$", n) for n in names(ctl)) if m)
    page_items = min(PER_PAGE, TOTAL)
    check(item_idx == list(range(page_items)),
          "首页应创建 item_0..item_%d（实际 %s）" % (page_items - 1, item_idx[:25]))
    out("入口 OK：'%s' · %d models / %d 页（首页 %d 项）" % (t, TOTAL, PAGES, page_items))

    # ---- ② 翻页：page_next 改页码文本，page_prev 回到原文本；再测越界循环（第 1 页 prev → 末页）----
    click(ctl, "page_next")
    label2 = wait_text(ctl, "page_label", expect="2 / %d" % PAGES)
    check(label2 == "2 / %d" % PAGES, "page_next 后 page_label 应为 '2 / %d'（实际 %r）"
          % (PAGES, label2))
    require_entity(ctl, "item_0")            # 新页按页内序号重建 item_0
    click(ctl, "page_prev")
    label3 = wait_text(ctl, "page_label", expect=label1)
    check(label3 == label1, "page_prev 后 page_label 应回到 %r（实际 %r）" % (label1, label3))
    # 越界循环：第 1 页 page_prev → 末页；末页 page_next → 第 1 页（changePage 取模）
    last_label = "%d / %d" % (PAGES, PAGES)
    click(ctl, "page_prev")
    label_last = wait_text(ctl, "page_label", expect=last_label)
    check(label_last == last_label, "第 1 页 page_prev 应循环到 '%s'（实际 %r）"
          % (last_label, label_last))
    step(ctl, 1)                             # node_destroy 帧末生效：等旧 UI 清理完再数条目
    item_last = sorted(int(m.group(1)) for m in
                       (re.match(r"^item_(\d+)$", n) for n in names(ctl)) if m)
    expect_last = min(PER_PAGE, TOTAL - (PAGES - 1) * PER_PAGE)
    check(item_last == list(range(expect_last)),
          "末页应创建 item_0..item_%d（实际 %s）" % (expect_last - 1, item_last[:25]))
    click(ctl, "page_next")
    label_wrap = wait_text(ctl, "page_label", expect=label1)
    check(label_wrap == label1, "末页 page_next 应循环回 %r（实际 %r）" % (label1, label_wrap))
    out("翻页 OK：%r → %r → %r；越界循环 %r ↔ %r（末页 %d 项）"
        % (label1, label2, label3, label1, last_label, expect_last))

    # ---- ③ 点 item_0 → demo：标题=MODELS[0]，demo_title 文本一致，demo_index="1/149" ----
    m0 = MODELS[0]
    click(ctl, "item_0")
    wait_title(ctl, m0)
    wait_entity(ctl, "demo_title")
    check(get_text(ctl, "demo_title") == m0,
          "demo_title 应为 '%s'（实际 %r）" % (m0, get_text(ctl, "demo_title")))
    check(get_text(ctl, "demo_index") == "1 / %d" % TOTAL,
          "demo_index 应为 '1 / %d'（实际 %r）" % (TOTAL, get_text(ctl, "demo_index")))
    check(entity(ctl, "gallery_title") is None, "进入 demo 后画廊 list 应随旧世界销毁")
    out("进入 demo OK：item_0 → '%s'" % m0)

    # ---- ④ demo_next → MODELS[1]；demo_prev 回 MODELS[0] ----
    click(ctl, "demo_next")
    wait_title(ctl, MODELS[1])
    wait_text(ctl, "demo_title", expect=MODELS[1])
    check(get_text(ctl, "demo_index") == "2 / %d" % TOTAL,
          "demo_next 后 demo_index 应为 '2 / %d'（实际 %r）"
          % (TOTAL, get_text(ctl, "demo_index")))
    click(ctl, "demo_prev")
    wait_title(ctl, m0)
    wait_text(ctl, "demo_title", expect=m0)
    check(get_text(ctl, "demo_index") == "1 / %d" % TOTAL,
          "demo_prev 后 demo_index 应回到 '1 / %d'（实际 %r）"
          % (TOTAL, get_text(ctl, "demo_index")))
    out("demo 上一个/下一个 OK：%s → %s → %s" % (m0, MODELS[1], m0))

    # ---- ⑤ 循环：首项 demo_prev → 末项；末项 demo_next → 首项 ----
    mlast = MODELS[-1]
    click(ctl, "demo_prev")
    wait_title(ctl, mlast)
    wait_text(ctl, "demo_title", expect=mlast)
    check(get_text(ctl, "demo_index") == "%d / %d" % (TOTAL, TOTAL),
          "首项 prev 回绕后 demo_index 应为 '%d / %d'（实际 %r）"
          % (TOTAL, TOTAL, get_text(ctl, "demo_index")))
    click(ctl, "demo_next")
    wait_title(ctl, m0)
    wait_text(ctl, "demo_title", expect=m0)
    out("循环 OK：%s --prev--> %s --next--> %s" % (m0, mlast, m0))

    # ---- ⑥ demo 截图（首项模型画面；抓图前多推几帧等 3D 装载/首帧稳定）----
    step(ctl, 3)
    dw, dh, drgb = capture(ctl, "demo")
    check((dw, dh) == (1280, 720), "demo 截图尺寸应为 1280x720（实际 %dx%d）" % (dw, dh))

    # ---- ⑦ demo_back → 入口标题 + 列表重建（item_0 再次出现，页码回到进入时那页）----
    click(ctl, "demo_back")
    wait_title(ctl, ENTRY_TITLE)
    wait_entity(ctl, "item_0")
    require_entity(ctl, "gallery_title")
    wait_text(ctl, "page_label", expect="1 / %d" % PAGES)
    check(get_text(ctl, "page_label") == "1 / %d" % PAGES,
          "返回列表后应回到第 1 页（实际 %r）" % get_text(ctl, "page_label"))
    out("返回列表 OK：title='%s'，item_0 重新出现" % ENTRY_TITLE)

    # ---- ⑧ 入口截图 + 像素断言 ----
    step(ctl, 3)
    gw, gh, grgb = capture(ctl, "gallery")
    check((gw, gh) == (1280, 720), "入口截图尺寸应为 1280x720（实际 %dx%d）" % (gw, gh))

    gstats = image_stats(gw, gh, grgb)
    dstats = image_stats(dw, dh, drgb)
    diff = mean_abs_diff(gw, gh, grgb, drgb)
    dcenter_nb, dcenter_br = center_stats(dw, dh, drgb)
    gcenter_nb, _ = center_stats(gw, gh, grgb)
    out("像素：入口 bg=%s nonbg=%.3f bright=%.3f colors=%d | demo bg=%s nonbg=%.3f "
        "bright=%.3f colors=%d | demo 中央 nonbg=%.3f | 入口 中央 nonbg=%.3f | 平均差=%.2f"
        % (gstats["bg"], gstats["nonbg"], gstats["bright"], gstats["colors"],
           dstats["bg"], dstats["nonbg"], dstats["bright"], dstats["colors"],
           dcenter_nb, gcenter_nb, diff))

    # 入口：文字像素明显（标题 + 20 个条目标签 + 页码，白字深底）
    check(gstats["bright"] > 0.003,
          "入口图文字像素过少（bright=%.4f，预期 >0.003）" % gstats["bright"])
    check(gstats["nonbg"] > 0.10,
          "入口图非背景占比过低（nonbg=%.3f，预期 >0.10：分页列表条目矩形 + 标题）"
          % gstats["nonbg"])
    check(gstats["colors"] > 20, "入口图颜色数过少（%d）——疑似没渲染 UI" % gstats["colors"])
    # demo：明显有内容（模型 + 上下栏），且中央区域非纯背景（模型真的画出来了）
    check(dstats["nonbg"] > 0.05,
          "demo 图非背景占比过低（nonbg=%.3f，预期 >0.05：3D 模型 + 上下栏）" % dstats["nonbg"])
    check(dcenter_nb > 0.02,
          "demo 图中央区域几乎没有模型像素（nonbg=%.3f，预期 >0.02）" % dcenter_nb)
    check(dstats["bright"] > 0.001,
          "demo 图几乎没有亮像素（bright=%.4f，预期 >0.001：标题/按钮文字）" % dstats["bright"])
    check(dstats["colors"] > 20, "demo 图颜色数过少（%d）——疑似没渲染 3D 模型" % dstats["colors"])
    # 两种模式画面显著不同
    check(diff > 8.0, "入口图与 demo 图差异过小（平均通道差 %.2f，预期 >8）" % diff)

    return dict(label1=label1, m0=m0, mlast=mlast, diff=diff,
                gstats=gstats, dstats=dstats, dcenter=dcenter_nb)


def main():
    ctl = Control(PLAYER, ROOT, WORK)
    summary = None
    had_error = False
    try:
        summary = run(ctl)
    except SystemExit:
        had_error = True
        raise
    finally:
        ctl.close(kill=had_error)

    # ---- ⑨ 覆盖层日志（on_start 每次场景装配后一行；index 0 基；翻页/模式切换都要打）----
    try:
        log = open(os.path.join(WORK, "player.log"), encoding="utf-8", errors="replace").read()
    except OSError:
        log = ""
    expected_logs = [
        "[gallery] mode=gallery page=1/%d items=%d" % (PAGES, min(PER_PAGE, TOTAL)),
        "[gallery] mode=demo title=%s index=0 count=%d" % (MODELS[0], TOTAL),
        "[gallery] mode=demo title=%s index=1 count=%d" % (MODELS[1], TOTAL),
        "[gallery] mode=gallery page=2/%d" % PAGES,
        "[gallery] mode=demo title=%s index=%d count=%d" % (MODELS[-1], TOTAL - 1, TOTAL),
    ]
    for line in expected_logs:
        check(line in log, "player 日志缺覆盖层日志行: %r" % line)

    out("日志 OK：mode=gallery/demo 的 on_start 行齐全（含 index 0/1 与回绕末项）")
    out("PASS：models=%d pages=%d demo=%s demo_diff=%.2f"
        % (TOTAL, PAGES, summary["m0"], summary["diff"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
PY
rc=$?
if [ "$rc" -ne 0 ]; then
    fail "驱动退出码 $rc"
fi

# 产物核对（截图是交付物，也是目检证据）
[ -f "$WORK/gallery.ppm" ] || fail "缺少入口截图 $WORK/gallery.ppm"
[ -f "$WORK/demo.ppm" ] || fail "缺少 demo 截图 $WORK/demo.ppm"

echo "khronos_gallery_e2e: ok (entry='$WORK/gallery.ppm' demo='$WORK/demo.ppm' work='$WORK')"
exit 0
