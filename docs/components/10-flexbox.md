# 组件详设 · UI 布局（Flexbox）

> 上游：`17`（约定 §2/§3）、`components/04-ui.md`（Control/Container）、`12`、`13`。
> 目的：提供**CSS Flexbox 语义**的一等布局模型。它是 `Container(H/V/Grid)` 的**超集/替代**，
> 用于复杂、可换行、可分配剩余空间的 UI 布局。

## 1. 与 `Container` 的关系

- `Container`（`components/04`）提供 H/V/Grid 的**简单**布局。
- **`FlexBox`** 提供完整 flex：方向、换行、主轴对齐、交叉轴对齐、增长/收缩/基准、顺序、gap。
- 约定：一个布局容器**只挂其一**（`Container` **或** `FlexBox`），避免语义冲突（校验保证）。
- 布局遵循"**组合优于继承**"：容器挂 `FlexBox`，子节点挂 `FlexItem`。

---

## 2. 组件：FlexBox（弹性容器）

- **归属模块 / 文档**：UI 布局；依赖 `components/04`（Control 提供盒模型）与 `math`。
- **说明**：挂在**父节点**上，决定其直接子节点在主轴/交叉轴上的排布。需要 `Control` 一起使用（Control 提供自身尺寸/内边距）。
- **依赖类型 / enum**：

```yaml
- enum: FlexDirection      # 主轴方向
  values: [Row, RowReverse, Column, ColumnReverse]     # default Row
- enum: FlexWrap
  values: [NoWrap, Wrap, WrapReverse]                  # default NoWrap
- enum: JustifyContent      # 主轴对齐
  values: [Start, End, Center, SpaceBetween, SpaceAround, SpaceEvenly]  # default Start
- enum: AlignItems          # 交叉轴对齐（单行）
  values: [Stretch, Start, End, Center, Baseline]      # default Stretch
- enum: AlignContent        # 多行（Wrap）时行间对齐
  values: [Stretch, Start, End, Center, SpaceBetween, SpaceAround]  # default Stretch
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| direction | FlexDirection | Row | | 主轴方向 |
| wrap | FlexWrap | NoWrap | | 是否换行 |
| justify_content | JustifyContent | Start | | 主轴对齐 |
| align_items | AlignItems | Stretch | | 交叉轴对齐（单行） |
| align_content | AlignContent | Stretch | | 多行对齐（wrap 时生效） |
| gap_main | float | 0.0F | unit:px | 主轴方向间距 |
| gap_cross | float | 0.0F | unit:px | 交叉轴方向间距 |
| padding | Vec4 | {0,0,0,0} | | 内边距 L/T/R/B |
| reverse_safe | bool | false | | RTL/镜像时的排序兼容开关 |
| dirty | bool | true | transient | 布局失效标记（运行时） |

- **句柄/引用**：无（纯布局数据）。
- **生命周期 / 确定性**：布局在**布局 pass** 内按**子节点声明序**求解，顺序确定（docs/10）。布局结果写回 `Control` 的 transient 计算槽，**不写模拟状态**。
- **序列化**：全部字段序列化（`dirty` transient）。文本入库、可 diff（docs/08）。

---

## 3. 组件：FlexItem（弹性子项）

- **归属模块 / 文档**：UI 布局；挂在**子节点**上，描述其在父 `FlexBox` 中的弹性行为。
- **说明**：语义等价于 CSS 的 `flex` / `align-self` / `order`。**仅在父节点挂了 `FlexBox` 时有意义**（否则忽略/告警）。
- **依赖类型 / enum**：

```yaml
- enum: FlexBasisMode
  values: [Auto, Points, Percent]     # default Auto
- enum: AlignSelf
  values: [Auto, Start, End, Center, Stretch, Baseline]  # default Auto（继承父 align_items）
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| grow | float | 0.0F | range:[0,inf) | 剩余空间放大比例 |
| shrink | float | 1.0F | range:[0,inf) | 空间不足时收缩比例 |
| basis_mode | FlexBasisMode | Auto | | 基准尺寸模式 |
| basis_value | float | 0.0F | unit:px | Points/Percent 时的基准值 |
| align_self | AlignSelf | Auto | | 覆盖父容器 align_items |
| order | int32_t | 0 | | 排列顺序（小在前，稳定序） |
| min_main | float | 0.0F | unit:px | 主轴最小尺寸（收缩下限） |
| max_main | float | -1.0F | unit:px | 主轴最大尺寸（-1=不限） |

- **句柄/引用**：无。
- **生命周期 / 确定性**：`order` 排序用**稳定排序**（相同 order 保持声明序），保证布局确定。
- **序列化**：全部序列化（布局派生量不落此组件）。

---

## 4. 布局求解（确定性要求）

1. 收集父 `FlexBox` 的直接子 `Control` + 可选 `FlexItem`。
2. 按 `order` **稳定排序**（同序保持声明序）→ 确定。
3. 计算主轴/交叉轴尺寸：`basis → grow → shrink → min/max` clamp。
4. 应用 `justify_content` / `align_items` / `align_content` / gap。
5. 结果写入子 `Control` 的 **transient 布局槽**（不序列化、不回写模拟）。

- **约束**：布局是**纯函数**（输入同 → 输出同）；不使用墙钟/系统熵；浮点遵循确定 FP 规则（docs/10 §6）。
- **与渲染**：布局结果经提取成为 DrawList 的 UI 层（docs/13），渲染**只读**。

## 5. 边界与校验

- 同节点**同时**挂 `Container` 与 `FlexBox` → 校验报错（互斥）。
- 挂 `FlexItem` 但父无 `FlexBox` → 告警并忽略。
- 循环/自引用布局 → 由节点树层级天然避免（无环）。

## 6. 序列化与 diff

- 字段文本入库、稳定键序、省略默认值（docs/08 §4）。
- `dirty`、计算槽等运行时派生量**一律 transient**，不入库、不参与快照 checksum。
