# 资产详设 · 纹理（Texture）

> 上游：`08`（资源/序列化中枢）、`06`（反射/asset 命名空间）、`04`（VFS 加载）、`13`（渲染 WebGPU）、
> `components/03-render.md`（`Sprite`/`TileMap` 引用）、`components/04-ui.md`（UI 图片/字形图集）、
> `components/06-animation-particle.md`（粒子贴图）。
> 说明：Texture 是**资产（Asset）**，不是组件。各绘制组件通过 `asset_ref<Texture>` 引用它。

## 1. 定位与归属

- **归属模块**：`renderer`（GPU 消费）；导入/烘焙由 `ai` 模块（`08 §6`）。
- **命名空间**：`asset.Texture`（`modules/core/data/schema/`）。
- **作用**：承载**二维像素数据**（颜色/遮罩/精灵表/粒子图/UI 图集），供：
  - `Sprite.texture` / `TileSet`（`03 §Sprite`/`§TileMap`）；
  - `Image.texture` / `TextLabel` 字形图集（`04`）；
  - `ParticleEmitter.texture`（`06 §4`）；
  - `Material` 的 `asset_ref<Texture>` 参数（`01 §2`）。

## 2. 两种形态的分工（关键）

纹理与其它资产不同：**像素本身无法用文本表达**，故采用"**图像文件 + 文本元数据**"双件：

| 形态 | 内容 | 是否入库 | 说明 |
|------|------|:--------:|------|
| **源图像** | PNG / JPG / GIF…（二进制像素） | ✅ | 美术提供、跨工具通用；`diff` 仅能做二进制比对 |
| **源元数据 `.texture`** | 导入选项（sRGB/filter/wrap/mipmap/压缩）+ 引用源图像 | ✅ | **文本、可 diff、AI 可读**（`08 §4`） |
| **cooked 纹理** | 平台 GPU 纹理（BC/ETC2/ASTC，含 mip 链） | ❌（派生） | 随平台变体，内容寻址缓存 |

> **真相归属**：像素真相在**源图像文件**，导入配置真相在**文本元数据**；
> 两者合起来构成开发期唯一真相。cooked 是可再生的发布产物（`08 §3`）。

## 3. 源文本（元数据）

```yaml
# 示意：texture 资产（源元数据）
texture:
  name: player
  source: ./images/player.png      # 源图像（相对路径，稳定）
  srgb: true                       # 颜色空间：颜色贴图 true / 数据贴图(法线/遮罩) false
  filter: Linear                   # Nearest/Linear/LinearMipmap…
  wrap: { u: Repeat, v: Repeat }   # Repeat/ClampToEdge/MirroredRepeat
  mipmap: Auto                     # Off/Auto/Manual(指定级数)
  compression: Auto                # Off/Auto/BC/ETC2/ASTC（随平台）
  max_size: 2048                   # 导入缩放上限（超过则等比缩小，确定）
  pivot: { x: 0.5, y: 0.5 }        # 精灵锚点（2D 用，可选）
```

- **省略默认**：等于默认值的选项**不写出**（`08 §4`）。
- **稳定**：路径、数值稳定；**禁**绝对路径、时间戳、生成 uuid。

## 4. 结构 / 字段（schema 风格）

与 `schema/` 的 `asset.Texture` 对齐（`06 §3`）：

```yaml
- namespace: asset
  class_list:
    - enum: FileFormat
      values: [ { name: PNG, value: 0 }, { name: JPG, value: 1 },
                { name: GIF, value: 2 } ]

    - enum: PixelFormat         # 贴图像素布局（cooked 元数据）
      values: [ { name: RGBA8, value: 0 }, { name: RGBA8_SRGB, value: 1 },
                { name: RGBA16F, value: 2 }, { name: R8, value: 3 },
                { name: BC7, value: 4 }, { name: ETC2_RGBA, value: 5 },
                { name: ASTC_4x4, value: 6 } ]

    - enum: FilterMode
      values: [ { name: Nearest, value: 0 }, { name: Linear, value: 1 } ]

    - enum: WrapMode
      values: [ { name: Repeat, value: 0 }, { name: ClampToEdge, value: 1 },
                { name: MirroredRepeat, value: 2 } ]

    - class: Texture
      fields:
        - { name: width,       type: uint32_t }             # 像素宽
        - { name: height,      type: uint32_t }             # 像素高
        - { name: file_format, type: FileFormat }           # 源图像格式
        - { name: data,        type: uint8_t[] }            # 像素字节（cooked）
        # —— cooked 元数据（派生）——
        - { name: pixel_format, type: PixelFormat }
        - { name: mip_levels,   type: uint8_t }
        - { name: srgb,         type: bool }
        - { name: filter,       type: FilterMode }
        - { name: wrap_u,       type: WrapMode }
        - { name: wrap_v,       type: WrapMode }
```

- **源字段 ↔ cooked 字段**：`width/height/file_format` 为源级（元数据里可得），`data`/`pixel_format`
  仅存在于 cooked（解码/压缩后的平台形态）。`srgb/filter/wrap` 由导入选项决定并**随平台烘焙**。
- **`FileFormat` 目前仅 PNG/JPG/GIF**；扩展格式（WebP/KTX2/EXR…）时增量加枚举值（`06 §7`）。
  **KTX2/Basis Universal** 当前由 runtime glTF 直读链路解码（`modules/image/src/ktx2_decode.cpp`，
  `KHR_texture_basisu` → RGBA8），不进入本 `FileFormat` cooked 资产枚举。

## 5. 格式、压缩与 mipmap

- **源格式**：PNG（无损/带 alpha，首选）、JPG（有损/照片，无 alpha）、GIF（多帧，导入可展开为精灵表）。
- **压缩（cooked）**：按平台选 GPU 压缩格式——桌面 `BC`（BC7/BC3…）、移动 `ETC2`/`ASTC`；
  `Off`=未压缩 RGBA8（小图/精度敏感）。
  - **确定**：编码器参数固定、无时间戳 → 同输入同产物（`08 §6`）。
  - **随平台变体**：同一源在不同目标平台产出不同 cooked（见 §8）。
- **mipmap**：`Auto`=按 `log2(max(w,h))` 生成完整链；`Off`=仅 mip0；`Manual`=指定级数。
  - 过滤与 mip 互相关联：`filter` 不应高于 mip 可用性（无 mip + `LinearMipmap` 退化警告）。
- **sRGB**：颜色贴图（baseColor/albedo）置 `true`；数据贴图（法线/粗糙度/遮罩/字形覆盖）置 `false`。

## 6. 导入选项总表

| 选项 | 取值 | 默认 | 影响 | 随平台烘焙 |
|------|------|------|------|:----------:|
| `srgb` | bool | `true` | GPU 采样时做 sRGB→线性转换 | ✅ |
| `filter` | `Nearest/Linear` | `Linear` | 放大/缩小采样；像素风用 `Nearest` | ✅ |
| `wrap` | `Repeat/ClampToEdge/MirroredRepeat` | `Repeat` | 越界 UV 寻址 | ✅ |
| `mipmap` | `Off/Auto/Manual` | `Auto` | 是否生成 mip 链及级数 | ✅ |
| `compression` | `Off/Auto/BC/ETC2/ASTC` | `Auto` | cooked 像素格式 | ✅ |
| `max_size` | uint | `2048` | 导入等比缩放上限 | ✅ |
| `pivot` | Vec2 | `{0.5,0.5}` | 2D 精灵锚点（元数据，非 GPU） | ❌ |

## 7. 引用关系、运行时表示与生命周期

- **引用**：各绘制组件的 `asset_ref<Texture>`（强引用，`07 §6`），序列化为**稳定路径**
  （如 `res://textures/player.texture`）；反序列化重建句柄（`08 §5`）。
- **运行时表示**：
  - **GPU**：`renderer/gfx` 据 cooked 创建 `GPUTexture` + `GPUSampler`（按 `filter/wrap`），
    **异步 + 流式**上传（`13 §5`）。
  - **采样器**：`filter/wrap` 相同 → sampler 缓存复用（`13 §5`，按状态哈希）。
  - **句柄**：`asset_ref` 存 `{index, generation}`（`07 §3`）；GPU 句柄由 `renderer` 内部持有，**不入组件**。
- **加载**：经 VFS 异步（`04`）；UI/精灵未就绪时的占位与帧边界回调（`04 §Image`）。
- **存活**：引用计数归零回收 CPU 字节与 `GPUTexture`（`07 §6`、`08 §7`）。
- **热重载**：源图像或 `.texture` 变更 → 重烘 → **帧边界**替换（`08 §7`）。
- **确定性**：纹理**只读消费**，不回写模拟、不参与 checksum（`10`、`13 §8`）。

## 8. 随平台烘焙变体

同一源纹理按目标平台产出**多个 cooked 变体**，以 `(源哈希, 平台, 导入选项)` 为键**内容寻址**缓存：

| 维度 | 变体 |
|------|------|
| 平台/GPU | 桌面（BC7）· 移动（ASTC/ETC2）· WebGPU/WASM（RGBA8 或 BC，取决于能力探测） |
| 能力降级 | 无压缩纹理支持 → 回落未压缩 RGBA8 |
| 尺寸 | 按 `max_size` 缩放；可针对低端平台再降档 |

- **打包**：变体随目标平台打包进归档（`archive://`，`04 §4`）；运行时按平台选择。
- **确定**：每个变体独立确定、可复现、可比对（`08 §6`）；`dump` 输出变体清单供 AI 检查。

## 9. 序列化 / diff 规范

- **元数据文本**：稳定键序、省略默认、一行一选项（`08 §4`）。
- **引用**：源图像相对路径 + 稳定资产路径；禁绝对路径、时间戳。
- **cooked 不入库**：GPU 字节、量化中间值、平台变体均为派生产物。
- **diff 说明**：`.texture` 文本改动（如 `srgb: false`）清晰可 diff；像素改动仅在源图像二进制层面可见
  ——这是纹理资产的固有边界，用**内容哈希**在 dump 中标记（`08 §6`）。

## 10. 风险

| 风险 | 处理 |
|------|------|
| 像素无法文本 diff | 元数据文本化 + 内容哈希标记源图像变更 |
| 压缩破坏 alpha/精度 | 按用途选格式；数据贴图 `srgb:false` + 无损/高精度 |
| 平台变体膨胀存储 | 内容寻址缓存 + 按目标平台仅打包所需变体 |
| 加载卡帧（大图/多图） | VFS 异步 + 预取 + mip 流式（`04`、`13 §5`） |
| sRGB 设错致颜色偏差 | 导入选项显式声明；取样器按 `srgb` 校验 |
| sampler 组合爆炸 | 按 `(filter,wrap)` 哈希缓存复用（`13 §5`） |
