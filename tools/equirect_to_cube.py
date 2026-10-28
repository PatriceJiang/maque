#!/usr/bin/env python3
"""equirect HDR（Radiance RGBE）→ 6 面 cubemap HDR（第 26 轮，cubemap env 测试资产）。

面序/朝向导与 runtime `env_face_dir` 逐式一致：
  0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z；面内 u 向右、v 向下（图片行 0 = 顶部）。
输出文件名：`<name>_px.hdr` / `_nx` / `_py` / `_ny` / `_pz` / `_nz`（runtime load_env_cubemap 约定）。

用法：
  python tools/equirect_to_cube.py scenes/pbr/env/studio.hdr \
      --out-dir scenes/pbr/env/studio_cube --name studio --size 128
"""
from __future__ import annotations

import argparse
import math
import os
import struct


def _read_hdr(path):
    """读 Radiance RGBE（支持 new-style RLE 与平铺）；返回 (w, h, list[float] RGB)。"""
    data = open(path, "rb").read()
    pos = 0

    def line():
        nonlocal pos
        end = data.index(b"\n", pos)
        s = data[pos:end].decode("ascii")
        pos = end + 1
        return s

    first = line()
    if not first.startswith("#?"):
        raise ValueError("not a Radiance HDR: " + path)
    rgbe = False
    while True:
        s = line()
        if not s:
            break
        if s.startswith("FORMAT="):
            rgbe = "32-bit_rle_rgbe" in s
    if not rgbe:
        raise ValueError("HDR is not 32-bit_rle_rgbe")
    res = line()  # -Y h +X w
    parts = res.split()
    if len(parts) != 4:
        raise ValueError("bad resolution line: " + res)
    h = int(parts[1])
    w = int(parts[3])
    rgb = [0.0] * (w * h * 3)
    scan = bytearray(w * 4)
    for y in range(h):
        b0, b1, b2, b3 = data[pos], data[pos + 1], data[pos + 2], data[pos + 3]
        if b0 == 2 and b1 == 2 and (((b2 << 8) | b3) == w):
            pos += 4
            for ch in range(4):
                x = 0
                while x < w:
                    cnt = data[pos]
                    pos += 1
                    if cnt > 128:
                        run = cnt - 128
                        v = data[pos]
                        pos += 1
                        for i in range(run):
                            if x + i < w:
                                scan[(x + i) * 4 + ch] = v
                        x += run
                    else:
                        for i in range(cnt):
                            if x + i < w:
                                scan[(x + i) * 4 + ch] = data[pos + i]
                        pos += cnt
                        x += cnt
        else:
            scan[:] = data[pos:pos + w * 4]
            pos += w * 4
        for x in range(w):
            r, g, b, e = scan[x * 4:x * 4 + 4]
            k = (y * w + x) * 3
            if e == 0:
                rgb[k] = rgb[k + 1] = rgb[k + 2] = 0.0
            else:
                f = math.ldexp(1.0, e - 136)
                rgb[k] = r * f
                rgb[k + 1] = g * f
                rgb[k + 2] = b * f
    return w, h, rgb


def _write_hdr(path, w, h, rgb):
    """写平铺 RGBE（runtime parse_radiance_hdr 兼容老格式平铺）。行 0 = 顶部。"""
    out = bytearray()
    out += b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n"
    out += f"-Y {h} +X {w}\n".encode("ascii")
    for i in range(w * h):
        r, g, b = rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]
        m = max(r, g, b)
        if m < 1e-32:
            out += b"\x00\x00\x00\x00"
            continue
        mant, exp = math.frexp(m)
        scale = math.ldexp(1.0, 8 - exp)
        out += bytes((min(255, int(r * scale)), min(255, int(g * scale)),
                      min(255, int(b * scale)), exp + 128))
    with open(path, "wb") as f:
        f.write(out)


def _face_dir(face, u, v):
    s = 2.0 * u - 1.0
    t = 2.0 * v - 1.0
    if face == 0:   return (1.0, -t, -s)
    if face == 1:   return (-1.0, -t, s)
    if face == 2:   return (s, 1.0, t)
    if face == 3:   return (s, -1.0, -t)
    if face == 4:   return (s, -t, 1.0)
    return (-s, -t, -1.0)


def _sample_equirect(w, h, rgb, d):
    x, y, z = d
    n = math.sqrt(x * x + y * y + z * z)
    x, y, z = x / n, y / n, z / n
    u = math.atan2(z, x) / (2.0 * math.pi) + 0.5
    v = math.acos(max(-1.0, min(1.0, y))) / math.pi
    fx = u * w - 0.5
    fy = v * h - 0.5
    x0 = math.floor(fx)
    y0 = math.floor(fy)
    tx = fx - x0
    ty = fy - y0

    def px(xx, yy):
        xx %= w                       # 经度 wrap
        yy = max(0, min(h - 1, yy))   # 纬度 clamp
        k = (yy * w + xx) * 3
        return rgb[k], rgb[k + 1], rgb[k + 2]

    c00, c10, c01, c11 = px(x0, y0), px(x0 + 1, y0), px(x0, y0 + 1), px(x0 + 1, y0 + 1)
    return tuple((c00[i] * (1 - tx) + c10[i] * tx) * (1 - ty) +
                 (c01[i] * (1 - tx) + c11[i] * tx) * ty for i in range(3))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="equirect Radiance .hdr")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--name", default=None, help="面文件基名（默认 = 输入文件名去扩展名）")
    ap.add_argument("--size", type=int, default=0, help="每面边长（默认 = 输入高度/2）")
    args = ap.parse_args()
    w, h, rgb = _read_hdr(args.input)
    size = args.size or max(8, h // 2)
    name = args.name or os.path.splitext(os.path.basename(args.input))[0]
    os.makedirs(args.out_dir, exist_ok=True)
    suffixes = ("px", "nx", "py", "ny", "pz", "nz")
    for face in range(6):
        out = [0.0] * (size * size * 3)
        for y in range(size):
            v = (y + 0.5) / size
            for x in range(size):
                u = (x + 0.5) / size
                c = _sample_equirect(w, h, rgb, _face_dir(face, u, v))
                k = (y * size + x) * 3
                out[k], out[k + 1], out[k + 2] = c
        path = os.path.join(args.out_dir, f"{name}_{suffixes[face]}.hdr")
        _write_hdr(path, size, size, out)
        print("wrote", path)
    print(f"equirect {w}x{h} -> cube {size}x{size} x6 -> {args.out_dir}")


if __name__ == "__main__":
    main()
