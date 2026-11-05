# 014 · 反射与组件访问热路径：每帧遍历的查表 / 分配开销

- **类型**：性能（CPU，每帧）
- **状态**：fixed
- **影响面**：所有场景的每帧路径 —— 组件提取（`Scene::extract`）、世界校验和、UI/音频/动画步进、
  脚本按名读写；WASM 与桌面共用同一代码路径（WASM 上同样受益）
- **发现版本**：`maque_player` 采样 profile（见 §症状 的两张图；采样百分比不是精确计时）

## 症状

采样 profile 里，`Engine::step_frame` 的内部构成（同一帧，占总采样比）：

| 热点 | 占比 | 性质 |
|---|---:|---|
| `Engine::world_checksum` | 17.70% | 每帧全场景遍历 + 每节点堆分配 / 排序 |
| `Engine::compute_scene_visual_hash` | 14.87% | 每帧全场景遍历 + 每节点堆分配 / 排序 |
| `dispatch_event`（脚本 `update`） | 10.13% | 脚本工作量本身（引擎侧无多余分派） |
| `Scene::extract` | 8.11% | 渲染提取（构建 draw item） |
| `Engine::anim_step` | 5.80% | 无动画组件时也在全树遍历 |
| `Engine::ui_step` | 5.50% | 无 UI 组件时也在全树遍历 |
| `Engine::audio_step` | 5.50% | 无音源/听者时也在全树遍历 |

`world_checksum` 的子项（第二张图）几乎全是容器与查表开销，而不是哈希本身：

```text
Scene::checksum                 2032 (13.95%)
  std::vector<unsigned int>      563 ( 3.86%)   ← 每节点一次 vector + push_back
  <lambda_1>::operator()         308 ( 2.11%)   ← 字节哈希循环
  mq_reflect_type_by_id          209 ( 1.43%)   ← 每组件一次 unordered_map 查找
  unordered_map<...Component...> 178 ( 1.23%)   ← operator[] 二次查找
  unordered_map<...SceneNode...> 111 ( 0.76%)
  std::sort                      109 ( 0.75%) + 69 (0.47%)
  std::vector::push_back          77 ( 0.53%)
  std::_Hash::_Umap_traits        65 ( 0.45%)
  std::vector::~vector            55 ( 0.38%)
```

结论：**每一步都在为“找到组件/字段”重复付钱**，与实际模拟/渲染工作量无关。

## 最小复现

- 用任意采样 profiler（本轮图为 Superluminal 风格）跑任意场景，按 `step_frame` 展开；
- 或 `MAQUE_TRACE_STATS=1` 看每帧 `logic/render/present` 与 draw 数是否与工作量相称；
- 微基准（本轮用独立小 harness，未入库；数值可在同机复跑）：

| 操作 | 耗时 |
|---|---:|
| `unordered_map<std::string,…>::find(const char*)`（构造临时 string） | 39.0–60.7 ns |
| 透明查找 `find(std::string_view)`（`by_name` 改造后） | 2.75–3.47 ns |
| `mq_reflect_field(d, "pressed")`（哈希 + type switch + 字段 switch + strcmp） | 26–28 ns |
| 已解析字段走 `accessor->get_f64`（间接调用） | ≈4 ns |
| 直接成员 / `component_of<Id>`（含一次 map find） | 0.6 ns / ≈3 ns |

## 根因

1. **组件表只有 type_id，没有描述符**：`comps` 是 `type_id → vector<uint8_t>`，每次遍历都要
   `mq_reflect_type_by_id`（`unordered_map<uint32_t,…>`）反查描述符；每帧每实体多次。
2. **按名 API 的固定开销**：`mq_reflect_type_by_name` 每次构造临时 `std::string`（>15 字符必堆分配）；
   `comp_by_name` 的短名兜底会**扫全表 254 个类型**并逐类型构造 string；
   `mq_reflect_field` 是 138 路 `switch (type_id)` + 字段哈希 switch + `strcmp`。
3. **标量读取走 accessor 间接调用**：`field_get_bool` / `read_num` 优先 `f->accessor->get_f64`，
   不可内联，每个字段约 4ns。
4. **每帧“记账”函数自己也在分配**：`Scene::checksum` 与 `compute_scene_visual_hash` 每节点
   `std::vector` + `std::sort` + 二次 `find`；后者在没有 `RenderTexture` 的场景里**没有任何消费者**。
5. **逐帧系统无条件全树遍历**：`ui_step` / `audio_step` / `anim_step` 即使场景没有对应组件
   （`ui.*` / 音源 / `AnimationPlayer` 与 glTF 模型）也走完整棵树。

## 修复

按提交（都在 `runtime`，不改 ABI / 场景格式）：

| 提交 | 内容 |
|---|---|
| `7dbc4d7` | codegen 生成 `MQ_TID_<NS>_<NAME>` 与 `type_of<Id>` 反向表 + type id 唯一性校验；`component_of<Id>` / `component<T>`；`ui_pressed`/`entity_z`/`ui_*_value` 直读；`by_name` 透明查找；`field_get_bool`/`read_num` 标量按 offset 直读 |
| `757880d` | canvas/audio/window/extract3d/spine 内部改 `component<T>`；`node_get/set_*` 标注为脚本慢路径（引擎内部清零） |
| `f73d24a` | RenderTexture / Camera2D / Camera3D / scn.Camera 的提取与回写直读 |
| `323c71b` | shader pass 时间推进/提取直读 |
| `5fd4dd6` | spine 帧步进（每实体每帧 12 次按名反射 → 直接成员） |
| `4396840` | UI 布局/进度/安全区的 `ui.Control` 查找直读 |
| `9196ab1` | nav 直读；组件按名查找限制为**全名 `ns.Type`**（删掉前缀猜测 + 全表扫描） |
| `011a7f2` | `comps` 值类型改为 `ComponentSlot { desc, bytes }`（`data()/size()/empty()` 透传），33 处 `mq_reflect_type_by_id` 改为 `slot.desc`；快照恢复后校验不变量 |
| `afc1963` | `Scene::checksum` 改栈数组 + 插入排序（零分配，**哈希值逐位不变**）；`compute_scene_visual_hash` 无 RT 时跳过、有 RT 时零分配 |
| `6116fa2` | `has_ui_components` / `has_anim_components` / `has_audio_components`（装配/快照/动态挂载三处置位，切场景清零）→ 无对应组件时跳过三个每帧系统 |

关键设计点：

- `comps` 的槽结构体带描述符缓存，**单一写入口** `component_slot()` 维持 `desc->type_id == key`；
- 校验和仍按 type_id 升序混入（语义不变），只是不再为排序分配内存；
- 视觉哈希只做**进程内前后帧比较**，因此不需要树序，直接按 map 顺序混入；
- 存在性标志“只置位不清零”：多置位只多走一趟（正确），漏置位才会漏更新，所以三个写入点必须齐全。

## 回归与证据

- 微基准：按名查找 **39–61 ns → 2.8–3.5 ns**；`read_num`/`read_f32`/`read_str` 去掉 accessor 间接调用；
- 功能回归：19–20 个场景全部 exit=0（ball/ui/canvas/label/nodes/storage/anim/multilang/nested/
  td×2/pbr×2/render_texture/shadertoy/audio/spine×2/自测×2），Debug+ASan 构建；
- 确定性：`nodes.scn` / `anim.scn` / `pbr/imported.scn` 两次独立运行 `world_trail` 逐位一致；
- 守卫未误杀：`MAQUE_TRACE_UI=1` 在 `nodes.scn` 仍打印 `[ui] layout`；音频 e2e 的 `comp_state3=1` 照常；
  2D/glTF 动画的 `world_trail` 随帧数变化；spine bbox 在 frame 0→20 变化（spine 有独立 step）；
- 单测：`maque_reflect_test` / `maque_control_test` 全过；codegen `--check` ok + 重新生成 0 diff；
- ✅ 哈希语义：`Scene::checksum` 的混入顺序与 FNV 序列未变，**同一场景的 `world_trail` 值与改动前一致**。

⚠️ **端到端复测尚未做**：本轮只验证了功能、确定性与微基准；要确认 profile 上的百分比下降，
需在**同一场景、同一构建类型**下再采一次样（本项目默认 Debug+ASan，采样绝对值会比 Release 高）。

## 相关

- [06 · 反射与代码生成](../06-反射与代码生成.md) §5（零开销原则、`type_id_v` / `type_of`）
- [07 · 内存与数据模型](../07-内存与数据模型.md) §5（组件槽 = 描述符缓存 + blob）
- [29 · Tracy 性能分析](../29-Tracy性能分析.md)（采样入口与 zone）
- 硬约定：AGENTS.md「内置组件不查字段名」「跨模块只暴露 C ABI」
- 后续可做：`Scene::extract` 的每帧临时分配（`collect_preorder` 缓冲复用、`out.reserve`）；
  `dispatch_event` 属脚本工作量，需从脚本侧优化。

## 回归命令（复测用）

```bash
# 功能（本机 Debug+ASan；e2e 需 Git Bash）
ctest --test-dir build-win --output-on-failure
# 确定性（同一命令两跑，world_trail 必须逐位一致）
./build-win/samples/player/maque_player.exe --frames 30 scenes/nodes/nodes.scn
# 采样
MAQUE_TRACE_STATS=1 ./build-win/samples/player/maque_player.exe --frames 600 scenes/td/level1.scn
```
