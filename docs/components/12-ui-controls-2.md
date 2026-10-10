# 12 · 内置组件设计 — UI 控件（补充）

> 上游：`17`（组件约定 §2/§3）、`04`（UI 基础：Control · Container · TextLabel · Image · Button）、
> `08`（UI 控件：CheckBox · RadioGroup · Tabs · ScrollView · ListView · Mask）、
> `12 §5`（UI 为独立渲染层）、`13 §4`（UI 独立 Render Graph 分支）、`13 §8`（渲染只读消费）、
> `06`（反射 / attrs / `entity_ref` / `asset_ref`）、`10`（确定性 / 输入记录）、`19`（输入与事件分发：指针捕获 / 滚轮 / 拖拽）。
> 本文是 `04`/`08` 的**第二批扩展控件**，风格与其完全一致：设计 5 个组件 ——
> **Slider · ProgressBar · PageView · SafeArea · UIOpacity**。

## 0. 控件总纲（5 个组件共享）

1. **全部基于 `Control`**：每个控件组件都必须与 `Control` 同挂于**同一节点**（`Control` 提供外框，
   `components/04 §0`），由 `Control.mouse_filter` 决定是否截获指针。`PageView / SafeArea / UIOpacity`
   的作用**对象**往往是子 `Control` 节点，自身同样是 `Control` 节点。
2. **交互 = 信号**：值变化、切页、拖动起止等一律以**信号**暴露，脚本按名 `connect`
   （`04 §0.5`、`06 §6`、`14 §2`）；**禁止手写绑定**，信号描述符经反射生成；兼容 `_on_<signal>` 生命周期式回调（`14 §4`）。
3. **引用一律用句柄**：`entity_ref`（内容 / 页面 / 遮罩源）与 `asset_ref<T>`（字体 / 纹理 / 样式），
   序列化为**稳定 ID / 路径**（`06 §3`、`12 §7`）；**禁止裸指针、`std::` 容器、隐式所有权**（`17 §2.2`）。
4. **事件是数据**：命中测试与拖动消费**输入记录**（`10 §3`、`19 §3`），UI 在**模拟线程**做命中 → 回放一致；
   派生量一律 `transient`。
5. **顺序确定**：子/页面遍历顺序 = 节点树**声明序 + 深度优先**（`12 §6`）；同类解算为纯函数，无墙钟依赖。
6. **渲染是只读消费方**：控件只产出中性的绘制描述，**不回写**模拟状态（`13 §8`）。

---

## Slider

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（Control 同族）· `19 §6/§8`（指针捕获 / Drag 订阅）
- **说明**：**连续/离散取值**的拖动控件（音量、进度、参数调节）。**必须与 `Control` 同挂**；
  `Slider` 自持轨道与滑块的样式，把归一化的指针位置映射为 `[min, max]` 区间内的值，并按 `step` 取整。
  拖动期间经 `19 §6` 的**指针捕获**保持对同一 `pointer_id` 的 Move/Up 订阅，避免移出边界丢目标。
- **依赖类型 / enum**：
  - `SliderOrientation`（轴向）：`Horizontal / Vertical`
  - `StepMode`（离散步进）：`None`（连续）/ `Steps`（按 `step` 吸附）
  - `asset_ref<Texture>`（轨道 / 滑块样式）、`asset_ref<Font>`（数值标签）

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `min_value` | `float` | `0` | — | 取值下限（`min_value < max_value`，校验保证） |
| `max_value` | `float` | `1` | — | 取值上限 |
| `value` | `float` | `0` | `range` | 当前值，clamp 到 `[min_value, max_value]` |
| `step` | `float` | `0` | `range:[0,inf)` | 步进；`StepMode.None` 或 `0` 时连续 |
| `step_mode` | `StepMode` | `None` | — | 连续或按 `step` 吸附 |
| `orientation` | `SliderOrientation` | `Horizontal` | — | 轨道方向（决定坐标系映射轴） |
| `invert` | `bool` | `false` | — | 反向：低值在右/上 |
| `exp_edit` | `bool` | `false` | — | 指数映射（对数滑条），便于宽范围参数 |
| `allow_greater` | `bool` | `false` | — | 允许拖动越界（拖出区间外不改值，仅到端点） |
| `disabled` | `bool` | `false` | — | 禁用：不响应且置禁用外观 |
| `track_texture` | `asset_ref<Texture>` | `null` | `ref:Texture` | 轨道底图；空用主题默认 |
| `grabber_texture` | `asset_ref<Texture>` | `null` | `ref:Texture` | 滑块贴图；空用主题默认 |
| `grabber_radius` | `float` | `8` | `unit:px` | 滑块命中半径（决定拖动热区） |
| `show_value_label` | `bool` | `false` | — | 是否显示当前值文本 |
| `font` | `asset_ref<Font>` | `null` | `ref:Font` | 数值标签字体；空用主题默认 |
| `dragging` | `bool` | `false` | `transient` | 是否处于拖动中（运行时派生） |
| `ratio` | `float` | `0` | `range:[0,1], transient` | 归一化位置（由 `value` 派生） |

- **句柄/引用**：`track_texture`、`grabber_texture`、`font` 均为 `asset_ref`（句柄，序列化为稳定资产 ID / 路径，`06 §3`）；无 `entity_ref`。
- **事件暴露**：`value_changed(value:float)`、`drag_started()`、`drag_ended()`。按下时调用 `set_pointer_capture(pointer_id)`
  （`19 §6`），Up/Cancel 自动释放。描述符由反射生成，脚本按名连接（`14 §2`）。
- **生命周期 / 确定性**：命中与拖动消费**输入记录**（`10 §3`），在**模拟线程、固定点**（输入处理阶段）更新 `value`：
  `ratio = clamp((pointer_pos − track_begin) / track_len, 0, 1)`；`value` 为 `ratio` 的**纯函数**
  （`StepMode.Steps` 时 `round((value−min)/step)*step+min` 再 clamp），`exp_edit` 用固定底数指数映射，
  **不使用墙钟**。信号派发顺序确定，参与回放。`dragging` / `ratio` 为 `transient` 派生量。
- **序列化**：`min_value`/`max_value`/`value`/`step`/`step_mode`/`orientation`/`invert`/`exp_edit`/`disabled`
  与样式字段全部落盘；`dragging`、`ratio` 等运行时量 `transient` 不落盘。

---

## ProgressBar

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（Image 同族自绘）· `13 §5`（纹理经 VFS 加载）
- **说明**：**只读进度**展示控件（加载条、血条、经验条）。**必须与 `Control` 同挂**；
  `ProgressBar` 按 `value`（0..1）与 `fill_mode` 绘制填充比例，不响应用户输入（
  `Control.mouse_filter` 建议设 `Ignore`，不参与命中，`19 §4`）。填充内容可由纹理、纯色或二者叠加。
- **依赖类型 / enum**：
  - `FillMode`（填充度量）：`LeftToRight / RightToLeft / TopToBottom / BottomToTop / Radial`
  - `ProgressMode`（显示模式）：`Determinate`（确定值）/ `Indeterminate`（不定长动画）
  - `asset_ref<Texture>`（背景 / 填充样式）、`asset_ref<Font>`（百分比文本）

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `value` | `float` | `0` | `range:[0,1]` | 进度值（0=空，1=满）；写入时 clamp |
| `min_display` | `float` | `0` | `range:[0,1]` | 低于此值时不显示填充（避免视觉残留） |
| `mode` | `ProgressMode` | `Determinate` | — | 确定 / 不定长 |
| `fill_mode` | `FillMode` | `LeftToRight` | — | 填充方向/形状 |
| `radial_center` | `Vec2` | `{0.5,0.5}` | `range:[0,1]` | `Radial` 圆心（归一化于自身矩形） |
| `radial_begin` | `float` | `0` | `unit:rad` | `Radial` 起始角 |
| `radial_fill` | `float` | `-2π` | `unit:rad` | `Radial` 满值扫过角（负=顺/逆时钟由符号定） |
| `indeterminate_speed` | `float` | `1` | `unit:1/s, range:[0,10]` | 不定长动画速度（模拟线程按固定 dt 推进） |
| `overlay` | `bool` | `false` | — | 填充叠在背景之上（否则填充先于背景） |
| `back_texture` | `asset_ref<Texture>` | `null` | `ref:Texture` | 背景底图；空用 `back_color` |
| `fill_texture` | `asset_ref<Texture>` | `null` | `ref:Texture` | 填充贴图；空用 `fill_color` |
| `back_color` | `Vec4` | `{0.2,0.2,0.2,1}` | — | 背景色（无底图时） |
| `fill_color` | `Vec4` | `{0.4,0.8,0.4,1}` | — | 填充色（含 alpha） |
| `show_percentage` | `bool` | `false` | — | 显示百分比文本 |
| `font` | `asset_ref<Font>` | `null` | `ref:Font` | 百分比字体；空用主题默认 |
| `phase` | `float` | `0` | `range:[0,1], transient` | 不定长相位（运行时派生，驱动绘制） |

- **句柄/引用**：`back_texture`、`fill_texture`、`font` 均为 `asset_ref`（句柄）；**无 `entity_ref`**，无信号（纯展示）。
- **生命周期 / 确定性**：`value` 由脚本/逻辑写入（帧边界生效）；填充比例是 `value × fill_mode` 的**纯函数**，
  `transient` 不入模拟校验和。`Indeterminate` 的 `phase` 由**固定 dt** 在模拟线程积分（确定，可回放），
  位相仅影响绘制、不回写逻辑；纹理经 VFS 异步加载，未就绪时用纯色占位（`04` Image 同规）。
- **序列化**：`value`（恢复显示）与样式字段落盘；`phase` 等运行时量 `transient` 不落盘。

---

## PageView

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `08`（ScrollView 同族，翻页变体）· `19 §5/§6`（滚轮冒泡 / 指针捕获拖拽）
- **说明**：**整页吸附**的分页容器（轮播、引导页、相册）。挂在 PageView 根（**必须与 `Control` 同挂**）；
  **页面 = 其直接子 `Control` 节点**（按声明序），每次只展示一页，拖动/滑动后**吸附**到最近页。
  与 `ScrollView` 共享滚动/裁剪语义，但以**页为单位**量化。可在 `loop` 时首尾接环。
- **依赖类型 / enum**：
  - `ScrollDirection`（翻页轴向）：`Horizontal / Vertical`
  - `PageTransition`（翻页过渡）：`None / Slide / Fade / Cursor`
  - `entity_ref`

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `current_page` | `int32` | `0` | `range:[0,65535]` | 当前页索引（子声明序） |
| `direction` | `ScrollDirection` | `Horizontal` | — | 翻页轴向 |
| `loop` | `bool` | `false` | — | 循环：末页之后回到首页，反之亦然 |
| `transition` | `PageTransition` | `Slide` | — | 翻页过渡方式 |
| `transition_time` | `float` | `0.3` | `unit:s, range:[0,10]` | 过渡时长（固定 dt 推进） |
| `drag_threshold` | `float` | `0.25` | `range:[0,1]` | 拖动超过页宽/高的比例即翻页 |
| `fling_threshold` | `float` | `400` | `unit:px/s` | 甩动翻页的速度阈值 |
| `deadzone` | `int32` | `0` | `unit:px` | 触摸起拖死区（`19 §5`） |
| `snap` | `bool` | `true` | — | 松手后吸附到最近页 |
| `clip_content` | `bool` | `true` | — | 裁剪溢出页（等价内建 `Mask.Rect`） |
| `page_count` | `int32` | `0` | `transient, readonly` | 页面数（派生） |
| `page_ratio` | `float` | `0` | `range:[0,1], transient` | 连续翻页进度（拖动中派生，供过渡绘制） |
| `current_node` | `entity_ref` | `null` | `ref, transient` | 当前页节点（由 `current_page` 解析，派生） |

- **句柄/引用**：页面经**节点树子列表**遍历（`entity_ref` 声明序，禁裸指针）；`current_node` 为 `entity_ref`，
  由 `current_page` 运行期解析，标 `transient`（不序列化）。
- **事件暴露**：`page_changed(index:int32)`、`page_changing(index:int32, ratio:float)`（拖动中连续）、
  `drag_started()`、`drag_ended()`。拖动经 `set_pointer_capture`（`19 §6`）；滚轮事件先给目标、未消费则冒泡到 PageView（`19 §5`）。
  派发在模拟线程、固定点。
- **生命周期 / 确定性**：页顺序 = 子**声明序**（`12 §6`）；翻页判定是纯函数（拖动位移/速度 × `drag_threshold`/`fling_threshold`）；
  过渡按**固定 dt** 在模拟线程推进（确定，参与回放）；`loop` 的环形索引用取模，边界固定。
  `page_ratio`/`page_count`/`current_node` 为 `transient` 派生量，不进模拟校验和。
- **序列化**：`current_page`（恢复位置）与配置字段落盘；派生量与过渡中间态 `transient` 不落盘。

---

## SafeArea

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（Control 布局约束）· `12 §5`（UI 独立坐标系）
- **说明**：把子树的可用外框**内缩到设备安全区**（刘海、圆角、手势条、状态栏）。挂在 SafeArea 根
  （**必须与 `Control` 同挂**）；在 layout pass 内将自身 `Control.rect` 按安全区边距收缩，并影响直接子 `Control`
  的外框解算（不改子节点自身字段，只改其 `transient` 布局槽）。用于全屏 UI 适配。
- **依赖类型 / enum**：
  - `SafeAreaMode`（边距来源）：`Device / Custom`
  - `SafeAreaAnchor`（内缩基准）：`Self`（仅内缩自身）/ `Children`（内缩并约束子布局）
  - `entity_ref`（可选：仅约束该子节点）

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `mode` | `SafeAreaMode` | `Device` | — | `Device`=用实例安全区；`Custom`=用手填边距 |
| `anchor` | `SafeAreaAnchor` | `Children` | — | `Self`=仅收缩自身外框；`Children`=同时约束子布局 |
| `target` | `entity_ref` | `null` | `ref` | 仅约束指定子节点；空=约束全部直接子 `Control` |
| `inset_left` | `float` | `0` | `unit:px` | 自定义左内缩（`Custom` 时生效） |
| `inset_top` | `float` | `0` | `unit:px` | 自定义上内缩 |
| `inset_right` | `float` | `0` | `unit:px` | 自定义右内缩 |
| `inset_bottom` | `float` | `0` | `unit:px` | 自定义下内缩 |
| `min_size` | `Vec2` | `{0,0}` | `unit:px` | 内缩后的尺寸下限（避免过度收缩） |
| `debug_draw` | `bool` | `false` | — | 调试：描边显示安全区（仅渲染，不入模拟） |
| `resolved_inset` | `Vec4` | `{0,0,0,0}` | `unit:px, transient` | 实际内缩 `{左,上,右,下}`（运行时派生） |

- **句柄/引用**：`target` 为 `entity_ref`（序列化为稳定 ID / 路径，禁裸指针）；无资产引用。
- **事件暴露**：`safe_area_changed(inset:Vec4)`（安全区可用、或视口/方向改变时触发）。派发在模拟线程、固定点。
- **生命周期 / 确定性**：**安全区是实例视口配置的一部分**（与视口尺寸同级，`19 §10` instance 所有），
  由平台在**帧边界**一次性同步为数据，非逐帧墙钟读取；`resolved_inset` 是 `mode`/视口配置的**纯函数**，
  在 layout pass 内解算并标 `transient`。`Device` 模式下回放时沿用**录制时的安全区数据**，保证同一输入记录 →
  同一布局。`debug_draw` 仅影响渲染，不参与模拟校验和（`13 §8`）。
- **序列化**：`mode`/`anchor`/`target`/`inset_*`/`min_size` 落盘；`resolved_inset` 等派生量 `transient` 不落盘。

---

## UIOpacity

- **归属模块 / 上游文档**：`ui` 模块 · `17 §2` / `04`（Image/TextLabel 染色约定）· `13 §4`（UI 分支 alpha 合成）
- **说明**：**组透明度**组件，对自身及其**整个 UI 子树**施加统一透明度（淡入淡出、整屏变暗、禁用遮罩）。
  挂在任意 `Control` 节点（**必须与 `Control` 同挂**）；合成时透明度沿子树**累乘**（父 × 子），
  最终乘到各叶子的绘制项颜色 alpha 上。它**只影响绘制**，不改变子节点的逻辑状态与命中测试。
- **依赖类型 / enum**：
  - `OpacityMode`（值语义）：`Normal`（0..1 线性）/ `Gamma`（按 gamma 曲线）
  - `entity_ref`（可选：仅作用于指定子树根）

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `opacity` | `float` | `1` | `range:[0,1]` | 自身透明度（1=不透明，0=全透明）；写入时 clamp |
| `mode` | `OpacityMode` | `Normal` | — | 值语义（线性 / gamma 曲线） |
| `affect_children` | `bool` | `true` | — | 为真则累乘到子树；为假仅自身 |
| `root` | `entity_ref` | `null` | `ref` | 作用子树根；空=自身 |
| `visible_threshold` | `float` | `0` | `range:[0,1]` | 合成后 alpha 低于此值的叶子**跳过绘制**（性能优化） |
| `final_alpha` | `float` | `1` | `range:[0,1], transient` | 累乘后的有效 alpha（运行时派生） |

- **句柄/引用**：`root` 为 `entity_ref`（序列化为稳定 ID / 路径，禁裸指针）；无资产引用。
- **事件暴露**：`opacity_changed(opacity:float)`。派发在模拟线程、固定点，参与回放。
- **生命周期 / 确定性**：`final_alpha` 是**父链累乘的纯函数**（自身 `opacity` × 祖先各 `UIOpacity.opacity`），
  在**提取阶段**（`13 §4` draw list 生成前的 UI 遍历）按深度优先 + 声明序解算，顺序确定；
  仅影响绘制项 alpha，**不写入子节点字段、不参与模拟校验和**（`13 §8`）。`Gamma` 模式用固定曲线表，无平台相关 libm 差异。
- **序列化**：`opacity`/`mode`/`affect_children`/`root`/`visible_threshold` 落盘；`final_alpha` `transient` 不落盘。

---

## 附：UI 控件（补充）信号速览

| 组件 | 信号 | 参数 | 说明 |
|------|------|------|------|
| Slider | `value_changed` | `float` | 值变化 |
| Slider | `drag_started` / `drag_ended` | — | 拖动起止 |
| ProgressBar | — | — | 纯展示，无信号 |
| PageView | `page_changed` | `int32` | 整页切换完成 |
| PageView | `page_changing` | `int32, float` | 拖动中连续翻页进度 |
| PageView | `drag_started` / `drag_ended` | — | 拖动起止 |
| SafeArea | `safe_area_changed` | `Vec4` | 安全区内缩变化 |
| UIOpacity | `opacity_changed` | `float` | 透明度变化 |

> 信号名即稳定标识，描述符由反射导出（`06 §6`）；所有 UI 交互在模拟线程、消费输入记录、按确定顺序派发
> → 回放一致（`10 §2 D6`）；交互统一走 `19` 的三阶段分发（捕获→目标→冒泡），拖动经指针捕获（`19 §6`），
> 滚轮未消费则冒泡到最近的 `PageView`/`ScrollView`（`19 §5`）。渲染与合成层为独立只读消费方（`13 §8`）。

## 状态

| 组件 | 设计 | 评审 | 落地 |
|------|------|------|------|
| Slider | ✅ | ⬜ | ⬜ |
| ProgressBar | ✅ | ⬜ | ⬜ |
| PageView | ✅ | ⬜ | ⬜ |
| SafeArea | ✅ | ⬜ | ⬜ |
| UIOpacity | ✅ | ⬜ | ⬜ |
