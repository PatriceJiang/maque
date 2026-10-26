# 资产详设 · 材质（Material / Effect / MaterialInstance）

> 上游：`08`（资源/序列化中枢）、`13`（渲染 WebGPU）、`components/03-render.md`（Sprite/MeshRenderer 引用材质）。
> 说明：材质是**资产（Asset）**，不是组件。渲染组件通过 `asset_ref<Material>` 引用它。
> 这是补齐"渲染组件已引用、但从未设计"的缺口。

## 1. 概念分层

| 概念 | 作用 | 变更频率 |
|------|------|----------|
| **Effect（着色器效果）** | WGSL 源码 + 管线状态 + 参数布局（uniform 声明） | 低（代码级） |
| **Material** | 对一个 Effect 的**一组参数取值** + 渲染状态覆盖 | 中（美术编辑） |
| **MaterialInstance** | 派生自某个 Material，**局部参数覆盖**（逐对象定制） | 高（运行时/大量） |

- **Effect** 决定"能用哪些参数、怎么算"；**Material** 是它的参数预设；**MaterialInstance** 是预设的局部覆写。
- 目的：**换参数不重编管线**（管线按 Effect + 静态状态缓存，docs/13 §5）。

## 2. Effect 资产

- **内容**：WGSL 源码（**文本入库，可 diff/AI 读**，docs/08 §4）+ 声明式**参数布局** + 管线状态默认值。
- **参数布局**：声明 uniform 名/类型/分组（bind group），交由 codegen 生成绑定（docs/06）。
- **参数类型**：`float/int/uint`、`Vec2/3/4`、`Color`(=Vec4)、`Mat3/Mat4`、`asset_ref<Texture>`、`sampler`、数组。
- **渲染状态**：blend（src/dst 因子）、depth_test/depth_write、cull_mode、primitive、stencil。

```yaml
# 示意：effect 资产（文本）
effect:
  name: unlit_sprite
  wgsl: ./shaders/unlit_sprite.wgsl
  params:
    - { name: baseColor, type: Color, default: "{1,1,1,1}" }
    - { name: baseMap,   type: "asset_ref<Texture>", default: null }
    - { name: tint,      type: Vec4, default: "{1,1,1,1}" }
  state:
    blend: { src: SrcAlpha, dst: OneMinusSrcAlpha }
    depth: { test: false, write: false }
    cull: None
```

## 3. Material 资产

- 引用一个 Effect + 为每个参数提供取值 + 可覆盖渲染状态子集。
- **未指定**的参数用 Effect 默认值 → 文本**省略默认值**（docs/08 §4，diff 友好）。

```yaml
material:
  name: player_unlit
  effect: "res://effects/unlit_sprite.effect"
  params:
    baseColor: "{1,0.8,0.2,1}"
    baseMap: "res://textures/player.png"   # asset_ref<Texture>，稳定路径
  state: {}          # 空 = 用 effect 默认
```

## 4. MaterialInstance（运行时/逐对象覆盖）

- `parent: asset_ref<Material>` + **override 列表**（只存被改动的参数）。
- 大量对象共享 Material、个别微调时，用实例而非复制。
- 与 draw call 批处理/合批配合：同 Effect + 同静态状态可合批；实例参数走 uniform/动态 offset。

## 5. 与渲染组件的接口

- `MeshRenderer.materials : asset_ref<Material>[]`（支持多材质子网格）。
- `Sprite.material : asset_ref<Material>`（2D 同样可自定义材质）。
- `TileMap` 每层/每图块可指定材质。
- 渲染提取阶段：`(Effect, state) → pipeline key`；参数打包为 bind group。

## 6. 与其它系统

| 系统 | 关系 |
|------|------|
| 反射（docs/06） | Effect 参数布局 → codegen 绑定；Material 参数类型安全 |
| 持久化（docs/08） | 材质/effect 文本入库、稳定路径、省略默认 |
| 渲染（docs/13） | 管线按 Effect+state 缓存；实例参数 uniform |
| 控制平面（docs/11） | AI 可 dump/改材质参数、截帧验证 |
| 确定性（docs/10） | 材质仅影响渲染，**不回写模拟**，不参与 checksum |

## 6.1 实现切片：builtin 资源 vs assets 资源 + 轻量材质

- **资源 scheme 区分**（VFS）：
  - `builtin:...` → **内建资源**（引擎提供；本切片程序化生成棋盘/白色贴图与默认材质）。
  - 其余（如 `ball.png`、`materials/ball.mat.json`）→ **assets**（场景目录，经 VFS + Wuffs 解码）。
- **轻量材质类型** `scn.SpriteMaterial { base_color: Vec4; texture: string }`：
  材质是一种**资源**（assets 的 `.mat.json` 或 `builtin:`），`Sprite.material` 引用它；
  渲染时材质覆盖 Sprite 的颜色/贴图。完整 `asset.Material`(effect+params) 后续对齐。
- 实测：球用 **assets 材质**（→`ball.png`），子节点用 **builtin 材质**（程序化棋盘）——两者同帧渲染。

## 7. 风险

| 风险 | 处理 |
|------|------|
| 材质爆炸（逐对象一个材质） | 用 MaterialInstance 覆盖，或参数合批 |
| 管线编译卡顿 | 按 Effect+state 缓存 + 预热（docs/13 §5） |
| 参数类型不匹配 | Effect 布局 + codegen 校验，加载期报错 |
| 着色器文本漂移 | 文本入库 + 编译期校验（docs/08） |

## 8. 3D 表面材质（当前实现，plans/19）

3D 网格材质**不**走本文的 Effect/Material 资产模型：现实现为节点上的 `asset.PbrMaterial` 组件
（typed 字段 + 贴图 VFS 路径），`shading_model` 选择 Standard/Physical/Basic/Lambert/Phong/Toon/
Normal/Matcap/Depth/Shadow 十种模型（three.js 材质族语义对齐，见 plans/19-threejs-materials.md）。
Effect/Material/MaterialInstance 仍是 2D/自定义 shader 材质的未来方向；两者都遵循同一条原则：
材质只影响渲染、不回写模拟。
