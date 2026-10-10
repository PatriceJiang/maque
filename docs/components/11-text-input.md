# 组件详设 · 文本输入（EditBox）

> 上游：`17`（约定 §2/§3）、`components/04-ui.md`（Control）、`components/08-ui-controls.md`、`12`、`13`。
> 命名：**`EditBox`**（别名 LineEdit / TextField / InputField）。
> 单行与多行由 `line_mode` 区分（多行即常说的 TextArea）。

## 1. 组件：EditBox（文本输入框）

- **归属模块 / 文档**：UI 控件；基于 `Control`（提供盒模型/布局），依赖字体资产与文本状态。
- **说明**：可编辑文本字段。`line_mode=SingleLine` 为单行输入；`MultiLine` 为多行（含换行与滚动）。
  与 `TextLabel`（只读展示）配对；文本变更经**信号/回调**暴露给脚本（docs/14）。
- **依赖类型 / enum**：

```yaml
- enum: EditLineMode
  values: [SingleLine, MultiLine]          # default SingleLine
- enum: TextOverflow                      # 单行溢出
  values: [Clip, Ellipsis, Scroll]         # default Clip
- enum: WrapMode                          # 多行换行
  values: [None, Word, Char]               # default Word
- enum: TextAlign
  values: [Left, Center, Right, Justify]   # default Left
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| text | string | "" | | 当前文本（**模拟状态**） |
| placeholder | string | "" | | 空时占位提示 |
| line_mode | EditLineMode | SingleLine | | 单行/多行 |
| max_length | int32_t | 0 | range:[0,inf) | 0=不限 |
| editable | bool | true | | 只读时禁止编辑（`readonly`） |
| secret | bool | false | | 密码遮罩（显示 ●） |
| secret_char | uint32_t | 0x2022 | | 遮罩字符 |
| select_all_on_focus | bool | false | | 获焦全选 |
| clear_button | bool | false | | 显示清除按钮 |
| text_color | Vec4 | {1,1,1,1} | | 文本色 |
| placeholder_color | Vec4 | {1,1,1,0.5} | | 占位色 |
| selection_color | Vec4 | {0.3,0.5,1,0.4} | | 选区色 |
| caret_color | Vec4 | {1,1,1,1} | | 光标色 |
| caret_width | float | 1.0F | unit:px | 光标宽度 |
| font | asset_ref&lt;Font&gt; | null | ref:Font | 字体资源 |
| font_size | float | 16.0F | unit:px | 字号 |
| align | TextAlign | Left | | 水平对齐 |
| overflow | TextOverflow | Clip | | 单行溢出策略 |
| wrap_mode | WrapMode | Word | | 多行换行策略（line_mode=MultiLine 生效） |
| line_spacing | float | 0.0F | unit:px | 多行行距 |
| caret_pos | int32_t | 0 | transient | 光标位置（运行时） |
| sel_start | int32_t | -1 | transient | 选区起点（-1=无选区） |
| sel_end | int32_t | -1 | transient | 选区终点 |
| scroll_offset | Vec2 | {0,0} | transient | 多行滚动偏移 |
| focused | bool | false | transient | 是否获焦 |
| ime_composing | bool | false | transient | IME 组字中（不提交到 text 直到确认） |

- **句柄/引用**：字体用 `asset_ref<Font>`（存句柄、序列化为稳定 ID/路径）。
- **信号/回调**（暴露给脚本，稳定 ID 或回调名）：
  - `on_text_changed(new_text)` —— 每次变更
  - `on_text_submitted(text)` —— 回车/失焦确认
  - `on_focus_entered` / `on_focus_exited`
- **生命周期 / 确定性**：
  - `text` 是**模拟状态**（可被玩法读取、参与回放）；输入事件在**模拟线程**处理。
  - `caret_pos/sel/scroll/focused/ime_composing` 为**运行时派生**（transient），**不参与快照 checksum**。
  - IME 组字期间**不提交**到 `text`（避免半成品污染回放）。
- **序列化**：`text` 与设置字段序列化；所有 `transient` 字段不入库。

## 2. 事件与输入管线

- 文本输入经 UI 输入系统 → 编辑逻辑（插入/删除/选区/剪贴板）→ 更新 `text` + 发信号。
- **剪贴板**：单行自动去换行；多行保留。
- **输入法（IME）**：组字中间态放 transient，确认后才写入 `text`。
- 与 `ScrollView`（`components/08`）组合可实现长文本编辑区。

## 3. 边界与校验

- `wrap_mode`/`line_spacing`/滚动仅在 `line_mode=MultiLine` 生效，否则忽略。
- `secret=true` 时导出文本经遮罩，禁止脚本直接读取明文（安全，对齐 docs/14 能力门控）。
- `max_length` 超限时截断（按**字符**计数，非字节）。

## 4. 拆分说明

若未来单行与多行字段分化过大，可拆为 `EditBox`（单行）+ `TextArea`（多行）；
当前以 `line_mode` 单组件覆盖，减少类型数。
