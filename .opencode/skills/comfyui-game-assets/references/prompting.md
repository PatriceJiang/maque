# Qwen Image 2.1 游戏素材提示词指南

给 `scripts/gen.py` 用的实用手册。所有结论基于本机实测 + 官方模板说明
（模板位于 `~/Ext/ComfyUI/.venv/.../comfyui_workflow_templates_json/templates/image_qwen_image_2_1_*.json`）。

## 模型能力速查

| 能力 | 说明 |
|---|---|
| 中文 / 英文文字渲染 | 很强。图上要出现的字直接写进 prompt，用引号标出，例如 `title "开始游戏"` |
| 原生透明背景 | 需要官方包裹写法（见下），输出真正的 RGBA 通道 |
| 原生 2K | 1024²（1MP）默认；2048²（4MP）原生支持；宽高尽量 32 的倍数 |
| 参考图 | 1–16 张，prompt 里用 `<image1>` … `<image16>` 指代 |
| cfg=1 | 官方路径；此时 negative 无效。只有需要 negative 时才调大 cfg |
| steps | 25 起步（模板默认）；要更高质感用 40–50；"advanced sampler 可以少步数" |
| 种子 | 同 prompt + 同 seed = 同图；迭代时锁 seed 只改一个要素 |

## 素材类 prompt 结构

一句话公式：**主体 → 视角/构图 → 风格 → 背景 → 光照/质感**。

好的例子（道具图标）：
> A golden key item icon for a fantasy RPG game, 3/4 view, clean readable
> silhouette, hand-painted game asset style, centered, soft rim light, subtly
> glowing. The image has an alpha channel and a transparent background.

好的例子（中式 UI）：
> 游戏主菜单界面设计稿，深色水墨风格，标题大字「魔雀」，下方三个横排按钮
> 分别写着「开始游戏」「设置」「退出」，边角有金色回纹装饰，界面整洁留白，
> 适合 1920x1080 分辨率。（英文亦可，混写没问题）

## 风格词表

| 想要的感觉 | 英文提示词片段 | 备注 |
|---|---|---|
| 像素风 | `pixel art, crisp square pixels, limited palette, 16-bit era JRPG style` | 生成 1024² 后用 `magick -filter point -resize 6.25%` 缩到 64 更"像素"；直接让小图出像素网格很难 |
| 手绘奇幻 | `hand-painted fantasy game asset style, soft brushwork, painterly` | 道具/立绘通吃 |
| 卡通渲染 | `cel-shaded, anime style, clean line art, vivid colors` | 角色/UI |
| 扁平矢量 | `flat vector style, minimal, thick outlines, solid colors` | UI 图标、logo |
| 等距 2.5D | `isometric view, 2.5D diorama, game asset` | 建筑/场景块 |
| 低多边形 | `low poly render, faceted geometry, stylized` | 3D 风概念图 |
| 写实质感 | `photorealistic, PBR material, studio lighting` | 材质参考、宣传图 |
| 儿童/休闲 | `cute chibi style, rounded shapes, pastel colors, kawaii` | 休闲游戏 |

## 透明背景（官方写法，gen.py --transparent 会自动包裹）

> This is an RGBA format image with transparency. `[主体描述]`.
> The image has an alpha channel and a transparent background.

- 生成后用 gen.py 的报告确认 `has_alpha` 与透明比例；失败就**换 seed 重试**（比改 prompt 有效）。
- 兜底：让背景变成纯色（`on a solid chroma green background`），再键色扣掉：
  ```bash
  magick in.png -fuzz 8% -transparent '#00FF00' out.png    # 抠绿
  magick in.png -fuzz 6% -transparent white out.png        # 抠白
  ```
- 透明素材边缘常带一圈半透明像素，游戏里出现"描边"时可用
  `magick in.png -channel A -threshold 50% +channel out.png` 硬化边缘。

## 图标 / 道具

- 关键词：`game item icon, centered, single object, clean silhouette, fills the frame`
- 留白：加 `with generous padding around the edges` 防止贴边
- 尺寸：1024² 生成，按需缩：`magick icon.png -resize 256x256 icon_256.png`
- 想一次出全套：`a set of 9 fantasy RPG inventory icons in a 3x3 grid,
  matching art style, consistent lighting` → 再用 `scripts/slice.py --auto` 切分

## 角色 / 立绘

- 立绘：`full body character illustration, standing pose, game character design,
  plain background`
- 做动画素材（配合骨骼/切图）：`T-pose` 或 `A-pose`、`side view`、`front view`
- 三视图：`character turnaround sheet, front view / side view / back view of
  the same character, consistent design`
- **一致性改图**（推荐工作流）：
  1. 先生成一张满意的基准图（多跑几个 seed，锁定一个）；
  2. `--ref 基准图.png` + prompt 里写 `Keep the character design and outfit in
     <image1> unchanged, change the pose to running` 等；
  3. 用 `<image1>` `<image2>` 指代第 1、2 张参考图（官方语法）。
- 多条候选帧/表情：`expression sheet`、`4 different poses` 一张图出多个再切分。

## 精灵表 / 动画帧

- 提示词：`sprite sheet of a slime bouncing, 2x2 grid, four animation frames,
  side view, consistent character, transparent background`
- 模型做逐帧连贯动画有限：帧间会有小漂移，风格统一没问题，**位置/比例要自己检查**；
  商用级动画建议"单帧基准图 + 参考图生成其余帧"而不是一次出全表。
- 切分：`python3 scripts/slice.py sheet.png --cols 2 --rows 2 --trim`
  （格间有透明间隙时可用 `--auto`）

## UI 设计稿

- 整体界面：`game UI screen design for a [fantasy/sci-fi/...] game, [布局描述],
  [配色], [装饰元素], designed for 1920x1080`
  - 16:9 用 `--width 2048 --height 1152`（32 的倍数）
  - **文字要短、要加引号**，一张图里文字分块越少越稳；中英文都可（中文是 Qwen 强项）
- 元件单独出（推荐）：按钮/面板/称号框分开生成，背景透明：
  - 按钮：`a game UI button, elongated rounded rectangle, gold border, dark
    leather texture, empty center, game UI element, transparent background`
  - 面板：`a fantasy game UI panel with ornamental golden corners, dark
    semi-transparent center, ornate frame, transparent background`（做 9-slice 用）
- HUD：`game HUD elements set: health bar, mana bar, minimap frame, skill icons,
  consistent style, transparent background`
- 风格一致性：先定一张"风格基准图"（或配色板），后续都带 `--ref` 并在 prompt 写
  `match the visual style of <image1>`

## 纹理 / 背景

- 无缝贴图：`seamless tileable texture, top-down, hand-painted, [草地/石砖/木纹...],
  no shadows, no text`
- 背景分层（视差）：`parallax background layer, distant mountains` 等分开生成，
  同 `--ref` 保持色调一致；提示词加 `no text, no characters` 避免乱入
- 尺寸建议 1536×1024 或 2048×1024 横构图

## 迭代方法（重要）

1. 第一轮 `--variants 4` 看方向，别急着抠细节
2. 选中方向后**锁 seed**，一次只改 prompt 的一个词，对比效果
3. 改两三处还不对 → 先换 seed，再考虑改 prompt（很多"风格问题"其实是采样运气）
4. 每次生成后**用图片查看工具实际看图**再决定下一步，不要盲改 prompt

## 常见问题速查

| 症状 | 处理 |
|---|---|
| 主体太小 / 太杂 | 加 `centered, close-up, fills the frame, single subject` |
| 透明失败（无 alpha） | 换 seed 重试；确认走了官方包裹；兜底键色扣图 |
| 文字糊 / 错字 | 字号描述加大 `large prominent text`；字数减少；只保留一段文字 |
| 多余的水印/签名 | negative 不可用（cfg=1）→ prompt 里强调 `no watermark, no signature`；或换 seed |
| 画面脏、有杂线 | steps 提到 40–50；加 `clean, polished, professional game art` |
| 像素风不够"像素" | 大图 + `magick -filter point -resize` 缩小，而不是硬求生成小尺寸 |
| 角色每张都变脸 | 用参考图 + `keep the character ... in <image1> unchanged` |
