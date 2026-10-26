#!/usr/bin/env python3
"""glTF → maque 离线资产导入（plans/18-pbr-3d.md 方向 2）。

输入：`.gltf`（JSON，external/内嵌 base64 buffer）或 `.glb`（二进制 chunk）。
输出（--out 目录，确定性文本，可纳入版本库/CI 校验）：
  <name>.mesh.json     网格 + 材质 + 贴图引用（M4b 运行时加载格式，48B 顶点布局）
  textures/<name>.png  bin bufferView 内嵌的 PNG 贴图（外部 uri 贴图也复制进来）

设计约束（与引擎一致）：
  · 顶点布局 = mq_renderer_mesh_gpu layout 0：pos3|normal3|tangent4|uv2（48B/顶点）
  · 只接受三角网格（mode=4/缺省）；其它 mode 跳过并告警（确定性：按 primitive 顺序）
  · 输出 JSON 排序键 + 固定缩进 → 同一 glTF 导入结果逐字节一致
  · 不做图像编解码：PNG 直接搬运；JPEG 贴图跳过（M5 接 image 解码器）
用法：
  python tools/gltf_import.py scene.gltf --out assets/imported
  python tools/gltf_import.py model.glb --out assets/imported --name hero
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import struct
import sys
import uuid

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
             5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
MODE_TRIANGLES = 4


def load_gltf(path: str):
    """返回 (json, bin_chunk or None, base_dir)。"""
    base_dir = os.path.dirname(os.path.abspath(path))
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] == b"glTF":
        magic, version, total = struct.unpack_from("<III", raw, 0)
        if magic != 0x46546C67 or version != 2:
            raise SystemExit(f"gltf_import: 不是 glTF 2.0 二进制: {path}")
        off, js, bin_chunk = 12, None, None
        while off + 8 <= total:
            ln, ctype = struct.unpack_from("<II", raw, off)
            off += 8
            chunk = raw[off:off + ln]
            off += ln
            if ctype == 0x4E4F534A:
                js = json.loads(chunk.decode("utf-8"))
            elif ctype == 0x004E4942:
                bin_chunk = chunk
        if js is None:
            raise SystemExit(f"gltf_import: GLB 缺 JSON chunk: {path}")
        return js, bin_chunk, base_dir
    return json.loads(raw.decode("utf-8")), None, base_dir


def read_buffers(js, bin_chunk, base_dir):
    out = []
    for buf in js.get("buffers", []):
        uri = buf.get("uri")
        if uri is None:
            if bin_chunk is None:
                raise SystemExit("gltf_import: buffer 无 uri 且无 GLB BIN chunk")
            out.append(bin_chunk)
        elif uri.startswith("data:"):
            out.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            with open(os.path.join(base_dir, uri), "rb") as f:
                out.append(f.read())
    return out


def accessor_values(js, buffers, idx):
    """读取 accessor → list[list[float]]（按类型分组；已处理 byteStride）。"""
    acc = js["accessors"][idx]
    view = js["bufferViews"][acc["bufferView"]]
    buf = buffers[view["buffer"]]
    fmt, size = COMPONENT[acc["componentType"]]
    nc = NCOMP[acc["type"]]
    count = acc["count"]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride") or size * nc
    rows = []
    for i in range(count):
        row = list(struct.unpack_from("<" + fmt * nc, buf, base + i * stride))
        rows.append(row)
    return rows


def normalize(v):
    n = sum(x * x for x in v) ** 0.5
    if n < 1e-12:
        return [0.0, 1.0, 0.0]
    return [x / n for x in v]


def sub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def compute_normals(positions, indices):
    nrm = [[0.0, 0.0, 0.0] for _ in positions]
    for t in range(0, len(indices) - 2, 3):
        i0, i1, i2 = indices[t], indices[t + 1], indices[t + 2]
        n = cross(sub(positions[i1], positions[i0]), sub(positions[i2], positions[i0]))
        for i in (i0, i1, i2):
            nrm[i] = [nrm[i][0] + n[0], nrm[i][1] + n[1], nrm[i][2] + n[2]]
    return [normalize(v) for v in nrm]


def material_from_gltf(js, mat, images_out, tex_dir, base_dir, buffers):
    pbr = mat.get("pbrMetallicRoughness", {})
    c = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
    e = mat.get("emissiveFactor", [0.0, 0.0, 0.0])
    m = {
        "type": "asset.PbrMaterial",
        "name": mat.get("name", ""),
        "base_color": {"r": c[0], "g": c[1], "b": c[2], "a": c[3]},
        "metallic": float(pbr.get("metallicFactor", 1.0)),
        "roughness": float(pbr.get("roughnessFactor", 1.0)),
        "emissive": {"x": e[0], "y": e[1], "z": e[2]},
        "double_sided": bool(mat.get("doubleSided", False)),
    }
    alpha_mode = mat.get("alphaMode", "OPAQUE")
    m["alpha_mode"] = {"OPAQUE": 0, "MASK": 1, "BLEND": 2}.get(alpha_mode, 0)
    m["alpha_cutoff"] = float(mat.get("alphaCutoff", 0.5))
    # 贴图槽：glTF 的 texture index → image uri 或 bufferView（PNG 搬运）
    def tex_slot(tinfo):
        if not tinfo:
            return ""
        tex = js["textures"][tinfo["index"]]
        src = tex.get("source")
        if src is None:
            return ""
        img = js["images"][src]
        name = (img.get("name") or f"image{src}") + ".png"
        if img.get("mimeType") not in (None, "image/png"):
            print(f"gltf_import: 跳过非 PNG 贴图 '{name}'（{img.get('mimeType')}）", file=sys.stderr)
            return ""
        dest = os.path.join(tex_dir, name)
        if img.get("uri") and not img["uri"].startswith("data:"):
            with open(os.path.join(base_dir, img["uri"]), "rb") as f:
                data = f.read()
        elif img.get("uri"):
            data = base64.b64decode(img["uri"].split(",", 1)[1])
        else:
            view = js["bufferViews"][img["bufferView"]]
            buf = buffers[view["buffer"]]
            off = view.get("byteOffset", 0)
            data = buf[off:off + view["byteLength"]]
        os.makedirs(tex_dir, exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
        images_out.add(name)
        return f"textures/{name}"

    pbr_tex = pbr.get("baseColorTexture")
    slot = tex_slot(pbr_tex)
    if slot:
        m["base_color_texture"] = slot
    mr_tex = pbr.get("metallicRoughnessTexture")
    slot = tex_slot(mr_tex)
    if slot:
        m["metallic_roughness_texture"] = slot
    slot = tex_slot(mat.get("normalTexture"))
    if slot:
        m["normal_texture"] = slot
    slot = tex_slot(mat.get("occlusionTexture"))
    if slot:
        m["occlusion_texture"] = slot
    slot = tex_slot(mat.get("emissiveTexture"))
    if slot:
        m["emissive_texture"] = slot
    return m


def import_gltf(path, out_dir, name=None):
    js, bin_chunk, base_dir = load_gltf(path)
    buffers = read_buffers(js, bin_chunk, base_dir)
    tex_dir = os.path.join(out_dir, "textures")
    name = name or os.path.splitext(os.path.basename(path))[0]
    images = set()
    meshes_out = []
    for mi, mesh in enumerate(js.get("meshes", [])):
        for pi, prim in enumerate(mesh.get("primitives", [])):
            mode = prim.get("mode", MODE_TRIANGLES)
            if mode != MODE_TRIANGLES:
                print(f"gltf_import: 跳过非三角 primitive mesh={mi} prim={pi} mode={mode}", file=sys.stderr)
                continue
            attrs = prim.get("attributes", {})
            if "POSITION" not in attrs:
                continue
            positions = accessor_values(js, buffers, attrs["POSITION"])
            n = len(positions)
            normals = (accessor_values(js, buffers, attrs["NORMAL"])
                       if "NORMAL" in attrs else compute_normals(positions, list(range(n))))
            uvs = (accessor_values(js, buffers, attrs["TEXCOORD_0"])
                   if "TEXCOORD_0" in attrs else [[0.0, 0.0]] * n)
            indices = (accessor_values(js, buffers, prim["indices"])
                       if "indices" in prim else [[i] for i in range(n)])
            indices = [int(r[0]) for r in indices]
            mat_idx = prim.get("material", -1)
            mats = []
            if mat_idx >= 0:
                mats.append(material_from_gltf(js, js["materials"][mat_idx], images,
                                               tex_dir, base_dir, buffers))
            verts = []
            for i in range(n):
                p, nr, uv = positions[i], normals[i], uvs[i]
                verts.append([p[0], p[1], p[2], nr[0], nr[1], nr[2],
                              1.0, 0.0, 0.0, 1.0, uv[0], uv[1]])
            xs = [p[0] for p in positions]; ys = [p[1] for p in positions]; zs = [p[2] for p in positions]
            mesh_name = name if len(js.get("meshes", [])) == 1 and pi == 0 else f"{name}_{mi}_{pi}"
            meshes_out.append({
                "name": mesh_name,
                "vertex_stride": 48,
                "vertex_layout": 0,
                "vertices": verts,
                "indices": indices,
                "bounds_min": [min(xs), min(ys), min(zs)],
                "bounds_max": [max(xs), max(ys), max(zs)],
                "materials": mats,
            })
    if not meshes_out:
        raise SystemExit("gltf_import: 没有可导入的三角网格")
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for m in meshes_out:
        dest = os.path.join(out_dir, m["name"] + ".mesh.json")
        with open(dest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(m, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write("\n")
        written.append(dest)
    return written, sorted(images)


def main(argv):
    ap = argparse.ArgumentParser(description="glTF/GLB → maque .mesh.json + 贴图（离线、确定性）")
    ap.add_argument("input")
    ap.add_argument("--out", required=True, help="输出目录")
    ap.add_argument("--name", default=None, help="输出基名（默认取输入文件名）")
    args = ap.parse_args(argv)
    written, images = import_gltf(args.input, args.out, args.name)
    for w in written:
        print(f"gltf_import: → {w}")
    for img in images:
        print(f"gltf_import: texture → textures/{img}")
    print(f"gltf_import: OK meshes={len(written)} textures={len(images)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
