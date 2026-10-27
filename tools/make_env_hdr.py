#!/usr/bin/env python3
"""生成 equirect Radiance .hdr 测试环境（HDR RGBE，RLE 扫描线）+ EXR 回归资产。

用途：envmap/IBL 的 HDR 回归资产（`scenes/pbr/env/studio.hdr`）、EXR 解码回归资产
（`studio.exr` = FLOAT/ZIP、`studio_half.exr` = HALF/ZIPS，像素值与 .hdr 解码后逐位一致）
与 demo 场景。天空渐变 + 3 个 HDR 软光箱（峰值 > 1，用于验证 RGBA16F/PMREM 路径；LDR PNG 会截断高光）。
"""
import math
import struct
import zlib


def rgbe(r, g, b):
    v = max(r, g, b)
    if v < 1e-32:
        return b"\x00\x00\x00\x00"
    mant, exp = math.frexp(v)          # v = mant * 2^exp, 0.5 <= mant < 1
    scale = mant * 256.0 / v
    return bytes((min(255, int(r * scale)), min(255, int(g * scale)),
                  min(255, int(b * scale)), exp + 128))


def rgbe_decode(p4):
    """RGBE → 线性 float RGB（与 runtime parse_radiance_hdr 的 ldexp 语义一致）。"""
    if p4[3] == 0:
        return 0.0, 0.0, 0.0
    f = math.ldexp(1.0, p4[3] - 136)
    return p4[0] * f, p4[1] * f, p4[2] * f


def _zip_encode(raw):
    """OpenEXR ZIP/RLE 前置变换：字节交错（奇偶劈半）→ predictor（差分 +128）→ zlib。"""
    n = len(raw)
    half = (n + 1) // 2
    t = bytearray(n)
    p1, p2 = 0, half
    for i in range(0, n, 2):
        t[p1] = raw[i]; p1 += 1
        if i + 1 < n:
            t[p2] = raw[i + 1]; p2 += 1
    out = bytearray(n)
    out[0] = t[0]                       # predictor：首字节原样，其后差分 +128
    prev = t[0]
    for i in range(1, n):
        out[i] = (t[i] - prev + 128) & 0xFF
        prev = t[i]
    return zlib.compress(bytes(out), 9)


def _exr_attr(name, typ, data):
    return name.encode() + b"\0" + typ.encode() + b"\0" + struct.pack("<I", len(data)) + data


def write_exr(path, w, h, rows_rgb, channel_type=2, compression=3):
    """写单 part scanline EXR（channel_type: 1=HALF, 2=FLOAT；compression: 0=NONE, 3=ZIP）。"""
    pack = "<e" if channel_type == 1 else "<f"
    cbytes = 2 if channel_type == 1 else 4
    # chlist：B/G/R 三通道（与 OpenCV/多数 EXR 一致），pLinear=0、采样率 1
    ch = bytearray()
    for name in ("B", "G", "R"):
        ch += name.encode() + b"\0" + struct.pack("<i", channel_type) + b"\0\0\0\0" + struct.pack("<ii", 1, 1)
    ch += b"\0"
    lines_per_block = 16 if compression == 3 else 1
    block_count = (h + lines_per_block - 1) // lines_per_block
    header = bytearray(struct.pack("<II", 0x01312F76, 2))
    header += _exr_attr("channels", "chlist", bytes(ch))
    header += _exr_attr("compression", "compression", bytes((compression,)))
    header += _exr_attr("dataWindow", "box2i", struct.pack("<iiii", 0, 0, w - 1, h - 1))
    header += _exr_attr("displayWindow", "box2i", struct.pack("<iiii", 0, 0, w - 1, h - 1))
    header += _exr_attr("lineOrder", "lineOrder", b"\0")
    header += _exr_attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    header += _exr_attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    header += _exr_attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    header += b"\0"
    blocks = []
    for b in range(block_count):
        y0 = b * lines_per_block
        nrows = min(lines_per_block, h - y0)
        raw = bytearray()
        for r in range(nrows):
            row = rows_rgb[y0 + r]
            for c in (2, 1, 0):                       # B、G、R
                raw += struct.pack("<" + pack[1] * w, *(row[x][c] for x in range(w)))
        if compression == 0:
            payload = bytes(raw)
        else:
            payload = _zip_encode(bytes(raw))
        blocks.append(struct.pack("<ii", y0, len(payload)) + payload)
    offset = len(header) + 8 * block_count
    table = bytearray()
    for blk in blocks:
        table += struct.pack("<Q", offset); offset += len(blk)
    with open(path, "wb") as f:
        f.write(bytes(header) + bytes(table) + b"".join(blocks))
    print(f"wrote {path} ({offset} bytes, {w}x{h}, type={channel_type}, compression={compression})")


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
    base = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "scenes", "pbr", "env")
    os.makedirs(base, exist_ok=True)
    dest = os.path.join(base, "studio.hdr")
    with open(dest, "wb") as f:
        f.write(out)
    print(f"wrote {dest} ({len(out)} bytes, {w}x{h} RGBE)")

    # EXR fixture：用 .hdr 解码后的线性值写，保证与 .hdr 渲染逐像素一致（RGBE 量化为 8-bit 尾数）。
    rows_rgb = []
    for row in rows:
        rgb = []
        for x in range(w):
            rgb.append((rgbe_decode(row[x * 4:x * 4 + 4])[0],
                        rgbe_decode(row[x * 4:x * 4 + 4])[1],
                        rgbe_decode(row[x * 4:x * 4 + 4])[2]))
        rows_rgb.append(rgb)
    write_exr(os.path.join(base, "studio.exr"), w, h, rows_rgb, channel_type=2, compression=3)   # FLOAT/ZIP
    write_exr(os.path.join(base, "studio_half.exr"), w, h, rows_rgb, channel_type=1, compression=2)  # HALF/ZIPS


if __name__ == "__main__":
    main()
