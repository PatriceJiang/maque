# tools/perf —— Linux 采样剖析（perf）工具

> 设计/权限/agent 闭环的完整说明在 [`docs/35`](../../docs/35-Linux采样剖析（perf）.md)。
> 决策：**只用 perf，不接 oprofile**（理由见 docs/35 §1）。

## 快速命令

```bash
# 检查 perf 权限 / 二进制符号
python3 tools/perf/maque_perf.py check --binary build/samples/player/maque_player

# 录 600 帧，生成 report.md + hotspots.json
python3 tools/perf/maque_perf.py record --out .maque/perf/base --frames 600 -- \
  ./build/samples/player/maque_player scenes/pbr/cube.scn

# 改完代码后重录并做回归门
python3 tools/perf/maque_perf.py record --out .maque/perf/cand --frames 600 -- \
  ./build/samples/player/maque_player scenes/pbr/cube.scn
python3 tools/perf/maque_perf.py compare --baseline .maque/perf/base \
  --candidate .maque/perf/cand --fail-on-regression

# 不需要 perf 权限的墙钟基准（3 次中位数）
python3 tools/perf/maque_perf.py bench --out .maque/bench/base --frames 600 --reps 3 -- \
  ./build/samples/player/maque_player scenes/pbr/cube.scn
```

## 产物

`<out>/` 下：`perf.data`、`perf.log`、`perf-report.txt`、`perf-script.txt`、
`hotspots.json`、`report.md`、`meta.json`（引擎内录制还有 `marks.jsonl`）。
`hotspots.json` 是 agent 的主入口；字段见 docs/35 §7。

## 离线解析（无 perf/权限）

```bash
python3 tools/perf/maque_perf.py parse \
  --report tests/data/perf/sample-report.txt \
  --script tests/data/perf/sample-script.txt \
  --out /tmp/perf-report --top 10
```

## 运行引擎内录制（不包一层 perf）

```bash
MAQUE_PROFILE=1 MAQUE_PROFILE_FRAMES=600 MAQUE_PROFILE_DIR=.maque/perf/live \
  ./build/samples/player/maque_player <scene>
```

录制中 `F5` 开关；控制面 `Perf.profile_status/start/stop/report/mark` 见 docs/35 §6。
`profile_start/stop/mark` 是变更类：只读控制会话会被拒绝，启动期环境方式不需要写权限。

## 常见失败

- `perf record is not permitted` → `sudo sysctl kernel.perf_event_paranoid=1` 或容器
  `--cap-add=PERFMON`（docs/35 §8）。
- 热点全是 `0x…`/`[unknown]` → 用 `-DMAQUE_PROFILE=ON` 构建、不要 strip。
- `perf.data` 为空 → 先 `check`，再看 `<out>/perf.log`。
