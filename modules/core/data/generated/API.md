# maque API 参考（自动生成）

> ⚠️ 本文件由 `modules/core/data/parser.js` 从 `schema/`（**唯一真相**）生成，**请勿手工编辑**。
> 重新生成：`cmake --build build --target maque_codegen`，再运行 `tools/gen_api_docs.sh` 同步到 `docs/api/`。

统计：脚本 API **284** 个 · 控制面方法 **49** 个 · 事件回调 **10** 个 · 模块 C ABI **26** 个 · 插件接口 **1** 个。

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

### math · `math.yaml`

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `math.clamp` | value: float, lo: float, hi: float | float |  |
| `math.lerp` | a: float, b: float, t: float | float |  |
| `math.saturate` | x: float | float | MANUAL → `math.custom.h` |

### storage · `io/storage.yaml`

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `storage.get` | key: string | string | ctx · 读键；不存在返回空串（用 exists 区分） |
| `storage.set` | key: string, value: string | float | ctx · 写键；返回 1/0 |
| `storage.remove` | key: string | float | ctx · 删键；返回 1/0 |
| `storage.exists` | key: string | float | ctx · 键是否存在 |
| `storage.keys` | — | string | ctx · 键列表（JSON 数组字符串） |
| `storage.info` | — | string | ctx · 自省：JSON {mode, root, ns}（AI/调试） |

### host（全局，无前缀） · `runtime/host.yaml`

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
| `entity_spawn` | — | float | ctx · 新建节点（返回单调 id）；entity_* 是 node_* 的**扁平别名**（按 id 直接寻址，同一模型） |
| `entity_set_pos` | id: float, x: float, y: float | void | ctx · 写 Transform 组件（世界坐标）；不要直接改 Entity::world_x/y（那是每帧展平的派生缓存） |
| `entity_x` | id: float | float | ctx · 读世界坐标（派生缓存；改位置用 entity_set_pos / node_set_position） |
| `entity_y` | id: float | float | ctx · 读世界坐标（派生缓存；改位置用 entity_set_pos / node_set_position） |
| `entity_count` | — | float | ctx · 节点总数（树的扁平视图） |
| `entity_destroy` | id: float | void | ctx · 销毁节点（帧末生效；与 node_destroy 相同） |
| `entity_name` | id: float | string | ctx · 节点名（空串 = 无/无此 id） |
| `env` | name: string | string | ctx |
| `entity_find` | name: string | float | ctx · 按名字找节点（返回 id；-1 = 没有） |
| `entity_valid` | id: float | float | ctx · id 是否仍有效（id 单调递增、永不复用） |
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
| `physics_contact_count` | — | float | ctx |
| `physics_contact_pair` | i: float | bool | ctx |
| `physics_contact_a` | i: float | float | ctx |
| `physics_contact_b` | i: float | float | ctx |
| `physics_contact_normal_x` | i: float | float | ctx |
| `physics_contact_normal_y` | i: float | float | ctx |
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
| `window_w` | — | float | ctx · 绘制区宽（设备像素；HiDPI 下大于 screen_w） |
| `window_h` | — | float | ctx |
| `canvas_scale` | — | float | ctx · 设计 → 设备 的等比缩放 |
| `canvas_scale_x` | — | float | ctx |
| `canvas_scale_y` | — | float | ctx |
| `canvas_offset_x` | — | float | ctx · 画布原点的绘制区偏移（留边） |
| `canvas_offset_y` | — | float | ctx |
| `canvas_device_scale` | — | float | ctx · 显示器内容缩放（HiDPI） |
| `canvas_safe_area` | edge: float | float | ctx · 安全区内缩（0=左 1=上 2=右 3=下，设备像素） |
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
| `spine_animation_count` | id: float | float | ctx · 动画个数（无 SpineSkeleton 组件 → 0） |
| `spine_animation_name` | id: float, index: float | string | ctx · 第 index 个动画名（声明序；越界 → ""） |
| `spine_animation_index` | id: float, name: string | float | ctx · 动画名 → 声明序下标（不存在 → -1） |
| `spine_play` | id: float, name: string, loop: float | float | ctx · 播放动画（loop != 0 循环）；成功 0，失败 -1（名字不存在/无实例） |
| `spine_current_animation` | id: float | string | ctx · track 0 当前动画名（未播放 → ""） |
| `spine_set_skin` | id: float, name: string | float | ctx · 换皮肤（名字不存在 → -1） |
| `spine_skin_count` | id: float | float | ctx |
| `spine_skin_name` | id: float, index: float | string | ctx |
| `spine_time` | id: float | float | ctx · track 0 播放头时间（秒） |
| `spine_set_time` | id: float, t: float | float | ctx · 设置播放头时间（确定性回放/定格用；下一帧 step 生效） |
| `spine_pages` | id: float | float | ctx · 图集页数（多页图集 = 多次绘制批次） |
| `spine_page_texture` | id: float, index: float | float | ctx · 第 index 页的引擎纹理句柄（-1 = 无/越界；用于确认纹理真的上传了） |
| `spine_set_animation` | id: float, track: float, name: string, loop: float | float | ctx · 指定轨播动画（IK: track0=walk、track1=aim）；成功 0，失败 -1 |
| `spine_add_animation` | id: float, track: float, name: string, loop: float, delay: float | float | ctx · 排队：delay 秒后播（Raptor: track1 gun-grab false 2） |
| `spine_track_count` | id: float | float | ctx · 有轨条目的轨数 |
| `spine_track_animation` | id: float, track: float | string | ctx · 该轨当前动画名（空轨 → ""） |
| `spine_queue_count` | id: float, track: float | float | ctx · 该轨排队中的条目数（addAnimation 之后还剩几个；官方 SkeletonAnimation 的 queue 语义） |
| `spine_clear_track` | id: float, track: float | float | ctx |
| `spine_track_time` | id: float, track: float | float | ctx · 该轨播放头时间（秒）；多轨各自独立推进 |
| `spine_set_track_time` | id: float, track: float, t: float | float | ctx · 设该轨播放头（设完立刻 step(0) 重算姿势） |
| `spine_track_time_scale` | id: float, track: float | float | ctx |
| `spine_set_track_time_scale` | id: float, track: float, scale: float | float | ctx |
| `spine_time_scale` | id: float | float | ctx · AnimationState 全局时间缩放（官方 Coin/Tank 的 setTimeScale(0.3)） |
| `spine_set_time_scale` | id: float, scale: float | float | ctx |
| `spine_default_mix` | id: float | float | ctx |
| `spine_set_default_mix` | id: float, duration: float | float | ctx · 换动画的默认混合时长（官方 C++ 入口 demo: 0.2） |
| `spine_set_mix` | id: float, from: string, to: string, duration: float | float | ctx · setMix(from,to,duration)（Batching walk→jump 0.2；Separator 0.4） |
| `spine_event_count` | id: float | float | ctx · 上一次帧边界以来发生的事件数（Spineboy: start/complete/event…） |
| `spine_event_type` | id: float, index: float | float | ctx · 事件类型：0=start 1=interrupt 2=end 3=complete 4=dispose 5=event（越界 → -1） |
| `spine_event_track` | id: float, index: float | float | ctx |
| `spine_event_animation` | id: float, index: float | string | ctx |
| `spine_event_name` | id: float, index: float | string | ctx · 用户事件名（仅 type=5；否则 ""） |
| `spine_event_int` | id: float, index: float | float | ctx |
| `spine_event_float` | id: float, index: float | float | ctx |
| `spine_event_string` | id: float, index: float | string | ctx |
| `spine_clear_events` | id: float | float | ctx |
| `spine_bone_count` | id: float | float | ctx |
| `spine_bone_name` | id: float, index: float | string | ctx |
| `spine_bone_find` | id: float, name: string | float | ctx · 骨骼名 → 序号（-1 = 无） |
| `spine_bone_value` | id: float, index: float, what: float | float | ctx · what: 0=局部x 1=局部y 2=世界x 3=世界y（世界坐标 = 骨架子空间、y 向上） |
| `spine_bone_set_position` | id: float, index: float, x: float, y: float | float | ctx · 设父空间局部位置 |
| `spine_bone_set_world_position` | id: float, index: float, x: float, y: float | float | ctx · 设世界位置（内部走父骨骼 worldToLocal，与官方 IK demo 同款） |
| `spine_update_world_transform` | id: float | float | ctx · 只重算世界变换（不 apply 动画）→ 设完骨骼位置后让 IK 链当帧跟随 |
| `spine_slot_count` | id: float | float | ctx |
| `spine_slot_name` | id: float, index: float | string | ctx |
| `spine_slot_index` | id: float, name: string | float | ctx · 槽名 → 声明序下标（= 官方 findSlot(name)->getData().getIndex()；-1 = 无） |
| `spine_slot_attachment` | id: float, index: float | string | ctx · 该槽当前附件名（"" = 空槽） |
| `spine_set_slots_range` | id: float, from: float, to: float | float | ctx · 只画声明序槽区间 [from,to]（to<0 → 到末尾；from<0 → 不限制）—— Separator demo 的前/后段拆分 |
| `spine_slots_from` | id: float | float | ctx |
| `spine_slots_to` | id: float | float | ctx |
| `spine_set_attachment` | id: float, slot_name: string, attachment_name: string, skin_name: string | float | ctx · 按皮肤名取附件设到槽（skin_name 空 → 当前皮肤；attachment_name 空 → 卸下） |
| `spine_skin_find` | id: float, name: string | float | ctx · 皮肤名 → 声明序下标（-1 = 无） |
| `spine_skin_create` | id: float, name: string | float | ctx · 新建空皮肤 → 返回**句柄**（>=0；失败 -1）。句柄由引擎持有，切场景/退出时释放 |
| `spine_skin_add` | id: float, handle: float, source_skin: string | float | ctx · 把骨架里名为 source_skin 的皮肤**并入**句柄（Skin::addSkin；同槽同名后者覆盖）；成功 0 |
| `spine_set_skin_obj` | id: float, handle: float | float | ctx · 把句柄指向的皮肤设为当前皮肤（Skin* 版 setSkin + 按 setup 姿势复位槽） |
| `spine_skin_compose` | id: float, names: string | float | ctx · 一步到位：逗号分隔皮肤名 → 合成新皮肤并设为当前（MixAndMatch: skin-base,nose/short,…）；成功 0 |
| `spine_skin_attachment_count` | id: float, handle: float | float | ctx · 皮肤附件数（handle<0 → 最近一次 create/compose 的那一个；用于断言合成真的合上了） |
| `spine_set_two_color` | id: float, on: float | float | ctx · 官方 setTwoColorTint 的忠实复刻：**本引擎网格恒定走两色着色路径**（暗色通道始终产出；素材无 dark 时是 (0,0,0,1) → 与关闭逐字节等价），故该开关不改变输出，只为让官方 demo 的调用点有落处（值照存进组件字段） |
| `spine_ik_count` | id: float | float | ctx |
| `spine_ik_name` | id: float, index: float | string | ctx |
| `spine_set_ik_mix` | id: float, index: float, mix: float | float | ctx |
| `spine_physics_count` | id: float | float | ctx · 物理约束个数（celestial-circus 有；普通骨架 0） |
| `spine_physics_name` | id: float, index: float | string | ctx |
| `spine_physics_translate` | id: float, x: float, y: float | float | ctx · 给所有物理约束一个位移冲量（官方 Physics demo） |
| `spine_physics_reset` | id: float | float | ctx |
| `spine_sequence_frames` | id: float, slot: float | float | ctx · 序列帧附件的总帧数（0 = 该槽不是序列帧附件） |
| `spine_sequence_index` | id: float, slot: float | float | ctx · 当前帧下标（-1 = setup 帧） |
| `spine_sequence_path` | id: float, slot: float | string | ctx · 当前帧路径名（Sequence::getPath） |
| `spine_sequence_value` | id: float, slot: float, what: float | float | ctx · what: 0=u 1=v 2=u2 3=v2 4=width 5=height 6=setup_index（逐帧不同 → 可断言真的换图了） |
| `spine_set_sequence_index` | id: float, slot: float, frame: float | float | ctx · 手动定格序列帧下标（-1 = setup 帧） |
| `imgui_ready` | — | float | ctx · ImGui 是否已初始化 |
| `imgui_enabled` | — | float | ctx · 面板是否启用（MAQUE_IMGUI=0 可关；默认关） |
| `imgui_set_enabled` | on: float | float | ctx · 运行时开关面板（关掉后不再占 GPU/输入） |
| `imgui_begin` | title: string | float | ctx · 开窗口 → 1=可见（须配对 imgui_end） |
| `imgui_end` | — | void | ctx |
| `imgui_begin_closed` | title: string, x: float, y: float, w: float, h: float | float | ctx · 指定位置/尺寸且默认打开 |
| `imgui_set_pos` | x: float, y: float | void | ctx |
| `imgui_set_size` | w: float, h: float | void | ctx |
| `imgui_same_line` | — | void | ctx |
| `imgui_separator` | — | void | ctx |
| `imgui_spacing` | — | void | ctx |
| `imgui_new_line` | — | void | ctx |
| `imgui_begin_disabled` | disabled: float | void | ctx |
| `imgui_end_disabled` | — | void | ctx |
| `imgui_begin_child` | id: string, w: float, h: float, border: float | float | ctx |
| `imgui_end_child` | — | void | ctx |
| `imgui_begin_menu_bar` | — | float | ctx |
| `imgui_end_menu_bar` | — | void | ctx |
| `imgui_begin_menu` | label: string | float | ctx |
| `imgui_end_menu` | — | void | ctx |
| `imgui_menu_item` | label: string, selected: float, enabled: float | float | ctx |
| `imgui_begin_tab_bar` | id: string | float | ctx |
| `imgui_end_tab_bar` | — | void | ctx |
| `imgui_begin_tab_item` | label: string | float | ctx |
| `imgui_end_tab_item` | — | void | ctx |
| `imgui_begin_table` | id: string, cols: float | float | ctx |
| `imgui_end_table` | — | void | ctx |
| `imgui_table_next_row` | — | void | ctx |
| `imgui_table_next_column` | — | void | ctx |
| `imgui_table_headers_row` | — | void | ctx |
| `imgui_text` | text: string | void | ctx |
| `imgui_text_wrapped` | text: string | void | ctx |
| `imgui_text_disabled` | text: string | void | ctx |
| `imgui_text_colored` | text: string, r: float, g: float, b: float, a: float | void | ctx |
| `imgui_label_text` | label: string, value: string | void | ctx |
| `imgui_bullet_text` | text: string | void | ctx |
| `imgui_button` | label: string | float | ctx · 点击 → 1 |
| `imgui_small_button` | label: string | float | ctx |
| `imgui_invisible_button` | id: string, w: float, h: float | float | ctx |
| `imgui_arrow_button` | id: string, dir: float | float | ctx |
| `imgui_checkbox` | label: string, value: float | float | ctx · 返回新值（0/1） |
| `imgui_radio_button` | label: string, active: float | float | ctx · 点了且变为选中 → 1 |
| `imgui_slider_float` | label: string, value: float, min: float, max: float | float | ctx · 返回新值 |
| `imgui_slider_int` | label: string, value: float, min: float, max: float | float | ctx · 返回新值（整数按 float 传） |
| `imgui_drag_float` | label: string, value: float, speed: float | float | ctx · 返回新值 |
| `imgui_drag_int` | label: string, value: float, speed: float | float | ctx |
| `imgui_input_float` | label: string, value: float | float | ctx · 返回新值（可键入） |
| `imgui_input_int` | label: string, value: float | float | ctx |
| `imgui_input_text` | label: string, text: string | string | ctx · 返回当前文本（引擎按 label 维持缓冲） |
| `imgui_combo` | label: string, current: float, items: string | float | ctx · items 是逗号分隔串；返回选中下标 |
| `imgui_list_box` | label: string, current: float, items: string | float | ctx |
| `imgui_selectable` | label: string, selected: float | float | ctx |
| `imgui_progress_bar` | fraction: float, w: float, h: float, overlay: string | void | ctx |
| `imgui_plot_value` | id: string, value: float | void | ctx · 把值推进该 id 的环形历史并画折线图（脚本不必自己存数组） |
| `imgui_color_edit` | label: string, packed: float | float | ctx · packed = r*65536+g*256+b（0..255）；返回新 packed |
| `imgui_tree_node` | label: string | float | ctx · 展开 → 1（须配 imgui_tree_pop） |
| `imgui_tree_pop` | — | void | ctx |
| `imgui_collapsing_header` | label: string | float | ctx · 展开 → 1 |
| `imgui_set_next_item_width` | w: float | void | ctx |
| `imgui_push_item_width` | w: float | void | ctx |
| `imgui_pop_item_width` | — | void | ctx |
| `imgui_push_id` | id: string | void | ctx |
| `imgui_pop_id` | — | void | ctx |
| `imgui_tooltip` | text: string | void | ctx · 悬停在本项时显示文本 |
| `imgui_is_item_hovered` | — | float | ctx |
| `imgui_is_item_active` | — | float | ctx |
| `imgui_is_window_hovered` | — | float | ctx |
| `imgui_set_item_default` | — | void | ctx |
| `imgui_show_demo` | — | float | ctx · 内建 ImGui 演示窗口（自检用） |
| `imgui_want_mouse` | — | float | ctx · 1 = ImGui 要吃鼠标 → 世界输入应被阻断 |
| `imgui_want_keyboard` | — | float | ctx |
| `audio_enabled` | — | float | ctx · 音频实例是否可用（MAQUE_AUDIO=off 时仍可逻辑播放） |
| `audio_play` | path: string, volume: float, loop: float, bus: string | double | ctx · 播放（VFS 路径，P0 仅 WAV）；返回 voice，0=失败 |
| `audio_stop` | voice: double | void | ctx · 停止并释放 voice |
| `audio_pause` | voice: double, paused: float | void | ctx |
| `audio_set_volume` | voice: double, volume: float | void | ctx |
| `audio_set_pitch` | voice: double, pitch: float | void | ctx |
| `audio_state` | voice: double | float | ctx · 0=停止 1=播放 2=暂停；-1=无效句柄 |
| `audio_playhead` | voice: double | float | ctx · 逻辑播放进度（秒，模拟时间；与设备是否出声无关） |
| `audio_set_listener` | x: float, y: float, dir_x: float, dir_y: float | void | ctx |
| `audio_master_volume` | volume: float | void | ctx |
| `audio_bus_volume` | bus: string, volume: float | void | ctx |
| `imgui_hovered` | — | float | ctx · 指针是否落在面板上 |
| `imgui_frame` | — | float | ctx · 面板已渲染的帧数（自省：证明真的在跑） |
| `imgui_last_cmd_count` | — | float | ctx · 上一帧导出的绘制命令数（0 = 面板没画东西） |

## 2. 控制面方法（control_list，AI/MCP）

JSON-RPC（`{"jsonrpc":"2.0","id":N,"method":"<方法>","params":{…}}`）；
同一份声明还生成 MCP `tools/list`（含类型化 `inputSchema`）。

| 方法 | 参数 | MCP 类型 |
|------|------|----------|
| `Control.info` | — | — |
| `Font.info` | — | — |
| `Font.glyphs` | index: float, size: float | number, number |
| `Font.atlas` | index: float, size: float, offset: float, max: float | number, number, number, number |
| `Capture.screen` | offset: float, max: float, max_dim: float | number, number, number |
| `Types.describe` | — | — |
| `Scene.info` | — | — |
| `Scene.entities` | — | — |
| `Scene.emit` | id: float, event: string, value: float | number, string, number |
| `Scene.get` | id: float, path: string | number, string |
| `Scene.set` | id: float, path: string, value: json, dry_run: bool | number, string, object, boolean |
| `Scene.spawn` | name: string, parent: float, component: string, dry_run: bool | string, number, string, boolean |
| `Scene.destroy` | id: float, dry_run: bool | number, boolean |
| `Scene.expand_prefabs` | dry_run: bool | boolean |
| `Scene.bake` | dry_run: bool | boolean |
| `Scene.save` | path: string | string |
| `Runtime.step` | frames: float | number |
| `Runtime.frame` | — | — |
| `Director.pause` | — | — |
| `Director.resume` | — | — |
| `Director.status` | — | — |
| `Audio.info` | — | — |
| `Audio.voices` | — | — |
| `Audio.play` | path: string, volume: float, loop: bool, bus: string | string, number, boolean, string |
| `Audio.stop` | voice: string | string |
| `Audio.set_master` | volume: float, mute: bool | number, boolean |
| `Audio.set_bus` | bus: string, volume: float, mute: bool | string, number, boolean |
| `Storage.info` | — | — |
| `Storage.list` | — | — |
| `Storage.get` | key: string | string |
| `Storage.put` | key: string, value: string | string, string |
| `Storage.remove` | key: string | string |
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
- `item`：x: float, y: float, size: float, r: float, g: float, b: float, a: float, texture: int32_t, rotation: float, half_w: float, half_h: float, filter: int32_t, snap: int32_t, uv0: float, uv1: float, uv2: float, uv3: float, layer: int32_t, z: int32_t
- `mesh`：vertices: const float*, uvs: const float*, colors: const float*, dark_colors: const float*, indices: const uint32_t*, vertex_count: uint32_t, first_index: uint32_t, index_count: uint32_t, base_vertex: uint32_t, texture: int32_t, blend: int32_t, pma: int32_t, layer: int32_t, clip_x: float, clip_y: float, clip_w: float, clip_h: float, z: int32_t
- `window`：kind: int32_t, display: void*, handle: void*, extra: void*, width: int32_t, height: int32_t
- `caps`：max_texture_size: int32_t, max_lights: int32_t, max_samples: int32_t, target_format: int32_t, depth_format: int32_t, direct_present: int32_t, hdr: int32_t, reserved: int32_t
- `texture_desc`：width: int32_t, height: int32_t, pixel_format: int32_t, kind: int32_t, mip_levels: int32_t, filter: int32_t, wrap_u: int32_t, wrap_v: int32_t, data: const uint8_t*, data_size: uint64_t
- `mesh_gpu`：vertices: const void*, vertex_bytes: uint64_t, vertex_stride: uint32_t, vertex_layout: int32_t, indices: const uint32_t*, index_count: uint32_t, bounds_min: float[3], bounds_max: float[3]
- `view3d`：view: float[16], proj: float[16], eye: float[3], viewport: float[4], exposure: float, clear_mode: int32_t, clear_color: float[4], layer_mask: int32_t
- `light`：type: int32_t, pos: float[3], dir: float[3], color: float[3], intensity: float, range: float, inner_cos: float, outer_cos: float, flags: int32_t, shadow_slot: int32_t
- `pbr_material`：base_color: float[4], emissive: float[3], metallic: float, roughness: float, normal_scale: float, occlusion_strength: float, alpha_cutoff: float, alpha_mode: int32_t, flags: int32_t, textures: int32_t[5], uv_scale: float[2], uv_offset: float[2], clearcoat: float, clearcoat_roughness: float, shading_model: int32_t, side: int32_t, flags2: int32_t, opacity: float, emissive_intensity: float, env_intensity: float, normal_scale_y: float, specular: float[3], shininess: float, specular_intensity: float, ior: float, sheen: float, sheen_roughness: float, sheen_color: float[3], iridescence: float, iridescence_ior: float, iridescence_thickness_min: float, iridescence_thickness_max: float, anisotropy: float, anisotropy_rotation: float, clearcoat_normal_scale: float, toon_steps: int32_t, depth_packing: int32_t, textures2: int32_t[3], normal_uv_scale: float[2], normal_uv_offset: float[2], uv1_mask: int32_t, textures3: int32_t[7], uv_rotation: float, normal_uv_rotation: float, textures4: int32_t[2]
- `draw3d`：mesh: int32_t, material: int32_t, model: float[16], submesh_first: int32_t, submesh_count: int32_t, flags: int32_t, layer: int32_t, sort_key: uint64_t
- `environment`：sky_mode: int32_t, sky_color: float[4], sky_texture: int32_t, env_color: float[4], env_texture: int32_t, env_irradiance_texture: int32_t, intensity: float, env_sky_color: float[4], env_ground_color: float[4], env_highlight: float, env_rotation: float
- `post`：exposure: float, tonemap: int32_t
- `frame3d`：width: int32_t, height: int32_t, views: const mq_renderer_view3d*, view_count: uint32_t, lights: const mq_renderer_light*, light_count: uint32_t, materials: const mq_renderer_pbr_material*, material_count: uint32_t, draws: const mq_renderer_draw3d*, draw_count: uint32_t, env: mq_renderer_environment, post: mq_renderer_post, items: const mq_renderer_item*, item_count: uint32_t, meshes: const mq_renderer_mesh*, mesh_count: uint32_t, views2d: const mq_renderer_view*, view2d_count: uint32_t, bg: float[4], out: uint8_t*, out_size: uint32_t

| 方法 | 参数 | 返回 |
|------|------|------|
| `create` | out_backend: const char** | self |
| `destroy` | self: self | void |
| `destroy_texture` | self: self, id: int32_t | void |
| `create_texture` | self: self, rgba: const uint8_t*, w: int32_t, h: int32_t | int32_t |
| `has_texture` | self: self, id: int32_t | int32_t |
| `render_views` | self: self, tw: int32_t, th: int32_t, views: const mq_renderer_view*, nviews: uint32_t, items: const mq_renderer_item*, count: uint32_t, bg: const float*, out: uint8_t*, out_size: uint32_t | int32_t |
| `render_mesh_views` | self: self, tw: int32_t, th: int32_t, views: const mq_renderer_view*, nviews: uint32_t, items: const mq_renderer_item*, count: uint32_t, meshes: const mq_renderer_mesh*, mesh_count: uint32_t, bg: const float*, out: uint8_t*, out_size: uint32_t | int32_t |
| `attach_window` | self: self, win: const mq_renderer_window* | int32_t |
| `resize_surface` | self: self, w: int32_t, h: int32_t | int32_t |
| `caps` | self: self, out: mq_renderer_caps* | int32_t |
| `create_texture2` | self: self, desc: const mq_renderer_texture_desc*, out_id: int32_t* | int32_t |
| `create_mesh` | self: self, data: const mq_renderer_mesh_gpu*, out_id: int32_t* | int32_t |
| `destroy_mesh` | self: self, id: int32_t | void |
| `render_frame3d` | self: self, frame: const mq_renderer_frame3d* | int32_t |
| `present` | self: self | int32_t |

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

### vfs — `maque.vfs` v1

头文件：`build/modules/core/data/generated/abi/vfs.h` · 宏：`MQ_VFS_ABI_VERSION` / `_IFACE_NAME`

虚拟文件系统：挂载点 + 多协议后端（docs/04）

枚举：

- (匿名)：MQ_VFS_OK=0, MQ_VFS_ERR_ARG=1, MQ_VFS_ERR_NO_MOUNT=2, MQ_VFS_ERR_NOTFOUND=3, MQ_VFS_ERR_NOMEM=4, MQ_VFS_ERR_FORMAT=5, MQ_VFS_ERR_IO=6, MQ_VFS_ERR_HASH=7, MQ_VFS_ERR_UNSUPPORTED=8

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

### storage — `maque.storage` v1

头文件：`build/modules/core/data/generated/abi/storage.h` · 宏：`MQ_STORAGE_ABI_VERSION` / `_IFACE_NAME`

键值存储：模式（file/mem/http(s)/ws(s)）可配 + (uid,session) 命名空间 + 固定读写协议

枚举：

- (匿名)：MQ_STORAGE_OK=0, MQ_STORAGE_ERR_ARG=1, MQ_STORAGE_ERR_MODE=2, MQ_STORAGE_ERR_NOTFOUND=3, MQ_STORAGE_ERR_NOMEM=4, MQ_STORAGE_ERR_IO=5, MQ_STORAGE_ERR_PROTO=6, MQ_STORAGE_ERR_UNSUPPORTED=7

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_storage_create` | mq_storage* | spec: const char*, uid: const char*, session: const char* |
| `mq_storage_destroy` | void | s: mq_storage* |
| `mq_storage_mode` | const char* | s: mq_storage* |
| `mq_storage_root` | const char* | s: mq_storage* |
| `mq_storage_ns` | const char* | s: mq_storage* |
| `mq_storage_put` | int | s: mq_storage*, key: const char*, data: const void*, size: size_t |
| `mq_storage_get` | int | s: mq_storage*, key: const char*, out: void**, out_size: size_t* |
| `mq_storage_free` | void | p: void* |
| `mq_storage_remove` | int | s: mq_storage*, key: const char* |
| `mq_storage_exists` | int | s: mq_storage*, key: const char* |
| `mq_storage_keys` | int | s: mq_storage*, out: char*, cap: size_t |

### net — `maque.net` v1

头文件：`build/modules/core/data/generated/abi/net.h` · 宏：`MQ_NET_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_net_create` | mq_net* | — |
| `mq_net_destroy` | void | net: mq_net* |

### platform — `maque.platform` v1

头文件：`build/modules/core/data/generated/abi/platform.h` · 宏：`MQ_PLATFORM_ABI_VERSION` / `_IFACE_NAME`

枚举：

- `mq_platform_pointer_type`：MQ_PLATFORM_POINTER_MOUSE=0, MQ_PLATFORM_POINTER_TOUCH=1, MQ_PLATFORM_POINTER_PEN=2
- `mq_platform_pointer_event_type`：MQ_PLATFORM_POINTER_DOWN=0, MQ_PLATFORM_POINTER_UP=1, MQ_PLATFORM_POINTER_MOVE=2, MQ_PLATFORM_POINTER_WHEEL=3, MQ_PLATFORM_POINTER_CANCEL=4
- (匿名)：MQ_KEY_UNKNOWN=0, MQ_KEY_ESCAPE=1, MQ_KEY_SPACE=2, MQ_KEY_ENTER=3, MQ_KEY_TAB=4, MQ_KEY_BACKSPACE=5, MQ_KEY_LEFT=6, MQ_KEY_RIGHT=7, MQ_KEY_UP=8, MQ_KEY_DOWN=9, MQ_KEY_TEXT=10, MQ_KEY_F1=1001, MQ_KEY_F2=1002, MQ_KEY_F3=1003, MQ_KEY_F4=1004, MQ_KEY_F5=1005, MQ_KEY_F6=1006, MQ_KEY_F7=1007, MQ_KEY_F8=1008, MQ_KEY_F9=1009, MQ_KEY_F10=1010, MQ_KEY_F11=1011, MQ_KEY_F12=1012, MQ_KEY_A=32

结构体：

- `mq_platform_pointer_event`：pointer_id: uint32_t, type: uint32_t, pointer_type: uint32_t, button: uint32_t, x: float, y: float, dx: float, dy: float, pressure: float, scroll_x: float, scroll_y: float, modifiers: uint32_t
- `mq_platform_key_event`：key: uint32_t, scancode: uint32_t, modifiers: uint32_t, repeat: uint32_t, down: uint32_t, text: char[16]
- `mq_input_frame`：quit: int, focused: int, mouse_x: float, mouse_y: float, mouse_down: int, pointers: mq_platform_pointer_event*, pointer_count: uint32_t, pointer_cap: uint32_t, keys: mq_platform_key_event*, key_count: uint32_t, key_cap: uint32_t, resized: int, width: int, height: int
- `mq_platform_native_window`：kind: int32_t, display: void*, handle: void*, extra: void*

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_platform_create` | mq_platform* | title: const char*, w: int, h: int |
| `mq_platform_present` | int | p: mq_platform*, rgba: const void*, w: int32_t, h: int32_t |
| `mq_platform_drawable_size` | void | p: mq_platform*, out_w: int32_t*, out_h: int32_t* |
| `mq_platform_content_scale` | float | p: mq_platform* |
| `mq_platform_screen_size` | void | p: mq_platform*, out_w: int32_t*, out_h: int32_t* |
| `mq_platform_display_changed` | int | p: mq_platform* |
| `mq_platform_sdl_window` | void* | p: mq_platform* |
| `mq_platform_get_native_window` | int | p: mq_platform*, out: mq_platform_native_window* |
| `mq_platform_draw_rect` | int | p: mq_platform*, rgba: const void*, w: int32_t, h: int32_t, dx: int32_t, dy: int32_t, dw: int32_t, dh: int32_t, filter: int32_t |
| `mq_platform_present_ui` | int | p: mq_platform* |
| `mq_platform_capture_now` | int | p: mq_platform* |
| `mq_platform_ensure_renderer` | void* | p: mq_platform* |
| `mq_platform_sdl_renderer` | void* | p: mq_platform* |
| `mq_platform_set_safe_area` | void | p: mq_platform*, l: float, t: float, r: float, b: float |
| `mq_platform_present_rect` | int | p: mq_platform*, rgba: const void*, w: int32_t, h: int32_t, dx: int32_t, dy: int32_t, dw: int32_t, dh: int32_t, filter: int32_t |
| `mq_platform_simulate_pointer` | int | p: mq_platform*, kind: int32_t, x: float, y: float, button: int32_t |
| `mq_platform_spawn_detached` | int | p: mq_platform*, exe: const char*, args: const char* |
| `mq_platform_process_stats` | int | p: mq_platform*, out_cpu_percent: double*, out_mem_bytes: uint64_t* |
| `mq_platform_destroy` | void | p: mq_platform* |
| `mq_platform_has_window` | int | p: mq_platform* |
| `mq_platform_pump` | void | p: mq_platform* |
| `mq_platform_poll` | uint32_t | p: mq_platform*, out: mq_input_frame* |
| `mq_platform_inject_pointer` | void | p: mq_platform*, ev: const mq_platform_pointer_event* |
| `mq_platform_inject_key` | void | p: mq_platform*, ev: const mq_platform_key_event* |
| `mq_platform_set_title` | void | p: mq_platform*, title: const char* |
| `mq_platform_request_quit` | void | p: mq_platform* |

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

### image — `maque.image` v1

头文件：`build/modules/core/data/generated/abi/image.h` · 宏：`MQ_IMAGE_ABI_VERSION` / `_IFACE_NAME`

图像编解码：解码（Wuffs）PNG/JPEG/GIF/BMP → RGBA8(非预乘)；编码 PNG（截图/预览）

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_image_decode` | int | data: const void*, size: size_t, out_rgba: uint8_t**, out_w: uint32_t*, out_h: uint32_t*, err: char*, err_cap: size_t |
| `mq_image_free` | void | p: void* |
| `mq_image_write_png` | int | path: const char*, w: uint32_t, h: uint32_t, rgba: const void*, stride: uint32_t |

### spine2d — `maque.spine2d` v1

头文件：`build/modules/core/data/generated/abi/spine2d.h` · 宏：`MQ_SPINE2D_ABI_VERSION` / `_IFACE_NAME`

Spine 2D 官方运行时（spine-cpp）的 C ABI 封装：骨架数据 + 实例 + 绘制数据（docs/assets/06 §2）

枚举：

- (匿名)：MQ_SPINE_OK=0, MQ_SPINE_ERR_ARG=1, MQ_SPINE_ERR_FORMAT=2, MQ_SPINE_ERR_NOTFOUND=3, MQ_SPINE_ERR_PAGE=4, MQ_SPINE_ERR_NOMEM=5
- `mq_spine_event_type`：MQ_SPINE_EVENT_START=0, MQ_SPINE_EVENT_INTERRUPT=1, MQ_SPINE_EVENT_END=2, MQ_SPINE_EVENT_COMPLETE=3, MQ_SPINE_EVENT_DISPOSE=4, MQ_SPINE_EVENT_EVENT=5
- `mq_spine_physics`：MQ_SPINE_PHYSICS_NONE=0, MQ_SPINE_PHYSICS_RESET=1, MQ_SPINE_PHYSICS_UPDATE=2, MQ_SPINE_PHYSICS_POSE=3
- `mq_spine_physics_field`：MQ_SPINE_PHYS_INERTIA=0, MQ_SPINE_PHYS_STRENGTH=1, MQ_SPINE_PHYS_DAMPING=2, MQ_SPINE_PHYS_MASS_INVERSE=3, MQ_SPINE_PHYS_WIND=4, MQ_SPINE_PHYS_GRAVITY=5, MQ_SPINE_PHYS_MIX=6

结构体：

- `mq_spine_page`：name: const char*, width: int, height: int, texture: uint64_t, pma: int
- `mq_spine_batch`：page: int, blend: int, first_index: uint32_t, index_count: uint32_t
- `mq_spine_draw`：vertex_count: int, index_count: int, batch_count: int, bounds_x: float, bounds_y: float, bounds_w: float, bounds_h: float
- `mq_spine_event`：type: int, track: int, animation: const char*, name: const char*, int_value: int, float_value: float, string_value: const char*
- `mq_spine_sequence_info`：frames: int, index: int, setup_index: int, id: int, path: const char*, u: float, v: float, u2: float, v2: float, width: int, height: int

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_spine_abi_version` | uint32_t | — |
| `mq_spine_data_create` | mq_spine_data* | skeleton_data: const void*, skeleton_size: size_t, skeleton_is_binary: int, atlas_data: const char*, atlas_dir_hint: const char*, load_texture: mq_spine_texture_cb, user: void* |
| `mq_spine_data_destroy` | void | data: mq_spine_data* |
| `mq_spine_data_animation_count` | int | data: mq_spine_data* |
| `mq_spine_data_animation_name` | const char* | data: mq_spine_data*, index: int |
| `mq_spine_data_animation_index` | int | data: mq_spine_data*, name: const char* |
| `mq_spine_data_skin_count` | int | data: mq_spine_data* |
| `mq_spine_data_skin_name` | const char* | data: mq_spine_data*, index: int |
| `mq_spine_data_page_count` | int | data: mq_spine_data* |
| `mq_spine_data_page` | int | data: mq_spine_data*, index: int, out: mq_spine_page* |
| `mq_spine_data_page_texture` | int | data: mq_spine_data*, index: int, texture: uint64_t |
| `mq_spine_instance_create` | mq_spine_instance* | data: mq_spine_data* |
| `mq_spine_instance_destroy` | void | inst: mq_spine_instance* |
| `mq_spine_instance_set_skin` | int | inst: mq_spine_instance*, name: const char* |
| `mq_spine_instance_play` | int | inst: mq_spine_instance*, animation: const char*, loop: int, track: int |
| `mq_spine_instance_stop` | void | inst: mq_spine_instance* |
| `mq_spine_instance_set_time` | int | inst: mq_spine_instance*, track: int, time: float |
| `mq_spine_instance_step` | int | inst: mq_spine_instance*, dt: float |
| `mq_spine_instance_time` | float | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_animation_name` | const char* | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_animation_complete` | int | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_set_transform` | void | inst: mq_spine_instance*, x: float, y: float, scale: float, flip_x: int, flip_y: int |
| `mq_spine_instance_set_color` | void | inst: mq_spine_instance*, r: float, g: float, b: float, a: float |
| `mq_spine_instance_color` | void | inst: mq_spine_instance*, r: float*, g: float*, b: float*, a: float* |
| `mq_spine_instance_bone_position` | int | inst: mq_spine_instance*, bone: const char*, x: float*, y: float* |
| `mq_spine_instance_set_float` | int | inst: mq_spine_instance*, target: int, index: int, field: int, value: float, blend: float |
| `mq_spine_instance_add_float` | int | inst: mq_spine_instance*, target: int, index: int, field: int, value: float, blend: float |
| `mq_spine_instance_get_float` | float | inst: mq_spine_instance*, target: int, index: int, field: int |
| `mq_spine_instance_ik_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_ik_name` | const char* | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_ik_set_mix` | int | inst: mq_spine_instance*, index: int, mix: float |
| `mq_spine_instance_transform_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_transform_name` | const char* | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_transform_position` | int | inst: mq_spine_instance*, index: int, x: float*, y: float* |
| `mq_spine_instance_draw` | int | inst: mq_spine_instance*, vertices: float*, uvs: float*, colors: float*, dark_colors: float*, indices: uint32_t*, batches: mq_spine_batch*, max_vertices: int, max_indices: int, max_batches: int, out: mq_spine_draw* |
| `mq_spine_instance_set_event_cb` | void | inst: mq_spine_instance*, cb: mq_spine_event_cb, user: void* |
| `mq_spine_instance_event_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_event` | int | inst: mq_spine_instance*, index: int, out: mq_spine_event* |
| `mq_spine_instance_clear_events` | void | inst: mq_spine_instance* |
| `mq_spine_instance_events_dropped` | int | inst: mq_spine_instance* |
| `mq_spine_instance_set_animation` | int | inst: mq_spine_instance*, track: int, animation: const char*, loop: int |
| `mq_spine_instance_add_animation` | int | inst: mq_spine_instance*, track: int, animation: const char*, loop: int, delay: float |
| `mq_spine_instance_track_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_track_animation` | const char* | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_queue_count` | int | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_clear_track` | void | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_set_track_time_scale` | int | inst: mq_spine_instance*, track: int, scale: float |
| `mq_spine_instance_track_time_scale` | float | inst: mq_spine_instance*, track: int |
| `mq_spine_instance_set_time_scale` | void | inst: mq_spine_instance*, scale: float |
| `mq_spine_instance_time_scale` | float | inst: mq_spine_instance* |
| `mq_spine_instance_set_default_mix` | void | inst: mq_spine_instance*, duration: float |
| `mq_spine_instance_default_mix` | float | inst: mq_spine_instance* |
| `mq_spine_instance_set_mix` | int | inst: mq_spine_instance*, from: const char*, to: const char*, duration: float |
| `mq_spine_data_skin_find` | int | data: mq_spine_data*, name: const char* |
| `mq_spine_data_skin_at` | mq_spine_skin* | data: mq_spine_data*, index: int |
| `mq_spine_skin_create` | mq_spine_skin* | name: const char* |
| `mq_spine_skin_destroy` | void | skin: mq_spine_skin* |
| `mq_spine_skin_add` | int | dst: mq_spine_skin*, src: mq_spine_skin* |
| `mq_spine_skin_name` | const char* | skin: mq_spine_skin* |
| `mq_spine_skin_attachment_count` | int | skin: mq_spine_skin* |
| `mq_spine_instance_set_skin_obj` | int | inst: mq_spine_instance*, skin: mq_spine_skin* |
| `mq_spine_instance_set_attachment` | int | inst: mq_spine_instance*, slot_name: const char*, attachment_name: const char*, skin_name: const char* |
| `mq_spine_instance_slot_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_slot_name` | const char* | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_slot_index` | int | inst: mq_spine_instance*, name: const char* |
| `mq_spine_instance_set_slots_range` | int | inst: mq_spine_instance*, from: int, to: int |
| `mq_spine_instance_slots_range` | int | inst: mq_spine_instance*, from: int*, to: int* |
| `mq_spine_instance_slot_attachment` | const char* | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_bone_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_bone_name` | const char* | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_bone_find` | int | inst: mq_spine_instance*, name: const char* |
| `mq_spine_instance_bone_parent` | int | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_bone_set_position` | int | inst: mq_spine_instance*, index: int, x: float, y: float |
| `mq_spine_instance_bone_position_at` | int | inst: mq_spine_instance*, index: int, x: float*, y: float* |
| `mq_spine_instance_bone_world_position` | int | inst: mq_spine_instance*, index: int, x: float*, y: float* |
| `mq_spine_instance_bone_set_world_position` | int | inst: mq_spine_instance*, index: int, x: float, y: float |
| `mq_spine_instance_update_world_transform` | int | inst: mq_spine_instance*, physics: int |
| `mq_spine_instance_step_ex` | int | inst: mq_spine_instance*, dt: float, physics: int |
| `mq_spine_instance_physics_count` | int | inst: mq_spine_instance* |
| `mq_spine_instance_physics_name` | const char* | inst: mq_spine_instance*, index: int |
| `mq_spine_instance_physics_translate` | void | inst: mq_spine_instance*, x: float, y: float |
| `mq_spine_instance_physics_set_float` | int | inst: mq_spine_instance*, index: int, field: int, value: float |
| `mq_spine_instance_physics_get_float` | float | inst: mq_spine_instance*, index: int, field: int |
| `mq_spine_instance_physics_reset` | void | inst: mq_spine_instance* |
| `mq_spine_attachment_sequence_info` | int | inst: mq_spine_instance*, slot_index: int, out: mq_spine_sequence_info* |
| `mq_spine_instance_set_sequence_index` | int | inst: mq_spine_instance*, slot_index: int, frame: int |

### script — `maque.script` v1

头文件：`build/modules/core/data/generated/abi/script.h` · 宏：`MQ_SCRIPT_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_script_vm_create` | mq_script_vm* | — |
| `mq_script_vm_create_lang` | mq_script_vm* | lang: const char* |
| `mq_script_lang_for_path` | const char* | path: const char* |
| `mq_script_backends` | int | out: char*, cap: int |
| `mq_script_lang` | const char* | vm: mq_script_vm* |
| `mq_script_vm_destroy` | void | vm: mq_script_vm* |
| `mq_script_bus_create` | mq_script_bus* | — |
| `mq_script_bus_add` | int | bus: mq_script_bus*, vm: mq_script_vm*, name: const char* |
| `mq_script_bus_count` | uint32_t | bus: mq_script_bus* |
| `mq_script_bus_swap` | mq_script_vm* | bus: mq_script_bus*, index: uint32_t, vm: mq_script_vm*, name: const char* |
| `mq_script_bus_vm_at` | mq_script_vm* | bus: mq_script_bus*, index: uint32_t |
| `mq_script_bus_name_at` | const char* | bus: mq_script_bus*, index: uint32_t |
| `mq_script_bus_has` | int | bus: mq_script_bus*, fn: const char* |
| `mq_script_bus_call` | int | bus: mq_script_bus*, fn: const char*, a: double, b: double, text: const char* |
| `mq_script_bus_call3` | int | bus: mq_script_bus*, fn: const char*, a: double, b: double, c: double, text: const char* |
| `mq_script_bus_gc` | void | bus: mq_script_bus* |
| `mq_script_bus_error_count` | uint32_t | bus: mq_script_bus* |
| `mq_script_bus_last_error` | int | bus: mq_script_bus*, out: char*, cap: int |
| `mq_script_bus_set_rng` | void | bus: mq_script_bus*, fn: mq_script_rng_fn, user: void* |
| `mq_script_bus_destroy` | void | bus: mq_script_bus* |
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
| `mq_script_frame_boundary` | void | vm: mq_script_vm* |
| `mq_script_register_num` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_num_fn, user: void* |
| `mq_script_register_str` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_str_fn, user: void* |
| `mq_script_register_ret_str` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_ret_str_fn, user: void* |
| `mq_script_register_mixed` | int | vm: mq_script_vm*, name: const char*, fn: mq_script_mixed_fn, user: void* |
| `mq_script_register_multi` | int | vm: mq_script_vm*, name: const char*, kinds: const char*, fn: mq_script_multi_fn, user: void* |
| `mq_script_register_multi_ret_str` | int | vm: mq_script_vm*, name: const char*, kinds: const char*, fn: mq_script_multi_ret_str_fn, user: void* |

### asset — `maque.asset` v1

头文件：`build/modules/core/data/generated/abi/asset.h` · 宏：`MQ_ASSET_ABI_VERSION` / `_IFACE_NAME`

资产管线：导入 → 依赖图 → 烘焙 → 打包 → dump（docs/08、ADR-0009）

枚举：

- (匿名)：MQ_ASSET_OK=0, MQ_ASSET_ERR_ARG=1, MQ_ASSET_ERR_NOT_FOUND=2, MQ_ASSET_ERR_IO=3, MQ_ASSET_ERR_FORMAT=4, MQ_ASSET_ERR_NO_MEM=5, MQ_ASSET_ERR_DEPENDENCY=6, MQ_ASSET_ERR_DUPLICATE=7
- `mq_asset_kind`：MQ_ASSET_KIND_OTHER=0, MQ_ASSET_KIND_TEXTURE=1, MQ_ASSET_KIND_MATERIAL=2, MQ_ASSET_KIND_MESH=3, MQ_ASSET_KIND_FONT=4, MQ_ASSET_KIND_ANIMATION=5, MQ_ASSET_KIND_SCENE=6, MQ_ASSET_KIND_SCRIPT=7, MQ_ASSET_KIND_AUDIO=8

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

### audio — `maque.audio` v1

头文件：`build/modules/core/data/generated/abi/audio.h` · 宏：`MQ_AUDIO_ABI_VERSION` / `_IFACE_NAME`

枚举：

- `mq_audio_status`：MQ_AUDIO_OK=0, MQ_AUDIO_ERR_ARG=1, MQ_AUDIO_ERR_FORMAT=2, MQ_AUDIO_ERR_IO=3, MQ_AUDIO_ERR_DEVICE=4, MQ_AUDIO_ERR_CAPACITY=5, MQ_AUDIO_ERR_HANDLE=6
- `mq_audio_backend`：MQ_AUDIO_BACKEND_AUTO=0, MQ_AUDIO_BACKEND_NULL=1, MQ_AUDIO_BACKEND_SDL=2
- `mq_audio_panning`：MQ_AUDIO_PANNING_STEREO=0, MQ_AUDIO_PANNING_SPATIAL=1, MQ_AUDIO_PANNING_HRTF=2
- `mq_audio_attenuation`：MQ_AUDIO_ATTEN_INVERSE=0, MQ_AUDIO_ATTEN_LINEAR=1, MQ_AUDIO_ATTEN_EXPONENTIAL=2, MQ_AUDIO_ATTEN_CURVE=3
- `mq_audio_play_state`：MQ_AUDIO_STOPPED=0, MQ_AUDIO_PLAYING=1, MQ_AUDIO_PAUSED=2

结构体：

- `mq_audio_desc`：struct_size: uint32_t, sample_rate: uint32_t, channels: uint32_t, buffer_frames: uint32_t, max_voices: uint32_t, backend: uint32_t, device: const char*
- `mq_audio_voice_desc`：struct_size: uint32_t, clip: const mq_audio_clip*, volume: float, pitch: float, pan: float, loop: uint32_t, bus: const char*, x: float, y: float, z: float, panning: uint32_t, attenuation: uint32_t, min_distance: float, max_distance: float, doppler: float
- `mq_audio_listener`：struct_size: uint32_t, x: float, y: float, z: float, dir_x: float, dir_y: float, dir_z: float
- `mq_audio_stats`：struct_size: uint32_t, sample_rate: uint32_t, channels: uint32_t, voices: uint32_t, callbacks: uint64_t, dropped: uint64_t

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_audio_create` | mq_audio* | desc: const mq_audio_desc* |
| `mq_audio_destroy` | void | a: mq_audio* |
| `mq_audio_load_memory` | int | a: mq_audio*, data: const void*, size: size_t, hint: const char*, out: mq_audio_clip** |
| `mq_audio_clip_release` | void | clip: mq_audio_clip* |
| `mq_audio_clip_info` | int | clip: const mq_audio_clip*, frames: uint32_t*, rate: uint32_t*, channels: uint32_t* |
| `mq_audio_play` | uint64_t | a: mq_audio*, d: const mq_audio_voice_desc* |
| `mq_audio_stop` | int | a: mq_audio*, voice: uint64_t |
| `mq_audio_pause` | int | a: mq_audio*, voice: uint64_t, paused: uint32_t |
| `mq_audio_set_voice` | int | a: mq_audio*, voice: uint64_t, volume: float, pitch: float, pan: float |
| `mq_audio_set_volume` | int | a: mq_audio*, voice: uint64_t, volume: float |
| `mq_audio_set_pitch` | int | a: mq_audio*, voice: uint64_t, pitch: float |
| `mq_audio_set_spatial` | int | a: mq_audio*, voice: uint64_t, x: float, y: float, z: float, panning: uint32_t, attenuation: uint32_t, min_distance: float, max_distance: float |
| `mq_audio_set_listener` | int | a: mq_audio*, l: const mq_audio_listener* |
| `mq_audio_set_bus` | int | a: mq_audio*, bus: const char*, volume: float, mute: uint32_t |
| `mq_audio_set_master` | int | a: mq_audio*, volume: float, mute: uint32_t |
| `mq_audio_update` | int | a: mq_audio*, dt: float |
| `mq_audio_mix` | int | a: mq_audio*, out: float*, frames: uint32_t |
| `mq_audio_voice_state` | int | a: mq_audio*, voice: uint64_t, state: uint32_t*, playhead: float* |
| `mq_audio_voice_at` | uint32_t | a: mq_audio*, index: uint32_t, voice: uint64_t*, state: uint32_t*, playhead: float* |
| `mq_audio_get_stats` | int | a: mq_audio*, out: mq_audio_stats* |

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

### text — `maque.text` v1

头文件：`build/modules/core/data/generated/abi/text.h` · 宏：`MQ_TEXT_ABI_VERSION` / `_IFACE_NAME`

字体加载/图集/文本布局/富文本标记解析（确定性纯函数，docs/13/17）

枚举：

- (匿名)：MQ_TEXT_OK=0, MQ_TEXT_ERR_ARG=1, MQ_TEXT_ERR_FONT=2, MQ_TEXT_ERR_CAP=3
- (匿名)：MQ_TEXT_ALIGN_BEGIN=0, MQ_TEXT_ALIGN_CENTER=1, MQ_TEXT_ALIGN_END=2
- (匿名)：MQ_TEXT_WRAP_NONE=0, MQ_TEXT_WRAP_WORD=1, MQ_TEXT_WRAP_CHAR=2

结构体：

- `mq_text_style`：struct_size: uint32_t, font_size: float, device_scale: float, pixel_snap: int32_t, hinting: int32_t, line_spacing: float, wrap: uint32_t, align: uint32_t, max_width: float, r: float, g: float, b: float, a: float
- `mq_text_glyph`：codepoint: uint32_t, x: float, y: float, w: float, h: float, ax: int32_t, ay: int32_t, u0: float, v0: float, u1: float, v1: float, r: float, g: float, b: float, a: float, size: float, device_size: float, x0: float, y0: float, bearing_x: int32_t, bearing_y: int32_t
- `mq_text_metrics`：width: float, height: float, line_count: uint32_t, glyph_count: uint32_t, baseline: float, ascent: float, device_scale: float

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
| `mq_font_atlas_generation` | uint32_t | font: mq_font*, size: float |
| `mq_font_glyph_count` | uint32_t | font: mq_font*, size: float |
| `mq_font_glyph_at` | int | font: mq_font*, size: float, index: uint32_t, out: mq_text_glyph* |
| `mq_font_atlas_size` | int | font: mq_font*, size: float, out_w: uint32_t*, out_h: uint32_t* |

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
| `mq_sim_normalize_key` | uint32_t | key: uint32_t |

### runtime — `maque.runtime` v1

头文件：`build/modules/core/data/generated/abi/runtime.h` · 宏：`MQ_RUNTIME_ABI_VERSION` / `_IFACE_NAME`

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_run_scene` | int | scn_path: const char* |
| `mq_run_scene_ex` | int | scn_path: const char*, frames_dir: const char* |

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
| `mq_control_is_mutating` | int | method: const char* |
| `mq_control_server_create` | mq_control_server* | c: mq_control*, spec: const char*, read_only: int |
| `mq_control_server_poll` | int | s: mq_control_server* |
| `mq_control_server_clients` | uint32_t | s: mq_control_server* |
| `mq_control_server_url` | const char* | s: mq_control_server* |
| `mq_control_server_rejected` | uint32_t | s: mq_control_server* |
| `mq_control_server_destroy` | void | s: mq_control_server* |
| `mq_control_dispatch_mcp` | const char* | c: mq_control*, request: const char* |

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

### canvas — `maque.canvas` v1

头文件：`build/modules/core/data/generated/abi/canvas.h` · 宏：`MQ_CANVAS_ABI_VERSION` / `_IFACE_NAME`

分辨率策略：设计分辨率 → 绘制区（设备像素）的缩放/偏移/安全区解析（纯函数）

枚举：

- (匿名)：MQ_CANVAS_FIT_NONE=0, MQ_CANVAS_FIT_WIDTH=1, MQ_CANVAS_FIT_HEIGHT=2, MQ_CANVAS_FIT_SHOW_ALL=3, MQ_CANVAS_FIT_NO_BORDER=4, MQ_CANVAS_FIT_EXACT=5
- (匿名)：MQ_CANVAS_ALIGN_BEGIN=0, MQ_CANVAS_ALIGN_CENTER=1, MQ_CANVAS_ALIGN_END=2

结构体：

- `mq_canvas_desc`：struct_size: uint32_t, design_w: float, design_h: float, fit: int32_t, align: int32_t, min_scale: float, max_scale: float, window_w: float, window_h: float, dpi_scale: float, safe_left: float, safe_top: float, safe_right: float, safe_bottom: float
- `mq_canvas_metrics`：scale: float, scale_x: float, scale_y: float, offset_x: float, offset_y: float, viewport_w: float, viewport_h: float, window_w: float, window_h: float, device_scale: float

| 函数 | 返回 | 参数 |
|------|------|------|
| `mq_canvas_resolve` | int | desc: const mq_canvas_desc*, out: mq_canvas_metrics* |
| `mq_canvas_window_to_design` | int | m: const mq_canvas_metrics*, wx: float, wy: float, out_dx: float*, out_dy: float* |
| `mq_canvas_design_to_window` | int | m: const mq_canvas_metrics*, dx: float, dy: float, out_wx: float*, out_wy: float* |
