# 23 · 运行时 glTF/GLB 直读（C++）

> 状态：**静态网格 + 核心 PBR 已落地**；蒙皮/动画/morph 已由 [plans/24](24-runtime-gltf-capabilities.md) 补上。
> 上游：`docs/20-运行时与资源模型.md`、`docs/31-gltf-model-migration.md`；相关：`plans/18-pbr-3d.md`
> （M4b/M4c 离线导入）、`plans/22-gltf-animation-implementation.md`（蒙皮/动画派生资产）。

## 1. 模块概述

- **职责**：让引擎在**运行时**直接用 C++ 读取 `.gltf`（JSON + 外部 `.bin` / `data:` URI）与
  `.glb`（二进制单文件），不再依赖 `python tools/gltf_import.py` 预生成 `.mesh.json`。
- **接入点**：场景组件 `render.MeshRenderer.mesh_path`（相对场景目录 / VFS 根）。
  扩展名 `.gltf` / `.glb` → 走本加载器；其它路径仍走既有 `.mesh.json` 管线。
- **所在层**：runtime（`modules/runtime/src/gltf_model.{h,cpp}`，runtime 内部 C++，**不新增跨模块 ABI**）。
- **一句话**：`mesh_path: "model.glb"` 即渲染；装配期解析/上传一次，模拟帧内只读缓存。

## 2. 接口与数据流（runtime 内部）

```
scn 装配 (setup_gltf_assets)
  └─ mesh_path 以 .gltf/.glb 结尾
       └─ Engine::load_gltf_model(path)                 // 按 VFS 路径缓存
            ├─ Engine::read_vfs_file(path)              // 场景目录 / 远程挂载均可
            ├─ gltf::parse(path, bytes, ReadFn)          // CPU：JSON/GLB/BIN/accessor/node/material
            ├─ mq_image_decode → upload_texture_rgba     // sRGB/线性 + CPU mip + sampler
            ├─ rapi->create_mesh                         // 每 primitive 一个网格句柄
            └─ mq_renderer_pbr_material[]                // 核心 PBR 材质数组
extract_3d
  └─ 每个 primitive → 一条 mq_renderer_draw3d（mesh + material + 实体 world_m）
```

- `gltf::Model`（CPU 侧，无 GPU/无全局状态）：`primitives[]`、`materials[]`、`images[]`、
  `has_skin/has_animation` 标记。
- 顶点行布局与 `.mesh.json` **完全一致**：layout 0 = `pos3|normal3|tangent4|uv0_2`（48B），
  layout 1 = `pos3|normal3|color4|uv0_2|uv1_2`（56B）；`TANGENT` 不读（shader 用屏幕导数切线基）。
- 材质：`baseColorFactor`、`metallicFactor`、`roughnessFactor`、`emissiveFactor`、
  `alphaMode/alphaCutoff`、`doubleSided→side/flags`、baseColor/MR/normal/occlusion/emissive 贴图、
  `KHR_materials_unlit`（→ Basic）、`KHR_materials_emissive_strength`、
  `KHR_texture_transform` 的 offset/scale（base 组 + normal 组）。
- 贴图槽顺序与组件路径一致：`textures[0..4] = base(sRGB)/MR(线性)/AO(线性)/normal(线性)/emissive(sRGB)`；
  glTF sampler 的 wrap/filter 直接传给 `create_texture2`；`TEXCOORD_1` → `uv1_mask << 4` 进 `flags2`。

## 3. 边界（首版明确不做）

| 未支持 | 现状 |
|---|---|
| 蒙皮 `skins` / 动画 `animations` | **已支持**（plans/24 P1/P2）：关节调色板 + T/R/S/weights 采样；见能力矩阵 |
| morph targets | **已支持**（plans/24 P3）：默认/动画权重 + CPU `update_mesh` |
| Draco / meshopt / WebP 贴图 | ⛔ 不做（无第三方解码依赖）；KTX2/BasisU 已单独接入（plans/24） |
| sparse accessor | **已支持**（plans/24 P6）：indices/values 覆盖 base |
| 节点层级实例化 | 节点世界矩阵逐帧计算；`EXT_mesh_gpu_instancing` 展开为合成子节点；一个 `MeshRenderer` 实体 = 一个整体模型 |
| glTF 相机/灯光 | **已支持**（plans/24 P5）：`KHR_lights_punctual` → frame3d；无场景相机时用 perspective 相机 |
| KHR 材质扩展（clearcoat/sheen/specular/transmission/volume…） | **已支持**（plans/24 P4）：因子 + 扩展贴图槽 + transmission/volume 近似 |
| 非三角 primitive（线/点） | 跳过并告警 |
| 无材质 primitive | 用引擎默认灰材质（base 白、metallic 0、roughness 0.6，与组件缺省一致） |

## 4. 正确性与确定性

- **不引入模拟帧内 IO**：解析/解码/上传全部在装配期（`setup_gltf_assets`）完成；提取期只读缓存。
- 缓存键 = `mesh_path` 字符串；切场景 `clear_asset_caches()` 统一销毁本加载器创建的网格/纹理句柄并清空。
- 外部 URI 相对**模型文件所在目录**解析（百分号解码、`.`/`..` 归一化；`data:` 内联；
  GLB 的 BIN chunk 作为无 `uri` 的 buffer[0]）。远程 VFS（http(s) 目录挂载）同样适用。
- 节点 `matrix` 或 TRS（T*R*S）按列主序合成；顶点保留 mesh 空间，节点世界矩阵在 draw 时应用
  （plans/24 起支持动画）；法线由 shader 的 model 逆转置处理；负行列式缩放时反转三角形绕序。
- 缺 `NORMAL`：按真实 indices 累计面法线；缺 `TEXCOORD_1`：复制 uv0；`COLOR_0` 支持 3/4 分量与 normalized。
- 渲染结果确定性：同一输入多次 `--shot` 逐字节一致；`.gltf`（外部 bin）/`.glb`/`data:` 三种容器
  同一几何的截图**逐像素一致**（实测 max channel diff = 0）。

## 5. 测试

- `tests/gltf_runtime_e2e.sh`（`ctest -R gltf_runtime_e2e`）：
  夹具 `tests/data/gltf/runtime/`（2 primitive / 2 material / 1 张红白棋盘贴图）覆盖
  ① `.gltf` + 外部 `.bin` + 外部贴图；② `.glb` 内嵌 BIN + bufferView 贴图；③ `data:` URI；
  ④ 缺 `.bin` 的失败路径（进程不崩、正常退出）；
  断言日志 `runtime: gltf model ... primitives>=2`、贴图加载日志、PPM 结构（非背景/中心/红蓝材质区分）、
  三种容器截图逐像素 ≤2。
- 手工回归：`scenes/pbr/cube.scn`、`imported.scn` 等 `.mesh.json` 场景不受影响。

## 6. 后续（详见 plans/24 能力矩阵）

1. ~~蒙皮/动画/morph~~：已由 [plans/24](24-runtime-gltf-capabilities.md) P1–P3 落地。
2. ~~材质 KHR 扩展映射（P4）、glTF 灯光/相机（P5）、instancing/sparse（P6）~~：已由 plans/24 落地；
   剩余 animation_pointer/variants/node_visibility。
3. `--scene` 等价物：把 glTF 节点树实例化为实体（per-node Transform3D + MeshRenderer + PbrMaterial），
   支持分部件独立运动与 glTF 相机/灯光。
4. Draco/meshopt 解压（KTX2 纹理解码已由 plans/24 接入 `third_party/basisu`；Draco/meshopt 仍需新依赖）。
5. 首次加载卡顿优化：可选把上传结果缓存为二进制旁路文件（但默认保持"零转换"）。
