#!/usr/bin/env python3
"""把 GLB 里的 EXT_texture_webp 贴图转成 PNG/JPEG，并改写成普通 core 纹理。

为什么需要：引擎 image 模块没有 WebP 解码依赖（wuffs 不支持 VP8 lossy，运行时明确不支持
EXT_texture_webp）。离线 importer 已用 Pillow 转码；runtime 直读 GLB 的 `scenes/khronos-bin`
路径没有离线步骤，所以对这类源资产做一次确定性转码：

  · 无 alpha 的 WebP → JPEG（quality 91），有 alpha → PNG；
  · 重建 GLB 的 BIN chunk（去掉旧 WebP 字节，按 8 字节对齐重排 bufferView）；
  · texture.extensions.EXT_texture_webp.source → texture.source，并移除扩展声明。

依赖 Pillow（与 tools/gltf_import.py 的 WebP 转码同一可选依赖）。用法：

  python tools/gltf_webp_to_png.py <model.glb> [--in-place]
  python tools/gltf_webp_to_png.py <model.glb> --out <converted.glb>
"""
from __future__ import annotations

import argparse
import io
import json
import os
import struct
import sys

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
WEBP_EXT = "EXT_texture_webp"


def read_glb(path: str):
    data = open(path, "rb").read()
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2:
        raise SystemExit("不是 glTF 2.0 GLB：%s" % path)
    off = 12
    js = None
    bin_chunk = b""
    while off + 8 <= min(length, len(data)):
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == CHUNK_JSON:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == CHUNK_BIN:
            bin_chunk = chunk
    if js is None:
        raise SystemExit("GLB 缺 JSON chunk")
    return js, bin_chunk


def sniff_webp(data: bytes) -> bool:
    return len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP"


def image_bytes(js, bin_chunk, img, base_dir):
    if img.get("uri"):
        if img["uri"].startswith("data:"):
            import base64
            return base64.b64decode(img["uri"].split(",", 1)[1])
        return open(os.path.join(base_dir, img["uri"]), "rb").read()
    bv = js["bufferViews"][img["bufferView"]]
    off = bv.get("byteOffset", 0)
    return bin_chunk[off:off + bv["byteLength"]]


def transcode(data: bytes):
    from PIL import Image
    im = Image.open(io.BytesIO(data))
    im.load()
    has_alpha = im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info)
    out = io.BytesIO()
    if has_alpha:
        im.convert("RGBA").save(out, "PNG", optimize=True)
        return out.getvalue(), "image/png"
    im.convert("RGB").save(out, "JPEG", quality=91, optimize=True, subsampling=0)
    return out.getvalue(), "image/jpeg"


def write_glb(js: dict, bin_bytes: bytes, dst: str) -> int:
    js_bytes = json.dumps(js, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    js_bytes += b" " * ((-len(js_bytes)) % 4)
    bin_bytes = bytes(bin_bytes)
    total = 12 + 8 + len(js_bytes) + 8 + len(bin_bytes)
    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, total)
    out += struct.pack("<II", len(js_bytes), CHUNK_JSON)
    out += js_bytes
    out += struct.pack("<II", len(bin_bytes), CHUNK_BIN)
    out += bin_bytes
    with open(dst, "wb") as f:
        f.write(out)
    return total


def convert(src: str, dst: str) -> dict:
    src_size = os.path.getsize(src)
    js, bin_chunk = read_glb(src)
    base_dir = os.path.dirname(os.path.abspath(src))
    images = js.get("images") or []
    views = js.get("bufferViews") or []
    buffers = js.get("buffers") or []
    if not buffers:
        raise SystemExit("GLB 缺 buffers")

    replaced_views = {}
    converted = {"webp": 0, "png": 0, "jpeg": 0, "bytes_before": 0, "bytes_after": 0}
    for idx, img in enumerate(images):
        if not isinstance(img, dict) or "bufferView" not in img:
            continue
        data = image_bytes(js, bin_chunk, img, base_dir)
        if "webp" not in (img.get("mimeType") or "").lower() and not sniff_webp(data):
            continue
        new_data, mime = transcode(data)
        replaced_views[img["bufferView"]] = new_data
        img["mimeType"] = mime
        converted["webp"] += 1
        converted["png" if mime == "image/png" else "jpeg"] += 1
        converted["bytes_before"] += len(data)
        converted["bytes_after"] += len(new_data)
        print(f"  image[{idx}] {len(data)}B WebP → {len(new_data)}B {mime}", file=sys.stderr)

    if not replaced_views:
        print("没有 WebP 贴图，跳过", file=sys.stderr)
        return converted

    # 重建 BIN：按原 bufferView 顺序重新打包（4/8 字节对齐），替换 WebP view。
    new_bin = bytearray()
    for vi, bv in enumerate(views):
        if bv.get("buffer", 0) != 0:
            raise SystemExit("只支持 GLB 内嵌 buffer 0（bufferView[%d] 指向 buffer %r）"
                             % (vi, bv.get("buffer")))
        data = replaced_views.get(vi)
        if data is None:
            off = bv.get("byteOffset", 0)
            data = bin_chunk[off:off + bv["byteLength"]]
        align = 8 if bv.get("byteStride") else 4
        pad = (-len(new_bin)) % align
        if pad:
            new_bin.extend(b"\x00" * pad)
        bv["byteOffset"] = len(new_bin)
        bv["byteLength"] = len(data)
        new_bin.extend(data)
    pad = (-len(new_bin)) % 4
    if pad:
        new_bin.extend(b"\x00" * pad)
    buffers[0]["byteLength"] = len(new_bin)

    # EXT_texture_webp 的 texture.source 提到 core source，移除扩展。
    for tex in js.get("textures") or []:
        ext = (tex.get("extensions") or {}).pop(WEBP_EXT, None)
        if ext and "source" in ext:
            tex["source"] = ext["source"]
        if not tex.get("extensions"):
            tex.pop("extensions", None)
    for key in ("extensionsUsed", "extensionsRequired"):
        if key in js and WEBP_EXT in js[key]:
            js[key] = [x for x in js[key] if x != WEBP_EXT]
            if not js[key]:
                js.pop(key)

    # 重建 GLB chunk。
    converted["glb_before"] = src_size
    converted["glb_after"] = write_glb(js, bytes(new_bin), dst)
    return converted


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("--out", default="")
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args()
    dst = args.input if args.in_place else (args.out or (os.path.splitext(args.input)[0] + "_png.glb"))
    if dst == args.input and not args.in_place:
        raise SystemExit("输出与输入相同；用 --in-place 或 --out")
    rec = convert(args.input, dst)
    print(json.dumps(rec, ensure_ascii=False))


if __name__ == "__main__":
    main()
