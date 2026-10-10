# 13 · 渲染扩展（2D）— RichText / Graphics / MotionStreak

> 归属模块：`renderer`（RichText 兼属 `ui`，见下）；文档 `13`（WebGPU / 2D 图层 / DrawList）、
> `12 §5`（2D 图层）、`components/03-render.md`、`components/04-ui.md`、`assets/01-material.md`（材质引用）。
> 上游：`17 §2/§3`（组件约定与记录格式）、`06`（反射 = 组件形状唯一真相）、`07`（句柄 / `asset_ref`）、
> `10`（确定性 / 固定仿真步）、`08`（资产加载与序列化）。
> 本文只定义 3 个扩展组件的**数据形状**；提取、tessellation 与 pass 编排见文档 13。

## 0. 通用约定（本类共同遵守）

- **单向数据流**：模拟（节点树/ECS）→ bake → 提取（cull/batch）→ DrawList → Render Graph。
  三者均为**只读消费方**，产出**中性绘制项**（字形项 / 几何项 / 拖尾带），**绝不回写模拟字段**（ADR-0005，13 §8）。
- **2D 图层归属**：`RichText` 产**字形绘制项**，`Graphics` / `MotionStreak` 产**几何绘制项**，均归
  **2D 图层分支**（`RenderLayer.kind = Canvas2D/UI`，03 §RenderLayer）。
- **配对**：三者均**与 `Control` 同挂于同一节点**——`Control.rect` 提供局部坐标系原点与外框；
  缺失 `Control` = 校验错误（编辑期报错，运行期提取时跳过该节点）。
- **几何在模拟步内生成、确定**：`Graphics` 的图元 tessellation 与 `MotionStreak` 的采点推进都在
  **固定仿真步**内完成，使用**固定细分段数 / 固定容差**，不读墙钟、不用系统熵（`10 §…`）。
- **缓存一律 `transient`**：tessellation 顶点/索引、字形 run 列表、拖尾 ring buffer **不序列化**，
  反序列化后由引擎按字段重建。
- **字段属性（attrs）**（`17 §2.3`）：`transient`（不序列化、运行时派生）、`ref`（引用目标类型）、
  `range`、`unit`、`readonly`。
- **脚本驱动 vs 静态数据**：`Graphics` 支持两种作者方式——**命令数组入库**（静态、可序列化、AI/文本可读）
  与**脚本逐帧重建**（`transient`，不入库）；同一组件可混用，序列化时以入库命令为准。

---

## RichText

- **归属模块 / 上游文档**：`ui`（渲染经 `renderer` 2D 图层）；`04 §TextLabel`（字形绘制项）、`13 §4`。
- **说明**：绘制**带内联标记**的一段富文本。**必须与 `Control` 同挂**；`Control.rect` 为可用包围盒。
  文本用**内联标签**（BBCode 风格）表达颜色、粗斜体、字号、字体、贴图、超链接等区段样式。
  解析产出一串**样式 run**（每段若干字形 + 样式），渲染器据字形图集批处理（`13 §5`）。
  与 `TextLabel` 的关系：`TextLabel` = 单一整段样式；`RichText` = 逐段样式 + 内联图片/链接。
- **依赖类型 / enum**：

```yaml
- enum: RichTagType          # 内联标签种类（解析后 run 的样式来源）
  values: [ { name: Text,          value: 0 },   # 纯文本（无样式）
            { name: Color,         value: 1 },   # [color=#RRGGBBAA]…[/color]
            { name: Bold,          value: 2 },   # [b]…[/b]
            { name: Italic,        value: 3 },   # [i]…[/i]
            { name: Underline,     value: 4 },   # [u]…[/u]
            { name: Strikethrough, value: 5 },   # [s]…[/s]
            { name: Font,          value: 6 },   # [font=res://…]…[/font]
            { name: Size,          value: 7 },   # [size=24]…[/size]
            { name: Outline,       value: 8 },   # [outline=size color]…[/outline]
            { name: Image,         value: 9 },   # [img=res://…;w;h]
            { name: Link,          value: 10 } ] # [url=…]…[/url] / [url]…[/url]
- enum: ImageAlign          # 内联图片基线对齐
  values: [ { name: Baseline, value: 0 }, { name: Center, value: 1 },
            { name: Top, value: 2 }, { name: Bottom, value: 3 } ]
- struct: RichRun            # 解析结果（transient，不入库）
  fields:
    - { name: tag,       type: "RichTagType" }   # run 的主要样式标签
    - { name: text_begin,type: "uint32_t" }      # 在富文本串内的字节范围（用于渲染取字）
    - { name: text_len,  type: "uint32_t" }
    - { name: color,     type: "Color" }
    - { name: font_size, type: "float" }
    - { name: link,      type: "uint32_t" }      # 链到 links 池的索引（0xFFFFFFFF=无）
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `text` | `string` | `""` | — | 含内联标签的文本（UTF-8，BBCode 风格） |
| `parse_tags` | `bool` | `true` | — | 为假时按纯文本绘制（标签原样显示） |
| `font` | `asset_ref<Font>` | 空 | `ref: Font` | 默认字体；空用 UI 主题默认字体 |
| `font_size` | `float` | `16` | `unit: px` `range:[1,512]` | 默认字号（像素） |
| `color` | `Color` | `{1,1,1,1}` | — | 默认文字色（线性空间，含 alpha） |
| `outline_size` | `float` | `0` | `unit: px` | 默认描边宽度；`0`=无 |
| `outline_color` | `Color` | `{0,0,0,1}` | — | 默认描边色 |
| `align_h` | `Align` | `Begin` | — | 水平对齐（复用 `04` 的 `Align`） |
| `align_v` | `Align` | `Begin` | — | 垂直对齐 |
| `wrap_mode` | `TextWrap` | `Word` | — | 换行策略（复用 `04 TextLabel`） |
| `line_spacing` | `float` | `0` | `unit: px` | 行距附加量 |
| `image_align` | `ImageAlign` | `Baseline` | — | `[img]` 内联图片的基线对齐 |
| `meta_underlined` | `bool` | `true` | — | `[url]` 链接是否自动加下划线 |
| `content_size` | `Vec2` | `{0,0}` | `transient` | 排版后内容尺寸（像素，运行时派生） |
| `runs` | `RichRun[]` | `[]` | `transient` | 解析出的样式 run 列表（运行时派生） |
| `links` | `string[]` | `[]` | `transient` | 本帧出现的链接目标池（索引与 run 对应） |

- **句柄/引用**：`font` 为强 `asset_ref<Font>`（07 §6）；`[font=…]` / `[img=…]` 中的路径在解析时经
  VFS 解析为**临时** `asset_ref`（`08 §5`），未就绪时该 run 跳过（不影响其它 run）。
- **事件暴露（链接）**：链接以**信号**暴露（`04 §附`、`14 §2`）：
  `meta_clicked(url: string)`、`meta_hover_started(url: string)`、`meta_hover_ended(url: string)`。
  命中测试消费**输入记录**（`10 §3`），在模拟线程、固定点派发，顺序确定。
- **生命周期 / 确定性**：解析与整形（标签扫描 → run 分段 → 换行）在 **layout pass** 内计算，写
  `runs`/`content_size`。字形度量可能因平台 libm/字体差异而不同，故 `content_size` 标 `transient`、
  **默认不入模拟校验和**；需像素级回放时改用引擎内置位图字体或规范化字形度量表（`04 TextLabel` 同款约束）。
  `text` 变更在帧边界生效。
- **序列化**：`text` 及全部样式字段、`font`（稳定 ID/路径）持久化；`runs`/`links`/`content_size`
  标 `transient`，反序列化后重建。

---

## Graphics

- **归属模块 / 上游文档**：`renderer`（2D 图层）；`13 §2/§5`、`03 §Sprite`（同入 batcher）、`assets/01`（材质）。
- **说明**：2D **矢量绘图**组件。**与 `Control` 同挂**，以 `Control.rect` 左上为局部原点（Y 向下，像素）。
  按一串**图元命令**（路径/线/矩形/圆/椭圆/弧/多边形/贝塞尔）生成几何，tessellation 后进入 **DrawList**
  的 2D 批次（`13 §5`）。用于血条、准星、轨迹、调试线框、程序化装饰等。

### 命令的序列化方案（确定）

绘图命令采用**「命令数组 + 共享控制点池」**的 SoA 结构（反射 POD + arena 动态数组，禁 `std::`，`17 §2.2`）：

- `commands : DrawCommand[]` —— 每条命令是定长 POD，声明「画什么」；
- `points : Vec2[]` —— 所有命令的**控制点共享池**，命令用 `(first, count)` 引用区间 `points[first .. first+count)`；
- `point_colors : Color[]` —— 可选**逐点颜色池**，长度与 `points` 对齐；空则由命令色统染。

此方案保证：**序列化稳定**（数组顺序即绘制顺序，可 diff）、**无指针/无嵌套容器**、**可被脚本增量重建**。
命令区间越界（`first+count > points.size()`）为校验错误。

```yaml
- enum: GraphicsOp            # 图元命令类型
  values: [ { name: Line,            value: 0 },   # 2 点
            { name: Polyline,        value: 1 },   # N 点，开放
            { name: Polygon,         value: 2 },   # N 点，闭合（可填充）
            { name: Rect,            value: 3 },   # 2 点（min,max）
            { name: Circle,          value: 4 },   # 1 点(中心) + param.x=半径
            { name: Ellipse,         value: 5 },   # 1 点 + param.xy=半径xy
            { name: Arc,             value: 6 },   # 1 点 + param=半径/起止角
            { name: QuadraticBezier, value: 7 },   # 3 点
            { name: CubicBezier,     value: 8 },   # 4 点
            { name: ConvexPoly,      value: 9 } ]  # N 点，凸多边形快速填充
- enum: GraphicsFlags         # DrawCommand.flags 位域
  values: [ { name: Fill,   value: 1 },   # bit0：填充
            { name: Stroke, value: 2 },   # bit1：描边
            { name: Closed, value: 4 } ]  # bit2：闭合（隐式连线首尾）
- enum: LineJoin              # 线段连接样式
  values: [ { name: Miter, value: 0 }, { name: Bevel, value: 1 }, { name: Round, value: 2 } ]
- enum: LineCap               # 线段端点样式
  values: [ { name: None, value: 0 }, { name: Butt, value: 1 },
            { name: Round, value: 2 }, { name: Square, value: 3 } ]
- enum: FillRule              # 自交多边形填充规则
  values: [ { name: NonZero, value: 0 }, { name: EvenOdd, value: 1 } ]
- struct: DrawCommand
  fields:
    - { name: op,    type: "GraphicsOp" }   # 图元类型
    - { name: first, type: "uint32_t" }     # points 池起始索引
    - { name: count, type: "uint32_t" }     # 控制点数量
    - { name: flags, type: "uint32_t" }     # GraphicsFlags 位域
    - { name: color, type: "Color" }        # 该命令统染（逐点色池存在时相乘）
    - { name: param, type: "Vec4" }         # op 相关标量（半径/角度等）
    - { name: width, type: "float" }        # 该命令描边宽度（像素；0=用组件 width）
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `commands` | `DrawCommand[]` | `[]` | — | 图元命令数组（顺序即绘制顺序） |
| `points` | `Vec2[]` | `[]` | — | 控制点共享池（局部像素坐标） |
| `point_colors` | `Color[]` | `[]` | — | 逐点颜色池；空则用命令色（长度须为 `0` 或 `points.size()`） |
| `fill_color` | `Color` | `{1,1,1,1}` | — | 默认填充色（命令 `color` 为 `{0,0,0,0}` 时回退） |
| `line_color` | `Color` | `{1,1,1,1}` | — | 默认描边色 |
| `width` | `float` | `1` | `unit: px` `range:[0,]` | 默认线宽 |
| `line_join` | `LineJoin` | `Miter` | — | 线段连接样式 |
| `line_cap` | `LineCap` | `Butt` | — | 线段端点样式 |
| `fill_rule` | `FillRule` | `NonZero` | — | 填充规则 |
| `miter_limit` | `float` | `4` | `range:[1,16]` | `Miter` 连接的最大尖角比 |
| `curve_tolerance` | `float` | `0.25` | `unit: px` `range:(0,]` | 贝塞尔/弧自适应细分容差（**固定**，保证确定） |
| `antialiased` | `bool` | `true` | — | 是否生成抗锯齿边缘（几何膨胀法，`13 §5`） |
| `material` | `asset_ref<Material>` | 空 | `ref: Material` | 可选自定义 2D 材质；空用默认 |
| `z_index` | `int32` | `0` | — | 层内排序（同 `RenderLayer` 内越小越先画） |
| `cache_geometry` | `bool` | `true` | — | 为真且命令未变时复用上次 tessellation（`transient` 缓存） |
| `geometry` | `Vec2[]` | `[]` | `transient` | tessellation 输出顶点（运行时派生） |
| `indices` | `uint32_t[]` | `[]` | `transient` | tessellation 输出索引（运行时派生） |
| `dirty` | `bool` | `true` | `transient` | 命令/字段变更置脏，触发重建 |

- **脚本驱动**：脚本经反射写入 `commands`/`points`（或调用引擎内置 helper：`line/rect/circle/polyline/bezier`…）
  逐帧重建几何。重建发生在**模拟步内**、写入 `transient` 池，**不参与校验和**（`13 §8`）；若脚本在模拟步写
  组件字段，则该写入走正常确定路径（`10`）。
- **句柄/引用**：`material` 为强 `asset_ref<Material>`（引用计数，07 §6）。`commands/points/point_colors`
  为**动态数组**（引擎 arena 容器，07 §7）。
- **生命周期 / 确定性**：命令 → 顶点在 **bake/extract 之前**、于模拟步内完成；曲线细分用**固定**
  `curve_tolerance`（自适应细分时按**局部弧长**而非帧时间决定段数），圆用**固定段数表** →
  相同字段生成**逐位相同**的几何。渲染只读消费；后端不可用时丢弃本次绘制，**不回写**组件（13 §8）。
- **序列化**：`commands`/`points`/`point_colors`/样式字段全部持久化（数组顺序稳定、可 diff）；
  `geometry`/`indices`/`dirty` 标 `transient`，反序列化后按 `cache_geometry` 重建。

---

## MotionStreak

- **归属模块 / 上游文档**：`renderer`（2D 图层）；`13 §2/§5`、`10 §…`（固定仿真步）、`assets/01`。
- **说明**：**拖尾/残影**组件。**与 `Control` 同挂**；每个固定仿真步采样一次节点当前 2D 位置
  （来自同节点 `Transform2D`，否则回退 `Control.rect` 中心），形成一条随时间衰减的**带状几何**，
  经 tessellation 进入 **DrawList**（`13 §5`）。用于冲刺轨迹、武器挥舞、流星、光标残影。
- **依赖类型 / enum**：

```yaml
- enum: StreakMode           # 拖尾形态
  values: [ { name: Ribbon, value: 0 },   # 沿采样点的带宽条带（默认）
            { name: Line,   value: 1 },   # 仅折线（无线宽渐变）
            { name: Points, value: 2 } ]  # 逐点圆点
- enum: StreakFade           # 两端淡出策略
  values: [ { name: None,  value: 0 },   # 不淡出（仅按 lifetime 截断）
            { name: Head, value: 1 },   # 仅头部淡出
            { name: Tail, value: 2 },   # 仅尾部淡出
            { name: Both, value: 3 } ]  # 两端淡出
- struct: StreakSample       # 单个采样点（transient ring buffer 元素）
  fields:
    - { name: pos,   type: "Vec2" }   # 采样位置（父局部像素坐标）
    - { name: age,   type: "float" }  # 已存活仿真时间（秒；由固定 dt 累加）
    - { name: width, type: "float" }  # 该点处带宽（供渐变）
```

- **字段表**：

| 字段 | 类型 | 默认 | attrs | 说明 |
|------|------|------|-------|------|
| `mode` | `StreakMode` | `Ribbon` | — | 拖尾形态 |
| `width` | `float` | `16` | `unit: px` `range:[0,]` | 带宽（头部）；尾部按渐变缩放 |
| `width_curve` | `float` | `1` | `range:[0,4]` | 从尾到头的宽度渐变指数（1=线性） |
| `color_start` | `Color` | `{1,1,1,1}` | — | 头部颜色（含 alpha） |
| `color_end` | `Color` | `{1,1,1,0}` | — | 尾部颜色（含 alpha） |
| `fade_mode` | `StreakFade` | `Both` | — | 两端淡出策略 |
| `lifetime` | `float` | `0.5` | `unit: s` `range:[0,]` | 单点存活时长；超时从尾移除 |
| `min_distance` | `float` | `2` | `unit: px` `range:[0,]` | 移动超过该距离才追加新点（防抖） |
| `max_points` | `uint32_t` | `64` | `range:[2,1024]` | 采样点上限（硬上界，超出挤掉最旧点） |
| `points_per_sec` | `float` | `0` | `range:[0,]` | 定点补充采样率；`0`=仅按 `min_distance` 触发 |
| `point_size` | `float` | `4` | `unit: px` | `Points` 模式下每点直径 |
| `texture` | `asset_ref<Texture>` | 空 | `ref: Texture` | 条带贴图（沿轨迹拉伸）；空=纯色 |
| `material` | `asset_ref<Material>` | 空 | `ref: Material` | 可选自定义 2D 材质 |
| `z_index` | `int32` | `0` | — | 层内排序 |
| `emit` | `bool` | `true` | — | 为假时停止取样，已有轨迹按 `lifetime` 自然消退 |
| `samples` | `StreakSample[]` | `[]` | `transient` | 采样 ring buffer（运行时派生） |
| `geometry` | `Vec2[]` | `[]` | `transient` | tessellation 输出条带顶点 |
| `indices` | `uint32_t[]` | `[]` | `transient` | 条带索引 |

- **句柄/引用**：`texture`/`material` 为强 `asset_ref`（引用计数，07 §6）；`samples` 为**动态数组**
  （arena 容器，07 §7，禁 `std::`），逻辑上为容量 `max_points` 的 ring buffer。
- **生命周期 / 确定性（关键）**：
  - 采样与 `age` 推进**严格绑定固定仿真步**：每步对所有 `age += dt`（`dt` = 引擎固定步长，`10`），
    按声明顺序追加/淘汰采样点；**绝不读墙钟**。相同输入序列 → 相同采样序列 → 相同条带几何。
  - 追加条件为纯函数：`distance(末点, 当前位) >= min_distance` 或按 `points_per_sec` 的**步计数**触发。
  - tessellation 在模拟步内、按 `samples` 顺序完成（固定规则），写 `transient` 几何，**不回写**模拟字段。
  - `samples` 为运行时派生，**默认不入校验和**；若需严格逐帧回放，可将 `samples` 纳入快照（其推进会随固定步确定复现）。
- **序列化**：样式/几何参数（`mode/width/color_*/fade_mode/lifetime/min_distance/max_points/…`）持久化；
  `samples`/`geometry`/`indices` 标 `transient`，反序列化后从空缓冲重建（首帧无拖尾）。

---

## 与 13 的对接速览

| 组件 | 图层/分支 | 输入提取 | 输出中性项 | 回写模拟 |
|------|-----------|----------|------------|:--------:|
| `RichText` | 2D/UI 图层 | 富文本 + `runs` + `Control.rect` | 字形/内联图绘制项 | 否 |
| `Graphics` | 2D 图层 | `commands` + `points` + `Control.rect` | tessellation 后的几何项 | 否 |
| `MotionStreak` | 2D 图层 | `samples` ring buffer + `Control.rect` | 条带几何项 | 否 |

> 三者均为 `renderer`（`RichText` 样式解析属 `ui`）模块解释、**布局中立**、可被反射统一读写（`17 §2.6`）。
> 几何/字形生成在**模拟步内、确定**完成（缓存标 `transient`）；渲染器仅消费**可序列化的绘制描述**（`13 §1`），
> 渲染失败或后端不可用**不回写**任何模拟字段（`13 §8`）。

## 状态

| 组件 | 设计 | 评审 | 落地 |
|------|------|------|------|
| RichText | ✅ | ⬜ | ⬜ |
| Graphics | ✅ | ⬜ | ⬜ |
| MotionStreak | ✅ | ⬜ | ⬜ |
