#!/usr/bin/env python3
"""maque Linux perf profiling helper (docs/35).

Decision: this project uses **perf(1)**, not oprofile.  oprofile is in maintenance mode and
needs an extra kernel module/daemon; perf is in-tree, produces `perf.data`, and resolves
DWARF/frame-pointer stacks to source lines.

Typical agent loop:

    python3 tools/perf/maque_perf.py check
    python3 tools/perf/maque_perf.py record --out .maque/perf/base --frames 600 -- \\
        ./build/samples/player/maque_player scenes/pbr/cube.scn
    # ... edit code ...
    python3 tools/perf/maque_perf.py record --out .maque/perf/cand --frames 600 -- \\
        ./build/samples/player/maque_player scenes/pbr/cube.scn
    python3 tools/perf/maque_perf.py compare --baseline .maque/perf/base \\
        --candidate .maque/perf/cand --fail-on-regression
    ctest --test-dir build --output-on-failure

All subcommands are pure stdlib.  `parse` works offline (no perf needed) and is covered by
tests; `check`/`record` additionally need a working perf (perf_event_paranoid <= 1 or
CAP_PERFMON).  See docs/35 for permissions, troubleshooting and the full agent workflow.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TOP = 40
PERF_MARKER_RE = re.compile(r"\[[.kugj]\]")
STATS_RE = re.compile(
    r"\[stats\] frame=(\d+) fps=([0-9.]+) total=([0-9.]+)ms logic=([0-9.]+) "
    r"render=([0-9.]+) present=([0-9.]+)"
)
SRCLINE_RE = re.compile(r"^(.*):(\d+)$")


# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------
def eprint(*args, **kwargs):
    print(*args, file=sys.stderr, **kwargs)


def die(msg: str, code: int = 1):
    eprint(f"maque_perf: {msg}")
    raise SystemExit(code)


def json_dump(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def json_load(path: Path, default=None):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def tail(text: str, limit: int = 4000) -> str:
    text = text.strip()
    return text[-limit:] if len(text) > limit else text


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def find_perf(explicit: str | None) -> str | None:
    if explicit:
        return explicit if Path(explicit).exists() or shutil.which(explicit) else None
    env = os.environ.get("MAQUE_PROFILE_PERF")
    if env:
        return env
    return shutil.which("perf")


def perf_version(perf: str) -> str:
    try:
        r = subprocess.run([perf, "--version"], capture_output=True, text=True, timeout=10)
        return (r.stdout or r.stderr).strip().splitlines()[0] if (r.stdout or r.stderr) else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def read_sysctl(name: str):
    try:
        return int(Path(name).read_text().strip())
    except (OSError, ValueError):
        return None


def run_cmd(cmd, *, env=None, timeout=900, log_path: Path | None = None):
    """Run a command, capture stdout+stderr, return (rc, output)."""
    started = time.perf_counter()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        rc, out = proc.returncode, (proc.stdout or "") + (proc.stderr or "")
    except subprocess.TimeoutExpired as exc:
        out = ((exc.stdout or "") + (exc.stderr or "")) if isinstance(exc.stdout, str) else ""
        rc = 124
        out += f"\n[maque_perf] timeout after {timeout}s\n"
    except OSError as exc:
        rc = 127
        out = f"[maque_perf] cannot execute {cmd[0]}: {exc}\n"
    elapsed = time.perf_counter() - started
    if log_path is not None:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(out, encoding="utf-8")
    return rc, out, elapsed


def _rel(path: str, repo: Path) -> str:
    p = path.replace("\\", "/")
    if not p:
        return ""
    if not os.path.isabs(p):
        return p.lstrip("./")
    try:
        return str(Path(p).resolve().relative_to(repo.resolve())).replace("\\", "/")
    except (OSError, ValueError):
        return p


def classify_module(source: str, dso: str, symbol: str, repo: Path) -> str:
    """Map a source path / DSO / symbol to a coarse engine module (for the report)."""
    rel = _rel(source, repo) if source else ""
    if rel.startswith("modules/"):
        parts = rel.split("/")
        if len(parts) >= 3 and parts[1] in {"core", "io", "data", "world", "render", "ui",
                                            "anim", "input", "script"}:
            if parts[2] not in {"src", "include", "tests"}:
                return "/".join(parts[:3])
        return "/".join(parts[:2])
    if rel.startswith("third_party/"):
        return "/".join(rel.split("/")[:2])
    if rel.startswith("samples/"):
        return "/".join(rel.split("/")[:2])
    if rel.startswith("tests/"):
        return "tests"

    # No srcline: fall back to namespace/DSO heuristics (still useful for ranking).
    if symbol.startswith("maque::profile"):
        return "modules/core/profile"
    if symbol.startswith("maque::tracy"):
        return "modules/core/tracy"
    if symbol.startswith("maque::rt") or symbol.startswith("maque::persist"):
        return "modules/runtime"
    if symbol.startswith("mq_control") or symbol.startswith("mq_obs"):
        return "modules/control" if symbol.startswith("mq_control") else "modules/observability"
    if "wgpu" in dso.lower() or "gfx" in dso.lower() or symbol.startswith("maque::gfx"):
        return "renderer"
    if "quickjs" in dso.lower() or "lua" in dso.lower() or symbol.startswith("js_"):
        return "script"
    if "maque_player" in dso or dso.endswith("maque_player"):
        return "maque_player"
    return dso or "unknown"


# ---------------------------------------------------------------------------
# parsers (shared by `report` and `parse`; unit-tested with fixtures)
# ---------------------------------------------------------------------------
def parse_report_text(text: str) -> list[dict]:
    """Parse `perf report --stdio --no-children --sort dso,symbol [--show-nr-samples]`.

    Returns [{percent, samples, dso, symbol}], aggregated by (dso, symbol).
    """
    agg: dict[tuple[str, str], dict] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] == "#" or not line[0].isdigit():
            continue
        pct_pos = line.find("%")
        if pct_pos < 0:
            continue
        try:
            pct = float(line[:pct_pos])
        except ValueError:
            continue
        m = PERF_MARKER_RE.search(line, pct_pos)
        if not m:
            continue
        symbol = line[m.end():].strip()
        if not symbol:
            continue
        left = line[:m.start()].strip()
        toks = left.split()
        if len(toks) < 2:
            continue
        dso = toks[-1]
        samples = 0
        if len(toks) >= 3 and toks[1].isdigit():
            samples = int(toks[1])
        key = (dso, symbol)
        cur = agg.setdefault(key, {"percent": 0.0, "samples": 0, "dso": dso, "symbol": symbol})
        cur["percent"] += pct
        cur["samples"] += samples
    return sorted(agg.values(), key=lambda h: (-h["percent"], -h["samples"], h["dso"], h["symbol"]))


def parse_script_text(text: str) -> tuple[dict[tuple[str, str, str], int], int]:
    """Parse `perf script -F ip,sym,dso[,srcline]` lines.

    Returns ({(dso, symbol, srcline): samples}, total_lines).
    """
    samples: dict[tuple[str, str, str], int] = {}
    total = 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] == "#":
            continue
        parts = line.split(None, 1)
        if len(parts) != 2 or not re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            continue
        rest = parts[1]
        pos = rest.rfind(" (")
        if pos < 0:
            continue
        symbol = rest[:pos].strip()
        dso_part = rest[pos + 2:]
        close = dso_part.find(")")
        if close < 0:
            continue
        dso = dso_part[:close].strip()
        srcline = dso_part[close + 1:].strip()
        if not symbol or not dso:
            continue
        if srcline and not SRCLINE_RE.match(srcline):
            srcline = ""
        key = (dso, symbol, srcline)
        samples[key] = samples.get(key, 0) + 1
        total += 1
    return samples, total


def build_hotspots(report_text: str, script_text: str, top: int, repo: Path) -> dict:
    """Merge script sample counts (preferred) with report text (fallback/validation)."""
    report_rows = parse_report_text(report_text)
    samples, script_total = parse_script_text(script_text)

    warnings: list[str] = []
    entries: list[dict] = []
    total_samples = 0

    if samples:
        by_sym: dict[tuple[str, str], dict] = {}
        for (dso, symbol, srcline), count in samples.items():
            key = (dso, symbol)
            cur = by_sym.setdefault(key, {"dso": dso, "symbol": symbol, "samples": 0,
                                          "sources": {}})
            cur["samples"] += count
            if srcline:
                cur["sources"][srcline] = cur["sources"].get(srcline, 0) + count
        total_samples = script_total
        for cur in by_sym.values():
            source = ""
            if cur["sources"]:
                source = max(cur["sources"].items(), key=lambda kv: kv[1])[0]
            entries.append({
                "samples": cur["samples"],
                "percent": (cur["samples"] * 100.0 / total_samples) if total_samples else 0.0,
                "dso": cur["dso"],
                "symbol": cur["symbol"],
                "source": source,
                "module": classify_module(source, cur["dso"], cur["symbol"], repo),
                "source_hit": bool(source),
            })
    else:
        total_samples = sum(h["samples"] for h in report_rows)
        entries = [{
            "samples": h["samples"],
            "percent": h["percent"],
            "dso": h["dso"],
            "symbol": h["symbol"],
            "source": "",
            "module": classify_module("", h["dso"], h["symbol"], repo),
            "source_hit": False,
        } for h in report_rows]
        if report_rows:
            warnings.append("perf script produced no samples; source lines unavailable")

    entries.sort(key=lambda h: (-h["percent"], -h["samples"], h["dso"], h["symbol"]))
    resolved = sum(h["samples"] for h in entries
                   if h["source_hit"] and not h["symbol"].startswith(("0x", "[unknown]")))
    source_samples = sum(h["samples"] for h in entries if h["source_hit"])
    unknown_samples = sum(h["samples"] for h in entries
                          if (not h["source_hit"] and (h["symbol"].startswith("0x") or
                                                       h["symbol"] in ("[unknown]", "unknown"))))
    if total_samples == 0:
        warnings.append("no samples parsed; check perf permissions and report format")
    if total_samples and source_samples * 100.0 / total_samples < 50.0:
        warnings.append(
            "less than 50% of samples resolve to source lines: build with -DMAQUE_PROFILE=ON "
            "(adds -g -fno-omit-frame-pointer) and do not strip the binary; see docs/35 §11")
    if total_samples and unknown_samples * 100.0 / total_samples > 10.0:
        warnings.append(
            "more than 10% samples are unresolved (0x…/[unknown]): perf permissions or missing "
            "symbols; see docs/35 §8/§11")

    # Module aggregation from the *full* entry set (not just top N).
    mods: dict[str, dict] = {}
    for h in entries:
        m = mods.setdefault(h["module"], {"module": h["module"], "samples": 0, "percent": 0.0})
        m["samples"] += h["samples"]
        m["percent"] += h["percent"]
    modules = sorted(mods.values(), key=lambda m: (-m["percent"], m["module"]))

    return {
        "total_samples": total_samples,
        "resolved_samples": resolved,
        "source_samples": source_samples,
        "unknown_samples": unknown_samples,
        "hotspots": entries,
        "modules": modules,
        "warnings": warnings,
    }


def write_report(out_dir: Path, perf: str, data: Path, report_text: str, result: dict,
                 *, command: list[str] | None, top: int, repo: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "perf-report.txt").write_text(report_text, encoding="utf-8")

    try:
        data_bytes = data.stat().st_size
    except OSError:
        data_bytes = 0
    version = perf_version(perf) if perf else ""
    hotspots = result["hotspots"][:top]
    payload = {
        "backend": "perf",
        "perf_version": version,
        "generated_at": now_iso(),
        "data": str(data),
        "data_bytes": data_bytes,
        "command": command or [],
        "total_samples": result["total_samples"],
        "resolved_samples": result["resolved_samples"],
        "source_samples": result["source_samples"],
        "unknown_samples": result["unknown_samples"],
        "hotspots": hotspots,
        "modules": result["modules"],
        "warnings": result["warnings"],
    }

    # Merge with an existing meta.json (in-engine runs already wrote one).
    meta_path = out_dir / "meta.json"
    meta = json_load(meta_path, {}) or {}
    meta["backend"] = "perf"
    if version:
        meta.setdefault("perf_version", version)
    meta["report_ok"] = result["total_samples"] > 0
    meta["samples"] = result["total_samples"]
    meta["report"] = {
        "generated_at": payload["generated_at"],
        "total_samples": result["total_samples"],
        "source_samples": result["source_samples"],
        "hotspots": len(hotspots),
        "modules": [m["module"] for m in result["modules"][:8]],
    }
    meta.setdefault("artifacts", {})
    meta["artifacts"]["report"] = str(out_dir / "perf-report.txt")
    meta["artifacts"]["hotspots"] = str(out_dir / "hotspots.json")
    json_dump(meta_path, meta)
    json_dump(out_dir / "hotspots.json", payload)

    (out_dir / "report.md").write_text(render_markdown(payload, meta, repo), encoding="utf-8")
    return payload


def _cell(text: str) -> str:
    return (text or "—").replace("|", "\\|").replace("\n", " ")


def fmt_pct(v: float) -> str:
    return f"{v:.2f}%"


def render_markdown(payload: dict, meta: dict, repo: Path) -> str:
    lines = [
        "# maque perf report",
        "",
        f"- generated: {payload['generated_at']}",
        f"- perf: `{payload['perf_version'] or '?'}`",
        f"- data: `{payload['data']}` ({payload['data_bytes']} bytes)",
        f"- samples: **{payload['total_samples']}** "
        f"(source-resolved: {payload['source_samples']})",
    ]
    ctx = meta.get("context") if isinstance(meta.get("context"), dict) else None
    if ctx:
        bits = [f"{k}={v}" for k, v in ctx.items() if k in ("scene", "frame", "build", "window")]
        if bits:
            lines.append(f"- context: `{', '.join(bits)}`")
    if payload.get("command"):
        lines.append("- command: `" + " ".join(payload["command"]) + "`")
    lines += ["", "## Hotspots (self time)", "",
              "| # | self % | samples | module | symbol | source |",
              "|---|--------|---------|--------|--------|--------|"]
    for i, h in enumerate(payload["hotspots"], 1):
        lines.append(
            f"| {i} | {fmt_pct(h['percent'])} | {h['samples']} | {_cell(h['module'])} | "
            f"`{_cell(h['symbol'])}` | `{_cell(h['source'])}` |")
    if not payload["hotspots"]:
        lines.append("| — | — | — | — | (no hotspots) | — |")

    lines += ["", "## Modules", "", "| module | self % | samples |",
              "|--------|--------|---------|"]
    for m in payload["modules"][:20]:
        lines.append(f"| {_cell(m['module'])} | {fmt_pct(m['percent'])} | {m['samples']} |")

    if payload["warnings"]:
        lines += ["", "## Warnings", ""] + [f"- {w}" for w in payload["warnings"]]
    lines += [
        "",
        "## Next steps",
        "",
        "```bash",
        f"python3 tools/perf/maque_perf.py report --data {payload['data']} --top {len(payload['hotspots'])}",
        "# edit the hottest source; rebuild; then:",
        "python3 tools/perf/maque_perf.py compare --baseline <base-dir> --candidate <candidate-dir> --fail-on-regression",
        "ctest --test-dir build --output-on-failure",
        "```",
        "",
    ]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# subcommands
# ---------------------------------------------------------------------------
def cmd_check(args) -> int:
    perf = find_perf(args.perf)
    result = {
        "ok": False,
        "platform": platform.platform(),
        "linux": platform.system() == "Linux",
        "perf": perf,
        "version": "",
        "paranoid": read_sysctl("/proc/sys/kernel/perf_event_paranoid"),
        "kptr_restrict": read_sysctl("/proc/sys/kernel/kptr_restrict"),
        "probe": None,
        "binary": None,
        "hints": [],
    }
    if not result["linux"]:
        result["hints"].append("perf profiling is Linux-only (docs/35)")
    if not perf:
        result["hints"].append(
            "perf not found: install linux-tools-$(uname -r) (Ubuntu/Debian), perf (Fedora) "
            "or set MAQUE_PROFILE_PERF=/path/to/perf")
    else:
        result["version"] = perf_version(perf)
        with tempfile.TemporaryDirectory(prefix="maque-perf-check-") as td:
            data = Path(td) / "probe.data"
            true_cmd = shutil.which("true") or "/bin/true"
            rc, out, _ = run_cmd([perf, "record", "-o", str(data), "-e", "task-clock", "--", true_cmd],
                                 timeout=30)
            if rc != 0:
                # task-clock may be unavailable on some kernels/VMs; fall back to perf's default
                # event (cycles) before declaring the platform unusable.
                rc, out, _ = run_cmd([perf, "record", "-o", str(data), "--", true_cmd], timeout=30)
            result["probe"] = {"rc": rc, "output": tail(out, 2000)}
            if rc != 0:
                result["hints"].append(
                    "perf record is not permitted: run `sudo sysctl kernel.perf_event_paranoid=1` "
                    "(or grant CAP_PERFMON to the process/container); see docs/35 §8")
            else:
                result["ok"] = True

    if args.binary:
        bin_path = Path(args.binary)
        info = {"path": str(bin_path), "exists": bin_path.exists(), "debug_info": None,
                "symtab": None}
        if bin_path.exists() and shutil.which("readelf"):
            rc, out, _ = run_cmd(["readelf", "-S", str(bin_path)], timeout=30)
            if rc == 0:
                info["debug_info"] = ".debug_info" in out
                info["symtab"] = ".symtab" in out
                if not info["debug_info"]:
                    result["hints"].append(
                        "binary has no .debug_info: configure with -DMAQUE_PROFILE=ON (adds -g)")
                if not info["symtab"]:
                    result["hints"].append("binary is stripped: keep symbols for perf reports")
        result["binary"] = info

    if result["ok"] and not result["hints"]:
        result["hints"].append("ready: `record` can run; see docs/35 §7 for the agent loop")

    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        print(f"platform: {result['platform']}")
        print(f"perf: {perf or '(not found)'} {result['version']}")
        print(f"perf_event_paranoid: {result['paranoid']}  kptr_restrict: {result['kptr_restrict']}")
        if result["probe"] is not None:
            print(f"probe rc: {result['probe']['rc']}")
        if result["binary"]:
            b = result["binary"]
            print(f"binary: {b['path']} exists={b['exists']} debug_info={b['debug_info']} symtab={b['symtab']}")
        for h in result["hints"]:
            print(f"hint: {h}")
        print("check: " + ("PASS" if result["ok"] else "FAIL"))
    return 0 if result["ok"] else 1


def _common_env(args, frames: int | None) -> dict:
    env = os.environ.copy()
    # Never let an inherited MAQUE_PROFILE start a *second* in-engine recorder.
    env.pop("MAQUE_PROFILE", None)
    env["MAQUE_PACE"] = "off"
    env["MAQUE_TRACE_STATS"] = "1"
    if args.headless:
        env["MAQUE_WINDOW"] = "0x0"
    if frames:
        env["MAQUE_MAX_FRAMES"] = str(frames)
    for item in args.env or []:
        if "=" not in item:
            die(f"--env expects KEY=VALUE, got {item!r}")
        k, v = item.split("=", 1)
        env[k] = v
    return env


def _perf_record_cmd(perf: str, data: Path, args, command: list[str], no_buildid: bool) -> list[str]:
    cmd = [perf, "record", "-o", str(data)]
    if args.frequency and args.frequency > 0:
        cmd += ["-F", str(args.frequency)]
    if args.callgraph and args.callgraph != "none":
        cmd += ["--call-graph", args.callgraph]
    if no_buildid:
        cmd.append("--no-buildid-mmap")
    cmd += ["--"] + command
    return cmd


def cmd_record(args) -> int:
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        die("record needs a command: ... -- ./build/samples/player/maque_player <scene>")
    perf = find_perf(args.perf)
    if not perf:
        die("perf not found; run `maque_perf.py check` or set MAQUE_PROFILE_PERF (docs/35 §8)", 77)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    data = out / "perf.data"
    log = out / "perf.log"
    env = _common_env(args, args.frames)

    # First try with --no-buildid-mmap; some containers/kernels print a build-id warning.
    rc, output, elapsed = run_cmd(_perf_record_cmd(perf, data, args, command, True),
                                  env=env, timeout=args.timeout, log_path=log)
    if rc != 0 and ("unknown option" in output or "unrecognized option" in output):
        rc, output, elapsed = run_cmd(_perf_record_cmd(perf, data, args, command, False),
                                      env=env, timeout=args.timeout, log_path=log)
    if rc != 0:
        hint = ""
        if "paranoid" in output or "permission" in output or "Access to performance monitoring" in output:
            hint = ("\nhint: sudo sysctl kernel.perf_event_paranoid=1  (or grant CAP_PERFMON); "
                    "see docs/35 §8")
        eprint(tail(output))
        die(f"perf record failed (rc={rc}){hint}")

    if not data.exists() or data.stat().st_size == 0:
        die("perf record produced an empty perf.data (permissions or no samples); see docs/35 §8")

    meta = {
        "backend": "perf",
        "mode": "external",
        "perf_version": perf_version(perf),
        "started_at": now_iso(),
        "wall_s": round(elapsed, 3),
        "frames": args.frames,
        "command": command,
        "env": {k: v for k, v in env.items() if k.startswith("MAQUE_")},
        "artifacts": {
            "data": str(data),
            "log": str(log),
            "report": str(out / "perf-report.txt"),
            "hotspots": str(out / "hotspots.json"),
        },
        "context": {
            "scene": _guess_scene(command),
            "frame": 0,
            "build": "unknown",
            "window": not args.headless,
        },
    }
    json_dump(out / "meta.json", meta)
    return _report_into(out, perf, data, args.top, command)


def _guess_scene(command: list[str]) -> str:
    for item in reversed(command):
        if item.endswith(".scn"):
            return item
    return ""


def _run_report(perf: str, data: Path, out: Path) -> tuple[int, str]:
    cmd = [perf, "report", "--stdio", "-i", str(data), "--no-children",
           "--sort", "dso,symbol", "--percent-limit", "0", "--show-nr-samples"]
    rc, report_text, _ = run_cmd(cmd, timeout=120)
    if rc != 0 and ("unknown option" in report_text or "unrecognized option" in report_text):
        cmd = [x for x in cmd if x != "--show-nr-samples"]
        rc, report_text, _ = run_cmd(cmd, timeout=120)
    (out / "perf-report.txt").write_text(report_text, encoding="utf-8")
    return rc, report_text


def _run_script(perf: str, data: Path, out: Path) -> tuple[int, str]:
    for fields in ("ip,sym,dso,srcline", "ip,sym,dso"):
        cmd = [perf, "script", "-i", str(data), "-F", fields]
        rc, text, _ = run_cmd(cmd, timeout=120)
        if rc == 0:
            (out / "perf-script.txt").write_text(text, encoding="utf-8")
            return rc, text
    (out / "perf-script.txt").write_text(text, encoding="utf-8")
    return rc, text


def _report_into(out: Path, perf: str, data: Path, top: int, command=None) -> int:
    rc, report_text = _run_report(perf, data, out)
    if rc != 0:
        eprint(tail(report_text))
        die(f"perf report failed (rc={rc})")
    src_rc, script_text = _run_script(perf, data, out)
    if src_rc != 0:
        script_text = ""
        eprint("maque_perf: warning: perf script failed; source lines unavailable")
    result = build_hotspots(report_text, script_text, top, REPO_ROOT)
    payload = write_report(out, perf, data, report_text, result, command=command, top=top,
                           repo=REPO_ROOT)
    if payload["total_samples"] == 0:
        die("no samples parsed; see docs/35 §8/§11")
    print(f"report: {payload['total_samples']} samples, "
          f"{len(payload['hotspots'])} hotspots → {out / 'hotspots.json'}")
    print(f"        {out / 'report.md'}")
    return 0


def cmd_report(args) -> int:
    data_arg = Path(args.data)
    if data_arg.is_dir():
        out = Path(args.out) if args.out else data_arg
        data = data_arg / "perf.data"
    else:
        data = data_arg
        out = Path(args.out) if args.out else data_arg.parent
    if not data.exists():
        die(f"perf.data not found: {data}")
    perf = find_perf(args.perf)
    if not perf:
        die("perf not found (needed to read perf.data); use `parse` with saved text if offline", 77)
    return _report_into(out, perf, data, args.top, command=None)


def cmd_parse(args) -> int:
    report_text = Path(args.report).read_text(encoding="utf-8") if args.report else ""
    script_text = Path(args.script).read_text(encoding="utf-8") if args.script else ""
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "perf-report.txt").write_text(report_text, encoding="utf-8")
    result = build_hotspots(report_text, script_text, args.top, REPO_ROOT)
    # `parse` is offline: use a placeholder perf path for metadata.
    payload = write_report(out, "", out / "perf.data", report_text, result,
                           command=None, top=args.top, repo=REPO_ROOT)
    if payload["total_samples"] == 0:
        die("no samples parsed from --report/--script")
    print(f"parse: {payload['total_samples']} samples, {len(payload['hotspots'])} hotspots → "
          f"{out / 'hotspots.json'}")
    return 0


def cmd_bench(args) -> int:
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        die("bench needs a command: ... -- ./build/samples/player/maque_player <scene>")
    out = Path(args.out) if args.out else Path(f".maque/bench/{int(time.time())}")
    out.mkdir(parents=True, exist_ok=True)
    env = _common_env(args, args.frames)

    runs = []
    for i in range(args.reps):
        log = out / f"bench-rep{i}.log"
        rc, output, elapsed = run_cmd(command, env=env, timeout=args.timeout, log_path=log)
        stats = [dict(zip(("frame", "fps", "total_ms", "logic_ms", "render_ms", "present_ms"),
                          (int(m.group(1)), float(m.group(2)), float(m.group(3)),
                           float(m.group(4)), float(m.group(5)), float(m.group(6)))))
                 for m in STATS_RE.finditer(output)]
        if len(stats) > 1:
            stats = stats[1:]          # drop the first (warm-up) line
        runs.append({"rc": rc, "wall_s": round(elapsed, 3),
                     "stats_lines": len(stats), "stats": stats,
                     "log": str(log)})
        print(f"bench[{i}]: rc={rc} wall={elapsed:.3f}s stats={len(stats)}")
        if rc != 0:
            eprint(tail(output))
            die(f"bench command failed (rc={rc})")

    def med(key):
        vals = [r[key] for r in runs]
        return statistics.median(vals) if vals else 0.0

    def med_stat(field):
        vals = [s[field] for r in runs for s in r["stats"]]
        return statistics.median(vals) if vals else 0.0

    summary = {
        "frames": args.frames,
        "reps": args.reps,
        "wall_s": med("wall_s"),
        "stats": {
            "total_ms": med_stat("total_ms"),
            "logic_ms": med_stat("logic_ms"),
            "render_ms": med_stat("render_ms"),
            "present_ms": med_stat("present_ms"),
            "fps": med_stat("fps"),
        },
    }
    json_dump(out / "bench.json", {"backend": "wallclock", "command": command,
                                   "generated_at": now_iso(), "summary": summary, "runs": runs})
    print(f"bench: median wall={summary['wall_s']:.3f}s total={summary['stats']['total_ms']:.2f}ms "
          f"logic={summary['stats']['logic_ms']:.2f}ms render={summary['stats']['render_ms']:.2f}ms "
          f"present={summary['stats']['present_ms']:.2f}ms → {out / 'bench.json'}")
    return 0


def _load_hotspots(directory: Path) -> dict | None:
    return json_load(directory / "hotspots.json")


def _hotspot_pct(h: dict) -> float:
    """The C++ (in-engine) writer uses `pct`; the Python report uses `percent`."""
    try:
        return float(h.get("percent", h.get("pct", 0.0)))
    except (TypeError, ValueError):
        return 0.0


def _hotspot_total(doc: dict) -> int:
    try:
        return int(doc.get("total_samples", doc.get("samples", 0)))
    except (TypeError, ValueError):
        return 0


def _load_bench(directory: Path) -> dict | None:
    return json_load(directory / "bench.json")


def cmd_compare(args) -> int:
    base = Path(args.baseline)
    cand = Path(args.candidate)
    threshold = args.threshold / 100.0
    min_pp = args.min_delta
    result = {"baseline": str(base), "candidate": str(cand), "threshold_pct": args.threshold,
              "bench": None, "hotspots": None, "regressions": [], "improvements": [],
              "warnings": []}

    bb, cb = _load_bench(base), _load_bench(cand)
    if bb and cb:
        bs, cs = bb.get("summary", {}), cb.get("summary", {})
        rows = []
        for key, label in (("wall_s", "wall seconds"),):
            b, c = bs.get(key, 0.0), cs.get(key, 0.0)
            if b:
                rows.append({"metric": label, "baseline": b, "candidate": c,
                             "delta_pct": (c - b) * 100.0 / b})
        for key, label in (("total_ms", "total ms"), ("logic_ms", "logic ms"),
                           ("render_ms", "render ms"), ("present_ms", "present ms")):
            b = bs.get("stats", {}).get(key, 0.0)
            c = cs.get("stats", {}).get(key, 0.0)
            if b:
                rows.append({"metric": label, "baseline": b, "candidate": c,
                             "delta_pct": (c - b) * 100.0 / b})
        result["bench"] = rows
        for row in rows:
            if row["delta_pct"] > args.threshold:
                result["regressions"].append({**row, "kind": "bench"})
            elif row["delta_pct"] < -args.threshold:
                result["improvements"].append({**row, "kind": "bench"})
    elif bb or cb:
        result["warnings"].append("only one side has bench.json; bench comparison skipped")

    bh, ch = _load_hotspots(base), _load_hotspots(cand)
    if bh and ch:
        def key(h):
            return (h.get("dso", ""), h.get("symbol", ""))
        bmap = {key(h): h for h in bh.get("hotspots", [])}
        cmap = {key(h): h for h in ch.get("hotspots", [])}
        bt, ct = _hotspot_total(bh), _hotspot_total(ch)
        if bt and ct:
            ratio = ct / max(1, bt)
            if ratio > 2.0 or ratio < 0.5:
                result["warnings"].append(
                    f"sample counts differ a lot ({bt} vs {ct}); "
                    "compare percentages with care (run the same frames/command)")
        rows = []
        for k in set(bmap) | set(cmap):
            b = _hotspot_pct(bmap.get(k, {}))
            c = _hotspot_pct(cmap.get(k, {}))
            if max(b, c) < 0.2:
                continue
            rows.append({"symbol": k[1], "dso": k[0], "baseline_pct": b, "candidate_pct": c,
                         "delta_pp": c - b,
                         "delta_pct": ((c - b) * 100.0 / b) if b > 0 else None,
                         "source": (cmap.get(k) or bmap.get(k) or {}).get("source", ""),
                         "module": (cmap.get(k) or bmap.get(k) or {}).get("module", "")})
        rows.sort(key=lambda r: (-(r["delta_pp"]), r["symbol"]))
        result["hotspots"] = rows[: args.top]
        for row in rows:
            if row["delta_pp"] <= min_pp:
                continue
            if row["baseline_pct"] <= 0:
                if row["candidate_pct"] >= max(1.0, args.threshold):
                    result["regressions"].append({**row, "kind": "new_hotspot"})
            elif row["candidate_pct"] > row["baseline_pct"] * (1.0 + threshold):
                result["regressions"].append({**row, "kind": "hotspot"})
            if row["delta_pp"] <= -min_pp and row["baseline_pct"] > 0 and \
                    row["candidate_pct"] < row["baseline_pct"] * (1.0 - threshold):
                result["improvements"].append({**row, "kind": "hotspot"})
    elif bh or ch:
        result["warnings"].append("only one side has hotspots.json; hotspot comparison skipped")

    if not result["bench"] and not result["hotspots"]:
        die("nothing to compare: need bench.json or hotspots.json on both sides")

    failed = bool(result["regressions"]) and args.fail_on_regression
    result["verdict"] = ("FAIL" if failed else
                         ("REGRESSED" if result["regressions"] else "OK"))
    if args.json:
        print(json.dumps(result, indent=2, ensure_ascii=False))
    else:
        if result["bench"]:
            print("bench:")
            for row in result["bench"]:
                print(f"  {row['metric']}: {row['baseline']:.3f} → {row['candidate']:.3f} "
                      f"({row['delta_pct']:+.1f}%)")
        if result["hotspots"]:
            print("hotspots (top deltas):")
            for row in result["hotspots"][:15]:
                rel = "new" if row["delta_pct"] is None else f"{row['delta_pct']:+.1f}%"
                print(f"  {row['delta_pp']:+.2f}pp {row['candidate_pct']:6.2f}% ({rel}) "
                      f"{row['module']}: {row['symbol']}")
        for r in result["regressions"][:10]:
            print(f"REGRESSION [{r['kind']}]: {r.get('metric') or r.get('symbol')}")
        for w in result["warnings"]:
            print(f"warning: {w}")
        print("compare: " + result["verdict"])
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="maque_perf.py",
        description="maque Linux perf profiling helper (docs/35)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("check", help="check perf availability/permissions/debug info")
    c.add_argument("--perf", help="perf executable (default: MAQUE_PROFILE_PERF or PATH)")
    c.add_argument("--binary", help="engine binary to check for .debug_info/.symtab")
    c.add_argument("--json", action="store_true")
    c.set_defaults(func=cmd_check)

    r = sub.add_parser("record", help="run a command under perf record, then report")
    r.add_argument("--out", required=True, help="artifact directory")
    r.add_argument("--frames", type=int, default=600, help="MAQUE_MAX_FRAMES (default 600)")
    r.add_argument("--frequency", type=int, default=999, help="perf sample frequency Hz (0=default)")
    r.add_argument("--callgraph", default="fp", choices=["fp", "dwarf", "none", "lbr"])
    r.add_argument("--top", type=int, default=DEFAULT_TOP)
    r.add_argument("--timeout", type=int, default=1800)
    r.add_argument("--headless", dest="headless", action="store_true", default=True,
                   help="set MAQUE_WINDOW=0x0 (default)")
    r.add_argument("--window", dest="headless", action="store_false",
                   help="keep the real window (do not force MAQUE_WINDOW=0x0)")
    r.add_argument("--perf")
    r.add_argument("--env", action="append", metavar="KEY=VALUE")
    r.add_argument("command", nargs=argparse.REMAINDER,
                   help="-- <engine command> (e.g. -- ./build/samples/player/maque_player scene.scn)")
    r.set_defaults(func=cmd_record)

    p2 = sub.add_parser("report", help="(re)generate report from perf.data")
    p2.add_argument("--data", required=True, help="perf.data or artifact directory")
    p2.add_argument("--out")
    p2.add_argument("--top", type=int, default=DEFAULT_TOP)
    p2.add_argument("--perf")
    p2.set_defaults(func=cmd_report)

    p3 = sub.add_parser("parse", help="offline: parse saved perf-report/perf-script text")
    p3.add_argument("--report", required=True, help="saved `perf report --stdio` text")
    p3.add_argument("--script", help="saved `perf script -F ip,sym,dso[,srcline]` text")
    p3.add_argument("--out", required=True)
    p3.add_argument("--top", type=int, default=DEFAULT_TOP)
    p3.set_defaults(func=cmd_parse)

    b = sub.add_parser("bench", help="wall-clock benchmark without perf (works without privileges)")
    b.add_argument("--out")
    b.add_argument("--frames", type=int, default=600)
    b.add_argument("--reps", type=int, default=3)
    b.add_argument("--timeout", type=int, default=1800)
    b.add_argument("--headless", dest="headless", action="store_true", default=True)
    b.add_argument("--window", dest="headless", action="store_false")
    b.add_argument("--env", action="append", metavar="KEY=VALUE")
    b.add_argument("command", nargs=argparse.REMAINDER)
    b.set_defaults(func=cmd_bench)

    cp = sub.add_parser("compare", help="compare baseline/candidate dirs (bench + hotspots)")
    cp.add_argument("--baseline", required=True)
    cp.add_argument("--candidate", required=True)
    cp.add_argument("--threshold", type=float, default=3.0,
                    help="relative %% considered a regression (default 3)")
    cp.add_argument("--min-delta", type=float, default=0.5,
                    help="minimum percentage-point delta for hotspot regressions (default 0.5)")
    cp.add_argument("--top", type=int, default=25)
    cp.add_argument("--fail-on-regression", action="store_true")
    cp.add_argument("--json", action="store_true")
    cp.set_defaults(func=cmd_compare)
    return p


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
