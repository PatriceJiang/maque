# scenes/fonts —— 项目字体

| 文件 | 说明 |
|------|------|
| `AlibabaPuHuiTi-3-55-Regular.ttf` | 阿里巴巴普惠体 3.0 (55 Regular)；含 CJK（约 2.8 万码点），用于中文 UI/文本 |

## 来源与许可

- 来源：阿里巴巴普惠体（Alibaba PuHuiTi 3.0），官方发布页 <https://www.alibabafonts.com/#/font>
- 许可：**SIL Open Font License 1.1**（可免费商用；再分发需随附许可，保留字体名称）
  许可全文见 <https://openfontlicense.org/open-font-license-official-text/>
- 本目录中字体文件**未做修改**（引擎按原文件加载；字形按需栅格化，见 docs/13 §字形管线）

## 引擎如何找到字体

1. `MAQUE_FONT=<ttf>` 环境变量（优先级最高，便于临时替换/CI）
2. **本目录下按文件名排序的第一个 `.ttf`**（默认字体；仓库自带 → 中文开箱可用）
3. 常见系统字体（DejaVu / Lato / Liberation / Tuffy）
4. 内置 5×7 位图字体（无外部依赖，仅 ASCII）

单个组件也可以指定字体：`scn.Label` / `ui.TextLabel` / `render.RichText` 的 `font` 字段
（路径相对工作目录或场景目录解析，例如 `"font": "scenes/fonts/AlibabaPuHuiTi-3-55-Regular.ttf"`）。
