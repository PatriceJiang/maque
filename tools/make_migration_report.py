#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 Khronos 模型迁移对比报告（reports/khronos-comparison/）。

对 `scenes/khronos/manifest.json` 里每个已迁移模型 + 手工 ToyCar：
  · 官方参考图（**large 优先**：screenshot_large / screenshot-Large / large.jpg …，
    否则 screenshot.*；GIF 取第一帧）→ 下采样到 ≤900px 存 `ref/<Model>.jpg`（透明背景合成中性灰）；
  · 本引擎迁移预览 `scenes/khronos/<Model>/preview.jpg`（已入库）；
  · 生成 `index.html`（逐模型左右对比 + 指标 + 自动/人工结论 + 差距/链接）、`comparison.csv/json`、
    `README.md`（摘要 + 低相似度清单 + 复现命令） 与 `overview.jpg`（代表模型拼图）。

用法（仓库根目录）：
  python tools/make_migration_report.py
  python tools/make_migration_report.py --max-dim 1200 --quality 85
"""
from __future__ import annotations

import argparse
import csv
import html
import json
import os
import re
import sys
import urllib.parse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import migrate_gltf_models as drv  # noqa: E402  （复用其中的参考图选择/对比逻辑）

DEFAULT_MODELS_ROOT = os.path.join(REPO, ".deps", "gltf-assets", "glTF-Sample-Assets-main", "Models")
DEFAULT_MANIFEST = os.path.join(REPO, "scenes", "khronos", "manifest.json")
OUT_ROOT = os.path.join(REPO, "reports", "khronos-comparison")
TOYCAR_REF = os.path.join(REPO, "build-win", "ref", "toycar_ref.jpg")


def safe_name(model):
    s = re.sub(r"[^A-Za-z0-9._-]+", "_", model).strip("_")
    return s or "model"


def esc(x):
    return html.escape(str(x), quote=True)


def load_readme_review(model):
    """从模型 README 的"人工视觉复核"小节提取结论文本（没有就返回空）。"""
    p = os.path.join(REPO, "scenes", "khronos", model, "README.md")
    if not os.path.isfile(p):
        return ""
    try:
        text = open(p, encoding="utf-8").read()
    except OSError:
        return ""
    idx = text.find("## 人工视觉复核")
    if idx < 0:
        return ""
    seg = text[idx:]
    nxt = seg.find("\n## ", 5)
    if nxt > 0:
        seg = seg[:nxt]
    seg = re.sub(r"^## 人工视觉复核[^\n]*\n", "", seg)
    seg = re.sub(r"<!--.*?-->", "", seg, flags=re.S).strip()
    return re.sub(r"\n{2,}", "\n", seg)[:1200]


def export_ref_image(model, model_dir, ref_path, out_dir, max_dim, quality):
    """参考图 → 下采样 JPEG（透明合成中性灰）。返回 (相对路径, 原尺寸/格式, 是否 gif, 错误)。"""
    from PIL import Image

    if not ref_path or not os.path.isfile(ref_path):
        return None, None, False, "无参考图"
    try:
        im = Image.open(ref_path)
        fmt = im.format or "?"
        has_alpha = im.mode in ("RGBA", "LA", "P") and "transparency" in im.info or im.mode in ("RGBA", "LA")
        if getattr(im, "is_animated", False):
            im.seek(0)
        im = im.convert("RGBA")
        if has_alpha:
            bg = Image.new("RGBA", im.size, (138, 138, 138, 255))
            im = Image.alpha_composite(bg, im)
        im = im.convert("RGB")
        orig_size = im.size
        im.thumbnail((max_dim, max_dim), Image.LANCZOS)
        name = safe_name(model) + ".jpg"
        dest = os.path.join(out_dir, name)
        im.save(dest, quality=quality, optimize=True)
        rel = "ref/" + name
        is_gif = os.path.splitext(ref_path)[1].lower() == ".gif"
        return rel, orig_size, is_gif, None
    except Exception as e:  # noqa: BLE001
        return None, None, False, str(e)


def verdict_class(v):
    if v.startswith("主体亮度"):
        return "good"
    if v.startswith("中等"):
        return "mid"
    return "bad"


def main():
    ap = argparse.ArgumentParser(description="生成 Khronos 模型迁移对比报告")
    ap.add_argument("--max-dim", type=int, default=900, help="参考图下采样最大边（默认 900）")
    ap.add_argument("--quality", type=int, default=82, help="参考图 JPEG 质量（默认 82）")
    args = ap.parse_args()

    manifest = json.load(open(DEFAULT_MANIFEST, encoding="utf-8"))
    models_root = os.path.join(REPO, manifest.get("models_root", ""))
    if not os.path.isdir(models_root):
        models_root = DEFAULT_MODELS_ROOT
    out_ref = os.path.join(OUT_ROOT, "ref")
    os.makedirs(out_ref, exist_ok=True)

    recs = [r for r in manifest["records"] if r.get("model")]
    recs.sort(key=lambda r: r["model"])
    rows = []
    for rec in recs:
        model = rec["model"]
        model_dir = os.path.join(models_root, model)
        ref_path = drv.find_reference_image(model_dir)
        rel, orig_size, is_gif, err = export_ref_image(
            model, model_dir, ref_path, out_ref, args.max_dim, args.quality)
        cmp_ = rec.get("compare") or {}
        preview = os.path.join(REPO, rec.get("preview_jpg")
                               or os.path.join("scenes", "khronos", model, "preview.jpg"))
        rows.append({
            "model": model,
            "status": rec.get("status", "?"),
            "tags": rec.get("tags") or [],
            "ref": rel or "",
            "ref_file": os.path.basename(ref_path) if ref_path else "",
            "ref_size": list(orig_size) if orig_size else None,
            "ref_is_gif": is_gif,
            "ref_large": "large" in (os.path.basename(ref_path).lower() if ref_path else ""),
            "ref_error": err,
            "preview": os.path.relpath(preview, REPO).replace("\\", "/") if os.path.isfile(preview) else "",
            "hist": cmp_.get("hist_similarity"),
            "lum": cmp_.get("mean_lum_delta"),
            "rgb": cmp_.get("rgb_delta"),
            "coverage": cmp_.get("preview_coverage"),
            "coverage_ratio": cmp_.get("coverage_ratio"),
            "verdict": cmp_.get("verdict") or "（无自动对比）",
            "framing_notes": cmp_.get("framing_notes") or [],
            "gaps": rec.get("gaps") or [],
            "stats": rec.get("stats") or {},
            "review": load_readme_review(model),
            "readme": f"../../scenes/khronos/{urllib.parse.quote(model)}/README.md",
            "scene": f"../../scenes/khronos/{urllib.parse.quote(model)}/{urllib.parse.quote(model)}.scn",
        })

    # 手工调校的 ToyCar 也纳入（不在 manifest；参考图在 build-win/ref，指标即时算）
    tc_model = "ToyCar"
    tc_dir = os.path.join(REPO, "scenes", "khronos", tc_model)
    tc_preview = os.path.join(tc_dir, "preview.jpg")
    if os.path.isfile(tc_preview):
        rel, orig_size, is_gif, err = export_ref_image(
            tc_model, tc_dir, TOYCAR_REF, out_ref, args.max_dim, args.quality)
        cmp_ = None
        if rel and os.path.isfile(TOYCAR_REF):
            try:
                cmp_ = drv.compare_images(tc_preview, TOYCAR_REF, os.path.join(REPO, "build-win", "audit", "toycar-report-compare.json"))
            except Exception:  # noqa: BLE001
                cmp_ = None
        cmp_ = cmp_ or {}
        rows.append({
            "model": tc_model, "status": "ok (hand-tuned)", "tags": ["hand-tuned", "showcase"],
            "ref": rel or "", "ref_file": os.path.basename(TOYCAR_REF), "ref_size": list(orig_size) if orig_size else None,
            "ref_is_gif": is_gif, "ref_large": False, "ref_error": err,
            "preview": os.path.relpath(tc_preview, REPO).replace("\\", "/"),
            "hist": cmp_.get("hist_similarity"), "lum": cmp_.get("mean_lum_delta"),
            "rgb": cmp_.get("rgb_delta"), "coverage": cmp_.get("preview_coverage"),
            "coverage_ratio": cmp_.get("coverage_ratio"),
            "verdict": cmp_.get("verdict") or "（无自动对比）",
            "framing_notes": cmp_.get("framing_notes") or [],
            "gaps": [], "stats": {}, "review": load_readme_review(tc_model),
            "readme": f"../../scenes/khronos/{tc_model}/README.md",
            "scene": f"../../scenes/khronos/{tc_model}/{tc_model}.scn",
        })
    rows.sort(key=lambda r: r["model"].lower())

    # ---- 统计 ----
    have_cmp = [r for r in rows if r["hist"] is not None]
    verdicts = {"good": 0, "mid": 0, "bad": 0}
    for r in have_cmp:
        verdicts[verdict_class(r["verdict"])] += 1
    import statistics
    med = lambda k: round(statistics.median([r[k] for r in have_cmp if r[k] is not None]), 4)  # noqa: E731
    stats = {
        "models": len(rows),
        "with_compare": len(have_cmp),
        "large_refs": sum(1 for r in rows if r["ref_large"]),
        "gif_refs": sum(1 for r in rows if r["ref_is_gif"]),
        "median_hist": med("hist"), "median_lum": med("lum"), "median_rgb": med("rgb"),
        "median_coverage": med("coverage"),
        "verdicts": verdicts,
        "ref_missing": sum(1 for r in rows if not r["ref"]),
        "preview_missing": sum(1 for r in rows if not r["preview"]),
    }
    worst = sorted([r for r in have_cmp if r["hist"] is not None], key=lambda r: r["hist"])[:15]
    best = sorted([r for r in have_cmp if r["hist"] is not None], key=lambda r: -r["hist"])[:10]

    # ---- CSV / JSON ----
    csv_path = os.path.join(OUT_ROOT, "comparison.csv")
    with open(csv_path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["model", "status", "reference", "ref_is_large", "ref_is_gif", "ref_size",
                    "preview", "hist_similarity", "mean_lum_delta", "rgb_delta",
                    "preview_coverage", "coverage_ratio", "verdict", "gaps_count", "has_manual_review"])
        for r in rows:
            w.writerow([r["model"], r["status"], r["ref_file"],
                        "large" if r["ref_large"] else "", "gif" if r["ref_is_gif"] else "",
                        "x".join(str(x) for x in r["ref_size"]) if r["ref_size"] else "",
                        r["preview"], r["hist"], r["lum"], r["rgb"], r["coverage"],
                        r["coverage_ratio"], r["verdict"], len(r["gaps"]), "yes" if r["review"] else ""])
    json_path = os.path.join(OUT_ROOT, "comparison.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump({"stats": stats, "models": rows}, f, ensure_ascii=False, indent=1, sort_keys=True)

    # ---- HTML ----
    def fmt(v, nd=3):
        return "—" if v is None else (f"{v:.{nd}f}" if isinstance(v, float) else str(v))

    def card(r):
        cls = verdict_class(r["verdict"]) if r["hist"] is not None else "mid"
        tags = "".join(f"<span class=tag>{esc(t)}</span>" for t in r["tags"][:4])
        ref_html = (f'<a href="{esc(r["ref"])}" target=_blank><img loading=lazy src="{esc(r["ref"])}" alt="ref {esc(r["model"])}"></a>'
                    if r["ref"] else '<div class=missing>无参考图</div>')
        preview_url = urllib.parse.quote(r["preview"]) if r["preview"] else ""
        prev_html = (f'<a href="../../{esc(preview_url)}" target=_blank><img loading=lazy src="../../{esc(preview_url)}" alt="maque {esc(r["model"])}"></a>'
                     if r["preview"] else '<div class=missing>无预览</div>')
        ref_cap = esc(r["ref_file"] or "—")
        if r["ref_size"]:
            ref_cap += f'（{"×".join(str(x) for x in r["ref_size"])}{"，large" if r["ref_large"] else ""}{"，GIF 首帧" if r["ref_is_gif"] else ""}）'
        gaps = r["gaps"]
        gap_html = ""
        if gaps:
            items = "".join(f"<li>{esc(g)}</li>" for g in gaps[:6])
            more = f"<li>… 共 {len(gaps)} 项，见该模型 README</li>" if len(gaps) > 6 else ""
            gap_html = f"<details><summary>已知差距 {len(gaps)} 项</summary><ul>{items}{more}</ul></details>"
        review = (f'<div class=review><b>人工视觉复核：</b>{esc(r["review"]).replace(chr(10), "<br>")}</div>'
                  if r["review"] else "")
        framing = ("".join(f"<span class=warn>{esc(x)}</span>" for x in r["framing_notes"]))
        metrics = (f'<span title="16 桶亮度直方图交集">直方图相似 {fmt(r["hist"])}</span>'
                   f'<span title="前景平均亮度差">亮度差 {fmt(r["lum"], 3)}</span>'
                   f'<span title="前景平均 RGB 距离">RGB 距离 {fmt(r["rgb"])}</span>'
                   f'<span title="预览前景像素占比">前景占比 {fmt((r["coverage"] or 0) * 100, 1)}%</span>'
                   if r["hist"] is not None else '<span>无自动指标</span>')
        return f'''
<article class="card {cls}" id="model-{esc(safe_name(r['model']))}" data-name="{esc(r['model'].lower())}"
         data-verdict="{cls}" data-hist="{fmt(r['hist'], 4)}" data-gaps="{len(gaps)}">
  <header class="card-head">
    <h2>{esc(r["model"])}</h2>
    <div class="meta">{tags}<span class="status">{esc(r["status"])}</span>
      <a href="{esc(r["readme"])}">笔记</a><a href="{esc(r["scene"])}">场景</a></div>
  </header>
  <div class="pair">
    <figure><div class="imgbox">{ref_html}</div><figcaption>官方参考：{ref_cap}</figcaption></figure>
    <figure><div class="imgbox">{prev_html}</div><figcaption>maque 迁移预览：preview.jpg（800×450）</figcaption></figure>
  </div>
  <div class="metrics">{metrics}{framing}</div>
  <div class="verdict"><b>自动初判：</b>{esc(r["verdict"])}</div>
  {review}{gap_html}
</article>'''

    cards = "\n".join(card(r) for r in rows)
    index_rows = "\n".join(
        f'<tr data-name="{esc(r["model"].lower())}" data-verdict="{verdict_class(r["verdict"]) if r["hist"] is not None else "mid"}">'
        f'<td><a href="#model-{esc(safe_name(r["model"]))}">{esc(r["model"])}</a></td>'
        f'<td>{"large" if r["ref_large"] else ("gif" if r["ref_is_gif"] else "screenshot")}</td>'
        f'<td>{esc(r["ref_file"])}</td>'
        f'<td>{fmt(r["hist"])}</td><td>{fmt(r["lum"], 3)}</td><td>{fmt((r["coverage"] or 0) * 100, 1)}%</td>'
        f'<td class="v-{verdict_class(r["verdict"]) if r["hist"] is not None else "mid"}">{esc(r["verdict"])}</td>'
        f'<td>{len(r["gaps"])}</td></tr>'
        for r in rows)

    html_doc = f'''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Khronos glTF-Sample-Assets × maque 迁移对比报告</title>
<style>
:root {{ --bg:#16181d; --card:#1e2128; --fg:#e6e8ee; --dim:#9aa3b2; --line:#2c313c;
         --good:#3fb950; --mid:#d29922; --bad:#f85149; --accent:#58a6ff; }}
* {{ box-sizing:border-box; }}
body {{ margin:0; background:var(--bg); color:var(--fg);
       font:14px/1.5 "Segoe UI", "Microsoft YaHei", system-ui, sans-serif; }}
header.top {{ padding:24px 32px; border-bottom:1px solid var(--line); background:#1a1d24; }}
header.top h1 {{ margin:0 0 8px; font-size:24px; }}
header.top p {{ margin:4px 0; color:var(--dim); max-width:1100px; }}
.stats {{ display:flex; gap:12px; flex-wrap:wrap; margin:14px 0 4px; }}
.stat {{ background:var(--card); border:1px solid var(--line); border-radius:8px; padding:8px 14px; }}
.stat b {{ font-size:20px; display:block; }}
.stat span {{ color:var(--dim); font-size:12px; }}
.controls {{ position:sticky; top:0; z-index:5; background:#1a1d24ee; backdrop-filter:blur(6px);
            padding:10px 32px; border-bottom:1px solid var(--line); display:flex; gap:12px; flex-wrap:wrap; }}
.controls input, .controls select {{ background:var(--card); color:var(--fg); border:1px solid var(--line);
            border-radius:6px; padding:6px 10px; }}
main {{ padding:20px 32px 60px; }}
table.idx {{ border-collapse:collapse; width:100%; font-size:12.5px; }}
table.idx th, table.idx td {{ border-bottom:1px solid var(--line); padding:5px 8px; text-align:left; }}
table.idx th {{ color:var(--dim); position:sticky; top:58px; background:var(--bg); }}
.v-good {{ color:var(--good); }} .v-mid {{ color:var(--mid); }} .v-bad {{ color:var(--bad); }}
.cards {{ display:grid; grid-template-columns:repeat(auto-fill, minmax(760px, 1fr)); gap:18px; margin-top:18px; }}
.card {{ background:var(--card); border:1px solid var(--line); border-radius:10px; padding:14px 16px; }}
.card.bad {{ border-left:4px solid var(--bad); }} .card.mid {{ border-left:4px solid var(--mid); }}
.card.good {{ border-left:4px solid var(--good); }}
.card-head {{ display:flex; justify-content:space-between; align-items:baseline; gap:10px; flex-wrap:wrap; }}
.card-head h2 {{ margin:0; font-size:18px; }}
.meta {{ color:var(--dim); font-size:12.5px; display:flex; gap:8px; align-items:center; flex-wrap:wrap; }}
.meta a {{ color:var(--accent); text-decoration:none; }}
.tag {{ border:1px solid var(--line); border-radius:10px; padding:0 7px; font-size:11.5px; color:var(--dim); }}
.status {{ color:var(--dim); }}
.pair {{ display:grid; grid-template-columns:1fr 1fr; gap:12px; margin:10px 0 6px; }}
figure {{ margin:0; }}
.imgbox {{ background:#111318 url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='16' height='16'%3E%3Crect width='8' height='8' fill='%23181b21'/%3E%3Crect x='8' y='8' width='8' height='8' fill='%23181b21'/%3E%3C/svg%3E");
          border:1px solid var(--line); border-radius:6px; height:260px; display:flex; align-items:center; justify-content:center; overflow:hidden; }}
.imgbox img {{ max-width:100%; max-height:100%; object-fit:contain; }}
figcaption {{ color:var(--dim); font-size:12px; margin-top:4px; }}
.metrics {{ display:flex; gap:8px; flex-wrap:wrap; font-size:12.5px; color:var(--dim); margin:8px 0; }}
.metrics span {{ background:#262b34; border-radius:6px; padding:2px 8px; }}
.warn {{ color:var(--bad); }}
.verdict {{ font-size:13px; }}
.review {{ margin-top:8px; padding:8px 10px; background:#242a33; border-left:3px solid var(--accent);
           border-radius:6px; font-size:12.5px; color:#c9d1d9; }}
details {{ margin-top:8px; color:var(--dim); font-size:12.5px; }}
details ul {{ margin:6px 0 0 18px; padding:0; }}
.missing {{ color:var(--dim); }}
footer {{ color:var(--dim); font-size:12px; padding:0 32px 40px; }}
@media (max-width:900px) {{ .cards {{ grid-template-columns:1fr; }} .pair {{ grid-template-columns:1fr; }} }}
</style></head><body>
<header class="top">
  <h1>Khronos glTF-Sample-Assets × maque 迁移对比报告</h1>
  <p>逐模型对比：<b>官方参考图（large 优先；GIF 取首帧）</b> vs <b>maque 引擎渲染的迁移预览</b>。
     参考图由上游 <code>Models/&lt;Model&gt;/screenshot/</code> 下采样到 ≤{args.max_dim}px 后收录在 <code>ref/</code>；
     预览来自仓库 <code>scenes/khronos/&lt;Model&gt;/preview.jpg</code>。</p>
  <p>⚠️ 对比<b>不是像素级 pass/fail</b>：官方图多为离线渲染（HDRI studio、软阴影、不同机位/背景、部分含透明背景），
     maque 当前无 HDRI IBL / transmission / 软阴影 / 动画骨骼。指标只用于筛查；每个模型卡片附自动初判、
     已知差距与（部分模型）人工视觉复核结论。迁移方法/踩坑见
     <a href="../../docs/31-gltf-model-migration.md">docs/31-gltf-model-migration.md</a>。</p>
  <div class="stats">
    <div class="stat"><b>{stats["models"]}</b><span>模型（含手工 ToyCar）</span></div>
    <div class="stat"><b>{stats["with_compare"]}</b><span>有自动对比</span></div>
    <div class="stat"><b>{stats["large_refs"]}</b><span>参考图使用 large 版</span></div>
    <div class="stat"><b>{stats["gif_refs"]}</b><span>参考图为 GIF（取首帧）</span></div>
    <div class="stat"><b>{stats["median_hist"]:.2f}</b><span>直方图相似度中位数</span></div>
    <div class="stat"><b>{stats["median_coverage"]*100:.1f}%</b><span>预览前景占比中位数</span></div>
    <div class="stat"><b>{verdicts["good"]} / {verdicts["mid"]} / {verdicts["bad"]}</b>
      <span>自动初判（较接近 / 中等 / 差异较大）</span></div>
  </div>
</header>
<div class="controls">
  <input id="q" placeholder="搜索模型名 / 标签…" size="28">
  <select id="vf">
    <option value="">全部初判</option><option value="bad">差异较大</option>
    <option value="mid">中等差异</option><option value="good">较接近</option>
  </select>
  <select id="sort">
    <option value="name">按名称</option><option value="hist-asc">相似度低→高</option>
    <option value="hist-desc">相似度高→低</option><option value="gaps">已知差距多→少</option>
  </select>
  <span id="count" style="color:var(--dim);align-self:center"></span>
</div>
<main>
  <details><summary><b>全部模型索引表（点击跳到对比卡片）</b></summary>
  <table class="idx"><thead><tr><th>模型</th><th>参考图类型</th><th>参考文件</th><th>直方图相似度</th>
    <th>亮度差</th><th>前景占比</th><th>自动初判</th><th>差距数</th></tr></thead>
  <tbody>{index_rows}</tbody></table></details>
  <section class="cards" id="cards">
{cards}
  </section>
</main>
<footer>生成：<code>python tools/make_migration_report.py</code>；
  数据源 <code>scenes/khronos/manifest.json</code> + 各模型 <code>compare.json</code>；参考图 downscale ≤{args.max_dim}px / JPEG q{args.quality}。</footer>
<script>
const q = document.getElementById('q'), vf = document.getElementById('vf'),
      sort = document.getElementById('sort'), cards = [...document.querySelectorAll('#cards .card')],
      idxRows = [...document.querySelectorAll('table.idx tbody tr')], count = document.getElementById('count');
function apply() {{
  const kw = q.value.trim().toLowerCase(), v = vf.value;
  let shown = 0;
  cards.forEach(c => {{
    const hit = (!kw || c.dataset.name.includes(kw)) && (!v || c.dataset.verdict === v);
    c.style.display = hit ? '' : 'none'; if (hit) shown++;
  }});
  idxRows.forEach(tr => {{
    const hit = (!kw || tr.dataset.name.includes(kw)) && (!v || tr.dataset.verdict === v);
    tr.style.display = hit ? '' : 'none';
  }});
  count.textContent = `显示 ${{shown}} / ${{cards.length}}`;
}}
function doSort() {{
  const mode = sort.value, parent = document.getElementById('cards');
  const arr = cards.slice();
  arr.sort((a, b) => {{
    if (mode === 'name') return a.dataset.name.localeCompare(b.dataset.name);
    if (mode === 'hist-asc') return (parseFloat(a.dataset.hist) || 9) - (parseFloat(b.dataset.hist) || 9);
    if (mode === 'hist-desc') return (parseFloat(b.dataset.hist) || -1) - (parseFloat(a.dataset.hist) || -1);
    if (mode === 'gaps') return (parseInt(b.dataset.gaps) || 0) - (parseInt(a.dataset.gaps) || 0);
    return 0;
  }});
  arr.forEach(c => parent.appendChild(c));
}}
q.addEventListener('input', apply); vf.addEventListener('change', apply);
sort.addEventListener('change', doSort); apply();
</script></body></html>'''
    html_path = os.path.join(OUT_ROOT, "index.html")
    with open(html_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(html_doc)

    # ---- 拼图总览（代表模型：最需关注 + 最接近 + 手工样板）----
    overview = os.path.join(OUT_ROOT, "overview.jpg")
    try:
        from PIL import Image, ImageDraw, ImageFont
        picks = []
        seen = set()

        def add(r):
            if r and r["model"] not in seen:
                seen.add(r["model"])
                picks.append(r)

        interesting = [r for r in have_cmp
                       if (r["stats"].get("vertices", 0) or 0) >= 1000 or "showcase" in (r["tags"] or [])]
        interesting.sort(key=lambda r: r["hist"] if r["hist"] is not None else 9)
        for r in interesting[:6]:
            add(r)
        for r in interesting[-6:]:
            add(r)
        for name in ("ToyCar", "Sponza", "FlightHelmet", "DamagedHelmet", "ABeautifulGame",
                     "TextureSettingsTest", "CompareClearcoat", "MetalRoughSpheres",
                     "CesiumMilkTruck", "SheenWoodLeatherSofa", "GLTFLogoWithSpaces", "Box With Spaces"):
            for r in rows:
                if r["model"] == name:
                    add(r)
        th_w, th_h = 300, 200
        cols = 3
        pad, cap = 10, 22
        rows_n = (len(picks) + cols - 1) // cols
        cell_w = th_w * 2 + pad * 3
        cell_h = th_h * 2 + pad * 2 + cap * 2 + 18
        sheet = Image.new("RGB", (cell_w * cols, cell_h * rows_n), (22, 24, 29))
        draw = ImageDraw.Draw(sheet)
        try:
            font = ImageFont.truetype(r"C:\Windows\Fonts\msyh.ttc", 15)
        except Exception:  # noqa: BLE001
            font = ImageFont.load_default()

        def thumb(path, box):
            try:
                im = Image.open(path).convert("RGB")
                im.thumbnail(box, Image.LANCZOS)
                return im
            except Exception:  # noqa: BLE001
                return None
        for i, r in enumerate(picks):
            cx, cy = (i % cols) * cell_w, (i // cols) * cell_h
            ref_im = thumb(os.path.join(OUT_ROOT, r["ref"]), (th_w, th_h)) if r["ref"] else None
            prev_im = thumb(os.path.join(REPO, r["preview"]), (th_w, th_h)) if r["preview"] else None
            x0, y0 = cx + pad, cy + cap
            if ref_im:
                sheet.paste(ref_im, (x0 + (th_w - ref_im.width) // 2, y0 + (th_h - ref_im.height) // 2))
            if prev_im:
                sheet.paste(prev_im, (x0 + th_w + pad + (th_w - prev_im.width) // 2,
                                      y0 + (th_h - prev_im.height) // 2))
            label = f'{r["model"]}   official(large优先) | maque'
            draw.text((cx + pad, cy + 4), label, fill=(210, 215, 225), font=font)
            draw.text((cx + pad, cy + cap + th_h + 4), "官方参考", fill=(150, 160, 175), font=font)
            draw.text((cx + pad + th_w + pad, cy + cap + th_h + 4), "maque 迁移预览", fill=(150, 160, 175), font=font)
        sheet.save(overview, quality=82, optimize=True)
    except Exception as e:  # noqa: BLE001
        print("overview 生成失败：", e, file=sys.stderr)
        overview = None

    # ---- README.md（摘要）----
    def md_row(r):
        return (f'| [{r["model"]}](#model-{safe_name(r["model"])}) '
                f'| {esc(r["ref_file"])}{"（large）" if r["ref_large"] else ""}{"（GIF）" if r["ref_is_gif"] else ""} '
                f'| {fmt(r["hist"])} | {fmt(r["lum"], 3)} | {fmt((r["coverage"] or 0) * 100, 1)}% '
                f'| {esc(r["verdict"])} | {len(r["gaps"])} |')

    md = f'''# Khronos glTF-Sample-Assets × maque 迁移对比报告

> 逐模型对比官方参考图（**large 优先**）与 maque 迁移后的渲染预览。
> 打开 [index.html](index.html) 查看左右对比（支持搜索/筛选/排序）。

- 模型：**{stats["models"]}**（{stats["with_compare"]} 个有自动对比；{stats["large_refs"]} 个参考图使用 large 版，{stats["gif_refs"]} 个为 GIF 首帧）
- 指标中位数：直方图相似度 **{stats["median_hist"]:.3f}**、前景平均亮度差 **{stats["median_lum"]:+.3f}**、
  前景 RGB 距离 **{stats["median_rgb"]:.3f}**、预览前景占比 **{stats["median_coverage"]*100:.1f}%**
- 自动初判：较接近 **{verdicts["good"]}** / 中等 **{verdicts["mid"]}** / 差异较大 **{verdicts["bad"]}**
- 参考图下采样：≤{args.max_dim}px、JPEG q{args.quality}，存于 `ref/`；预览来自 `scenes/khronos/<Model>/preview.jpg`
- 复现：`python tools/make_migration_report.py`；表格数据：[comparison.csv](comparison.csv)、[comparison.json](comparison.json)

![总览拼图](overview.jpg)

> ⚠️ **不是像素级 pass/fail**：官方图多为离线渲染（HDRI studio、软阴影、不同机位/背景、含透明背景），
> maque 无 HDRI IBL / transmission / 软阴影 / 动画骨骼。指标只用于筛查，差异需人工归因
> （见各模型 README 的「与官方 screenshot 对比」与「人工视觉复核」；迁移方法与踩坑见
> [docs/31-gltf-model-migration.md](../../docs/31-gltf-model-migration.md)）。

## 最需人工关注的 15 个模型（直方图相似度最低）

| 模型 | 参考图 | 直方图相似度 | 亮度差 | 前景占比 | 自动初判 | 差距数 |
|---|---|---|---|---|---|---|
{chr(10).join(md_row(r) for r in worst)}

## 最接近的 10 个模型

| 模型 | 参考图 | 直方图相似度 | 亮度差 | 前景占比 | 自动初判 | 差距数 |
|---|---|---|---|---|---|---|
{chr(10).join(md_row(r) for r in best)}

## 全部模型

完整左右对比见 [index.html](index.html)；也可用 CSV 自行分析。
'''
    md_path = os.path.join(OUT_ROOT, "README.md")
    with open(md_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(md)
    top_readme = os.path.join(REPO, "reports", "README.md")
    with open(top_readme, "w", encoding="utf-8", newline="\n") as f:
        f.write("# reports\n\n- [khronos-comparison/README.md](khronos-comparison/README.md) —— "
                "Khronos glTF-Sample-Assets 149 个模型的官方参考图（large 优先）vs maque 迁移预览逐模型对比报告；"
                "打开 [khronos-comparison/index.html](khronos-comparison/index.html) 浏览。\n")

    total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _dn, fn in os.walk(OUT_ROOT) for f in fn)
    print("report:", os.path.relpath(html_path, REPO))
    print("  models=%d compare=%d large=%d gif=%d ref_missing=%d preview_missing=%d"
          % (stats["models"], stats["with_compare"], stats["large_refs"], stats["gif_refs"],
             stats["ref_missing"], stats["preview_missing"]))
    print("  html=%.1f KB, report dir=%.1f MB, overview=%s"
          % (os.path.getsize(html_path) / 1024.0, total / 1048576.0,
             "ok" if overview else "skip"))


if __name__ == "__main__":
    main()
