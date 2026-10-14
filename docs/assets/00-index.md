# 资产详设 · 索引

> 上游：`08`（资源持久化与资产管线）、`06`（反射与代码生成）、`04`（VFS 与多协议）。
> 本文是 `docs/assets/` 系列的总索引：**资产 = 类型 + 源文本形态 + 烘焙二进制形态 + 引用方式 + 归属模块**。
> 每个资产的详设文件遵循同一结构（概念/源文本/烘焙/引用/生命周期/序列化/风险）。

## 1. 系列定位

- 资产（Asset）是**被引用的数据单元**（`08 §1`），不是组件。
- **源文本是开发期唯一真相**（入库、git diff、AI 可读）；**烘焙二进制是发布派生形态**（可再生缓存）。
- 两者由**同一反射模型**派生 → 无损往返（`08 §3`）；引用一律用 `asset_ref<T>`（`07 §6`）。
- 全套资产由 `ai` 模块的管线处理：`import → deps → cook → pack → dump`（`08 §6`）。

## 2. 总表

| 资产 | 类型种类 | 源文本形态（dev，入库） | 烘焙二进制形态（cooked） | 引用方式 | 归属模块 | 详设 |
|------|----------|--------------------------|---------------------------|----------|----------|------|
| **Effect** | 着色器效果 | `.effect`（JSON/YAML: WGSL 路径 + 参数布局 + 管线状态） | 预编译着色器模块（SPIR-V/WGSL 绑定表）+ 管线状态哈希 | `asset_ref<Effect>`（被 Material 引用） | `renderer` | [01](01-material.md) |
| **Material** | 参数预设 | `.material`（JSON: effect 路径 + 参数取值） | 参数块（bind group 布局 + 常量偏移） | `asset_ref<Material>` | `renderer` | [01](01-material.md) |
| **MaterialInstance** | 局部覆写 | `.materialinst`（JSON: parent + override 列表） | 覆写参数块（叠加在 parent 上） | `asset_ref<MaterialInstance>` | `renderer` | [01](01-material.md) |
| **Mesh** | 几何数据 | `.mesh`（JSON: vertices/indices/normals/uvs…，可 diff） | 顶点/索引缓冲（GPU 友好、交错、压缩） | `asset_ref<Mesh>` | `renderer`（`asset` 命名空间） | [02](02-mesh.md) |
| **Texture** | 像素数据 | 图像文件（PNG/JPG/GIF…）+ `.texture` 导入元数据（文本） | 平台 GPU 纹理（BC/ETC2/ASTC…，含 mip 链） | `asset_ref<Texture>` | `renderer`（`asset` 命名空间） | [03](03-texture.md) |
| **Font** | 字体 | 字体源文件（TTF/OTF…）+ `.font` 导入元数据（文本） | 字形图集 + 度量表（SDF/位图） | `asset_ref<Font>` | `ui` | [04](04-font.md) |
| **AnimationClip** | 属性轨道关键帧 | `.anim`（JSON: 轨道 `field_path` + 关键帧） | 采样/压缩后的轨道缓冲 | `asset_ref<AnimationClip>` | `animation` | [05](05-animation-clip.md) |
| **SkeletonData** | 3D 骨架绑定 | `.skeleton`（JSON: 骨骼名/父索引/绑定/逆绑定矩阵） | 骨骼层级 + 逆绑定矩阵表 | `asset_ref<SkeletonAsset>` | `animation` | [06](06-skeleton-spine.md) |
| **SpineData** | 2D 骨骼图集 | Spine 骨架 JSON/Binary + 图集描述（文本可 diff） | 骨架运行时缓冲 + 图集页（Texture） | `asset_ref<SpineAsset>` | `spine2d` | [06](06-skeleton-spine.md) |

> 已知文件：`01-material.md`（Effect/Material/MaterialInstance）。`02-mesh.md`、`03-texture.md` 为本任务。
> `04`/`05`/`06` 由他人编写，链接按上表预留。

## 3. 命名空间对齐

资产类型与 `modules/core/data/schema/` 的 `asset` 命名空间对齐：

```yaml
- namespace: asset
  class_list:
    - enum: FileFormat            # PNG/JPG/GIF…
    - class: Texture { width, height, file_format, data }
    - class: Mesh    { vertices, indices, normals, uvs }
```

- 反射（`06`）据此生成 C++ POD 头 + C ABI 描述符 + JSON schema，三者同源。
- **cooked 结构**（GPU 缓冲、压缩纹理）是**派生形态**，可超出 schema 的**源字段**（额外携带布局/格式元数据）。
- `asset_ref<T>` 序列化为**稳定 ID / 路径**（禁顺序号，`08 §5`）；反序列化重建句柄。

## 4. 通用约定（全系列共享）

- **稳定键序 / 省略默认 / 一行一元素 / 浮点最短往返**（`08 §4`）。
- **禁止易变字段入库**：时间戳、生成 uuid、绝对路径（`08 §4`）。
- **烘焙确定**：同一输入 → 同一产物（排序、无时间戳、内容哈希判失效，`08 §6`）。
- **运行时只读消费**：渲染类资产不回写模拟，不参与 checksum（`13 §8`、`10`）。
- **加载经 VFS**：异步 + 缓存 + 预取；热重载在**帧边界**替换源文本重烘结果（`04`、`08 §7`）。

## 5. 风险（系列级）

| 风险 | 处理 |
|------|------|
| 文本/二进制漂移 | 单一反射模型 + 往返一致性测试（`08 §9`） |
| 大资源（纹理/网格）体积 | 源文本仅 dev；发布一律 cooked + 压缩 |
| 平台差异（压缩格式/精度） | 随平台烘焙变体，按内容寻址缓存 |
| 引用悬垂 | 稳定 ID + 加载期解析校验（`08 §9`） |
| cook 不确定性 | 内容哈希 + 稳定排序 + 禁时间戳（`08 §6`） |
