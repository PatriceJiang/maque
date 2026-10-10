# 资产详设 · 字体（Font）

> 上游：`08`（资源/序列化中枢）、`04`（VFS 多协议加载）、`06`（反射）、`13`（字形批处理）、`components/04-ui.md`、`components/11-text-input.md`。
> 说明：字体是**资产（Asset）**，不是组件。`TextLabel` / `Button` / `EditBox` 通过 `asset_ref<Font>` 引用它
> （`components/04-ui.md §TextLabel`、`components/11-text-input.md`）。本节补齐"UI 组件已引用、但从未详设"的缺口。

## 1. 定位与形态

- **源字体文件**：`ttf` / `otf`（矢量轮廓）+ 烘焙参数。**矢量字形是唯一设计真相**；位图/SDF 是派生。
- **字体资产（Font）**：一份**描述符**，引用源字体文件 + 烘焙设置（字号、字符集、SDF/位图选项、图集参数）。
- **烘焙二进制（cooked）**：字形图集纹理（atlas）+ 字形度量表 + 字距表（kerning）。运行期只加载 cooked。
- 硬约束：**源字体 + 描述符入库（文本/二进制可 diff，`08 §4`）；图集是派生产物**（内容哈希判定失效，`08 §6`）。

```
FontSource(ttf/otf, 矢量) ──烘焙──▶ { GlyphAtlas(纹理) + Metrics(度量/字距) } ──运行──▶ FontHandle
        ▲                                  ▲
   font.font 描述符(文本) ──────────────┘  字号/字符集/SDF 参数决定烘焙输出
```

## 2. 概念分层

| 概念 | 作用 | 变更频率 |
|------|------|----------|
| **FontSource** | 原始矢量字体文件（ttf/otf） | 极低（换字体才变） |
| **Font（描述符）** | 引用 FontSource + 烘焙设置（字号/字符集/SDF/图集） | 低（美术/技术编辑） |
| **FontAtlas（cooked）** | 字形图集纹理 + 度量表（派生产物） | 随描述符重烘而变 |
| **FontHandle（运行时句柄）** | 已加载的字体实例（图集页 + 度量 + 字形缓存） | 运行时 |

- **描述符决定"烘焙出什么字形、多大、什么风格"**；cooked 是它的产物；句柄是加载后的运行时对象。

## 3. Font 描述符（源文本，开发期真相）

```yaml
font:
  name: ui_regular
  source: "res://fonts/SourceHanSans-Regular.otf"   # FontSource 稳定路径
  mode: SDF                                          # Bitmap | MSDF | SDF
  sizes: [16, 24, 32, 48]                            # 需预烘的像素字号（Bitmap 必需；SDF 单尺寸即可缩放）
  charset:                                           # 字符集（见 §4）
    preset: LatinAndCJK
    ranges: ["0x0020-0x007E", "0x4E00-0x9FFF"]       # Unicode 码点区间
    extra: ["★", "→"]
  sdf: { spread: 8, px_range: 4 }                    # 仅 SDF/MSDF：描边外扩像素 / 距离场范围
  atlas: { max_size: 2048, padding: 2 }              # 图集纹理尺寸上限 / 字形间距
  style: { bold: false, italic: false }              # 从变体/合成得到
  fallback: ["res://fonts/Emoji.ttf"]                # 缺字回退链（emoji/符号）
```

- **省略默认值**：等于 schema 默认的字段不写出（`08 §4`），如 `atlas.max_size` 默认即省略。
- **数组一行一元素**：`sizes` / `ranges` / `fallback` 逐行，减少 diff 冲突（`08 §4`）。
- **`source` 为稳定路径**，禁止绝对路径与顺序号（`08 §4/§5`）。

## 4. 字符集（charset）与烘焙

- **字符集决定哪些字形入图集**（决定图集体积，是烘焙的核心成本项）。
  - `preset`：`Ascii` / `Latin` / `LatinAndCJK` / `AllOfFont`（整字库，慎用）。
  - `ranges`：Unicode 码点区间（含端点），文本可读、可 diff。
  - `extra`：个别符号，直接列字符。
- **动态字形回退**：字符集外的字符在运行期按需**动态上载**到图集（预留 `dynamic` 页）；
  动态路径会产生**非确定时序**，故仅用于 **UI 显示层**，不得进入模拟校验（见 §7）。
- **缺字回退链**：本字体缺字 → 依次尝试 `fallback` 字体 → 最后画替换符（`□`/豆腐块）。

## 5. 位图 / SDF / MSDF 选项

| 模式 | 原理 | 优点 | 代价 | 适用 |
|------|------|------|------|------|
| **Bitmap** | 每字号烘静态字形位图 | 采样最快、像素锐利 | 只有预烘字号，缩放模糊 | 固定字号 UI、像素字体 |
| **SDF** | 单字形距离场，着色器阈值化 | 单尺寸任意缩放/描边/发光 | 尖角略钝（需校正） | 通用 UI 文本 |
| **MSDF** | 多通道距离场 | 尖角锐利、可缩放 | 烘焙慢、图集更大 | 高质量标题/LOGO |

- 模式与 `sizes` 正交：Bitmap 需列出所有用到的 `sizes`；SDF/MSDF 通常只烘一个基准尺寸。
- **描边/发光**（`outline_size`，`components/04-ui.md` 字段）由着色器在距离场上完成，**无需重复烘字形**。
- 像素字体建议 **Bitmap + nearest 采样**，避免距离场软化边缘。

## 6. cooked 产物（烘焙二进制）

- **GlyphAtlas**：一或多页纹理（`atlas.max_size` 上限；超出分页）。内容哈希命名，可 `archive://` 打包（`04 §8`）。
- **GlyphMetrics 表**：每字形 `{ 码点, 图集页/UV 区域, advance, bearing, 基准字号 }`，按**码点升序**排列
  （稳定序，便于 diff/二分，`10 D4`）。
- **Kerning 表**：字距对 `(码点 a, 码点 b) → 偏移`，按 `(a,b)` 稳定排序。
- **行高度 / 上升下降（ascent/descent）**：排版基准，随字号缩放。
- 烘焙**确定**：同一 FontSource + 描述符 + 烘焙器版本 → 同一图集与度量（禁时间戳、排序稳定，`08 §6`）。

## 7. 引用与运行时句柄

| 引用方（组件） | 字段 | 类型 |
|----------------|------|------|
| `TextLabel` | `font` | `asset_ref<Font>`（`components/04-ui.md §TextLabel`） |
| `Button` | `font` | `asset_ref<Font>` |
| `EditBox` | `font` | `asset_ref<Font>`（`components/11-text-input.md`） |
| `ListView` / `Tabs` 等 | 经主题默认或项模板 | `asset_ref<Font>` |

- `asset_ref<Font>` 序列化为**稳定路径**（`08 §5`）；强引用计数控制存活（`07 §6`）。
- **运行时句柄 `FontHandle`**（不透明 `uint64_t`）：指向已加载字体实例（图集页 + 度量 + 动态字形缓存），
  由 `ui` / 渲染模块持有，**标 `transient`、绝不序列化**；反序列化后按稳定路径重建并**重绑**。
- **字形绘制项**：排版 → 每个可见字符产出一个 `{ 字形UV, 位置, 颜色 }` 绘制描述，由渲染按**字体/图集页**批处理
  （`components/04-ui.md`、`13 §5`）。渲染为**只读消费**，不回写文本字段。

## 8. 生命周期 / 确定性

- **加载**：经 VFS 异步加载（`04`），完成回调在**帧边界**处理（`08 §7`）；未就绪时按缺字占位或跳过绘制。
- **排版**在 layout pass 内计算（整形/换行/字距），写 `text_size`（`components/04-ui.md`）。
- **确定性关键约束**：
  - 字形度量/整形可能因**平台 libm / 字体整形库差异**而不同 → `text_size` 等**排版派生量标 `transient`**，
    **默认不进模拟校验和**（`components/04-ui.md §TextLabel`）。
  - 需要像素级回放一致时：使用**引擎内置位图字体**或**规范化的字形度量表**（固定度量，跨平台一致）。
  - **动态字形上载**由帧内加载时机决定，**不得影响模拟字段**；仅作用于显示。
- 热重载：监听源字体/描述符变更 → 重烘 → **帧边界**替换句柄（`08 §7`）。

## 9. 序列化 / diff

- **Font 描述符**：文本入库（JSON 主推 + 可选 YAML/TOML，`08 §4`），字段按 schema 声明序、省略默认、
  数组一行一元素、稳定路径引用 FontSource。
- **cooked 图集**：派生产物，可 gitignore（`08 §3`）；以内容哈希命名、可打包。
- **组件侧**：仅序列化 `asset_ref<Font>`（稳定路径）与字号等设计参数；`FontHandle` / `text_size` 为 transient。

## 10. 风险

| 风险 | 处理 |
|------|------|
| 图集体积爆炸（CJK 全字库） | 按 `charset` 范围 + preset 精确烘焙；`AllOfFont` 慎用；分页 |
| 动态字形破坏确定性 | 动态上载仅限显示层；排版量 transient、不进 checksum |
| 跨平台度量不一致 | 内置位图字体 / 规范化度量表；`text_size` transient |
| 字体授权/体积 | 子集化烘焙（只含字符集）；授权信息走单独元数据，不入字体资产 |
| SDF 尖角钝化 | 高品质需求用 MSDF；或按字号多烘 Bitmap |
| 文本/二进制漂移 | 单一模型 + 往返一致性测试（`08 §9`） |
| 缺字（豆腐块） | `fallback` 回退链 + 导入期字符集覆盖率校验 |
