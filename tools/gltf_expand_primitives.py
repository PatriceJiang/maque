#!/usr/bin/env python3
"""把 GLB 的非三角 primitive 展开成三角网格（离线近似，供运行时直读）。

引擎只有三角光栅，且不新增点/线图元拓扑；`MeshPrimitiveModes` 这类测试模型在
`scenes/khronos-bin`（runtime .glb 直读）里会只剩三角形那一行。这个工具在 GLB 生成期：

  · TRIANGLE_STRIP / TRIANGLE_FAN → 三角形索引（顶点不变）；
  · POINTS / LINES / LINE_STRIP / LINE_LOOP → 细四边面（点=小方块、线=逐段细长四边形，
    宽度取该 primitive 包围盒对角线的 ~2.5%/4%），与 `tools/gltf_import.py` 的离线近似同口径。

只处理 POSITION/NORMAL/TEXCOORD_0/COLOR_0（float32；COLOR_0 支持 UNSIGNED_BYTE normalized）；
其它属性（JOINTS/WEIGHTS/TANGENT）在非三角 primitive 上罕见，保持原样会失配 → 跳过并告警。

用法：python tools/gltf_expand_primitives.py <model.glb> --in-place [--out ...]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import sys

import gltf_webp_to_png as glb

FLOAT = 5126
UBYTE = 5121


def read_accessor(js, bin_bytes, idx, want_comps):
    acc = js["accessors"][idx]
    comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
    if comps != want_comps:
        raise ValueError("accessor %d type %s != %d comps" % (idx, acc["type"], want_comps))
    bv = js["bufferViews"][acc["bufferView"]]
    ctype = acc["componentType"]
    count = int(acc["count"])
    if ctype == FLOAT:
        size, fmt, norm = 4, "f", 1.0
    elif ctype == UBYTE:
        size, fmt, norm = 1, "B", 1.0 / 255.0
    else:
        raise ValueError("accessor %d 分量类型 %d 未支持（只支持 float32/ubyte）" % (idx, ctype))
    stride = int(bv.get("byteStride") or comps * size)
    base = int(bv.get("byteOffset", 0)) + int(acc.get("byteOffset", 0))
    out = []
    for i in range(count):
        row = struct.unpack_from("<" + fmt * comps, bin_bytes, base + i * stride)
        out.append([float(x) * norm for x in row])
    return out


def add_view(js, bin_parts, data):
    off = sum(len(x) for x in bin_parts)
    pad = (-off) % 4
    if pad:
        bin_parts.append(b"\x00" * pad)
        off += pad
    bv = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
    js["bufferViews"].append(bv)
    bin_parts.append(data)
    return len(js["bufferViews"]) - 1


def add_accessor(js, bin_parts, arr, comps, ctype=FLOAT):
    if ctype == FLOAT:
        data = b"".join(struct.pack("<" + "f" * comps, *row) for row in arr)
    elif ctype == 5125:  # UNSIGNED_INT indices
        data = b"".join(struct.pack("<I", int(row[0])) for row in arr)
    elif ctype == 5121:  # UNSIGNED_BYTE
        data = b"".join(bytes(int(x) for x in row) for row in arr)
    else:
        raise ValueError("add_accessor 不支持 componentType=%d" % ctype)
    view = add_view(js, bin_parts, data)
    acc = {"bufferView": view, "componentType": ctype, "count": len(arr),
           "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4"}[comps]}
    if arr:
        mins = [min(row[i] for row in arr) for i in range(comps)]
        maxs = [max(row[i] for row in arr) for i in range(comps)]
        acc["min"], acc["max"] = mins, maxs
    js["accessors"].append(acc)
    return len(js["accessors"]) - 1


def expand_primitive(js, bin_bytes, bin_parts, prim):
    mode = int(prim.get("mode", 4))
    if mode == 4:
        return False
    attrs = dict(prim.get("attributes") or {})
    if "POSITION" not in attrs:
        return False
    for k in attrs:
        if k not in ("POSITION", "NORMAL", "TEXCOORD_0", "COLOR_0"):
            print("  primitive mode=%d 含属性 %s → 跳过（未支持）" % (mode, k), file=sys.stderr)
            return False
    pos = read_accessor(js, bin_bytes, attrs["POSITION"], 3)
    n = len(pos)
    uv = read_accessor(js, bin_bytes, attrs["TEXCOORD_0"], 2) if "TEXCOORD_0" in attrs else [[0.0, 0.0]] * n
    nor = read_accessor(js, bin_bytes, attrs["NORMAL"], 3) if "NORMAL" in attrs else None
    col = None
    if "COLOR_0" in attrs:
        acc = js["accessors"][attrs["COLOR_0"]]
        c = 4 if acc["type"] == "VEC4" else 3
        col = read_accessor(js, bin_bytes, attrs["COLOR_0"], c)
        if c == 3:
            col = [r + [1.0] for r in col]
    indices = []
    if "indices" in prim:
        acc = js["accessors"][prim["indices"]]
        bv = js["bufferViews"][acc["bufferView"]]
        ctype = acc["componentType"]
        fmt = {5121: "B", 5123: "H", 5125: "I"}[ctype]
        size = struct.calcsize(fmt)
        stride = int(bv.get("byteStride") or size)
        base = int(bv.get("byteOffset", 0)) + int(acc.get("byteOffset", 0))
        for i in range(int(acc["count"])):
            indices.append(struct.unpack_from("<" + fmt, bin_bytes, base + i * stride)[0])
    else:
        indices = list(range(n))

    if mode in (5, 6):
        out = []
        if mode == 5:
            for t in range(max(0, len(indices) - 2)):
                a, b, c = indices[t], indices[t + 1], indices[t + 2]
                out += [a, b, c] if t % 2 == 0 else [b, a, c]
        else:
            for t in range(1, max(1, len(indices) - 1)):
                out += [indices[0], indices[t], indices[t + 1]]
        prim["indices"] = add_accessor(js, bin_parts, [[v] for v in out], 1, 5125)
        prim["mode"] = 4
        print("  primitive mode=%d → %d 三角形" % (mode, len(out) // 3), file=sys.stderr)
        return True

    xs = [p[0] for p in pos]; ys = [p[1] for p in pos]; zs = [p[2] for p in pos]
    diag = math.sqrt((max(xs) - min(xs)) ** 2 + (max(ys) - min(ys)) ** 2 + (max(zs) - min(zs)) ** 2)
    half = max(1e-4, diag * (0.04 if mode == 0 else 0.025)) * 0.5
    new_pos, new_uv, new_nor, new_col, tris = [], [], [], [], []

    def emit(quad, quad_uv, quad_nor, quad_col, quad_tris):
        base = len(new_pos)
        new_pos.extend(quad)
        new_uv.extend(quad_uv)
        if col is not None:
            new_col.extend(quad_col)
        if nor is not None:
            new_nor.extend(quad_nor)
        tris.extend(base + t for t in quad_tris)

    if mode == 0:
        for i, p in enumerate(pos):
            quad = [[p[0] - half, p[1] - half, p[2]], [p[0] + half, p[1] - half, p[2]],
                    [p[0] + half, p[1] + half, p[2]], [p[0] - half, p[1] + half, p[2]]]
            nn = [nor[i]] * 4 if nor else []
            emit(quad, [uv[i]] * 4, nn, [col[i]] * 4 if col else [], [0, 1, 2, 0, 2, 3])
    else:
        if mode == 1:
            segs = [(indices[i], indices[i + 1]) for i in range(0, len(indices) - 1, 2)]
        elif mode == 2:
            segs = [(indices[i], indices[(i + 1) % len(indices)]) for i in range(len(indices))]
        else:
            segs = [(indices[i], indices[i + 1]) for i in range(len(indices) - 1)]
        for a, b in segs:
            pa, pb = pos[a], pos[b]
            dx, dy = pb[0] - pa[0], pb[1] - pa[1]
            ln = math.hypot(dx, dy)
            px, py = (-dy / ln * half, dx / ln * half) if ln > 1e-8 else (half, 0.0)
            quad = [[pa[0] + px, pa[1] + py, pa[2]], [pa[0] - px, pa[1] - py, pa[2]],
                    [pb[0] + px, pb[1] + py, pb[2]], [pb[0] - px, pb[1] - py, pb[2]]]
            nn = [nor[a], nor[a], nor[b], nor[b]] if nor else []
            cc = [col[a], col[a], col[b], col[b]] if col else []
            emit(quad, [uv[a], uv[a], uv[b], uv[b]], nn, cc, [0, 1, 3, 0, 3, 2])

    attrs["POSITION"] = add_accessor(js, bin_parts, new_pos, 3)
    attrs["TEXCOORD_0"] = add_accessor(js, bin_parts, new_uv, 2)
    if nor is not None:
        attrs["NORMAL"] = add_accessor(js, bin_parts, new_nor, 3)
    if col is not None:
        attrs["COLOR_0"] = add_accessor(js, bin_parts, new_col, 4)
    prim["attributes"] = attrs
    prim["indices"] = add_accessor(js, bin_parts, [[v] for v in tris], 1, 5125)
    prim["mode"] = 4
    print("  primitive mode=%d → %d 顶点 / %d 三角形" % (mode, len(new_pos), len(tris) // 3),
          file=sys.stderr)
    return True


def convert(src, dst):
    src_size = os.path.getsize(src)
    js, bin_chunk = glb.read_glb(src)
    bin_parts = [bin_chunk]
    changed = 0
    for mesh in js.get("meshes") or []:
        for prim in mesh.get("primitives") or []:
            if expand_primitive(js, bin_chunk, bin_parts, prim):
                changed += 1
    if not changed:
        print("没有非三角 primitive，跳过", file=sys.stderr)
        return 0
    new_bin = b"".join(bin_parts)
    (js.get("buffers") or [{}])[0]["byteLength"] = len(new_bin)
    size = glb.write_glb(js, new_bin, dst)
    print(json.dumps({"primitives": changed, "glb_before": src_size,
                      "glb_after": size}, ensure_ascii=False))
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("--out", default="")
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args()
    dst = args.input if args.in_place else (args.out or os.path.splitext(args.input)[0] + "_tri.glb")
    if dst == args.input and not args.in_place:
        raise SystemExit("输出与输入相同；用 --in-place 或 --out")
    convert(args.input, dst)


if __name__ == "__main__":
    main()
