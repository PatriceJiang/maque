#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把精灵表 / 图集 PNG 切成单帧文件（游戏素材后处理）。需要 Pillow。

用法:
  python3 scripts/slice.py sheet.png --cols 4 --rows 2 --trim
  python3 scripts/slice.py sheet.png --cell 256x256 --pad 2
  python3 scripts/slice.py sheet.png --auto          # 按透明间隙自动找格子（尽力而为）

输出: <sheet>_frames/ 目录下的 <prefix>_00.png ...（行优先顺序）。
"""

import argparse
import json
import sys
from pathlib import Path

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("[slice] 需要 Pillow：pip install pillow（或用 ComfyUI 自带 venv 的 python）")


def alpha_extrema_ok(im):
    a = im.getchannel("A")
    return a.getextrema()[0] < 250


def make_mask(im, tolerance):
    """返回 bool 意义的 L 掩码：255=有内容。优先用真 alpha，否则与左上角背景色做差。"""
    has_real_alpha = False
    if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
        has_real_alpha = alpha_extrema_ok(im.convert("RGBA"))
    if has_real_alpha:
        src = im.convert("RGBA").getchannel("A")
    else:
        rgb = im.convert("RGB")
        bg = rgb.getpixel((0, 0))
        src = ImageChops.difference(rgb, Image.new("RGB", rgb.size, bg)).convert("L")
    return src.point(lambda v: 255 if v > tolerance else 0)


def axis_runs(mask, axis, min_gap):
    """沿 axis 找"有内容"的连续区段（含首尾）。min_gap: 小于此宽度的空档忽略（抗噪）。"""
    w, h = mask.size
    n = w if axis == "x" else h
    filled = []
    for i in range(n):
        box = (i, 0, i + 1, h) if axis == "x" else (0, i, w, i + 1)
        filled.append(mask.crop(box).getbbox() is not None)
    runs = []
    start = None
    for i, f in enumerate(filled):
        if f and start is None:
            start = i
        elif not f and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, n))
    # 合并间隔很小的区段（噪点抗性）
    merged = []
    for a, b in runs:
        if merged and a - merged[-1][1] < min_gap:
            merged[-1] = (merged[-1][0], b)
        else:
            merged.append((a, b))
    return merged


def extract(im, mask, box, pad, do_trim):
    """按 box 裁一帧；do_trim 时按内容紧贴裁剪（外扩 pad）。无内容返回 None。"""
    x0, y0, x1, y1 = box
    cell = mask.crop(box)
    mb = cell.getbbox()
    if mb is None:
        return None
    if do_trim:
        bx0, by0, bx1, by1 = mb
        bx0, by0 = max(0, bx0 - pad), max(0, by0 - pad)
        bx1, by1 = min(x1 - x0, bx1 + pad), min(y1 - y0, by1 + pad)
        return im.crop((x0 + bx0, y0 + by0, x0 + bx1, y0 + by1))
    return im.crop(box)


def parse_cell(spec):
    try:
        w, h = spec.lower().split("x")
        return int(w), int(h)
    except Exception:
        sys.exit(f"[slice] --cell 格式应为 WxH，例如 256x256（收到 {spec!r}）")


def main():
    ap = argparse.ArgumentParser(description="精灵表切帧（游戏素材后处理）")
    ap.add_argument("sheet", help="精灵表 PNG")
    ap.add_argument("--cols", type=int, help="列数（与 --cell 二选一）")
    ap.add_argument("--rows", type=int, help="行数（与 --cell 二选一）")
    ap.add_argument("--cell", help="格子像素尺寸 WxH（与 --cols/--rows 二选一）")
    ap.add_argument("--auto", action="store_true",
                    help="自动按透明/背景间隙找格子（格间必须有间隙，否则用 --cols/--rows）")
    ap.add_argument("--trim", action="store_true", help="按每格内容紧贴裁剪（去透明边）")
    ap.add_argument("--pad", type=int, default=0, help="trim 时向外留的像素边距")
    ap.add_argument("--tolerance", type=int, default=16,
                    help="alpha/背景色差阈值；默认 16，背景不干净时调大")
    ap.add_argument("--out", help="输出目录（默认 <sheet>_frames/）")
    ap.add_argument("--prefix", help="帧文件名前缀（默认取 sheet 文件名）")
    ap.add_argument("--json", action="store_true", help="只输出一行 JSON 清单")
    args = ap.parse_args()

    sheet = Path(args.sheet).expanduser()
    if not sheet.exists():
        sys.exit(f"[slice] 找不到 {sheet}")
    orig = Image.open(sheet)
    im = orig.convert("RGBA")
    mask = make_mask(orig, args.tolerance)
    W, H = im.size

    auto = args.auto or not (args.cols or args.rows or args.cell)
    if auto and (args.cols or args.rows or args.cell):
        sys.exit("[slice] --auto 与 --cols/--rows/--cell 不能同时用")
    if args.cell and (args.cols or args.rows):
        sys.exit("[slice] --cell 与 --cols/--rows 不能同时用")

    if auto:
        xruns = axis_runs(mask, "x", min_gap=2)
        yruns = axis_runs(mask, "y", min_gap=2)
        if len(xruns) * len(yruns) <= 1:
            sys.exit("[slice] 自动切分只找到 1 块内容 —— 可能是格间没有透明间隙。"
                     "请用 --cols/--rows 或 --cell 手动指定网格")
        boxes = [(x0, y0, x1, y1) for (y0, y1) in yruns for (x0, x1) in xruns]
        grid = (len(yruns), len(xruns))
        do_trim = True
    else:
        if args.cell:
            cw, ch = parse_cell(args.cell)
            cols, rows = W // cw, H // ch
        else:
            cols, rows = args.cols or 1, args.rows or 1
            cw, ch = W // cols, H // rows
        if cw < 2 or ch < 2 or cols < 1 or rows < 1:
            sys.exit(f"[slice] 网格不合理：{cols}x{rows} @ {cw}x{ch}（图 {W}x{H}）")
        boxes = [(c * cw, r * ch, (c + 1) * cw, (r + 1) * ch)
                 for r in range(rows) for c in range(cols)]
        grid = (rows, cols)
        do_trim = args.trim

    out_dir = Path(args.out).expanduser() if args.out else sheet.parent / (sheet.stem + "_frames")
    out_dir.mkdir(parents=True, exist_ok=True)
    prefix = args.prefix or sheet.stem

    files, skipped = [], 0
    for box in boxes:
        frame = extract(im, mask, box, args.pad, do_trim)
        if frame is None:
            skipped += 1
            continue
        dest = out_dir / f"{prefix}_{len(files):02d}.png"
        frame.save(dest)
        files.append({"file": str(dest), "size": list(frame.size),
                      "source_box": list(box)})

    if not files:
        sys.exit("[slice] 没切出任何帧（整张图都近似空白？）")

    if args.json:
        print(json.dumps({"ok": True, "grid": list(grid), "count": len(files),
                          "skipped_empty": skipped, "files": files},
                         ensure_ascii=False))
    else:
        print(f"✅ 切出 {len(files)} 帧 → {out_dir}/  "
              f"（网格 {grid[0]}x{grid[1]}"
              + (f"，跳过 {skipped} 个空格" if skipped else "") + "）")
        sizes = {tuple(f["size"]) for f in files}
        if len(sizes) > 1:
            print(f"  注：帧尺寸不完全一致 {sorted(sizes)}；做逐帧动画时建议配合 --trim 或统一 --pad")


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass
    main()
