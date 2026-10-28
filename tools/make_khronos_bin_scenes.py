#!/usr/bin/env python3
"""生成 scenes/khronos-bin：与 scenes/khronos 相同的相机/灯光/环境，但模型用源 `.glb`。

用途：对比“离线 .mesh.json 资产”与“runtime 直读 GLB”的加载速度。每个模型场景只保留
旧场景里的 Camera3D/Light 实体（以及场景级 env_* 字段），模型整体改为一个
`render.MeshRenderer.mesh_path = assets/<Model>.glb` 实体；相机/灯光/环境逐字节复用旧 JSON。

优先复制 Khronos 源仓库的 `glTF-Binary/<Model>.glb`；缺失时用
`npx @gltf-transform/cli copy` 从 `glTF/<Model>.gltf` 生成（外部 buffer/贴图内嵌进 GLB）。

用法：
  python tools/make_khronos_bin_scenes.py [--src scenes/khronos] [--out scenes/khronos-bin]
        [--models-root .deps/gltf-assets/glTF-Sample-Assets-main/Models] [--jobs 4]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Pairs(list):
    """保留顺序与重复键（.scn 灯光组件依赖重复 "type" 键）。"""


def load_pairs(path):
    with open(path, encoding="utf-8") as f:
        return json.loads(f.read(), object_pairs_hook=Pairs)


def dump_pairs(v, indent=0):
    pad = "  " * indent
    if isinstance(v, Pairs):
        if not v:
            return "{}"
        body = ",\n".join(
            "%s  %s: %s" % (pad, json.dumps(k, ensure_ascii=False), dump_pairs(val, indent + 1))
            for k, val in v
        )
        return "{\n%s\n%s}" % (body, pad)
    if isinstance(v, list):
        if not v:
            return "[]"
        body = ",\n".join("%s  %s" % (pad, dump_pairs(e, indent + 1)) for e in v)
        return "[\n%s\n%s]" % (body, pad)
    return json.dumps(v, ensure_ascii=False)


def jget(obj, key, default=None):
    if isinstance(obj, Pairs):
        for k, v in obj:
            if k == key:
                return v
    return default


def component_types(entity):
    return [jget(c, "type") for c in (jget(entity, "components") or [])]


def has_mesh(entity):
    return "render.MeshRenderer" in component_types(entity)


def is_camera_or_light(entity):
    ts = component_types(entity)
    return "render.Camera3D" in ts or "render.Light" in ts


def model_entity(model):
    local = Pairs([
        ("position", Pairs([("x", 0.0), ("y", 0.0), ("z", 0.0)])),
        ("rotation", Pairs([("x", 0.0), ("y", 0.0), ("z", 0.0)])),
        ("scale", Pairs([("x", 1.0), ("y", 1.0), ("z", 1.0)])),
    ])
    return Pairs([
        ("name", "model"),
        ("components", [
            Pairs([("type", "scene.Transform3D"), ("local", local)]),
            Pairs([("type", "render.MeshRenderer"), ("mesh_path", "assets/%s.glb" % model)]),
        ]),
    ])


def build_scene(old_path, model):
    old = load_pairs(old_path)
    kept = [e for e in (jget(old, "entities") or []) if is_camera_or_light(e)]
    out = Pairs()
    for k, v in old:
        if k == "entities":
            out.append(("entities", kept + [model_entity(model)]))
        else:
            out.append((k, v))
    if not jget(old, "entities"):
        out.append(("entities", [model_entity(model)]))
    return dump_pairs(out) + "\n"


def find_source_glb(models_root, model):
    root = os.path.join(models_root, model)
    exact = os.path.join(root, "glTF-Binary", model + ".glb")
    if os.path.isfile(exact):
        return exact
    bin_dir = os.path.join(root, "glTF-Binary")
    if os.path.isdir(bin_dir):
        glbs = sorted(f for f in os.listdir(bin_dir) if f.lower().endswith(".glb"))
        if glbs:
            return os.path.join(bin_dir, glbs[0])
    return None


def find_source_gltf(models_root, model):
    root = os.path.join(models_root, model, "glTF")
    exact = os.path.join(root, model + ".gltf")
    if os.path.isfile(exact):
        return exact
    if os.path.isdir(root):
        for f in sorted(os.listdir(root)):
            if f.lower().endswith(".gltf"):
                return os.path.join(root, f)
    return None


def ensure_glb(models_root, model, dst):
    src = find_source_glb(models_root, model)
    if src:
        shutil.copyfile(src, dst)
        return "copy"
    gltf = find_source_gltf(models_root, model)
    if not gltf:
        raise RuntimeError("找不到源 .glb/.gltf: %s" % model)
    npx = shutil.which("npx") or "npx"
    if os.name == "nt":
        cmd = ["cmd", "/c", npx, "--yes", "@gltf-transform/cli", "copy", gltf, dst]
    else:
        cmd = [npx, "--yes", "@gltf-transform/cli", "copy", gltf, dst]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", cwd=REPO)
    if proc.returncode != 0 or not os.path.isfile(dst):
        raise RuntimeError("GLB 生成失败 %s\n%s\n%s" % (model, proc.stdout[-2000:], proc.stderr[-2000:]))
    return "convert"


def process_model(src_root, out_root, models_root, model):
    out_dir = os.path.join(out_root, model)
    os.makedirs(os.path.join(out_dir, "assets"), exist_ok=True)
    src_scn = os.path.join(src_root, model, model + ".scn")
    if not os.path.isfile(src_scn):
        raise RuntimeError("缺少场景: " + src_scn)
    mode = ensure_glb(models_root, model, os.path.join(out_dir, "assets", model + ".glb"))
    with open(os.path.join(out_dir, model + ".scn"), "w", encoding="utf-8", newline="\n") as f:
        f.write(build_scene(src_scn, model))
    return mode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join("scenes", "khronos"))
    ap.add_argument("--out", default=os.path.join("scenes", "khronos-bin"))
    ap.add_argument("--models-root",
                    default=os.path.join(".deps", "gltf-assets", "glTF-Sample-Assets-main", "Models"))
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    src_root = os.path.join(REPO, args.src)
    out_root = os.path.join(REPO, args.out)
    models_root = os.path.join(REPO, args.models_root)
    models = sorted(d for d in os.listdir(src_root) if os.path.isdir(os.path.join(src_root, d)))
    if args.only:
        want = set(x for x in args.only.split(",") if x)
        models = [m for m in models if m in want]
    os.makedirs(out_root, exist_ok=True)
    for name in ("index.scn", "gallery.js"):
        src = os.path.join(src_root, name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(out_root, name))

    results = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = {ex.submit(process_model, src_root, out_root, models_root, m): m for m in models}
        for fut in as_completed(futs):
            m = futs[fut]
            try:
                mode = fut.result()
                results.append((m, mode))
                print("[%d/%d] %-28s %s" % (len(results), len(models), m, mode), flush=True)
            except Exception as e:  # noqa: BLE001
                print("FAIL %s: %s" % (m, e), file=sys.stderr, flush=True)
                return 1

    readme = """# khronos-bin

与 `scenes/khronos` 相同的画廊（`index.scn` + `gallery.js`）与每个模型的 **相机/灯光/场景 env**，
但模型改为 runtime 直读的源 `.glb`（`assets/<Model>.glb`），不再经过离线 `.mesh.json` 资产。

用途：对比两种加载路径的速度（`tools/bench_khronos_load.py`）。

生成：`python tools/make_khronos_bin_scenes.py`（缺失 GLB 时会调用 `npx @gltf-transform/cli copy`）。
本目录的 `assets/*.glb` 是派生资产，体积大，不提交（见 `.gitignore`）。
"""
    with open(os.path.join(out_root, "README.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(readme)
    print("done: %d scenes -> %s" % (len(results), out_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
