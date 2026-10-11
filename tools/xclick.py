#!/usr/bin/env python3
"""向 X11 窗口合成真实鼠标事件（XTest）—— 用于验证"真实窗口输入通路"。

用法：
  python3 tools/xclick.py <窗口标题片段|类名> <窗口内 x> <窗口内 y> [--move-only] [--button N]
  python3 tools/xclick.py --list                # 列出可见窗口（几何/类名/标题）

窗口发现走 `xwininfo -root -tree`（稳定），事件合成走 libXtst（ctypes，无需额外安装）。
坐标是**窗口内像素**；脚本自动换算为屏幕坐标（含窗口位置）。
"""
import ctypes
import ctypes.util
import re
import subprocess
import sys
import time


def list_windows():
    """解析 xwininfo 树 → [(id, title, klass, w, h, abs_x, abs_y)]（按面积降序）"""
    try:
        out = subprocess.run(['xwininfo', '-root', '-tree'], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.SubprocessError) as e:
        raise SystemExit(f'xclick: 无法运行 xwininfo（{e}）')
    wins = []
    pat = re.compile(r'\s+(0x[0-9a-f]+)\s+"([^"]*)":\s+\("([^"]*)"\s+"([^"]*)"\)\s+'
                     r'(\d+)x(\d+)\+(-?\d+)\+(-?\d+)(?:\s+\+(-?\d+)\+(-?\d+))?')
    for line in out.splitlines():
        m = pat.match(line)
        if m:
            wid, title, k1, k2, w, h, rx, ry, ax, ay = m.groups()
            # 末尾的 +X+Y 才是屏幕绝对坐标（前面的 +x+y 相对父窗口）
            px = int(ax) if ax is not None else int(rx)
            py = int(ay) if ay is not None else int(ry)
            wins.append((int(wid, 16), title, k1 or k2, int(w), int(h), px, py))
    wins.sort(key=lambda t: t[3] * t[4], reverse=True)
    return wins


def find_window(needle):
    for wid, title, klass, w, h, ax, ay in list_windows():
        if needle in title or needle == klass:
            return wid, title, klass, w, h, ax, ay
    return None


class XClientMessageEvent(ctypes.Structure):
    _fields_ = [('type', ctypes.c_int), ('serial', ctypes.c_ulong), ('send_event', ctypes.c_int),
                ('display', ctypes.c_void_p), ('window', ctypes.c_ulong),
                ('message_type', ctypes.c_ulong), ('format', ctypes.c_int),
                ('data', ctypes.c_long * 5)]


class XEvent(ctypes.Union):
    _fields_ = [('type', ctypes.c_int), ('xclient', XClientMessageEvent),
                ('pad', ctypes.c_long * 24)]


class XTest:
    def __init__(self):
        libx = ctypes.util.find_library('X11')
        libt = ctypes.util.find_library('Xtst')
        if not libx or not libt:
            raise SystemExit('xclick: 缺少 libX11 / libXtst')
        self.x = ctypes.CDLL(libx)
        self.t = ctypes.CDLL(libt)
        self.x.XOpenDisplay.restype = ctypes.c_void_p
        self.x.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x.XFlush.argtypes = [ctypes.c_void_p]
        self.x.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.x.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.x.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
        self.x.XFree.argtypes = [ctypes.c_void_p]
        self.t.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                                ctypes.c_int, ctypes.c_ulong]
        self.t.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int,
                                                ctypes.c_ulong]
        self.dpy = self.x.XOpenDisplay(None)
        if not self.dpy:
            raise SystemExit('xclick: 无法连接 X 显示（DISPLAY 未设置？）')

    def _top_level(self, win):
        """沿父链找到顶层窗口（reparenting WM 下客户端外面还有框架窗口）"""
        cur = win
        for _ in range(16):
            out = subprocess.run(['xwininfo', '-id', hex(cur)], capture_output=True, text=True,
                                 timeout=10).stdout
            m = re.search(r'Parent window id:\s*(0x[0-9a-f]+)', out)
            if not m:
                return cur
            parent = int(m.group(1), 16)
            root = self.x.XDefaultRootWindow(self.dpy)
            if parent in (0, root):
                return cur
            cur = parent
        return cur

    def _send_ewmh(self, win, message_name, data):
        """EWMH 客户端消息（置顶/激活由 WM 执行；直接 XRaiseWindow 对框架无效）"""
        root = self.x.XDefaultRootWindow(self.dpy)
        ev = XEvent()
        ctypes.memset(ctypes.byref(ev), 0, ctypes.sizeof(ev))
        ev.type = 33                                     # ClientMessage
        ev.xclient.window = win
        ev.xclient.message_type = self.x.XInternAtom(self.dpy, message_name, 0)
        ev.xclient.format = 32
        for i, v in enumerate(data[:5]):
            ev.xclient.data[i] = v
        mask = 0x1A0000                                  # SubstructureRedirect|SubstructureNotify
        self.x.XSendEvent(self.dpy, ctypes.c_ulong(root), 0, ctypes.c_long(mask), ctypes.byref(ev))
        self.x.XFlush(self.dpy)

    def raise_window(self, win):
        """置顶并聚焦（窗口可能被其它窗口遮挡 → 合成点击会打到别的窗口上）"""
        top = self._top_level(win)
        self.x.XMapWindow(self.dpy, ctypes.c_ulong(top))
        self.x.XRaiseWindow(self.dpy, ctypes.c_ulong(top))      # 先抬框架窗口
        atom_above = self.x.XInternAtom(self.dpy, b'_NET_WM_STATE_ABOVE', 0)
        self._send_ewmh(win, b'_NET_WM_STATE', [1, atom_above, 0, 1, 0])   # 加 ABOVE
        self._send_ewmh(win, b'_NET_ACTIVE_WINDOW', [2, 0, 0, 0, 0])       # 激活（源=应用）
        self.x.XRaiseWindow(self.dpy, ctypes.c_ulong(top))
        self.x.XSetInputFocus(self.dpy, ctypes.c_ulong(win), 2, 0)
        self.x.XSync(self.dpy, 0)

    def move(self, x, y):
        self.t.XTestFakeMotionEvent(self.dpy, -1, x, y, 0)
        self.x.XFlush(self.dpy)

    def click(self, x, y, button=1):
        self.move(x, y)
        self.t.XTestFakeButtonEvent(self.dpy, button, 1, 0)
        self.x.XFlush(self.dpy)
        self.t.XTestFakeButtonEvent(self.dpy, button, 0, 0)
        self.x.XFlush(self.dpy)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if '--list' in sys.argv:
        for wid, title, klass, w, h, ax, ay in list_windows():
            print(f'  {wid:#x}  {w}x{h}+{ax}+{ay}  [{klass}]  {title!r}')
        return 0
    if len(args) < 3:
        print(__doc__)
        return 1
    needle, rx, ry = args[0], int(args[1]), int(args[2])
    button = int(sys.argv[sys.argv.index('--button') + 1]) if '--button' in sys.argv else 1
    found = find_window(needle)
    if not found:
        print(f'xclick: 未找到标题/类名含 "{needle}" 的窗口', file=sys.stderr)
        return 2
    wid, title, klass, w, h, ax, ay = found
    px, py = ax + rx, ay + ry
    xt = XTest()
    if '--no-raise' not in sys.argv:
        xt.raise_window(wid)          # 默认置顶聚焦：否则点击可能落在遮挡窗口上
        time.sleep(0.15)
    if '--move-only' in sys.argv:
        xt.move(px, py)
        print(f'xclick: move → [{klass}] {title!r} {w}x{h} 内 ({rx},{ry}) → 屏幕 ({px},{py})')
        return 0
    if not (ax <= px < ax + w and ay <= py < ay + h):
        print(f'xclick: 目标点 ({px},{py}) 不在窗口矩形 ({ax},{ay})+{w}x{h} 内', file=sys.stderr)
        return 3
    xt.click(px, py, button)
    print(f'xclick: click → [{klass}] {title!r} {w}x{h}@{ax},{ay} 内 ({rx},{ry}) → 屏幕 ({px},{py})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
