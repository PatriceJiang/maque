# 25 · runtime.cpp 拆分（组合根 → 分域编译单元）

> 上游：`docs/12`（场景）、`docs/13`（渲染）、`docs/28`（ImGui）、`docs/32`（场景实例）、
> `docs/34`（脚本模块）、`plans/11-scene.md`。
> Owner agent：`agent-runtime`。

## 1. 背景与目标

`modules/runtime/src/runtime.cpp` 一个编译单元里同时是：场景管理（loader/runner/节点树）、
动画播放器、引擎实例、脚本引擎、ImGui 面板、渲染提取、Spine、资源缓存、控制面与 310 个
脚本 API。拆分前实测：

| 指标 | 拆分前 |
|---|---|
| 行数 / 体积 | 17,163 行 / 914 KB |
| 单 TU 编译 | 24–50 s（Debug+ASan 常态 30–45 s，`build-win/.ninja_log`） |
| 顶层定义 | ≈620：`Engine::` 170、`Scene::` 17、host API 310、控制面 57 |
| `Engine` 结构体 | 845 行 / 129 数据成员 / 87 内联方法声明 |
| 最大函数 | `step_frame` 766 行、`extract_3d` 724 行 |
| 自由函数 | 337 个，295 个只用一次，仅 ~25 个跨关注点 |

**目标**（按优先级）：

1. 每个文件一个关注点、≤800 行（硬上限 1200）；改一个特性只动一个文件。
2. 增量编译从"改一行重编 40 s"降到秒级。
3. `Engine` 从状态袋变成**组合根**（阶段 2）；`step_frame` 的帧序显式化（阶段 3）。
4. **行为零变化**：确定性（world_trail）与像素逐字节不变。

**不做**：把 runtime 拆成独立 C 模块（它是依赖图顶端，Scene 的 ABI 要重新设计，收益不抵）；
引入 ECS/archetype（`AGENTS.md` 明确节点树是唯一模型）；顺手改容器类型或遍历顺序。

## 2. 不变量（每一步都必须满足）

1. 公开 ABI 不动：`include/runtime/*.h`、`maque_api.h`、`maque_control.h` 与生成物零改动。
2. 只搬不改：不改容器类型、遍历顺序、浮点表达式、日志文本。
3. 公开面之外的符号留在 `maque::rt`（内部命名空间），不进安装头。
4. 每个文件 ≤ 800 行；超了按子关注点再切。

## 3. 护栏：确定性 oracle

`tests/runtime_oracle.sh`（ctest `runtime_oracle`）：18 个场景覆盖各关注点，每个场景取两路信号

- **模拟状态**：`--frames 60` 退出行的 `world_trail`（组件/脚本/物理/粒子/实例）
- **渲染像素**：`--shot` 的 PNG 逐字节 sha256（提取/文本/骨骼/3D/后处理）

```bash
# 每步之前 record、之后 check（同一台机器、同一构建配置）
tests/runtime_oracle.sh <player> <repo> <work> record /tmp/baseline.txt
tests/runtime_oracle.sh <player> <repo> <work> check  /tmp/baseline.txt
```

ctest 里跑 `selfcheck`（fast 集，两遍互相比较，可移植）。

## 4. 结构落地（当前）

```
modules/runtime/src/
  runtime_common.h      # 共享类型 + 跨 TU 内部辅助声明 + 内联工具
  runtime_scene.h       # Scene / SceneInstanceRuntime / SnapshotWriter
  runtime_math3d.h      # m4_* 内联数学（extract_3d 逐骨骼调用）
  runtime_engine.h      # Engine（组合根）
  runtime.cpp           # 剩余编排：帧序、场景、实例、脚本宿主、setup/teardown
  api/        system node ui scene spine imgui physics nav audio storage   （310 个脚本 API）
  control/    system scene world render storage audio input                （57 个控制面方法）
  assets/     texture env gltf gltf_anim                                   （资源管线）
  render/     math3d targets shader_passes extract3d text                  （渲染）
  ui/         debug_panel engine_options                                   （ImGui/F4）
  anim/       spine                                                        （Spine 2D）
  systems/    anim audio particles nav                                     （模拟系统）
  engine/     tracy                                                        （诊断）
  gltf_model.{h,cpp}                                                       （glTF CPU 解析，原有）
```

## 5. 已完成步骤

| 步 | 提交 | 内容 | 行数 |
|---|---|---|---|
| S1 | `2d369ef` | 内部头（类型 + Engine/Scene）+ `namespace maque::rt` + oracle | 17,163 → 15,937 |
| S2 | `125e9f4` | 宿主 API 310 个 → `api/*`（按前缀 10 个文件） | → 13,922 |
| S3 | `125e9f4` | 控制面 57 个 → `control/*`（按域 7 个文件） | → 12,645 |
| S4 | `3079a34` | 资源/glTF → `assets/*`（texture/env/gltf/gltf_anim） | → 10,319 |
| S5 | `56b0bec` | Spine / ImGui 面板 / F4 选项 | → 9,063 |
| S6 | `5d57878` | 3D 数学 / 渲染目标 / 场景 shader / extract_3d | → 6,685 |
| S7 | `555ef26` | 系统（anim/audio/粒子/导航）+ 文本 + Tracy | → 5,663 |

每步都过了：全量构建 + oracle 18 场景逐字段一致 + 相关 ctest 子集全绿。

## 6. 剩余步骤（阶段 1 收尾）

| 步 | 目标文件 | 源区（S7 后行号） | 备注 |
|---|---|---|---|
| S8 | `script/module.cpp` | 17–294 | 包/模块 VM（docs/34 P0） |
| S9 | `scene/tree.cpp`、`scene/loader.cpp` | 2045–2394 | 树操作 + 反序列化/实例化 |
| S10 | `scene/instance.cpp`、`scene/instance_assets.cpp` | 2395–3286 | docs/32 实例与资源基址 |
| S11 | `scene/codec.cpp` | 354–516、3287–3622 | 快照/校验和/世界编解码 |
| S12 | `input/dispatch.cpp`、`engine/ui.cpp`、`systems/physics.cpp` | 3623–4057、4303–4728 | 命中/分发、UI 语义、物理组件 |
| S13 | `engine/{storage,canvas,sim,window,caps}.cpp` | 4058–4302、4817–5109 | 装配期子系统 |
| S14 | `engine/{setup,loop,crash}.cpp` | 1279–2044、4729–4816、5110–5368 | setup/teardown/主循环/崩溃上下文 |
| S15 | `engine/frame.cpp`、`entry.cpp` | 517–1278、5369–5663 | `step_frame`（整段搬，阶段 3 再拆）+ 音频生命周期 + 嵌入宿主 ABI |

S15 之后 `runtime.cpp` 只剩几十行的 `using`/注释，可整体删除或留作空的 TU。

## 7. 阶段 2（拆所有权，`Engine` 845 → ≤250 行）

只抽真正独立的六个子对象（每个都自带状态 + 方法 + 适配器）：

`AssetStore`（纹理解码/env/mesh/材质缓存）、`GltfRuntime`（models/states + 动画步进）、
`SpineRuntime`、`TextRenderer`（字体/图集/图元）、`DebugUi` + `EngineOptions`、`Systems`
（粒子/导航/音频/tween/schedule/clip）。

**不抽**：`ScriptHost` / `InstanceRegistry` / `FrameRenderer` —— 它们的算法天然是关于整个引擎的
编排，抽成对象只多一层 `->`。它们的状态块留在 `Engine` 里但成段、加注释、方法分文件。

可选（先量再定）：大状态块改用 `unique_ptr` 持有，让头文件只见到前置声明 —— 改 `Spine`
字段不再触发全部 TU 重编。

## 8. 阶段 3（帧管线阶段化）

`step_frame`（766 行）→ 10 个具名阶段，顺序即契约（docs/10 D1）：

```cpp
void Engine::step_frame(bool render_frame) {
    FrameCtx f{ .dt = dt, .render = render_frame };
    begin_frame(f);       // 帧号/计时/窗口 resize/orbit
    poll_control(f);      // 控制端点（帧边界一次）
    feed_input(f);        // 设备事件 → ImGui → 世界坐标 → 模拟器
    hotkeys(f);           // F2/F3/F4，并从本帧输入里消费掉
    frame_boundary(f);    // 热重载 / director.loadScene
    run_scripts(f);       // update → 帧边界 GC → 实例操作队列
    simulate(f);          // 动画/粒子/导航/物理/spine/音频/UI 布局
    take_snapshot(f);     // 校验和 + 周期快照
    if (f.render) render(f);
    finish_frame(f);      // 统计/Tracy/截图/墙钟
}
```

同时拆 `extract_3d`：`build_frame3d()`（收集） + `submit_frame3d()`（提交），每段 ≤150 行。

## 9. 实测的坑（新拆分必须遵守）

1. **自由函数必须落在 `maque::rt` 内**：文件级只写 `using namespace maque::rt;` 时，`Engine::x`
   的定义仍作用于同一实体，但**自由函数**会定义到全局命名空间 → 与头里的声明不是同一实体，
   链接期 undefined symbol（S4 踩过）。所以带自由函数的文件要把内容整段包进
   `namespace maque::rt { }`。
2. **`static` 的自由函数跨 TU 不可见**：`read_sim_scenario`、`read_f32`、`write_ppm` 等必须
   去掉 `static` 并在 `runtime_common.h` 声明才搬得走。
3. **`namespace fs = std::filesystem`**：一律放 `runtime_common.h` 的 `maque::rt` 内；
   全局再放一个会与 `using namespace maque::rt;` 撞名（S7 踩过）。
4. **hot 路径数学函数用 `inline` 头**：`m4_*` 在 extract_3d 里逐节点/逐骨骼调用，跨 TU 非内联
   会有实测开销 → `runtime_math3d.h`。
5. **行尾**：仓库 `core.autocrlf=true`，工作区是 CRLF；用脚本生成新文件后要统一成 CRLF，
   否则 `git ls-files --eol` 报 `w/mixed`。
6. **文档锚点**：`docs/bugs/{001,002,005}` 里有 4 处 `runtime.cpp#L<行号>`，拆分完成后改成
   "文件 + 符号名"。

## 10. 验收标准（DoD）

- [ ] `runtime.cpp` ≤ 200 行（或删除），单文件 ≤ 800 行
- [ ] 全量构建通过；`ctest` 全绿（当前 120 项）
- [ ] `runtime_oracle` 18 场景 `world_trail` + PNG 逐字节与拆分前一致
- [ ] 增量编译：改一个子系统 ≤ 8 s（当前单 TU 30–45 s）
- [ ] 公开 ABI 零改动（`git diff` 不含 `include/` 与 `generated/`）
- [ ] `Engine` ≤ 250 行（阶段 2）、`step_frame` 编排 ≤ 50 行（阶段 3）

## 11. 风险

| 风险 | 处置 |
|---|---|
| 拆分窗口内的并行改动冲突 | 每步独立提交、可回滚；拆分期避免同时改 runtime 行为 |
| 静态局部状态被复制成两份 | 只搬不复制；oracle 会抓到行为差异 |
| 头文件反向变慢（Engine 内联类型多） | 阶段 2 用 `unique_ptr` 隔开，先量后定 |
| 场景实例/脚本宿主交错，误切 | S9–S11 按函数边界切，不按行号硬切 |
