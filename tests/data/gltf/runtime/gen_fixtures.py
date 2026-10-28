#!/usr/bin/env python3
"""生成 runtime 直接加载 glTF/GLB 的 e2e 夹具（tests/gltf_runtime_e2e.sh 用）。

用法（仓库根目录）：
    python tests/data/gltf/runtime/gen_fixtures.py

产出（全部为确定性字节，可逐字节重生成；仓库内已入库，平时不必重跑）：
    model.gltf             glTF 2.0 JSON：buffer 用**外部** model.bin、贴图用**外部**
                            textures/checker.png（覆盖外部 URI 解析 + data: URI 之外的路径）
    model.bin              顶点/索引数据（8 个 accessor 共用）
    textures/checker.png   16x16 红白棋盘（8x8 像素格；红=(200,40,40)、白=(240,240,240)）
    model.glb              GLB：buffer 与 PNG 都内嵌（BIN chunk；image 用 bufferView、无 uri）
    model_data_uri.gltf    同几何/材质，但 buffer 与 image 都用 data: URI（内嵌 base64）

几何（确定性）：
  · 1 个 node / 1 个 mesh / **2 个 primitive**（多 primitive 支持）
  · primitive 0 = 贴图盒（左，x 中心 -0.5，尺寸 0.9）→ 材质 0（baseColorTexture=checker）
  · primitive 1 = 纯色盒（右，x 中心 +0.5，尺寸 0.9）→ 材质 1（baseColor=[0.10,0.30,0.85]）
  · 每个 primitive 均含 POSITION/NORMAL/TEXCOORD_0 + indices（uint16）
  · 几何总尺寸 1.9 x 0.9 x 0.9，以原点为中心 → 场景相机取景与 importer 自动机位同源
"""
from __future__ import annotations

import base64
import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

BOX_HALF = 0.45                 # 0.9 边长
BOX_XS = (-0.5, 0.5)            # 两个盒子中心 x
TEX_SIZE = 16
TEX_CELL = 8                    # 2x2 棋盘格
TEX_RED = (200, 40, 40)
TEX_WHITE = (240, 240, 240)


def box_geometry(cx: float) -> tuple[list[list[float]], list[int]]:
    """以 (cx, 0, 0) 为中心的立方体：24 顶点（每面 4 个，逐面法线）+ 36 索引（CCW 朝外）。

    顶点行 = [px,py,pz, nx,ny,nz, u,v]（POSITION/NORMAL/TEXCOORD_0 各一个 accessor 引用同一 bufferView 段）。
    """
    h = BOX_HALF
    # 每个面：(法线, 4 个角点，从面外侧看为 CCW)，UV 与角点顺序一致
    faces = [
        ((1.0, 0.0, 0.0), [(+h, -h, +h), (+h, -h, -h), (+h, +h, -h), (+h, +h, +h)]),
        ((-1.0, 0.0, 0.0), [(-h, -h, -h), (-h, -h, +h), (-h, +h, +h), (-h, +h, -h)]),
        ((0.0, 1.0, 0.0), [(-h, +h, +h), (+h, +h, +h), (+h, +h, -h), (-h, +h, -h)]),
        ((0.0, -1.0, 0.0), [(-h, -h, -h), (+h, -h, -h), (+h, -h, +h), (-h, -h, +h)]),
        ((0.0, 0.0, 1.0), [(-h, -h, +h), (+h, -h, +h), (+h, +h, +h), (-h, +h, +h)]),
        ((0.0, 0.0, -1.0), [(+h, -h, -h), (-h, -h, -h), (-h, +h, -h), (+h, +h, -h)]),
    ]
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    verts: list[list[float]] = []
    idx: list[int] = []
    for normal, corners in faces:
        base = len(verts)
        for (x, y, z), (u, v) in zip(corners, uvs):
            verts.append([round(cx + x, 6), round(y, 6), round(z, 6),
                          normal[0], normal[1], normal[2], u, v])
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return verts, idx


def make_checker_png() -> bytes:
    """16x16 红白棋盘 PNG（RGB 8-bit，无 alpha；只用标准库 zlib+struct）。"""
    rows = []
    for y in range(TEX_SIZE):
        row = bytearray(b"\x00")        # filter type 0
        for x in range(TEX_SIZE):
            red = ((x // TEX_CELL) + (y // TEX_CELL)) % 2 == 0
            row.extend(TEX_RED if red else TEX_WHITE)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(typ: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", TEX_SIZE, TEX_SIZE, 8, 2, 0, 0, 0)  # RGB, non-interlaced
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def build_bin() -> tuple[bytes, list[dict], list[dict]]:
    """顶点/索引 → BIN 字节 + bufferViews + accessors。

    布局（每个 primitive 4 个 view，偏移均 4 字节对齐）：
        [0] POSITION   24*12 = 288
        [1] NORMAL     24*12 = 288
        [2] TEXCOORD_0 24*8  = 192
        [3] indices    36*2  =  72
    """
    views: list[dict] = []
    accessors: list[dict] = []
    data = bytearray()

    def add_view(blob: bytes, target: int) -> int:
        assert len(data) % 4 == 0, len(data)
        views.append({"buffer": 0, "byteOffset": len(data), "byteLength": len(blob), "target": target})
        data.extend(blob)
        return len(views) - 1

    for cx in BOX_XS:
        verts, idx = box_geometry(cx)
        pos = b"".join(struct.pack("<3f", *v[0:3]) for v in verts)
        nrm = b"".join(struct.pack("<3f", *v[3:6]) for v in verts)
        uv = b"".join(struct.pack("<2f", *v[6:8]) for v in verts)
        ind = b"".join(struct.pack("<H", i) for i in idx)
        vp = add_view(pos, 34962)
        vn = add_view(nrm, 34962)
        vu = add_view(uv, 34962)
        vi = add_view(ind, 34963)
        px = [v[0] for v in verts]
        py = [v[1] for v in verts]
        pz = [v[2] for v in verts]
        accessors.extend([
            {"bufferView": vp, "componentType": 5126, "count": len(verts), "type": "VEC3",
             "min": [round(min(px), 6), round(min(py), 6), round(min(pz), 6)],
             "max": [round(max(px), 6), round(max(py), 6), round(max(pz), 6)]},
            {"bufferView": vn, "componentType": 5126, "count": len(verts), "type": "VEC3"},
            {"bufferView": vu, "componentType": 5126, "count": len(verts), "type": "VEC2"},
            {"bufferView": vi, "componentType": 5123, "count": len(idx), "type": "SCALAR"},
        ])
    return bytes(data), views, accessors


def base_json(bin_len: int, views: list[dict], accessors: list[dict]) -> dict:
    return {
        "asset": {"version": "2.0", "generator": "maque tests/data/gltf/runtime/gen_fixtures.py"},
        "scene": 0,
        "scenes": [{"name": "Scene", "nodes": [0]}],
        "nodes": [{"name": "model", "mesh": 0}],
        "meshes": [{
            "name": "model",
            "primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                 "indices": 3, "material": 0},
                {"attributes": {"POSITION": 4, "NORMAL": 5, "TEXCOORD_0": 6},
                 "indices": 7, "material": 1},
            ],
        }],
        "materials": [
            {   # 材质 0：贴图盒 —— baseColor 白，靠 baseColorTexture（红白棋盘）上色
                "name": "checker_textured",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.6,
                },
                "doubleSided": False,
            },
            {   # 材质 1：纯色蓝（金属度 0 / 粗糙度 0.55 → 漫反射为主，颜色稳定）
                "name": "solid_blue",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.10, 0.30, 0.85, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 0.55,
                },
                "emissiveFactor": [0.0, 0.0, 0.0],
                "doubleSided": False,
            },
        ],
        "textures": [{"sampler": 0, "source": 0}],
        "samplers": [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": bin_len}],
    }


def write_json(path: str, obj: dict) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=False)
        f.write("\n")


def build_glb(js: dict, bin_data: bytes, png: bytes) -> bytes:
    """GLB：JSON chunk（空格补齐 4 字节）+ BIN chunk（几何 + 对齐 + PNG，零补齐）。"""
    js = json.loads(json.dumps(js))         # 深拷贝
    assert "uri" not in js["buffers"][0]
    js["buffers"][0]["byteLength"] = len(bin_data) + len(png)
    blob = bytearray(bin_data)
    blob.extend(png)
    body = bytes(blob)
    pad = (-len(body)) % 4
    body += b"\x00" * pad
    js_text = json.dumps(js, ensure_ascii=False, indent=2, sort_keys=False).encode("utf-8")
    js_pad = (-len(js_text)) % 4
    js_text += b" " * js_pad
    total = 12 + 8 + len(js_text) + 8 + len(body)
    out = struct.pack("<III", 0x46546C67, 2, total)
    out += struct.pack("<II", len(js_text), 0x4E4F534A) + js_text
    out += struct.pack("<II", len(body), 0x004E4942) + body
    return out


def main() -> None:
    bin_data, views, accessors = build_bin()
    png = make_checker_png()
    os.makedirs(os.path.join(HERE, "textures"), exist_ok=True)
    with open(os.path.join(HERE, "textures", "checker.png"), "wb") as f:
        f.write(png)
    with open(os.path.join(HERE, "model.bin"), "wb") as f:
        f.write(bin_data)

    # ---- model.gltf：外部 buffer + 外部贴图（uri 相对 model.gltf 所在目录）----
    js_external = base_json(len(bin_data), views, accessors)
    js_external["buffers"] = [{"uri": "model.bin", "byteLength": len(bin_data)}]
    js_external["images"] = [{"name": "checker", "uri": "textures/checker.png"}]
    write_json(os.path.join(HERE, "model.gltf"), js_external)

    # ---- model.glb：BIN chunk 内嵌 buffer + PNG（image 用 bufferView，不写 uri）----
    js_glb = base_json(len(bin_data), views, accessors)
    js_glb["bufferViews"] = list(views) + [
        {"buffer": 0, "byteOffset": len(bin_data), "byteLength": len(png)}]
    js_glb["images"] = [{"name": "checker", "bufferView": len(views), "mimeType": "image/png"}]
    with open(os.path.join(HERE, "model.glb"), "wb") as f:
        f.write(build_glb(js_glb, bin_data, png))

    # ---- model_data_uri.gltf：buffer 与 image 都用 data: URI（内嵌 base64）----
    js_data = base_json(len(bin_data), views, accessors)
    js_data["buffers"] = [{
        "uri": "data:application/octet-stream;base64," + base64.b64encode(bin_data).decode("ascii"),
        "byteLength": len(bin_data)}]
    js_data["images"] = [{
        "name": "checker",
        "uri": "data:image/png;base64," + base64.b64encode(png).decode("ascii"),
        "mimeType": "image/png"}]
    write_json(os.path.join(HERE, "model_data_uri.gltf"), js_data)

    print(f"gen_fixtures: bin={len(bin_data)}B png={len(png)}B "
          f"glb={os.path.getsize(os.path.join(HERE, 'model.glb'))}B")
    print("gen_fixtures: 写入 model.gltf / model.bin / textures/checker.png / "
          "model.glb / model_data_uri.gltf")


if __name__ == "__main__":
    main()
