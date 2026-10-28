#!/usr/bin/env python3
"""对比 Khronos 场景的两条加载路径：离线 .mesh.json（scenes/khronos）vs runtime 直读 .glb
（scenes/khronos-bin）。

每个场景单独起一个 maque_player 进程，`--frames 1` 后退出；记录：
  - wall_ms：整个进程墙钟（含引擎/GPU 初始化，两个变体相同）
  - load_ms：引擎日志里的模型加载时间（JSON 为 sum(runtime: mesh ...)，GLB 为
    runtime: gltf model ... (N ms)）

用法：
  python tools/bench_khronos_load.py [--only ABeautifulGame,Box] [--repeat 2]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MESH_RE = re.compile(r"runtime: mesh '([^']*)' vertices=\d+ indices=\d+ \(([0-9.]+) ms\)")
GLTF_RE = re.compile(r"runtime: gltf model '([^']*)' .*? \(([0-9.]+) ms\)")


def run_scene(player, scene, timeout):
    env = dict(os.environ)
    env.update({
        "MAQUE_WINDOW": "0x0",
        "MAQUE_PRESENT": "off",
        "MAQUE_HEADLESS": "1",
        "MAQUE_RENDER3D_INFO": "1",
        "MAQUE_ANIM": "0",
        "MAQUE_MORPH": "0",
    })
    t0 = time.perf_counter()
    try:
        p = subprocess.run([player, "--frames", "1", scene], cwd=REPO, env=env,
                           capture_output=True, timeout=timeout)
        wall_ms = (time.perf_counter() - t0) * 1000.0
        text = (p.stdout + p.stderr).decode("utf-8", "replace")
        return {"rc": p.returncode, "wall_ms": wall_ms, "text": text}
    except subprocess.TimeoutExpired:
        return {"rc": -9, "wall_ms": (time.perf_counter() - t0) * 1000.0, "text": "TIMEOUT"}


def parse_load_ms(mode, text):
    if mode == "json":
        vals = [float(m.group(2)) for m in MESH_RE.finditer(text)]
    else:
        vals = [float(m.group(2)) for m in GLTF_RE.finditer(text)]
    return sum(vals) if vals else None


def bench_one(player, mode, scene, repeat, timeout):
    runs = []
    for _ in range(repeat):
        r = run_scene(player, scene, timeout)
        r["load_ms"] = parse_load_ms(mode, r["text"])
        runs.append(r)
    ok = [r for r in runs if r["rc"] == 0]
    walls = [r["wall_ms"] for r in ok]
    loads = [r["load_ms"] for r in ok if r["load_ms"] is not None]
    return {
        "mode": mode,
        "rc": runs[-1]["rc"],
        "wall_ms": round(statistics.median(walls), 1) if walls else None,
        "load_ms": round(statistics.median(loads), 1) if loads else None,
        "runs": [{"rc": r["rc"], "wall_ms": round(r["wall_ms"], 1),
                  "load_ms": None if r["load_ms"] is None else round(r["load_ms"], 1)}
                 for r in runs],
        "tail": runs[-1]["text"][-1500:] if runs[-1]["rc"] != 0 else "",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--player", default=os.path.join("build-win", "samples", "player", "maque_player.exe"))
    ap.add_argument("--json-dir", default=os.path.join("scenes", "khronos"))
    ap.add_argument("--bin-dir", default=os.path.join("scenes", "khronos-bin"))
    ap.add_argument("--only", default="")
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--out", default=os.path.join("maque-test-tmp", "khronos_load_bench.json"))
    args = ap.parse_args()
    player = os.path.join(REPO, args.player)
    json_root = os.path.join(REPO, args.json_dir)
    bin_root = os.path.join(REPO, args.bin_dir)
    models = sorted(d for d in os.listdir(json_root)
                    if os.path.isdir(os.path.join(json_root, d)) and d != "assets")
    if args.only:
        want = set(x for x in args.only.split(",") if x)
        models = [m for m in models if m in want]
    out = {"player": player, "repeat": args.repeat, "models": {}}
    started = time.time()
    total = len(models) * 2

    def job(m):
        res = {}
        for mode, root in (("json", json_root), ("bin", bin_root)):
            scene = os.path.join(root, m, m + ".scn")
            if not os.path.isfile(scene):
                res[mode] = {"rc": -1, "wall_ms": None, "load_ms": None, "runs": [],
                             "tail": "missing scene %s" % scene}
            else:
                res[mode] = bench_one(player, mode, scene, args.repeat, args.timeout)
        return m, res

    if args.jobs <= 1:
        it = map(job, models)
        for m, res in it:
            out["models"][m] = res
            print("%-28s json load=%8s wall=%8s | bin load=%8s wall=%8s" % (
                m, res["json"].get("load_ms"), res["json"].get("wall_ms"),
                res["bin"].get("load_ms"), res["bin"].get("wall_ms")), flush=True)
    else:
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = [ex.submit(job, m) for m in models]
            for fut in as_completed(futs):
                m, res = fut.result()
                out["models"][m] = res
                print("%-28s json load=%8s wall=%8s | bin load=%8s wall=%8s" % (
                    m, res["json"].get("load_ms"), res["json"].get("wall_ms"),
                    res["bin"].get("load_ms"), res["bin"].get("wall_ms")), flush=True)

    ok = {m: r for m, r in out["models"].items()
          if r["json"].get("load_ms") is not None and r["bin"].get("load_ms") is not None}
    rows = []
    for m, r in ok.items():
        j = r["json"]["load_ms"]
        b = r["bin"]["load_ms"]
        jw = r["json"].get("wall_ms")
        bw = r["bin"].get("wall_ms")
        rows.append((j / b if b and b > 0 else 0.0, m, j, b, jw, bw))
    rows.sort(reverse=True)
    both_wall = [x for x in rows if x[4] is not None and x[5] is not None]
    summary = {
        "models": len(out["models"]),
        "both_ok": len(rows),
        "json_load_ms_sum": round(sum(j for _, _, j, _ in [(r[0], r[1], r[2], r[3]) for r in rows]), 1),
        "bin_load_ms_sum": round(sum(b for _, _, _, b in [(r[0], r[1], r[2], r[3]) for r in rows]), 1),
        "json_load_ms_median": round(statistics.median([j for _, _, j, _ in [(r[0], r[1], r[2], r[3]) for r in rows]]), 1) if rows else None,
        "bin_load_ms_median": round(statistics.median([b for _, _, _, b in [(r[0], r[1], r[2], r[3]) for r in rows]]), 1) if rows else None,
        "speedup_load_x": round(sum(j for _, _, j, _ in [(r[0], r[1], r[2], r[3]) for r in rows]) / sum(b for _, _, _, b in [(r[0], r[1], r[2], r[3]) for r in rows]), 2)
        if rows and sum(b for _, _, _, b in [(r[0], r[1], r[2], r[3]) for r in rows]) > 0 else None,
        "json_wall_ms_sum": round(sum(x[4] for x in both_wall), 1) if both_wall else None,
        "bin_wall_ms_sum": round(sum(x[5] for x in both_wall), 1) if both_wall else None,
        "json_wall_ms_median": round(statistics.median([x[4] for x in both_wall]), 1) if both_wall else None,
        "bin_wall_ms_median": round(statistics.median([x[5] for x in both_wall]), 1) if both_wall else None,
        "speedup_wall_x": round(sum(x[4] for x in both_wall) / sum(x[5] for x in both_wall), 2)
        if both_wall and sum(x[5] for x in both_wall) > 0 else None,
        "elapsed_s": round(time.time() - started, 1),
    }
    out["summary"] = summary
    print("\n== summary ==")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print("\n top 15 JSON/GLB load-time ratios:")
    for ratio, m, j, b, jw, bw in rows[:15]:
        print("  %-28s %6.2fx  json=%8.1f ms  bin=%8.1f ms  | wall json=%9.1f bin=%9.1f ms" % (
            m, ratio, j, b, jw or 0.0, bw or 0.0))
    os.makedirs(os.path.dirname(os.path.join(REPO, args.out)) or ".", exist_ok=True)
    with open(os.path.join(REPO, args.out), "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    print("\nwrote %s" % os.path.join(REPO, args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
