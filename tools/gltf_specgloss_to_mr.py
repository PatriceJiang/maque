#!/usr/bin/env python3
"""把 GLB 的 KHR_materials_pbrSpecularGlossiness 材质离线转成 metallic-roughness。

运行时/离线 importer 的 SG 近似只做因子级（引擎贴图槽没有通道重映射）；要还原官方样例的
「瓶身金属金、瓶盖红、底座黑」，必须逐像素合成：

  baseColor        = lerp(diffuse, specular.rgb, metallic)   （metallic 掩码 = specular 亮度）
  metallicRoughness: G = 1 - specular.a (glossiness), B = metallic

对每个 SG 材质：解码 diffuse/specularGlossiness 两张图，写 baseColor.jpg + metallicRoughness.png，
追加 image/texture/bufferView，改写 material.pbrMetallicRoughness，移除 SG 扩展。
无 specularGlossinessTexture 的材质只做因子映射（diffuse→baseColor、metallic=0、roughness=1-gloss）。

依赖 Pillow。用法：
  python tools/gltf_specgloss_to_mr.py <model.glb> [--in-place | --out <dst.glb>]
"""
from __future__ import annotations

import argparse
import io
import json
import os
import sys

import gltf_webp_to_png as glb


def _tex_image(js, tinfo):
    if not tinfo:
        return None
    tex = (js.get("textures") or [])[tinfo["index"]]
    src = tex.get("source")
    if src is None:
        for ext in (tex.get("extensions") or {}).values():
            if isinstance(ext, dict) and "source" in ext:
                src = ext["source"]
                break
    return src


def _image_bytes(js, bin_chunk, idx, base_dir):
    img = (js.get("images") or [])[idx]
    if img.get("uri"):
        if img["uri"].startswith("data:"):
            import base64
            return base64.b64decode(img["uri"].split(",", 1)[1])
        return open(os.path.join(base_dir, img["uri"]), "rb").read()
    bv = js["bufferViews"][img["bufferView"]]
    off = bv.get("byteOffset", 0)
    return bin_chunk[off:off + bv["byteLength"]]


def _texture_sampler(js, tinfo):
    if not tinfo:
        return None
    return (js.get("textures") or [])[tinfo["index"]].get("sampler")


def convert(src, dst):
    src_size = os.path.getsize(src)
    js, bin_chunk = glb.read_glb(src)
    base_dir = os.path.dirname(os.path.abspath(src))
    images = js.setdefault("images", [])
    textures = js.setdefault("textures", [])
    views = js.setdefault("bufferViews", [])
    materials = js.get("materials") or []
    additions = []          # (image_index, bytes)
    converted = 0

    for mat in materials:
        exts = mat.get("extensions") or {}
        sg = exts.get("KHR_materials_pbrSpecularGlossiness")
        if not sg:
            continue
        spec_factor = [float(x) for x in sg.get("specularFactor", [1.0, 1.0, 1.0])]
        gloss = max(0.0, min(1.0, float(sg.get("glossinessFactor", 1.0))))
        diff_factor = [float(x) for x in sg.get("diffuseFactor", [1.0, 1.0, 1.0, 1.0])]
        sg_tex = sg.get("specularGlossinessTexture")
        diff_tex = sg.get("diffuseTexture")
        metal_style = bool(sg_tex) and max(spec_factor) > 0.01

        if metal_style:
            try:
                from PIL import Image
                di = _tex_image(js, diff_tex)
                si = _tex_image(js, sg_tex)
                if di is None or si is None:
                    raise ValueError("缺少 diffuseTexture/specularGlossinessTexture")
                diff = Image.open(io.BytesIO(_image_bytes(js, bin_chunk, di, base_dir))).convert("RGBA")
                spec = Image.open(io.BytesIO(_image_bytes(js, bin_chunk, si, base_dir))).convert("RGBA")
                if spec.size != diff.size:
                    spec = spec.resize(diff.size)
                metal = spec.convert("L")
                base = Image.composite(spec.convert("RGB"), diff.convert("RGB"), metal)
                rough = spec.getchannel("A").point(lambda a: 255 - a)
                white = Image.new("L", diff.size, 255)
                mr = Image.merge("RGB", (white, rough, metal))

                bio = io.BytesIO()
                if diff.mode == "RGBA" and diff.getextrema()[3][0] < 255:
                    base_rgba = base.convert("RGBA")
                    base_rgba.putalpha(diff.getchannel("A"))
                    base_rgba.save(bio, "PNG", optimize=True)
                    base_mime, base_ext = "image/png", "png"
                else:
                    base.save(bio, "JPEG", quality=91, optimize=True, subsampling=0)
                    base_mime, base_ext = "image/jpeg", "jpg"
                base_bytes = bio.getvalue()

                mio = io.BytesIO()
                mr.save(mio, "PNG", optimize=True)
                mr_bytes = mio.getvalue()

                # 新增两张 image（bufferView 在重建 BIN 时补），再建 texture。
                name = str(mat.get("name") or "material")
                images.append({"mimeType": base_mime, "name": name + "_sg_base." + base_ext})
                base_img = len(images) - 1
                additions.append((base_img, base_bytes))
                images.append({"mimeType": "image/png", "name": name + "_sg_mr.png"})
                mr_img = len(images) - 1
                additions.append((mr_img, mr_bytes))

                tex = {"source": base_img}
                if _texture_sampler(js, diff_tex) is not None:
                    tex["sampler"] = _texture_sampler(js, diff_tex)
                textures.append(tex)
                base_tex = len(textures) - 1
                tex = {"source": mr_img}
                if _texture_sampler(js, sg_tex) is not None:
                    tex["sampler"] = _texture_sampler(js, sg_tex)
                textures.append(tex)
                mr_tex = len(textures) - 1

                mat["pbrMetallicRoughness"] = {
                    "baseColorFactor": diff_factor,
                    "baseColorTexture": {"index": base_tex},
                    "metallicFactor": 1.0,
                    "roughnessFactor": 1.0,
                    "metallicRoughnessTexture": {"index": mr_tex},
                }
                print("  material %r: SG 贴图 → baseColor + MR（%d/%d B）"
                      % (name, len(base_bytes), len(mr_bytes)), file=sys.stderr)
            except Exception as exc:  # noqa: BLE001 —— Pillow 缺失/解码失败退回因子映射
                print("  material %r: SG 逐像素转换失败（%s），退回因子映射"
                      % (mat.get("name"), exc), file=sys.stderr)
                metal_style = False

        if not metal_style:
            pbr = {"baseColorFactor": diff_factor, "metallicFactor": 0.0,
                   "roughnessFactor": max(0.04, min(1.0, 1.0 - gloss))}
            if diff_tex:
                pbr["baseColorTexture"] = diff_tex
            mat["pbrMetallicRoughness"] = pbr
            print("  material %r: SG 因子 → MR（metallic=0, roughness=%.3f）"
                  % (mat.get("name"), pbr["roughnessFactor"]), file=sys.stderr)

        exts.pop("KHR_materials_pbrSpecularGlossiness", None)
        if not exts:
            mat.pop("extensions", None)
        converted += 1

    if not converted:
        print("没有 KHR_materials_pbrSpecularGlossiness 材质，跳过", file=sys.stderr)
        return 0

    # 重建 BIN：现有 bufferView 原样重排，新增 image 追加在末尾。
    new_bin = bytearray()

    def append_view(data):
        nonlocal new_bin
        new_bin.extend(b"\x00" * ((-len(new_bin)) % 8))
        bv = {"buffer": 0, "byteOffset": len(new_bin), "byteLength": len(data)}
        views.append(bv)
        new_bin.extend(data)
        return len(views) - 1

    for bv in list(views):
        off = bv.get("byteOffset", 0)
        data = bin_chunk[off:off + bv["byteLength"]]
        new_bin.extend(b"\x00" * ((-len(new_bin)) % 8))
        bv["byteOffset"] = len(new_bin)
        new_bin.extend(data)
    for img_idx, data in additions:
        images[img_idx]["bufferView"] = append_view(data)
    new_bin.extend(b"\x00" * ((-len(new_bin)) % 4))
    (js.get("buffers") or [{}])[0]["byteLength"] = len(new_bin)

    for key in ("extensionsUsed", "extensionsRequired"):
        if key in js:
            js[key] = [x for x in js[key] if x != "KHR_materials_pbrSpecularGlossiness"]
            if not js[key]:
                js.pop(key)

    size = glb.write_glb(js, bytes(new_bin), dst)
    print(json.dumps({"materials": converted, "glb_before": src_size,
                      "glb_after": size}, ensure_ascii=False))
    return converted


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input")
    ap.add_argument("--out", default="")
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args()
    dst = args.input if args.in_place else (args.out or os.path.splitext(args.input)[0] + "_mr.glb")
    if dst == args.input and not args.in_place:
        raise SystemExit("输出与输入相同；用 --in-place 或 --out")
    convert(args.input, dst)


if __name__ == "__main__":
    main()
