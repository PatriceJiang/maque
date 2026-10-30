# 33 · RenderTexture：相机离屏纹理与截图

> 上游：[13 · 渲染（WebGPU）](13-渲染（WebGPU）.md) §6、[17 · 内置组件设计](17-内置组件设计.md)、
> [32 · 场景实例与事件作用域](32-场景实例与事件作用域.md)。
> API 单一真相：`modules/core/data/schema/render/render.yaml`、`modules/core/data/schema/runtime/host.yaml`、
> `modules/core/data/schema/control.yaml`；自动生成参考见 [API](api/README.md)。

## 1. 能力与适用场景

`render.RenderTexture` 把一台场景相机的输出保存在 GPU 纹理中。该纹理使用普通纹理句柄空间，后续 pass 可直接采样，
逐帧路径不经过 CPU readback。

典型用途：

- 小地图、后视镜、监控画面；
- 角色/道具预览；
- 门户、屏幕、模型表面的动态画面；
- 2D 内容先离屏，再作为 Sprite 组合；
- 测试、Inspector 或 AI 工具对指定相机截图。

RenderTexture 是**相机节点上的组件**，不是一种节点类型，也不内嵌另一套相机字段。节点必须同时挂以下三类相机之一：

| 相机 | 建议变换 | 用途 |
|------|----------|------|
| `scn.Camera` | 相机自身的 `x/y` 字段 | 旧 2D 场景兼容 |
| `render.Camera2D` | 同节点 `scn.Transform2D` | 新 2D 场景；支持 viewport、layer mask、clear color |
| `render.Camera3D` | 同节点 `scene.Transform3D` | 3D 透视/正交、viewport、clear mode、culling mask |

挂了 RenderTexture 的相机只驱动自己的离屏目标，不参与主画面的相机优先级竞争。主画面仍由没有 RenderTexture 的 active
相机选择。

> `asset.RenderTarget` 与 `render.RenderLayer.target` 是通用声明式 pass/目标模型，当前 runtime 尚未消费这条路径。
> 相机离屏纹理应使用本文的 `render.RenderTexture`，不要把 transient GPU id 写入 `asset_ref<Texture>`。

## 2. 最小 2D 场景

```json
{
  "title": "rt_2d",
  "width": 320,
  "height": 240,
  "views": 1,
  "entities": [
    {
      "name": "capture_camera",
      "components": [
        { "type": "scn.Transform2D", "x": 80.0, "y": 120.0 },
        { "type": "render.Camera2D",
          "zoom": { "x": 1.0, "y": 1.0 },
          "viewport": { "x": 0.0, "y": 0.0, "width": 1.0, "height": 1.0 },
          "layer_mask": 1,
          "clear_color": { "r": 0.02, "g": 0.03, "b": 0.05, "a": 1.0 } },
        { "type": "render.RenderTexture",
          "name": "minimap", "width": 160, "height": 120,
          "update_mode": 0, "filter": 0, "enabled": true }
      ]
    },
    {
      "name": "monitor",
      "components": [
        { "type": "scn.Transform2D", "x": 235.0, "y": 65.0 },
        { "type": "scn.Sprite", "width": 144.0, "height": 108.0,
          "r": 1.0, "g": 1.0, "b": 1.0,
          "texture": "rendertexture://minimap" }
      ]
    }
  ]
}
```

`viewport` 是目标内的归一化矩形。`layer_mask` 的 bit 0/1 分别对应当前 2D 世界层/UI 层；
`include_ui=false` 时即使 mask 包含 UI，目标也不会收集游戏 UI。引擎 ImGui 永不进入相机 RenderTexture。

Camera2D 目前使用等比缩放 ABI。`zoom.x != zoom.y` 时会打印一次诊断并使用 `zoom.x`，不会静默做非等比变形。

完整动态文本示例：

- `scenes/render_texture/render_texture_2d.scn`
- `scenes/render_texture/render_texture_2d.js`

## 3. 最小 3D 场景

相机、目标和可见对象通过 mask 配对：

```json
{
  "name": "preview_camera",
  "components": [
    { "type": "scene.Transform3D", "local": {
        "position": { "x": 0.0, "y": 1.2, "z": 4.0 } } },
    { "type": "render.Camera3D",
      "projection": 0, "fov_y": 45.0, "near": 0.1, "far": 100.0,
      "culling_mask": 2, "clear_mode": 1,
      "clear_color": { "r": 0.04, "g": 0.05, "b": 0.08, "a": 1.0 } },
    { "type": "render.RenderTexture",
      "name": "preview", "width": 256, "height": 256,
      "update_mode": 0, "filter": 1, "enabled": true }
  ]
}
```

被拍摄网格写相交的 `culling_mask`：

```json
{ "type": "render.MeshRenderer", "mesh_path": "assets/model.glb", "culling_mask": 2 }
```

`Camera3D.clear_mode`：

| 值 | 名称 | 颜色 | 深度 | 天空 |
|---:|------|------|------|------|
| `0` | `Skybox` | 清除 | 清除 | 绘制环境天空 |
| `1` | `Color` | 按 `clear_color` 清除 | 清除 | 不绘制天空 |
| `2` | `DepthOnly` | 保留 | 清除 | 不绘制天空 |
| `3` | `None` | 保留 | 保留 | 不绘制天空 |

当前示例：

- `scenes/render_texture/render_texture_3d.scn`：两台 3D 相机分别拍 Duck/Fox，再显示到主画面 Sprite；
- `scenes/render_texture/render_texture_cube.scn`：把两张 RenderTexture 绑定到 PBR/Basic 材质贴图，显示在 cube 表面。

## 4. RenderTexture 字段

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `name` | string | `""` | 实例内唯一名；空时取节点名，节点名也为空则用 `camera-<entity-id>` |
| `width` | uint32 | `0` | 目标宽；`0` 跟随场景设计宽度 |
| `height` | uint32 | `0` | 目标高；`0` 跟随场景设计高度 |
| `update_mode` | enum | `EveryFrame` | `0=EveryFrame`、`1=OnDemand` |
| `filter` | enum | `Linear` | `0=Nearest`、`1=Linear` |
| `include_ui` | bool | `false` | 是否允许收集游戏 UI 层；仍受 Camera2D `layer_mask` 限制 |
| `enabled` | bool | `true` | false 时停止自动提交并保留上次内容；显式截图请求仍可强制更新 |
| `clear_color` | Color | 透明黑 | `scn.Camera` 路径使用；Camera2D/Camera3D 使用相机自己的 clear color |
| `texture` | string | `""` | transient/readonly；运行期规范 URI |
| `ready` | bool | `false` | transient/readonly；至少成功渲染一次后为 true |
| `last_frame` | uint32 | `0` | transient/readonly；最后成功更新的模拟帧号 |

`texture/ready/last_frame` 不进入场景保存与世界快照。RenderTexture 本身是视图资源，不参与模拟 checksum。

## 5. 纹理 URI 与场景实例作用域

场景中推荐写短 URI：

```text
rendertexture://minimap
```

运行期规范 URI 为：

```text
rendertexture://<scene-instance-id>/minimap
```

解析规则：

1. 根场景短 URI 自动解析到实例 `0`；
2. 子场景中的短 URI 按**消费者节点所属实例**解析；
3. 两个子场景实例可以各自定义同名目标，不会串纹理；
4. 运行期实例 id 不会写回场景文本，因此 bake/save 后作为根场景重新加载不会断链；
5. 同一实例内重复 name 会打印错误，后定义目标不会覆盖前者。

脚本需要规范 URI 时调用 `camera_texture(cameraEntity)`。当前 `.scn` 作者面支持所有实际消费 string 纹理路径的槽，
主要包括 `scn.Sprite.texture` 和 `asset.PbrMaterial` 的 `*_texture` 字段。

## 6. 更新模式与帧序

### EveryFrame

每个渲染帧都更新。适合镜子、监控、持续动画预览。

### OnDemand

首帧生成一次，之后保持内容。需要刷新时：

```js
const camera = entity_find("capture_camera");
camera_update(camera); // 返回 1=节点有 RenderTexture；0=无效
```

尺寸、filter、相机维度、`include_ui` 或重新启用等变化也会自动标脏。resize 原位替换底层 GPU 资源，纹理句柄保持稳定，
已绑定的 Sprite/材质无需重新赋值。

一帧顺序：

```text
脚本/动画/物理
  -> 节点世界变换展平
  -> 2D/3D 绘制描述提取
  -> RenderTexture pass（依赖顺序）
  -> 主画面 pass（可采样本帧 RT）
  -> present / 主画面截图
```

因此生产者到消费者不需要一帧延迟。RenderTexture pass 计入 render 耗时，不计入 logic。

## 7. 多目标依赖、环与自采样

若目标 A 的可见内容采样目标 B，引擎先渲染 B，再渲染 A。同帧 queue 提交顺序保证后续 pass 可见前一目标结果，
无需 copy 或 CPU readback。

- 2D 依赖只统计被 `include_ui/layer_mask` 接受的 item/mesh；
- 3D PBR 依赖只统计 active、visible 且通过 Camera3D/MeshRenderer culling mask 的材质；
- 目标直接采样自己时，后端剔除该批次，避免同一 WebGPU texture 同时作为 attachment 和 sampled texture；
- A↔B 环按相机 entity id 稳定排序：较早目标读取较晚目标的上一帧，较晚目标可读取较早目标的本帧；
- 创建和 resize 后目标先确定性清屏，不会读取未初始化显存。

这种环语义适合明确的一帧反馈。普通业务应优先保持 DAG，以便首帧结果直观且成本可控。

## 8. 脚本截图 API

```js
function on_start() {
  const camera = entity_find("capture_camera");
  const uri = camera_texture(camera);       // 无有效组件时返回 ""
  log("camera texture=" + uri);

  camera_update(camera);                    // OnDemand 标脏
  const ok = camera_capture(camera, "shots/minimap.png");
  // ok=1：请求已排队；ok=0：camera/path 无效
}
```

`camera_capture` 在本帧 RenderTexture pass 成功后回读同一目标，不建立第二套相机，也不重复执行不同语义的渲染。
请求会强制更新 OnDemand 目标；即使 `enabled=false`，显式截图仍执行一次，但节点/父链必须 active。

路径是宿主文件系统路径，相对路径基于进程工作目录，不走场景 VFS。`.png` 写 PNG，其他扩展写 PPM。
若本帧相机更新失败，不会把旧帧伪装成本次截图。

## 9. 控制面截图

### Capture.camera

只读方法，读取指定相机最后成功的 RenderTexture，输出 PPM 字节流的 base64 分块。`camera` 是场景 entity id，
可先通过 `Scene.entities` 按节点名定位，再用 `Scene.get` 确认其组件：

```json
{"jsonrpc":"2.0","id":1,"method":"Capture.camera","params":{
  "camera":0,"offset":0,"max":24576,"max_dim":512
}}
```

成功响应的关键字段：

```json
{"ok":true,"camera":0,"w":160,"h":120,"frame":42,
 "total":57615,"offset":0,"size":24576,"data":"...base64..."}
```

- `offset=0` 建立一次锁帧快照；
- 后续请求用返回的 `offset + size` 继续，所有块来自同一帧；
- `max=0` 或大于 1 MiB 时回退 24 KiB；
- `max_dim>0` 时等比盒式降采样，降低 Inspector/AI 传输量；
- 若 snapshot 失效，返回 `snapshot expired; restart at offset 0`；
- 文件截图使用独立回读缓冲，不会覆盖正在分块传输的快照。

### Capture.camera_save

变更类方法：

```json
{"jsonrpc":"2.0","id":2,"method":"Capture.camera_save","params":{
  "camera":0,"path":"shots/minimap.png"
}}
```

方法立即返回 `deferred:true`，下一渲染帧强制更新并落盘。Inspector 默认只读会拒绝该方法；需要
`MAQUE_INSPECT_RW=1`。

## 10. 生命周期与资源管理

- 场景装配时先创建全部 RenderTexture，再预解析 Sprite/PBR 消费者；
- GPU 句柄与普通纹理共用命名空间，但不会暴露到序列化字段；
- 动态增删组件、改名、resize 在固定帧边界同步；
- resize 失败保留旧物理纹理和旧尺寸，不留下半初始化句柄；
- 节点销毁、组件移除、场景热重载、`director.load_scene` 和引擎退出都会释放目标及 URI 别名；
- `resource_bytes()` 把持久 RenderTexture 计入 VRAM 估算；depth/MSAA 等帧临时附件不计入；
- null renderer 保留句柄、尺寸、清屏和回读语义，用于 headless/WASM P0；它不执行真实几何光栅化。

## 11. 格式、采样与限制

当前目标固定为：

- 2D、单层、单 mip；
- `RGBA8Unorm`；
- 物理目标单采样；场景 MSAA 使用临时多采样附件并 resolve；
- usage 包含 RenderAttachment、TextureBinding、CopySrc、CopyDst。

采样规则：

- Sprite 和 2D mesh 使用 RenderTexture 自己的 nearest/linear sampler；
- PBR 当前一个材质共用一个 sampler；材质含 RenderTexture 时优先使用 RT sampler，否则使用第一张有效静态贴图 sampler；
- RenderTexture 是已渲染颜色，适合 base color/emissive/matcap 类颜色用途，不建议放入 normal/MR/AO 数据槽；
- 不自动生成 mip，高频画面缩小时应控制分辨率或避免斜视下的细密纹理。

当前不提供：cubemap/array RenderTexture、深度纹理对外采样、持久历史纹理 API、任意 WGSL channel 输入、
Camera2D 非等比 zoom。

## 12. 性能建议

每个 EveryFrame 目标至少增加一组相机 pass；3D 目标还会执行可见网格、灯光、阴影和材质工作。

1. UI 头像/静态预览优先 `OnDemand`；
2. 小地图按实际显示尺寸创建，不要默认跟随大窗口；
3. 不需要 UI 时保持 `include_ui=false`；
4. 用 Camera2D `layer_mask` / Camera3D `culling_mask` 缩小工作集；
5. 避免依赖环和无意义的相机互拍；
6. 截图/readback 是冷路径，不要每帧调用；
7. 用 `MAQUE_TRACE_STATS=1` 观察 render 时间、draws、tris 和 target，VRAM 看 `resource_bytes` 统计。

## 13. Windows 构建、运行与测试

Debug + ASan：

```powershell
tools\win\build.cmd
$env:MAQUE_WINDOW='0x0'
$env:MAQUE_PRESENT='off'
.\build\samples\player\maque_player.exe `
  --shot .\build\rendertexture.png --shot-frame 2 `
  .\scenes\render_texture\render_texture.scn
```

Release：

```powershell
tools\win\build.cmd -BuildDir build-win-release -BuildType Release -NoSanitize
.\build-win-release\samples\player\maque_player.exe `
  .\scenes\render_texture\render_texture_3d.scn
```

聚焦回归：

```powershell
ctest --test-dir build --timeout 180 --output-on-failure `
  -R '^render_texture_e2e$'
```

`tests/render_texture_e2e.sh` 覆盖 GPU 离屏目标、同帧 Sprite 采样、Camera2D/Camera3D、截图和像素断言。
手工场景还包括 `render_texture_2d.scn` 与 `render_texture_cube.scn`。

## 14. 排错表

| 现象 | 检查 |
|------|------|
| `ready=false` | 组件是否与有效相机同节点；节点/父链是否 active；renderer 是否带 RenderTexture ABI |
| Sprite 纯色或 PBR 白色 | URI 是否拼写为 `rendertexture://name`；生产者是否在同一场景实例；name 是否重复 |
| RenderTexture 相机改变主画面 | 相机与 RenderTexture 是否确实同节点；查看创建日志中的 `camera=2d/3d` |
| 目标始终不更新 | `update_mode` 是否 OnDemand；调用 `camera_update`；检查 `enabled/active` |
| UI 没被拍到 | 同时检查 `include_ui=true` 与 Camera2D `layer_mask` 的 UI bit |
| 3D 对象缺失 | Camera3D `culling_mask` 与 MeshRenderer `culling_mask` 是否相交 |
| 天空覆盖 clear color | `clear_mode` 应为 `Color(1)`，只有 `Skybox(0)` 绘制环境天空 |
| 画面模糊 | `filter=Nearest(0)`；RenderTexture 尺寸是否低于最终显示尺寸 |
| 截图没有生成 | 路径是否可写；请求后是否至少执行一个渲染帧；查看 `camera shot` 日志 |
| `Capture.camera` 后续块失效 | 从 `offset=0` 重启同一次分块传输 |
| Windows 无法加载 renderer | 确认 `maque_gfx.dll`、`wgpu_native.dll` 在 player 旁；优先使用 `tools\win\build.cmd` |

## 15. 相关入口

- 总体渲染语义：[13 §6](13-渲染（WebGPU）.md#61-相机-rendertexture)
- 组件字段：[components/03-render](components/03-render.md)
- 自动生成脚本/控制面 API：[api/README](api/README.md)
- 自动生成类型参考：`modules/core/data/generated/TYPES.md`
- 2D 示例：`scenes/render_texture/render_texture_2d.scn`
- 3D 双相机示例：`scenes/render_texture/render_texture_3d.scn`
- PBR cube 示例：`scenes/render_texture/render_texture_cube.scn`
- 回归：`tests/render_texture_e2e.sh`
