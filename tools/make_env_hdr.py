#!/usr/bin/env python3
"""生成 equirect Radiance .hdr 测试环境（HDR RGBE，RLE 扫描线）。

用途：envmap/IBL 的 HDR 回归资产（scenes/pbr/env/studio.hdr）与 demo 场景。
天空渐变 + 3 个 HDR 软光箱（峰值 > 1，用于验证 RGBA16F/PMREM 路径；LDR PNG 会截断高光）。
"""
import math
import struct


def rgbe(r, g, b):
    v = max(r, g, b)
    if v < 1e-32:
        return b"\x00\x00\x00\x00"
    mant, exp = math.frexp(v)          # v = mant * 2^exp, 0.5 <= mant < 1
    scale = mant * 256.0 / v
    return bytes((min(255, int(r * scale)), min(255, int(g * scale)),
                  min(255, int(b * scale)), exp + 128))


def main():
    w, h = 128, 64
    rows = []
    boxes = [
        (-0.55, 0.78, 0.55, (1.0, 0.97, 0.92), 40.0, 18.0),
        (-0.30, 0.84, 0.62, (1.0, 0.98, 0.95), 12.0, 24.0),
        (0.80, 0.30, 0.45, (0.85, 0.92, 1.0), 8.0, 10.0),
    ]
    for y in range(h):
        lat = math.pi * (0.5 - (y + 0.5) / h)
        cl, sl = math.cos(lat), math.sin(lat)
        row = bytearray()
        for x in range(w):
            lon = math.pi * 2.0 * ((x + 0.5) / w) - math.pi
            dx, dy, dz = cl * math.cos(lon), sl, cl * math.sin(lon)
            t = max(0.0, min(1.0, dy * 0.5 + 0.5))
            r = 0.14 + (0.62 - 0.14) * t
            g = 0.13 + (0.66 - 0.13) * t
            b = 0.12 + (0.74 - 0.12) * t
            horizon = math.exp(-abs(dy) * 8.0)
            r += horizon * 0.18
            g += horizon * 0.17
            b += horizon * 0.16
            for bx, by, bz, col, amp, sharp in boxes:
                l = math.sqrt(bx * bx + by * by + bz * bz)
                c = max(0.0, (dx * bx + dy * by + dz * bz) / l)
                s = (c ** sharp) * amp
                r += col[0] * s
                g += col[1] * s
                b += col[2] * s
            row += rgbe(r, g, b)
        rows.append(bytes(row))

    def rle_channel(vals):
        out = bytearray()
        i = 0
        while i < len(vals):
            n = min(128, len(vals) - i)
            out.append(n)
            out += vals[i:i + n]
            i += n
        return bytes(out)

    out = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
    out += f"-Y {h} +X {w}\n".encode()
    for row in rows:
        channels = [bytes(row[i::4]) for i in range(4)]
        out += bytes((2, 2, (w >> 8) & 0xFF, w & 0xFF))
        for ch in channels:
            out += rle_channel(ch)

    import os
    dest = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "scenes", "pbr", "env", "studio.hdr")
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(out)
    print(f"wrote {dest} ({len(out)} bytes, {w}x{h} RGBE)")


if __name__ == "__main__":
    main()
