#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""批量迁移 Khronos glTF-Sample-Assets 的 Models 到 maque 场景。

对每个模型：
  1. 选主变体（优先 `glTF/<Model>.gltf`，否则 `glTF` 目录下第一个 `.gltf`）；
  2. `tools/gltf_import.py --scene --compact` → `scenes/khronos/<Model>/`
     （`assets/*.mesh.json` + `textures/*` 为派生资产，体积大 → .gitignore 不入库；
      可用本脚本/README 里的命令从 `.deps/gltf-assets` 重新生成）；
  3. maque_player `--shot` 渲染 1280x720 → 本地 `preview.png`（不入库）+ 提交用 `preview.jpg`（800x450）；
  4. 与官方 `screenshot/` 参考图做定量对比（亮度/直方图/边缘密度/色度）；
  5. 生成 per-model `README.md` 笔记（来源/许可/模型统计/命令/对比/踩坑/已知差距）；
  6. 汇总 `scenes/khronos/README.md` 索引 + `scenes/khronos/manifest.json`。

用法（仓库根目录）：
  python tools/migrate_gltf_models.py                       # 全部模型
  python tools/migrate_gltf_models.py --only Box,BoxAnimated # 指定模型
  python tools/migrate_gltf_models.py --skip-existing        # 已迁移的跳过
  python tools/migrate_gltf_models.py --no-render --no-notes # 只导入

设计约束：
  * 不修改引擎/其他工具；只写 `scenes/khronos/<Model>/`、`scenes/khronos/README.md`、
    `scenes/khronos/manifest.json`；
  * per-model README 中 `<!-- 手动笔记开始 -->` 之后的内容在重生成时保留（手改不丢）；
  * 单模型失败不中断整批：记录 status/blocked 原因，继续。
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_MODELS_ROOT = os.path.join(REPO, ".deps", "gltf-assets", "glTF-Sample-Assets-main", "Models")
DEFAULT_OUT_ROOT = os.path.join(REPO, "scenes", "khronos")
PLAYER = os.path.join(REPO, "build-win", "samples", "player", "maque_player.exe")
IMPORTER = os.path.join(REPO, "tools", "gltf_import.py")
REFERENCES_URL = "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models"

MANUAL_MARKER = "<!-- 手动笔记开始（重生成时保留以下内容） -->"

# 手动机位（迁移报告/视觉复核判定自动机位不可用的模型；坐标=模型局部世界空间）。
# 生成流程在 importer 之后应用（幂等），README 会记录"手动相机"。
MANUAL_CAMERA = {
    # Sponza 官方参考图是主厅内部视角；自动包围盒拟合把相机放在建筑内部但朝向/远平面
    # 不适配长向柱廊 → 手动放到 +X 端看向 -X（主厅纵深），fov 60 / far 覆盖 2800 单位。
    # 世界空间尺寸约 30×12×18（节点带 ~0.02 缩放，顶点 bounds 是局部值）→ 内部机位用米级坐标：
    # 主厅沿 X 长 30，把相机放在 +X 端柱廊内（x≈11,y≈3,z≈0）向 -X 看，fov 60。
    "Sponza": {"position": (9.5, 1.7, 0.0), "target": (-13.0, 2.2, 0.0),
               "fov": 65.0, "near": 0.05, "far": 100.0,
               "note": "手动机位：主厅内部沿 -X 看（官方参考图为内部视角）"},
}


def _look_rotation(eye, target):
    d = [target[i] - eye[i] for i in range(3)]
    ln = math.sqrt(sum(v * v for v in d)) or 1.0
    d = [v / ln for v in d]
    return math.asin(max(-1.0, min(1.0, d[1]))), math.atan2(-d[0], -d[2])


def apply_camera_override(scene_path, spec):
    """把 MANUAL_CAMERA 的显式机位写进 .scn（幂等；保持 importer 的 JSON 格式）。"""
    if not os.path.isfile(scene_path):
        return False
    try:
        scn = load_json(scene_path)
    except Exception:  # noqa: BLE001
        return False
    changed = False
    for ent in scn.get("entities") or []:
        comps = ent.get("components") or []
        is_cam = any(c.get("type") == "render.Camera3D" for c in comps)
        if not is_cam:
            continue
        pitch, yaw = _look_rotation(spec["position"], spec["target"])
        for c in comps:
            if c.get("type") == "scene.Transform3D":
                c["local"]["position"] = {"x": spec["position"][0], "y": spec["position"][1],
                                          "z": spec["position"][2]}
                c["local"]["rotation"] = {"x": pitch, "y": yaw, "z": 0.0}
                changed = True
            elif c.get("type") == "render.Camera3D":
                if "fov" in spec:
                    c["fov_y"] = spec["fov"]
                if "near" in spec:
                    c["near"] = spec["near"]
                if "far" in spec:
                    c["far"] = spec["far"]
                if "clear_color" in spec:
                    r, g, b = spec["clear_color"]
                    c["clear_color"] = {"r": r, "g": g, "b": b, "a": 1.0}
                changed = True
    if changed:
        with open(scene_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(scn, f, ensure_ascii=False, indent=2)
            f.write("\n")
    return changed


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))





# ---------------------------------------------------------------- 基础工具
def run(cmd, cwd=None, timeout=600):
    env = dict(os.environ)
    env.setdefault("PYTHONIOENCODING", "utf-8")
    p = subprocess.run(cmd, cwd=cwd or REPO, env=env, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def dir_size(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for fn in files:
            try:
                total += os.path.getsize(os.path.join(root, fn))
            except OSError:
                pass
    return total


def human_size(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0
    return "%.1f GB" % n


def find_reference_image(model_dir):
    """在 `<Model>/screenshot/` 里挑参考图。

    上游格式不统一：png/jpg/jpeg/webp/gif（动画模型常只给 gif，比较时取第一帧）；也有的模型
    `screenshot/` 里混着大量过程图，优先 basename 含 screenshot 的；无 screenshot 目录
    （如 12 个模型）→ 返回 None（笔记里标注"上游无参考图"）。"""
    shot_dir = os.path.join(model_dir, "screenshot")
    if not os.path.isdir(shot_dir):
        return None
    exts = (".png", ".jpg", ".jpeg", ".webp", ".gif")
    files = sorted(f for f in os.listdir(shot_dir) if f.lower().endswith(exts))
    if not files:
        return None
    prio = {".png": 0, ".jpg": 1, ".jpeg": 2, ".webp": 3, ".gif": 4}
    # 0) **large 优先**（screenshot_large / screenshot-Large / large.jpg 等；报告按用户要求优先用大图）
    large = [f for f in files if "large" in f.lower()]
    if large:
        def _large_key(f):
            # 同尺寸优先"canonical"screenshot_large；排除 bloom/变体/对比图等过程图
            low = f.lower()
            penalty = 1 if any(k in low for k in ("bloom", "on-off", "on_off", "variant",
                                                  "compare", "before", "after")) else 0
            canonical = 0 if low.startswith("screenshot") and "large" in low else 1
            return (penalty, canonical, prio.get(os.path.splitext(low)[1], 9), len(f))
        return os.path.join(shot_dir, sorted(large, key=_large_key)[0])
    # 1) 精确 screenshot.*（多格式时 png > jpg > webp > gif）
    exact = [f for f in files if os.path.splitext(f)[0].lower() == "screenshot"]
    if exact:
        return os.path.join(shot_dir, sorted(
            exact, key=lambda f: prio.get(os.path.splitext(f)[1].lower(), 9))[0])
    # 2) basename 含 "screenshot"（screenshot_Large / screenshot-large 等）
    named = [f for f in files if "screenshot" in f.lower()]
    if named:
        return os.path.join(shot_dir, sorted(
            named, key=lambda f: (prio.get(os.path.splitext(f)[1].lower(), 9), len(f)))[0])
    # 3) 兜底：第一张图
    return os.path.join(shot_dir, files[0])


def find_primary_gltf(model_dir):
    gdir = os.path.join(model_dir, "glTF")
    if not os.path.isdir(gdir):
        return None
    files = sorted(f for f in os.listdir(gdir) if f.lower().endswith(".gltf"))
    if not files:
        # 有些模型只有 GLB 变体
        bdir = os.path.join(model_dir, "glTF-Binary")
        if os.path.isdir(bdir):
            glbs = sorted(f for f in os.listdir(bdir) if f.lower().endswith(".glb"))
            return os.path.join(bdir, glbs[0]) if glbs else None
        return None
    base = os.path.basename(model_dir)
    exact = [f for f in files if os.path.splitext(f)[0] == base]
    return os.path.join(gdir, (exact or files)[0])


# ---------------------------------------------------------------- glTF 统计
def gltf_stats(gltf_path):
    """解析 glTF JSON，统计迁移需要的信息（不读 buffer，只看 JSON）。"""
    st = {
        "gltf": os.path.basename(gltf_path),
        "nodes": 0, "meshes": 0, "primitives": 0, "vertices": 0, "triangles": 0,
        "materials": 0, "textures": 0, "images": 0, "animations": 0, "skins": 0,
        "morph_prims": 0, "cameras": 0,
        "extensions_used": [], "extensions_required": [],
        "image_kinds": {}, "texcoord_sets": {}, "alpha_modes": {}, "double_sided": 0,
        "no_normal_prims": 0, "color0_prims": 0, "non_tri_prims": 0,
        "has_khr_lights": False,
    }
    try:
        js = load_json(gltf_path)
    except Exception as e:  # noqa: BLE001
        st["parse_error"] = str(e)
        return st
    st["extensions_used"] = list(js.get("extensionsUsed") or [])
    st["extensions_required"] = list(js.get("extensionsRequired") or [])
    st["nodes"] = len(js.get("nodes") or [])
    st["meshes"] = len(js.get("meshes") or [])
    st["materials"] = len(js.get("materials") or [])
    st["textures"] = len(js.get("textures") or [])
    st["images"] = len(js.get("images") or [])
    st["animations"] = len(js.get("animations") or [])
    st["skins"] = len(js.get("skins") or [])
    st["cameras"] = len(js.get("cameras") or [])

    accessors = js.get("accessors") or []
    for mesh in js.get("meshes") or []:
        for prim in mesh.get("primitives") or []:
            st["primitives"] += 1
            attrs = prim.get("attributes") or {}
            if "POSITION" in attrs and attrs["POSITION"] < len(accessors):
                st["vertices"] += int(accessors[attrs["POSITION"]].get("count") or 0)
            if "indices" in prim and prim["indices"] < len(accessors):
                st["triangles"] += int(accessors[prim["indices"]].get("count") or 0) // 3
            else:
                st["triangles"] += int(accessors[attrs["POSITION"]].get("count") or 0) // 3 if "POSITION" in attrs else 0
            if prim.get("mode", 4) != 4:
                st["non_tri_prims"] += 1
            if "NORMAL" not in attrs:
                st["no_normal_prims"] += 1
            if "COLOR_0" in attrs:
                st["color0_prims"] += 1
            if "targets" in prim:
                st["morph_prims"] += 1
            for key, val in attrs.items():
                if key.startswith("TEXCOORD_"):
                    st["texcoord_sets"][key] = st["texcoord_sets"].get(key, 0) + 1
    for mat in js.get("materials") or []:
        st["alpha_modes"][mat.get("alphaMode", "OPAQUE")] = \
            st["alpha_modes"].get(mat.get("alphaMode", "OPAQUE"), 0) + 1
        if mat.get("doubleSided"):
            st["double_sided"] += 1
        for tinfo in [mat.get("normalTexture"), mat.get("occlusionTexture"), mat.get("emissiveTexture"),
                      (mat.get("pbrMetallicRoughness") or {}).get("baseColorTexture"),
                      (mat.get("pbrMetallicRoughness") or {}).get("metallicRoughnessTexture")]:
            if tinfo and tinfo.get("texCoord", 0):
                st["texcoord_sets"]["textureInfo>0"] = st["texcoord_sets"].get("textureInfo>0", 0) + 1
                if tinfo.get("texCoord", 0) >= 2:
                    st["texcoord_sets"]["textureInfo>=2"] = st["texcoord_sets"].get("textureInfo>=2", 0) + 1
    for img in js.get("images") or []:
        mime = img.get("mimeType") or ""
        uri = (img.get("uri") or "").lower()
        if "jpeg" in mime or uri.endswith(".jpg") or uri.endswith(".jpeg"):
            kind = "jpeg"
        elif "png" in mime or uri.endswith(".png"):
            kind = "png"
        elif "ktx" in mime or uri.endswith(".ktx2"):
            kind = "ktx2"
        elif "webp" in mime or uri.endswith(".webp"):
            kind = "webp"
        elif uri.startswith("data:"):
            kind = "data-uri(" + ("jpeg" if "image/jpeg" in uri else "png") + ")"
        else:
            kind = "bufferView"
        st["image_kinds"][kind] = st["image_kinds"].get(kind, 0) + 1
    st["has_khr_lights"] = "KHR_lights_punctual" in st["extensions_used"]
    return st


# ---------------------------------------------------------------- 差距清单
def known_gaps(stats):
    """把 glTF 特性 → 引擎当前已知差距（供 README「已知差距」与迁移文档汇总）。"""
    used = set(stats.get("extensions_used") or [])
    req = set(stats.get("extensions_required") or [])
    gaps = []
    if stats.get("animations"):
        gaps.append("动画未导入（静态渲染；角色模型为 bind/T-pose）")
    if stats.get("skins"):
        gaps.append("骨骼蒙皮未应用（蒙皮网格按节点世界矩阵烘焙，姿势/形变与官方参考不同）")
    if stats.get("morph_prims"):
        gaps.append("morph targets 未应用（默认权重不生效）")
    if stats.get("cameras"):
        gaps.append("未使用 glTF 自带相机（自动机位，与官方 screenshot 机位不同）")
    if stats.get("has_khr_lights"):
        gaps.append("KHR_lights_punctual 已导入为 Light（≤8 盏、方向光优先；强度为观感归一化、非物理换算；有灯光时不再追加默认太阳）")
    if stats.get("non_tri_prims"):
        gaps.append("非三角形 primitive 被跳过（线/点）")
    if stats.get("color0_prims"):
        if stats.get("texcoord_sets", {}).get("textureInfo>=2"):
            gaps.append("部分贴图使用 TEXCOORD_2+（引擎支持 TEXCOORD_0/1；更高 UV 集会退回 uv0）")
    if "KHR_texture_transform" in used:
        gaps.append("KHR_texture_transform 支持 scale/offset/rotation（基础组与法线组）；逐贴图独立 transform 仍部分近似")
    if "KHR_materials_transmission" in used:
        gaps.append("KHR_materials_transmission 近似为 alpha BLEND（透光可见背景，但无折射/厚度/吸收）")
    if "KHR_materials_volume" in used or "KHR_materials_dispersion" in used or "KHR_materials_volume_scatter" in used:
        gaps.append("KHR_materials_volume 近似为薄壁色调（attenuationColor×baseColor）；dispersion/volume_scatter 未实现")
    if "KHR_materials_diffuse_transmission" in used:
        gaps.append("KHR_materials_diffuse_transmission 近似为 alpha BLEND（乳浊半透观感，无散射）")
    if "KHR_materials_clearcoat" in used:
        gaps.append("KHR_materials_clearcoat：因子/清漆贴图已导入；clearcoat BRDF 为观感近似")
    if "KHR_materials_sheen" in used:
        gaps.append("KHR_materials_sheen：因子/颜色/sheen 贴图已导入；sheen BRDF 为观感近似")
    if "KHR_materials_iridescence" in used:
        gaps.append("KHR_materials_iridescence：薄膜干涉为观感近似（虹彩/厚度贴图已导入）")
    if "KHR_materials_specular" in used:
        gaps.append("KHR_materials_specular：factor/color 已映射；specularTexture/specularColorTexture 与 f90 行为未实现")
    if "KHR_materials_anisotropy" in used:
        gaps.append("KHR_materials_anisotropy：strength/贴图已导入、只做粗糙度近似（各向异性 BRDF/方向未实现）")
    if "KHR_materials_variants" in used:
        gaps.append("KHR_materials_variants：只使用默认材质（不做变体切换）")
    if "KHR_materials_pbrSpecularGlossiness" in used:
        gaps.append("KHR_materials_pbrSpecularGlossiness（旧扩展）未实现")
    if "KHR_texture_basisu" in used or stats.get("image_kinds", {}).get("ktx2"):
        gaps.append("KTX2/BasisU 贴图无解码器（相关贴图会缺失）")
    if "EXT_texture_webp" in used or stats.get("image_kinds", {}).get("webp"):
        gaps.append("WebP 贴图：导入期用 PIL 转 PNG（需 Pillow；无 Pillow 时该贴图缺失）")
    if "KHR_node_visibility" in used:
        gaps.append("KHR_node_visibility 已支持：visible=false 的节点及整棵子树不导入")
    if "EXT_mesh_gpu_instancing" in used:
        gaps.append("EXT_mesh_gpu_instancing 已导入（导入期展开实例；非 GPU 实例化）")
    if "KHR_materials_retroreflection" in used:
        gaps.append("KHR_materials_retroreflection 未实现")
    if "EXT_lights_image_based" in used:
        gaps.append("EXT_lights_image_based 未实现（忽略其 IBL 灯光）")
    if "KHR_animation_pointer" in used:
        gaps.append("KHR_animation_pointer 动画未导入")
    if "KHR_draco_mesh_compression" in req:
        gaps.append("主变体必需 Draco 压缩（无解码器）→ 该变体无法导入")
    if "KHR_mesh_quantization" in req:
        gaps.append("KHR_mesh_quantization（量化顶点）导入未特殊处理（数值/法线精度可能异常）")
    if "EXT_meshopt_compression" in used or "KHR_meshopt_compression" in used:
        gaps.append("meshopt 压缩未实现")
    if stats.get("alpha_modes", {}).get("MASK"):
        gaps.append("alphaMode=MASK 已支持；BLEND 已支持（排序为物体级）")
    return gaps


# ---------------------------------------------------------------- 图片对比
def _foreground_mask(im, np, allow_alpha=True):
    """估计"主体"掩码：allow_alpha 且图像存在**大量全透明像素**时才用 alpha（如官方参考图的透明背景）；
    否则用边框像素中值当背景色、与背景差异大的像素为主体。

    注意：maque 的 --shot 预览 PNG 虽然带 alpha 通道，但 alpha 是 3D pass 写出的材质 alpha
    （OPAQUE 材质的 baseColor.a 也可能是 0），**不是**有效的前景掩码 → 预览图必须 allow_alpha=False。"""
    a = np.asarray(im, dtype=np.float32) / 255.0
    if allow_alpha and a.shape[2] == 4:
        transparent = float((a[..., 3] < 0.02).mean())
        if transparent > 0.02:
            return a[..., 3] > 0.5, "alpha"
    rgb = a[..., :3]
    h, w, _ = rgb.shape
    border = np.concatenate([rgb[0, :, :], rgb[-1, :, :], rgb[:, 0, :], rgb[:, -1, :]], axis=0)
    bg = np.median(border, axis=0)
    dist = np.abs(rgb - bg).max(axis=2)
    mask = dist > 0.06
    # 背景近纯色且主体占比过小（<0.5%）时视为背景检测失败 → 全图
    if mask.mean() < 0.005:
        mask = np.ones_like(mask, dtype=bool)
    return mask, "border-bg"


def _masked_stats(im, np, allow_alpha=True):
    a = np.asarray(im, dtype=np.float32) / 255.0
    mask, kind = _foreground_mask(im, np, allow_alpha=allow_alpha)
    rgb = a[..., :3]
    lum = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    vals = lum[mask] if mask.any() else lum.reshape(-1)
    rgbv = rgb[mask] if mask.any() else rgb.reshape(-1, 3)
    rgba = a[mask] if mask.any() else a.reshape(-1, 4)
    h, w = lum.shape
    ys, xs = np.where(mask)
    bbox = None
    if mask.any():
        bbox = [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]
    return {
        "mean_lum": float(vals.mean()) if vals.size else 0.0,
        "mean_rgb": [float(x) * 255.0 for x in rgbv.mean(axis=0)],
        "mask_kind": kind,
        "coverage": float(mask.mean()),
        "bbox": bbox,
        "alpha_mean": float(rgba[:, 3].mean()) if rgba.shape[1] == 4 else 1.0,
    }


def compare_images(preview_path, reference_path, out_json):
    """预览图 vs 官方 screenshot 的定量对比（PIL/numpy 可选；失败返回 None）。

    对比在"主体掩码"上做（参考图若有 alpha 用 alpha；否则用边框背景色估计），
    避免背景/机位差异主导统计。指标只用于筛查，不作像素级 pass/fail。"""
    try:
        from PIL import Image
        import numpy as np
    except Exception:  # noqa: BLE001
        return None
    try:
        ref = Image.open(reference_path).convert("RGBA")
        cur = Image.open(preview_path).convert("RGBA")
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}

    ref_st = _masked_stats(ref, np, allow_alpha=True)
    cur_st = _masked_stats(cur, np, allow_alpha=False)   # 预览 PNG 的 alpha 不是前景掩码
    # 统一缩放到 256x256，在掩码上交集中比较
    ref_small = ref.resize((256, 256), Image.LANCZOS)
    cur_small = cur.resize((256, 256), Image.LANCZOS)
    mask_a, _ = _foreground_mask(ref_small, np, allow_alpha=True)
    mask_b, _ = _foreground_mask(cur_small, np, allow_alpha=False)
    both = mask_a & mask_b
    a = np.asarray(ref_small.convert("RGB"), dtype=np.float32) / 255.0
    b = np.asarray(cur_small.convert("RGB"), dtype=np.float32) / 255.0
    if both.sum() < 64:            # 掩码几乎不重叠（机位/构图差异极大）→ 退化到整图
        both = np.ones((256, 256), dtype=bool)
    la = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]
    lb = 0.2126 * b[..., 0] + 0.7152 * b[..., 1] + 0.0722 * b[..., 2]
    hist_a, _ = np.histogram(la[both], bins=16, range=(0, 1))
    hist_b, _ = np.histogram(lb[both], bins=16, range=(0, 1))
    hist_sim = float(np.minimum(hist_a, hist_b).sum() / max(1, hist_a.sum()))
    # 颜色相似度：主体平均色的 RGB 距离（0..1）与掩码覆盖率差异
    rgb_delta = float(np.abs(np.asarray(ref_st["mean_rgb"]) - np.asarray(cur_st["mean_rgb"])).mean() / 255.0)
    cov_ratio = cur_st["coverage"] / ref_st["coverage"] if ref_st["coverage"] > 1e-6 else None
    result = {
        "reference": os.path.basename(reference_path),
        "reference_size": list(ref.size),
        "reference_has_alpha": bool(ref.getextrema()[3][0] < 255),
        "preview_size": list(cur.size),
        "ref_mask": ref_st["mask_kind"], "preview_mask": cur_st["mask_kind"],
        "ref_mean_lum": round(ref_st["mean_lum"], 4),
        "preview_mean_lum": round(cur_st["mean_lum"], 4),
        "mean_lum_delta": round(cur_st["mean_lum"] - ref_st["mean_lum"], 4),
        "hist_similarity": round(hist_sim, 4),
        "rgb_delta": round(rgb_delta, 4),
        "ref_coverage": round(ref_st["coverage"], 4),
        "preview_coverage": round(cur_st["coverage"], 4),
        "coverage_ratio": round(cov_ratio, 3) if cov_ratio else None,
        "ref_bbox": ref_st["bbox"], "preview_bbox": cur_st["bbox"],
        "ref_mean_rgb": [round(x, 1) for x in ref_st["mean_rgb"]],
        "preview_mean_rgb": [round(x, 1) for x in cur_st["mean_rgb"]],
    }
    # 构图告警：主体太小/贴边/覆盖异常
    notes = []
    if result["preview_coverage"] < 0.05:
        notes.append("预览主体占比过小（<5%）→ 机位过远？")
    bbox = cur_st["bbox"]
    if bbox:
        w, h = cur.size
        if bbox[0] <= 1 or bbox[1] <= 1 or bbox[2] >= w - 2 or bbox[3] >= h - 2:
            notes.append("预览主体贴边/被裁剪 → 机位过近？")
    result["framing_notes"] = notes
    # 粗略结论（前景统计）
    if hist_sim > 0.6 and abs(result["mean_lum_delta"]) < 0.2:
        verdict = "主体亮度/色调分布较接近（机位/背景可能不同）"
    elif hist_sim > 0.4:
        verdict = "中等差异（多为机位/背景/光照/缺失特性）"
    else:
        verdict = "差异较大（需人工确认：机位/缺失特性/导入问题）"
    if notes:
        verdict += "；" + "；".join(notes)
    result["verdict"] = verdict
    try:
        with open(out_json, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
    except OSError:
        pass
    return result


# ---------------------------------------------------------------- 生成笔记
def read_manual_notes(readme_path):
    if not os.path.isfile(readme_path):
        return ""
    with open(readme_path, "r", encoding="utf-8") as f:
        text = f.read()
    idx = text.find(MANUAL_MARKER)
    return text[idx:] if idx >= 0 else ""


def write_readme(path, model, meta, stats, gaps, import_info, compare, issues, manual_notes):
    legal = (meta or {}).get("legal") or []
    lic_lines = []
    for item in legal:
        lic_lines.append("- %s（%s）%s，%s %s" % (
            item.get("text") or item.get("license") or "?",
            item.get("spdx") or item.get("license") or "?",
            item.get("licenseUrl") or "",
            item.get("artist") or item.get("owner") or "",
            item.get("year") or ""))
    if not lic_lines:
        lic_lines.append("- 见上游 `Models/%s/metadata.json`" % model)

    lines = []
    lines.append("# %s —— Khronos glTF 示例迁移" % model)
    lines.append("")
    lines.append("**来源**：[KhronosGroup/glTF-Sample-Assets · Models/%s](%s/%s)  " % (model, REFERENCES_URL, model))
    lines.append("**上游概览**：%s" % ((meta or {}).get("summary") or "（见上游 README）"))
    tags = (meta or {}).get("tags") or []
    if tags:
        lines.append("**标签**：%s  " % "、".join("`%s`" % t for t in tags))
    lines.append("**许可**：（资产元数据，保留出处）")
    lines.extend(lic_lines)
    lines.append("")
    lines.append("> 本目录由 `python tools/migrate_gltf_models.py --only %s` 生成；"
                 "`assets/`、`textures/` 为派生资产（体积大，不入库），"
                 "需从 `.deps/gltf-assets` 重新生成，步骤见 `scenes/khronos/README.md`。" % model)
    lines.append("")
    lines.append("## 模型统计")
    lines.append("")
    lines.append("| 项 | 值 |")
    lines.append("|---|---|")
    lines.append("| 主 glTF 文件 | `%s` |" % stats.get("gltf", "?"))
    lines.append("| 节点 / 网格 / primitive | %d / %d / %d |" % (
        stats.get("nodes", 0), stats.get("meshes", 0), stats.get("primitives", 0)))
    lines.append("| 顶点 / 三角形 | %s / %s |" % (f"{stats.get('vertices', 0):,}", f"{stats.get('triangles', 0):,}"))
    lines.append("| 材质 / 纹理 / 图像 | %d / %d / %d |" % (
        stats.get("materials", 0), stats.get("textures", 0), stats.get("images", 0)))
    kinds = stats.get("image_kinds") or {}
    if kinds:
        lines.append("| 图像编码 | %s |" % "、".join("%s×%d" % (k, v) for k, v in sorted(kinds.items())))
    lines.append("| 动画 / 骨骼 / morph primitive / 相机 | %d / %d / %d / %d |" % (
        stats.get("animations", 0), stats.get("skins", 0), stats.get("morph_prims", 0), stats.get("cameras", 0)))
    used = stats.get("extensions_used") or []
    req = stats.get("extensions_required") or []
    if used:
        lines.append("| 使用扩展 | %s |" % "、".join("`%s`" % e for e in used))
    if req:
        lines.append("| 必需扩展 | %s |" % "、".join("`%s`" % e for e in req))
    lines.append("| alpha 模式分布 | %s |" % "、".join("%s×%d" % (k, v) for k, v in sorted((stats.get("alpha_modes") or {}).items())))
    lines.append("")
    lines.append("**导入产物**：`assets/` %d 个 `.mesh.json`（合计 %s），`textures/` %d 张；场景 `%s`。" % (
        import_info.get("mesh_files", 0), human_size(import_info.get("mesh_bytes", 0)),
        import_info.get("texture_files", 0), import_info.get("scene", "?")))
    lines.append("")
    lines.append("## 生成 / 运行")
    lines.append("")
    lines.append("```bash")
    lines.append("# 从 Khronos 源资产重新生成（仓库根目录；源资产需先解包到 .deps/gltf-assets）")
    lines.append("python tools/migrate_gltf_models.py --only %s --force" % model)
    lines.append("# 或单独导入：")
    lines.append("python tools/gltf_import.py .deps/gltf-assets/glTF-Sample-Assets-main/Models/%s/glTF/%s "
                 "--out scenes/khronos/%s --name %s --scene --compact" % (
                     model, stats.get("gltf", model + ".gltf"), model, model))
    lines.append("# 运行 / 截图")
    lines.append("./build-win/samples/player/maque_player scenes/khronos/%s/%s.scn" % (model, model))
    lines.append("./build-win/samples/player/maque_player --shot /tmp/%s.png scenes/khronos/%s/%s.scn" % (model, model, model))
    lines.append("```")
    lines.append("")
    lines.append("## 与官方 screenshot 对比")
    lines.append("")
    if compare and "error" not in compare:
        lines.append("参考图：`screenshot/%s`（%s%s）。以下统计只在前景掩码上计算（参考图有 alpha 用 alpha，"
                     "否则以边框背景色估计掩码），**不是像素级 pass/fail**：官方图多为离线渲染，"
                     "常带 HDRI studio、软阴影、不同机位/背景；maque 暂无 HDRI IBL/transmission/软阴影。" % (
                         compare.get("reference"), "×".join(str(x) for x in compare.get("reference_size", [])),
                         "，含透明背景" if compare.get("reference_has_alpha") else ""))
        lines.append("")
        lines.append("| 指标 | 参考图 | 本预览 | 说明 |")
        lines.append("|---|---|---|---|")
        lines.append("| 前景平均亮度 | %.3f | %.3f | 差 %+.3f |" % (
            compare.get("ref_mean_lum", 0), compare.get("preview_mean_lum", 0), compare.get("mean_lum_delta", 0)))
        lines.append("| 前景平均 RGB | %s | %s | RGB 距离 %.3f |" % (
            compare.get("ref_mean_rgb"), compare.get("preview_mean_rgb"), compare.get("rgb_delta", 0)))
        lines.append("| 亮度直方图相似度 | — | — | %.2f（16 桶交集，1=完全一致） |" % compare.get("hist_similarity", 0))
        lines.append("| 前景占比 | %.1f%% | %.1f%% | 覆盖率比 %s |" % (
            compare.get("ref_coverage", 0) * 100, compare.get("preview_coverage", 0) * 100,
            compare.get("coverage_ratio")))
        lines.append("")
        lines.append("自动初判：**%s**" % compare.get("verdict", "?"))
        if compare.get("framing_notes"):
            lines.append("")
            lines.append("⚠️ 构图告警：%s" % "；".join(compare["framing_notes"]))
    else:
        lines.append("未生成对比数据（缺参考图或 PIL 不可用）。")
    lines.append("")
    lines.append("**差异归因**（按本模型实际特征）：")
    attrib = []
    if (meta or {}).get("screenshot"):
        attrib.append("官方机位/背景/焦距与本场景自动机位不同（本场景为统一 3/4 视角、中性灰背景）")
    attrib.extend(gaps)
    attrib.append("引擎无 HDRI/PMREM IBL、无软阴影/面光、无 MSAA（高光与间接光观感差异）")
    for a in attrib:
        lines.append("- %s" % a)
    lines.append("")
    lines.append("## 已知差距（引擎/导入器层面）")
    lines.append("")
    if gaps:
        for g in gaps:
            lines.append("- %s" % g)
    else:
        lines.append("- 无（core glTF + 已支持材质路径）")
    lines.append("")
    lines.append("## 踩过的坑 / 注意")
    lines.append("")
    if issues:
        for issue in issues:
            lines.append("- %s" % issue)
    else:
        lines.append("- 本模型导入/渲染无告警。")
    lines.append("")
    lines.append(MANUAL_MARKER)
    lines.append("")
    lines.append(manual_notes.split(MANUAL_MARKER, 1)[-1].strip() if manual_notes else "（可在此手写本模型的额外注意事项）")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


# ---------------------------------------------------------------- 主流程
def migrate_one(model, args, models_root, out_root, manifest):
    model_dir = os.path.join(models_root, model)
    out_dir = os.path.join(out_root, model)
    rec = {"model": model, "status": "pending", "gaps": [], "issues": []}
    gltf_path = find_primary_gltf(model_dir)
    if not gltf_path:
        rec["status"] = "blocked"
        rec["issues"].append("找不到 glTF 主变体（glTF/ 或 glTF-Binary/ 缺失）")
        return rec
    rec["source"] = os.path.relpath(gltf_path, REPO)
    meta = None
    meta_path = os.path.join(model_dir, "metadata.json")
    if os.path.isfile(meta_path):
        try:
            meta = load_json(meta_path)
        except Exception:  # noqa: BLE001
            meta = None
    rec["tags"] = (meta or {}).get("tags") or []
    rec["summary"] = (meta or {}).get("summary") or ""
    stats = gltf_stats(gltf_path)
    rec["stats"] = stats
    rec["gaps"] = known_gaps(stats)
    if "KHR_draco_mesh_compression" in (stats.get("extensions_required") or []):
        rec["status"] = "blocked"
        rec["issues"].append("主变体必需 Draco（无解码器）")
        return rec

    readme_path = os.path.join(out_dir, "README.md")
    manual_notes = read_manual_notes(readme_path)
    need_import = args.force or not os.path.isfile(os.path.join(out_dir, model + ".scn"))
    if need_import:
        os.makedirs(out_dir, exist_ok=True)
        if args.force:
            # 只清派生资产/场景/预览，**保留 README.md**（其中手动笔记区要跨重生成保留）
            for sub in ("assets", "textures"):
                p = os.path.join(out_dir, sub)
                if os.path.isdir(p):
                    shutil.rmtree(p)
            for fn in (model + ".scn", "preview.png", "preview.jpg", "compare.json"):
                p = os.path.join(out_dir, fn)
                if os.path.isfile(p):
                    os.remove(p)
        cmd = [sys.executable, IMPORTER, gltf_path, "--out", out_dir, "--name", model,
               "--scene", "--compact"]
        cam_scale = getattr(args, "camera_scale", None)
        if isinstance(cam_scale, dict):
            cam_scale = cam_scale.get(model)
        if cam_scale:
            cmd += ["--camera-distance-scale", "%.6f" % float(cam_scale)]
            rec["camera_distance_scale"] = round(float(cam_scale), 4)
        t0 = time.time()
        rc, log = run(cmd, timeout=1200)
        rec["import_seconds"] = round(time.time() - t0, 2)
        rec["import_log_tail"] = log.strip().splitlines()[-6:]
        if rc != 0:
            rec["status"] = "failed"
            rec["issues"].append("importer 失败（rc=%d）：%s" % (rc, log.strip().splitlines()[-1] if log.strip() else ""))
            return rec
        # importer 告警 → 记录到坑
        for line in log.splitlines():
            if any(k in line for k in ("跳过", "告警", "warn", "失败")):
                rec["issues"].append(line.strip())
    scene = os.path.join(out_dir, model + ".scn")
    if not os.path.isfile(scene):
        rec["status"] = "failed"
        rec["issues"].append("importer 未生成场景文件")
        return rec
    # 手动机位（Sponza 等）：每次生成后幂等覆盖；README/报告里记录
    if model in MANUAL_CAMERA:
        if apply_camera_override(scene, MANUAL_CAMERA[model]):
            rec["camera_override"] = MANUAL_CAMERA[model]["note"]
            rec["issues"].append("手动机位：%s" % MANUAL_CAMERA[model]["note"])
    # 产物统计
    assets_dir = os.path.join(out_dir, "assets")
    tex_dir = os.path.join(out_dir, "textures")
    mesh_files = [f for f in os.listdir(assets_dir) if f.endswith(".mesh.json")] if os.path.isdir(assets_dir) else []
    tex_files = os.listdir(tex_dir) if os.path.isdir(tex_dir) else []
    import_info = {"mesh_files": len(mesh_files), "mesh_bytes": dir_size(assets_dir) if assets_dir else 0,
                   "texture_files": len(tex_files), "scene": os.path.basename(scene)}
    rec["import"] = import_info
    rec["out_size_bytes"] = dir_size(out_dir)
    if stats.get("no_normal_prims"):
        rec["issues"].append("%d 个 primitive 无 NORMAL（导入器按索引重建法线）" % stats["no_normal_prims"])
    if stats.get("non_tri_prims"):
        rec["issues"].append("%d 个非三角 primitive 被跳过" % stats["non_tri_prims"])
    if stats.get("vertices", 0) > 500000:
        rec["issues"].append("大模型：%s 顶点（加载/渲染较慢）" % f"{stats['vertices']:,}")

    # 渲染 + 对比
    compare = None
    ref = find_reference_image(model_dir)
    rec["reference"] = os.path.relpath(ref, REPO) if ref else None
    full_png = os.path.join(out_dir, "preview.png")
    if not args.no_render:
        reuse = bool(args.reuse_preview) and os.path.isfile(full_png) and not args.force
        if reuse:
            rec["preview_png"] = os.path.relpath(full_png, REPO)
            rec["render_seconds"] = 0.0
        else:
            env = dict(os.environ)
            env.update({"MAQUE_PRESENT": "off", "MAQUE_WINDOW": "0x0"})
            t0 = time.time()
            p = subprocess.run([PLAYER, "--shot", full_png, "--shot-frame", "1", scene],
                               cwd=REPO, env=env, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=900)
            rec["render_seconds"] = round(time.time() - t0, 2)
            render_log = (p.stdout or "") + (p.stderr or "")
            rec["render_log_tail"] = render_log.strip().splitlines()[-6:]
            log_dir = os.path.join(REPO, "build-win", "audit", "migrate-logs")
            try:
                os.makedirs(log_dir, exist_ok=True)
                with open(os.path.join(log_dir, model + ".render.log"), "w", encoding="utf-8") as lf:
                    lf.write(render_log)
            except OSError:
                pass
            if p.returncode != 0 or not os.path.isfile(full_png):
                rec["status"] = "render_failed"
                rec["issues"].append("渲染失败（rc=%d）" % p.returncode)
                for line in render_log.splitlines():
                    if "error" in line.lower() or "[wgpu]" in line:
                        rec["issues"].append(line.strip())
            else:
                m = re.search(r"3d scene cams=(\d+) meshes=(\d+)", render_log)
                if m:
                    cams, meshes = int(m.group(1)), int(m.group(2))
                    if cams == 0:
                        rec["issues"].append("渲染日志无相机（cams=0）→ 场景相机无效")
                    if meshes == 0:
                        rec["issues"].append("渲染日志 meshes=0 → 没有网格被提取（资产导入/实体问题）")
                if "mesh=-1" in render_log:
                    rec["issues"].append("有 mesh 加载失败（mesh=-1；检查 .mesh.json 路径/JSON/VFS 根=场景目录）")
                if "[wgpu] error" in render_log:
                    rec["issues"].append("wgpu 校验错误（见 build-win/audit/migrate-logs/%s.render.log）" % model)
                rec["preview_png"] = os.path.relpath(full_png, REPO)
        if rec.get("status") != "render_failed" and os.path.isfile(full_png):
            # 提交用缩略图（JPEG，避免 149 张 PNG 膨胀仓库）
            try:
                from PIL import Image
                im = Image.open(full_png).convert("RGB")
                im.thumbnail((800, 450), Image.LANCZOS)
                im.save(os.path.join(out_dir, "preview.jpg"), quality=82, optimize=True)
                rec["preview_jpg"] = os.path.relpath(os.path.join(out_dir, "preview.jpg"), REPO)
            except Exception as e:  # noqa: BLE001
                rec["issues"].append("preview.jpg 生成失败：%s" % e)
            if ref:
                cmp_json = os.path.join(out_dir, "compare.json")
                compare = compare_images(full_png, ref, cmp_json)
                rec["compare"] = compare
                if compare and "error" not in compare:
                    rec["issues"].append("参考图与本预览差异：%s" % compare.get("verdict"))
                    if compare.get("preview_coverage", 1) < 0.02:
                        rec["issues"].append("预览前景占比 %.2f%% < 2%%（机位过远/模型过小？）"
                                             % (compare["preview_coverage"] * 100))
                    for fn in (compare.get("framing_notes") or []):
                        rec["issues"].append("构图告警：%s" % fn)

    # 写笔记（render 失败也写，标注状态）
    if not args.no_notes:
        write_readme(readme_path, model, meta, stats, rec["gaps"], import_info, compare,
                     [i for i in rec["issues"] if not i.startswith("参考图与本预览差异")], manual_notes)
    rec["readme"] = os.path.relpath(readme_path, REPO)
    if rec["status"] == "pending":
        rec["status"] = "ok" if "preview_jpg" in rec else "imported"
    return rec


def write_failure_note(path, model, status, issues, stats=None, meta=None, manual_notes=""):
    """blocked / failed 模型的占位笔记（保证每个 demo 都有一份独立笔记）。"""
    legal = (meta or {}).get("legal") or []
    lines = ["# %s —— Khronos glTF 示例迁移（未完成）" % model, "",
             "**来源**：[KhronosGroup/glTF-Sample-Assets · Models/%s](%s/%s)  " % (model, REFERENCES_URL, model),
             "**状态**：`%s`" % status, ""]
    if legal:
        lines.append("**许可**：（资产元数据，保留出处）")
        for item in legal:
            lines.append("- %s（%s）%s" % (item.get("text") or "?", item.get("spdx") or "?",
                                          item.get("licenseUrl") or ""))
        lines.append("")
    if stats:
        lines.append("**模型统计**：节点 %d / 网格 %d / primitive %d / 顶点 %s / 三角形 %s / 材质 %d / 纹理 %d；扩展：%s" % (
            stats.get("nodes", 0), stats.get("meshes", 0), stats.get("primitives", 0),
            f"{stats.get('vertices', 0):,}", f"{stats.get('triangles', 0):,}",
            stats.get("materials", 0), stats.get("textures", 0),
            "、".join("`%s`" % e for e in (stats.get("extensions_used") or [])) or "无"))
        lines.append("")
    lines.append("## 原因 / 踩坑")
    lines.append("")
    for issue in issues or ["未知"]:
        lines.append("- %s" % issue)
    lines.append("")
    lines.append(MANUAL_MARKER)
    lines.append("")
    lines.append(manual_notes.split(MANUAL_MARKER, 1)[-1].strip() if manual_notes else
                 "（该模型尚未完成迁移；可在此记录阻塞原因与后续处理）")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def write_index(manifest, out_root):
    """生成 scenes/khronos/README.md 总索引。"""
    recs = manifest["records"]
    # 手工调校、不参与自动机位的既有场景（如 ToyCar）：目录存在但没有自动记录 → 补一行
    known = {r.get("model") for r in recs}
    if os.path.isdir(out_root):
        for name in sorted(os.listdir(out_root)):
            d = os.path.join(out_root, name)
            if name in known or not os.path.isdir(d):
                continue
            if os.path.isfile(os.path.join(d, name + ".scn")) and \
               os.path.isfile(os.path.join(d, "README.md")):
                recs.append({"model": name, "status": "ok (hand-tuned)",
                             "tags": ["hand-tuned"], "gaps": [],
                             "issues": ["手写调校场景（机位/灯光/环境按参考图手调，不参与自动机位批量生成）"],
                             "readme": os.path.relpath(os.path.join(d, "README.md"), REPO),
                             "preview_jpg": os.path.relpath(
                                 os.path.join(d, "preview.jpg" if os.path.isfile(os.path.join(d, "preview.jpg"))
                                              else "preview.png"), REPO)
                             if (os.path.isfile(os.path.join(d, "preview.jpg"))
                                 or os.path.isfile(os.path.join(d, "preview.png"))) else None})
    recs = sorted(recs, key=lambda r: r.get("model", ""))
    ok = [r for r in recs if r.get("status") in ("ok", "imported")]
    blocked = [r for r in recs if r.get("status") == "blocked"]
    failed = [r for r in recs if r.get("status") not in ("ok", "imported", "blocked")]
    total_assets = sum((r.get("out_size_bytes") or 0) for r in recs)
    lines = []
    lines.append("# Khronos glTF-Sample-Assets · 模型迁移索引")
    lines.append("")
    lines.append("本目录把 [KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) "
                 "`Models/` 下的模型逐个迁移为 maque 场景。每个模型一个子目录：")
    lines.append("")
    lines.append("- `<Model>/<Model>.scn` —— maque 场景（自动机位 + 方向光 + 引擎 analytic studio 环境）；")
    lines.append("- `<Model>/preview.jpg` —— 本引擎渲染的 800×450 预览（提交）；`preview.png` 为 1280×720 原图（本地，不入库）；")
    lines.append("- `<Model>/README.md` —— **独立笔记**：来源/许可/模型统计/生成与运行命令/与官方 screenshot 的定量对比与差异归因/已知差距/踩坑；")
    lines.append("- `<Model>/assets/`、`textures/` —— 导入产物（`.mesh.json`/贴图，体积大，**不入库**，"
                 "由 `python tools/migrate_gltf_models.py --only <Model> --force` 从 `.deps/gltf-assets` 重新生成）。")
    lines.append("")
    lines.append("迁移流程、踩坑与经验：见 [docs/31-gltf-model-migration.md](../../docs/31-gltf-model-migration.md)。"
                 "机器可读清单：[manifest.json](manifest.json)；"
                 "**逐模型官方参考图（large 优先）vs 迁移预览对比报告：[reports/khronos-comparison/index.html](../../reports/khronos-comparison/index.html)**。")
    lines.append("")
    lines.append("## 总览")
    lines.append("")
    lines.append("| 项 | 值 |")
    lines.append("|---|---|")
    lines.append("| 模型数 | %d |" % len(recs))
    lines.append("| 迁移成功（含仅导入） | %d |" % len(ok))
    lines.append("| blocked（主变体必需 Draco 等） | %d |" % len(blocked))
    lines.append("| 失败/异常 | %d |" % len(failed))
    lines.append("| 派生资产总大小（本地，不入库） | %s |" % human_size(total_assets))
    lines.append("")
    lines.append("> ⚠️ 与官方 `screenshot/` 对比**不是像素级 pass/fail**：官方图多为离线渲染（常带 HDRI studio、"
                 "软阴影、不同机位/背景），maque 暂无 HDRI IBL/transmission/软阴影/动画骨骼；"
                 "每个模型的 README 给出定量指标与差异归因。")
    lines.append("")
    lines.append("## 模型列表")
    lines.append("")
    lines.append("| 模型 | 标签 | 顶点/三角形 | 材质/纹理 | 已知差距 | 状态 | 预览 | 笔记 |")
    lines.append("|---|---|---|---|---|---|---|---|")
    for r in recs:
        name = r.get("model", "?")
        st = r.get("stats") or {}
        tags = "、".join(r.get("tags") or [])
        verts = f"{st.get('vertices', 0):,}"
        tris = f"{st.get('triangles', 0):,}"
        gaps = len(r.get("gaps") or [])
        status = r.get("status", "?")
        preview = "[图](%s/preview.jpg)" % name if r.get("preview_jpg") else "—"
        lines.append("| [%s](%s/README.md) | %s | %s / %s | %d / %d | %d | `%s` | %s | [笔记](%s/README.md) |" % (
            name, name, tags, verts, tris, st.get("materials", 0), st.get("textures", 0), gaps,
            status, preview, name))
    lines.append("")
    if blocked or failed:
        lines.append("## blocked / 失败明细")
        lines.append("")
        for r in blocked + failed:
            lines.append("- `%s`：%s" % (r.get("model"), "；".join(r.get("issues") or [])[:300]))
        lines.append("")
    lines.append("## 复现")
    lines.append("")
    lines.append("```bash")
    lines.append("# 1) 下载/解包 Khronos 资产（本机 Node TLS 可用；shell 的 schannel 出网在本环境不稳定）")
    lines.append("node .deps/download-gltf-assets.js .deps/gltf-sample-assets.zip")
    lines.append("tar -xf .deps/gltf-sample-assets.zip -C .deps/gltf-assets")
    lines.append("# 2) 批量迁移 + 预览 + 笔记 + 索引")
    lines.append("python tools/migrate_gltf_models.py")
    lines.append("# 单个模型 / 重新导入")
    lines.append("python tools/migrate_gltf_models.py --only Box --force")
    lines.append("```")
    lines.append("")
    path = os.path.join(out_root, "README.md")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return path


def run_auto_framing(args, manifest, models_root, out_root):
    """按参考图前景覆盖率自动调机位距离（覆盖率比 = 预览/参考；cov ∝ 1/d² → d *= sqrt(ratio)）。

    只处理 coverage_ratio 偏离 [0.6, 1.8] 的模型（跳过手动机位/ToyCar）；每模型重新导入+渲染+笔记。
    """
    recs = [r for r in manifest.get("records", []) if r.get("model")]
    by_name = {r["model"]: r for r in recs}
    flagged = []
    for r in recs:
        model = r["model"]
        if model in MANUAL_CAMERA or model == "ToyCar":
            continue
        c = r.get("compare") or {}
        ratio = c.get("coverage_ratio")
        cov = c.get("preview_coverage")
        if ratio is None or cov is None or cov < 1e-4:
            continue
        if 0.6 <= ratio <= 1.8:
            continue
        scale = _clamp(math.sqrt(float(ratio)), 0.35, 2.0)
        flagged.append((model, scale))
    print("auto-framing: %d models outside coverage_ratio [0.6, 1.8]" % len(flagged))
    if args.dry_run:
        for model, scale in flagged:
            print("  %-28s scale=%.3f" % (model, scale))
        return
    base_force = args.force
    for i, (model, scale) in enumerate(flagged, 1):
        args.force = True
        args.camera_scale = {model: scale}
        try:
            rec = migrate_one(model, args, models_root, out_root, manifest)
        except Exception as e:  # noqa: BLE001
            rec = {"model": model, "status": "exception", "issues": [repr(e)]}
        by_name[model] = rec
        print("[%2d/%2d] %-28s scale=%.3f -> %s" % (i, len(flagged), model, scale, rec.get("status")))
    args.force = base_force
    args.camera_scale = None
    manifest["records"] = [by_name[k] for k in sorted(by_name)]
    manifest["auto_framing"] = {"adjusted": len(flagged)}


def main():
    ap = argparse.ArgumentParser(description="批量迁移 Khronos glTF-Sample-Assets Models → maque scenes")
    ap.add_argument("--models-root", default=DEFAULT_MODELS_ROOT)
    ap.add_argument("--out-root", default=DEFAULT_OUT_ROOT)
    ap.add_argument("--only", default="", help="逗号分隔的模型名/子串过滤")
    ap.add_argument("--exact", action="store_true", help="--only 精确匹配模型名（默认子串）")
    ap.add_argument("--skip-existing", action="store_true", help="已生成 README 的模型跳过")
    ap.add_argument("--force", action="store_true", help="重新导入（覆盖 assets/scn）")
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--reuse-preview", action="store_true",
                    help="不重渲染，复用已有 preview.png 重算对比/笔记（快）")
    ap.add_argument("--no-notes", action="store_true")
    ap.add_argument("--exclude", default="ToyCar",
                    help="逗号分隔的排除模型（默认 ToyCar：已手工调校机位/灯光，不参与自动批量）")
    ap.add_argument("--manifest", default=os.path.join(DEFAULT_OUT_ROOT, "manifest.json"))
    ap.add_argument("--auto-framing", action="store_true",
                    help="按参考图前景覆盖率自动调相机距离（需已有 manifest/compare；逐模型重导+渲染）")
    ap.add_argument("--dry-run", action="store_true", help="只打印 auto-framing 计划，不执行")
    args = ap.parse_args()
    args.camera_scale = None

    if not os.path.isdir(args.models_root):
        sys.exit("migrate: 找不到 models 目录：%s\n（先运行 node .deps/download-gltf-assets.js 并解包到 .deps/gltf-assets）"
                 % args.models_root)
    names = sorted(d for d in os.listdir(args.models_root)
                   if os.path.isdir(os.path.join(args.models_root, d)))
    excludes = {x.strip() for x in (args.exclude or "").split(",") if x.strip()}
    names = [n for n in names if n not in excludes]
    if args.only:
        pats = [p.strip() for p in args.only.split(",") if p.strip()]
        if args.exact:
            names = [n for n in names if n in pats]
        else:
            names = [n for n in names if any(p.lower() in n.lower() for p in pats)]
    if args.skip_existing:
        names = [n for n in names
                 if not os.path.isfile(os.path.join(args.out_root, n, "README.md"))]
    os.makedirs(args.out_root, exist_ok=True)

    if args.auto_framing:
        if not os.path.isfile(args.manifest):
            sys.exit("migrate: --auto-framing 需要已有 manifest（先跑一次批量迁移）")
        manifest0 = load_json(args.manifest)
        run_auto_framing(args, manifest0, args.models_root, args.out_root)
        # 写回 manifest + 索引（复用自动取景后的 records）
        import hashlib as _hl

        def _sha(path):
            try:
                h = _hl.sha256()
                with open(path, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                return h.hexdigest()
            except OSError:
                return None

        rc, git_head = run(["git", "rev-parse", "HEAD"], timeout=30)
        manifest0.setdefault("provenance", {}).update({
            "git_head": git_head.strip() if rc == 0 else None,
            "importer_sha256": _sha(IMPORTER),
            "migrate_script_sha256": _sha(os.path.abspath(__file__)),
            "player_sha256": _sha(PLAYER),
        })
        with open(args.manifest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(manifest0, f, ensure_ascii=False, indent=1, sort_keys=True)
            f.write("\n")
        idx = write_index(manifest0, args.out_root)
        print("migrate(auto-framing): %d records → %s / %s"
              % (len(manifest0["records"]), os.path.relpath(args.manifest, REPO),
                 os.path.relpath(idx, REPO)))
        return

    manifest = {"generated_by": "tools/migrate_gltf_models.py",
                "models_root": os.path.relpath(args.models_root, REPO),
                "count": len(names), "records": []}
    if os.path.isfile(args.manifest) and args.only:
        try:
            old = load_json(args.manifest)
            manifest["records"] = old.get("records") or []
        except Exception:  # noqa: BLE001
            manifest["records"] = []
    by_name = {r.get("model"): r for r in manifest["records"]}

    t_all = time.time()
    for i, name in enumerate(names, 1):
        t0 = time.time()
        try:
            rec = migrate_one(name, args, args.models_root, args.out_root, manifest)
        except Exception as e:  # noqa: BLE001
            import traceback
            rec = {"model": name, "status": "exception", "issues": [repr(e)],
                   "traceback": traceback.format_exc()}
        by_name[name] = rec
        # 失败/blocked 也要有一份独立笔记（保留手工笔记）
        if rec.get("status") in ("blocked", "failed", "exception") and not args.no_notes:
            try:
                d = os.path.join(args.out_root, name)
                os.makedirs(d, exist_ok=True)
                note = os.path.join(d, "README.md")
                write_failure_note(note, name, rec.get("status"), rec.get("issues") or [],
                                   rec.get("stats"), None, read_manual_notes(note))
                rec["readme"] = os.path.relpath(note, REPO)
            except Exception as e:  # noqa: BLE001
                rec["issues"].append("占位笔记写入失败：%s" % e)
        status = rec.get("status")
        extra = ""
        if rec.get("stats"):
            extra = " verts=%s tris=%s" % (f"{rec['stats'].get('vertices', 0):,}",
                                           f"{rec['stats'].get('triangles', 0):,}")
        print("[%3d/%3d] %-28s %-12s %5.1fs%s" % (i, len(names), name, status, time.time() - t0, extra))
        if rec.get("issues"):
            for issue in rec["issues"][:3]:
                print("          - %s" % issue)
    manifest["records"] = [by_name[k] for k in sorted(by_name)]
    manifest["count"] = len(manifest["records"])
    manifest["total_seconds"] = round(time.time() - t_all, 1)
    # provenance（不含时间戳 → 同一代码/资产状态下 manifest 逐字节稳定）
    import hashlib

    def _sha256(path):
        try:
            h = hashlib.sha256()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            return h.hexdigest()
        except OSError:
            return None

    rc, git_head = run(["git", "rev-parse", "HEAD"], timeout=30)
    manifest["provenance"] = {
        "git_head": git_head.strip() if rc == 0 else None,
        "importer_sha256": _sha256(IMPORTER),
        "migrate_script_sha256": _sha256(os.path.abspath(__file__)),
        "player_sha256": _sha256(PLAYER),
    }
    os.makedirs(os.path.dirname(args.manifest), exist_ok=True)
    with open(args.manifest, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1, sort_keys=True)
        f.write("\n")
    ok = sum(1 for r in manifest["records"] if r.get("status") in ("ok", "imported"))
    blocked = sum(1 for r in manifest["records"] if r.get("status") == "blocked")
    failed = len(manifest["records"]) - ok - blocked
    index = write_index(manifest, args.out_root)
    print("\nmigrate: %d ok, %d blocked, %d failed (%.1fs)" % (ok, blocked, failed, manifest["total_seconds"]))
    print("  manifest: %s" % os.path.relpath(args.manifest, REPO))
    print("  index   : %s" % os.path.relpath(index, REPO))


if __name__ == "__main__":
    main()
