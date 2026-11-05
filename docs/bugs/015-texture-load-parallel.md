# 015 · 纹理/网格装配期单线程：ABeautifulGame 加载时 decode / mip / mesh JSON 热点

- **类型**：性能（CPU，装配期/首帧）
- **状态**：fixed
- **影响面**：所有带文件 PBR 贴图或 `.mesh.json` 的场景（`.mesh.json` 的
  `asset.PbrMaterial`、运行时 `.gltf/.glb` 内嵌/外链贴图）；ABeautifulGame（33 张 2048² JPG +
  15 个唯一 mesh，49 个 MeshRenderer 实例）最典型
- **发现版本**：`maque_player scenes/khronos/ABeautifulGame/ABeautifulGame.scn` 的 perf 采样

## 症状

采样（`perf`）在加载 ABeautifulGame 时集中在：

| 热点 | 占比（inclusive） |
|---|---:|
| `maque::rt::Engine::load_gltf_model` | 51.2% |
| `mq_image_decode` | 50.1% |
| `maque::rt::build_mip_chain` | 36.0% |
| `maque::rt::srgb_encode_from_linear` | 14.0%（`build_mip_chain` 子项） |

同一场景 headless（Debug、`--frames 2`）墙钟约 **11.0 s**；33 张贴图全部在装配期/首帧被
**串行** decode，再逐张做 CPU mip 链，GPU 上传才返回；同时 15 个 `.mesh.json` 在主线程逐个
`mq_json_parse` + 顶点转换 + `create_mesh`（约 5 s）。RSS 约 1.9 GB（2048² 贴图较多）。

## 最小复现

```bash
# 装配期墙钟 + preload 统计
MAQUE_TEX_INFO=1 MAQUE_WINDOW=0x0 MAQUE_MAX_FRAMES=1 \
  ./build/samples/player/maque_player scenes/khronos/ABeautifulGame/ABeautifulGame.scn

# 对照：关闭并行（应明显变慢，但输出像素必须不变）
MAQUE_TEX_THREADS=1 ... --shot /tmp/serial.ppm ...
MAQUE_TEX_THREADS=4 ... --shot /tmp/mt.ppm ...
cmp /tmp/serial.ppm /tmp/mt.ppm
```

## 根因

1. **所有 CPU 工作串行**：`load_texture_srgb/mips` 在 `extract_3d` 首次遇到材质时逐张
   `mq_image_decode` + `build_mip_chain`；glTF 路径的 `load_gltf_model` 也在材质循环里
   逐 image decode、逐 texture 生成 mip。
2. **mip 生成实现分配/分支偏多**：每层 `out.resize`（可能重复扩容）、每输出像素
   `std::min` 取 4 邻居、sRGB 每通道二分编码；单张 2048² 已足够成为可见热点。
3. **没有预载**：`preload_scene_assets()` 只预载 2D Sprite 纹理；PBR 材质槽要等首帧
   `extract_3d` 才第一次读取；`.mesh.json` 也要等 `setup_gltf_assets()` / 首帧 `load_mesh`。
4. **mesh JSON 是 DOM 解析**：1.3M+ JSON 数字逐节点分配，单线程解析后再上传 GPU。

## 修复

- **装配期 PBR 贴图预载**：`preload_scene_assets()` 扫描所有 `asset.PbrMaterial` 的文件贴图槽
  （base/MR/AO/normal/emissive + KHR 扩展槽），`preload_texture_refs()` 按
  `(ref, srgb)` 去重排序；**decode + mip 在有限 worker 线程里做**，`create_texture2` 仍在主线程。
  缓存 key 与 lazy 路径完全一致（`srgb:<ref>` / `mip:<ref>`），首帧直接命中。
- **mesh `.json` 预载**：`preload_scene_assets()` 收集非 glTF 的 `render.MeshRenderer.mesh_path`，
  `preload_mesh_refs()` 按路径去重排序；CPU `mq_json_parse` + 顶点/索引/蒙皮/morph 转成
  `PreparedMeshData` 在 worker 线程做，GPU `create_mesh` 与 morph 默认权重应用回主线程；
  cache/`mesh_layout`/`mesh_stats` 语义不变。
- **glTF 路径**：`load_gltf_model` 先扫描材质引用到的 image，批量并行 decode；上传/材质循环
  仍主线程。未引用的 image 不 decode。
- **`build_mip_chain`**：一次算好总字节数、只分配一次；大层按**行**分片并行（行间无依赖）；
  内层循环拆开 4 邻居读取；sRGB/线性的求和顺序与旧实现完全一致，**输出逐字节不变**。
- 可调：`MAQUE_TEX_THREADS`（默认 `min(8, hw)`，Android 最多 4；1=串行，4=更省内存）、
  `MAQUE_TEX_PRELOAD=0`（关闭贴图预载）、`MAQUE_TEX_INFO=1`、
  `MAQUE_MESH_PRELOAD=0`（关闭 mesh 预载）、`MAQUE_MESH_INFO=1`（打印 mesh preload 统计）。

## 回归与证据

同机 Debug headless：

| 场景/配置 | 修复前 | 修复后 |
|---|---:|---:|
| `scenes/khronos/ABeautifulGame`（`.mesh.json`，默认 8 线程） | 11.0 s（`--frames 2`） | **3.9 s**（mesh 1.35 s + texture 1.14 s） |
| 同上，4 线程（`MAQUE_TEX_THREADS=4`） | — | 5.1 s（mesh 1.89 s + texture 1.72 s） |
| 同上，串行（`MAQUE_TEX_THREADS=1`） | — | 9.6 s（纹理与 mesh 预载都串行） |
| `scenes/khronos-bin/ABeautifulGame`（`.glb`，默认 8 线程） | 6.5 s（串行） | **2.9 s**（glTF model 1.48 s：decode 0.73 / materials 0.52 / parse 0.13 / meshes 0.10） |

- 33 个 texture job + 15 个 mesh job，均 `uploaded==jobs`、`failed=0`。
- **像素一致性**：串行 vs 4 线程的 `--shot` PPM 对两套场景均 `sha256` 相同
  （`.mesh.json` 路径 `96c6d3…`；`.glb` 路径 `1ca952…`）。
- 回归：`texture_preload_e2e`（串行/并行逐字节 + texture/mesh preload 统计）、
  `gltf_runtime_e2e`、`gltf_runtime_anim_e2e`、`gltf_runtime_material_e2e`、
  `gltf_runtime_ktx2_e2e`、`gltf_morph_e2e`、`gltf_skin_e2e`、`render3d_scene_e2e`、
  `material_*_e2e`、`pbr_regression_e2e` 全过。
- 后续：`.glb` 的 `gltf phases`（`MAQUE_RENDER3D_INFO=1`）显示 mesh parse 只有 0.13 s，
  剩余主要是 image decode（0.73 s）与材质/mip 上传（0.52 s）；下一步应继续压纹理路径，
  而不是 mesh 解析。

## 相关

- [13 · 渲染 §8](../13-渲染（WebGPU）.md)（纹理装配并行开关）
- [29 · Tracy](../29-Tracy性能分析.md) / [35 · perf](../35-Linux采样剖析（perf）.md)（采样入口）
- 测试：`tests/texture_preload_e2e.sh`（CTest `texture_preload_e2e`）
