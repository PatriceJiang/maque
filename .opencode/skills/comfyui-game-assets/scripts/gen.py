#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用本地 ComfyUI + Qwen Image 2.1 生成游戏素材 / UI 设计稿。

把测试过的 API-format 工作流（UNETLoader + CLIPLoader + VAELoader +
TextEncodeQwenImage21 + EmptyLatentImage + KSampler + VAEDecode + SaveImage）
提交给 ComfyUI，轮询等待完成，并把结果复制到当前项目目录。

只依赖 Python 标准库；Pillow 存在时做 PNG 校验（尺寸 / alpha 通道）。
ComfyUI 未启动时会自动尝试拉起（用 ~/Ext/ComfyUI 下的 venv）。

示例:
  python3 scripts/gen.py --prompt "pixel art sword icon, RPG item" --transparent
  python3 scripts/gen.py --prompt "主菜单 UI，标题「魔雀」，按钮 开始/设置/退出" --width 2048 --height 1152
  python3 scripts/gen.py --prompt "same hero, running pose" --ref hero.png --variants 3
"""

import argparse
import hashlib
import json
import mimetypes
import os
import random
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

DEFAULT_SERVER = "http://127.0.0.1:8188"
COMFYUI_DIR = Path(os.environ.get("COMFYUI_DIR", "~/Ext/ComfyUI")).expanduser()

# 与 references/api.md 保持一致；换模型时两处一起改。
MODELS = {
    "unet": "qwen_image_2.1_int8_convrot.safetensors",
    "clip": "qwen3vl_8b_int8_convrot.safetensors",
    "vae": "qwen_image_2.1_vae_bf16.safetensors",
}

TRANSPARENT_HEAD = "This is an RGBA format image with transparency."
TRANSPARENT_TAIL = "The image has an alpha channel and a transparent background."


# ---------------------------------------------------------------- HTTP 基础

def http_json(url, data=None, timeout=30):
    body = None
    headers = {}
    if data is not None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, headers=headers,
                                 method="POST" if body else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def http_bytes(url, timeout=120):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def http_multipart(url, fields, file_field, file_path, filename=None, timeout=120):
    boundary = uuid.uuid4().hex
    filename = filename or file_path.name
    body = bytearray()
    for k, v in fields.items():
        body += (f"--{boundary}\r\n"
                 f'Content-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n').encode("utf-8")
    ctype = mimetypes.guess_type(filename)[0] or "application/octet-stream"
    body += (f"--{boundary}\r\n"
             f'Content-Disposition: form-data; name="{file_field}"; '
             f'filename="{filename}"\r\n'
             f"Content-Type: {ctype}\r\n\r\n").encode("utf-8")
    body += file_path.read_bytes() + b"\r\n"
    body += f"--{boundary}--\r\n".encode("utf-8")
    req = urllib.request.Request(
        url, data=bytes(body), method="POST",
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


# ------------------------------------------------------------ 服务器管理

def server_up(server, timeout=4):
    try:
        http_json(server + "/system_stats", timeout=timeout)
        return True
    except Exception:
        return False


def start_server(log_path):
    py = COMFYUI_DIR / ".venv" / "bin" / "python3"
    if not py.exists():
        py = COMFYUI_DIR / ".venv" / "bin" / "python"
    if not py.exists():
        py = shutil.which("python3")
        if py is None:
            raise RuntimeError("找不到 python3，无法启动 ComfyUI")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    logf = open(log_path, "ab")
    subprocess.Popen([str(py), "main.py"], cwd=str(COMFYUI_DIR),
                     stdout=logf, stderr=subprocess.STDOUT,
                     start_new_session=True)
    return log_path


def ensure_server(server, auto_start, wait_s):
    if server_up(server):
        return
    if not auto_start:
        sys.exit(f"[gen] ComfyUI 未在 {server} 运行（--no-start 已给出，不自动拉起）")
    host = urllib.parse.urlparse(server).hostname or ""
    if host not in ("127.0.0.1", "localhost", "::1"):
        sys.exit(f"[gen] ComfyUI 未在 {server} 运行，且远程服务器不能自动启动")
    if not (COMFYUI_DIR / "main.py").exists():
        sys.exit(f"[gen] ComfyUI 未运行，且 {COMFYUI_DIR} 下找不到 main.py"
                 "（可用环境变量 COMFYUI_DIR 指定安装目录）")
    log = start_server(COMFYUI_DIR / "logs" / "skill_gen_server.log")
    print(f"[gen] ComfyUI 未运行，尝试启动（日志 {log}）…", flush=True)
    t0 = time.time()
    while time.time() - t0 < wait_s:
        time.sleep(3)
        if server_up(server):
            print(f"[gen] ComfyUI 已就绪（{int(time.time() - t0)}s）", flush=True)
            return
    sys.exit(f"[gen] 等待 ComfyUI 启动超时（{wait_s}s），请查看 {log}")


# -------------------------------------------------------------- 工作流

def wrap_transparent(prompt):
    p = prompt.strip()
    if not p.endswith((".", "。", "!", "！", "?", "？")):
        p += "."
    return f"{TRANSPARENT_HEAD} {p} {TRANSPARENT_TAIL}"


def upload_ref(server, path):
    """上传参考图（文件名带内容哈希，避免重名互相覆盖）。返回 ComfyUI 里的名字。"""
    data = path.read_bytes()
    digest = hashlib.sha1(data).hexdigest()[:10]
    name = f"skillref_{digest}{path.suffix.lower() or '.png'}"
    resp = http_multipart(server + "/upload/image", {"overwrite": "true"},
                          "image", path, filename=name)
    uploaded = resp.get("name") or name
    if resp.get("subfolder"):
        uploaded = f"{resp['subfolder']}/{uploaded}"
    return uploaded


def build_graph(args, ref_names):
    prompt = wrap_transparent(args.prompt) if args.transparent else args.prompt
    g = {
        "1": {"class_type": "UNETLoader",
              "inputs": {"unet_name": MODELS["unet"], "weight_dtype": "default"}},
        "2": {"class_type": "CLIPLoader",
              "inputs": {"clip_name": MODELS["clip"], "type": "qwen_image",
                         "device": "default"}},
        "3": {"class_type": "VAELoader",
              "inputs": {"vae_name": MODELS["vae"]}},
        "5": {"class_type": "EmptyLatentImage",
              "inputs": {"width": args.width, "height": args.height,
                         "batch_size": args.variants}},
        "6": {"class_type": "KSampler",
              "inputs": {"model": ["1", 0], "positive": ["4", 0],
                         "negative": ["4", 1], "latent_image": ["5", 0],
                         "seed": args.seed, "steps": args.steps, "cfg": args.cfg,
                         "sampler_name": args.sampler,
                         "scheduler": args.scheduler, "denoise": 1.0}},
        "7": {"class_type": "VAEDecode",
              "inputs": {"samples": ["6", 0], "vae": ["3", 0]}},
        "8": {"class_type": "SaveImage",
              "inputs": {"images": ["7", 0],
                         "filename_prefix": f"skillgen/{args.name}"}},
    }
    te_inputs = {"clip": ["2", 0], "prompt": prompt,
                 "negative_prompt": args.negative or "",
                 "resolution": 1024}
    if ref_names:
        te_inputs["vae"] = ["3", 0]
        for i, name in enumerate(ref_names):
            nid = str(20 + i)
            g[nid] = {"class_type": "LoadImage", "inputs": {"image": name}}
            te_inputs[f"images.image_{i + 1}"] = [nid, 0]
    g["4"] = {"class_type": "TextEncodeQwenImage21", "inputs": te_inputs}
    return g, prompt


# ---------------------------------------------------------------- 提交/轮询

def submit(server, graph):
    resp = http_json(server + "/prompt", {"prompt": graph}, timeout=60)
    errors = resp.get("node_errors") or {}
    if errors:
        sys.exit("[gen] 工作流校验失败：\n" +
                 json.dumps(errors, ensure_ascii=False, indent=2))
    return resp["prompt_id"]


def wait_done(server, prompt_id, timeout):
    t0 = time.time()
    last = 0.0
    while True:
        try:
            hist = http_json(f"{server}/history/{prompt_id}", timeout=15)
        except Exception:
            hist = None
        if hist:
            return hist[prompt_id], time.time() - t0
        elapsed = time.time() - t0
        if elapsed > timeout:
            sys.exit(f"[gen] 等待超时（{timeout}s）。可能排队较长或显存不足；"
                     f"可查 {server}/queue、调大 --timeout，或 POST {server}/interrupt 取消")
        if elapsed - last >= 15:
            last = elapsed
            print(f"[gen] 已等待 {int(elapsed)}s（模型冷加载约需 1-3 分钟）…", flush=True)
        time.sleep(3)


def exec_seconds(entry):
    ts = {m[0]: m[1].get("timestamp") for m in entry.get("status", {}).get("messages", [])}
    if "execution_start" in ts and "execution_success" in ts:
        return (ts["execution_success"] - ts["execution_start"]) / 1000.0
    return None


def fetch_results(server, entry, out_dir, name):
    imgs = []
    for node_out in entry.get("outputs", {}).values():
        imgs.extend(node_out.get("images", []))
    saved = []
    for i, im in enumerate(imgs):
        suffix = "" if len(imgs) == 1 else f"_{i:02d}"
        dest = out_dir / f"{name}{suffix}.png"
        qs = urllib.parse.urlencode({"filename": im["filename"],
                                     "subfolder": im.get("subfolder", ""),
                                     "type": im.get("type", "output")})
        dest.write_bytes(http_bytes(f"{server}/view?{qs}"))
        saved.append(dest)
    return saved


# ---------------------------------------------------------------- 校验

def verify_png(path, want_transparent, expect_size):
    info = {"file": str(path), "bytes": path.stat().st_size}
    warnings = []
    try:
        from PIL import Image
    except ImportError:
        info["note"] = "未安装 Pillow，跳过图像校验"
        return info, warnings
    with Image.open(path) as im:
        info["size"] = list(im.size)
        info["mode"] = im.mode
        if im.size != expect_size:
            warnings.append(f"尺寸 {im.size} 与请求 {expect_size} 不一致")
        if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
            info["has_alpha"] = True
            a = im.convert("RGBA").getchannel("A")
            total = im.size[0] * im.size[1]
            hist = a.histogram()
            transparent = sum(hist[:250])
            subject = total - sum(hist[:16])
            info["transparent_pct"] = round(100.0 * transparent / total, 1)
            info["subject_pct"] = round(100.0 * subject / total, 1)
            if subject < total * 0.003:
                warnings.append("图像几乎全透明，可能是空图")
        else:
            info["has_alpha"] = False
            if want_transparent:
                warnings.append(
                    "要求透明背景但输出没有 alpha 通道：可换 seed 重试，"
                    "或按键色扣背景（见 SKILL.md「透明背景」小节）")
    return info, warnings


# ---------------------------------------------------------------- main

def slugify(text, maxlen=40):
    s = re.sub(r"[^0-9a-zA-Z\u4e00-\u9fff]+", "-", text.strip())[:maxlen].strip("-")
    s = re.sub(r"[\u4e00-\u9fff]+", lambda m: m.group(0)[:8], s)
    s = s.strip("-")
    return s or "asset"


def snap32(v):
    r = round(v / 32) * 32
    return max(64, r)


def main():
    ap = argparse.ArgumentParser(
        description="用本地 ComfyUI (Qwen Image 2.1) 生成游戏素材/UI 图",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--prompt", help="画面描述（中文/英文均可）")
    ap.add_argument("--prompt-file", help="从文件读取 prompt（长 prompt 更省心）")
    ap.add_argument("--negative", default="", help="负向提示词（cfg=1 时无效）")
    ap.add_argument("--width", type=int, default=1024)
    ap.add_argument("--height", type=int, default=1024)
    ap.add_argument("--steps", type=int, default=25, help="官方建议 25-50")
    ap.add_argument("--cfg", type=float, default=1.0, help="官方面向 Qwen Image 2.1 保持 1.0")
    ap.add_argument("--sampler", default="euler")
    ap.add_argument("--scheduler", default="simple")
    ap.add_argument("--seed", type=int, default=None, help="不给出则随机；给出可复现")
    ap.add_argument("--variants", type=int, default=1, help="一次生成几张（batch）")
    ap.add_argument("--transparent", action="store_true", help="透明背景（RGBA），适合 sprite/图标")
    ap.add_argument("--ref", action="append", nargs="+", default=[], metavar="PATH",
                    help="参考图（可多次/多张，最多 16 张），用于一致性改图")
    ap.add_argument("--out", default="assets/generated", help="相对当前项目目录的输出目录")
    ap.add_argument("--name", default=None, help="文件名（默认取 prompt 摘要）")
    ap.add_argument("--server", default=DEFAULT_SERVER)
    ap.add_argument("--timeout", type=int, default=1800, help="等待生成的总秒数")
    ap.add_argument("--start-timeout", type=int, default=300, help="自动拉起 ComfyUI 的等待秒数")
    ap.add_argument("--no-start", action="store_true", help="服务器没开时不自动启动")
    ap.add_argument("--meta", action="store_true", help="在图片旁写 .meta.json（记录全部参数）")
    ap.add_argument("--json", action="store_true", help="只输出一行 JSON 摘要")
    args = ap.parse_args()

    if args.prompt_file:
        args.prompt = Path(args.prompt_file).read_text(encoding="utf-8").strip()
    if not args.prompt:
        ap.error("需要 --prompt 或 --prompt-file")
    args.name = args.name or slugify(args.prompt)
    for attr in ("width", "height"):
        v = getattr(args, attr)
        snapped = snap32(v)
        if snapped != v:
            print(f"[gen] {attr}={v} 已对齐到 32 的倍数：{snapped}", file=sys.stderr)
        setattr(args, attr, snapped)
    if args.variants < 1 or args.variants > 16:
        ap.error("--variants 支持 1-16")
    refs = [Path(p).expanduser() for group in args.ref for p in group]
    if len(refs) > 16:
        ap.error("参考图最多 16 张")
    for r in refs:
        if not r.exists():
            sys.exit(f"[gen] 参考图不存在：{r}")
    if args.cfg != 1.0 and not args.negative:
        print("[gen] 提醒：cfg>1 但 negative 为空，引导会退化为普通 cfg", file=sys.stderr)
    if args.seed is None:
        args.seed = random.randrange(0, 2 ** 62)

    ensure_server(args.server, not args.no_start, args.start_timeout)

    ref_names = [upload_ref(args.server, r) for r in refs]
    graph, final_prompt = build_graph(args, ref_names)
    prompt_id = submit(args.server, graph)
    print(f"[gen] 已提交 {prompt_id}（{args.width}x{args.height} x{args.variants}，"
          f"steps={args.steps} cfg={args.cfg} seed={args.seed}）", flush=True)

    entry, wall = wait_done(args.server, prompt_id, args.timeout)
    status = entry["status"].get("status_str")
    if status != "success":
        msgs = entry["status"].get("messages", [])
        err = [m for m in msgs if m[0] in ("execution_error", "execution_interrupted")]
        print("[gen] 生成失败（execution_error）：", file=sys.stderr)
        print(json.dumps(err or msgs, ensure_ascii=False, indent=2)[:4000], file=sys.stderr)
        sys.exit(1)

    out_dir = (Path.cwd() / args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    saved = fetch_results(args.server, entry, out_dir, args.name)
    if not saved:
        sys.exit("[gen] 任务成功但没有任何输出图片")

    report, warnings = [], []
    for p in saved:
        info, w = verify_png(p, args.transparent, (args.width, args.height))
        report.append(info)
        warnings.extend(f"{p.name}: {x}" for x in w)
        if args.meta:
            meta = {"prompt": final_prompt, "negative": args.negative,
                    "width": args.width, "height": args.height,
                    "steps": args.steps, "cfg": args.cfg, "sampler": args.sampler,
                    "scheduler": args.scheduler, "seed": args.seed,
                    "variants": args.variants, "refs": [str(r) for r in refs],
                    "transparent": args.transparent,
                    "comfyui_prompt_id": prompt_id,
                    "comfyui_outputs": [im.get("filename") for node_out in
                                        entry.get("outputs", {}).values()
                                        for im in node_out.get("images", [])]}
            p.with_suffix(".meta.json").write_text(
                json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")

    esec = exec_seconds(entry)
    if args.json:
        print(json.dumps({"ok": True, "files": [str(p) for p in saved],
                          "seed": args.seed, "wall_seconds": round(wall, 1),
                          "exec_seconds": esec and round(esec, 1),
                          "info": report, "warnings": warnings},
                         ensure_ascii=False))
    else:
        print(f"\n✅ 完成 {len(saved)} 张（{int(wall)}s"
              + (f"，其中采样 {int(esec)}s" if esec else "") + "）")
        for idx, p in enumerate(saved):
            i = report[idx]
            extra = ""
            if i.get("has_alpha"):
                extra = f" RGBA 透明 {i.get('transparent_pct')}%"
            print(f"  {p}  {i.get('size', '?')} {i.get('mode', '')}{extra}")
        for w in warnings:
            print(f"  ⚠️  {w}", file=sys.stderr)
        print(f"  seed={args.seed} prompt_id={prompt_id}"
              f"（ComfyUI 副本在 output/skillgen/）")
        print("  下一步：用 read 工具查看图片，不符预期就改 prompt/seed 重跑。")


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass
    main()
