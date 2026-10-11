# maque API 参考（自动生成）

> ⚠️ 本文件由 `modules/core/data/parser.js` 从 `schema.yaml`（**唯一真相**）生成，**请勿手工编辑**。
> 重新生成：`cmake --build build --target maque_codegen`，再运行 `tools/gen_api_docs.sh` 同步到 `docs/api/`。

统计：脚本 API **107** 个 · 控制面方法 **31** 个 · 事件回调 **10** 个 · 模块 C ABI **22** 个 · 插件接口 **1** 个。

## 目录

1. [脚本 API（api_list）](#1-脚本-apiapilist)
2. [控制面方法（control_list，AI/MCP）](#2-控制面方法control_listaimcp)
3. [脚本事件回调（callbacks）](#3-脚本事件回调callbacks)
4. [插件接口（interface → C vtable）](#4-插件接口interface--c-vtable)
5. [模块 C ABI（module_abi）](#5-模块-c-abimodule_abi)
6. [数据类型与组件](TYPES.md)

## 1. 脚本 API（api_list）

脚本里直接调用（`host` 命名空间为全局函数，无前缀；其余为 `<ns>.<fn>`）。
`ctx` = 首参注入引擎实例（脚本不可见）。

### math

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `math.clamp` | value: float, lo: float, hi: float | float |  |
| `math.lerp` | a: float, b: float, t: float | float |  |
| `math.saturate` | x: float | float | MANUAL → `math.custom.h` |

### host（全局，无前缀）

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `screen_w` | — | float | ctx |
| `screen_h` | — | float | ctx |
| `rng` | — | float | ctx |
| `quit` | — | void | ctx |
| `clear` | r: float, g: float, b: float | void | ctx |
| `log` | msg: string | void | ctx |
| `cfg` | path: string | float | ctx |
| `load_texture` | path: string | float | ctx |
| `entity_spawn` | — | float | ctx |
| `entity_set_pos` | id: float, x: float, y: float | void | ctx |
| `entity_x` | id: float | float | ctx |
| `entity_y` | id: float | float | ctx |
| `entity_count` | — | float | ctx |
| `entity_destroy` | id: float | void | ctx |
| `entity_name` | id: float | string | ctx |
| `entity_find` | name: string | float | ctx |
| `entity_valid` | id: float | float | ctx |
| `input_x` | — | float | ctx |
| `input_y` | — | float | ctx |
| `input_target` | — | float | ctx |
| `input_down` | — | float | ctx |
| `input_key` | code: float | float | ctx |
| `input_focus` | — | float | ctx |
| `input_set_focus` | id: float | void | ctx |
| `ui_hit` | — | float | ctx |
| `ui_hover` | — | float | ctx |
| `ui_pressed` | id: float | float | ctx |
| `entity_z` | id: float | float | ctx |
| `entity_set_z` | id: float, z: float | void | ctx |
| `spawn_ball` | x: float, y: float, vx: float, vy: float, radius: float | float | ctx |
| `bounce_step` | dt: float | void | ctx |
| `physics_step` | — | void | ctx |
| `ball_clear` | — | void | ctx |
| `set_label` | id: float, text: string | void | ctx |
| `ui_hit_at` | x: float, y: float | float | ctx |
| `nav_setup` | cols: float, rows: float, cell: float | void | ctx |
| `nav_block` | cx: float, cy: float, w: float, h: float | void | ctx |
| `nav_goto` | id: float, x: float, y: float | float | ctx |
| `nav_path_len` | id: float | float | ctx |
| `nav_tick` | — | void | ctx |
| `node_set_parent` | id: float, parent: float | float | ctx |
| `node_parent` | id: float | float | ctx |
| `node_child_count` | id: float | float | ctx |
| `node_child_at` | id: float, index: float | float | ctx |
| `node_find_child` | id: float, name: string | float | ctx |
| `node_set_name` | id: float, name: string | void | ctx |
| `node_set_active` | id: float, on: float | void | ctx |
| `node_active` | id: float | float | ctx |
| `node_set_scale` | id: float, s: float | void | ctx |
| `node_scale` | id: float | float | ctx |
| `node_set_angle` | id: float, deg: float | void | ctx |
| `node_angle` | id: float | float | ctx |
| `node_set_opacity` | id: float, a: float | void | ctx |
| `node_opacity` | id: float | float | ctx |
| `node_set_z_index` | id: float, z: float | void | ctx |
| `node_z_index` | id: float | float | ctx |
| `node_width` | id: float | float | ctx |
| `node_height` | id: float | float | ctx |
| `node_set_position` | id: float, x: float, y: float | void | ctx |
| `node_position_x` | id: float | float | ctx |
| `node_position_y` | id: float | float | ctx |
| `node_set_content_size` | id: float, w: float, h: float | void | ctx |
| `node_set_anchor` | id: float, ax: float, ay: float | void | ctx |
| `node_has_component` | id: float, type: string | float | ctx |
| `node_add_component` | id: float, type: string | float | ctx |
| `node_remove_component` | id: float, type: string | float | ctx |
| `node_component_count` | id: float | float | ctx |
| `node_component_at` | id: float, index: float | string | ctx |
| `node_child_with_component` | id: float, type: string | float | ctx |
| `node_get_number` | id: float, comp: string, field: string | float | ctx |
| `node_set_number` | id: float, comp: string, field: string, value: float | float | ctx |
| `node_get_bool` | id: float, comp: string, field: string | float | ctx |
| `node_set_bool` | id: float, comp: string, field: string, value: float | float | ctx |
| `node_get_string` | id: float, comp: string, field: string | string | ctx |
| `node_set_string` | id: float, comp: string, field: string, value: string | float | ctx |
| `node_on` | id: float, event: string, handler: string | float | ctx |
| `node_off` | id: float, event: string | float | ctx |
| `node_emit` | id: float, event: string | float | ctx |
| `node_emit_number` | id: float, event: string, value: float | float | ctx |
| `ui_set_value` | id: float, v: float | float | ctx |
| `ui_get_value` | id: float | float | ctx |
| `ui_set_checked` | id: float, on: float | float | ctx |
| `ui_get_checked` | id: float | float | ctx |
| `ui_set_text` | id: float, text: string | float | ctx |
| `ui_get_text` | id: float | string | ctx |
| `ui_text_width` | id: float | float | ctx |
| `ui_text_height` | id: float | float | ctx |
| `ui_text_lines` | id: float | float | ctx |
| `director_pause` | — | void | ctx |
| `director_resume` | — | void | ctx |
| `director_paused` | — | float | ctx |
| `director_frame` | — | float | ctx |
| `director_load_scene` | path: string | float | ctx |
| `tween_start` | id: float, field: string, to: float, duration: float, easing: float | float | ctx |
| `tween_stop` | id: float | float | ctx |
| `tween_count` | id: float | float | ctx |
| `tween_progress` | id: float | float | ctx |
| `schedule_once` | handler: string, delay: float | float | ctx |
| `schedule` | handler: string, interval: float | float | ctx |
| `unschedule` | handler: string | float | ctx |
| `schedule_count` | — | float | ctx |
| `node_destroy` | id: float | float | ctx |
| `node_valid` | id: float | float | ctx |
| `node_find_path` | root: float, path: string | float | ctx |

## 2. 控制面方法（control_list，AI/MCP）

JSON-RPC（`{"jsonrpc":"2.0","id":N,"method":"<方法>","params":{…}}`）；
同一份声明还生成 MCP `tools/list`（含类型化 `inputSchema`）。

| 方法 | 参数 | MCP 类型 |
|------|------|----------|
| `Types.describe` | — | — |
| `Scene.info` | — | — |
| `Scene.entities` | — | — |
| `Scene.get` | id: float, path: string | number, string |
| `Scene.set` | id: float, path: string, value: json, dry_run: bool | number, string, object, boolean |
| `Scene.spawn` | name: string, parent: float, component: string, dry_run: bool | string, number, string, boolean |
| `Scene.destroy` | id: float, dry_run: bool | number, boolean |
| `Scene.bake` | dry_run: bool | boolean |
| `Scene.save` | path: string | string |
| `Runtime.step` | frames: float | number |
| `Runtime.frame` | — | — |
| `Director.pause` | — | — |
| `Director.resume` | — | — |
| `Director.status` | — | — |
| `World.checksum` | — | — |
| `World.trail` | — | — |
| `World.snapshot` | — | — |
| `World.restore` | — | — |
| `Replay.log` | — | — |
| `Replay.checkpoints` | — | — |
| `Replay.seek` | frame: float | number |
| `Input.sim_load` | path: string, json: string, loop: bool, reset: bool | string, string, boolean, boolean |
| `Input.sim_status` | — | — |
| `Input.sim_events` | — | — |
| `Input.sim_stop` | — | — |
| `Input.inject` | type: float, x: float, y: float, button: float, key: float | number, number, number, number, number |
| `Perf.get` | — | — |
| `Reload.script` | dry_run: bool | boolean |
| `Reload.scene` | dry_run: bool | boolean |
| `Reload.files` | — | — |
| `Control.mcp` | method: string, request: string, name: string, arguments: json | string, string, string, object |

## 3. 脚本事件回调（callbacks）

引擎按名字调用脚本里的全局函数；`触发` 列为对应输入事件（无则非输入触发）。

| 回调 | 参数 | 触发 |
|------|------|------|
| `on_start` | — | — |
| `update` | dt: float | — |
| `on_pointer_down` | target: float, x: float, y: float | pointer_down |
| `on_pointer_up` | target: float, x: float, y: float | pointer_up |
| `on_pointer_move` | target: float, x: float, y: float | pointer_move |
| `on_wheel` | target: float, x: float, y: float | pointer_wheel |
| `on_click` | target: float, x: float, y: float | click |
| `on_key_down` | target: float, key: float | key_down |
| `on_key_up` | target: float, key: float | key_up |
| `on_text` | target: float, text: string | text |

## 4. 插件接口（interface → C vtable）

跨 `.so` 的唯一函数表；宿主经 `maque::module`(dlopen) 加载并校验版本/结构大小。

### renderer v1 — `undefined`

记录（POD）：

- `view`：vx: float, vy: float, vw: float, vh: float, pan: float, eye: float, cx: float, cy: float, zoom: float, rot: float
- `item`：x: float, y: float, size: float, r: float, g: float, b: float, a: float, texture: int32_t, rotation: float, uv0: float, uv1: float, uv2: float, uv3: float, layer: int32_t, z: int32_t

| 方法 | 参数 | 返回 |
|------|------|------|
| `create` | out_backend: const char** | self |
| `destroy` | self: self | void |
| `create_texture` | self: self, rgba: const uint8_t*, w: int32_t, h: int32_t | int32_t |
| `has_texture` | self: self, id: int32_t | int32_t |
| `render_views` | self: self, tw: int32_t, th: int32_t, views: const mq_renderer_view*, nviews: uint32_t, items: const mq_renderer_item*, count: uint32_t, bg: const float*, out: uint8_t*, out_size: uint32_t | int32_t |

## 5. 模块 C ABI（module_abi）

每模块的公开 C 头由 DSL 生成（原手写头是 `#include "abi/<mod>.h"` shim）；
`language: cxx` 的模块生成命名空间声明。

### math — `maque.math` v1（C++）

头文件：`build/modules/core/data/generated/abi/math.h` · 宏：`MQ_MATH_ABI_VERSION` / `_IFACE_NAME`

数学库：对生成 POD 类型的确定性操作（布局即 ABI，docs/06/07）

| 函数 | 返回 | 参数 |
|------|------|------|
| `dot` | float | a: const Vec2&, b: const Vec2& |
| `length_sq` | float | a: const Vec2& |
| `length` | float | a: const Vec2& |
| `lerp` | Vec2 | a: const Vec2&, b: const Vec2&, t: float |
| `normalize` | Vec2 | a: const Vec2& |
| `dot` | float | a: const Vec3&, b: const Vec3& |
| `cross` | Vec3 | a: const Vec3&, b: const Vec3& |
| `length_sq` | float | a: const Vec3& |
| `length` | float | a: const Vec3& |
| `distance` | float | a: const Vec3&, b: const Vec3& |
| `lerp` | Vec3 | a: const Vec3&, b: const Vec3&, t: float |
| `normalize` | Vec3 | a: const Vec3& |
| `dot` | float | a: const Vec4&, b: const Vec4& |
| `lerp` | Vec4 | a: const Vec4&, b: const Vec4&, t: float |
| `quat_identity` | Quat | — |
| `quat_conjugate` | Quat | q: const Quat& |
| `quat_normalize` | Quat | q: const Quat& |
| `quat_from_axis_angle` | Quat | axis: const Vec3&, radians: float |
| `quat_rotate` | Vec3 | q: const Quat&, v: const Vec3& |
| `quat_slerp` | Quat | a: Quat, b: Quat, t: float |
| `mat4_identity` | Mat4 | — |
| `transform_point` | Vec3 | a: const Mat4&, p: const Vec3& |
| `transform_vector` | Vec3 | a: const Mat4&, v: const Vec3& |
| `mat4_translation` | Mat4 | t: const Vec3& |
| `mat4_scale` | Mat4 | s: const Vec3& |
| `mat4_from_quat` | Mat4 | q: const Quat& |
| `mat4_from_transform` | Mat4 | t: const Transform3D& |
| `to_euler_xyz` | Vec3 | q: const Quat& |

### asset — `maque.asset` v1

头文件：`build/modules/core/data/generated/abi/asset.h` · 宏：`MQ_ASSET_ABI_VERSION` / `_IFACE_NAME`

资产管线：导入 → 依赖图 → 烘焙 → 打包 → dump（docs/08、ADR-0009）

枚举：

- (匿名)：MQ_ASSET_OK=0, MQ_ASSET_ERR_ARG=1, MQ_ASSET_ERR_NOT_FOUND=2, MQ_ASSET_ERR_IO=3, MQ_ASSET_ERR_FORMAT=4, MQ_ASSET_ERR_NO_MEM=5, MQ_ASSET_ERR_DEPENDENCY=6, MQ_ASSET_ERR_DUPLICATE=7
- `mq_asset_kind`：MQ_ASSET_KIND_OTHER=0, MQ_ASSET_KIND_TEXTURE=1, MQ_ASSET_KIND_MATERIAL=2, MQ_ASSET_KIND_MESH=3, MQ_ASSET_KIND_FONT=4, MQ_ASSET_KIND_ANIMATION=5, MQ_ASSET_KIND_SCENE=6, MQ_ASSET_KIND_SCRIPT=7

结构体：

- `mq_asset_info`：id: uint64_t, kind: mq_asset_kind, source: char[256], cooked: char[256], source_hash: uint64_t, cooked_hash: uint64_t, dep_count: uint32_t, flags: uint32_t

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_asset_create` | mq_asset_db* | vfs: mq_vfs* |
| `mq_asset_destroy` | void | db: mq_asset_db* |
| `mq_asset_import` | int | db: mq_asset_db*, path: const char*, out: mq_asset_info* |
| `mq_asset_find` | int | db: mq_asset_db*, path: const char*, out: mq_asset_info* |
| `mq_asset_find_by_id` | int | db: mq_asset_db*, id: uint64_t, out: mq_asset_info* |
| `mq_asset_count` | uint32_t | db: mq_asset_db* |
| `mq_asset_info_at` | int | db: mq_asset_db*, index: uint32_t, out: mq_asset_info* |
| `mq_asset_add_dependency` | int | db: mq_asset_db*, path: const char*, dep_path: const char* |
| `mq_asset_dependency_count` | uint32_t | db: mq_asset_db*, path: const char* |
| `mq_asset_dependency_at` | int | db: mq_asset_db*, path: const char*, index: uint32_t, out: char*, out_cap: size_t |
| `mq_asset_toposort` | uint32_t | db: mq_asset_db*, out: char(*)[256], out_cap: uint32_t |
| `mq_asset_validate` | int | db: mq_asset_db*, missing_out: char*, missing_cap: size_t |
| `mq_asset_cook` | int | db: mq_asset_db*, path: const char*, out_path: const char* |
| `mq_asset_pack` | int | db: mq_asset_db*, paths: const char* const*, count: uint32_t, out_path: const char* |
| `mq_asset_dump_json` | const char* | db: mq_asset_db* |
| `mq_asset_count_kind` | uint32_t | db: mq_asset_db*, kind: mq_asset_kind |

### physics — `maque.physics` v1

头文件：`build/modules/core/data/generated/abi/physics.h` · 宏：`MQ_PHYSICS_ABI_VERSION` / `_IFACE_NAME`

确定性 2D 物理求解器（ADR-0028 回退路径；插件 vtable 冻结）

枚举：

- (匿名)：MQ_PHYSICS_OK=0, MQ_PHYSICS_ERR_INVALID_ARG=1, MQ_PHYSICS_ERR_INVALID_HANDLE=2, MQ_PHYSICS_ERR_OUT_OF_MEMORY=3, MQ_PHYSICS_ERR_CAPACITY=4, MQ_PHYSICS_ERR_NOT_SUPPORTED=5
- (匿名)：MQ_PHYSICS_BODY_STATIC=0, MQ_PHYSICS_BODY_KINEMATIC=1, MQ_PHYSICS_BODY_DYNAMIC=2
- (匿名)：MQ_PHYSICS_SHAPE_BALL=0, MQ_PHYSICS_SHAPE_CUBOID=1

结构体：

- `mq_physics_body`：index: uint32_t, generation: uint32_t
- `mq_physics_config`：struct_size: uint32_t, gravity_x: float, gravity_y: float, fixed_dt: float, solver_iterations: uint32_t, max_bodies: uint32_t, seed: uint64_t
- `mq_physics_body_desc`：struct_size: uint32_t, type: uint32_t, shape: uint32_t, x: float, y: float, vx: float, vy: float, radius: float, half_w: float, half_h: float, mass: float, restitution: float, friction: float, flags: uint32_t
- `mq_physics_contact`：a_index: uint32_t, b_index: uint32_t, nx: float, ny: float, depth: float
- `mq_physics_v1`：abi_version: uint32_t, name: const char*, create: mq_physics_world* (*)(const mq_physics_config*), destroy: void (*)(mq_physics_world*), add_body: int (*)(mq_physics_world*, const mq_physics_body_desc*, mq_physics_body*), remove_body: int (*)(mq_physics_world*, mq_physics_body), step: int (*)(mq_physics_world*), get_transform: int (*)(mq_physics_world*, mq_physics_body, float*, float*), set_transform: int (*)(mq_physics_world*, mq_physics_body, float, float), set_velocity: int (*)(mq_physics_world*, mq_physics_body, float, float), checksum: uint64_t (*)(mq_physics_world*)

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_physics_create` | mq_physics_world* | cfg: const mq_physics_config* |
| `mq_physics_destroy` | void | w: mq_physics_world* |
| `mq_physics_add_body` | int | w: mq_physics_world*, d: const mq_physics_body_desc*, out: mq_physics_body* |
| `mq_physics_remove_body` | int | w: mq_physics_world*, b: mq_physics_body |
| `mq_physics_body_count` | int | w: mq_physics_world* |
| `mq_physics_body_valid` | int | w: mq_physics_world*, b: mq_physics_body |
| `mq_physics_get_transform` | int | w: mq_physics_world*, b: mq_physics_body, x: float*, y: float* |
| `mq_physics_set_transform` | int | w: mq_physics_world*, b: mq_physics_body, x: float, y: float |
| `mq_physics_get_velocity` | int | w: mq_physics_world*, b: mq_physics_body, vx: float*, vy: float* |
| `mq_physics_set_velocity` | int | w: mq_physics_world*, b: mq_physics_body, vx: float, vy: float |
| `mq_physics_interpolate` | int | w: mq_physics_world*, b: mq_physics_body, alpha: float, x: float*, y: float* |
| `mq_physics_set_bounds` | int | w: mq_physics_world*, min_x: float, min_y: float, max_x: float, max_y: float |
| `mq_physics_step` | int | w: mq_physics_world* |
| `mq_physics_step_count` | uint32_t | w: mq_physics_world* |
| `mq_physics_checksum` | uint64_t | w: mq_physics_world* |
| `mq_physics_contact_count` | uint32_t | w: mq_physics_world* |
| `mq_physics_contact_at` | int | w: mq_physics_world*, i: uint32_t, out: mq_physics_contact* |
| `mq_physics_state_size` | size_t | w: mq_physics_world* |
| `mq_physics_save_state` | size_t | w: mq_physics_world*, out: void*, cap: size_t |
| `mq_physics_load_state` | int | w: mq_physics_world*, in: const void*, size: size_t |
| `mq_physics_query` | const mq_physics_v1* | abi_version: uint32_t |

### nav — `maque.nav` v1

头文件：`build/modules/core/data/generated/abi/nav.h` · 宏：`MQ_NAV_ABI_VERSION` / `_IFACE_NAME`

确定性 2D 网格寻路：A* + 视线平滑 + 固定步长追踪

枚举：

- (匿名)：MQ_NAV_OK=0, MQ_NAV_ERR_ARG=1, MQ_NAV_ERR_RANGE=2, MQ_NAV_ERR_BLOCKED=3, MQ_NAV_ERR_NOPATH=4, MQ_NAV_ERR_NOMEM=5, MQ_NAV_ERR_CAPACITY=6

结构体：

- `mq_nav_config`：struct_size: uint32_t, width: uint32_t, height: uint32_t, cell_size: float, allow_diagonal: uint8_t

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_nav_create` | mq_nav_world* | cfg: const mq_nav_config* |
| `mq_nav_destroy` | void | w: mq_nav_world* |
| `mq_nav_set_blocked` | int | w: mq_nav_world*, x: uint32_t, y: uint32_t, blocked: int |
| `mq_nav_is_blocked` | int | w: mq_nav_world*, x: uint32_t, y: uint32_t |
| `mq_nav_set_rect` | int | w: mq_nav_world*, x: uint32_t, y: uint32_t, width: uint32_t, height: uint32_t, blocked: int |
| `mq_nav_walkable_count` | uint32_t | w: mq_nav_world* |
| `mq_nav_cell_at` | int | w: mq_nav_world*, wx: float, wy: float, out_x: uint32_t*, out_y: uint32_t* |
| `mq_nav_cell_center` | void | w: mq_nav_world*, x: uint32_t, y: uint32_t, out_wx: float*, out_wy: float* |
| `mq_nav_is_walkable_at` | int | w: mq_nav_world*, wx: float, wy: float |
| `mq_nav_find_path` | int | w: mq_nav_world*, sx: float, sy: float, tx: float, ty: float, out_xy: float*, out_cap: uint32_t, out_count: uint32_t*, out_needed: uint32_t* |
| `mq_nav_line_of_sight` | int | w: mq_nav_world*, ax: float, ay: float, bx: float, by: float |
| `mq_nav_smooth` | uint32_t | w: mq_nav_world*, xy: float*, count: uint32_t |
| `mq_nav_step_towards` | int | w: mq_nav_world*, px: float*, py: float*, path_xy: const float*, count: uint32_t, index: uint32_t*, speed: float, dt: float, arrive_dist: float |
| `mq_nav_last_expanded` | uint32_t | w: mq_nav_world* |

### script — `maque.script` v1

头文件：`build/modules/core/data/generated/abi/script.h` · 宏：`MQ_SCRIPT_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_script_vm_create` | mq_script_vm* | — |
| `mq_script_vm_destroy` | void | vm: mq_script_vm* |
| `mq_script_eval` | int | vm: mq_script_vm*, src: const char*, filename: const char* |
| `mq_script_has` | int | vm: mq_script_vm*, fn: const char* |
| `mq_script_call0` | int | vm: mq_script_vm*, fn: const char*, ret: double* |
| `mq_script_call1` | int | vm: mq_script_vm*, fn: const char*, arg: double, ret: double* |
| `mq_script_call3` | int | vm: mq_script_vm*, fn: const char*, a: double, b: double, c: double, ret: double* |
| `mq_script_call2` | int | vm: mq_script_vm*, fn: const char*, a: double, b: double, ret: double* |
| `mq_script_call1s` | int | vm: mq_script_vm*, fn: const char*, a: double, s: const char*, ret: double* |
| `mq_script_call2s` | int | vm: mq_script_vm*, fn: const char*, a: double, b: double, s: const char*, ret: double* |
| `mq_script_call0s` | int | vm: mq_script_vm*, fn: const char*, s: const char*, ret: double* |
| `mq_script_last_error` | int | vm: mq_script_vm*, out: char*, cap: int |
| `mq_script_set_rng` | void | vm: mq_script_vm*, fn: mq_script_rng_fn, user: void* |
| `mq_script_gc` | void | vm: mq_script_vm* |
| `mq_script_register_num` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_num_fn, user: void* |
| `mq_script_register_str` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_str_fn, user: void* |
| `mq_script_register_ret_str` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_ret_str_fn, user: void* |
| `mq_script_register_mixed` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_mixed_fn, user: void* |
| `mq_script_register_multi` | int | vm: mq_script_vm*, name: const char*, kinds: const char*, fn: mq_script_multi_fn, user: void* |
| `mq_script_register_multi_ret_str` | int | vm: mq_script_vm*, name: const char*, kinds: const char*, fn: mq_script_multi_ret_str_fn, user: void* |

### input — `maque.input` v1

头文件：`build/modules/core/data/generated/abi/input.h` · 宏：`MQ_INPUT_ABI_VERSION` / `_IFACE_NAME`

枚举：

- `mq_input_phase`：MQ_INPUT_PHASE_CAPTURE=0, MQ_INPUT_PHASE_TARGET=1, MQ_INPUT_PHASE_BUBBLE=2
- `mq_input_event_type`：MQ_INPUT_POINTER_DOWN=0, MQ_INPUT_POINTER_UP=1, MQ_INPUT_POINTER_MOVE=2, MQ_INPUT_POINTER_WHEEL=3, MQ_INPUT_POINTER_ENTER=4, MQ_INPUT_POINTER_LEAVE=5, MQ_INPUT_CLICK=6, MQ_INPUT_DOUBLE_CLICK=7, MQ_INPUT_KEY_DOWN=8, MQ_INPUT_KEY_UP=9, MQ_INPUT_TEXT=10, MQ_INPUT_POINTER_CANCEL=11

结构体：

- `mq_input_target`：entity: int, handle: uint32_t, depth: int, z: int, local_x: float, local_y: float, consumed: int, stopped: int
- `mq_input_event`：type: uint32_t, phase: uint32_t, pointer_id: uint32_t, pointer_type: uint32_t, button: uint32_t, modifiers: uint32_t, key: uint32_t, text: char[16], x: float, y: float, dx: float, dy: float, scroll_x: float, scroll_y: float, pressure: float, target: int

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_input_create` | mq_input* | — |
| `mq_input_destroy` | void | in: mq_input* |
| `mq_input_begin_frame` | void | in: mq_input*, frame: const mq_input_frame*, frame_index: uint64_t |
| `mq_input_set_hit_test` | void | in: mq_input*, fn: mq_input_hit_test, user: void* |
| `mq_input_set_parent_fn` | void | in: mq_input*, fn: mq_input_parent_fn, user: void* |
| `mq_input_add_handler` | void | in: mq_input*, fn: mq_input_handler, user: void* |
| `mq_input_dispatch` | uint32_t | in: mq_input* |
| `mq_input_end_frame` | void | in: mq_input*, frame_index: uint64_t |
| `mq_input_quit` | int | in: mq_input* |
| `mq_input_pointer_pos` | void | in: mq_input*, pointer_id: uint32_t, x: float*, y: float* |
| `mq_input_pointer_down` | int | in: mq_input*, pointer_id: uint32_t |
| `mq_input_key_down` | int | in: mq_input*, key: uint32_t |
| `mq_input_focus` | int | in: mq_input* |
| `mq_input_set_focus` | void | in: mq_input*, entity: int |
| `mq_input_set_capture` | void | in: mq_input*, pointer_id: uint32_t, entity: int |
| `mq_input_get_capture` | int | in: mq_input*, pointer_id: uint32_t |
| `mq_input_release_capture` | void | in: mq_input*, pointer_id: uint32_t |
| `mq_input_frame_index` | int | in: mq_input* |
| `mq_input_record_begin` | void | in: mq_input* |
| `mq_input_record_end` | void | in: mq_input* |
| `mq_input_recording` | int | in: mq_input* |
| `mq_input_replay_begin` | int | in: mq_input*, log: const void*, size: size_t |
| `mq_input_replay_end` | void | in: mq_input* |
| `mq_input_replaying` | int | in: mq_input* |
| `mq_input_inject` | void | in: mq_input*, ev: const mq_input_event* |
| `mq_input_last_consumed` | int | in: mq_input* |
| `mq_input_log_bytes` | const void* | in: mq_input*, out_size: size_t* |
| `mq_input_log_json` | const char* | in: mq_input* |
| `mq_input_log_load_json` | int | in: mq_input*, json: const char* |
| `mq_input_log_set_meta` | void | in: mq_input*, key: const char*, value: const char* |
| `mq_input_log_get_meta` | const char* | in: mq_input*, key: const char* |

### control — `maque.control` v1

头文件：`build/modules/core/data/generated/abi/control.h` · 宏：`MQ_CONTROL_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_control_create` | mq_control* | — |
| `mq_control_destroy` | void | c: mq_control* |
| `mq_control_register` | void | c: mq_control*, method: const char*, fn: mq_control_handler, user: void* |
| `mq_control_dispatch` | const char* | c: mq_control*, request: const char* |
| `mq_control_method_count` | uint32_t | c: mq_control* |
| `mq_control_method_at` | const char* | c: mq_control*, index: uint32_t |
| `mq_control_set_tools_handler` | void | c: mq_control*, fn: mq_control_handler, user: void* |
| `mq_control_set_tools_json` | void | c: mq_control*, fn: mq_control_tools_json_fn, user: void* |
| `mq_control_dispatch_mcp` | const char* | c: mq_control*, request: const char* |

### vfs — `maque.vfs` v1

头文件：`build/modules/core/data/generated/abi/vfs.h` · 宏：`MQ_VFS_ABI_VERSION` / `_IFACE_NAME`

虚拟文件系统：挂载点 + 多协议后端（docs/04）

枚举：

- (匿名)：MQ_VFS_OK=0, MQ_VFS_ERR_ARG=1, MQ_VFS_ERR_NO_MOUNT=2, MQ_VFS_ERR_NOTFOUND=3, MQ_VFS_ERR_NOMEM=4, MQ_VFS_ERR_FORMAT=5, MQ_VFS_ERR_IO=6, MQ_VFS_ERR_HASH=7

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_vfs_create` | mq_vfs* | — |
| `mq_vfs_destroy` | void | vfs: mq_vfs* |
| `mq_vfs_mount` | int | vfs: mq_vfs*, mountpoint: const char*, source: const char* |
| `mq_vfs_add_mem` | int | vfs: mq_vfs*, name: const char*, data: const void*, size: size_t |
| `mq_vfs_mount_zip` | int | vfs: mq_vfs*, mountpoint: const char*, zip_path: const char* |
| `mq_vfs_exists` | int | vfs: mq_vfs*, path: const char* |
| `mq_vfs_register_builtin` | void | vfs: mq_vfs*, logical: const char*, data: const void*, size: size_t |
| `mq_vfs_read` | int | vfs: mq_vfs*, path: const char*, out: void**, out_size: size_t* |
| `mq_vfs_free` | void | p: void* |
| `mq_vfs_hash` | uint64_t | data: const void*, size: size_t |
| `mq_vfs_hash_path` | uint64_t | vfs: mq_vfs*, path: const char* |
| `mq_vfs_read_verified` | int | vfs: mq_vfs*, path: const char*, expect: uint64_t, out: void**, out_size: size_t* |
| `mq_vfs_resolve` | int | vfs: mq_vfs*, path: const char*, out: char*, out_cap: size_t |

### platform — `maque.platform` v1

头文件：`build/modules/core/data/generated/abi/platform.h` · 宏：`MQ_PLATFORM_ABI_VERSION` / `_IFACE_NAME`

枚举：

- `mq_platform_pointer_type`：MQ_PLATFORM_POINTER_MOUSE=0, MQ_PLATFORM_POINTER_TOUCH=1, MQ_PLATFORM_POINTER_PEN=2
- `mq_platform_pointer_event_type`：MQ_PLATFORM_POINTER_DOWN=0, MQ_PLATFORM_POINTER_UP=1, MQ_PLATFORM_POINTER_MOVE=2, MQ_PLATFORM_POINTER_WHEEL=3, MQ_PLATFORM_POINTER_CANCEL=4
- (匿名)：MQ_KEY_UNKNOWN=0, MQ_KEY_ESCAPE=1, MQ_KEY_SPACE=2, MQ_KEY_ENTER=3, MQ_KEY_TAB=4, MQ_KEY_BACKSPACE=5, MQ_KEY_LEFT=6, MQ_KEY_RIGHT=7, MQ_KEY_UP=8, MQ_KEY_DOWN=9, MQ_KEY_TEXT=10, MQ_KEY_A=32

结构体：

- `mq_platform_pointer_event`：pointer_id: uint32_t, type: uint32_t, pointer_type: uint32_t, button: uint32_t, x: float, y: float, dx: float, dy: float, pressure: float, scroll_x: float, scroll_y: float, modifiers: uint32_t
- `mq_platform_key_event`：key: uint32_t, scancode: uint32_t, modifiers: uint32_t, repeat: uint32_t, down: uint32_t, text: char[16]
- `mq_input_frame`：quit: int, focused: int, mouse_x: float, mouse_y: float, mouse_down: int, pointers: mq_platform_pointer_event*, pointer_count: uint32_t, pointer_cap: uint32_t, keys: mq_platform_key_event*, key_count: uint32_t, key_cap: uint32_t, resized: int, width: int, height: int

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_platform_create` | mq_platform* | title: const char*, w: int, h: int |
| `mq_platform_present` | int | p: mq_platform*, rgba: const void*, w: int32_t, h: int32_t |
| `mq_platform_simulate_pointer` | int | p: mq_platform*, kind: int32_t, x: float, y: float, button: int32_t |
| `mq_platform_destroy` | void | p: mq_platform* |
| `mq_platform_has_window` | int | p: mq_platform* |
| `mq_platform_pump` | void | p: mq_platform* |
| `mq_platform_poll` | uint32_t | p: mq_platform*, out: mq_input_frame* |
| `mq_platform_inject_pointer` | void | p: mq_platform*, ev: const mq_platform_pointer_event* |
| `mq_platform_inject_key` | void | p: mq_platform*, ev: const mq_platform_key_event* |
| `mq_platform_set_title` | void | p: mq_platform*, title: const char* |
| `mq_platform_request_quit` | void | p: mq_platform* |

### handle — `maque.handle` v1

头文件：`build/modules/core/data/generated/abi/handle.h` · 宏：`MQ_HANDLE_ABI_VERSION` / `_IFACE_NAME`

结构体：

- `mq_handle`：index: uint32_t, generation: uint32_t

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_handle_table_create` | mq_handle_table* | elem_size: size_t, elem_align: size_t |
| `mq_handle_table_destroy` | void | table: mq_handle_table* |
| `mq_handle_table_alloc` | mq_handle | table: mq_handle_table*, out_storage: void** |
| `mq_handle_table_free` | int | table: mq_handle_table*, h: mq_handle |
| `mq_handle_table_get` | void* | table: mq_handle_table*, h: mq_handle |
| `mq_handle_table_valid` | int | table: mq_handle_table*, h: mq_handle |
| `mq_handle_table_count` | uint32_t | table: mq_handle_table* |
| `mq_handle_table_capacity` | uint32_t | table: mq_handle_table* |
| `mq_handle_table_handle_at` | mq_handle | table: mq_handle_table*, index: uint32_t |

### module — `maque.module` v1

头文件：`build/modules/core/data/generated/abi/module.h` · 宏：`MQ_MODULE_ABI_VERSION` / `_IFACE_NAME`

结构体：

- `mq_api_header`：struct_size: uint32_t, abi_version: uint32_t

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_host_create` | mq_host* | — |
| `mq_host_destroy` | void | host: mq_host* |
| `mq_host_load` | int | host: mq_host*, path: const char*, out_module: mq_loaded_module** |
| `mq_host_unload` | void | host: mq_host*, module: mq_loaded_module* |
| `mq_host_module_count` | uint32_t | host: mq_host* |
| `mq_host_query` | void* | host: mq_host*, interface_name: const char*, min_version: uint32_t |

### obs — `maque.obs` v1

头文件：`build/modules/core/data/generated/abi/obs.h` · 宏：`MQ_OBS_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_obs_create` | mq_obs_ctx* | — |
| `mq_obs_destroy` | void | ctx: mq_obs_ctx* |
| `mq_obs_event` | void | ctx: mq_obs_ctx*, domain: const char*, level: int, event: const char* |
| `mq_obs_counter_add` | void | ctx: mq_obs_ctx*, name: const char*, delta: double |
| `mq_obs_gauge_set` | void | ctx: mq_obs_ctx*, name: const char*, value: double |
| `mq_obs_gauge_get` | double | ctx: mq_obs_ctx*, name: const char* |
| `mq_obs_dump_json` | const char* | ctx: mq_obs_ctx* |

### reflect — `maque.reflect` v1

头文件：`build/modules/core/data/generated/abi/reflect.h` · 宏：`MQ_REFLECT_ABI_VERSION` / `_IFACE_NAME`

结构体：

- `mq_visitor`：user: void*, on_enter: int (*)(void* user, const mq_type_desc* type, void* obj), on_exit: int (*)(void* user, const mq_type_desc* type, void* obj), on_field: int (*)(void* user, const mq_field_desc* field, const void* value_ptr)

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_reflect_create` | mq_reflect_ctx* | — |
| `mq_reflect_destroy` | void | ctx: mq_reflect_ctx* |
| `mq_reflect_load_schema` | int | ctx: mq_reflect_ctx* |
| `mq_reflect_register` | int | ctx: mq_reflect_ctx*, type: const mq_type_desc* |
| `mq_reflect_type_by_id` | const mq_type_desc* | ctx: mq_reflect_ctx*, type_id: uint32_t |
| `mq_reflect_type_by_name` | const mq_type_desc* | ctx: mq_reflect_ctx*, fq_name: const char* |
| `mq_reflect_count` | uint32_t | ctx: mq_reflect_ctx* |
| `mq_reflect_type_at` | const mq_type_desc* | ctx: mq_reflect_ctx*, index: uint32_t |
| `mq_reflect_field` | const mq_field_desc* | type: const mq_type_desc*, field_name: const char* |
| `mq_reflect_field_path` | const mq_field_desc* | ctx: mq_reflect_ctx*, type: const mq_type_desc*, path: const char*, out_absolute_offset: uint32_t* |
| `mq_reflect_get_f64` | int | ctx: mq_reflect_ctx*, obj: const void*, field: const mq_field_desc*, out: double* |
| `mq_reflect_set_f64` | int | ctx: mq_reflect_ctx*, obj: void*, field: const mq_field_desc*, value: double |
| `mq_reflect_get_i64` | int | ctx: mq_reflect_ctx*, obj: const void*, field: const mq_field_desc*, out: int64_t* |
| `mq_reflect_set_i64` | int | ctx: mq_reflect_ctx*, obj: void*, field: const mq_field_desc*, value: int64_t |
| `mq_reflect_visit` | int | ctx: mq_reflect_ctx*, obj: void*, type: const mq_type_desc*, visitor: const mq_visitor* |

### image — `maque.image` v1

头文件：`build/modules/core/data/generated/abi/image.h` · 宏：`MQ_IMAGE_ABI_VERSION` / `_IFACE_NAME`

图像解码（Wuffs）：PNG/JPEG/GIF/BMP → RGBA8(非预乘)

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_image_decode` | int | data: const void*, size: size_t, out_rgba: uint8_t**, out_w: uint32_t*, out_h: uint32_t*, err: char*, err_cap: size_t |
| `mq_image_free` | void | p: void* |

### persist — `maque.persist` v1

头文件：`build/modules/core/data/generated/abi/persist.h` · 宏：`MQ_PERSIST_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_persist_load_json` | int | ctx: mq_reflect_ctx*, type: const mq_type_desc*, json: const char*, len: size_t, out: void* |
| `mq_json_parse` | mq_json* | text: const char*, len: size_t |
| `mq_json_free` | void | doc: mq_json* |
| `mq_json_get` | const mq_json* | obj: const mq_json*, key: const char* |
| `mq_json_len` | uint32_t | arr: const mq_json* |
| `mq_json_at` | const mq_json* | arr: const mq_json*, i: uint32_t |
| `mq_json_num` | double | v: const mq_json*, def: double |
| `mq_json_str` | const char* | v: const mq_json*, def: const char* |
| `mq_json_object_each` | uint32_t | obj: const mq_json*, fn: mq_json_kv_fn, user: void* |
| `mq_persist_apply_json` | int | ctx: mq_reflect_ctx*, type: const mq_type_desc*, node: const mq_json*, out: void* |
| `mq_persist_write_json` | size_t | ctx: mq_reflect_ctx*, type: const mq_type_desc*, obj: const void*, out: char*, cap: size_t |

### persist_binary — `maque.persist_binary` v1

头文件：`build/modules/core/data/generated/abi/persist_binary.h` · 宏：`MQ_PERSIST_BINARY_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_persist_encode` | size_t | ctx: mq_reflect_ctx*, type: const mq_type_desc*, obj: const void*, out: void*, out_cap: size_t |
| `mq_persist_decode` | long | ctx: mq_reflect_ctx*, type: const mq_type_desc*, in: const void*, in_size: size_t, out: void* |
| `mq_snapshot_pool_create` | mq_snapshot_pool* | count: uint32_t, capacity_bytes: size_t |
| `mq_snapshot_pool_destroy` | void | pool: mq_snapshot_pool* |
| `mq_snapshot_pool_acquire` | mq_snapshot* | pool: mq_snapshot_pool* |
| `mq_snapshot_create` | mq_snapshot* | — |
| `mq_snapshot_destroy` | void | s: mq_snapshot* |
| `mq_snapshot_begin` | void* | s: mq_snapshot*, want: size_t |
| `mq_snapshot_commit` | void | s: mq_snapshot*, used: size_t |
| `mq_snapshot_bytes` | const void* | s: mq_snapshot*, out_size: size_t* |
| `mq_snapshot_set_frame` | void | s: mq_snapshot*, frame: uint64_t |
| `mq_snapshot_frame` | uint64_t | s: mq_snapshot* |
| `mq_snapshot_set_desc` | void | s: mq_snapshot*, desc: const char* |
| `mq_snapshot_desc` | const char* | s: mq_snapshot* |
| `mq_snapshot_clone` | mq_snapshot* | s: mq_snapshot* |
| `mq_snapshot_checksum` | uint64_t | s: mq_snapshot* |

### watcher — `maque.watcher` v1

头文件：`build/modules/core/data/generated/abi/watcher.h` · 宏：`MQ_WATCHER_ABI_VERSION` / `_IFACE_NAME`

枚举：

- `mq_watch_kind`：MQ_WATCH_MODIFIED=0, MQ_WATCH_REMOVED=1, MQ_WATCH_ADDED=2

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_watcher_create` | mq_watcher* | — |
| `mq_watcher_destroy` | void | w: mq_watcher* |
| `mq_watcher_add` | int | w: mq_watcher*, path: const char* |
| `mq_watcher_remove` | int | w: mq_watcher*, path: const char* |
| `mq_watcher_count` | uint32_t | w: mq_watcher* |
| `mq_watcher_path_at` | const char* | w: mq_watcher*, index: uint32_t |
| `mq_watcher_poll` | uint32_t | w: mq_watcher*, fn: mq_watch_fn, user: void* |
| `mq_watcher_set_stamp` | int | w: mq_watcher*, path: const char*, stamp: uint64_t |
| `mq_watcher_get_stamp` | uint64_t | w: mq_watcher*, path: const char* |
| `mq_watcher_stat` | uint64_t | path: const char* |

### net — `maque.net` v1

头文件：`build/modules/core/data/generated/abi/net.h` · 宏：`MQ_NET_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_net_create` | mq_net* | — |
| `mq_net_destroy` | void | net: mq_net* |

### host_scene — `maque.host_scene` v1

头文件：`build/modules/core/data/generated/abi/host_scene.h` · 宏：`MQ_HOST_SCENE_ABI_VERSION` / `_IFACE_NAME`

枚举：

- (匿名)：MQ_HOST_POINTER_DOWN=0, MQ_HOST_POINTER_UP=1, MQ_HOST_POINTER_MOVE=2, MQ_HOST_POINTER_WHEEL=3, MQ_HOST_POINTER_CANCEL=4

结构体：

- `mq_host_desc`：struct_size: uint32_t, width: int32_t, height: int32_t, renderer: const char*, flags: uint32_t

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_host_scene_create` | mq_host_scene* | scn_path: const char*, desc: const mq_host_desc* |
| `mq_host_scene_destroy` | void | s: mq_host_scene* |
| `mq_host_tick` | int | s: mq_host_scene*, out: uint8_t*, out_size: size_t |
| `mq_host_size` | void | s: mq_host_scene*, w: int*, h: int* |
| `mq_host_push_pointer` | void | s: mq_host_scene*, type: int, button: int, x: float, y: float |
| `mq_host_push_key` | void | s: mq_host_scene*, key: int, down: int, text: const char* |
| `mq_host_frame` | uint64_t | s: mq_host_scene* |
| `mq_host_checksum` | uint64_t | s: mq_host_scene* |
| `mq_host_should_quit` | int | s: mq_host_scene* |
| `mq_host_control` | const char* | s: mq_host_scene*, request: const char* |

### runtime — `maque.runtime` v1

头文件：`build/modules/core/data/generated/abi/runtime.h` · 宏：`MQ_RUNTIME_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_run_scene` | int | scn_path: const char* |
| `mq_run_scene_ex` | int | scn_path: const char*, frames_dir: const char* |

### text — `maque.text` v1

头文件：`build/modules/core/data/generated/abi/text.h` · 宏：`MQ_TEXT_ABI_VERSION` / `_IFACE_NAME`

字体加载/图集/文本布局/富文本标记解析（确定性纯函数，docs/13/17）

枚举：

- (匿名)：MQ_TEXT_OK=0, MQ_TEXT_ERR_ARG=1, MQ_TEXT_ERR_FONT=2, MQ_TEXT_ERR_CAP=3
- (匿名)：MQ_TEXT_ALIGN_BEGIN=0, MQ_TEXT_ALIGN_CENTER=1, MQ_TEXT_ALIGN_END=2
- (匿名)：MQ_TEXT_WRAP_NONE=0, MQ_TEXT_WRAP_WORD=1, MQ_TEXT_WRAP_CHAR=2

结构体：

- `mq_text_style`：struct_size: uint32_t, font_size: float, line_spacing: float, wrap: uint32_t, align: uint32_t, max_width: float, r: float, g: float, b: float, a: float
- `mq_text_glyph`：codepoint: uint32_t, x: float, y: float, w: float, h: float, u0: float, v0: float, u1: float, v1: float, r: float, g: float, b: float, a: float, size: float
- `mq_text_metrics`：width: float, height: float, line_count: uint32_t, glyph_count: uint32_t, baseline: float

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_font_load_file` | mq_font* | path: const char* |
| `mq_font_destroy` | void | font: mq_font* |
| `mq_font_builtin` | mq_font* | — |
| `mq_font_height` | float | font: mq_font*, size: float |
| `mq_font_advance` | float | font: mq_font*, codepoint: uint32_t, size: float |
| `mq_text_measure` | int | font: mq_font*, style: const mq_text_style*, text: const char*, out_metrics: mq_text_metrics* |
| `mq_text_layout` | int | font: mq_font*, style: const mq_text_style*, text: const char*, out: mq_text_glyph*, cap: uint32_t, out_count: uint32_t*, out_metrics: mq_text_metrics* |
| `mq_text_strip_markup` | int | text: const char*, out: char*, cap: uint32_t |
| `mq_font_atlas` | int | font: mq_font*, size: float, out_rgba: uint8_t*, cap: size_t, out_w: uint32_t*, out_h: uint32_t* |

### sim — `maque.sim` v1

头文件：`build/modules/core/data/generated/abi/sim.h` · 宏：`MQ_SIM_ABI_VERSION` / `_IFACE_NAME`

输入模拟器：把 .sim.json 场景回放为确定性输入事件流（开发/自动化/演示）

枚举：

- (匿名)：MQ_SIM_POINTER_MOVE=0, MQ_SIM_POINTER_DOWN=1, MQ_SIM_POINTER_UP=2, MQ_SIM_POINTER_WHEEL=3, MQ_SIM_KEY_DOWN=4, MQ_SIM_KEY_UP=5, MQ_SIM_TEXT=6
- (匿名)：MQ_SIM_SPACE_WINDOW=0, MQ_SIM_SPACE_WORLD=1

结构体：

- `mq_sim_event`：frame: uint64_t, kind: uint32_t, space: uint32_t, x: float, y: float, dx: float, dy: float, button: uint32_t, key: uint32_t, text: const char*

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_sim_create` | mq_sim* | — |
| `mq_sim_destroy` | void | sim: mq_sim* |
| `mq_sim_load_json` | int | sim: mq_sim*, json: const char*, len: size_t, err: char*, err_cap: size_t |
| `mq_sim_count` | uint32_t | sim: mq_sim* |
| `mq_sim_event_at` | int | sim: mq_sim*, index: uint32_t, out: mq_sim_event* |
| `mq_sim_due` | int | sim: mq_sim*, frame: uint64_t, out: mq_sim_event*, cap: uint32_t, out_count: uint32_t* |
| `mq_sim_cursor` | uint32_t | sim: mq_sim* |
| `mq_sim_finished` | int | sim: mq_sim* |
| `mq_sim_reset` | void | sim: mq_sim* |
| `mq_sim_set_loop` | void | sim: mq_sim*, loop: int |
| `mq_sim_loop` | int | sim: mq_sim* |
| `mq_sim_loaded` | int | sim: mq_sim* |
| `mq_sim_last_frame` | uint64_t | sim: mq_sim* |
