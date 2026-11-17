# 37 · DragonBones 2D 运行时（模块计划）

> 状态：**P0 已落地**（模块 + C ABI + 组件 + 脚本 API + 示例 + 测试全绿）。
> 设计/踩坑/能力矩阵的完整说明见 [docs/36](../docs/36-DragonBones运行时接入.md)。

## 1. 模块概述

- **职责**：解析 DragonBones 5.x 导出资源（`*_ske.json` 骨架 + `*_tex.json` 图集），
  逐帧求值（骨骼世界矩阵 / 槽显示 / 颜色 / FFD），导出**绘制数据**
  （顶点 / UV / 颜色 / 索引 / 批次）。不碰 GPU、不解码纹理。
- **所在层**：数据/世界（与 `spine2d` 并列），由 `runtime` 消费。
- **上游文档**：docs/36（本实现）、docs/02（C ABI）、docs/15（动画子系统）、
  docs/components/09 §4（组件"同构但不合并"）。
- **一句话**：与 Spine 同构的第二条 2D 骨骼运行时，但坐标轴（y 向下）与若干语义不同。

## 2. 接口层设计（已冻结）

- **公共头**：`modules/core/data/generated/abi/dragonbones.h`（由
  `modules/core/data/schema/world/dragonbones.yaml` 的 `module_abi` 生成，**单一真相**）。
- **不透明句柄**：`mq_db_data`（骨架数据，装配期建）/ `mq_db_instance`（实例，帧内用）。
  分配者释放：`create`/`destroy` 成对；页面纹理句柄归引擎（tex_cache）。
- **类型**：`mq_db_page` / `mq_db_batch` / `mq_db_draw` / `mq_db_anim` / `mq_db_texture_cb` +
  错误码（`MQ_DB_OK/ERR_ARG/ERR_FORMAT/ERR_NOTFOUND/ERR_PAGE/ERR_NOMEM`）+ 混合模式枚举。
- **操作面**：53 个函数 —— 数据（armature/动画/皮肤/骨骼/槽/页/诊断计数）、实例
  （play/stop/step/set_time/皮肤/骨骼世界位置/槽显示/draw）、变换与 tint。
- **错误模型**：结构化错误码；空指针/越界一律返回错误码而不是崩溃；`draw` 缓冲不足返回
  `ERR_NOMEM` 并在 `out` 里给出实际规模（调用方扩容重试）。
- **版本化**：`MQ_DRAGONBONES_ABI_VERSION=1`；结构体只追加字段。
- **线程/确定性**：单线程、帧内同步；固定 `dt`；不读随机数、不发异步 → 可重放。

## 3. 依赖与边界

- **可依赖**：`maque::persist`（只借它的 `mq_json_*` C ABI 做 JSON DOM）、标准库。
- **禁止**：依赖 gfx/image（纹理由引擎回填）、跨边界异常/RTTI/STL、全局可变状态。
- **与其它模块的接缝**：
  - runtime → 模块：VFS 读文件 + `mq_image_decode` + gfx 上传；
  - 模块 → runtime：绘制数据 + 批次（转 `mq_renderer_mesh`，与 Spine/ImGui 同一通道）。

## 4. 测试计划（先行）

| 层 | 测试 | 断言 |
|---|---|---|
| 模块单测 | `modules/anim/dragonbones/tests/dragonbones_test.cpp` | 真实素材（mecha_1004d/weapon_1004_show）解析计数、动画时长/循环、绘制规模/UV/包围盒、确定性、缓冲不足；**合成素材**覆盖 `inheritScale=false`、裁剪 pivot、皮肤切换、空皮肤 |
| e2e | `dragonbones_smoke` | 装配日志（页纹理句柄/尺寸）+ 绘制规模 56 顶点/84 索引 + 无报错 |
| e2e | `dragonbones_switch` | `MAQUE_SIM` 注入 ↑/↓/空格 → 5 次切换顺序 + 引擎读回一致 + 两张截图不同 + 像素断言（框选排除 HUD） |
| e2e | `dragonbones_determinism` | 同场景两次 → 截图逐字节一致 + `world_trail` 一致 |
| 人工 | 官方参考图对照 | "CORE ELEMENT" logo 正读（证明 y 未翻转）+ 机甲站立姿势与官方 demo 一致 |

## 5. 落地阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| M0 | schema（组件 + `module_abi`）+ codegen + 模块骨架（解析 JSON/图集/回调） | ✅ |
| M1 | 求值（骨骼矩阵/加法式动画/循环/时间轴补间）+ 绘制导出（image/mesh/FFD） | ✅ |
| M2 | runtime 接线（setup/step/extract/release）+ 组件 + 脚本 API + 页纹理上传 | ✅ |
| M3 | 示例工程 + 单测 + 3 个 e2e + 文档 | ✅ |
| P1 | 带权重网格、嵌套子骨架、颜色 offset、多页图集、动画混合 | ⬜ |
| P2 | `.dbbin` 二进制、IK、zOrder 时间轴、action 事件 | ⬜ |

## 6. 风险与取舍

| 风险 | 取舍 |
|---|---|
| 官方 C++ 运行时停更且绑 cocos2d-x | 自研（按官方 JSON 规范 + DragonBonesJS/C++ 源码逐段对照），用官方 C++ 核心做数值对拍 |
| 语义近似（`inheritScale=false`）悄悄画错 | 真实素材 + 官方参考图对照；合成素材单测锁定（见 docs/36 §5.1） |
| 图集 rotated 无真实素材验证 | 按 `PixiSlot` 公式实现，文档标注"未逐像素验证" |
| 颜色 offset 需要 shader 加法项 | P0 只乘乘子，文档列明未支持 |
| 示例素材许可（CC BY-NC-SA） | 单独 LICENSE 文件写明，仅示例/测试；商用请替换 |
