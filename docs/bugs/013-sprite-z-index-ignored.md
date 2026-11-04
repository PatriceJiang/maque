# 013 · `scn.Sprite` 的 `node_set_z_index` 被静默忽略

- **类型**：正确性（2D 渲染图层）
- **状态**：fixed
- **影响面**：`modules/runtime` 的 2D sprite 提取；所有依赖节点 z 的 `.scn`（含嵌套场景实例）
- **发现版本**：2026-11-04，重写 `samples/tank` 时用“树盖坦克 / 过场文字盖地形”的截图对照发现
- **修复**：`modules/runtime/src/runtime.cpp`（`Scene::extract`）

## 症状

- `node_set_z_index(id, 9000)` 之后 `node_z_index(id)` 确实返回 9000；
- 但渲染顺序仍等于**实体创建顺序**（`it.z = id`）：
  - 树（后创建的 stage 节点）与坦克（早期 prefab 池）之间无法按设计分层；
  - 嵌套 HUD 场景里的过场文字（早期创建）被后创建的 stage 砖块/钢块盖住；
  - 只有纯色叠色探针（两个 root 场景实体）也按 id，而不是 z。

## 最小复现

`tests/data/zorder/zorder.scn` + `zorder.lua`：

```lua
-- 红块 z=0，绿块 z=9000，同一位置同一尺寸
solid(160, 120, 1, 0, 0, 0)
solid(160, 120, 0, 1, 0, 9000)
```

```bash
maque_player --shot out.ppm --shot-frame 2 tests/data/zorder/zorder.scn
# 修复前：中心像素 = (255,0,0)
# 修复后：中心像素 = (0,255,0)
```

## 根因

`Scene::extract` 的 sprite 分支只写：

```cpp
it.z = (int)idx;   // 稳定序，但 z_index 完全没读
```

UI 控件（`MQ_ROLE_UI_BUTTON`）与 Spine 提取各自写了 `z_index * 100000 + id`；sprite 分支漏了。
渲染器 `gfx` 本身是 `layer → z → 原始下标` 稳定排序，问题只在运行时没有把 z 填进渲染项。

## 修复

```cpp
it.layer = 0;
/* 节点 z_index 必须参与排序；与 UI/Spine 同一套 "z_index*100000+id" 稳定键 */
it.z = e.z_index * 100000 + (int)idx;
```

- 保序：同 z 仍按实体 id（确定性，`docs/10`）。
- 兼容：没设 z 的旧场景 `z_index=0`，排序退化为实体 id，与旧行为一致。
- `samples/tank` 用到的分层：地形 10 / 坦克 20 / 子弹 30 / 树 40 / 特效 50 /
  HUD 100 / 位图文字 9000；stage tile 的 z 在父场景 `collect_stage()` 里设置。

## 回归与证据

- `tank_zorder`（CTest）：`tests/tank_e2e.sh zorder` 跑上面的探针，PPM 中心像素必须是绿色。
- `tank_smoke` / `tank_play` / `tank_rules`：树、暂停面板、结算面板在截图中可见（不再被地形盖住）。
- 修复后 `samples/tank/shot_pause.png` 的 `PAUSE / P ENTER TO RESUME` 完整显示在战场中央；
  修复前文字被钢块局部覆盖。

## 相关

- `docs/10-确定性.md`（稳定排序键）
- `docs/13-渲染.md`（layer/z/纹理批处理）
- `samples/tank/README.md`（场景分层与树/坦克用例）
