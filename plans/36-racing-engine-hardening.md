# 36 · 赛车方向引擎补强（typed transform / 手柄 / 剔除 / 3D 音频 / 碰撞查询 / 合批）

> 来源：`D:\Projects\race-compare\maque-impl\maque-engine-assessment.md`（2026-11-09，Conditional Go）。
> 本计划把评估里**可落地且不改 3D 管线拓扑**的能力补进引擎；明确不做完整 3D 刚体/编辑器（见 §8）。
> 关联：[27](27-threejs-feature-migration.md)（three.js 迁移 M1-4）、[28](28-gpu-morph-and-particles.md)（粒子批）。

## 1. 交付清单

| # | 能力 | 落点 | 开关/默认 | 测试 |
|---|---|---|---|---|
| 1 | **3D 变换 typed API**（值类型编组 + 11 个动词） | `schema/runtime/host.yaml`、`parser.js`、`api/node.cpp` | 无（旧 API 不变） | `node_transform_api_e2e` |
| 2 | **手柄 + 模拟量** | `schema/platform.yaml`、`platform.cpp`、`input.cpp`、`api/system.cpp` | 无手柄 = 轴 0；`MAQUE_GAMEPAD` 供 CI | `gamepad_e2e` |
| 3 | **跟车相机 / 关闭主相机 Orbit** | `render.Camera3D.orbit_controls`、`extract3d.cpp`、`frame.cpp` | 默认 true（旧行为） | `orbit_toggle_e2e` |
| 4 | **3D 视锥剔除** | `render.MeshRenderer.frustum_culled` + gfx 主 pass | 默认 true；`MAQUE_CULL=0` 关闭 | `cull_e2e` |
| 5 | **阴影按相机附近拟合** | gfx shadow fit（`MAQUE_SHADOW_RANGE`，默认 120m） | `0` = 关（旧全场景） | `scenes/pbr/shadows.scn` 回归 |
| 6 | **3D 音频 listener / Doppler** | `world/audio` ABI + mixer + `systems/audio.cpp` | `AudioSource.doppler=0` 默认关 | `audio`（新增 3D/Doppler 用例） |
| 7 | **3D 碰撞查询**（球/扫掠球/定向盒） | `scene/pick.cpp` + `scene_*` host API | 只读查询，无动力学 | `collision_e2e` |
| 8 | **静态网格合并工具** | `tools/merge_static_meshes.py` | 离线工具 | `merge_static_meshes_e2e` |
| 9 | 平级 `"parent": "名字"` | `scene/loader.cpp` | 嵌套 `children` 优先 | 随 transform/parent 场景 |
| 10 | 程序化图元缓存上限 64 → 512 | `render/primitives.cpp` | 超限仍回退内建 cube | `geometry_primitives_e2e` 回归 |

## 2. 3D 变换 typed API（plans/27 M1-4）

### 2.1 值类型编组（前置基础设施）

`api_list` 原先只允许标量/`string`/`bytes`；three.js 风格 API 需要向量返回值。现在：

- `api_list` 的**参数/返回值**可写**全限定具名 struct**（如 `math.Vec3`），要求：
  扁平 `float` 字段（`Vec2`/`Vec3`/`Vec4`/`Color`/`Quat`）；嵌套/容器/bytes 混用会被 schema 校验拒绝。
- 生成物：
  - `maque_api.h`：C++ 原生值（`math::Vec3 node_world_position(...)`），引擎实现最自然；
  - JS/Lua：注册 `mq_script_register_value`（kinds 里 `2/3/4` = 值参数分量数）；
    JS 返回 `{x,y,z}`，Lua 返回 table；参数可传对象/数组，也可传 3 个连续数字；
  - C guest：C ABI 不传 struct 值 → 值参数平铺 `const float*`、值返回尾部 `float* out`，
    `maque/script.hpp` SDK 负责拆/装。
- 脚本 ABI：`mq_script_register_value`（`abi/script.h`，`MQ_SCRIPT_ABI_VERSION` 4 → 5）。

### 2.2 新增动词

| API | 语义 |
|---|---|
| `node_set_local_position/rotation/scale(id,x,y,z)` | 写 `scene.Transform3D.local`；rotation 弧度；缺组件自动补挂 |
| `node_local_position/rotation/scale(id)` → `Vec3` | 读局部（与反射 `local.position.x` 同一存储） |
| `node_world_position(id)` → `Vec3` | 世界矩阵平移（按父链**即时重算**，含 glTF 动画姿态） |
| `node_world_rotation(id)` → `Vec3` | 世界矩阵的 Euler（与 `m4_from_trs` 同约定） |
| `node_local_to_world(id, p)` / `node_world_to_local(id, p)` | 点变换 / 逆变换（O(depth)，不扫全场景） |
| `node_look_at(id, x,y,z)` | 本地 -Z 指向世界目标、+Y 为上；**含父链补偿**（只改 rotation） |

`node_set_position` 仍是 2D/世界语义；`local_*` 与它刻意分开，避免"局部/世界"混名。

## 3. 手柄与模拟量

- 平台：`SDL_INIT_GAMECONTROLLER` + 热插拔（`SDL_CONTROLLERDEVICEADDED/REMOVED`），最多 4 pad；
  `mq_input_frame` 尾部追加 `gamepad_count/connected/axis[24]/buttons[4]`；每帧 `poll` 采样。
- 输入模块：`mq_input_axis(pad,axis)` / `mq_input_gamepad_buttons(pad)` / 计数；模拟量是状态，不进事件流。
- 脚本：`input_axis(axis)`（pad0）、`input_axis_pad(pad,axis)`、`input_gamepad_button(bit)`、
  `input_gamepad_buttons(pad)`、`input_gamepad_connected()`；轴下标见 `mq_gamepad_axis`，
  按钮位见 `mq_gamepad_button`（South=1、East=2、West=4、North=8、…）。
- CI：`MAQUE_GAMEPAD="lx,ly,rx,ry,lt,rt[,buttons]"` 覆盖 pad0（确定性；无手柄/无窗口也可用）。

## 4. 相机：跟车 vs 内建 Orbit

`render.Camera3D.orbit_controls=false`：右键/中键/滚轮/触摸手势不再改写相机矩阵，指针事件仍给脚本；
跟车相机由脚本用 `node_world_position` + `node_look_at` 每帧计算。默认 true = 旧行为。
（输入分发读上一帧提取缓存，切开关有 ≤1 帧延迟。）

## 5. 3D 剔除与阴影

- 剔除：提取期把 `render.MeshRenderer.frustum_culled` 写成 `draw3d.flags` bit29；gfx 主 pass 用
  view-proj 六平面 + draw 世界 AABB 保守剔除（不误剔）。阴影 pass **不剔除**（视外物体投影仍在）。
  `MAQUE_CULL=0` 关闭、`MAQUE_CULL_INFO=1` 打印 `gfx: culled draws N/M`。
- 阴影拟合：仍在光源视空间按 draw bounds 拟合（R2），新增**相机距离过滤**：
  包围球完全在 `MAQUE_SHADOW_RANGE`（默认 120m）外不参与拟合；蒙皮 draw 不参与距离过滤。
  `MAQUE_SHADOW_RANGE=0` = 旧全场景行为。长赛道车影不再被远景物体撑成几个像素。

## 6. 3D 音频

- `mq_audio_listener` 尾部追加 `up_x/y/z`、`vel_x/y/z`；`mq_audio_set_velocity(voice, v, doppler)`。
- Mixer：右向量 `cross(dir,up)`（up 全 0 时退回旧 2D `(dir_y,-dir_x)`，旧用例零变化）；
  Doppler `scale = c/(c+v_rel)`（c=343，按 `doppler` 强度插值，clamp 0.5..2）。
- runtime：听者/声源位置与朝向取节点世界矩阵（`-Z` 前、`+Y` 上）；速度用上一帧位置差分；
  `AudioSource.doppler` 透传。2D 场景（无 Transform3D）继续用 `world_x/y` + 节点 angle。

## 7. 3D 碰撞查询（只查询，无动力学）

- `scene_sphere_overlap(x,y,z,r)`、`scene_sphere_cast(ox,oy,oz,dx,dy,dz,r,max)`、
  `scene_obb_overlap(x,y,z,hx,hy,hz,yaw)` → 最近实体 id（-1 未命中）；
- 命中详情：`scene_cast_distance()` / `scene_cast_point_x/y/z()`（与 `scene_raycast` 共用）。
- 几何与拾取同源：glTF primitive bounds / `.mesh.json` bounds / 内建 cube；球在本地空间按
  `radius/min_scale` 保守膨胀（非均匀缩放不会漏检）；OBB-OBB 用 15 轴 SAT。
- **不提供**刚体、冲量、约束、CCD 求解；赛道护栏/车车碰撞仍需游戏层解算（本 API 只做查询）。

## 8. 合批内容管线 + 兼容修复

- `tools/merge_static_meshes.py`：manifest（groups → meshes[path,position,rotation_deg,scale]）
  → 每 group 一个 `.mesh.json`，TRS 烘焙（法线逆转置、切线方向、索引偏移、bounds 重算），
  输出确定性（`sort_keys` + 2 空格 + LF）。只接受静态三角网格 layout 0/1。
- 平级 `"parent": "名字"`：整棵实体树加载完再解析（支持前向引用）；父不存在/成环 → 警告并保持根；
  已有父（嵌套 `children`）不覆盖。
- 程序化图元缓存 64 → 512（4 场景 + 8 车仍有余量；超限行为不变）。

## 9. 明确未做（评估里的 P0/P1 剩余）

- **3D 刚体物理 / 动力学**（只做了查询；车辆模型仍建议 2.5D 街机）；
- **InstancedMesh / GPU 实例化静态合批**（当前推荐离线合并；gfx 仍逐 draw）；
- 地形 / 水面 / 3D 世界文字 / 3D 编辑器 / CSM / 多 shadow map；
- 手柄震动 / 陀螺仪；音频 HRTF / 多次折射；三角形级拾取。

## 10. 测试与诊断

```bash
ctest --test-dir build -R 'node_transform_api_e2e|gamepad_e2e|collision_e2e|orbit_toggle_e2e' --output-on-failure
ctest --test-dir build -R 'cull_e2e|merge_static_meshes_e2e' --output-on-failure
ctest --test-dir build -R '^audio$|geometry_primitives_e2e|pick_e2e' --output-on-failure
```

诊断开关：`MAQUE_CULL=0|1`、`MAQUE_CULL_INFO=1`、`MAQUE_SHADOW_RANGE=<m>`、
`MAQUE_GAMEPAD=...`、`MAQUE_RENDER3D_INFO=1`（mesh 载入/材质/灯光）。
