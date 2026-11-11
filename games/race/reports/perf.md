# Perf (desert, 1280x720, Release)

Collected: 2026-11-10 21:28

| metric | measured | budget |
|---|---:|---:|
| 3D draws | 132 | <= 200 |
| draws total (incl. UI) | 139 | - |
| triangles | 28708 | <= 250k |
| frame total (avg) | 10.6 ms | <= 16.7 ms |
| logic, script+sim (avg) | 1.68 ms | <= 3 ms |

## verify.ps1 output

```
=== maque racing: verify ===
[ok]   menu smoke
[ok]   race smoke desert
[ok]   race smoke beach
[ok]   race smoke forest
[ok]   race smoke city
[ok]   determinism world_trail=0x3b1387422b0e6c7c
[ok]   shot desert (377102 bytes)
[ok]   shot beach (423479 bytes)
[ok]   shot forest (400662 bytes)
[ok]   shot city (549369 bytes)
  stats (avg of 9 samples): draws=139 3d=132 tris=28708 total=10.6ms logic=1.68ms
[ok]   draw budget 3d_draws=132 <= 200
[ok]   frame budget total=10.6ms <= 18ms
```
