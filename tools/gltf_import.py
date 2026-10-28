#!/usr/bin/env python3
"""glTF → maque 离线资产导入（plans/18-pbr-3d.md 方向 2）。

输入：`.gltf`（JSON，external/内嵌 base64 buffer）或 `.glb`（二进制 chunk）。
输出（--out 目录，确定性文本，可纳入版本库/CI 校验）：
  <name>.mesh.json     网格 + 材质 + 贴图引用（运行时加载格式；默认 48B/layout0，必要时 56B/layout1）
  textures/<name>.png   PNG 贴图（bin bufferView 内嵌/外部 uri/data: URI 一律搬运；JPEG 输出 .jpg）

设计约束（与引擎一致）：
  · 顶点布局 = mq_renderer_mesh_gpu：
      layout 0（旧）：pos3|normal3|tangent4|uv2                    = 12 float / 48B
      layout 1（新）：pos3|normal3|color4|uv0_2|uv1_2              = 14 float / 56B
    仅当 primitive 有 COLOR_0 或其材质任一贴图 texCoord>0 时用 layout 1；
    否则 layout 0 输出与迁移前逐字节一致（旧资产零漂移）。
  · 只接受三角网格（mode=4/缺省）；其它 mode 跳过并告警（确定性：按 primitive 顺序）
  · 输出 JSON 排序键 + 固定缩进 → 同一 glTF 导入结果逐字节一致
  · 不做图像编解码：PNG/JPEG 原样搬运（运行时 image 模块按 magic 解码）；
    其它编码（KTX2/BasisU/WebP）跳过并告警
  · 材质扩展：KHR_materials_{unlit,clearcoat,sheen,iridescence,specular,ior,anisotropy,
    emissive_strength} → asset.PbrMaterial（只写非默认值）；transmission → alpha BLEND 近似 +
    transmission_factor/transmission_texture；volume.thicknessFactor → volume_thickness（>0 才写）；
    dispersion → dispersion（>0 才写）。后三者是 shader 折射/色散近似的参数。
  · KHR 扩展材质贴图槽（plans/20 第 2 轮 + specular）：clearcoat / clearcoat_roughness / sheen_color /
    sheen_roughness / iridescence / iridescence_thickness / anisotropy 7 槽 + specular /
    specular_color 2 槽 + transmission 1 槽 → 字符串字段。UV 近似复用 uv_scale/uv_offset
    （TEXCOORD_0；不支持这些槽的逐槽 texCoord/transform）。
  · glTF sampler 非默认时贴图路径追加 `#wrap=U,V&filter=nearest`（runtime 据此建 sampler）。
  · KHR_lights_punctual：`--scene` 时导入为 render.Light 实体（≤8 盏、方向光优先；node 世界变换 →
    position/rotation，传播方向 = 节点 -Z；spot 用内/外锥角）。强度为**观感归一化**（directional≈2.6×
    intensity，point/spot 按模型尺度约几十~几百），有灯光时不再追加默认太阳。
  · KHR_node_visibility.visible=false：跳过该节点及整棵子树（mesh 与灯光都不生成）。
  · glTF 自带相机：`--use-gltf-camera` 取节点遍历第一个 perspective 相机（yfov/znear/zfar + 节点世界 TRS；
    引擎仅透视，orthographic 打印警告并回退自动机位）；默认仍自动取景。
  · KHR_texture_transform.rotation（glTF T*R*S 约定，弧度）→ 材质 `uv_rotation`（baseColor 组）/
    `normal_uv_rotation`（normal 组）；仅非 0 写（旧输出零漂移）。
  · EXT_mesh_gpu_instancing：节点的 TRANSLATION/ROTATION(xyzw)/SCALE 实例 accessor → 每个 mesh
    primitive 展开 N 个实体（实例世界矩阵 = node 世界矩阵 × 实例 TRS）；>4096 警告并截断。
  · KHR_materials_diffuse_transmission → alpha BLEND 近似（opacity=clamp(1-0.7*factor,0.25,1)）；
    KHR_materials_volume 的 attenuationColor+finite attenuationDistance → base_color.rgb 乘色调（薄壁近似），
    thicknessFactor（>0）→ volume_thickness；KHR_materials_dispersion 的 dispersion（>0）→ dispersion。
  · 蒙皮 / morph（plans/22 §2.1，S1+S3）：JOINTS_0/WEIGHTS_0 → "joints"/"weights"（u16 语义；权重归一化
    sum≈1）；有蒙皮的 primitive 用 vertex_layout 2（48+24B→stride 72）或 3（56+24B→stride 80），
    vertices 行宽不变。primitive.targets → "morph":{default_weights,targets[{positions,normals}]}
    （POSITION 必需、NORMAL 有则存、TANGENT 忽略；上限 8 个 target）。
  · 骨架 / 动画（plans/22 §2.2/§2.3，S1+S2）：每个引用 skin 的节点一份 .skel.json（--scene 写
    assets/skin/，否则 skin/；joint_names = 全场景唯一节点名，inverse_bind 列主序）；每个 animation
    一份 .clip.json（--scene 写 assets/anim/，否则 anim/；track = node/path/interpolation/times/values，
    按 (node, translation<rotation<scale<weights, 原 channel 序) 排序；pointer 通道跳过并汇总，
    CUBICSPLINE 取关键帧 value 按 LINEAR 近似并计数）。
  · 层级场景（plans/22 §3）：模型含 skins / animations / 任一 primitive.targets 时 --scene 输出 glTF
    节点树 1:1 的嵌套 children（节点实体 = 本地 TRS；primitive 子实体 identity；蒙皮 primitive 实体放
    场景根 + MeshRenderer.skin_path；有片段时追加 anim_player 实体）。无这三类特性的模型仍输出旧的
    平级 + 世界烘焙结果（逐字节不变）。
用法：
  python tools/gltf_import.py scene.gltf --out assets/imported
  python tools/gltf_import.py model.glb --out assets/imported --name hero
  python tools/gltf_import.py model.gltf --out out --scene --camera-distance-scale 1.2
  python tools/gltf_import.py model.gltf --out out --scene \
      --camera-position "3,2,1" --camera-target "0,0.5,0" --camera-fov 30
"""
from __future__ import annotations

import argparse
import base64
import json
import math
import os
import struct
import sys
import urllib.parse
import uuid

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
             5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
MODE_TRIANGLES = 4
# 会映射为 MaterialShadingModel::Physical 的 KHR 扩展（unlit 单独映射为 Basic）。
PHYSICAL_EXTS = ("KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_iridescence",
                 "KHR_materials_specular", "KHR_materials_ior", "KHR_materials_anisotropy")


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
            with open(os.path.join(base_dir, urllib.parse.unquote(uri)), "rb") as f:
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


def accessor_norm_scale(js, idx):
    """整数 accessor 的归一化系数（normalized 属性；颜色/UV 量化用）。

    glTF 规范：COLOR_0/TEXCOORD_n 允许 float 或 normalized ubyte/ushort
    （KHR_mesh_quantization 还允许 signed byte/short）。非 normalized 整数保持原值。
    """
    acc = js["accessors"][idx]
    ct = acc["componentType"]
    if ct == 5126 or not acc.get("normalized"):
        return 1.0
    return {5120: 1.0 / 127.0, 5121: 1.0 / 255.0,
            5122: 1.0 / 32767.0, 5123: 1.0 / 65535.0}.get(ct, 1.0)


def effective_texcoord(tinfo):
    """textureInfo 实际使用的 UV 集：KHR_texture_transform.texCoord 覆盖 textureInfo.texCoord。"""
    if not tinfo:
        return 0
    tr = (tinfo.get("extensions") or {}).get("KHR_texture_transform")
    if isinstance(tr, dict) and "texCoord" in tr:
        try:
            return int(tr["texCoord"])
        except (TypeError, ValueError):
            return 0
    return int(tinfo.get("texCoord", 0))


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


def sniff_image(data):
    """按 magic 判断贴图编码（不信任扩展名/mimeType）。返回 'png'/'jpeg'/'gif'/'webp'/'ktx2'/None。"""
    if len(data) >= 8 and data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if len(data) >= 2 and data[:2] == b"\xff\xd8":
        return "jpeg"
    if len(data) >= 6 and data[:6] in (b"GIF87a", b"GIF89a"):
        return "gif"
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if len(data) >= 12 and data[:12] == b"\xabKTX 20\xbb\r\n\x1a\n":
        return "ktx2"
    return None


def transcode_to_png(data, name_hint):
    """WebP/GIF 等引擎无解码器的贴图 → 用 PIL 转 PNG（import 期离线做，确定性）。
    返回 PNG 字节；PIL 不可用/解码失败返回 None（调用方告警跳过）。"""
    try:
        from PIL import Image
        import io
    except Exception:  # noqa: BLE001
        return None
    try:
        im = Image.open(io.BytesIO(data))
        if getattr(im, "is_animated", False):
            im.seek(0)                      # 动画贴图取第一帧
        im = im.convert("RGBA")
        out = io.BytesIO()
        im.save(out, format="PNG")
        return out.getvalue()
    except Exception:  # noqa: BLE001
        return None


def material_from_gltf(js, mat, images_out, tex_dir, base_dir, buffers, name_registry=None):
    pbr = mat.get("pbrMetallicRoughness", {})
    c = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
    e = mat.get("emissiveFactor", [0.0, 0.0, 0.0])
    double_sided = bool(mat.get("doubleSided", False))
    m = {
        "type": "asset.PbrMaterial",
        "name": mat.get("name", ""),
        "base_color": {"r": c[0], "g": c[1], "b": c[2], "a": c[3]},
        "metallic": float(pbr.get("metallicFactor", 1.0)),
        "roughness": float(pbr.get("roughnessFactor", 1.0)),
        "emissive": {"x": e[0], "y": e[1], "z": e[2]},
        "double_sided": double_sided,
        # 运行时剔除只看 MaterialSide（gfx side→cullMode）：doubleSided=false 必须 Front(0)
        # 才会剔除背面；true → Double(2)（保留 double_sided 供 shader 侧将来使用）
        "side": 2 if double_sided else 0,
    }
    alpha_mode = mat.get("alphaMode", "OPAQUE")
    m["alpha_mode"] = {"OPAQUE": 0, "MASK": 1, "BLEND": 2}.get(alpha_mode, 0)
    orig_alpha_mode = m["alpha_mode"]
    m["alpha_cutoff"] = float(mat.get("alphaCutoff", 0.5))
    # KHR 材质扩展 → asset.PbrMaterial 字段（plans/19 材质迁移）。缺省值 = schema 默认；
    # 扩展字段只写"非默认"值（无扩展材质不因扩展逻辑产生漂移）。例外：side/double_sided 对
    # **每个**材质都写 —— 修 M1（运行时剔除只认 side，不看 double_sided）→ 已入库资产一次性重生成。
    exts = mat.get("extensions") or {}

    def put_num(field, value, default):
        if float(value) != float(default):
            m[field] = float(value)

    def put_vec3(field, value, default):
        v = [float(x) for x in value]
        if v != [float(x) for x in default]:
            m[field] = {"x": v[0], "y": v[1], "z": v[2]}

    if "KHR_materials_unlit" in exts:
        m["shading_model"] = 2  # MaterialShadingModel::Basic
    elif any(k in exts for k in PHYSICAL_EXTS):
        m["shading_model"] = 1  # MaterialShadingModel::Physical
    if "KHR_materials_clearcoat" in exts:
        cc = exts["KHR_materials_clearcoat"] or {}
        put_num("clearcoat", cc.get("clearcoatFactor", 0.0), 0.0)
        put_num("clearcoat_roughness", cc.get("clearcoatRoughnessFactor", 0.0), 0.03)
    if "KHR_materials_sheen" in exts:
        sh = exts["KHR_materials_sheen"] or {}
        m["sheen"] = 1.0  # glTF 无 sheen 开关：扩展存在即视为开启（近似）
        put_vec3("sheen_color", sh.get("sheenColorFactor", [0.0, 0.0, 0.0]), [0.0, 0.0, 0.0])
        put_num("sheen_roughness", sh.get("sheenRoughnessFactor", 0.0), 0.5)
    if "KHR_materials_iridescence" in exts:
        ir = exts["KHR_materials_iridescence"] or {}
        put_num("iridescence", ir.get("iridescenceFactor", 0.0), 0.0)
        put_num("iridescence_ior", ir.get("iridescenceIor", 1.3), 1.3)
        put_num("iridescence_thickness_min", ir.get("iridescenceThicknessMinimum", 100.0), 100.0)
        put_num("iridescence_thickness_max", ir.get("iridescenceThicknessMaximum", 400.0), 400.0)
    if "KHR_materials_specular" in exts:
        sp = exts["KHR_materials_specular"] or {}
        put_num("specular_intensity", sp.get("specularFactor", 1.0), 1.0)
        put_vec3("specular", sp.get("specularColorFactor", [1.0, 1.0, 1.0]), [1.0, 1.0, 1.0])
    if "KHR_materials_ior" in exts:
        put_num("ior", (exts["KHR_materials_ior"] or {}).get("ior", 1.5), 1.5)
    if "KHR_materials_anisotropy" in exts:
        an = exts["KHR_materials_anisotropy"] or {}
        put_num("anisotropy", an.get("anisotropyStrength", 0.0), 0.0)
        put_num("anisotropy_rotation", an.get("anisotropyRotation", 0.0), 0.0)
    if "KHR_materials_emissive_strength" in exts:
        put_num("emissive_intensity",
                (exts["KHR_materials_emissive_strength"] or {}).get("emissiveStrength", 1.0), 1.0)
    # KHR_materials_transmission 近似（无 backbuffer 折射）：factor>0 时改 alpha BLEND 半透。
    # opacity = clamp(1 - 0.85*factor, 0.15, 1)（ToyCar Glass 手调 opacity≈0.3 的同款思路）。
    tr = exts.get("KHR_materials_transmission") or {}
    try:
        tf = float(tr.get("transmissionFactor", 0.0) or 0.0)
    except (TypeError, ValueError):
        tf = 0.0
    if tf > 0.01:
        m["alpha_mode"] = 2                       # PbrAlphaMode::Blend
        m["transmission_factor"] = tf
        approx = max(0.15, min(1.0, 1.0 - 0.85 * tf))
        m["opacity"] = min(float(m.get("opacity", 1.0)), approx)
    # KHR_materials_diffuse_transmission：**不透明的薄壁透光**（陶瓷/叶片/果皮本身不可见"透明"，
    # 官方参考图是实心材质 + 背光透射）。旧版按 alpha BLEND 近似会把 teacup 变成幽灵玻璃——改为
    # 写 diffuse_transmission_factor 供 shader 做背光透射项；alphaMode/opacity 保持原样。
    dt = exts.get("KHR_materials_diffuse_transmission") or {}
    try:
        dtf = float(dt.get("diffuseTransmissionFactor", 0.0) or 0.0)
    except (TypeError, ValueError):
        dtf = 0.0
    if dtf > 0.01:
        m["diffuse_transmission_factor"] = max(0.0, min(1.0, dtf))
        dcol = dt.get("diffuseTransmissionColorFactor")
        if isinstance(dcol, list) and len(dcol) == 3:
            try:
                cr, cg, cb = (max(0.0, min(1.0, float(x))) for x in dcol)
                m["diffuse_transmission_color"] = {"x": cr, "y": cg, "z": cb}
            except (TypeError, ValueError):
                pass
    # KHR_materials_volume：（第 24 轮）厚度/吸收/散射完整接入，替代旧的"base_color × attenuationColor"
    # 薄壁色调近似：
    #   thicknessFactor → volume_thickness（折射厚度）；thicknessTexture.G → volume_thickness_texture；
    #   attenuationColor/attenuationDistance → attenuation_color / attenuation_distance（0=无限=无吸收），
    #   shader 用 Beer-Lambert：T = exp(-(1-attenuationColor) * thickness / attenuationDistance)。
    vol = exts.get("KHR_materials_volume") or {}
    try:
        vol_thickness = float(vol.get("thicknessFactor", 0.0) or 0.0)
    except (TypeError, ValueError):
        vol_thickness = 0.0
    if vol_thickness > 0.0:
        m["volume_thickness"] = vol_thickness
    ac = vol.get("attenuationColor")
    if isinstance(ac, list) and len(ac) == 3:
        try:
            m["attenuation_color"] = {"x": float(ac[0]), "y": float(ac[1]), "z": float(ac[2])}
        except (TypeError, ValueError):
            pass
    ad = vol.get("attenuationDistance")
    try:
        adf = float(ad) if ad is not None else None
    except (TypeError, ValueError):
        adf = None
    if adf is not None and math.isfinite(adf) and adf > 0.0:
        m["attenuation_distance"] = adf
    # KHR_materials_volume_scatter（ScatteringSkull）：multiscatterColorFactor → 多次散射色（0=无散射）。
    vs = exts.get("KHR_materials_volume_scatter") or {}
    ms = vs.get("multiscatterColorFactor")
    if isinstance(ms, list) and len(ms) == 3:
        try:
            m["multiscatter_color"] = {"x": float(ms[0]), "y": float(ms[1]), "z": float(ms[2])}
        except (TypeError, ValueError):
            pass
    # KHR_materials_dispersion：dispersion > 0 写材质 dispersion（shader 色散近似；0=关闭，不写）。
    dsp = exts.get("KHR_materials_dispersion") or {}
    try:
        disp = float(dsp.get("dispersion", 0.0) or 0.0)
    except (TypeError, ValueError):
        disp = 0.0
    if disp > 0.0:
        m["dispersion"] = disp

    # glTF sampler → 贴图路径后缀（仅非默认；runtime 据此建 per-texture sampler）。
    # 默认 (repeat, repeat, linear) 不写 → 旧资产路径逐字节不变。
    # transmission/diffuse 近似覆盖了 MASK → BLEND 时保留 alpha 镂空（shader 按 alpha_cutoff discard）
    if orig_alpha_mode == 1 and m.get("alpha_mode") == 2:
        m["alpha_mask_in_blend"] = True
    def tex_suffix(tinfo):
        if not tinfo:
            return ""
        tex = js["textures"][tinfo["index"]]
        si = tex.get("sampler")
        samplers = js.get("samplers") or []
        if si is None or not isinstance(si, int) or si < 0 or si >= len(samplers):
            return ""
        s = samplers[si] or {}
        wrap = {33071: "clamp", 33648: "mirror", 10497: "repeat"}
        u = wrap.get(s.get("wrapS"), "repeat")
        v = wrap.get(s.get("wrapT"), "repeat")
        parts = []
        if u != "repeat" or v != "repeat":
            parts.append(f"wrap={u},{v}")
        if s.get("magFilter") == 9728:            # NEAREST；未给/9729=linear 默认
            parts.append("filter=nearest")
        return "#" + "&".join(parts) if parts else ""

    # 贴图槽：glTF 的 texture index → image uri 或 bufferView（PNG/JPEG 原样搬运）
    def tex_slot(tinfo):
        if not tinfo:
            return ""
        tex = js["textures"][tinfo["index"]]
        src = tex.get("source")
        if src is None:
            # EXT_texture_webp / KHR_texture_basisu / EXT_texture_avif：source 放在 texture.extensions 里
            tex_exts = tex.get("extensions") or {}
            for key in ("EXT_texture_webp", "KHR_texture_basisu", "EXT_texture_avif"):
                ext = tex_exts.get(key) or {}
                if "source" in ext:
                    src = ext["source"]
                    break
        if src is None:
            return ""
        img = js["images"][src]
        if img.get("uri") and not img["uri"].startswith("data:"):
            with open(os.path.join(base_dir, urllib.parse.unquote(img["uri"])), "rb") as f:
                data = f.read()
        elif img.get("uri"):
            data = base64.b64decode(img["uri"].split(",", 1)[1])
        else:
            view = js["bufferViews"][img["bufferView"]]
            buf = buffers[view["buffer"]]
            off = view.get("byteOffset", 0)
            data = buf[off:off + view["byteLength"]]
        enc = sniff_image(data)
        base = img.get("name") or f"image{src}"
        # glTF image.name 常自带扩展名（如 "CesiumMilkTruck.jpg"）→ 去掉再拼真实扩展，避免 .jpg.jpg
        for ext0 in (".png", ".jpg", ".jpeg", ".webp", ".gif", ".ktx2", ".avif"):
            if base.lower().endswith(ext0):
                base = base[:-len(ext0)]
                break
        os.makedirs(tex_dir, exist_ok=True)
        if name_registry is not None:
            # 不同 image 用了同名（或都没名字）→ 追加 _<src>，避免互相覆盖造成"串图"
            cand = base
            suffix = 0
            while cand in name_registry and name_registry[cand] != src:
                suffix += 1
                cand = f"{base}_{src}" if suffix == 1 else f"{base}_{src}_{suffix}"
            name_registry[cand] = src
            base = cand
        if enc in ("png", "jpeg"):
            ext = ".jpg" if enc == "jpeg" else ".png"
            name = base + ext
            with open(os.path.join(tex_dir, name), "wb") as f:
                f.write(data)
            images_out.add(name)
            return f"textures/{name}" + tex_suffix(tinfo)
        if enc in ("gif", "webp"):
            # 引擎 image 模块不支持 WebP（wuffs 无解码器）；用 PIL 离线转 PNG（可选依赖）。
            png = transcode_to_png(data, base)
            if png is not None:
                name = base + ".png"
                with open(os.path.join(tex_dir, name), "wb") as f:
                    f.write(png)
                images_out.add(name)
                print(f"gltf_import: 贴图 image={src} ({enc}) → PNG 转码（PIL）", file=sys.stderr)
                return f"textures/{name}" + tex_suffix(tinfo)
            print(f"gltf_import: 跳过 {enc} 贴图 image={src}（PIL 不可用），该贴图将缺失", file=sys.stderr)
            return ""
        print(f"gltf_import: 跳过不支持的贴图 image={src}（magic={enc}, mimeType={img.get('mimeType')}）",
              file=sys.stderr)
        return ""

    # uv1_mask：bit0..7 = base/mr/ao/normal/emissive/matcap/gradient/clearcoat_normal
    # 使用 TEXCOORD_1（仅非 0 时写；对应网格必须 vertex_layout=1，uv1 数据在顶点末尾）。
    UV1_BIT = {"base": 1 << 0, "mr": 1 << 1, "ao": 1 << 2, "normal": 1 << 3,
               "emissive": 1 << 4, "matcap": 1 << 5, "gradient": 1 << 6, "clearcoat_normal": 1 << 7}
    uv1_mask = 0

    def mark_uv1(bit, tinfo):
        nonlocal uv1_mask
        if effective_texcoord(tinfo) > 0:
            uv1_mask |= UV1_BIT[bit]

    pbr_tex = pbr.get("baseColorTexture")
    slot = tex_slot(pbr_tex)
    if slot:
        m["base_color_texture"] = slot
    mark_uv1("base", pbr_tex)
    mr_tex = pbr.get("metallicRoughnessTexture")
    slot = tex_slot(mr_tex)
    if slot:
        m["metallic_roughness_texture"] = slot
    mark_uv1("mr", mr_tex)
    normal_tex = mat.get("normalTexture")
    slot = tex_slot(normal_tex)
    if slot:
        m["normal_texture"] = slot
    mark_uv1("normal", normal_tex)
    if normal_tex:
        # glTF normalTexture.scale（缺省 1）→ 引擎 normal_scale / normal_scale_y
        # （three.js 语义：两轴同值；引擎把 x/y 拆开是为了兼容手写 scn）
        put_num("normal_scale", normal_tex.get("scale", 1.0), 1.0)
        put_num("normal_scale_y", normal_tex.get("scale", 1.0), 1.0)
    occ_tex = mat.get("occlusionTexture")
    slot = tex_slot(occ_tex)
    if slot:
        m["occlusion_texture"] = slot
    mark_uv1("ao", occ_tex)
    if occ_tex:
        # glTF occlusionTexture.strength（缺省 1）→ 引擎 occlusion_strength
        put_num("occlusion_strength", occ_tex.get("strength", 1.0), 1.0)
    emissive_tex = mat.get("emissiveTexture")
    slot = tex_slot(emissive_tex)
    if slot:
        m["emissive_texture"] = slot
    mark_uv1("emissive", emissive_tex)
    # KHR_texture_transform（plans/19 PBR 正确性）：基础贴图一组 UV，法线单独一组。
    # 只写非默认值（旧无变换资产逐字节不变）；rotation（弧度，glTF T*R*S 约定）→
    # 引擎 uv_rotation / normal_uv_rotation（schema 新增 float；仅非 0 写）。
    def tex_transform(tinfo):
        t = ((tinfo or {}).get("extensions") or {}).get("KHR_texture_transform")
        return t if isinstance(t, dict) else None

    def put_uv(prefix, tr):
        if not tr:
            return
        sc = [float(x) for x in tr.get("scale", [1.0, 1.0])]
        of = [float(x) for x in tr.get("offset", [0.0, 0.0])]
        if sc != [1.0, 1.0]:
            m[prefix + "_scale"] = {"x": sc[0], "y": sc[1]}
        if of != [0.0, 0.0]:
            m[prefix + "_offset"] = {"x": of[0], "y": of[1]}
        try:
            rot = float(tr.get("rotation", 0.0) or 0.0)
        except (TypeError, ValueError):
            rot = 0.0
        if abs(rot) > 1e-9:
            m[prefix + "_rotation"] = rot

    put_uv("uv", tex_transform(pbr_tex))
    put_uv("normal_uv", tex_transform(normal_tex))
    # clearcoat 法线贴图：引擎有独立槽（clearcoat_normal_texture + scale）。
    if "KHR_materials_clearcoat" in exts:
        cn = (exts["KHR_materials_clearcoat"] or {}).get("clearcoatNormalTexture")
        slot = tex_slot(cn)
        if slot:
            m["clearcoat_normal_texture"] = slot
            put_num("clearcoat_normal_scale", cn.get("scale", 1.0), 1.0)
        mark_uv1("clearcoat_normal", cn)
    # KHR 扩展材质贴图槽（plans/20 第 2 轮）：7 个 glTF textureInfo → 字符串字段（仅非空时写；
    # 无这些扩展/无贴图的材质输出逐字节不变）。复用 tex_slot（sampler 后缀/同名去重/URL 解码）。
    # UV 近似：引擎对这些槽复用 uv_scale/uv_offset（TEXCOORD_0）——不支持逐槽 texCoord/transform。
    ext_tex_slots = []
    if "KHR_materials_clearcoat" in exts:
        cc2 = exts["KHR_materials_clearcoat"] or {}
        ext_tex_slots += [("clearcoat_texture", cc2.get("clearcoatTexture")),
                          ("clearcoat_roughness_texture", cc2.get("clearcoatRoughnessTexture"))]
    if "KHR_materials_sheen" in exts:
        sh2 = exts["KHR_materials_sheen"] or {}
        ext_tex_slots += [("sheen_color_texture", sh2.get("sheenColorTexture")),
                          ("sheen_roughness_texture", sh2.get("sheenRoughnessTexture"))]
    if "KHR_materials_iridescence" in exts:
        ir2 = exts["KHR_materials_iridescence"] or {}
        ext_tex_slots += [("iridescence_texture", ir2.get("iridescenceTexture")),
                          ("iridescence_thickness_texture", ir2.get("iridescenceThicknessTexture"))]
    if "KHR_materials_specular" in exts:
        sp2 = exts["KHR_materials_specular"] or {}
        ext_tex_slots += [("specular_texture", sp2.get("specularTexture")),
                          ("specular_color_texture", sp2.get("specularColorTexture"))]
    if "KHR_materials_anisotropy" in exts:
        an2 = exts["KHR_materials_anisotropy"] or {}
        ext_tex_slots += [("anisotropy_texture", an2.get("anisotropyTexture"))]
    if "KHR_materials_transmission" in exts:
        # transmissionTexture：R = 透射率掩码（线性数据贴图，0=不透、1=全透；runtime 用 mips 非 sRGB）。
        tr2 = exts["KHR_materials_transmission"] or {}
        ext_tex_slots += [("transmission_texture", tr2.get("transmissionTexture"))]
    if "KHR_materials_diffuse_transmission" in exts:
        # diffuseTransmissionTexture：A = 透射率乘子（线性数据）；diffuseTransmissionColorTexture：
        # RGB = 透射色（sRGB）。两者分别乘 factor / colorFactor（KHR 规范）。
        dt2 = exts["KHR_materials_diffuse_transmission"] or {}
        ext_tex_slots += [("diffuse_transmission_texture", dt2.get("diffuseTransmissionTexture")),
                          ("diffuse_transmission_color_texture", dt2.get("diffuseTransmissionColorTexture"))]
    if "KHR_materials_volume" in exts:
        # thicknessTexture：G = 厚度乘子（线性数据；乘 thicknessFactor）。
        vv = exts["KHR_materials_volume"] or {}
        ext_tex_slots += [("volume_thickness_texture", vv.get("thicknessTexture"))]
    for field, tinfo in ext_tex_slots:
        slot = tex_slot(tinfo)
        if slot:
            m[field] = slot
    if uv1_mask:
        m["uv1_mask"] = uv1_mask
    return m


# ---- 场景模式（M4e：把 glTF 场景迁移成 maque .scn）----
def _mat_identity():
    return [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def _mat_mul(a, b):
    o = [0.0] * 16
    for c in range(4):
        for r in range(4):
            o[c * 4 + r] = (a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                            a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3])
    return o


def _node_matrix(node):
    if "matrix" in node:
        return [float(x) for x in node["matrix"]]
    t = node.get("translation", [0.0, 0.0, 0.0])
    q = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    x, y, z, w = q
    m = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    r00 = 1 - 2 * (y * y + z * z); r01 = 2 * (x * y - z * w); r02 = 2 * (x * z + y * w)
    r10 = 2 * (x * y + z * w); r11 = 1 - 2 * (x * x + z * z); r12 = 2 * (y * z - x * w)
    r20 = 2 * (x * z - y * w); r21 = 2 * (y * z + x * w); r22 = 1 - 2 * (x * x + y * y)
    m[0] = r00 * s[0]; m[1] = r10 * s[0]; m[2] = r20 * s[0]
    m[4] = r01 * s[1]; m[5] = r11 * s[1]; m[6] = r21 * s[1]
    m[8] = r02 * s[2]; m[9] = r12 * s[2]; m[10] = r22 * s[2]
    m[12], m[13], m[14] = t[0], t[1], t[2]
    return m


INSTANCE_LIMIT = 4096


def _instance_matrices(js, buffers, node, node_name):
    """EXT_mesh_gpu_instancing.attributes → [node-local TRS 矩阵]（TRANSLATION/ROTATION/SCALE）。

    ROTATION 为四元数 xyzw；缺失属性用缺省（t=0, q=identity, s=1）。实例数 > INSTANCE_LIMIT 时
    截断并 warning（确定性：保留前 INSTANCE_LIMIT 个）。返回 [] = 无有效实例属性（按普通节点处理）。
    """
    ext = (node.get("extensions") or {}).get("EXT_mesh_gpu_instancing") or {}
    attrs = ext.get("attributes") or {}

    def rows(name):
        idx = attrs.get(name)
        if idx is None:
            return None
        sc = accessor_norm_scale(js, idx)
        return [[float(x) * sc for x in r] for r in accessor_values(js, buffers, idx)]

    tr, ro, sc = rows("TRANSLATION"), rows("ROTATION"), rows("SCALE")
    counts = [len(v) for v in (tr, ro, sc) if v is not None]
    if not counts:
        return []
    n = min(counts)                       # 规范要求各属性 count 一致；容错取最小
    if n > INSTANCE_LIMIT:
        print(f"gltf_import: EXT_mesh_gpu_instancing 实例数 {n} > {INSTANCE_LIMIT}"
              f"（node '{node_name}'）→ 截断到 {INSTANCE_LIMIT}", file=sys.stderr)
        n = INSTANCE_LIMIT
    mats = []
    for i in range(n):
        t = list(tr[i][:3]) if tr is not None else [0.0, 0.0, 0.0]
        q = list(ro[i][:4]) if ro is not None else [0.0, 0.0, 0.0, 1.0]
        s = list(sc[i][:3]) if sc is not None else [1.0, 1.0, 1.0]
        qn = math.sqrt(sum(v * v for v in q))
        if qn > 1e-12:
            q = [v / qn for v in q]
        mats.append(_node_matrix({"translation": t, "rotation": q, "scale": s}))
    return mats


def _decompose(m):
    """列主序矩阵 → (position, euler Rz*Ry*Rx 弧度, scale)，与 runtime m4_from_trs 约定一致。

    两个数值陷阱（迁移时 CesiumMilkTruck 整车翻倒的根因）：
      1) **万向锁**：|r20|→1（ry=±90°）时 r21/r22、r10/r00 都趋近 0，atan2 比值是浮点噪声 →
         rx/rz 随机。此时固定 rz=0，用 r01/r11 求 rx（β=+90°）或 -r01/r11（β=-90°）。
      2) **负行列式（镜像缩放）**：把 sx 取负、旋转列按负 sx 归一化，保证重组后仍是 proper rotation。
    """
    sx = math.sqrt(m[0] ** 2 + m[1] ** 2 + m[2] ** 2) or 1.0
    sy = math.sqrt(m[4] ** 2 + m[5] ** 2 + m[6] ** 2) or 1.0
    sz = math.sqrt(m[8] ** 2 + m[9] ** 2 + m[10] ** 2) or 1.0
    det = (m[0] * (m[5] * m[10] - m[6] * m[9])
           - m[4] * (m[1] * m[10] - m[2] * m[9])
           + m[8] * (m[1] * m[6] - m[2] * m[5]))
    if det < 0.0:
        sx = -sx
    r00, r10, r20 = m[0] / sx, m[1] / sx, m[2] / sx
    r01, r11, r21 = m[4] / sy, m[5] / sy, m[6] / sy
    r02, r12, r22 = m[8] / sz, m[9] / sz, m[10] / sz
    r20 = max(-1.0, min(1.0, r20))
    if abs(r20) > 1.0 - 1e-6:                  # 万向锁：ry = ±90°
        ry = math.copysign(math.pi / 2.0, -r20)
        rz = 0.0
        if r20 < 0.0:                          # β=+90°
            rx = math.atan2(r01, r11)
        else:                                  # β=-90°
            rx = math.atan2(-r01, r11)
    else:
        ry = math.asin(-r20)
        rx = math.atan2(r21, r22)
        rz = math.atan2(r10, r00)
    return (m[12], m[13], m[14]), (rx, ry, rz), (sx, sy, sz)


def _xform_point(m, p):
    return [m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]]


def _look_rotation(eye, center):
    d = [center[i] - eye[i] for i in range(3)]
    ln = math.sqrt(sum(v * v for v in d)) or 1.0
    d = [v / ln for v in d]
    return math.asin(max(-1.0, min(1.0, d[1]))), math.atan2(-d[0], -d[2])


def _light_rotation(propagation):
    """光传播方向 → 灯节点 rotation（runtime 用节点 -Z 轴作为传播方向）。"""
    p = [float(v) for v in propagation]
    ln = math.sqrt(sum(v * v for v in p)) or 1.0
    p = [v / ln for v in p]
    rx = math.asin(max(-1.0, min(1.0, p[1])))
    ry = math.atan2(-p[0], -p[2])
    return rx, ry


def _vec3(x, y, z):
    return {"x": float(x), "y": float(y), "z": float(z)}


def _vec4(r, g, b, a):
    return {"r": float(r), "g": float(g), "b": float(b), "a": float(a)}


def _light_type_code(gltf_type):
    """KHR_lights_punctual.type → render.LightType（0=directional / 1=point / 2=spot）。"""
    return {"directional": 0, "point": 1, "spot": 2}.get(str(gltf_type or "directional"), 0)


def _norm_light_intensity(gltf_type, intensity, radius):
    """glTF 灯光强度（directional=lux、point/spot=candela）→ 引擎观感单位。

    引擎自动太阳 intensity=2.6；glTF 物理单位常差几个数量级，故按**模型尺度**归一化到
    「典型受光距离 d_ref（≈半个模型半径）处的照度 ≈ 自动太阳」：
      · directional：`2.6 * intensity`（夹 [0,20]，避免 NodePerformanceTest 的 1e4 lux 过曝）；
      · point/spot：`2.6 * (d_ref² + 1) * intensity`（夹 [0,2000]）——d_ref²+1 抵消引擎
        衰减 `win²/(d²+1)` 中的 +1 与小模型尺度效应。
    这是**观感归一化**，不是物理单位换算（README/known_gaps 需注明）。
    """
    try:
        iv = float(intensity)
    except (TypeError, ValueError):
        iv = 1.0
    if iv < 0.0:
        iv = 0.0
    if _light_type_code(gltf_type) == 0:
        return max(0.0, min(20.0, 2.6 * iv))
    r = max(float(radius), 1e-3)
    d_ref = max(0.5 * r, 1e-3)
    return max(0.0, min(2000.0, 2.6 * (d_ref * d_ref + 1.0) * iv))


def _spot_angles_deg(light):
    """KHR spot → (inner_deg, outer_deg)。glTF 是半角弧度；缺省 inner=0 / outer=π/4。"""
    spot = light.get("spot") or {}
    try:
        inner = math.degrees(float(spot.get("innerConeAngle", 0.0)))
        outer = math.degrees(float(spot.get("outerConeAngle", math.pi / 4.0)))
    except (TypeError, ValueError):
        inner, outer = 0.0, 45.0
    inner = max(0.0, min(90.0, inner))
    outer = max(0.0, min(90.0, outer))
    if inner > outer:
        inner = outer
    return inner, outer


def _fit_camera_distance(center, bmin, bmax, dir_unit, fov_y_deg, aspect, margin=0.92):
    """求让包围盒 8 角点都落进视锥（NDC*|margin|）的相机距离。

    比"包围球半径 / tan(fov/2)"更贴合宽扁/长条模型：包围球对薄板会高估距离 → 画面过小。
    用相机基（forward=-dir、right、up）+ 角点投影，迭代缩放距离 8 次即可收敛。
    """
    fx, fy, fz = -dir_unit[0], -dir_unit[1], -dir_unit[2]          # forward（相机 → 中心）
    # right = normalize(cross(forward, world_up))；forward 接近 ±Y 时用 X 轴兜底
    ux, uy, uz = (1.0, 0.0, 0.0) if abs(fy) > 0.99 else (0.0, 1.0, 0.0)
    rx = fy * uz - fz * uy
    ry = fz * ux - fx * uz
    rz = fx * uy - fy * ux
    rl = math.sqrt(rx * rx + ry * ry + rz * rz) or 1.0
    rx, ry, rz = rx / rl, ry / rl, rz / rl
    # up = cross(right, forward)
    upx = ry * fz - rz * fy
    upy = rz * fx - rx * fz
    upz = rx * fy - ry * fx
    tan_y = math.tan(math.radians(fov_y_deg) * 0.5)
    tan_x = tan_y * aspect
    corners = [(bmin[0] if i & 1 else bmax[0],
                bmin[1] if i & 2 else bmax[1],
                bmin[2] if i & 4 else bmax[2]) for i in range(8)]
    radius = math.sqrt(sum(((bmax[i] - bmin[i]) * 0.5) ** 2 for i in range(3)))
    dist = max(1e-4, radius * 2.0)
    for _ in range(10):
        cx = center[0] + dir_unit[0] * dist
        cy = center[1] + dir_unit[1] * dist
        cz = center[2] + dir_unit[2] * dist
        worst = 1e-6
        for (px, py, pz) in corners:
            vx, vy, vz = px - cx, py - cy, pz - cz
            z = vx * fx + vy * fy + vz * fz
            if z <= 1e-6:
                worst = max(worst, 1e6)
                break
            nx = abs(vx * rx + vy * ry + vz * rz) / (z * tan_x)
            ny = abs(vx * upx + vy * upy + vz * upz) / (z * tan_y)
            worst = max(worst, nx, ny)
        if worst <= margin:
            # 尝试再靠近一点（避免过远）：按 worst 缩放
            dist *= max(0.9, worst / margin)
            if abs(worst / margin - 1.0) < 0.01:
                break
            continue
        dist *= worst / margin
        if dist > radius * 200.0:
            break
    return max(1e-4, dist)


# ---- 蒙皮 / 骨架 / 动画资产（plans/22 §2）----
CLIP_PATH_RANK = {"translation": 0, "rotation": 1, "scale": 2, "weights": 3}
MORPH_TARGET_LIMIT = 8


def _sanitize_file_base(name):
    """文件名基名清洗：非 [字母数字._-] → '_'（确定性；避免路径/非法字符）。"""
    base = "".join(ch if (ch.isalnum() or ch in "._-") else "_" for ch in str(name))
    return base or "unnamed"


def _unique_file_base(base, used):
    """同名追加 _2/_3…（确定性：按遍历顺序）。"""
    cand = base
    k = 1
    while cand in used:
        k += 1
        cand = f"{base}_{k}"
    used.add(cand)
    return cand


def _identity_local():
    """identity scene.Transform3D.local（层级模式下 primitive 子实体显式写全）。"""
    return {"position": _vec3(0.0, 0.0, 0.0), "rotation": _vec3(0.0, 0.0, 0.0),
            "scale": _vec3(1.0, 1.0, 1.0)}


def _round_json_floats(v, nd=6):
    """--compact：递归圆整 JSON 里的 float（int/bool/str 原样；与 mesh 顶点 6 位小数约定一致）。"""
    if isinstance(v, float):
        return round(v, nd)
    if isinstance(v, list):
        return [_round_json_floats(x, nd) for x in v]
    if isinstance(v, dict):
        return {k: _round_json_floats(x, nd) for k, x in v.items()}
    return v


def _write_json_file(path, obj, compact):
    """派生资产 JSON 落盘（确定性：非 compact 排序键 + 2 缩进；compact 6 位小数 + 紧凑分隔符）。"""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        if compact:
            json.dump(_round_json_floats(obj), f, ensure_ascii=False, separators=(",", ":"))
        else:
            json.dump(obj, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def _node_unique_names(js):
    """node index → 全场景唯一名（node.name 或 node<idx>；重名按文档顺序追加 #2/#3）。

    场景实体名、.skel.json 的 joint_names、.clip.json 的 node 字段共用这一张表（plans/22 §3.2）。
    """
    used = set()
    names = []
    for idx, node in enumerate(js.get("nodes", [])):
        node = node or {}
        base = node.get("name") or f"node{idx}"
        cand = base
        k = 1
        while cand in used:
            k += 1
            cand = f"{base}#{k}"
        used.add(cand)
        names.append(cand)
    return names


def _has_morph_targets(js):
    return any(prim.get("targets") for mesh in js.get("meshes", [])
               for prim in (mesh or {}).get("primitives", []))


def emit_skeletons(js, buffers, skel_dir, node_names, prims_meta, compact, rel_prefix):
    """每个引用 skin 的节点一份 .skel.json（plans/22 §2.2；返回 {node: 相对路径}, [文件路径]）。

    文件名优先用该节点 mesh 的资产名（`<mesh_name>.skel.json`，与 plans/22 一致）；同一 mesh 被多个
    蒙皮节点共享时（RecursiveSkeletons：84 个 skin 共用 1 个 mesh）追加节点名保证唯一。
    joint_names 用全场景唯一节点名（= 场景实体名），inverse_bind 为列主序 4x4。
    """
    if not (js.get("skins") and node_names):
        return {}, []
    os.makedirs(skel_dir, exist_ok=True)
    mesh_first = {}
    for meta in prims_meta:
        mesh_first.setdefault(meta["mi"], meta["mesh_name"])
    nodes = js.get("nodes", [])
    used = set()
    by_node = {}
    written = []
    for ni, node in enumerate(nodes):
        node = node or {}
        si = node.get("skin")
        mi = node.get("mesh")
        if not isinstance(si, int) or not (0 <= si < len(js["skins"])):
            continue
        if not isinstance(mi, int):
            continue                       # skin 只在带 mesh 的节点上生效
        if mi not in mesh_first:
            continue                       # 该 mesh 没有可导入的三角 primitive → 不需要骨架文件
        sk = js["skins"][si] or {}
        joints = sk.get("joints") or []
        joint_names = []
        for j in joints:
            if isinstance(j, int) and 0 <= j < len(node_names):
                joint_names.append(node_names[j])
            else:
                print(f"gltf_import: skin={si} 含非法关节节点下标 {j!r}（node={ni}）→ 用占位名",
                      file=sys.stderr)
                joint_names.append(f"node{j}")
        acc_idx = sk.get("inverseBindMatrices")
        ibm = []
        if isinstance(acc_idx, int) and 0 <= acc_idx < len(js.get("accessors", [])):
            for r in accessor_values(js, buffers, acc_idx):
                row = [float(x) for x in r[:16]]
                ibm.append(row + [0.0] * (16 - len(row)))
            if len(ibm) != len(joint_names):
                print(f"gltf_import: skin={si} inverseBindMatrices 数 {len(ibm)} != 关节数 "
                      f"{len(joint_names)}（node={ni}）→ 截断/补 identity", file=sys.stderr)
                while len(ibm) < len(joint_names):
                    ibm.append(_mat_identity())
                ibm = ibm[:len(joint_names)]
        else:
            ibm = [_mat_identity() for _ in joint_names]
        base = _sanitize_file_base(mesh_first.get(mi, node_names[ni]))
        if base in used:                   # 共享 mesh 的多个蒙皮节点（不同 skin）→ 加节点名区分
            base = _sanitize_file_base(f"{mesh_first.get(mi, node_names[ni])}.{node_names[ni]}")
        base = _unique_file_base(base, used)
        rec = {"version": 1, "name": node_names[ni], "joint_names": joint_names,
               "inverse_bind": ibm}
        path = os.path.join(skel_dir, base + ".skel.json")
        _write_json_file(path, rec, compact)
        by_node[ni] = f"{rel_prefix}/{base}.skel.json"
        written.append(path)
        print(f"gltf_import: skeleton → {path} joints={len(joint_names)}")
    return by_node, written


def emit_clips(js, buffers, anim_dir, node_names, compact, rel_prefix):
    """每个 animation 一份 .clip.json（plans/22 §2.3；返回 ([相对路径], [文件路径])）。

    轨道排序 = (node 名, translation<rotation<scale<weights, 原 channel 序)；pointer 通道跳过并 stderr
    汇总；CUBICSPLINE 取每关键帧 value（不取切线）按 LINEAR 近似并计数；无有效轨道的片段跳过。
    """
    animations = js.get("animations") or []
    if not (animations and node_names):
        return [], []
    os.makedirs(anim_dir, exist_ok=True)
    used = set()
    rel_paths = []
    written = []
    skipped_pointer = 0
    anim_with_pointer = 0
    cubic = 0
    for ai, anim in enumerate(animations):
        anim = anim or {}
        raw_name = anim.get("name") or f"animation{ai}"
        tracks = []
        anim_has_pointer = False
        for ch_idx, ch in enumerate(anim.get("channels") or []):
            ch = ch or {}
            tgt = ch.get("target") or {}
            ptr = ((tgt.get("extensions") or {}).get("KHR_animation_pointer") or {}).get("pointer")
            if ptr is not None:
                skipped_pointer += 1
                anim_has_pointer = True
                continue
            path = tgt.get("path")
            if path not in CLIP_PATH_RANK:
                continue
            ni = tgt.get("node")
            if not isinstance(ni, int) or not (0 <= ni < len(node_names)):
                print(f"gltf_import: 片段 '{raw_name}' 轨道目标节点非法 {ni!r} → 跳过", file=sys.stderr)
                continue
            samplers = anim.get("samplers") or []
            si = ch.get("sampler")
            if not isinstance(si, int) or not (0 <= si < len(samplers)):
                continue
            smp = samplers[si] or {}
            in_acc, out_acc = smp.get("input"), smp.get("output")
            if not isinstance(in_acc, int) or not isinstance(out_acc, int):
                continue
            try:
                times = [float(r[0]) for r in accessor_values(js, buffers, in_acc)]
                rows = [[float(x) for x in r] for r in accessor_values(js, buffers, out_acc)]
            except (KeyError, IndexError, TypeError, ValueError):
                print(f"gltf_import: 片段 '{raw_name}' channel {ch_idx} 采样数据无效 → 跳过",
                      file=sys.stderr)
                continue
            if not times or not rows:
                continue
            interp = str(smp.get("interpolation") or "LINEAR").upper()
            if interp == "CUBICSPLINE":
                # 每关键帧 3 行（in-tangent, value, out-tangent）→ 只取中间 value
                if len(rows) != 3 * len(times):
                    print(f"gltf_import: 片段 '{raw_name}' channel {ch_idx} CUBICSPLINE 行数不符 "
                          f"→ 跳过", file=sys.stderr)
                    continue
                rows = [rows[3 * k + 1] for k in range(len(times))]
                cubic += 1
                interp = "LINEAR"
            elif interp not in ("LINEAR", "STEP"):
                interp = "LINEAR"
            if path == "weights":
                # weights 通道：output 为 SCALAR，扁平值数 = 关键帧数 × mesh 的 morph target 数
                flat = [x for r in rows for x in r]
                if not flat or len(flat) % len(times):
                    print(f"gltf_import: 片段 '{raw_name}' channel {ch_idx} weights 值数不是关键帧"
                          f"整数倍 → 跳过", file=sys.stderr)
                    continue
                ncomp = len(flat) // len(times)
                values = [flat[k * ncomp:(k + 1) * ncomp] for k in range(len(times))]
            else:
                if len(rows) != len(times):
                    print(f"gltf_import: 片段 '{raw_name}' channel {ch_idx} 关键帧/值数不符 → 跳过",
                          file=sys.stderr)
                    continue
                if path in ("translation", "scale"):
                    if any(len(r) < 3 for r in rows):
                        continue
                    values = [r[:3] for r in rows]
                else:                      # rotation：xyzw
                    if any(len(r) < 4 for r in rows):
                        continue
                    values = [r[:4] for r in rows]
            tracks.append((node_names[ni], CLIP_PATH_RANK[path], ch_idx, {
                "node": node_names[ni], "path": path, "interpolation": interp,
                "times": times, "values": values}))
        if not tracks:
            print(f"gltf_import: 跳过片段 '{raw_name}'（无有效轨道；pointer 通道已跳过）",
                  file=sys.stderr)
            continue
        tracks.sort(key=lambda t: (t[0], t[1], t[2]))
        duration = 0.0
        for _, _, _, tr in tracks:
            if tr["times"]:
                duration = max(duration, tr["times"][-1])
        base = _unique_file_base(_sanitize_file_base(raw_name), used)
        rec = {"version": 1, "name": raw_name, "duration": duration,
               "tracks": [tr for _, _, _, tr in tracks]}
        path = os.path.join(anim_dir, base + ".clip.json")
        _write_json_file(path, rec, compact)
        rel_paths.append(f"{rel_prefix}/{base}.clip.json")
        written.append(path)
        print(f"gltf_import: clip → {path} tracks={len(tracks)} duration={duration:g}")
        if anim_has_pointer:
            anim_with_pointer += 1
    if skipped_pointer:
        print(f"gltf_import: 跳过 KHR_animation_pointer 通道 {skipped_pointer} 条"
              f"（片段 {anim_with_pointer} 个）", file=sys.stderr)
    if cubic:
        print(f"gltf_import: CUBICSPLINE 通道 {cubic} 条 → 取关键帧 value 按 LINEAR 近似",
              file=sys.stderr)
    return rel_paths, written


def emit_scene(js, out_dir, name, prims_meta, scene_mats=None, camera=None, buffers=None,
               hierarchy=False, node_names=None, skel_paths=None, clip_paths=None):
    """glTF 节点树 → maque .scn。

    默认（hierarchy=False，保持旧行为）：每个 (node, primitive) 一个**平级实体**（世界变换烘焙进 local）。
    hierarchy=True（模型有 skins / animations / morph targets 时自动启用，plans/22 §3.2）：
    每个 glTF node → 实体（`scene.Transform3D` = 节点本地 TRS）+ 嵌套 `children`；
    带 mesh 的节点每个 primitive 生成 identity 子实体；蒙皮 primitive 实体放**场景根** +
    `MeshRenderer.skin_path`（忽略节点世界变换）；有片段时追加 `anim_player` 根实体。
    camera（可选，来自 CLI）：{distance_scale, position[3], target[3], fov}；
    全空/None 时输出与旧版自动取景逐字节一致。
    KHR_lights_punctual：节点引用 → render.Light 实体（最多 8 盏、方向光优先、强度观感归一化），
    有灯光时不再追加默认太阳；KHR_node_visibility.visible=false → 跳过该节点及整棵子树。
    EXT_mesh_gpu_instancing：node 的实例 accessor（需 buffers）→ 每个 primitive 展开 N 个实体
    （世界矩阵 = node 世界矩阵 × 实例 TRS；实体名 `<node>_<inst>_<prim>`）。
    """
    nodes = js.get("nodes", [])
    scn_nodes = js.get("scenes", [{}])[js.get("scene", 0)].get("nodes", []) if js.get("scenes") else []
    gltf_lights = (((js.get("extensions") or {}).get("KHR_lights_punctual") or {})
                   .get("lights") or [])
    light_refs = []                    # [(light_index, world_matrix, node_name)]（节点遍历顺序）
    camera_refs = []                   # [(camera_index, world_matrix, node_name)]（节点遍历顺序）
    hidden_nodes = 0
    entities = []
    bounds_min = [1e30, 1e30, 1e30]
    bounds_max = [-1e30, -1e30, -1e30]

    def walk(ni, parent, depth=0):
        nonlocal hidden_nodes
        if depth > 128:
            return
        node = nodes[ni]
        node_exts = node.get("extensions") or {}
        # KHR_node_visibility：visible=false 的节点及其整棵子树不生成实体（灯光/mesh 都不）
        if (node_exts.get("KHR_node_visibility") or {}).get("visible", True) is False:
            hidden_nodes += 1
            return
        world = _mat_mul(parent, _node_matrix(node))
        # KHR_lights_punctual：记录节点引用的灯（世界变换在 collect 后用 radius 归一化强度）
        lref = (node_exts.get("KHR_lights_punctual") or {}).get("light")
        if isinstance(lref, int) and 0 <= lref < len(gltf_lights):
            light_refs.append((lref, world, node.get("name") or f"node{ni}"))
        cref = node.get("camera")
        if isinstance(cref, int):
            camera_refs.append((cref, world, node.get("name") or f"node{ni}"))
        mi = node.get("mesh")
        nm = node.get("name") or f"node{ni}"
        # EXT_mesh_gpu_instancing：节点 mesh 按 N 个实例 TRS 展开（world_i = node world × instance TRS）
        inst_mats = None
        if mi is not None and buffers is not None and node_exts.get("EXT_mesh_gpu_instancing"):
            inst_mats = _instance_matrices(js, buffers, node, nm)
            if not inst_mats:
                inst_mats = None
        if mi is not None:
            for meta in prims_meta:
                if meta["mi"] != mi:
                    continue
                if inst_mats is None:
                    instance_variants = [(None, world)]
                else:
                    instance_variants = [(i, _mat_mul(world, im)) for i, im in enumerate(inst_mats)]
                for inst_idx, w in instance_variants:
                    for cx in (meta["bounds_min"], meta["bounds_max"]):
                        lo = [cx[0], cx[1], cx[2]]
                        # 8 个角点 → 世界包围盒（用 min/max 两个角近似覆盖旋转）
                        for sx in (cx[0], meta["bounds_max"][0]):
                            for sy in (cx[1], meta["bounds_max"][1]):
                                for sz in (cx[2], meta["bounds_max"][2]):
                                    wp = _xform_point(w, [sx, sy, sz])
                                    for k in range(3):
                                        bounds_min[k] = min(bounds_min[k], wp[k])
                                        bounds_max[k] = max(bounds_max[k], wp[k])
                    pos, rot, scl = _decompose(w)
                    comps = [
                        {"type": "scene.Transform3D", "local": {
                            "position": _vec3(*pos), "rotation": _vec3(*rot), "scale": _vec3(*scl)}},
                        {"type": "render.MeshRenderer",
                         "mesh_path": "assets/" + meta["mesh_name"] + ".mesh.json"},
                    ]
                    if meta.get("material"):
                        comps.append(meta["material"])
                    ent_name = (f"{nm}_{meta['pi']}" if inst_idx is None
                                else f"{nm}_{inst_idx}_{meta['pi']}")
                    entities.append({"name": ent_name, "components": comps})
        for ci in node.get("children", []):
            walk(ci, world, depth + 1)

    # ---- 层级模式（plans/22 §3.2）：glTF node 树 1:1；skinned primitive 实体放场景根 ----
    skinned_entities = []
    root_node_entities = []
    mesh_entity_count = [0]

    def _accum_bounds(w, meta):
        """世界矩阵 × mesh 局部 bounds 的 8 角点 → 场景 AABB（与平级模式同一取景数学）。"""
        for i in range(8):
            sx = meta["bounds_max"][0] if i & 1 else meta["bounds_min"][0]
            sy = meta["bounds_max"][1] if i & 2 else meta["bounds_min"][1]
            sz = meta["bounds_max"][2] if i & 4 else meta["bounds_min"][2]
            wp = _xform_point(w, [sx, sy, sz])
            for k in range(3):
                bounds_min[k] = min(bounds_min[k], wp[k])
                bounds_max[k] = max(bounds_max[k], wp[k])

    def walk_h(ni, parent_world, depth=0):
        nonlocal hidden_nodes
        if depth > 128:
            return None
        node = nodes[ni] or {}
        node_exts = node.get("extensions") or {}
        # KHR_node_visibility：visible=false 的节点及其整棵子树不生成实体（灯光/mesh 都不）
        if (node_exts.get("KHR_node_visibility") or {}).get("visible", True) is False:
            hidden_nodes += 1
            return None
        local = _node_matrix(node)
        world = _mat_mul(parent_world, local)
        ent_name = node_names[ni] if node_names else (node.get("name") or f"node{ni}")
        # KHR_lights_punctual / camera：仍按节点**世界**变换烘焙到独立根实体
        lref = (node_exts.get("KHR_lights_punctual") or {}).get("light")
        if isinstance(lref, int) and 0 <= lref < len(gltf_lights):
            light_refs.append((lref, world, ent_name))
        cref = node.get("camera")
        if isinstance(cref, int):
            camera_refs.append((cref, world, ent_name))
        pos, rot, scl = _decompose(local)
        ent = {"name": ent_name, "components": [
            {"type": "scene.Transform3D", "local": {
                "position": _vec3(*pos), "rotation": _vec3(*rot), "scale": _vec3(*scl)}}]}
        children = []
        mi = node.get("mesh")
        skin_idx = node.get("skin")
        skel_rel = skel_paths.get(ni) if skel_paths else None
        inst_mats = None
        if mi is not None and buffers is not None and node_exts.get("EXT_mesh_gpu_instancing"):
            inst_mats = _instance_matrices(js, buffers, node, ent_name)
            if not inst_mats:
                inst_mats = None
        if mi is not None:
            for meta in prims_meta:
                if meta["mi"] != mi:
                    continue
                if inst_mats is None:
                    variants = [(None, world, None)]
                else:
                    variants = [(i, _mat_mul(world, im), im) for i, im in enumerate(inst_mats)]
                for inst_idx, w, im in variants:
                    _accum_bounds(w, meta)
                    mesh_name = (f"{ent_name}_{meta['pi']}" if inst_idx is None
                                 else f"{ent_name}_{inst_idx}_{meta['pi']}")
                    if im is None:
                        ent_local = _identity_local()
                    else:                  # 实例 TRS 作为子实体本地变换（node 变换已在父实体）
                        ipos, irot, iscl = _decompose(im)
                        ent_local = {"position": _vec3(*ipos), "rotation": _vec3(*irot),
                                     "scale": _vec3(*iscl)}
                    mr = {"type": "render.MeshRenderer",
                          "mesh_path": "assets/" + meta["mesh_name"] + ".mesh.json"}
                    # 蒙皮 primitive：忽略节点世界变换（glTF 规范）→ 场景根 + identity + skin_path
                    skinned_here = bool(meta.get("skinned") and isinstance(skin_idx, int) and skel_rel)
                    if skinned_here:
                        mr["skin_path"] = skel_rel
                        ent_local = _identity_local()
                    comps = [{"type": "scene.Transform3D", "local": ent_local}, mr]
                    if meta.get("material"):
                        comps.append(meta["material"])
                    mesh_ent = {"name": mesh_name, "components": comps}
                    mesh_entity_count[0] += 1
                    if skinned_here:
                        skinned_entities.append(mesh_ent)
                    else:
                        children.append(mesh_ent)
        for ci in node.get("children", []):
            child_ent = walk_h(ci, world, depth + 1)
            if child_ent is not None:
                children.append(child_ent)
        if children:
            ent["children"] = children
        return ent

    if hierarchy:
        for ni in scn_nodes:
            root_ent = walk_h(ni, _mat_identity())
            if root_ent is not None:
                root_node_entities.append(root_ent)
        entities = root_node_entities + skinned_entities
        if mesh_entity_count[0] == 0:
            # 纯灯光/visibility/pointer 场景没有 mesh：给自动机位一个单位包围盒兜底
            bounds_min = [-1.0, -1.0, -1.0]
            bounds_max = [1.0, 1.0, 1.0]
            print("gltf_import: hierarchy 场景无 mesh 实体 → 相机取景回退单位包围盒",
                  file=sys.stderr)
    else:
        for ni in scn_nodes:
            walk(ni, _mat_identity())
    if hidden_nodes:
        print(f"gltf_import: KHR_node_visibility=false 跳过 {hidden_nodes} 个隐藏节点（含整棵子树）",
              file=sys.stderr)
    if not entities:
        raise SystemExit("gltf_import: --scene 没有生成任何实体（缺 mesh 节点？）")
    center = [(bounds_min[i] + bounds_max[i]) * 0.5 for i in range(3)]
    half = [max(bounds_max[i] - bounds_min[i], 1e-6) * 0.5 for i in range(3)]
    radius = max(1e-3, math.sqrt(sum(h * h for h in half)))       # 包围球半径（半对角线）
    # KHR_lights_punctual → render.Light 实体（position/rotation 用节点世界变换；
    # glTF 与引擎都以节点 -Z 为传播方向。强度按模型尺度观感归一化，见 _norm_light_intensity）。
    light_entities = []
    for li, world, nm in light_refs:
        L = gltf_lights[li] or {}
        lt = str(L.get("type") or "directional")
        code = _light_type_code(lt)
        pos, rot, scl = _decompose(world)
        col = L.get("color")
        if not (isinstance(col, list) and len(col) == 3):
            col = [1.0, 1.0, 1.0]
        comp = {"type": "render.Light", "__mq_light_type__": code,
                "intensity": _norm_light_intensity(lt, L.get("intensity", 1.0), radius),
                "color": _vec4(col[0], col[1], col[2], 1.0)}
        if code in (1, 2):
            rng = L.get("range")
            comp["range"] = float(rng) if rng is not None else 0.0   # 0 = 无限远（引擎语义）
        if code == 2:
            inner, outer = _spot_angles_deg(L)
            comp["inner_angle"] = inner
            comp["outer_angle"] = outer
        light_entities.append({"name": f"{nm}_light", "components": [
            {"type": "scene.Transform3D", "local": {
                "position": _vec3(*pos), "rotation": _vec3(*rot), "scale": _vec3(*scl)}},
            comp]})
    if len(light_entities) > 8:
        # 引擎最多 8 盏（shader 循环）；方向光优先（阴影只作用于第 0 盏方向光），其余保持遍历顺序
        dirs = [e for e in light_entities if e["components"][1]["__mq_light_type__"] == 0]
        others = [e for e in light_entities if e["components"][1]["__mq_light_type__"] != 0]
        ordered = dirs + others
        dropped = len(ordered) - 8
        light_entities = ordered[:8]
        print(f"gltf_import: KHR_lights_punctual 共 {len(ordered)} 盏 > 8，按方向光优先丢弃 {dropped} 盏",
              file=sys.stderr)
    # glTF 自带相机（--use-gltf-camera）：取**节点遍历顺序的第一个**相机（glTF 无"激活相机"概念，
    # 默认场景里通常只有一个；多相机模型通常参考图是多视图拼接，仍走自动机位）。
    # 引擎只支持透视相机；orthographic 会打印警告并回退自动机位（不静默产出错机位）。
    camera = camera or {}
    imported_cam = None
    if camera.get("use_gltf") and camera_refs:
        ci, cworld, cname = camera_refs[0]
        gltf_cams = js.get("cameras") or []
        cdef = gltf_cams[ci] if 0 <= ci < len(gltf_cams) else {}
        ctype = str(cdef.get("type") or "perspective")
        if ctype != "perspective":
            print(f"gltf_import: 相机 '{cname}' 类型 {ctype}（引擎仅支持 perspective）→ 回退自动机位",
                  file=sys.stderr)
        else:
            p = cdef.get("perspective") or {}
            fov_y = math.degrees(float(p.get("yfov", 0.7)))
            cnear = max(1e-4, float(p.get("znear", 0.1)))
            cfar = float(p.get("zfar", max(cnear * 1000.0, 1000.0)))
            if not (0.0 < fov_y < 179.0):
                fov_y = 45.0
            if cfar <= cnear:
                cfar = cnear * 1000.0
            pos, rot, scl = _decompose(cworld)
            imported_cam = {"name": "camera", "components": [
                {"type": "scene.Transform3D", "local": {
                    "position": _vec3(*pos), "rotation": _vec3(*rot)}},
                {"type": "render.Camera3D", "fov_y": fov_y, "near": cnear, "far": cfar,
                 "clear_color": _vec4(0.13, 0.13, 0.14, 1.0)},
            ]}
            print(f"gltf_import: 使用 glTF 相机 '{cname}' fov={fov_y:.2f}° near={cnear:g} far={cfar:g} "
                  f"pos=({pos[0]:.4g},{pos[1]:.4g},{pos[2]:.4g})", file=sys.stderr)
    # 自动机位：3/4 视角（左上前），距离按包围盒 8 角点投射进视锥拟合（_fit_camera_distance）。
    # 旧公式固定 -1.2r/+0.55r/+2.1r 对 1 单位立方体太近；只用包围球半径又会对薄板/长条模型过远。
    # CLI 覆盖（迁移手调）：显式 position/target 优先于自动拟合；distance_scale 乘拟合距离。
    fov_y = float(camera["fov"]) if camera.get("fov") is not None else 45.0
    aspect = 1280.0 / 720.0
    dirv = (-0.485, 0.222, 0.849)                                  # 归一化后的 3/4 视角方向
    pos_override = camera.get("position")
    target_override = camera.get("target")
    if pos_override is not None:
        eye = [float(v) for v in pos_override]
        dist = max(1e-4, math.sqrt(sum((eye[i] - center[i]) ** 2 for i in range(3))))
    else:
        dist_fit = _fit_camera_distance(center, bounds_min, bounds_max, dirv, fov_y, aspect)
        scale = float(camera["distance_scale"]) if camera.get("distance_scale") is not None else 1.0
        dist = max(1e-4, dist_fit * scale)
        eye = [center[i] + dirv[i] * dist for i in range(3)]
    look_at = target_override if target_override is not None else center
    cam_near = max(1e-4, dist * 0.02)
    cam_far = max(dist * 10.0, (dist + radius) * 8.0)
    pitch, yaw = _look_rotation(eye, look_at)
    lrx, lry = _light_rotation((0.35, -0.85, -0.4))
    if imported_cam is not None:
        cam = imported_cam                                   # glTF 相机（不参与自动取景）
    else:
        cam = {"name": "camera", "components": [
            {"type": "scene.Transform3D", "local": {
                "position": _vec3(*eye), "rotation": _vec3(pitch, yaw, 0.0)}},
            {"type": "render.Camera3D", "fov_y": fov_y, "near": cam_near, "far": cam_far,
             "clear_color": _vec4(0.13, 0.13, 0.14, 1.0)},
        ]}
    sun = {"name": "sun", "components": [
        {"type": "scene.Transform3D", "local": {"rotation": _vec3(lrx, lry, 0.0)}},
        {"type": "render.Light", "intensity": 2.6,
         "color": _vec4(1.0, 0.96, 0.9, 1.0)},
    ]}
    # 有 glTF 灯光时用导入灯光替代默认太阳（避免双主光）；否则保持旧行为（零漂移）。
    scene_entities = ([cam] + light_entities + entities) if light_entities else ([cam, sun] + entities)
    # 层级模式 + 有片段：追加 anim_player 根实体（scn.AnimationPlayer.clips，plans/22 §3.2 第 5 条）
    if hierarchy and clip_paths:
        scene_entities.append({
            "name": "anim_player",
            "components": [{"type": "scn.AnimationPlayer", "clips": list(clip_paths),
                            "clip_index": 0, "playing": True, "loop": True,
                            "speed": 1.0, "time": 0.0}],
        })
    scn = {"title": name, "width": 1280, "height": 720, "views": 1,
           "entities": scene_entities}
    dest = os.path.join(out_dir, name + ".scn")
    # render.Light 的字段名 "type" 与组件判别键 "type" 同名：场景加载器按 JSON 键顺序应用
    # （先字符串判别组件类型、后数值写入 LightType，lights.scn 同款约定），故先写占位键再替换成
    # 第二个 "type"。无灯光场景不出现占位键 → 输出与旧版逐字节一致。
    text = json.dumps(scn, ensure_ascii=False, indent=2, sort_keys=False)
    if light_entities:
        text = text.replace('"__mq_light_type__"', '"type"')
    with open(dest, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
        f.write("\n")
    print(f"gltf_import: scene → {dest} entities={len(entities)} lights={len(light_entities)} "
          f"hidden={hidden_nodes} bounds={bounds_min}..{bounds_max}")
    if hierarchy:
        print(f"gltf_import: hierarchy nodes={len(root_node_entities)} "
              f"skinned_meshes={len(skinned_entities)} clips={len(clip_paths or [])}")
    return dest


def _round_mesh(m):
    m = dict(m)
    m['vertices'] = [[round(float(v), 6) for v in row] for row in m['vertices']]
    # plans/22 §2.1：--compact 时 joints（int 保持）/ weights / morph 增量同样 6 位小数圆整。
    if 'joints' in m:
        m['joints'] = [[int(j) for j in row] for row in m['joints']]
    if 'weights' in m:
        m['weights'] = [[round(float(v), 6) for v in row] for row in m['weights']]
    if 'morph' in m:
        morph = dict(m['morph'])
        morph['default_weights'] = [round(float(v), 6) for v in morph['default_weights']]
        morph['targets'] = [{
            "positions": [[round(float(v), 6) for v in row] for row in t["positions"]],
            "normals": [[round(float(v), 6) for v in row] for row in t.get("normals", [])],
        } for t in morph['targets']]
        m['morph'] = morph
    return m


def import_gltf(path, out_dir, name=None, emit_scene_flag=False, compact=False, camera=None):
    js, bin_chunk, base_dir = load_gltf(path)
    buffers = read_buffers(js, bin_chunk, base_dir)
    tex_dir = os.path.join(out_dir, "textures")
    name = name or os.path.splitext(os.path.basename(path))[0]
    images = set()
    tex_name_registry = {}
    meshes_out = []
    prims_meta = []
    # plans/22 §3.1：hierarchy = skins / animations / 任一 primitive.targets 三者之一存在。
    # 三者都不存在 → 保持旧平级 + 世界烘焙输出（逐字节不变）。
    hierarchy = (bool(js.get("skins")) or bool(js.get("animations"))
                 or _has_morph_targets(js))
    node_names = _node_unique_names(js) if hierarchy else []
    for mi, mesh in enumerate(js.get("meshes", [])):
        mesh_weights = (mesh or {}).get("weights")
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
            uvs = (accessor_values(js, buffers, attrs["TEXCOORD_0"])
                   if "TEXCOORD_0" in attrs else [[0.0, 0.0]] * n)
            # COLOR_0（可选，缺省白）：float 或 normalized ubyte/ushort；VEC3 → alpha=1。
            colors = None
            if "COLOR_0" in attrs:
                acc = attrs["COLOR_0"]
                sc = accessor_norm_scale(js, acc)
                colors = []
                for r in accessor_values(js, buffers, acc):
                    c = [float(x) * sc for x in r]
                    if len(c) == 3:
                        c.append(1.0)
                    colors.append(c[:4])
            # TEXCOORD_1（可选；缺省复制 uv0）：第二套 UV 与 uv0 同规则读取（含量化归一化）。
            uvs1 = None
            if "TEXCOORD_1" in attrs:
                acc = attrs["TEXCOORD_1"]
                sc = accessor_norm_scale(js, acc)
                uvs1 = [[float(r[0]) * sc, float(r[1]) * sc]
                        for r in accessor_values(js, buffers, acc)]
            indices = (accessor_values(js, buffers, prim["indices"])
                       if "indices" in prim else [[i] for i in range(n)])
            indices = [int(r[0]) for r in indices]
            # 缺 NORMAL：glTF 规范允许。**有索引时必须按真实 indices** 累积（旧实现传 range(n)
            # 把顶点序号当三角形汤 → 法线随机错）；无索引时 range(n) 才等价于索引序列。
            normals = (accessor_values(js, buffers, attrs["NORMAL"])
                       if "NORMAL" in attrs
                       else compute_normals(positions, indices if "indices" in prim else list(range(n))))
            mat_idx = prim.get("material", -1)
            mats = []
            if mat_idx >= 0:
                mats.append(material_from_gltf(js, js["materials"][mat_idx], images,
                                               tex_dir, base_dir, buffers, tex_name_registry))
            # ---- 蒙皮顶点（plans/22 §2.1）：JOINTS_0/WEIGHTS_0 → joints/weights（与 vertices 等长）----
            joints = None
            weights = None
            if "JOINTS_1" in attrs or "WEIGHTS_1" in attrs:
                print(f"gltf_import: 忽略 JOINTS_1/WEIGHTS_1（>4 影响；mesh={mi} prim={pi}，"
                      f"plans/22 只支持单组）", file=sys.stderr)
            if "JOINTS_0" in attrs and "WEIGHTS_0" in attrs:
                jrows = accessor_values(js, buffers, attrs["JOINTS_0"])
                wrows = accessor_values(js, buffers, attrs["WEIGHTS_0"])
                wsc = accessor_norm_scale(js, attrs["WEIGHTS_0"])
                if len(jrows) != n or len(wrows) != n:
                    print(f"gltf_import: JOINTS_0/WEIGHTS_0 元素数 {len(jrows)}/{len(wrows)} != "
                          f"顶点数 {n}（mesh={mi} prim={pi}）→ 忽略蒙皮数据", file=sys.stderr)
                else:
                    joints, weights = [], []
                    zero_w = 0
                    for jr, wr in zip(jrows, wrows):
                        jrow = [int(x) for x in jr[:4]] + [0] * max(0, 4 - len(jr))
                        joints.append(jrow[:4])
                        wrow = [float(x) * wsc for x in wr[:4]] + [0.0] * max(0, 4 - len(wr))
                        wrow = wrow[:4]
                        s = sum(wrow)
                        if s > 1e-8:
                            wrow = [x / s for x in wrow]      # 归一化：sum≈1
                        else:
                            zero_w += 1
                            wrow = [1.0, 0.0, 0.0, 0.0]       # 全 0 权重 → 退化到 joint0
                        weights.append(wrow)
                    if zero_w:
                        print(f"gltf_import: 蒙皮权重全 0 的顶点 {zero_w} 个（mesh={mi} prim={pi}）"
                              f"→ 退化为 joint0 权重 1", file=sys.stderr)
            elif "JOINTS_0" in attrs or "WEIGHTS_0" in attrs:
                print(f"gltf_import: JOINTS_0/WEIGHTS_0 缺一（mesh={mi} prim={pi}）→ 忽略蒙皮数据",
                      file=sys.stderr)
            # ---- morph targets（plans/22 §2.1 / S3）：每个 target 只存 POSITION/NORMAL 增量 ----
            morph = None
            targets = prim.get("targets") or []
            if targets:
                if len(targets) > MORPH_TARGET_LIMIT:
                    print(f"gltf_import: mesh={mi} prim={pi} morph target 数 {len(targets)} > "
                          f"{MORPH_TARGET_LIMIT} → 忽略后 {len(targets) - MORPH_TARGET_LIMIT} 个",
                          file=sys.stderr)
                    targets = targets[:MORPH_TARGET_LIMIT]
                target_list = []
                tangent_skipped = 0
                for t in targets:
                    t = t or {}
                    if "TANGENT" in t:
                        tangent_skipped += 1
                    if "POSITION" in t:
                        tpos = accessor_values(js, buffers, t["POSITION"])
                        if len(tpos) != n:
                            print(f"gltf_import: morph target POSITION 元素数 {len(tpos)} != 顶点数 "
                                  f"{n}（mesh={mi} prim={pi}）→ 零增量占位", file=sys.stderr)
                            tpos = [[0.0, 0.0, 0.0]] * n
                    else:
                        print(f"gltf_import: morph target 缺 POSITION（mesh={mi} prim={pi}）"
                              f"→ 零增量占位", file=sys.stderr)
                        tpos = [[0.0, 0.0, 0.0]] * n
                    tnor = []
                    if "NORMAL" in t:
                        tnor = accessor_values(js, buffers, t["NORMAL"])
                        if len(tnor) != n:
                            print(f"gltf_import: morph target NORMAL 元素数 {len(tnor)} != 顶点数 "
                                  f"{n}（mesh={mi} prim={pi}）→ 忽略法线增量", file=sys.stderr)
                            tnor = []
                    target_list.append({
                        "positions": [[float(x) for x in r[:3]] for r in tpos],
                        "normals": [[float(x) for x in r[:3]] for r in tnor]})
                dw = list(mesh_weights) if isinstance(mesh_weights, list) else []
                if len(dw) != len(target_list):
                    if dw:
                        print(f"gltf_import: mesh={mi} weights 长度 {len(dw)} != target 数 "
                              f"{len(target_list)} → 截断/补 0", file=sys.stderr)
                    dw = (dw + [0.0] * len(target_list))[:len(target_list)]
                morph = {"default_weights": [float(x) for x in dw], "targets": target_list}
                if tangent_skipped:
                    print(f"gltf_import: mesh={mi} prim={pi} 忽略 {tangent_skipped} 个 morph TANGENT "
                          f"增量（shader 用屏幕导数构造切线基）", file=sys.stderr)
            # 顶点布局 v1：有 COLOR_0 或材质任一贴图用 TEXCOORD_1 → 14 float / 56B
            # （pos3|normal3|color4|uv0_2|uv1_2）；否则维持旧布局（12 float / 48B，逐字节不变）。
            # 蒙皮另加 24B（joints u16x4 + weights f32x4）：layout 2 = 48+24 / layout 3 = 56+24。
            layout1 = colors is not None or bool(mats and mats[0].get("uv1_mask", 0))
            if joints is not None:
                vertex_layout = 3 if layout1 else 2
                vertex_stride = 80 if layout1 else 72
            else:
                vertex_layout = 1 if layout1 else 0
                vertex_stride = 56 if layout1 else 48
            verts = []
            for i in range(n):
                p, nr, uv = positions[i], normals[i], uvs[i]
                if layout1:
                    col = colors[i] if colors is not None else [1.0, 1.0, 1.0, 1.0]
                    uv1 = uvs1[i] if uvs1 is not None else uv
                    verts.append([p[0], p[1], p[2], nr[0], nr[1], nr[2],
                                  col[0], col[1], col[2], col[3],
                                  uv[0], uv[1], uv1[0], uv1[1]])
                else:
                    verts.append([p[0], p[1], p[2], nr[0], nr[1], nr[2],
                                  1.0, 0.0, 0.0, 1.0, uv[0], uv[1]])
            if repair_inside_out_mesh(verts, indices):
                nm = name if len(js.get("meshes", [])) == 1 and pi == 0 else f"{name}_{mi}_{pi}"
                print(f"gltf_import: mesh '{nm}' 闭合网格法线/绕序一致朝内 → 已翻正"
                      f"（法线取反 + 三角形绕序反转）", file=sys.stderr)
            xs = [p[0] for p in positions]; ys = [p[1] for p in positions]; zs = [p[2] for p in positions]
            mesh_name = name if len(js.get("meshes", [])) == 1 and pi == 0 else f"{name}_{mi}_{pi}"
            mesh_rec = {
                "name": mesh_name,
                "vertex_stride": vertex_stride,
                "vertex_layout": vertex_layout,
                "vertices": verts,
                "indices": indices,
                "bounds_min": [min(xs), min(ys), min(zs)],
                "bounds_max": [max(xs), max(ys), max(zs)],
                "materials": mats,
            }
            # 追加字段全部在尾部（紧凑输出时排在 materials 之后）；缺失 = 非蒙皮/非 morph，
            # 旧模型输出逐字节不变（plans/22 §2.1）。
            if joints is not None:
                mesh_rec["joints"] = joints
                mesh_rec["weights"] = weights
            if morph is not None:
                mesh_rec["morph"] = morph
            meshes_out.append(mesh_rec)
            prims_meta.append({"mi": mi, "pi": pi, "mesh_name": mesh_name,
                               "material": mats[0] if mats else None,
                               "skinned": joints is not None,
                               "morph": morph is not None,
                               "bounds_min": [min(xs), min(ys), min(zs)],
                               "bounds_max": [max(xs), max(ys), max(zs)]})
    if not meshes_out:
        raise SystemExit("gltf_import: 没有可导入的三角网格")
    mesh_dir = os.path.join(out_dir, "assets") if emit_scene_flag else out_dir
    os.makedirs(mesh_dir, exist_ok=True)
    written = []
    for m in meshes_out:
        dest = os.path.join(mesh_dir, m["name"] + ".mesh.json")
        with open(dest, "w", encoding="utf-8", newline="\n") as f:
            if compact:
                json.dump(_round_mesh(m), f, ensure_ascii=False, separators=(",", ":"))
            else:
                json.dump(m, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write("\n")
        written.append(dest)
    # ---- 骨架 / 动画派生资产（plans/22 §2.2/§2.3）：有 skin/animation 才写；--scene 时放 assets/ 下 ----
    skin_dir = os.path.join(out_dir, "assets", "skin") if emit_scene_flag else os.path.join(out_dir, "skin")
    anim_dir = os.path.join(out_dir, "assets", "anim") if emit_scene_flag else os.path.join(out_dir, "anim")
    skel_paths, _skel_written = emit_skeletons(
        js, buffers, skin_dir, node_names, prims_meta, compact,
        "assets/skin" if emit_scene_flag else "skin")
    clip_paths, _clip_written = emit_clips(
        js, buffers, anim_dir, node_names, compact,
        "assets/anim" if emit_scene_flag else "anim")
    if emit_scene_flag:
        emit_scene(js, out_dir, name, prims_meta, camera=camera, buffers=buffers,
                   hierarchy=hierarchy, node_names=node_names,
                   skel_paths=skel_paths, clip_paths=clip_paths)
    return written, sorted(images)


def repair_inside_out_mesh(verts, indices):
    """闭合实体网格若法线/绕序一致朝内 → 就地翻正（法线取反 + 每个三角形绕序反转），返回 True。

    为什么：glTF-Sample-Assets 里存在 "实心球/实体被整体做成内表面" 的资产（如 DirectionalLight 的
    三个测试球：源 NORMAL 相对球心一致朝内、三角形绕序也随之内向）。单面材质（doubleSided=false）下
    正确渲染器会剔掉外表面而只剩内表面：法线朝背 → 平行光只在轮廓掠射处点亮、且阴影贴图把远面判成
    被近面遮挡 → 整颗球发黑（"DirectionalLight 效果错误"）。

    判定很保守（避免误伤合法的"内表面"资产：房间内壁 Sponza、天空穹顶 VirtualCity 等）：
      ① 顶点数 ≥ 64、三角形数 ≥ 32；
      ② 顶点法线相对包围盒中心的平均 cos < -0.75（几乎全部朝内）；
      ③ 无向边中"恰好被 2 个三角形共享"的比例 ≥ 0.98（近似闭合实体）。
    """
    nv = len(verts)
    idx = indices
    if nv < 64 or len(idx) < 96:
        return False
    pos = [(v[0], v[1], v[2]) for v in verts]
    nor = [(v[3], v[4], v[5]) for v in verts]
    mn = [min(p[i] for p in pos) for i in range(3)]
    mx = [max(p[i] for p in pos) for i in range(3)]
    c = [(mn[i] + mx[i]) * 0.5 for i in range(3)]
    dot_sum = 0.0
    dot_cnt = 0
    for p, q in zip(pos, nor):
        r = (p[0] - c[0], p[1] - c[1], p[2] - c[2])
        lr = math.sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2])
        ln = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2])
        if lr < 1e-9 or ln < 1e-9:
            continue
        dot_sum += (r[0] * q[0] + r[1] * q[1] + r[2] * q[2]) / (lr * ln)
        dot_cnt += 1
    if dot_cnt < 64:
        return False
    avg_cos = dot_sum / dot_cnt
    if avg_cos >= -0.75:
        return False
    # 闭合性（无向边计数；顶点按坐标量化合并，容忍 UV 缝处的重复顶点）
    key = {}
    q_pos = [None] * nv
    for i, p in enumerate(pos):
        k = (round(p[0], 5), round(p[1], 5), round(p[2], 5))
        q_pos[i] = key.setdefault(k, len(key))
    edges = {}
    for t in range(0, len(idx) - 2, 3):
        a, b, d = q_pos[idx[t]], q_pos[idx[t + 1]], q_pos[idx[t + 2]]
        for e0, e1 in ((a, b), (b, d), (d, a)):
            e = (e0, e1) if e0 < e1 else (e1, e0)
            edges[e] = edges.get(e, 0) + 1
    if not edges:
        return False
    shared2 = sum(1 for v in edges.values() if v == 2)
    if shared2 / len(edges) < 0.98:
        return False
    # 翻正：法线取反 + 每个三角形绕序反转
    for v in verts:
        v[3] = -v[3]
        v[4] = -v[4]
        v[5] = -v[5]
    for t in range(0, len(idx) - 2, 3):
        idx[t + 1], idx[t + 2] = idx[t + 2], idx[t + 1]
    return True


def _parse_vec3(text):
    """CLI 坐标参数 "x,y,z" → [float, float, float]（argparse type）。"""
    try:
        parts = [p.strip() for p in str(text).split(",")]
        if len(parts) != 3:
            raise ValueError
        return [float(p) for p in parts]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"坐标应为 'x,y,z' 三个浮点数，收到: {text!r}") from exc


def main(argv):
    ap = argparse.ArgumentParser(description="glTF/GLB → maque .mesh.json + 贴图（离线、确定性）")
    ap.add_argument("input")
    ap.add_argument("--out", required=True, help="输出目录")
    ap.add_argument("--name", default=None, help="输出基名（默认取输入文件名）")
    ap.add_argument("--scene", action="store_true",
                    help="额外生成 <name>.scn（glTF 节点树；含 skins/animations/morph 时输出层级 children + "
                         "anim_player，否则旧的平级烘焙实体；含相机/方向光；可直接用 player 打开）")
    ap.add_argument("--compact", action="store_true",
                    help="网格 JSON 紧凑输出（大模型显著减小体积，仍可被运行时解析）")
    # 相机 CLI（只对 --scene 生效；迁移脚本用于自动取景微调 / 手动机位）
    ap.add_argument("--camera-distance-scale", type=float, default=None, metavar="F",
                    help="自动取景距离倍率（拟合距离 × F，默认 1）")
    ap.add_argument("--camera-position", type=_parse_vec3, default=None, metavar="x,y,z",
                    help="显式相机位置（世界坐标；与 --camera-target 搭配）")
    ap.add_argument("--camera-target", type=_parse_vec3, default=None, metavar="x,y,z",
                    help="相机看向点（默认包围盒中心）")
    ap.add_argument("--camera-fov", type=float, default=None, metavar="DEG",
                    help="相机垂直 FOV（度，默认 45）")
    ap.add_argument("--use-gltf-camera", action="store_true",
                    help="用 glTF 自带相机（节点遍历顺序第一个 perspective；orthographic 回退自动机位）")
    args = ap.parse_args(argv)
    camera = {}
    if args.camera_distance_scale is not None:
        if args.camera_distance_scale <= 0.0:
            ap.error("--camera-distance-scale 必须 > 0")
        camera["distance_scale"] = args.camera_distance_scale
    if args.camera_position is not None:
        camera["position"] = args.camera_position
    if args.camera_target is not None:
        camera["target"] = args.camera_target
    if args.camera_fov is not None:
        if not (0.0 < args.camera_fov < 179.0):
            ap.error("--camera-fov 必须在 (0, 179) 度之间")
        camera["fov"] = args.camera_fov
    if args.use_gltf_camera:
        if camera:
            ap.error("--use-gltf-camera 不能与 --camera-* 显式机位同时使用")
        camera["use_gltf"] = True
    if camera and not args.scene:
        ap.error("--camera-* / --use-gltf-camera 只在 --scene 生成场景时生效（请同时加 --scene）")
    written, images = import_gltf(args.input, args.out, args.name, args.scene, args.compact, camera)
    for w in written:
        print(f"gltf_import: → {w}")
    for img in images:
        print(f"gltf_import: texture → textures/{img}")
    print(f"gltf_import: OK meshes={len(written)} textures={len(images)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
