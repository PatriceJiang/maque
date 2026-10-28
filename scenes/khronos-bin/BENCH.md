# khronos-bin 加载速度对比

对比 `scenes/khronos`（离线 `.mesh.json` 资产）与 `scenes/khronos-bin`（runtime 直读源 `.glb`）。
两个目录的 **画廊 UI、每个模型的相机/灯光/场景 env 一致**；bin 变体只把模型整体换成一个
`render.MeshRenderer.mesh_path = assets/<Model>.glb`。

## 方法

```powershell
# 生成对照场景 + GLB（缺失 GLB 时 npx @gltf-transform/cli copy 自动补）
python tools/make_khronos_bin_scenes.py

# 全量 149 模型逐场景测时（每个场景一个 maque_player 进程，--frames 1 后退出）
python tools/bench_khronos_load.py --repeat 1 --jobs 1 --timeout 300
```

- 构建：Windows clang-cl **Debug + ASan**，`MAQUE_WINDOW=0x0`、`MAQUE_PRESENT=off`、
  `MAQUE_ANIM=0`、`MAQUE_MORPH=0`、`MAQUE_RENDER3D_INFO=1`。
- 每个模型跑 JSON / GLB 各一次；全量耗时 **528.5 s**（含进程启动）。
- `load_ms`：引擎日志中模型加载时间之和（JSON = `runtime: mesh '...'`，GLB = `runtime: gltf model '...'`）。
- `wall_ms`：整个 `maque_player` 进程墙钟（含引擎/GPU 初始化，两个变体相同）。**主指标用 wall**，因为
  JSON 路径的 `load_ms` 只含 mesh 解析，贴图解码在日志外；GLB 的 `load_ms` 含内嵌贴图解码。

## 汇总（149 模型，全部成功）

| 指标 | JSON (.mesh.json) | GLB | 结果 |
|---|---:|---:|---:|
| wall 总和 | 377.6 s | 150.8 s | **2.50x 更快** |
| wall 中位数 | 864.0 ms | 571.0 ms | 1.51x |
| load 总和 | 209.6 s | 80.9 s | 2.59x |
| load 中位数 | 131.9 ms | 113.9 ms | 1.16x |
| 更快模型数 | — | **137 / 149** | 12 个 JSON 更快 |

## wall 提速最大的模型

| 模型 | JSON | GLB | 提速 |
|---|---:|---:|---:|
| MetalRoughSpheresNoTextures | 16.8 s | 0.62 s | 26.9x |
| NodePerformanceTest | 75.1 s | 4.1 s | 18.3x |
| IridescenceMetallicSpheres | 13.2 s | 0.81 s | 16.4x |
| IridescenceDielectricSpheres | 13.1 s | 0.91 s | 14.5x |
| MetalRoughSpheres | 8.2 s | 0.66 s | 12.5x |
| SheenWoodLeatherSofa | 5.5 s | 0.50 s | 11.0x |
| AnimationPointerUVs | 6.3 s | 0.81 s | 7.8x |
| CarConcept | 7.4 s | 0.99 s | 7.5x |
| BrainStem | 3.5 s | 0.49 s | 7.1x |
| IORTestGrid | 2.9 s | 0.47 s | 6.3x |

## GLB 反而更慢的模型（12 个）

| 模型 | JSON | GLB | 说明 |
|---|---:|---:|---|
| MaterialsVariantsShoe | 1.64 s | 1.97 s | GLB 内嵌贴图解码/上传更重 |
| AnimatedCube | 448 ms | 483 ms | 极小模型，进程启动占主导 |
| BoomBox | 1.47 s | 1.52 s | 同上（贴图为主） |
| UnlitTest | 444 ms | 460 ms | 启动噪声 |
| Avocado | 1.22 s | 1.26 s | 贴图为主 |
| Box | 423 ms | 431 ms | 启动噪声 |
| BoxVertexColors | 424 ms | 430 ms | 启动噪声 |
| SimpleMaterial | 429 ms | 434 ms | 启动噪声 |
| TriangleWithoutIndices | 422 ms | 425 ms | 启动噪声 |
| BoxTextured | 432 ms | 433 ms | 启动噪声 |
| AnimatedTriangle | 431 ms | 432 ms | 启动噪声 |
| BoxTexturedNonPowerOfTwo | 433 ms | 433 ms | 启动噪声 |

## 结论与注意

- 对**大网格/多节点**模型（NodePerformanceTest、MetalRoughSpheres 系列、Iridescence 系列），GLB 直读
  显著快于离线 JSON：省掉大量 JSON 文本解析与逐 mesh 创建，wall 提升一个数量级。
- 对**小模型**，进程启动（约 0.42 s）掩盖差异；GLB 内嵌贴图解码可能略慢。
- 这是 **Debug + ASan** 数据，绝对值请以 Release 重测为准；相对趋势可复现。
- 全量结果 JSON：`maque-test-tmp/khronos_load_bench.json`（临时目录，不入库）。
