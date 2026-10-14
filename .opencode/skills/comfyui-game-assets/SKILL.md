---
name: comfyui-game-assets
description: >-
  用本地 ComfyUI + Qwen Image 2.1 生成游戏素材与 UI 设计稿：透明背景 sprite/图标/道具、
  角色立绘与参考图一致性改图、批量出变体、精灵表切帧、UI 界面/HUD mockup（中文英文文字渲染都强）。
  当用户要求绘制/生成/设计任何游戏视觉内容（游戏素材、游戏美术、图标、物品图、立绘、角色设计、
  精灵图、序列帧、动画帧、贴图纹理、背景图、UI 设计稿、界面 mockup、按钮/面板/HUD、
  游戏 logo），或提到 ComfyUI、Qwen Image、本地生图、AI 画图、给游戏"画/生成"图片时，
  使用本 skill。即使用户没明确说"游戏素材"三个字，只要目标是游戏画面资源就用它。
---

# ComfyUI 游戏素材工坊（Qwen Image 2.1）

用本地 ComfyUI 生成游戏美术：**生成 → 看原图 → 迭代 → 后处理**。
核心入口是封装好的 `scripts/gen.py`（提交任务、等待、把图存回当前项目目录一条龙）。

> 脚本路径相对本 skill 目录解析；在项目里用时写全路径，例如
> `python3 .opencode/skills/comfyui-game-assets/scripts/gen.py --prompt "..."`。

## 关键事实（先读，避免走弯路）

| 事实 | 说明 |
|---|---|
| 服务器 | `http://127.0.0.1:8188`（ComfyUI 装在 `~/Ext/ComfyUI`，脚本会自动拉起） |
| 模型 | 主模型 `qwen_image_2.1_int8_convrot` + 编码器 `qwen3vl_8b_int8_convrot` + `qwen_image_2.1_vae_bf16` |
| 耗时 | 1024²/25 步热机 **约 1–2 分钟**，2K 图约 4–6 分钟；冷启动/闲置后重新加载模型再加 1–3 分钟 |
| 开销与运行方式 | 生成时给足 timeout（如 10 分钟）或后台运行；生成中会每 15s 打印进度 |
| 输出 | 默认存到当前项目 `assets/generated/`（`--out` 可改），同时 ComfyUI `output/skillgen/` 留副本 |
| 透明背景 | 原生支持，但需要官方包裹写法（`--transparent` 自动处理）；**生成后必须验证 alpha** |
| 文字渲染 | 很强（中英皆可）。要出现在图上的字写进 prompt 并加引号 |
| 2K | 原生 2048²；宽高建议 32 的倍数 |
| 参考图 | 1–16 张，做角色/风格一致性；prompt 里用 `<image1>` 指代 |
| 负向提示 | cfg=1（官方默认）时 **negative 无效**；别指望它去掉水印，改用措辞 + 换 seed |

## 标准工作流

1. **明确需求**：素材类型（图标/立绘/UI/贴图/精灵表）、风格、尺寸、要不要透明。
2. **生成**：跑 `gen.py`（见下）。先出 1 张看方向；方向对了再批量。
3. **看图自检**（不可省略）：用读图工具打开生成的文件，检查内容、构图、透明、
   有没有乱码文字。**不要只看脚本报告就宣布完成。**
4. **迭代**：不满意 → 优先换 seed（`--seed` 换一个值）；或锁 seed 只改 prompt 的一个点。
   详见 `references/prompting.md` 的迭代方法。
5. **后处理**：像素风缩放、键色扣背景、精灵表切帧（`scripts/slice.py`）。

## 命令速查

```bash
S=.opencode/skills/comfyui-game-assets            # 本 skill 目录

# 道具图标（透明背景）
python3 $S/scripts/gen.py --prompt "a golden key item icon for a fantasy RPG, clean silhouette, game asset" --transparent

# 批量看方向：4 个变体
python3 $S/scripts/gen.py --prompt "..." --variants 4

# UI 设计稿（16:9，中文文字；文字写进 prompt 并加引号）
python3 $S/scripts/gen.py --width 2048 --height 1152 --prompt "游戏主菜单 UI 设计稿，深色水墨风，标题「魔雀」，三个按钮写着「开始游戏」「设置」「退出」"

# 角色立绘（2K）
python3 $S/scripts/gen.py --width 1536 --height 2048 --steps 40 --prompt "full body character illustration of a young alchemist, game character design, plain background"

# 参考图一致性：保持设计、换姿势（<image1> 指第 1 张参考图）
python3 $S/scripts/gen.py --prompt "Keep the character design and outfit in <image1> unchanged, change the pose to running" --ref hero.png

# 精灵表切帧（格间有透明间隙自动找格子）
python3 $S/scripts/slice.py sheet.png --auto
python3 $S/scripts/slice.py sheet.png --cols 4 --rows 2 --trim

# 像素化（大图缩小的方法最稳）
magick out.png -filter point -resize 6.25% out_64.png

# 不用透明包裹时的兜底：键色扣背景
magick in.png -fuzz 8% -transparent '#00FF00' out.png
```

`gen.py --help` 有全部参数：`--width/--height --steps --cfg --sampler --seed --variants
--transparent --ref --out --name --prompt-file --meta --json`。
长 prompt 建议写进文件用 `--prompt-file`（免去 shell 转义烦恼）。
`--json` 输出一行 JSON 摘要（文件路径/seed/耗时/校验结果），适合自动化流程。

## 透明背景

- 直接用 `--transparent`；它会按官方写法包裹 prompt。**输出后验证**：脚本报告里
  `has_alpha=true` 才算成功（透明占比视主体大小而定：小图标常 90%+，大主体 30%+ 也正常）。
- 失败（无 alpha）时：**先换 seed 重试**；仍失败就让背景出纯色再键色扣：
  `magick in.png -fuzz 8% -transparent '#00FF00' out.png`。
- 边缘毛糙：`magick in.png -channel A -threshold 50% +channel out.png` 硬化 alpha。

## 参考图一致性（改图 / 系列素材）

- `--ref 图1 --ref 图2 ...`（或 `--ref 图1 图2`）最多 16 张；prompt 里用
  `<image1>`、`<image2>` 指代。
- 好用法：① 锁一张满意的基准图 → ② 每次带 `--ref 基准图.png`，prompt 写
  `Keep the ... in <image1> unchanged, change X` → ③ 系列素材风格稳定。
- 参考图分辨率不限，脚本按内容哈希命名上传，不会互相覆盖。

## 批量与种子

- `--variants N` 一次出 N 张（同一批，比连跑 N 次快）；文件名 `_00`、`_01`…。
- 种子会打印；**同 prompt + 同 `--seed` 可复现**。想系列统一就用固定 seed 起步再微调。
- 多个任务会服务器排队串行，不会互相抢显存；不用手动串行化。

## 精灵表切帧

`scripts/slice.py`：`--auto`（按透明间隙找格子，格间无缝时用不了）或
`--cols/--rows`/`--cell WxH` 手动网格；`--trim` 去每帧透明边；`--pad` 留边；
输出 `<sheet>_frames/<prefix>_00.png...`（行优先）。切完**看一下帧数对不对**。

## 提示词要点（速记）

- 结构：主体 → 视角/构图 → 风格 → 背景 → 光照。
- 图标：`centered, single object, clean silhouette, game item icon`。
- 文字要少、加引号；中英文都行。
- 风格词表、UI/立绘/贴图模板、常见问题 → 读 `references/prompting.md`。

## 常见故障

| 症状 | 处理 |
|---|---|
| `Connection refused` / 脚本报未运行 | 脚本会自动拉起 `~/Ext/ComfyUI`；等待期看 `~/Ext/ComfyUI/logs/skill_gen_server.log` |
| 报"找不到 main.py" | ComfyUI 不在默认位置：`export COMFYUI_DIR=/path/to/ComfyUI` 再跑 |
| 等很久没完成 | 冷启动加载模型要 1–3 分钟；`/queue` 看是否排队；可调大 `--timeout` |
| 生成失败（execution_error） | 报错里有 node 信息；多为显存/模型文件缺失；重试一次多半能过 |
| 图里出现不想要的文字/水印 | 拆小画面、多生成几轮，或换 seed；记住 cfg=1 时 negative 无效 |
| 一切都是空的/全透明 | 换 seed；确认 prompt 描述具体（"a sword" 比 "武器" 稳） |

## 进阶（手写工作流）

要改结构（加 LoRA、img2img、实验节点）时读 `references/api.md`：
端点表、实测通过的 API-format 图、参考图接线（`images.image_N` 扁平键）、
`/object_info` 查参数、官方模板文件位置。默认**优先用 gen.py**，别重造轮子。

## 维护提醒

- 模型文件名、采样默认值在 `scripts/gen.py` 顶部的 `MODELS` / argparse 默认值里，
  与 `references/api.md` 同步维护。
