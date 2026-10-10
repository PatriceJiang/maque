# 08 · 内置组件设计 — UI 控件

> 上游：`17`（组件约定 §2/§3）、`04`（UI 基础：Control · Container · TextLabel · Image · Button）、
> `12 §5`（UI 为独立渲染层）、`13 §4`（UI 独立 Render Graph 分支，裁剪/stencil pass）、`13 §8`（渲染只读消费）、
> `06`（反射 / attrs / `entity_ref` / `asset_ref`）、`10`（确定性 / 输入记录）。
> 本文是 `04` 的**扩展控件**，风格与其完全一致：设计 6 个组件 ——
> **CheckBox · RadioGroup · Tabs · ScrollView · ListView · Mask**。

## 0. 控件总纲（6 个组件共享）

1. **全部基于 `Control`**：每个控件组件都必须与 `Control` 同挂于**同一节点**（`Control` 提供外框），
   由 `Control.mouse_filter` 决定是否截获指针（`04 §0`）。`RadioGroup / Tabs / ScrollView / ListView / Mask`
   的操作**对象**是子 `Control` 节点，自身同样是一个 `Control` 节点。
2. **交互 = 信号**：点击、切换、滚动、选中、切页、索要数据等一律以**信号**暴露，脚本按名 `connect`
   （`04 §0.5`、`06 §6`、`14 §2`）；**禁止手写绑定**，信号描述符经反射生成。
3. **引用一律用句柄**：`entity_ref`（成员 / 内容 / 页 / 数据源 / 项实例）与 `asset_ref<T>`（字体 / 纹理 / 场景模板），
   序列化为**稳定 ID / 路径**（`06 §3`、`12 §7`）；**禁止裸指针、`std::` 容器、隐式所有权**（`17 §2.2`）。
4. **事件是数据**：命中测试消费**输入记录**（`10 §3`），UI 在**模拟线程**做命中 → 回放一致；派生量一律 `transient`。
5. **顺序确定**：子/成员的遍历顺序 = 节点树**声明序 + 深度优先**（`12 §6`）；同类解算为纯函数，无墙钟依赖。
6. **渲染是只读消费方**：控件只产出中性的绘制/裁剪描述，**不回写**模拟状态（`13 §8`）。

---

## CheckBox

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（Button 同族）· `09`（信号反射）
- **说明**：带勾选框的布尔输入控件，是 `Button` 的**选中态变体**。**必须与 `Control` 同挂**；
  `CheckBox` 自持标签文字与勾选框三态外观，处理指针命中并发出**信号**。可两态或三态（`tristate`）。
- **依赖类型 / enum**：
  - `CheckState`（显示态）：`Unchecked / Checked / Indeterminate`
  - `Align`（复用 `04 Container`）：`Begin / Center / End`
  - `asset_ref<Font>`、`asset_ref<Texture>`

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `checked` | `bool` | `false` | — | 两态逻辑值；`tristate=false` 时为准 |
| `tristate` | `bool` | `false` | — | 启用三态（`Unchecked/Checked/Indeterminate`） |
| `indeterminate` | `bool` | `false` | — | 三态中间态标记（仅 `tristate=true` 有效） |
| `disabled` | `bool` | `false` | — | 禁用：不响应且置禁用外观 |
| `text` | `string` | `""` | — | 勾选标签文字（UTF-8） |
| `font` | `asset_ref<Font>` | `null` | `ref:Font` | 标签字体；空用 UI 主题默认字体 |
| `font_size` | `float` | `16` | `unit:px, range:[1,512]` | 字号 |
| `text_color` | `Vec4` | `{1,1,1,1}` | — | 标签颜色（含 alpha） |
| `icon_checked` | `asset_ref<Texture>` | `null` | `ref:Texture` | 选中框贴图；空用主题默认 |
| `icon_unchecked` | `asset_ref<Texture>` | `null` | `ref:Texture` | 未选中框贴图；空用主题默认 |
| `icon_indeterminate` | `asset_ref<Texture>` | `null` | `ref:Texture` | 中间态框贴图；空用主题默认 |
| `box_position` | `Align` | `Begin` | — | 勾选框相对文字位置 |
| `state` | `CheckState` | `Unchecked` | `transient` | 显示态（由 `checked/tristate/indeterminate` 派生） |

- **句柄/引用**：`font` 及各 `icon_*` 均为 `asset_ref`（句柄，序列化为稳定资产 ID / 路径，`06 §3`）；无 `entity_ref`。
- **事件暴露**：`toggled(checked:bool)`、`state_changed(state:CheckState)`、`hover_changed(hover:bool)`。
  描述符由反射生成，脚本按名连接（`14 §2`）；兼容 `_on_<signal>` 生命周期式回调（`14 §4`）。
- **生命周期 / 确定性**：指针命中测试消费**输入记录**（`10 §3`），按 UI 层命中优先级（`z_index` → 声明序 →
  深度优先）定位唯一目标；点击翻转是纯函数（当前态 × 输入 × `disabled`）。信号派发在**模拟线程、固定点**（输入处理阶段），
  顺序确定，参与回放。`state` 为 `transient` 派生量。
- **序列化**：逻辑与样式字段全部落盘；`state`、悬停等运行时量 `transient` 不落盘。

---

## RadioGroup

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（Container 同族遍历语义）
- **说明**：**单选组**逻辑组件，管理一组互斥选项。**必须与 `Control` 同挂**；组的**成员 = 其直接子 `Control` 节点**
  （按声明序），通常每个成员再挂 `Button`/`CheckBox` 作为外观。`RadioGroup` 只维护互斥与选中索引，
  **不重复排版**（排版交给子节点的 `Container`/`Control`）。语义对应 Godot `ButtonGroup`。
- **依赖类型 / enum**：
  - `Orientation`（导航主轴，复用 `ScrollView`）：`Vertical / Horizontal`
  - `entity_ref`

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `selected_index` | `int32` | `-1` | `range:[-1,65535]` | 选中成员索引（子声明序）；`-1`=无选中 |
| `allow_unselect` | `bool` | `false` | — | 允许再次点击取消选中（否则强制保留一个） |
| `loop` | `bool` | `true` | — | 键盘方向键导航是否首尾循环 |
| `orientation` | `Orientation` | `Vertical` | — | 方向键导航主轴 |
| `disabled` | `bool` | `false` | — | 整组禁用 |
| `selected` | `entity_ref` | `null` | `ref, transient` | 选中成员节点（由 `selected_index` 解析，派生） |
| `member_count` | `int32` | `0` | `transient, readonly` | 成员数（派生） |

- **句柄/引用**：成员经**节点树子列表**遍历（`entity_ref` 声明序，禁裸指针）；`selected` 为 `entity_ref`，
  由 `selected_index` 在运行期解析，标 `transient`（不序列化）。成员子节点可在其它组件里被引用，仍走 `entity_ref`。
- **事件暴露**：`changed(index:int32)`、`selected_changed(node:entity_ref)`。派发在模拟线程、固定点。
- **生命周期 / 确定性**：成员顺序 = 子**声明序**（`12 §6`）；互斥翻转是纯函数（旧索引 × 点击目标 × `allow_unselect`）；
  键盘导航按 `orientation`/`loop` 确定推进。信号参与回放（脚本据以改模拟状态）。
- **序列化**：`selected_index` 及配置字段落盘；`selected`、`member_count` 为 `transient` 派生量，不落盘。

---

## Tabs

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（复合控件）
- **说明**：**复合**分页控件（类似 Godot `TabContainer`）。挂在 Tabs 根节点（**必须与 `Control` 同挂**）。
  根下**一个子节点作 TabBar**（通常挂 `Container` + 若干 `Button`），**其余子节点为页面**（`Control` 子树）。
  `Tabs` 管理当前页、页面显隐与标签栏排序。TabBar 自身是普通节点，不在本组件内重复定义。
- **依赖类型 / enum**：
  - `TabTransition`（切页过渡）：`None / Fade / Slide`
  - `Side`（标签栏位置）：`Top / Bottom / Left / Right`
  - `entity_ref`

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `current_tab` | `int32` | `0` | `range:[0,65535]` | 当前页索引（页子节点声明序） |
| `tab_bar` | `entity_ref` | `null` | `ref` | 承载标签栏的子节点（挂 `Container`/`Button`）；排序由 Tabs 负责 |
| `side` | `Side` | `Top` | — | 标签栏相对页面的位置 |
| `transition` | `TabTransition` | `None` | — | 切页过渡方式 |
| `transition_time` | `float` | `0.15` | `unit:s, range:[0,10]` | 过渡时长 |
| `tab_count` | `int32` | `0` | `transient, readonly` | 页面数（派生） |
| `current_page` | `entity_ref` | `null` | `ref, transient` | 当前页节点（由 `current_tab` 解析，派生） |

- **句柄/引用**：`tab_bar`、`current_page` 为 `entity_ref`（序列化为稳定 ID / 路径）；页面 = 除 `tab_bar` 外的
  直接子 `Control` 节点（按声明序）；标签按钮由 Tabs 在 TabBar 内按页序生成/绑定。
- **事件暴露**：`tab_changed(index:int32)`；标签按钮 `pressed` 由 Tabs 转译为切页。派发在模拟线程、固定点。
- **生命周期 / 确定性**：页顺序 = 子**声明序**；切页 = 纯状态写 + 子节点 `visible` 切换；
  `transition != None` 时按固定 `dt` 在模拟线程推进（确定，参与回放）。`current_page`/`tab_count` 为 `transient`。
- **序列化**：`current_tab`、`tab_bar`(句柄)、`side`、`transition*` 落盘；`current_page`、`tab_count` 不落盘。

---

## ScrollView

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `13 §4`（裁剪 pass）· `04`（与 Mask 配对）
- **说明**：**滚动视口**控件。挂在 ScrollView 根（**必须与 `Control` 同挂**）。根下**一个子节点作 `content`**
  （内容通常大于视口）；ScrollView 提供水平/垂直滚动、滚动条，并裁剪溢出内容（裁剪可由 `Mask` 或自身 clip 承担，
  两者可配对）。用于长列表、日志、滚动面板。
- **依赖类型 / enum**：
  - `ScrollMode`（滚动条策略）：`Disabled / Auto / Always`
  - `Orientation`（滚动轴向）：`Horizontal / Vertical / Both`
  - `entity_ref`

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `content` | `entity_ref` | `null` | `ref` | 可滚动内容子节点（其 `Control.rect` 定义内容尺寸） |
| `scroll_h` | `float` | `0` | `range:[0,1]` | 水平滚动位置（归一化） |
| `scroll_v` | `float` | `0` | `range:[0,1]` | 垂直滚动位置（归一化） |
| `scroll_mode_h` | `ScrollMode` | `Auto` | — | 水平滚动条可见策略 |
| `scroll_mode_v` | `ScrollMode` | `Auto` | — | 垂直滚动条可见策略 |
| `scroll_step` | `Vec2` | `{16,16}` | `unit:px` | 滚轮/按键单步像素 `{x,y}` |
| `follow_focus` | `bool` | `false` | — | 子控件获焦时自动滚入可见区 |
| `deadzone` | `int32` | `0` | `unit:px` | 触摸拖拽判定死区 |
| `smooth` | `bool` | `false` | — | 惯性平滑滚动 |
| `bar_thickness` | `float` | `8` | `unit:px` | 滚动条厚度 |
| `scroll_offset` | `Vec2` | `{0,0}` | `unit:px, transient` | 解算后的像素偏移（运行时派生） |
| `content_size` | `Vec2` | `{0,0}` | `unit:px, transient` | 内容尺寸（运行时派生） |

- **句柄/引用**：`content` 为 `entity_ref`（序列化为稳定 ID / 路径）；滚动条为运行时派生绘制项，不持有独立句柄。
- **事件暴露**：`scrolled(offset:Vec2)`、`scroll_started()`、`scroll_ended()`。派发在模拟线程、固定点。
- **生命周期 / 确定性**：滚动位置由**输入记录**驱动（滚轮/拖拽/按键，`10 §3`），在模拟线程固定点更新；
  `smooth` 惯性用固定 `dt` 积分，确定；`scroll_offset`/`content_size` 为 `transient` 派生量，不进入模拟校验和。
- **序列化**：`scroll_h`/`scroll_v`（恢复位置）与配置字段落盘；`scroll_offset`、`content_size` 不落盘。

---

## ListView

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `12 §7`（实例化 + 引用重绑）· `13 §5`（模板经 VFS 加载）
- **说明**：**虚拟化**列表控件。挂在 ListView 根（**必须与 `Control` 同挂**）；只实例化**可见区间**的项，
  以**项模板**（子场景 / Prefab 子树）批量生成并**复用**。项数据由数据源按索引提供（经信号/脚本绑定）。
  通常内含 `ScrollView`（复用其滚动与裁剪）。
- **依赖类型 / enum**：
  - `Orientation`：`Vertical / Horizontal`
  - `SelectionMode`：`None / Single / Multi`
  - `asset_ref<Scene>`（项模板）、`entity_ref`（数据源 / 滚动 / 项实例）

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `item_template` | `asset_ref<Scene>` | `null` | `ref:Scene` | 列表项模板（场景/Prefab）；每项实例化一份 |
| `data_source` | `entity_ref` | `null` | `ref` | 数据源节点（挂 `Script`）；空则用 `item_count` 空项 |
| `item_count` | `int32` | `0` | `range:[0,1000000]` | 项总数（虚拟长度） |
| `item_extent` | `float` | `40` | `unit:px, range:[0,8192]` | 单项主尺寸（固定高/宽）；`0`=按模板自适应 |
| `orientation` | `Orientation` | `Vertical` | — | 排列方向 |
| `spacing` | `float` | `0` | `unit:px` | 项间距 |
| `overscan` | `int32` | `2` | `range:[0,64]` | 可见区外预生成项数（滚动缓冲） |
| `selection_mode` | `SelectionMode` | `Single` | — | 选择模式 |
| `selected_index` | `int32` | `-1` | `range:[-1,1000000]` | 选中项索引；`-1`=无 |
| `scroll` | `entity_ref` | `null` | `ref` | 关联 `ScrollView` 节点；空则 ListView 内建滚动 |
| `pool_size` | `int32` | `0` | `transient, readonly` | 当前复用的项实例数（派生） |

- **句柄/引用**：`item_template` 为 `asset_ref<Scene>`（句柄，序列化为稳定资产 ID / 路径）；`data_source`、`scroll`
  以及**生成的项实例**均以 `entity_ref` 持有。项实例归属 ListView 子树，实例化时**重新生成 id 并重绑内部引用**
  （`12 §7`）；外部引用走句柄。**禁裸指针**。
- **事件暴露**：`item_requested(index:int32)`（向数据源索数据）、`item_bound(index:int32, node:entity_ref)`、
  `selection_changed(index:int32)`、`item_activated(index:int32)`。派发在模拟线程、固定点。
- **生命周期 / 确定性（虚拟化关键）**：
  - 可见区间 = 纯函数 `f(scroll 偏移, item_extent, 视口尺寸, overscan)`；
  - 项的实例化/复用**按索引有序**，回收池策略固定（同一区间 → 同一批项绑定同一数据）→ 回放一致；
  - 项实例化在**帧边界、固定点**进行，**禁止帧内阻塞加载**；模板未就绪时占位（`13 §5`）；
  - `data_source` 回填经信号、在模拟线程；纯 `item_count` 模式完全确定。
- **序列化**：`item_template`(资产 ID)、`selected_index`、布局字段落盘；项实例节点与 `pool_size` 为运行期派生，
  **transient** 不落盘（重建时按当前区间重新实例化）。

---

## Mask

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `13 §4`（UI 分支的 clip/stencil pass）· `13 §8`（只读消费）
- **说明**：对子节点做**裁剪 / 遮罩**。挂在 Mask 根（**必须与 `Control` 同挂**）；按 `mask_mode` 把自身或子树绘制
  限制到矩形 / 圆角 / 纹理 / stencil 区域。对应 UI 独立 Render Graph 分支中的 clip/stencil pass（`13 §4`）。
- **依赖类型 / enum**：
  - `MaskMode`：`Rect / RoundedRect / Texture / Stencil`
  - `MaskChannel`（纹理取样通道）：`Alpha / Luminance / Red`
  - `CompareFunc`（stencil 比较）：`Always / Equal / NotEqual / Less / Greater`
  - `asset_ref<Texture>`

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `mask_mode` | `MaskMode` | `Rect` | — | 遮罩方式 |
| `clip_children` | `bool` | `true` | — | 为真裁剪子节点；为假仅限制自身绘制 |
| `corner_radius` | `Vec4` | `{0,0,0,0}` | `unit:px` | 四角圆角半径 `{左上,右上,右下,左下}`（`RoundedRect`） |
| `mask_texture` | `asset_ref<Texture>` | `null` | `ref:Texture` | 遮罩纹理（`Texture`：按通道裁剪） |
| `texture_channel` | `MaskChannel` | `Alpha` | — | 纹理取样通道 |
| `invert` | `bool` | `false` | — | 反转遮罩（区域内 / 外互换） |
| `stencil_ref` | `int32` | `0` | `range:[0,255]` | stencil 模板参考值（`Stencil`） |
| `stencil_write` | `bool` | `true` | — | 是否写入 stencil |
| `stencil_compare` | `CompareFunc` | `Equal` | — | stencil 比较函数 |
| `feather` | `float` | `0` | `unit:px, range:[0,64]` | 边缘羽化宽度；`0`=硬边 |

- **句柄/引用**：`mask_texture` 为 `asset_ref<Texture>`（句柄，稳定资产 ID / 路径）；作用对象 = 直接子 `Control`
  节点（按声明序，无字段）。
- **事件暴露**：无信号（纯渲染约束，不是输入控件）。
- **生命周期 / 确定性**：只产出**绘制裁剪约束**（clip rect / stencil 状态），属渲染**只读消费方**，
  **不参与模拟校验和**（`13 §8`）；裁剪区域解算为纯函数（自身 `rect` + 模式参数），不引入墙钟 / 系统熵。
- **序列化**：全部字段序列化；**无** `transient` 字段。

---

## 附：UI 控件信号速览

| 组件 | 信号 | 参数 | 说明 |
|------|------|------|------|
| CheckBox | `toggled` | `bool` | 勾选翻转 |
| CheckBox | `state_changed` | `CheckState` | 三态变化 |
| RadioGroup | `changed` | `int32` | 选中索引变化 |
| RadioGroup | `selected_changed` | `entity_ref` | 选中成员节点变化 |
| Tabs | `tab_changed` | `int32` | 切页 |
| ScrollView | `scrolled` | `Vec2` | 滚动偏移变化 |
| ScrollView | `scroll_started` / `scroll_ended` | — | 滚动起止 |
| ListView | `item_requested` | `int32` | 向数据源索要数据 |
| ListView | `item_bound` | `int32, entity_ref` | 项绑定完成 |
| ListView | `selection_changed` | `int32` | 选中项变化 |
| ListView | `item_activated` | `int32` | 项被激活（双击/回车） |

> 信号名即稳定标识，描述符由反射导出（`06 §6`）；所有 UI 事件在模拟线程、消费输入记录、按确定顺序派发
> → 回放一致（`10 §2 D6`）；渲染与裁剪层为独立只读消费方（`13 §8`）。

## 状态

| 组件 | 设计 | 评审 | 落地 |
|------|------|------|------|
| CheckBox | ✅ | ⬜ | ⬜ |
| RadioGroup | ✅ | ⬜ | ⬜ |
| Tabs | ✅ | ⬜ | ⬜ |
| ScrollView | ✅ | ⬜ | ⬜ |
| ListView | ✅ | ⬜ | ⬜ |
| Mask | ✅ | ⬜ | ⬜ |
