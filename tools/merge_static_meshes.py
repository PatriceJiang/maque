#!/usr/bin/env python3
"""Merge static meshes into a few `.mesh.json` files (plans/36 / assessment P0).

Why: maque draws one 3D entity = one draw call. A scene made of hundreds of props
must be merged by material into a handful of meshes; this tool bakes each instance's
TRS into the vertices and concatenates them.

Manifest (JSON):
    {
      "name": "city",
      "groups": [
        {
          "material": { ...asset.PbrMaterial fields... },   # optional
          "meshes": [
            {"path": "a.mesh.json", "position": [x,y,z],
             "rotation_deg": [x,y,z], "scale": [sx,sy,sz]},
            ...
          ]
        }
      ]
    }

Output (per group):
    <out>/<name>_g0.mesh.json, ...   (single group → <name>.mesh.json)

Only static triangle meshes with layout 0/1 (48B/56B canonical vertex rows) are
accepted; skinned/morph assets are rejected explicitly. Output is deterministic:
groups/instances keep manifest order; JSON uses sorted keys + 2-space indent.
"""
import argparse
import json
import math
import os
import sys

ROW_FLOATS = {0: 12, 1: 14}          # layout → floats per vertex (pos3|nrm3|tan4|uv2 [+color4|uv1 2])


def load_mesh(path):
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    layout = int(data.get("vertex_layout", 0))
    if layout not in ROW_FLOATS:
        raise ValueError(f"{path}: vertex_layout {layout} 不支持（只接受 0/1；蒙皮/morph 请勿合并）")
    stride = int(data.get("vertex_stride", ROW_FLOATS[layout] * 4))
    expected = ROW_FLOATS[layout] * 4
    if stride != expected:
        raise ValueError(f"{path}: vertex_stride={stride} 与 layout={layout}（{expected}B）不一致")
    verts = data["vertices"]
    if not isinstance(verts, list) or (verts and not isinstance(verts[0], list)):
        raise ValueError(f"{path}: vertices 必须是逐顶点数组（[[float...], ...]）")
    for row in verts:
        if len(row) != ROW_FLOATS[layout]:
            raise ValueError(f"{path}: 顶点行宽 {len(row)} != layout={layout} 应有 {ROW_FLOATS[layout]}")
    if data.get("morph"):
        raise ValueError(f"{path}: 含 morph 数据，不能做静态合并")
    return data, data["indices"], verts, layout


def euler_matrix(deg):
    """与引擎 m4_from_trs 同约定（three.js 'ZYX' 结果）的 3x3 旋转。"""
    x, y, z = (math.radians(float(v)) for v in deg)
    cx, sx = math.cos(x), math.sin(x)
    cy, sy = math.cos(y), math.sin(y)
    cz, sz = math.cos(z), math.sin(z)
    return (
        (cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz),
        (cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz),
        (-sy, sx * cy, cx * cy),
    )


def transform_vertex(row, layout, r, pos, scale):
    """Bake TRS into one canonical row (layout 0: pos3/nrm3/tan4/uv2; layout 1: pos3/nrm3/color4/uv0/uv1)."""
    sx, sy, sz = scale
    px, py, pz = row[0] * sx, row[1] * sy, row[2] * sz
    out = [r[0][0] * px + r[0][1] * py + r[0][2] * pz + pos[0],
           r[1][0] * px + r[1][1] * py + r[1][2] * pz + pos[1],
           r[2][0] * px + r[2][1] * py + r[2][2] * pz + pos[2]]
    # 法线用逆转置（R · diag(1/sx,1/sy,1/sz)）再归一化；非均匀缩放/镜像都正确。
    nx, ny, nz = row[3] / sx, row[4] / sy, row[5] / sz
    n = _normalize((r[0][0] * nx + r[0][1] * ny + r[0][2] * nz,
                    r[1][0] * nx + r[1][1] * ny + r[1][2] * nz,
                    r[2][0] * nx + r[2][1] * ny + r[2][2] * nz))
    out += [n[0], n[1], n[2]]
    if layout == 0:
        # 切线 = R·S·t（方向），w 保留手性；uv 原样。
        tx, ty, tz = row[6] * sx, row[7] * sy, row[8] * sz
        t = _normalize((r[0][0] * tx + r[0][1] * ty + r[0][2] * tz,
                        r[1][0] * tx + r[1][1] * ty + r[1][2] * tz,
                        r[2][0] * tx + r[2][1] * ty + r[2][2] * tz))
        out += [t[0], t[1], t[2], row[9], row[10], row[11]]
    else:
        # layout 1：color4 + uv0(2) + uv1(2) 原样
        out += list(row[6:14])
    return out


def _normalize(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if l < 1e-20:
        return (0.0, 0.0, 0.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def merge_group(instances, out_name):
    """instances: [(data, indices, verts, layout, position, rotation_deg, scale)]"""
    layout = instances[0][3]
    row_floats = ROW_FLOATS[layout]
    verts_out = []
    idx_out = []
    bmin = [math.inf, math.inf, math.inf]
    bmax = [-math.inf, -math.inf, -math.inf]
    vcount = 0
    for data, indices, verts, lay, pos, rot_deg, scale in instances:
        if lay != layout:
            raise ValueError("同组内 vertex_layout 必须一致")
        r = euler_matrix(rot_deg)
        base = vcount
        for row in verts:
            out = transform_vertex(row, layout, r, pos, scale)
            verts_out.append(out)
            for a in range(3):
                bmin[a] = min(bmin[a], out[a])
                bmax[a] = max(bmax[a], out[a])
            vcount += 1
        idx_out.extend(int(k) + base for k in indices)
    mats = []
    for inst in instances:
        m = inst[0].get("materials") or []
        if m:
            mats = m
            break
    return {
        "name": out_name,
        "vertex_layout": layout,
        "vertex_stride": row_floats * 4,
        "vertices": verts_out,
        "indices": idx_out,
        "bounds_min": bmin,
        "bounds_max": bmax,
        "materials": mats,
    }


def instance_of(base_dir, spec):
    path = spec["path"]
    if not os.path.isabs(path):
        path = os.path.join(base_dir, path)
    data, indices, verts, layout = load_mesh(path)
    pos = tuple(float(v) for v in spec.get("position", [0.0, 0.0, 0.0]))
    rot = tuple(float(v) for v in spec.get("rotation_deg", [0.0, 0.0, 0.0]))
    scale = tuple(float(v) for v in spec.get("scale", [1.0, 1.0, 1.0]))
    if any(abs(s) < 1e-9 for s in scale):
        raise ValueError(f"{path}: scale 分量不能为 0")
    return (data, indices, verts, layout, pos, rot, scale)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Merge static .mesh.json instances (bake TRS).")
    ap.add_argument("manifest", help="manifest JSON 路径")
    ap.add_argument("--out", required=True, help="输出目录")
    ap.add_argument("--name", default=None, help="输出基名（默认 manifest.name）")
    args = ap.parse_args(argv)

    with open(args.manifest, "r", encoding="utf-8") as f:
        man = json.load(f)
    base_dir = os.path.dirname(os.path.abspath(args.manifest))
    name = args.name or man.get("name") or "merged"
    groups = man.get("groups")
    if not groups:
        raise SystemExit("manifest 缺少 groups[]")
    os.makedirs(args.out, exist_ok=True)
    written = []
    for gi, group in enumerate(groups):
        specs = group.get("meshes") or []
        if not specs:
            continue
        instances = [instance_of(base_dir, s) for s in specs]
        suffix = "" if len(groups) == 1 else f"_g{gi}"
        out_name = f"{name}{suffix}"
        merged = merge_group(instances, out_name)
        if group.get("material") is not None:
            merged["materials"] = [group["material"]]
        out_path = os.path.join(args.out, out_name + ".mesh.json")
        with open(out_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(merged, f, sort_keys=True, indent=2)
            f.write("\n")
        written.append((out_path, len(merged["indices"]) // 3, len(merged["vertices"])))
    for path, tris, verts in written:
        print(f"merge_static_meshes: {path}  vertices={verts} triangles={tris}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
