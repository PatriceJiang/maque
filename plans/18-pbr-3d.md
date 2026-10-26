# 18 · PBR 材质系统与 3D 渲染（直接呈现）

> 上游：`docs/13`（渲染 WebGPU / 分层 / 资源与管线）、`docs/components/03-render.md`（渲染组件）、
> `docs/components/14-render-extra-3d.md`（Billboard/ReflectionProbe/Skybox/Terrain）、
> `docs/assets/01-material.md`（材质资产）、`docs/assets/03-texture.md`（纹理导入）、
> `docs/components/02-transform.md`（Transform3D）、`docs/22`（多视图/VR）、`docs/02`（ABI）、`docs/10`（确定性）。
> 决策（用户 2026-10-26 拍板）：① **直接呈现**（wgpu swapchain，不再经 CPU readback→SDL）；
> ② **离线 glTF/纹理导入**；③ **直接实现 schema 中已声明的 `render.*` 3D 组件**。
> 本计划遵循 `plans/_template.md`：接口与测试**先于**实现冻结。

## 1. 模块概述

- **职责**：把“可序列化的 3D 场景描述（相机/光源/网格/材质/环境）”翻译为 WebGPU 命令，直接呈现到窗口
  （`wgpuSurface*`+swapchain），并在 headless/`--shot` 下渲染到离屏目标 + readback；同时保持现有
  2D 图元/网格/文字/ImGui 覆盖层的像素级行为与测试不变。
- **所在层**：呈现层。**渲染只读消费模拟**（ADR-0005）：提取在 runtime，渲染在 `modules/render/gfx` 插件。
- **新增/改动模块**：
  - `modules/render/gfx`：ABI 新方法实现、3D 管线、pass 编排、surface/present、离屏回读；
  - `modules/platform`：暴露**原生窗口句柄**（Win32 HWND / X11 Window / Wayland wl_surface）；
  - `modules/runtime`：3D 提取（Transform3D/Camera3D/Light/MeshRenderer）、呈现路径切换、截图/抓帧；
  - `modules/ui/imgui`：直接呈现路径下走 **CPU 像素 → 2D 网格**的既有回退路径（不引入第二套 ImGui 后端）；
  - `asset` / schema：`PbrMaterial`、纹理导入选项、`Mesh` 规范化布局；
  - `tools/gltf_import.py`：离线 glTF/GLB → 引擎文本资产（确定性）。
- **非目标**：deferred/簇光照、GPU-driven、frame graph 重排、运行时 glTF 解析、HDR 显示器输出、OSX 原生窗口（后续）。

## 2. 接口层设计（先冻结）

### 2.1 版本化策略

- `renderer` 接口沿用 `version: 1`，**只在 vtable 表尾追加**（`struct_size` 只增不改，docs/02）。
  既有方法签名/记录**绝不修改**；旧插件按其 `struct_size` 截断使用。
- 宿主调用新方法前检查 `header.struct_size`（runtime 侧集中在一处 helper）。
- 新记录全部 POD、C 标量/定长数组；无 STL/异常/裸内部指针。

### 2.2 新记录（`modules/core/data/schema/render/renderer.yaml`）

```c
typedef struct mq_renderer_window {     /* 原生窗口（交给 wgpu 建 surface） */
    int32_t kind;      /* 0=none 1=win32 2=xlib 3=wayland */
    void*   display;   /* win32: HINSTANCE；xlib: Display*；wayland: wl_display* */
    void*   handle;    /* win32: HWND；xlib: Window；wayland: wl_surface* */
    void*   extra;     /* 预留（xcb connection / cocoa view） */
    int32_t width, height;   /* 绘制区（设备像素） */
} mq_renderer_window;

typedef struct mq_renderer_caps {
    int32_t max_texture_size, max_lights, max_samples;
    int32_t target_format;   /* mq_renderer_format：当前呈现目标格式 */
    int32_t depth_format;    /* 0=Depth32Float 1=Depth24Plus 2=Depth24PlusStencil8 */
    int32_t direct_present;  /* 1 = swapchain 直呈可用 */
    int32_t hdr;             /* 1 = RGBA16F 离屏可用 */
    int32_t reserved;
} mq_renderer_caps;

typedef struct mq_renderer_texture_desc {
    int32_t width, height;
    int32_t pixel_format;   /* 0=RGBA8 1=RGBA8_SRGB 2=RGBA16F 3=BC7(预留) */
    int32_t kind;           /* 0=2D 1=cube 2=equirect（cubemap/IBL 预留） */
    int32_t mip_levels;     /* 1 = 无 mip；data 内 mip 连续存放 */
    int32_t filter;         /* 0=nearest 1=linear(含 mip linear) */
    int32_t wrap_u, wrap_v; /* 0=repeat 1=clamp 2=mirror */
    const uint8_t* data;    /* RGBA8 各 mip 连续；cube 预留为 6 面 */
    uint64_t data_size;
} mq_renderer_texture_desc;

typedef struct mq_renderer_mesh_gpu {   /* 一次性上传的 3D 网格 */
    const void*    vertices;       /* 规范布局：pos3 normal3 tangent4 uv2（stride 48B） */
    uint64_t       vertex_bytes;
    uint32_t       vertex_stride;
    int32_t        vertex_layout;  /* 0=PBR(48B) */
    const uint32_t* indices;
    uint32_t       index_count;
    float          bounds_min[3], bounds_max[3];   /* AABB（剔除用） */
} mq_renderer_mesh_gpu;

typedef struct mq_renderer_view3d {
    float view[16], proj[16];   /* 列主序；WebGPU NDC z∈[0,1] */
    float eye[3];
    float viewport[4];          /* 目标像素 x,y,w,h */
    float exposure;
    int32_t clear_mode;         /* 0=skybox 1=color 2=depth 3=none */
    float clear_color[4];
    int32_t layer_mask;
} mq_renderer_view3d;

typedef struct mq_renderer_light {
    int32_t type;               /* 0=directional 1=point 2=spot */
    float pos[3], dir[3], color[3];
    float intensity, range, inner_cos, outer_cos;
    int32_t flags;              /* bit0 cast_shadow */
    int32_t shadow_slot;        /* -1 = 不投影；0..N cascades */
} mq_renderer_light;

typedef struct mq_renderer_pbr_material {
    float base_color[4];        /* 线性 */
    float emissive[3]; float metallic;
    float roughness; float normal_scale; float occlusion_strength; float alpha_cutoff;
    int32_t alpha_mode;         /* 0=opaque 1=mask 2=blend */
    int32_t flags;              /* bit0 double_sided */
    int32_t textures[5];        /* base_color / mr / normal / occlusion / emissive */
    float uv_scale[2], uv_offset[2];
} mq_renderer_pbr_material;

typedef struct mq_renderer_draw3d {
    int32_t mesh;               /* create_mesh 句柄 */
    int32_t material;           /* materials[] 下标 */
    float model[16];
    int32_t submesh_first, submesh_count;   /* 保留：按 SubMesh.material_slot 覆盖材质 */
    int32_t flags;              /* cast/receive shadow、alpha mode 等 */
    int32_t layer;
    uint64_t sort_key;
} mq_renderer_draw3d;

typedef struct mq_renderer_environment {
    int32_t sky_mode;           /* 0=color 1=gradient 2=cubemap(预留) 3=equirect(预留) */
    float sky_color[4];
    int32_t sky_texture;
    float env_color[4];
    int32_t env_texture;
    float intensity;
} mq_renderer_environment;

typedef struct mq_renderer_post {
    float exposure;
    int32_t tonemap;            /* 0=none 1=ACES(默认) */
} mq_renderer_post;

typedef struct mq_renderer_frame3d {
    int32_t width, height;                  /* 目标像素 */
    const mq_renderer_view3d* views;  uint32_t view_count;
    const mq_renderer_light* lights;  uint32_t light_count;
    const mq_renderer_pbr_material* materials; uint32_t material_count;
    const mq_renderer_draw3d* draws;  uint32_t draw_count;
    mq_renderer_environment env;
    mq_renderer_post post;
    /* 2D 覆盖层（与 3D 同一帧、固定合成在 3D 之后）：沿用既有记录 */
    const mq_renderer_item* items; uint32_t item_count;
    const mq_renderer_mesh* meshes; uint32_t mesh_count;
    /* 2D 覆盖层视图（与旧 render_views 的 view 语义相同：多视图/相机）；空 = 默认单视图 */
    const mq_renderer_view* views2d; uint32_t view2d_count;
    float bg[4];                            /* 2D 覆盖层清屏色 */
    /* 非空 = 离屏渲染并回读 RGBA8（headless / --shot / 抓帧）；空 = 直接呈现到窗口 */
    uint8_t* out; uint32_t out_size;
} mq_renderer_frame3d;
```

### 2.3 新方法（追加在 vtable 表尾，旧方法原样保留）

| 方法 | 签名 | 语义 |
|---|---|---|
| `attach_window` | `int32_t (self, const mq_renderer_window*)` | 建 surface+swapchain，返回 0 成功；非 0 = 无直接呈现（headless/off），调用方回退离屏。可重复调用（resize/换显示器） |
| `resize_surface` | `int32_t (self, int32_t w, int32_t h)` | 重建 swapchain/深度/中间目标；下一帧生效 |
| `caps` | `int32_t (self, mq_renderer_caps*)` | 适配器与目标能力（提取/cook 据此选择） |
| `create_texture2` | `int32_t (self, const mq_renderer_texture_desc*, int32_t* out_id)` | 带 sRGB/mip/wrap/filter 的纹理；2D 旧 `create_texture` 保留 |
| `create_mesh` | `int32_t (self, const mq_renderer_mesh_gpu*, int32_t* out_id)` | 上传 3D 网格（GPU 常驻；runtime 按资产缓存） |
| `destroy_mesh` | `void (self, int32_t id)` | 释放 GPU 网格 |
| `render_frame3d` | `int32_t (self, const mq_renderer_frame3d*)` | 一帧：3D 各 pass + 2D 覆盖层 → 直接呈现或离屏回读 |
| `present` | `int32_t (self)` | 提交交换链（直接呈现；离屏路径为空操作） |

**错误码**：沿用现有约定（0=OK，非 0=失败原因；`render_frame3d` 返回 1 = 无 3D 能力/降级为 2D）。

### 2.4 平台 ABI（`modules/core/data/schema/platform.yaml`，追加）

```c
typedef struct mq_platform_native_window {
    int32_t kind;      /* 0=none 1=win32 2=xlib 3=wayland */
    void*   display;   /* HINSTANCE / Display* / wl_display* */
    void*   handle;    /* HWND / Window / wl_surface* */
    void*   extra;     /* 预留 */
} mq_platform_native_window;

int mq_platform_native_window(mq_platform* p, mq_platform_native_window* out); /* 1=可用 0=无窗口 */
```

### 2.5 所有权 / 线程 / 确定性

- 纹理/网格句柄由 renderer 拥有（`int32` + 世代槽），调用方用 `destroy_*` 归还；runtime 以
  `资产稳定路径 + 内容哈希` 缓存句柄，资源在**装帧期/帧边界**创建，不在热循环内反复建。
- 输入数组（views/lights/materials/draws/items）**仅调用期间有效**，renderer 复制所需数据；
  `out` 由调用方提供（离屏回读）。
- 单线程：与现有渲染一致（模拟线程录制）；wgpu 异步用 `wgpuInstanceProcessEvents` 轮询。
- 渲染**不得回写模拟**；排序 tie-break 固定：view 按声明序、光照 `(priority desc, entity id asc)`、
  opaque `(pipeline, material, mesh, sort_key)`、transparent `(视距 desc, entity id asc)`。

## 3. 依赖与边界

- **可依赖**：`platform`（原生窗口/尺寸/content scale）、`math`（Mat4/Quat/AABB）、`image`（PNG 解码）、
  `vfs`（资产读取）、`module`（插件加载）。
- **禁止**：runtime 直接持有 WGPU 对象；gfx 认识 scene 内部结构（只认 frame3d）；帧内阻塞等待 GPU；
  每帧重建纹理/网格/管线；渲染回写模拟（ADR-0005）。
- **接缝**：runtime 提取 → `render_frame3d`（直接呈现或离屏）→ `present`；截图/CI 走离屏 `out`。

## 4. 渲染设计（实现要点）

### 4.1 直接呈现（决策①）

- **窗口路径**：`platform` 给出原生句柄 → `gfx.attach_window` 建 `WGPUSurface`（Win32/Xlib/Wayland）
  → `wgpuSurfaceGetCapabilities` 选格式（优先 RGBA8/BGRA8 UNORM，其次 `*_SRGB`）→ `wgpuSurfaceConfigure`
  → 每帧 `wgpuSurfaceGetCurrentTexture` 渲染 → `wgpuSurfacePresent`。
- **离屏路径**（headless / `--shot` / `MAQUE_CAPTURE` / 测试）：渲染到 `RGBA8Unorm` 目标 + depth，
  `CopyTextureToBuffer` + map 回读（沿用现有 `read_impl`）。
- **管线格式**：2D/3D/后处理管线按**目标格式**创建；窗口与离屏格式不同 → 各自缓存一套（数量可控）。
- **尺寸/DPI**：swapchain 用 `mq_platform_drawable_size`（设备像素）；`display_changed` → `resize_surface`。
- **回退**：`attach_window` 失败（无窗口/驱动不支持/`MAQUE_PRESENT=off`）→ 离屏路径 + 现有 SDL 呈现；
  老 2D 游戏默认仍走 SDL 路径，**行为不变**（`MAQUE_PRESENT=wgpu` 强制新路径，稳定后 M3 再切默认）。
- **ImGui/覆盖层**：直接呈现路径不引入第二套 ImGui 后端：沿用“ImGui draw list → CPU 像素 → 2D 纹理网格”
  回退链（现有 headless 路径），随 frame3d 的 `meshes` 提交，`MQ_IMGUI_LAYER` 保证最上层。
- **2D/文字/ImGui 零回归**：2D 合成仍发生在 3D 之后、不参与 tonemap；现有 `text_pixel`/`imgui_present_e2e`/
  `shot_e2e`/`window_present_e2e` 全部保持绿（SDL 路径不变；新路径单独加测）。

### 4.2 Pass 顺序（gfx 内部固定，不做 frame graph）

```
per view:
  ① shadow      depth-only（方向光，MVP 1 张 2048²；CSM 在 M5）
  ② opaque      HDR(RGBA16F 可用则用) + depth；PBR forward；材质 alpha_mode=Opaque
  ③ skybox/env  depth==far 全屏三角形（或 clear color）
  ④ transparent 视距降序；depth test on/write off；Mask 用 discard（走 opaque 管线）
  ⑤ tonemap     HDR→LDR + ACES + sRGB（若 LDR 直出则跳过）
  ⑥ 2D 覆盖层   现有 item/mesh 管线（layer 排序不变）
  ⑦ present     或 CopyTextureToBuffer 回读
```

- 一个 pass 一次 begin/end，pass 内切 pipeline/bind group/scissor（沿用既有性能经验）。
- 多视图（`MAQUE_VIEWS>1`/VR）：每视图独立 3D pass；shadow pass 多视图共享。

### 4.3 材质与 uniform 布局

- 绑定：**group0** = 帧（viewProj/eye/env/光源数组/阴影贴图+比较采样器）；
  **group1** = 材质（5 贴图+sampler+材质 uniform 动态偏移）；逐 draw 数据（model/flags）放独立 storage/uniform
  大缓冲 + dynamic offset（256B 对齐）。
- WGSL 侧布局与 C POD **逐字段对齐**（`01_uniforms.wgsl` 注释 + C 侧 `static_assert` 守卫尺寸）。
- PBR：metallic-roughness（glTF 对齐），`F0 = mix(0.04, baseColor, metallic)`、GGX D/G/F、法线贴图 TBN、
  环境光 = 半球/天空色漫反射 + 解析 env BRDF 近似（Karis）；IBL 全量（cubemap+预过滤）在 M4。
- 管线缓存：`(shader_variant, alpha_mode, double_sided, receive_shadow)` 枚举变体，启动期预创建。

### 4.4 提取（runtime）

- `Transform3D` 展平（DFS + 声明序，transient world Mat4）；`Camera3D` 择一（priority desc, entity id asc），
  view/proj 按 WebGPU NDC z∈[0,1]；`Light` 排序+截断到 `caps.max_lights`（溢出确定性地告警一次）；
  `MeshRenderer` → `draw3d`（mesh/material 解析、AABB 视锥剔除、shadow flags）。
- 现有 2D 提取不变；图层固定顺序：World3D → Canvas2D → UI → ImGui。

### 4.5 资产管线（决策②）

- `tools/gltf_import.py`（离线、确定性）：glTF/GLB → `.mesh.json`（规范 48B 顶点布局、tangent 缺失时
  确定性生成、submesh/bounds）+ `.material.json`（`PbrMaterial`）+ PNG 纹理（sRGB 标记）；输出可入库、可 diff。
- 纹理：加载期 VFS→`image` 解码→CPU mip 链（盒式，固定算法）→`create_texture2`；压缩纹理（BC7/ETC2/ASTC）
  在 M5 按 `caps` 选择、产物不入库。
- 样例：`scenes/pbr/`（材质矩阵场景 + 相机环绕脚本），截图序列作为 e2e 基准。

## 5. 测试计划（先于实现）

| # | 用例 | 期望 | 类型 |
|---|---|---|---|
| 1 | ABI 追加不破坏旧表 | 旧 `struct_size` 调用旧方法仍工作；新方法用 `struct_size` 守卫 | 版本 |
| 2 | 记录布局 | `static_assert(sizeof/offsetof)` 全记录；C/WGSL 布局一致性（C 头注释 + 手写校验） | 守卫 |
| 3 | 相机矩阵 | 已知 eye/target → view/proj 手算值；NDC z∈[0,1]、Y-flip 约定 | 单测 |
| 4 | 光源排序/截断 | priority/id 破平；超上限确定性截断 | 单测 |
| 5 | 网格上传/缓存 | 同资产重复 load 只建一次；destroy 后句柄失效 | 单测 |
| 6 | 深度/遮挡 | 离屏两个不同深度 quad → 前遮后；near/far 裁剪 | gfx |
| 7 | PBR 解析 probe | 1×1/小目标、固定方向光+材质 → 中心像素 ≈ 手算 BRDF（±容差） | gfx |
| 8 | metallic/roughness 单调性 | 高光宽度/亮度随参数单调变化 | gfx |
| 9 | 阴影开关 | 阴影区亮度 < 无阴影；PCF 边界不抖动（确定性） | gfx |
| 10 | 直接呈现冒烟 | 真窗口 + `MAQUE_PRESENT=wgpu` 跑 60 帧不崩；resize 后仍正常 | e2e |
| 11 | 截图回归 | `scenes/pbr` `--shot-seq` 多帧：结构性断言（像素占比/亮暗关系），同机逐字节确定 | e2e |
| 12 | 2D 回归 | `text_pixel`/`shot_e2e`/`imgui_present_e2e`/`window_present_e2e` 全绿 | 回归 |
| 13 | 确定性 | 渲染不影响模拟 checksum；两次运行 draw 顺序/截图一致 | 确定性 |
| 14 | glTF 导入确定性 | 同一 glTF 两次导入输出逐字节一致 | 单测 |
| 15 | 性能 | 800×650、~100 draw、1 方向光：Debug+ASan render < 8ms；无逐帧资源重建 | 性能 |

测试替身：离屏 `headless` 目标（现有 `--shot` 链路）即可无窗口验证 3D/材质；直接呈现用真实窗口 e2e。

## 6. 开发计划（分阶段，TDD）

| 阶段 | 内容 | 验收 |
|---|---|---|
| **M0 接口冻结** | 本计划 + schema 追加（渲染器 v1 表尾 / platform 原生窗口）+ 生成物 + `static_assert` + 平台句柄实现 + gfx 骨架方法（caps/纹理 v2/网格上传，3D 渲染返回未实现） | 构建通过、CTest 全绿（无行为变化） |
| **M1 直接呈现** | `attach_window`（Win32/X11/Wayland surface）+ swapchain + 按目标格式建管线 + `render_frame3d` 的 2D 路径 + `present`；runtime 切 `MAQUE_PRESENT=wgpu`（默认仍 SDL）；resize/DPI；ImGui CPU 回退接线 | 真窗口 60 帧冒烟 + 2D 回归全绿 |
| **M2 3D 基础** | depth + 3D 相机 pass + unlit 3D 网格（新 shader `unlit3d`）+ Transform3D/Camera3D/MeshRenderer 提取 + `scenes/pbr/cube.scn` | 截图见正确透视/遮挡 |
| **M3 PBR** | `pbr` shader（GGX/tonemap）+ 方向光 + `PbrMaterial`（无贴图）→ 贴图（create_texture2/sRGB/mip/法线）→ `scenes/pbr/materials.scn` | probe 像素 + 材质矩阵截图 |
| **M4 阴影/环境/资产** | 单方向 shadow map+PCF；渐变天空/环境光；`tools/gltf_import.py` + 样例；透明排序；多光源 | e2e 截图断言 + 导入确定性 |
| **M5 功能补齐** | CSM、IBL（equirect→cube+辐照度/预过滤+解析 BRDF）、ReflectionProbe、Billboard、多视图/VR、MSAA、压缩纹理、材质实例、热重载 | 按游戏需求验收；性能预算 |
| **M6 默认切换** | 直接呈现路径稳定后：窗口 + 有 3D 内容的场景默认 `wgpu`，2D 游戏默认 SDL；文档/AGENTS 更新 | 全量 Windows/Linux ctest |

**当前进度（2026-10-26）**

- **M0 ✅**：renderer 接口表尾追加（10 记录 + 8 方法）、platform 原生窗口 ABI、parser 定长数组、
  gfx `caps`/`create_texture2`/`create_mesh`、runtime 能力自省与探测回退；Windows 构建 + ctest 全绿。
- **M1 ✅（2D 路径）**：`attach_window`（Win32/X11/Wayland wgpu surface + swapchain，按 swapchain 格式重建 2D 管线）、
  `render_frame3d` 直接呈现 2D 覆盖层、`present`、`resize_surface`、surface 抓帧（`MAQUE_CAPTURE_WGPU`）；
  runtime `MAQUE_PRESENT=wgpu` 切换 + SDL_Renderer 不建（ImGui 走 CPU 网格链）+ 多帧冒烟；
  新增 `render3d_present_e2e`。
- **M2 ✅**：depth 缓冲 + 3D pass（`unlit3d`：方向光 Lambert + 环境色 + 透视相机）；
  **直接实现已声明的 `render.*` 组件提取**——`scene.Transform3D` + `render.Camera3D/MeshRenderer/Light`
  （实体按 id 排序保证确定性；M2 所有网格共用内建 cube，M4 换 glTF mesh 资产）；
  3D + 2D 覆盖层同帧（3D 先跑、2D `clear_first=false` 叠加，2D 像素路径不变）；
  `scenes/pbr/cube.scn`（3 个 cube + 方向光 + 透视相机）；新增 `render3d_scene_e2e`
  （离屏 3D 结构断言 + 3D 直呈抓帧）。
- **M3 ✅**：PBR 材质系统——`pbr` shader（metallic-roughness + Cook-Torrance GGX + ACES tonemap +
  gamma）；schema 新增 `asset.PbrMaterial`（typed 字段 + `PbrAlphaMode` + 5 贴图路径槽，角色 `pbr_material`）；
  runtime 从**同节点 `asset.PbrMaterial` 组件**取材质（基色/金属度/粗糙度/自发光/AO），无组件用默认灰；
  `scenes/pbr/materials.scn`（光滑 0.12 vs 粗糙 0.9 金属）+ e2e 粗糙度能量分布断言。
- **M4 ✅（贴图子集）**：PBR albedo 贴图通路——gfx draw bind group 增加 `texture_2d` + `sampler`
  （缺省 1×1 白纹理，材质无贴图时基色不变）；pbr shader 采样 albedo（`RGBA8_SRGB` 采样自动解码）
  + uv 顶点属性（location2, offset40）；内建 cube 补每面 UV（(0,0)…(1,1)）；
  runtime 读 `asset.PbrMaterial.base_color_texture`（`builtin:/` 或 VFS 路径 → `create_texture` 句柄）；
  `scenes/pbr/materials.scn` 左 cube 绑 `builtin:/textures/checker`；e2e 断言贴图真正加载/绑定
  （`MAQUE_RENDER3D_INFO` 打印 albedo texture 路径 + 句柄）。
- **M4b（进行中）**：
  · **离线 glTF/GLB 导入 ✔**：`tools/gltf_import.py`（纯 Python，无第三方依赖）——
    读 `.gltf`（external/内嵌 base64 buffer）与 `.glb`；accessor（含 byteStride）→
    48B 顶点布局（pos3|normal3|tangent4|uv2，缺 NORMAL 时确定性计算）、三角 primitive、
    材质（baseColorFactor/metallic/roughness/emissive/doubleSided/alphaMode → `asset.PbrMaterial`）、
    PNG 贴图搬运；输出 `<name>.mesh.json` 排序键 + 固定缩进（逐字节确定性）；
    e2e `gltf_import_e2e`（夹具 `tests/data/gltf/triangle.gltf`：结构/确定性/`--name`/错误路径）。
  · **运行时加载导入网格 ✔（M4c）**：`render.MeshRenderer.mesh_path`（schema 新增 string 字段）→
    runtime `load_mesh()` 解析 `.mesh.json`（VFS 只读 + 路径缓存）→ `create_mesh`；
    draw 使用导入网格（为空时回退内建 cube）。样例 `scenes/pbr/imported.scn`
    （glTF 夹具 `tests/data/gltf/cube.gltf` → 已入库 `assets/imported/cube.mesh.json`）；
    e2e ④：importer 重新生成与已入库资产**逐字节一致**（漂移检测）+ 场景渲染非空。
  · 待做：MR/normal/AO 贴图槽采样、方向光 shadow map + PCF、`.material.json` 共享材质引用。
- **M4d ✅（贴图子集）**：metallic-roughness 贴图（G=roughness、B=metallic，glTF 约定）+ AO 贴图（R）：
  gfx draw bind group 扩到 5 项（uniform/albedo/sampler/MR/AO；缺省绑 1×1 白纹理），
  pbr shader 用 `metallicFactor×MR.b`、`roughnessFactor×MR.g`、`occlusionStrength×AO.r`；
  runtime 读 `metallic_roughness_texture`/`occlusion_texture` 并修正材质贴图槽默认值 **-1**（未给贴图）；
  `scenes/pbr/imported.scn` 的 cube 绑 MR/AO checker 贴图；e2e ④ 断言两槽真实加载。
  **法线贴图**（需切线空间）与 **方向光 shadow map+PCF**（M4d 尝试遇到 wgpu 纹理使用域冲突，
  已回退，留待重做）仍待做。
- **测试稳定性（R7）**：`imgui_present_e2e` 真实窗口在 CI 负载下偶发**进程级**失败
  （实测约 1/6：同一命令重跑即过，日志/抓帧/像素断言均正常 → 非渲染正确性问题）；
  脚本内每次 player 调用最多两次尝试（保留 `*.retry1` 首次失败日志），两次都失败才判失败；
  连续 4 次 + 全量 81 项 ctest 100% 通过。

## 7. 验收标准（DoD）

- 本计划接口章节冻结；schema 生成物入库、`codegen_drift`/`builtin_shaders` 通过；
- M2 起 `scenes/pbr` 可 `--shot` 出图并通过结构性断言；M4 起导入器可用且确定性；
- 直接呈现路径在 Windows/Linux 真窗口运行 ≥60 帧稳定，resize/最小化/恢复不崩；
- 2D/文字/ImGui/SDL 路径回归测试**全部保持绿**；模拟 checksum 不受渲染影响；
- 性能：见测试 #15；在 800×650 Debug+ASan 下 M3 场景 ≥30fps、Release ≥60fps（以 `MAQUE_TRACE_STATS` 为准）。

## 8. 风险

| 风险 | 处理 |
|---|---|
| 直接呈现与现有 SDL 呈现双路径打架 | 默认 SDL 不变；`MAQUE_PRESENT=wgpu` 显式启用；M6 前不切默认；两路径共用同一帧描述 |
| swapchain 格式与 2D 管线格式不一致 | 管线按目标格式缓存（窗口/离屏各一套），启动期创建 |
| ImGui 在直接呈现下行为退化 | 走既有 CPU 像素回退链（headless 已验证）；后续可选 `imgui_impl_wgpu`（已 vendored） |
| 跨 GPU 像素差异导致测试不稳 | 测试用结构性/解析 probe/容差断言；同机逐字节确定性 |
| wgpu-native 特性缺口（v29） | `caps` 探测；无 HDR → RGBA8、无 cubemap → 解析环境色；不静默降级（打印原因） |
| 大范围改动破坏 2D 回归 | 每阶段跑全量 ctest；2D 路径代码不动，只加新路径 |
| 资产体积/加载卡顿 | 离线 cook + 帧边界预加载；纹理 mip/尺寸上限可配 |

## 9. Owner agent

- **角色标识**：`agent-render3d`（与 `agent-renderer` 协作：ABI/管线归 renderer，3D 提取归 runtime）。
- **职责边界**：本计划覆盖 renderer 3D 扩展、platform 原生窗口、runtime 3D 提取、PBR 资产与导入工具；
  不改动 2D 组件语义与脚本可见 API（新增 3D 便利函数需单独列 schema `api_list` 并走生成绑定）。

- **M4e（示例迁移）**：`tools/gltf_import.py` 增加 `--scene`（glTF 节点树 → 平级实体 + 相机/方向光）
  与 `--compact`；迁移 Khronos **ToyCar**（77k 顶点 / 7 张 PBR 贴图）到 `scenes/khronos/ToyCar/`，
  `preview.png` 已可用（阴影/IBL/clearcoat/transmission 仍缺）。

- **M4g（贴图正确性 + 官方场景回归）**：emissive 贴图槽（emissiveFactor×emissiveTexture）、
  albedo sRGB 解码（`load_texture_srgb` / RGBA8_SRGB）、环境高光近似；Khronos ToyCar 渲染回归
  （`render3d_scene_e2e` ⑤：3D 内容/贴图槽/非空像素）。
- **剩余（M4e/M5）**：方向光 shadow map+PCF、analytic/HDRI IBL、多光源补光
  （第二次尝试 2-light 时遇 wgpu uniform/管线校验崩溃，已回退待重做）、clearcoat/transmission 扩展、
  3D 场景默认切 wgpu 直呈（M6）、Linux 侧构建/测试验证。

- **M4h（交互）**：3D **OrbitControls** —— runtime 消费平台鼠标事件（左键拖拽环绕、滚轮缩放），
  相机的环绕状态在 `extract_3d` 里覆盖场景相机（场景相机仅作为初始机位）；
  `MAQUE_ORBIT=yaw,pitch,dist` 供 CI 确定性覆盖；`render3d_scene_e2e` ⑥ 断言两个环绕角度渲染显著不同。

- **M5a ✅ 透明材质**：draw uniform 112→128（alpha_cutoff/mode/flags）；shader MASK discard、输出 alpha；
  gfx 新增 blend 管线（srcAlpha/1-srcAlpha、depthWrite=false），draw 按材质 alpha_mode 分成
  不透明/透明两组，透明按相机距离远→近排序并画在不透明之后；`scenes/pbr/transparency.scn`
  + e2e ⑦（BLEND 混合可见、MASK 被裁剪、不透明不受影响）。

- **M5b ✅ 多光源**：runtime 提取 Directional/Point/Spot（pos+dir+range+cos(inner/outer)），
  gfx 相机 uniform 128→640B（8 盏灯 × {pos,dir,color,params}），shader 8 灯循环：
  方向光直接取；点光 `window(d/range)²/(d²+1)` 距离衰减；聚光再加 `smoothstep(cos_outer, cos_inner, cone)`；
  `scenes/pbr/lights.scn` + e2e ⑧（红点光染左 cube / 绿聚光染中 cube / 右 cube 中性）。

- **M5c ✅ 方向光阴影**：1024² Depth32Float shadow map；相机 uniform 640→720B（+light_vp/shadow_params）；
  阴影 pass 用同 shader 的 `vs_shadow`（group0 复用相机 BGL，纹理槽绑 1×1 深度占位避免同 pass 读写冲突）；
  shader 3×3 PCF（comparison sampler，`MAQUE_SHADOW` 开关）；`scenes/pbr/shadows.scn` + e2e ⑨（开/关像素差）。

- **M5d ✅ 环境光按场景配置**：`scn.Scene.env_color/env_intensity` → frame3d env；默认值等于原硬编码（零回归）；
  ToyCar 暖灰影棚环境。**M5e ✅ clearcoat**：`asset.PbrMaterial.clearcoat/clearcoat_roughness`
  （同 KHR_materials_clearcoat 字段名），draw uniform 144B，shader 独立 F0=0.04 清漆窄高光。
