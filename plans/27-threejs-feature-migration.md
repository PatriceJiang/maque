# 27 · three.js 常用功能迁移评估（能力矩阵 · 命名对照 · 路线图）

> 上游：[18](18-pbr-3d.md)（PBR/3D 基础设施）、[19](19-threejs-materials.md)（材质族迁移）、
> [20](20-pbr-audit.md)（PBR 审计）、[23](23-runtime-gltf-loading.md)/[24](24-runtime-gltf-capabilities.md)（glTF 直读）。
> 基准：three.js **r188dev**——`src/Three.js`、`Three.Core.js`、`geometries/Geometries.js`、
> `materials/Materials.js`、`extras/curves/Curves.js` 的导出清单，以及 `examples/jsm` 的
> controls / postprocessing / loaders。
> 参考：<https://github.com/mrdoob/three.js/blob/dev/src/Three.Core.js>、
> <https://github.com/mrdoob/three.js/blob/dev/src/geometries/Geometries.js>、
> <https://github.com/mrdoob/three.js/blob/dev/src/materials/Materials.js>、
> <https://github.com/mrdoob/three.js/blob/dev/src/constants.js>。
> 性质：**评估 + 路线图**，不是又一份接口声明；所有建议字段一律**尾部追加 + 默认关闭**。
> 不移植 three.js 源码，只迁移**能力语义**与**可检索的命名**。

## 0. 结论（评审先看这十条）

1. **能迁移的是能力语义，不是 API 形状。** three.js 是"对象即 API"（`mesh.position.x = 1`、`scene.add(mesh)`、
   `object.traverse()`）；maque 是"节点树 + 组件数据 + 动词 API"（`Transform3D` + `node_*`）。
   逐字照搬会绕开反射、持久化、确定性与控制面，正确做法是
   **three.js 概念名 → maque 组件字段 / 枚举 / 脚本动词**（§3）。
2. **schema 面已经很大，缺的是消费端。** `render.Billboard` / `Skybox` / `ReflectionProbe` / `Terrain` /
   `MotionStreak`、`asset.Material` / `Effect`、`render.MeshRenderer.mesh` / `material` 句柄都已在
   `modules/core/data/schema/` 声明，但 runtime/gfx **没有消费路径**（无 `component<render::X>` 引用、
   无场景使用；`mesh`/`material` 句柄也不在提取里读）。**声明 ≠ 已实现**，迁移前必须先确认落点。
3. **最缺的"常用功能"集中在 10 处**：几何图元、雾、色调映射多模式、3D 点/线、3D 精灵/Billboard、
   InstancedMesh、LOD、AnimationMixer 混合、后处理链、3D 控制器；另有视锥剔除与多光源阴影两个性能项。
4. **首批（M1）只做 5 件**：几何图元、雾 + 色调映射（同一个 atmosphere 包）、变换/朝向 API、
   补 Billboard 消费、（可选）视锥剔除。它们都不改 3D 管线拓扑，不动模拟状态。
5. **命名规则**：`PascalCase` 类型/枚举值，`snake_case` 字段，动词 API 用 `node_*`/`scene_*`，
   资源用路径字符串 + `*_path` / `*_texture`；枚举数值尽量与 three.js 常量对齐（`MaterialSide` 已如此）。
6. **值类型是隐藏前置条件**：three.js 大量返回 `Vector3/Quaternion/Color`，而 `api_list` 只支持标量/字符串
   （[docs/21](../docs/21-接口声明与绑定生成.md) 第 2 节；"具名 struct 值传递"在其后续项里）。
   `getWorldPosition`/`localToWorld` 这类 API 要么先补 codegen 编组，要么只能返回 JSON 字符串。
7. **ABI 策略**：雾/色调映射属于"每帧大气参数"，建议在 `mq_renderer_frame3d` **尾部**追加
   `mq_renderer_atmosphere` 并升 `renderer.v5`；旧字段偏移不变，runtime 探测版本降级。
8. **明确不迁移**：TSL/NodeMaterial、`ShaderMaterial`（GLSL 字符串）、WebGLRenderer 后端、ObjectLoader、
   Canvas/Video/HTMLTexture、WebXR、BatchedMesh、`onBeforeCompile` 式 shader 打补丁（§5.4）。
9. **默认值必须复刻旧行为**：例：内建 Cube 当前是 ±0.6，图元默认尺寸取 **1.2** 而非 1.0；
   雾/色调映射默认关闭/ACES，旧场景像素零差异（PPM 逐字节 A/B）。
10. **验收 = 新 e2e + 旧场景零回归 + codegen 三门禁**（`codegen_drift` / `schema_imports` /
    `builtin_shaders`），不接受"只看能编译"。

## 1. 判定口径

| 代号 | 含义 | 典型工作 | 落点 |
|---|---|---|---|
| **A** | 已等价（只差文档/别名/糖） | 写对照表、补脚本别名 | 文档 / `host.yaml` |
| **B** | 可直接迁移 | schema 尾部字段、CPU 计算、提取期选择、离线工具 | `schema/` + `modules/runtime` |
| **C** | 需要新渲染机制 | gfx 管线拓扑、实例缓冲、多 shadow map、pass 链、ABI | `modules/render/gfx` + ABI |
| **D** | 不迁移 | 与引擎模型冲突，或依赖浏览器/DOM/JS 运行时 | — |

判定不按"three.js 有没有"，而按四条约束：
1. 是否违反"渲染只读、不回写模拟"（ADR-0005）；
2. 是否需要跨模块 ABI 变更（需要则至少 C，且必须版本化）；
3. 是否依赖浏览器/DOM/JS 对象语义（是则 D）；
4. 是否与"引擎即浏览器、游戏即资源"冲突（把 JS 类继承/回调搬进 C++ = D）。

## 2. 现状基线（maque 已有的 three.js 等价能力）

| three.js 能力 | maque 落点 | 状态 |
|---|---|---|
| Scene / Group / Object3D 层级 | 节点树 + `scene.Transform3D`（`modules/runtime/src/scene/tree.cpp`） | ✅ |
| Perspective / Orthographic Camera | `render.Camera3D.projection`（`render.yaml:235`） | ✅ |
| 多视图 / ArrayCamera | `scn.Scene.views` + `view3d.viewport` | ✅（VR 双眼；非 XR） |
| RenderTarget / WebGLRenderTarget | `render.RenderTexture`（相机离屏目标，`render.yaml:252`） | ✅ |
| Mesh + 10 种 Mesh*Material | `render.MeshRenderer` + `asset.PbrMaterial.shading_model`（`asset.yaml:251`） | ✅ |
| Directional / Point / Spot Light | `render.Light.type`（`render.yaml:337`，最多 8 盏） | ✅ |
| 方向光阴影 + PCF | 1024² shadow map + 3×3 PCF（`gfx.cpp` shadow pass） | ✅（单灯） |
| 环境贴图 / PMREM / Skybox | `scn.Scene.env_texture` / `env_cubemap` / `env_background`（`scn.yaml:188`） | ✅（场景级） |
| 后处理（Bloom/暗角/色差/分级/ACES） | `mq_renderer_post`（`maque_ifaces.h:247`）+ F4 面板 | 🟡（视图层、不可场景化） |
| glTF/GLB 直读 + 蒙皮/动画/morph | `render.MeshRenderer.mesh_path`（`gltf_model.cpp`） | ✅ |
| KTX2 / Basis、HDR / EXR、PNG/JPEG | image 模块 + `assets/env.cpp` | ✅ |
| 拾取（Raycaster） | `scene_pick` / `scene_raycast`（OBB 级，`scene/pick.cpp`） | 🟡（非三角形级） |
| Layers | `culling_mask`（相机 / 网格 / 灯光） | ✅（位掩码） |
| 2D：Sprite / UI / Mask / List / 图集 | docs/13、docs/17 | ✅（不在本次范围） |
| 音频 / Spine / 物理 / 导航 | audio / spine2d / physics / nav 模块 | ✅（各有语义，不按 three.js 音频迁移） |

**同名不同物**（迁移时最容易踩）：

| three.js | maque | 差异 |
|---|---|---|
| `Scene` | `scn.Scene` | three.js 是容器；maque 是**文档 + 场景配置**，节点树独立 |
| `Mesh` | `render.MeshRenderer` + `asset.Mesh` | three.js 对象 = geometry + material；maque 组件只持有**引用/路径** |
| `Material` | `asset.PbrMaterial` | 单组件承载 10 种模型；没有 `Material` 基类继承 |
| `Texture` | `asset.Texture` / 路径字符串 | 运行时用路径 + `#wrap=…&filter=…` 后缀，不是对象属性 |
| `Sprite` | `scn.Sprite` | maque 是**屏幕空间 2D**；three.js 是 3D billboard（不等价） |
| `Light` | `render.Light` | 无 Ambient/Hemisphere/RectArea 类型；环境用 `scn.Scene.env_*` |

## 3. 命名迁移规范

### 3.1 规则

| three.js 形态 | maque 形态 | 例 |
|---|---|---|
| 类 / 构造器（`MeshStandardMaterial`） | 类型 + 枚举值 / 组件 | `asset.PbrMaterial` + `shading_model = Standard` |
| 属性 `camelCase` | 字段 `snake_case` | `castShadow` → `cast_shadow`；`envMapIntensity` → `env_intensity` |
| 枚举常量 `UpperCamel` | 枚举值 `PascalCase`（数值尽量一致） | `DoubleSide` → `MaterialSide::Double`（2） |
| 方法 `verbNoun()` | 脚本动词 `node_*` / `scene_*` / 组件字段 | `lookAt()` → `node_look_at()` |
| 资源对象（`texture`） | VFS 路径字符串 + `*_texture` | `base_color_texture` |
| 几何/模型对象 | 资产路径 + `*_path` | `mesh_path` |
| 构造参数 | 组件字段（单位写进 `attrs.unit`） | `fov` → `fov_y`（deg） |
| 布尔 | 正向命名 | `receive_shadow`，不用 `no_shadow` |
| 缩写 | 跟随引擎既有拼写 | `Lod`（`TerrainLodMode`）、`Id`；组件名可用 `LOD` 保留检索性 |

补充：
- **不引入 `THREE.` 前缀**，也不建 `three` 命名空间；检索靠文档与本表的"three.js 名"列。
- **不搬运继承**：`MeshBasicMaterial extends Material` 在 maque 里是 `shading_model` 的一个值。
- **数值对齐 three.js 的枚举**（`MaterialSide`、`ToneMapping`、`LoopMode`），便于把 three.js 场景/工具的输出直接映射；
  不追求逐字类名一致（`BoxGeometry` → `MeshGeometry::Cube` 是刻意选择：引擎既有术语是 cube）。

### 3.2 冲突与例外

| 冲突 | 处理 |
|---|---|
| `Scene` 已被 `scn.Scene` 占用 | three.js 的 `scene.add()` 语义落在**节点树**，不新增 `Scene` API |
| `Mesh` / `Material` / `Texture` 已是资产类型名 | 运行时对象叫 `MeshRenderer`；材质组件叫 `PbrMaterial`；贴图用路径 |
| `Sprite` 已被 2D 占用 | three.js Sprite 另叫 `render.Billboard` + Plane 图元（不抢 `Sprite`） |
| `Object3D.visible` | 节点 `active`（含子树）与 `MeshRenderer.visible`（仅自身）是**两个**语义，不能合并 |
| `Object3D.layers` | `culling_mask`（位掩码）语义等价，但名字不同；文档给对照，不新增 `layers` |

### 3.3 值类型编组（前置基础设施）

`api_list` 目前只支持标量/`string`/`bytes`（docs/21 §2），而 three.js 的高频 API 全是向量：

```js
// three.js
const p = new THREE.Vector3();
mesh.getWorldPosition(p);
mesh.localToWorld(p);
```

建议先补 codegen（docs/21 §后续项"具名 struct 值传递"）：
- `returns: math.Vec3` → JS 返回 `{ x, y, z }`（Lua 返回 table）；
- 参数接受 `{x,y,z}` **或** 3 个 float（重载在生成期展开）。

在此之前，脚本侧只能：
- 用 `node_get_number(id, "local", "position.x", …)` 逐分量读；
- 或新 API 返回 JSON 字符串（与 `global_keys` 一致，但不适合热循环）。

**结论：M1 的"变换/朝向 API"应连带做 codegen 值类型，否则命名迁移只能做一半。**

## 4. 能力矩阵

> 判定：**A** 已等价 · **B** 可直接迁移 · **C** 需新机制 · **D** 不迁移。
> "落点"列给的是建议命名，不是已实现字段。

### 4.1 场景图 / Object3D

| three.js | maque 现状 | 判定 | 迁移落点（命名） |
|---|---|---|---|
| `Object3D` / `Group` / `children` | 节点树 + `node_child_*` / `node_set_parent` | A | 文档对照 |
| `position` / `rotation` / `scale` | `scene.Transform3D.local.*`（欧拉 XYZ、弧度、y-up 右手系） | A | 同名字段（多一层 `local`） |
| `visible` | 节点 `active` / `MeshRenderer.visible` | A | 已有 |
| `layers` | `culling_mask`（相机/网格/灯光） | A | 已有 |
| `lookAt()` | 无（相机内部有 look_at 数学） | **B** | `node_look_at(id, x, y, z[, up])` |
| `getWorldPosition()` / `localToWorld()` / `worldToLocal()` | 无脚本 API | **B** | `node_world_position` / `node_local_to_world` / `node_world_to_local`（依赖 §3.3） |
| `traverse()` / `add()` / `remove()` | `node_child_*` / `node_set_parent` / `node_destroy` | A | 已有 |
| `frustumCulled` | 无剔除（只按 `culling_mask`） | **B** | `MeshRenderer.frustum_culled`（默认 true），提取期剔除 |
| `LOD` | 无 | **B** | `render.LOD` + `render.LodLevel[]`（§5.2） |
| `InstancedMesh` | 仅 glTF `EXT_mesh_gpu_instancing` 展开为多 draw | **C** | `render.InstancedMesh`（需 ABI 实例缓冲） |
| `BatchedMesh` | 无 | D | 待 instancing 有真实需求再议 |
| `matrixAutoUpdate` / `onBeforeRender` | 无对应钩子（引擎每帧统一展平） | D | 用系统/组件替代 |

### 4.2 相机 / 控制器

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `PerspectiveCamera` / `OrthographicCamera` | `render.Camera3D.projection` | A | 已有 |
| `ArrayCamera` / 多视口 | `scn.Scene.views` + `viewport` | A | 已有 |
| `RenderTarget` / `WebGLRenderTarget` | `render.RenderTexture` | A | 已有 |
| `CubeCamera` | 无；环境用预烘焙 cube 贴图 | **C** | `render.CubeCamera`（6 面离屏）或先走离线 |
| `StereoCamera` | `views=2` + `eye_sep` | A- | 已有近似 |
| `OrbitControls` | 仅主相机内建右键环绕（`MAQUE_ORBIT`） | **B** | `render.OrbitCamera` 组件（含 target/distance/damping） |
| `FlyControls` / `FirstPersonControls` / `PointerLockControls` | 无 | **C** | 指针锁定 + 游戏侧控制器（先做输入捕获 API） |
| `MapControls` / `TransformControls` | 无 | C/D | 编辑器能力，非运行时优先 |

### 4.3 几何

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `BufferGeometry` + attributes | `asset.Mesh`（顶点/索引/法线/UV/切线/骨权重）+ `.mesh.json` | A | 已有（导入侧） |
| `BoxGeometry` / `SphereGeometry` / `PlaneGeometry` / `CylinderGeometry` / `ConeGeometry` / `TorusGeometry` | **只有内建 cube**（±0.6） | **B** | `MeshRenderer.geometry`（`MeshGeometry` 枚举）+ `geometry_size` + `geometry_segments` |
| `CircleGeometry` / `RingGeometry` / `CapsuleGeometry` | 无 | **B** | 同上枚举扩展 |
| `TorusKnot` / `Polyhedron` / `Lathe` / `Extrude` / `Shape` / `Tube` | 无 | **C** | 独立 `tools/geometry_*.py` 离线生成；不进运行时 |
| `EdgesGeometry` / `WireframeGeometry` | 仅全局 wireframe 调试模式（F4） | **C** | 依赖 3D 线拓扑（§5.2） |
| `computeVertexNormals` / `computeBoundingBox` / `merge` | 导入器侧有部分（法线/包围盒） | **B** | `tools/` 或 math API；运行时不做重计算 |
| `Curve` / `Path` / `CatmullRomCurve3` / `Bezier` | 无 | **B** | `math` 曲线类型 + 采样 API（纯 CPU，可确定性） |
| `InstancedBufferGeometry` | 无 | C | 随 InstancedMesh |

### 4.4 材质

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `MeshStandard/Physical/Basic/Lambert/Phong/Toon/Normal/Matcap/Depth/Shadow` | 10 个 `shading_model` | A | 已有（plans/19） |
| `LineBasic` / `LineDashed` / `PointsMaterial` / `SpriteMaterial` | 无 3D 线/点/精灵 | **C** | 随 3D 拓扑 / Billboard |
| `ShaderMaterial` / `RawShaderMaterial` | 无物体级自定义 shader；只有全屏 `ShaderPass`/`ShaderGraph` | **D**（GLSL）/ **C**（WGSL） | 若要做：`asset.Effect` 消费 + 新管线（M3+） |
| `color` / `map` / `normalMap` / `roughnessMap` / `metalnessMap` / `aoMap` / `emissiveMap` | 同名语义字段 | A | 已有 |
| `alphaMap` / `lightMap` / `bumpMap` / `displacementMap` | 无 | **C** | 逐项评估（displacement 需顶点阶段改造） |
| `wireframe`（材质级） | 仅全局调试模式 | **C** | `asset.PbrMaterial.wireframe`：需 per-material 管线 + 索引展开（全局模式已有 LineList 管线可复用） |
| `vertexColors` | glTF `COLOR_0`（layout1） | A | 已有 |
| `flatShading` | `flat_shading` | A | 已有 |
| `transparent` + `opacity` | `alpha_mode=Blend` + `opacity` | A | 已有 |
| `alphaTest` | `alpha_mode=Mask` + `alpha_cutoff` | A | 已有 |
| `side` | `side`（Front/Back/Double） | A | 已有 |
| `blending`（Additive/Multiply…） | 3D 只有 alpha 混合；2D 有 BlendMode | **C** | `asset.PbrMaterial.blend_mode` + 管线混合状态 |
| `toneMapped` | Basic 走 `output_direct`（glTF unlit 语义） | **B** | `tone_mapped` 字段（默认 true；unlit importer 写 false） |
| `fog`（材质级开关） | 无雾 | **B** | 场景级雾 + 可选 `fog` 开关 |
| `depthTest` / `depthWrite` / `polygonOffset` | 固定深度语义 | **C** | 阴影/pass 排序复杂化，暂不做 |
| `clippingPlanes` / `stencil*` | 无 | **C/D** | 需要全局裁剪面/模板缓冲，M3+ 再议 |
| TSL / `NodeMaterial` | 无 | **D** | 明确不做节点图编译器（plans/19 §1） |

### 4.5 灯光 / 阴影

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `DirectionalLight` / `PointLight` / `SpotLight` | `render.Light.type` 0/1/2 | A | 已有 |
| `AmbientLight` | `scn.Scene.env_color`（解析环境项） | A | 语义等价物；不新增灯型 |
| `HemisphereLight` | `env_sky_color` / `env_ground_color` | A | 已有 |
| `RectAreaLight` | 无 | **C** | 面光源 LTC 或解析近似，独立立项 |
| `LightProbe` / SH | 无 | D | 与"麻雀虽小"定位冲突，环境已有解析 IBL |
| `shadow.mapSize` | 固定 1024² | **B** | `render.Light.shadow_map_size`（256/512/1024/2048） |
| `shadow.bias` / `normalBias` | `shadow_bias`；无 normalBias | **B** | `shadow_normal_bias` |
| `shadow.radius` / `PCFSoft` | `ShadowMode::Soft`（3×3 PCF）；r188 已弃用 PCFSoft | A- | 已有近似 |
| `BasicShadowMap` / `VSMShadowMap` | 无 | **C** | gfx 新 shadow 路径 |
| 点光/聚光投影 | 只有 0 号方向光有 shadow map（`has_shadow_light`） | **C** | 多 shadow map（ABI 已留 `light.shadow_slot`） |
| 阴影相机拟合 | 按 mesh bounds 在光视空间拟合 | A | 已有（plans/20） |

### 4.6 纹理 / 环境

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `Texture`（wrap/filter/repeat/offset/rotation） | 路径后缀 `#wrap=…&filter=…`；UV 变换在材质上（`uv_scale/offset/rotation`） | A- | 建议：把 sampler 属性提升为 `asset.Texture` 资产字段 |
| `anisotropy` | 无 | **B** | `asset.Texture.anisotropy`（wgpu 支持） |
| `colorSpace` | 固定 sRGB 输出 + 贴图槽 sRGB 约定 | A- | 不做运行时切换（引擎单一色彩管线） |
| `CubeTexture` | `scn.Scene.env_cubemap`（6 面文件） | A | 已有 |
| `DataTexture` | 无脚本级原始像素上传 | **C** | `create_texture2` 已有，缺脚本面 |
| `CanvasTexture` / `VideoTexture` / `HTMLTexture` / `ExternalTexture` | 无 | **D** | DOM/浏览器语义，WASM 目标不成立 |
| `DepthTexture` | 内部有 shadow depth，无用户可采样深度 | **C** | 后处理/软粒子需要时再做 |
| `PMREMGenerator` | 装配期 CPU PMREM（GGX mip + irradiance） | A | 已有（`assets/env.cpp`） |
| `TextureLoader` / `ImageLoader` / `FileLoader` | VFS 路径 + image 解码 | A | 已有 |

### 4.7 渲染器 / 后处理

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `WebGLRenderer` / `WebGPURenderer` | gfx（WebGPU only） | A/D | 不提供多后端 |
| `renderer.toneMapping` | 固定 ACES（`tone_output`） | **B** | `scn.Scene.tone_mapping`（枚举值与 three.js 对齐） |
| `renderer.toneMappingExposure` | `render.Camera3D.exposure` | A | 已有（每相机） |
| `renderer.outputColorSpace` | 固定 sRGB OETF | A- | 不切换 |
| `renderer.shadowMap.enabled/type` | `MAQUE_SHADOW` + `Light.shadow_mode` | A- | 已有 |
| `renderer.setPixelRatio` | `scn.Scene.render_scale` | A | 已有 |
| `EffectComposer` + `RenderPass` / `ShaderPass` | 无 pass 链；`ShaderPass` 是**背景层全屏**，不是后处理 | **C** | `render.PostChain` + 多 pass（gfx pass graph） |
| `UnrealBloomPass` / `OutputPass` | 内建 post 有 bloom/ACES（F4 only） | **B** | 场景级 `render.PostEffect` 消费 `mq_renderer_post` |
| `SSAO` / `SSR` / `TAA` / `DOF` / `Outline` | 无 | **C** | 逐项独立，依赖深度/法线缓冲 |
| `renderer.info` | F4 stats + Tracy | A | 已有 |

### 4.8 动画

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `AnimationClip` / `KeyframeTrack` | glTF 片段（`scn.AnimationPlayer.clips` / `MeshRenderer.clip`） | A | 已有 |
| `AnimationMixer` / `AnimationAction.play/pause/stop` | 单 clip 播放（`playing/time/speed/loop`） | A- | 已有简化面 |
| `crossFadeTo` / `fadeIn/out` / `warp` | 无 | **C** | `AnimationMixer` 子集：双 clip 加权 + 时间缩放 |
| `AdditiveAnimationBlendMode` | schema 有 `anim.BlendMode`，runtime 未用 | **C** | 随混合一起做 |
| `LoopOnce/Repeat/PingPong` | `loop` bool；`anim.LoopMode` 有 PingPong（runtime 未接） | **B** | `loop_mode` 字段 |
| Morph targets / SkinnedMesh | glTF 已支持（CPU morph / 骨骼调色板） | A | 已有 |
| `AnimationObjectGroup` | 节点树天然共享 | A/D | 不需要 |

### 4.9 加载器

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `GLTFLoader` / `GLB` | 运行时直读（`mesh_path`） | A | 已有 |
| `KTX2Loader` / `DRACOLoader` / `MeshoptDecoder` | KTX2 ✅；Draco/meshopt ❌ | **C** | 优先离线解码（Python），运行时不做 |
| `RGBELoader` / `EXRLoader` | `.hdr` / `.exr` 环境贴图 | A | 已有 |
| `OBJLoader` / `FBXLoader` / `STLLoader` / `PLYLoader` / `ColladaLoader` | 无 | **B** | `tools/obj_import.py` 等 → `.gltf`/`.mesh.json`（不进运行时） |
| `TextureLoader` / `ImageLoader` / `FileLoader` | VFS + image | A | 已有 |
| `AudioLoader` | audio 模块 | A | 语义不同，按 docs/30 |
| `FontLoader` / `TextGeometry` | 2D 文本 ✅；3D 文字 ❌ | **D** | 用 2D Label 或离线网格 |
| `ObjectLoader` / `BufferGeometryLoader` | `.scn` + `.mesh.json` | D | 格式不兼容 |

### 4.10 数学 / 曲线 / 时间

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `Vector2/3/4`、`Quaternion`、`Matrix3/4`、`Euler`、`Color` | 同名 POD（`math.*`） | A | 已有 |
| `Box3` / `Sphere` / `Ray` / `Plane` / `Triangle` / `Line3` / `Frustum` | `Aabb` / `Ray` / `Plane`；其余缺 | **B** | `math.Box3`/`Sphere`/`Frustum` + 求交函数 |
| `MathUtils`（clamp/lerp/smoothstep/degToRad） | `clamp`/`lerp`/`saturate` | **B** | 补 `smoothstep`/`deg_to_rad` 等纯函数 |
| `Curve` / `CurvePath`（2D/3D） | 无 | **B** | `math` 曲线 + 采样（确定性） |
| `Clock` / `Timer` | `update(dt)` + `director_frame()` | A- | 建议 `time()`（累计模拟时间）糖 |
| `Spherical` / `Cylindrical` | 无 | **B** | 纯数学，随曲线一起 |

### 4.11 拾取 / 辅助

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `Raycaster.setFromCamera` + `intersectObjects` | `scene_pick` / `scene_raycast`（OBB 级） | A- | 已有；无 triangles/face/uv |
| 三角形级命中 / `face.normal` / `uv` | 无 | **C** | 需要 CPU 网格副本 + BVH，热路径成本高 |
| `AxesHelper` / `GridHelper` / `BoxHelper` / `CameraHelper` | 无 3D 线绘制 | **C** | 依赖 3D LineList（§5.2） |
| `Stats` / `lil-gui` | F4 + Tracy + Inspector | A | 已有 |

### 4.12 音频 / XR

| three.js | maque 现状 | 判定 | 迁移落点 |
|---|---|---|---|
| `Audio` / `PositionalAudio` / `AudioListener` | `audio.AudioSource` / `AudioListener` | A | 语义不同（bus/curve），按 docs/30 |
| `AudioAnalyser` | 无 | **C** | 需要实时 FFT，非核心 |
| `WebXR` | 无（只有多视图） | **D** | 与"引擎即浏览器"目标无关，独立立项才做 |

## 5. 迁移批次

### 5.1 M1：低成本、高感知（建议先做）

#### M1-1 几何图元（three.js `BoxGeometry` 族）—— ✅ 已落地

> **实现（2026-11）**：`modules/runtime/src/render/primitives.cpp`；schema 尾部字段；
> 样例 `scenes/pbr/primitives.scn`（含 `primitives.js` 拾取自检；预览 `primitives.png`）；
> 测试 `ctest -R 'primitives|geometry_primitives_e2e'`：
> 单测覆盖顶点布局/索引范围/法线单位长/包围盒包含/三角绕序朝外/分段夹紧；
> e2e 覆盖 6 个非缺省图元创建 + 轮廓结构断言 + 球/柱/锥拾取与空隙 miss + 两次渲染逐字节一致 +
> 旧 cube 场景不创建图元。
> 与提案的差异：Cube 固定 1×1/面（`geometry_segments` 只对其它 kind 生效）；Torus 与 three.js 一致
> 位于 XY 平面；Plane 单面（材质默认 Double）；UV 的 v 轴沿用引擎约定；缓存上限 64 组。

- **命名**（`render.MeshRenderer` 尾部追加）：

```yaml
- enum: MeshGeometry
  values:
    - { name: Cube,     value: 0 }   # three.js BoxGeometry（引擎既有术语 cube）
    - { name: Sphere,   value: 1 }
    - { name: Plane,    value: 2 }
    - { name: Cylinder, value: 3 }
    - { name: Cone,     value: 4 }
    - { name: Torus,    value: 5 }
    - { name: Capsule,  value: 6 }
# MeshRenderer：
- { name: geometry,          type: MeshGeometry, default: "MeshGeometry::Cube" }
- { name: geometry_size,     type: math.Vec3, default: "{1.2F, 1.2F, 1.2F}", attrs: { unit: m } }
- { name: geometry_segments, type: math.Vec3, default: "{32.0F, 16.0F, 8.0F}" }  # 取整使用；Cube 固定 1×1/面
```

- **语义**（`geometry_size` 的分量按 kind 解释，写进组件 doc）：

| kind | size.x | size.y | size.z | segments |
|---|---|---|---|---|
| Cube | 宽 | 高 | 深 | 固定 1×1/面（`geometry_segments` 忽略） |
| Sphere | 半径 | — | — | 经度/纬度 |
| Plane | 宽 | 高 | — | 宽/高分段 |
| Cylinder / Cone | 底/顶半径 | 高 | — | 径向/高度分段 |
| Torus | 主半径 | 管半径 | — | 径向/管向分段 |
| Capsule | 半径 | 圆柱段高 | — | 径向/半球分段 |

- **默认值**：`geometry_size=1.2` 与内建 cube ±0.6 一致 → 旧场景零变化。
- **集成点**（已按实际实现更新）：
  - schema：`modules/core/data/schema/render/render.yaml`（`MeshRenderer` 尾部）；
  - 生成器/缓存：`modules/runtime/src/render/primitives.cpp`（`build_geometry` + `primitive_bounds`
    + `Engine::load_primitive`，按 kind/size/segments 缓存，上限 64 组）；
  - 提取：`extract3d.cpp`（选择顺序 `mesh_path` > `geometry` > 内建 cube；缺省 Cube(1.2) 不进图元缓存）；
  - 资产清理：`systems/nav.cpp:clear_asset_caches`（切场景销毁图元句柄；`cube_mesh` 跨场景保留）；
  - 拾取：`scene/pick.cpp`（用 `primitive_bounds`，不再假设所有无路径网格都是 ±0.6）。
- **验收**：`scenes/pbr/primitives.scn` 七种图元一览（Cube 缺省 + 6 个非缺省）；`geometry_primitives_e2e`
  断言创建数量/轮廓可区分/拾取命中/miss；旧场景零变化；`codegen_drift` 绿。

#### M1-2 雾（three.js `Fog` / `FogExp2`）—— ✅ 已落地

> 实现（2026-11）：`scn.Scene.fog_*` + `mq_renderer_frame3d.atmosphere`（尾部追加）+
> gfx camera uniform `atmos0/atmos1` + `pbr` shader `apply_fog`；样例
> `scenes/pbr/atmosphere.scn`，测试 `ctest -R atmosphere_e2e`（近端不变/远端混色/确定性）。

```yaml
- enum: FogMode
  values:
    - { name: None,   value: 0 }
    - { name: Linear, value: 1 }   # THREE.Fog
    - { name: Exp2,   value: 2 }   # THREE.FogExp2
# scn.Scene 尾部：
- { name: fog_mode,    type: FogMode, default: "FogMode::None" }
- { name: fog_color,   type: math.Color, default: "{0.5F, 0.5F, 0.5F, 1.0F}" }
- { name: fog_near,    type: float, default: 1.0F,   attrs: { unit: m } }
- { name: fog_far,     type: float, default: 100.0F, attrs: { unit: m } }
- { name: fog_density, type: float, default: 0.01F }
```

- **语义**：`Linear` = `smoothstep(near, far, depth)`；`Exp2` = `1 - exp(-(density·depth)²)`；
  在 **tonemap 之前**按线性空间混合（与 three.js 一致）；`depth = length(eye - world_pos)`。
- **集成点**：`scn.yaml` → `targets.cpp:fill_frame_environment` → `mq_renderer_atmosphere`（ABI 尾部）
  → `gfx.cpp` camera uniform（912B 追加 32B）→ `10_utils.wgsl` 的 `tone_output` **与** `output_direct`
  两个统一出口（Basic/Normal/Shadow 走 `output_direct`）。**例外**：`fs_standard` 的 transmission
  折射分支提前 `return`，必须单独补一次雾（否则玻璃不参与雾）。
- **验收**：`scene_fog_e2e`：开/关同场景像素差、远处变暗、近处不变；默认关闭 → 旧场景零差异。

#### M1-3 色调映射（three.js `renderer.toneMapping`）—— ✅ 已落地

> 实现（2026-11）：`scn.Scene.tone_mapping`（None/Linear/Reinhard/Cineon/ACES，数值对齐 three.js）+
> `scene_tonemap()`；默认 ACES = 旧行为。AgX(6)/Neutral(7) 暂未实现（枚举未列，避免假支持）。
> 测试 `ctest -R atmosphere_e2e`（ACES vs Reinhard/None 亮度差）。

```yaml
- enum: ToneMapping          # 数值与 three.js 常量对齐（5=Custom 跳过）
  values:
    - { name: None,       value: 0 }
    - { name: Linear,     value: 1 }
    - { name: Reinhard,   value: 2 }
    - { name: Cineon,     value: 3 }
    - { name: ACESFilmic, value: 4 }
    - { name: AgX,        value: 6 }
    - { name: Neutral,    value: 7 }
# scn.Scene：
- { name: tone_mapping, type: ToneMapping, default: "ToneMapping::ACESFilmic" }
```

- **语义**：按 three.js `tonemapping_pars_fragment` 逐式移植（Linear/Reinhard/OptimizedCineon/ACES/AgX/Neutral）；
  exposure 仍用 `render.Camera3D.exposure`（每相机）。
- **兼容**：默认 `ACESFilmic` = 现有行为；`Normal/Depth/Shadow` 继续 `output_direct`；
  `Basic` 增加 `tone_mapped`（默认 true，glTF unlit importer 写 false，保持 Khronos 参考色）。
- **与后处理互斥**：`mq_renderer_post.tonemap` 也会做一次 ACES（F4 选项）。场景启用 `tone_mapping` 时
  post 的 tonemap 必须为 none，否则二次 tonemap；优先级写进 `engine_options_fill_post`（场景 > F4 > 默认）。
- **验收**：`tone_mapping_e2e`：同一 HDR 场景在 6 种模式下的亮度/高光裁剪差异可区分；ACES 默认像素零差异。

#### M1-4 变换 / 朝向 API（three.js `Object3D` 动词）—— ✅ 已落地

> 实现（2026-11）：`api_list` 值类型编组（`math.Vec3` ↔ JS `{x,y,z}` / Lua table / C guest `float*`）+
> `mq_script_register_value`（script ABI 5）；新增 `node_set_local_position/rotation/scale`、
> `node_local_position/rotation/scale`、`node_world_position/rotation`、`node_local_to_world`、
> `node_world_to_local`、`node_look_at`（含父链补偿；缺组件自动补挂 `scene.Transform3D`）。
> 样例 `scenes/transform_api/`，测试 `ctest -R node_transform_api_e2e`；细节见 [plans/36](36-racing-engine-hardening.md) §2。

```yaml
# host.yaml（api_list）建议：
- { fn: node_look_at,           ctx: true, args: [ { name: id, type: float }, { name: x, type: float }, { name: y, type: float }, { name: z, type: float } ] }
- { fn: node_set_local_position, ctx: true, args: [ { name: id, type: float }, { name: x, type: float }, { name: y, type: float }, { name: z, type: float } ] }
- { fn: node_set_local_rotation, ctx: true, args: [ { name: id, type: float }, { name: x, type: float }, { name: y, type: float }, { name: z, type: float } ] }  # 弧度，XYZ
- { fn: node_set_local_scale,    ctx: true, args: [ { name: id, type: float }, { name: x, type: float }, { name: y, type: float }, { name: z, type: float } ] }
# 依赖值类型编组（§3.3）后再加：
- { fn: node_world_position,  ctx: true, args: [ { name: id, type: float } ], returns: math.Vec3 }
- { fn: node_local_to_world,  ctx: true, args: [ { name: id, type: float }, { name: p, type: math.Vec3 } ], returns: math.Vec3 }
```

> 已有的 `node_set_position` 是**世界坐标**（2D 语义）；three.js 的 `position` 是**局部**坐标，
> 所以新动词显式带 `local_`，避免把两套语义混成一个名字。

- **语义**：`node_look_at` 与 three.js `Object3D.lookAt(worldTarget)` 一致：**世界坐标目标**，
  对象 -Z 指向目标，+Y 为上（可用可选 `up`）。
- **集成点**：`host.yaml`（api_list）→ `modules/runtime/src/api/node.cpp`；值类型需先改
  `modules/core/data/parser.js` 的类型白名单与 `maque_bindings.cpp` 生成逻辑（docs/21 §后续项）。
- **验收**：`node_transform_api_e2e`：三轴位置/旋转/缩放读写一致；`look_at` 后相机/网格朝向断言；
  与反射路径（`node_get_number("local.position.x")`）读数一致。

#### M1-5 补 Billboard 消费（schema 已声明）

- **命名**：沿用 `render.Billboard`（`render.yaml:425`），不新增字段。
- **语义**：提取期把 `MeshRenderer` 的 model 矩阵替换为朝向相机的 billboard 矩阵（`YAxisLocked` 默认），
  **不回写** `Transform3D`；阴影 pass 仍用原矩阵。
- **集成点**：`extract3d.cpp` 绘制循环（读 `mq_renderer_view3d.view` 的相机基向量）；
  `math3d.cpp` 增加 `billboard_matrix()`；无需 ABI 变更。
- **验收**：`billboard_e2e`：两个不同机位下卡片朝向变化、屏幕覆盖率稳定；模拟字段不变（快照对比）。

#### M1-6（可选）视锥剔除

- **命名**：`render.MeshRenderer.frustum_culled`（默认 true，对应 `Object3D.frustumCulled`）。
- **语义**：提取期用相机 view-proj 六平面剔除 OBB(AABB×world)；阴影 pass 用光锥单独剔除；
  纯提取期，不改节点状态。
- **集成点**：`extract3d.cpp`（每 mesh 循环前）、`runtime_math3d.h`（平面提取）。
- **验收**：`frustum_cull_e2e`：相机外物体 draw 数下降但画面像素不变；关闭开关后 draw 数恢复。

### 5.2 M2：渲染机制

| 项 | 命名 | 要点 | 工作量 |
|---|---|---|---|
| 3D 点/线 | `MeshRenderer.geometry = Lines/Points` 或独立 `render.LineRenderer` | gfx 增加 LineList/PointList 管线 + `mesh_gpu.topology` 尾部字段；`LineBasicMaterial`/`PointsMaterial` 映射到 `shading_model` | M |
| LOD | `render.LOD` + `render.LodLevel{ target(entity_ref), max_distance, hysteresis }` | 提取期按相机距离选层，只画选中子树；拾取同层 | S–M |
| InstancedMesh | `render.InstancedMesh`（`count`、`matrices`/`colors`，或引用子 MeshRenderer） | gfx 实例缓冲 + 顶点着色器 instance step；glTF EXT 目前展开为多 draw | L |
| 多光源阴影 | `render.Light.shadow_slot`（ABI 已留）+ `shadow_map_size` | gfx shadow map 数组 / spot 透视 / point cube | L |
| 后处理组件化 | `render.PostEffect`（场景级） | 先暴露现有 `mq_renderer_post`，再做 pass 链 | S（暴露）/ L（链） |
| 混合模式 | `asset.PbrMaterial.blend_mode`（Alpha/Additive/Multiply） | 3D 管线 blend state + 排序 | M |
| 贴图 sampler 属性 | `asset.Texture.wrap_u/v` / `anisotropy` | 把路径后缀升级为资产字段 | S |

### 5.3 M3：动画 / 资产 / 大件

| 项 | 命名 | 要点 |
|---|---|---|
| AnimationMixer 子集 | `scn.AnimationPlayer.cross_fade(to_clip, duration)` / `weights` | 双 clip 采样 + slerp/lerp；LoopMode 全量；additive 后置 |
| OBJ/FBX/STL 导入 | `tools/obj_import.py` 等 | 离线转 `.gltf`/`.mesh.json`，运行时零新依赖 |
| Draco/meshopt | 离线解码 | 运行时只吃解码后的 glTF |
| CubeCamera / ReflectionProbe | `render.CubeCamera` / 已有 `render.ReflectionProbe` 补消费 | 6 面离屏 + GGX 预过滤；移动端可关 |
| 3D 控制器 | `render.OrbitCamera`（含 target/distance/damping/limit） | 复用现有主相机 orbit 数学；输入走确定性输入流 |

### 5.4 追加项：3D 粒子系统 MVP（plans/28 的前置切片）—— ✅ 已落地

> 用户需求（2026-11）：支持 3D 粒子并尽量 GPU 加速。MVP 落**确定性模拟 + GPU 实例化渲染**：
> CPU 固定步长/引擎 RNG 模拟 → 48B/粒子实例数组（`frame3d.particles` + `particle_systems`）→
> gfx 实例化 billboard quad（每系统一次 draw；同 texture/blend/facing 可合并）。
> GPU 解析模拟（可选、不进 checksum）与 GPU morph 的可行性评估见
> [plans/28](28-gpu-morph-and-particles.md)。

- **命名**：`render.ParticleEmitter3D`（`rate/lifetime/speed/spread/direction/gravity/size_*/
  color_*/rotation*/texture/max_count/blend/facing/sort/culling_mask`）+ `render.ParticleField3D`
  （脚本外部驱动，`particles3d_upload` 上传 48B/粒子）；内置软边纹理 `builtin:/textures/particle_soft`。
- **实现**：`modules/runtime/src/systems/particles.cpp:particles3d_step`（模拟）+
  `extract3d.cpp` 粒子批次（实例数组/远→近排序/每系统 material）。
- **约束**：alpha/additive、不投影；不参与拾取/反射；模拟不进 checksum/`world_trail`
  （渲染开关 `MAQUE_PARTICLES=0`）。
- **样例/测试**：`scenes/pbr/particles3d.scn`（火焰 + 烟雾；预览 `particles3d.png`）；
  `ctest -R particles3d_e2e`（可见/可关/时间变化/确定性）。
- **Boids**：`scenes/boids/boids.scn`（JS 参考：空间网格 + `ParticleField3D`）与
  `scenes/boids/boids-webgpu.scn`（全 shader：ShaderGraph 4 pass、O(N²) 力、splat）；
  测试 `boids_e2e`（可见性/确定性 + GPU 240 帧性能预算）。
- **下一步（plans/28）**：可选 GPU 解析模拟（stateless，默认关、不进 checksum）。

### 5.5 M4：明确不做

| 项 | 原因 |
|---|---|
| TSL / NodeMaterial / `onBeforeCompile` | 节点图编译器与"小而可预测"冲突；已由 plans/19 §1 排除 |
| `ShaderMaterial` / `RawShaderMaterial`（GLSL 字符串） | 引擎只认 WGSL 模块；运行时编译 GLSL 不成立 |
| `WebGLRenderer` / 多后端 | 引擎明确 WebGPU only（AGENTS.md） |
| `ObjectLoader` / three.js 场景 JSON | `.scn` + 持久化是唯一格式 |
| `CanvasTexture` / `VideoTexture` / `HTMLTexture` | DOM/浏览器语义，WASM/Android 不成立 |
| `WebXR` | 非当前目标；多视图已覆盖 VR 渲染的一半 |
| `BatchedMesh` | 等 InstancedMesh 有真实需求再说 |
| `clippingPlanes` / `stencil` | 需要全局裁剪/模板状态，收益低 |
| `AudioAnalyser` | 非核心；需要时按 audio 模块加 |
| `matrixAutoUpdate` / 每对象回调 | 引擎每帧统一展平，无对象级回调模型 |

## 6. 与现有引擎的结合（集成契约）

### 6.1 数据流（新功能一律走这条）

```
schema/*.yaml  →  codegen（types/desc/bindings/control/abi）  →  生成物入库
     ↓
.scn 反序列化 / 脚本 API / 控制面 / Inspector
     ↓
装配期（IO/解码/网格生成/PMREM）—— 允许阻塞，不在模拟帧内
     ↓
帧内提取（extract_3d，只读，确定性）
     ↓
gfx（mq_renderer_v5，管线/绑定/绘制）
```

### 6.2 四条硬约束

1. **渲染只读**：three.js 的 `lookAt`/Billboard/LOD 选择都不得写回 `Transform3D`/`node.active`；
   LOD/Billboard 的结果只进 DrawList（`transient`）。
2. **尾部追加 + 默认关闭**：旧字段偏移不变；新枚举默认值 = 旧行为；默认关闭的功能必须有 A/B 像素门。
3. **ABI 版本化**：`mq_renderer_frame3d` 尾部新增 `mq_renderer_atmosphere`（雾/色调映射/未来大气），
   升 `MQ_RENDERER_API_VERSION 5` / `maque.renderer.v5`；runtime 读 `header.version` 降级。
   不要把新字段插进 `environment`/`post` 中间（会平移后续指针字段）。
4. **值类型先补 codegen**：M1-4 依赖 docs/21 的"具名 struct 值传递"；不做就只能返回 JSON，
   等于把 three.js 风格 API 降级成字符串协议。

### 6.3 命名门禁

- schema 字段只允许 `snake_case`（生成器可加校验）；
- 枚举值 `PascalCase`；与 three.js 常量数值对齐的项在 YAML 注释里写 `three.js XXX = n`；
- 所有 three.js 对照写进本文件 §4 / 附录 B，避免各模块各自造词；
- 新增字段必须在组件 doc 里写"three.js 等价物"，方便检索与迁移工具生成映射。

### 6.4 测试矩阵

| 层 | 门禁 |
|---|---|
| schema | `node parser.js --check`、`ctest -R schema_imports`、`ctest -R codegen_drift` |
| shader | `ctest -R builtin_shaders`（源码唯一真相 + 压缩包逐字节） |
| 运行时 | 新场景 e2e（像素/结构断言）+ 旧场景 PPM 逐字节 A/B |
| 确定性 | 新功能不进 checksum；开/关不影响模拟快照 |
| 平台 | 至少 Windows + WASM headless 冒烟（涉及 gfx 管线时） |

## 7. 验收标准（DoD）

- M1 每项：新增场景 + e2e（命名见 §5.1），默认关闭时旧场景像素零差异；
- `codegen_drift` / `schema_imports` / `builtin_shaders` 全绿，生成物入库；
- 全量 ctest 绿（至少不新增失败）；
- 新字段在 Inspector/反射中可读写（`node_get_number` 路径可用）；
- 文档：`docs/13` 补一节"three.js 对齐清单"，AGENTS.md 补一行入口；
- 性能：`MAQUE_TRACE_STATS` 对比基线无明显劣化（雾/色调映射为逐片元常数级）。

## 8. 风险

| 风险 | 对策 |
|---|---|
| 10 个 fragment 入口重复加雾/tonemap → 漏改 | 只改 `tone_output` / `output_direct` 两个统一出口；e2e 覆盖全部 10 个模型 |
| 默认值改观感（图元尺寸、Basic tonemap） | Cube 默认 1.2；`tone_mapped` 默认 true 但 unlit importer 显式 false；PPM A/B |
| ABI 追加破坏旧插件 | 只在 `frame3d` 尾部追加；版本 v5；旧字段偏移 `static_assert` 守卫 |
| 值类型编组改动面大（codegen 全量） | 先只支持 `math.Vec2/3/4` 返回值 + 3-float 参数，逐步扩 |
| "声明了但没实现"继续膨胀（Skybox 等） | 新计划只允许"声明 + 消费 + e2e"同一批合入；否则不写 schema |
| 移动端/WASM 预算 | 雾/tonemap 逐片元廉价；instancing/多阴影/后处理链按平台能力开关 |
| three.js 版本漂移 | 本文锁 r188；升级时只重跑 §4 矩阵，不追 API |

## 9. 附录 A：three.js 导出清单判定（按模块）

| 模块 | 代表导出 | 判定 | 说明 |
|---|---|---|---|
| core | `Object3D`、`Group`、`Layers`、`Clock`、`Timer`、`Raycaster` | A/B | 节点树/掩码已有；`lookAt`/世界变换/Clock 糖待补 |
| scenes | `Scene`、`Fog`、`FogExp2` | B | 场景已有；雾缺 |
| cameras | `PerspectiveCamera`、`OrthographicCamera`、`ArrayCamera`、`CubeCamera`、`StereoCamera` | A/C | 前三个已等价；CubeCamera 缺 |
| objects | `Mesh`、`SkinnedMesh`、`Bone`、`Skeleton`、`LOD`、`InstancedMesh`、`BatchedMesh`、`Line`、`Points`、`Sprite` | A/B/C | skin/morph 已有；LOD/线/点/实例缺 |
| geometries | `Box/Sphere/Plane/Cylinder/Cone/Torus/...` | B/C | 图元缺；高级几何走离线 |
| materials | 18 个导出（17 种材质 + `Material` 基类） | A/C/D | 10 种 mesh 模型已有；线/点/精灵材质缺；ShaderMaterial 不做 |
| textures | `Texture`、`CubeTexture`、`DataTexture`、`CompressedTexture`、`DepthTexture`、`VideoTexture` | A/B/C/D | KTX2/cube 已有；Data/Depth 缺；Video 不做 |
| renderers | `WebGLRenderer`、`WebGLRenderTarget`、`PMREMGenerator` | A/D | WebGPU only；RenderTexture/PMREM 已有 |
| lights | `Directional/Point/Spot/Ambient/Hemisphere/RectArea/LightProbe` | A/C/D | 前三 + 环境等价物；面光源/探针缺 |
| animation | `AnimationClip`、`AnimationMixer`、`AnimationAction`、`KeyframeTrack` | A/C | 单 clip 已有；混合缺 |
| loaders | `GLTFLoader`、`KTX2Loader`、`DRACOLoader`、`OBJLoader`、`FBXLoader` | A/B/C | glTF/KTX2 已有；其余离线或不做 |
| math | `Vector*`、`Quaternion`、`Matrix*`、`Box3`、`Sphere`、`Frustum`、`Curve` 族 | A/B | 基础 POD 已有；Box3/Sphere/Frustum/Curve 缺 |
| helpers | `AxesHelper`、`GridHelper`、`BoxHelper`… | C | 依赖 3D 线拓扑 |
| audio | `Audio`、`PositionalAudio`、`AudioListener`、`AudioAnalyser` | A/C | 前三个等价物已有 |
| addons | OrbitControls、EffectComposer、UnrealBloomPass、TransformControls、WebXR | B/C/D | 见 §4.2/§4.7 |

## 10. 附录 B：命名速查（three.js 片段 → maque）

```js
// three.js
const geo = new THREE.SphereGeometry(0.5, 32, 16);
const mat = new THREE.MeshStandardMaterial({ color: 0x88ccff, roughness: 0.4 });
const mesh = new THREE.Mesh(geo, mat);
mesh.position.set(0, 1, 0);
mesh.castShadow = true;
scene.add(mesh);
```

```json
// maque .scn（等价意图；geometry 字段为 M1 提案）
{ "name": "ball",
  "components": [
    { "type": "scene.Transform3D", "local": { "position": { "x": 0.0, "y": 1.0, "z": 0.0 } } },
    { "type": "render.MeshRenderer", "geometry": 1, "geometry_size": { "x": 0.5, "y": 0.0, "z": 0.0 },
      "cast_shadow": 1 },
    { "type": "asset.PbrMaterial", "base_color": { "r": 0.53, "g": 0.8, "b": 1.0, "a": 1.0 },
      "roughness": 0.4 }
  ] }
```

| three.js | maque |
|---|---|
| `mesh.position/rotation/scale` | `scene.Transform3D.local.position/rotation/scale` |
| `mesh.visible = false` | `node_set_active(id, 0)` 或 `MeshRenderer.visible=false` |
| `mesh.castShadow/receiveShadow` | `render.MeshRenderer.cast_shadow` / `receive_shadow` |
| `mesh.material.side = DoubleSide` | `asset.PbrMaterial.side = 2` |
| `material.transparent/opacity` | `alpha_mode=2` / `opacity` |
| `material.alphaTest` | `alpha_mode=1` / `alpha_cutoff` |
| `material.map = tex` | `base_color_texture = "path#wrap=repeat"` |
| `scene.fog = new Fog(c, n, f)` | `scn.Scene.fog_mode=1` + `fog_color/near/far`（M1） |
| `renderer.toneMapping = ACESFilmic` | `scn.Scene.tone_mapping=4`（M1） |
| `renderer.toneMappingExposure = 1.2` | `render.Camera3D.exposure = 1.2` |
| `light.shadow.mapSize` | `render.Light.shadow_map_size`（M2） |
| `light.shadow.bias` | `render.Light.shadow_bias` |
| `new OrbitControls(cam, dom)` | `render.OrbitCamera`（M3）或主相机内建环绕 |
| `raycaster.setFromCamera(m, cam)` | `scene_pick(x, y)` |
| `new GLTFLoader().load(url)` | `render.MeshRenderer.mesh_path = "model.glb"` |
| `new WebGLRenderTarget(w, h)` | `render.RenderTexture` |
| `object.lookAt(v)` | `node_look_at(id, x, y, z)`（M1） |
| `object.getWorldPosition(v)` | `node_world_position(id)`（M1，需值类型） |
| `clock.getElapsedTime()` | 累计 `update(dt)` 或 `director_frame()/60`（建议补 `time()`） |

## 11. 附录 C：可选 JS 兼容层（迁移辅助，不是引擎契约）

给从 three.js 迁过来的团队一层**纯脚本适配器**（docs/34 的包/模块机制，`maque:` 保留命名空间）：

```js
// 示意：maque/three-compat 包
export class Object3D {
  constructor(id) { this.id = id; }
  get position() { return vec3Proxy(this.id, "local.position"); }   // 读写经 node_get/set_number
  lookAt(x, y, z) { node_look_at(this.id, x, y, z); }
  get visible() { return node_active(this.id) !== 0; }
  set visible(v) { node_set_active(this.id, v ? 1 : 0); }
  add(child) { node_set_parent(child.id, this.id); return this; }
  traverse(fn) { for (let i = 0; i < node_child_count(this.id); ++i) { /* DFS */ } }
}
```

- **只做名词适配**：不模拟 `Matrix4` 缓存、不模拟 `matrixAutoUpdate`、不做对象级渲染回调；
- **不改变引擎语义**：`visible` 映射 `active`（含子树）会与 three.js 的"仅自身"不同 → 文档必须标注；
- **定位**：迁移期脚手架/原型；长期代码仍建议直接用 `node_*` + 组件字段（更少分配、更可诊断）。

## 12. Owner / 工作量

| 项 | Owner | 规模 |
|---|---|---|
| 几何图元、Billboard、LOD、视锥剔除 | `agent-render3d`（runtime 提取） | S–M |
| 雾、色调映射、材质/管线能力 | `agent-render3d` + `agent-renderer`（gfx） | M |
| 值类型编组、脚本动词 | `agent-core`（codegen）+ `agent-runtime` | M |
| InstancedMesh、3D 点/线、多阴影、后处理链 | `agent-renderer` | L |
| 动画混合 | `agent-render3d`（gltf_anim）+ `agent-animation` | M–L |
| 离线导入器（OBJ/FBX/Draco） | `agent-asset` | M |

> 建议顺序：**M1-1 → M1-2/M1-3（同一 atmosphere 包）→ M1-4 → M1-5 → M1-6**。
> 每项独立可回滚：schema 默认值 + gfx 版本探测 + e2e 门禁。
