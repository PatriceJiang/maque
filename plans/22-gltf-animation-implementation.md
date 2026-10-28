# 22 · glTF 动画 / 蒙皮 / morph 落地实现（S1+S2+S3）

> 状态：**已落地（第 29 轮，2026-10-28）**：S1 骨架蒙皮 + S2 关键帧采样 + S3 morph 全部实现，27 个 Khronos
> 动画/蒙皮/morph 模型已重新迁移；`ctest -R 'gltf_(skin|anim|morph)_e2e'` 全绿，非动画场景回归无变化。
> 本文件是 `plans/21-gltf-animation-and-compression.md`（评估稿）的
> **实现契约**：ABI / 资产格式 / 场景结构 / 渲染与采样语义在这里冻结，实现与测试以本文为准。
> 关联：`plans/21`（评估/统计）、`docs/31 §5.4/§11`（迁移差距）、`docs/13`（WebGPU）、`docs/21`（schema/codegen）。
>
> 范围：S1 骨架/蒙皮/bind pose、S2 关键帧采样 + 节点/骨骼动画、S3 morph（默认权重 + weights 动画）。
> **不做**：`KHR_animation_pointer`、CUBICSPLINE 精确切线（按 LINEAR 近似）、动画状态机/混合、
> `plans/12` 完整动画模块（`mq_anim.h` ABI 冻结）。

---

## 0. TL;DR（改动清单）

| 层 | 文件 | 改动 |
|---|---|---|
| schema | `modules/core/data/schema/render/renderer.yaml` | 追加 `skin` 记录、`draw3d.skin`、`frame3d.skins/skin_count`、`update_mesh` 方法（全部表尾追加，version 仍 1） |
| schema | `modules/core/data/schema/render/render.yaml` | `render.MeshRenderer` 追加 `skin_path: string`（空=非蒙皮） |
| schema | `modules/core/data/schema/world/scn.yaml` | `scn.AnimationPlayer` 追加 `clips: string[]` / `clip_index: int32_t`（非空=glTF 片段通路；空=旧 2D 属性轨道） |
| 生成物 | `modules/core/data/generated/*` | `node parser.js` 重新生成并提交 |
| 导入器 | `tools/gltf_import.py` | 读 `skins` / `JOINTS_0` / `WEIGHTS_0` / `animations` / `primitive.targets`；输出 `.skel.json` / `.clip.json` / 扩展 `.mesh.json`；有 skin/anim/morph 的模型输出**层级场景**（其余模型逐字节不变） |
| 渲染 | `modules/render/gfx/src/gfx.cpp` | 顶点布局 2/3（48+24 / 56+24B）、4 个顶点入口、调色板 storage buffer（group1 binding 23）、阴影皮肤变体、`update_mesh` |
| shader | `assets/builtin/shaders/pbr/00_types.wgsl` + `40_vertex.wgsl` | `skin_palette` binding + `vs_skin/vs1_skin/vs_shadow_skin/vs_shadow1_skin` |
| 运行时 | `modules/runtime/src/runtime.cpp` | `.skel/.clip/.mesh(morph)` 解析与缓存、层级世界矩阵、动画采样、派生姿态、调色板、CPU morph、`MAQUE_ANIM`/`MAQUE_SKIN` 诊断开关 |
| 测试 | `tests/gltf_skin_e2e.sh`、`tests/gltf_anim_e2e.sh`、`tests/gltf_morph_e2e.sh`、`tests/data/gltf/{skinned,animated,morph}.gltf`、`samples/player/CMakeLists.txt` | 新 e2e（结构断言 + 像素/确定性断言） |
| 迁移 | `scenes/khronos/<26 个动画模型>`、`manifest.json`、`README.md`、`reports/khronos-comparison/`、`docs/31` | 定向重跑 + 预览/对比/README 更新，差距文字改为"动画已支持" |

---

## 1. 坐标 / 语义总约定（不变量）

1. **节点树是唯一真源**：`.scn` 的嵌套 `children` 由 `load_entity` 递进加载，`Entity::parent` 生效；
   动画/蒙皮模型的场景从"平级 + 世界烘焙"改为"glTF 节点树 1:1"。
2. **3D 世界矩阵每帧由节点树派生**：`Entity::world_m[16]`（列主序）= `parent.world_m * local`；
   平级场景 `world_m == local`，旧场景零漂移。
3. **动画采样是 `(time, clip)` 的纯函数**，不写回 `scene.Transform3D`（Euler 有损/万向锁）；
   采样结果只写"本帧派生姿态表"（`Engine::anim_pose`），`time` 是可快照的标量。
4. **绑定姿态不变量**：`palette[j] = world(joint_j) * inverse_bind_j`；bind pose 下 `palette ≈ I`。
5. **蒙皮网格忽略节点世界变换**（glTF 规范）：蒙皮 mesh 实体放在场景根、local = identity，
   `render.MeshRenderer.skin_path` 指向 `.skel.json`。
6. 单位/手性沿用现有引擎：右手系、Y 向上、相机看 -Z；矩阵列主序；WebGPU NDC z∈[0,1]。
7. **确定性**：所有遍历按实体 id 升序、轨道按声明顺序；采样不引入随机/墙钟；同一场景同一帧两次截图逐字节一致。
8. 旧资产/旧场景零回归：无 `skin_path`、无 `clips`、无 `morph` 时行为与改动前逐字节一致。

---

## 2. 资产格式（全部为文本 JSON，`--compact` 时 6 位小数圆整）

### 2.1 `.mesh.json` 扩展（旧字段不变）

```jsonc
{
  "name": "...", "vertex_stride": 72|80, "vertex_layout": 2|3,
  "vertices": [[...]],            // 与旧版同宽（layout2=12 float / layout3=14 float）
  "indices": [ ... ],
  "bounds_min": [x,y,z], "bounds_max": [x,y,z],
  "materials": [ ... ],           // 冗余副本，运行时只读 .scn
  // ---- 以下全部可选；缺失 = 非蒙皮 / 非 morph ----
  "joints":  [[j0,j1,j2,j3], ...],   // 与 vertices 等长；u16 语义（以数字写出）
  "weights": [[w0,w1,w2,w3], ...],   // 与 vertices 等长；归一化（sum≈1）
  "morph": {
    "default_weights": [w0, ...],     // 长度 = targets 数（glTF mesh.weights，缺省 0）
    "targets": [ {"positions": [[dx,dy,dz], ...], "normals": [[dx,dy,dz], ...]} ]
  }
}
```

- `vertex_layout`：0=48B（旧）、1=56B（COLOR_0/TEXCOORD_1）、**2=48+24B（蒙皮）**、**3=56+24B（蒙皮+布局1）**。
  JSON 里 `vertices` 行宽仍按 12/14，蒙皮数据在独立 `joints`/`weights` 数组 → 运行时打包成 GPU stride：
  - layout2：`pos@0 | normal@12 | tangent@24(未用) | uv0@40`（48B）+ `joints u16x4@48 | weights f32x4@56`（24B）= **72B**
  - layout3：`pos@0 | normal@12 | color@24 | uv0@40 | uv1@48`（56B）+ `joints@56 | weights@64` = **80B**
- `TANGENT` morph 增量忽略（shader 用屏幕导数构造切线基）；targets 顺序 = glTF 顺序，上限 8（超出跳过 + 告警）。

### 2.2 `.skel.json`（骨架，写成 `<out>/assets/skin/<mesh_name>.skel.json`）

```jsonc
{"version": 1, "name": "<node name>",
 "joint_names": ["<scene entity name>", ...],     // 与 inverse_bind 等长；名字 = 场景内唯一节点名
 "inverse_bind": [[16 floats 列主序], ...]}
```

- 每个 `(引用 skin 的节点)` 一份；该节点的所有 primitive 共享。
- 缺失 `inverseBindMatrices` 时写单位矩阵。

### 2.3 `.clip.json`（动画片段，写成 `<out>/assets/anim/<clip>.clip.json`）

```jsonc
{"version": 1, "name": "clip", "duration": 3.42,
 "tracks": [
   {"node": "<scene entity name>", "path": "translation"|"rotation"|"scale"|"weights",
    "interpolation": "LINEAR"|"STEP",
    "times": [t0, t1, ...],
    "values": [[...], ...]}   // translation/scale: [x,y,z]；rotation: [x,y,z,w]；weights: [w0..wn]
 ]}
```

- 轨道顺序确定性：按 (node 名, path 序 translation<rotation<scale<weights, 原 channel 序) 排序。
- `duration` = 所有 sampler input 的 max；`TIMES` 升序。
- `KHR_animation_pointer` 通道跳过并汇总计数（stderr）。
- CUBICSPLINE：取每个关键帧的 value（不取切线），按 LINEAR 近似并 stderr 计数。

### 2.4 路径约定

- `.skel.json` / `.clip.json` 与 `.mesh.json` 一样是**派生资产**，放 `scenes/khronos/<Model>/assets/skin|anim/`
  （`.gitignore` 已忽略 `scenes/khronos/*/assets/`，不入库，可由 importer 确定性重生成）。
- `mesh_path` / `skin_path` / `clips[]` 都**相对场景目录**（运行时 VFS 根 = 场景目录）。

---

## 3. 场景输出（`tools/gltf_import.py --scene`）

### 3.1 两种模式（自动判定，保持旧输出零漂移）

`hierarchy = bool(js.get("skins")) or bool(js.get("animations")) or any(prim.targets)`。
- `hierarchy == False`：**现有平级输出逐字节不变**（保护 148 模型的资产与 e2e 漂移断言）。
- `hierarchy == True`：层级输出（下面规则）。

### 3.2 层级输出规则

1. 每个 `glTF node` → 一个实体：`components=[scene.Transform3D{local=node 本地 TRS}]`，
   节点级组件（`render.Light` 等）仍按旧逻辑生成在**独立根实体**（世界变换烘焙，避免干扰层级）。
2. 节点名：`node.name or "node<idx>"`，全场景**唯一**（重名追加 `#2/#3`）——动画轨道/骨架按名字引用。
   `KHR_node_visibility.visible=false` 的节点及整棵子树跳过（沿用旧逻辑）。
3. 带 `mesh` 的节点：每个 primitive 生成一个**子实体**（parent=节点实体）：
   - `scene.Transform3D` 显式 identity；
   - `render.MeshRenderer{mesh_path:"assets/<mesh>.mesh.json"}`；
   - 材质组件（`asset.PbrMaterial`）同旧逻辑。
   - 实体名 `<node_name>_<prim_index>`。
4. 蒙皮 primitive：mesh 子实体**放到场景根**（parent=-1）、identity Transform3D、`MeshRenderer.skin_path`；
   其 glTF 节点实体仍然存在（保留层级供关节引用）。
5. 场景根实体（与旧版同样放在 `entities` 顶层）：`camera`、灯光/`sun`、`anim_player`（仅当有 clips）。
   `anim_player` 组件：
   ```jsonc
   {"type": "scn.AnimationPlayer", "clips": ["assets/anim/<c0>.clip.json", ...],
    "clip_index": 0, "playing": true, "loop": true, "speed": 1.0, "time": 0.0}
   ```
6. 相机取景：仍然按**世界 AABB**（节点世界矩阵 × mesh 局部 bounds 8 角点）自动拟合；
   层级输出不改变取景数学（蒙皮 bind pose 近似）。
7. 场景 JSON 嵌套：`{"name": ..., "components": [...], "children": [...]}`（顺序 = glTF 遍历顺序）。

---

## 4. Schema / ABI（append-only）

### 4.1 `renderer.yaml`

```yaml
    - record: skin                 # 新记录（调色板）
      fields:
        - { name: palette,     type: "const float*" }   # joint_count × 16 列主序 mat4
        - { name: joint_count, type: uint32_t }
    # draw3d 表尾追加：
        - { name: skin,        type: int32_t }          # frame3d.skins 下标；-1 = 非蒙皮
    # frame3d 表尾追加：
        - { name: skins,       type: "const mq_renderer_skin*" }
        - { name: skin_count,  type: uint32_t }
    # methods 表尾追加：
    - fn: update_mesh
      returns: int32_t
      args: [ {name: self, type: self}, {name: id, type: int32_t},
              {name: vertices, type: "const void*"}, {name: vertex_bytes, type: uint64_t} ]
      doc: "更新网格顶点（CPU morph；大小必须与创建时一致，stride/layout 不变）；0=成功"
```

`mesh_gpu` **不改**（蒙皮顶点与基础顶点同 buffer，由 layout 2/3 表达）。

### 4.2 `render.yaml` / `scn.yaml`

```yaml
# render.MeshRenderer 表尾
- { name: skin_path, type: string, default: "" }   # .skel.json 相对路径；空 = 非蒙皮
# scn.AnimationPlayer 表尾
- { name: clips,      type: "string[]" }           # 非空 = glTF 片段（相对场景目录）
- { name: clip_index, type: int32_t, default: 0 }
```

---

## 5. 渲染层（gfx + shader）

### 5.1 顶点布局（主 pass 与阴影 pass 都要）

| layout | stride | 主 pass 入口 | 属性（location:offset） |
|---|---|---|---|
| 0 | 48 | `vs` | pos:0@0, normal:1@12, uv0:2@40 |
| 1 | 56 | `vs1` | + color:3@24, uv1:4@48 |
| 2 | 72 | `vs_skin` | 0/1/2 同上 + joints:5@48(u16x4), weights:6@56(f32x4) |
| 3 | 80 | `vs1_skin` | 0/1/2/3/4 同上 + joints:5@56, weights:6@64 |

| layout | 阴影入口 | 阴影属性 |
|---|---|---|
| 0 | `vs_shadow` | pos:0@0, uv:2@40 |
| 1 | `vs_shadow1` | pos:0@0, color:1@24, uv:2@40, uv1:3@48 |
| 2 | `vs_shadow_skin` | pos:0@0, uv:2@40, joints:5@48, weights:6@56 |
| 3 | `vs_shadow1_skin` | pos:0@0, color:1@24, uv:2@40, uv1:3@48, joints:5@56, weights:6@64 |

- 管线缓存：`p3dMatPipeline[2][10][2][3][2][4]`、`shadowPipeline[4]`（`mi` = MSAA 1x/4x）。
- `r_create_mesh` 校验：layout 0→48、1→56、2→72、3→80；layout≥2 时 `skinned=true`。

### 5.2 调色板（group1 binding 23）

- group1 BGL 追加第 24 项：`binding=23`、`visibility=Vertex`、`buffer.type=ReadOnlyStorage`、`minBindingSize=64`。
- 设备级 dummy：64B 单位 mat4（非蒙皮 draw 绑它；shader 不读它，保证 bind group 始终合法）。
- 蒙皮 draw：每帧为每个蒙皮 draw 建 storage buffer（`Storage|CopyDst`，大小 = `joint_count*64`），
  在阴影 pass 与主 pass 里各绑一次，**提交后释放**（与现有 per-draw uniform 同生命周期）。
- `MQ_SKIN_MAX_JOINTS = 256`（本批最大 24）；`draw.skin` 越界/`joint_count=0` → 退回非蒙皮路径 + 告警一次。

### 5.3 shader

```wgsl
@group(1) @binding(23) var<storage, read> skin_palette: array<mat4x4f>;
fn skin_matrix(joints: vec4u, weights: vec4f) -> mat4x4f {
  return weights.x * skin_palette[joints.x] + weights.y * skin_palette[joints.y]
       + weights.z * skin_palette[joints.z] + weights.w * skin_palette[joints.w];
}
```
`vs_skin*`：`wp = dr.model * (skin * vec4f(pos,1))`；法线用 `mat3x3f(skin)`（非均匀缩放近似，本批无）。
阴影 `vs_shadow*_skin`：`cam.light_vp * dr.model * (skin * pos)`。

### 5.4 `update_mesh`

- 校验：mesh 存在、`vertex_bytes == 创建时字节数`；`wgpuQueueWriteBuffer(vbo)`；不改 index/stride。
- 用途：CPU morph（顶点数小：本批 3/24/30/1528）。

---

## 6. 运行时（runtime.cpp）

### 6.1 数据与解析

- `Entity` 增加 `float world_m[16]`（初始 identity）。
- `load_mesh`：解析 `joints/weights/morph`；按 layout 2/3 打包 interleaved 顶点（joints 转 u16、weights 原样）；
  `morph` 网格额外保存 `base_vertex_bytes`（打包后的基础顶点）+ targets/default weights，创建后立即应用默认权重。
- `load_skeleton(path)`：解析 `.skel.json` → `{joint_names, inverse_bind}`，按 path 缓存。
- `load_gclip(path)`：解析 `.clip.json` → `{duration, tracks[{node,path,interp,times,values,comps}]}`，按 path 缓存。
- 名字解析：`setup_3d` 建 `entity_by_name`（场景内唯一名 → id；重名取最小 id + 告警一次）。

### 6.2 动画采样（`anim_step`，在每模拟步调用）

- 仅处理 `clips` 非空的 `scn.AnimationPlayer`（旧 `clip` 通路完全不变）；按实体 id 升序、轨道声明序。
- 播放头：`time += dt*speed`；`loop` → 取模；否则 clamp 到 `duration`；写回 `time`/`duration`。
- 采样（纯函数）：定位 `k` 使 `t∈[keys[k], keys[k+1])`；`STEP` 取 `k`；`LINEAR` 线性；
  quat 先按 `dot<0` 翻转再 slerp（`dot>0.9995` 用 nlerp）；越界 clamp。
- 结果写 `Engine::anim_pose[entity]`：`{has_pos/rot/scale, vec3/quat, has_weights, weights[]}`；
  `path=="weights"` 时写该节点下所有 morph mesh 实体的权重覆盖。
- `MAQUE_ANIM=0`：跳过采样（保持 bind pose），用于 A/B 对照。

### 6.3 世界矩阵 / 抽取（`extract_3d`）

- 每帧 `flatten_world3d()`：与 `flatten_world` 相同的确定性遍历；local = 组件 TRS 叠加 `anim_pose`
  （有 rotation 轨道用 quat → `m4_from_quat_trs`，否则 `m4_from_trs`）；`world_m = parent.world_m * local`。
- 相机/灯光：用 `world_m` 的平移/旋转（根节点下与旧 `trs` 完全一致）。
- mesh draw：`dr.model = world_m`；蒙皮 mesh 的 `dr.model = identity`、`dr.skin = <skins 下标>`。
- 调色板：`palette[j] = world_m(joint_j) * ibm_j`（列主序）；`joint_count` 写入 `frame3d.skins`。
- `MAQUE_SKIN=0`：调色板填 identity（`joint_count` 保留）→ bind pose 与关闭前逐像素一致；用于 e2e 不变量。
- `MAQUE_RENDER3D_INFO=1`：打印 `skin path=... joints=N`、`clip path=... tracks=N duration=...`、
  `anim entity=... pose=pos/rot/scale/weights` 等装配/首帧诊断。

### 6.4 morph

- 每个 morph mesh 实体：权重 = 动画覆盖 > 默认权重；权重变化时 CPU 重算顶点
  （`pos + Σ w_i·Δp_i`，`normal + Σ w_i·Δn_i` 后归一化），调 `update_mesh`；权重不变不重算。
- 同一 mesh 被多实体共享：首实体生效 + 告警一次（本批 morph mesh 均唯一）。
- 上限：每 mesh targets ≤ 8（导入器已截断/告警）。

### 6.5 缓存/清理

- `clear_asset_caches()` 一并清 `gclips/skins/morph_states/entity_by_name`（切场景资源基准，docs/32 §5.1）。
- 热重载/场景重载后 `setup_3d` 重建名字表与 skin 解析。

---

## 7. 验收（可断言）

1. `ctest -R 'gltf_import_e2e|gltf_skin_e2e|gltf_anim_e2e|gltf_morph_e2e|render3d_scene_e2e|pbr_regression_e2e|determinism'` 全绿。
2. 真实模型抽检（headless `--shot-seq`）：
   - S1 bind pose 正确：`Fox/CesiumMan/BrainStem/RiggedFigure/RiggedSimple/SimpleSkin/RecursiveSkeletons`
     无 `mesh=-1`、非背景占比正常；`MAQUE_SKIN=0` 与默认截图在 bind pose 场景逐像素一致（fixture 断言）。
   - S2：`BoxAnimated/AnimatedCube/AnimatedTriangle/InterpolationTest/VirtualCity` 第 1/30/60 帧像素不同、
     同帧两次 SHA256 一致。
   - S3：`SimpleMorph/MorphPrimitivesTest/AnimatedMorphCube/MorphStressTest` 默认权重生效、动画权重变化。
3. 导入确定性：同一 glTF 两次导入输出逐字节一致（含 `.skel/.clip`）；无动画模型输出与旧版逐字节一致。
4. 旧场景零回归：无 skin/anim/morph 的 148−30 个模型 `.scn`/预览不变。

## 8. 迁移与文档

- 定向重跑：26 个动画 + 7 骨骼 + 4 morph 的并集（约 30 个模型）`python -u tools/migrate_gltf_models.py --only ... --force`；
  批量完成后再全量 `--reuse-preview` 刷新索引/报告。
- `tools/migrate_gltf_models.py`：`known_gaps()` 更新（动画/骨骼/morph 不再列为缺失；pointer 保留）；
  manifest 增 `animation` 统计字段（clips/skins/morph）。
- 文档：`docs/31 §5.4/§10.3/§11` 状态更新、`AGENTS.md` 增动画约定、`plans/21` 标注"已落地（plans/22）"。

## 9. 文件所有权 / 并行规则

- `tools/gltf_import.py` + `tests/data/gltf/*` + 迁移工具：importer 负责人。
- `modules/render/gfx/src/gfx.cpp` + `assets/builtin/shaders/pbr/*`：renderer 负责人。
- `modules/runtime/src/runtime.cpp`：runtime 负责人。
- `modules/core/data/schema/*` + `generated/*`：Lead 统一改/生成，其他人只读。
- `tests/*.sh`、`samples/player/CMakeLists.txt`、文档：测试/Lead。
- **构建串行**：只有 Lead 跑 `ninja -C build-win`（同 build 目录并发会互相踩）；实现者用
  `python -m py_compile` / `node parser.js --check` / WGSL 人工审查做自检。
