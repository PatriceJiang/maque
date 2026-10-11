# 18 · 节点/组件 API 完备性对照

> **本文是"参考对照"文档，不是设计来源**：参考基准 与 Our Machinery（docs/03）
> 仅作为**参考基准**用于完备性体检；maque 不依赖、不模仿其实现，代码与命名也不使用其专有名称。
> 目的：以主流 2D 引擎的公开 API 面为基准做**完备性体检**，找出 maque 缺失的节点/组件与资产。
> 图例：✅ 已有 · 🔶 等价/部分 · ❌ 缺失 · ➕ maque 独有（参考基准无）。
>
> **实现现状（2026-10-11 更新）**：§1 的表格是**设计期**的组件清单（类型已全部在 schema 中声明）；
> 引擎侧**语义实现**本轮大幅补齐，按参考基准的 API 面新增 50 个接口（全部 DSL 声明 → 生成绑定）：
>
> | 类别 | 已实现语义（引擎按 role 通用处理） | 验证 |
> |------|-----------------------------------|------|
> | Node | 层级（setParent/parent/childCount/childAt/getChildByName/find(path)）、
> 位置/缩放/角度/透明度/zIndex/contentSize/anchor/active、destroy（帧末生效）/isValid | `nodes_e2e` |
> | Component | has/add/remove/count/at、getComponentInChildren、反射字段读写（number/bool/string） | `nodes_e2e` |
> | 事件 | node.on/off/emit/emit(number)，沿父链冒泡 | `nodes_e2e` |
> | UI 布局 | ui.Container（HBox/VBox/Grid）自动布局；ui.Control 盒模型参与命中测试 | `nodes_e2e` 布局断言 |
> | UI 控件 | Slider（拖拽改值 + changed）、CheckBox（点击切换 + toggled）、ProgressBar（值→填充）、
> UIOpacity（子树透明度）、SafeArea（内缩）、ScrollView（内容偏移） | `nodes_e2e` |
> | 物理 | physics.RigidBody + physics.Collider → 装配期建体、帧内步进并回写 Transform2D | `nodes_e2e` 下落断言 |
> | Director | pause/resume/paused/frame/loadScene（帧边界切场景）；控制面 Director 域 | `nodes_e2e` 暂停断言 |
> | 动作/定时 | Tween（linear/ease_in/ease_out/ease_in_out + tween_complete）、schedule/scheduleOnce/unschedule | `nodes_e2e` |
> | 文本 | **真实字体渲染**：FreeType 栅格化 → 图集 → 字形 quad（UV 子矩形）；`scn.Label` / `ui.TextLabel` / `render.RichText` 统一排版（换行 None/Word/Char、对齐 Begin/Center/End、行距、实测 text_w/h/line_count）；**富文本标记** `<color=#RRGGBB>` `<size=NN>` `<b>` `<br/>`（`parse_tags=false` 时当纯文本）；内置 5x7 位图字体作为无字体环境回退 | `text_e2e`（含像素断言） |
>
> **仍未实现**（诚实清单）：3D 相关（MeshRenderer/Light/Billboard/ReflectionProbe/Terrain/Skybox）、
> 骨骼动画与动画状态机、RichText/Graphics/MotionStreak、ScrollBar/PageView、VideoPlayer/WebView、
> AudioSource（本机无音频输出设备）、ConstantForce、LabelOutline/Shadow。

## 1. 对照总表

### 结构与 UI 基础
| 参考基准 | maque | 说明 |
|---|---|---|
| Node | 节点（反射记录） | 🔶 maque 节点=反射记录+组件 |
| UITransform | **Control** | 🔶 等价（2D 盒模型） |
| Canvas（UI 根 + 分辨率策略） | `ui.Canvas`（设计分辨率 + fit_mode + 安全区 + HiDPI） | ✅ 已实现（docs/17 §画布） |
| Widget（对齐父级） | **Control**（锚点/偏移）+ FlexItem | 🔶 等价 |
| Layout（H/V/Grid 自动布局） | **Container** / **FlexBox** | ✅ 且更强（含 Flexbox） |
| SafeArea | — | ❌ 缺失 |
| UIOpacity | — | ❌ 缺失（或用 Control.opacity） |
| BlockInputEvents | — | ❌ 缺失 |

### 2D 渲染 / 文本
| 参考基准 | maque | 说明 |
|---|---|---|
| Sprite | **Sprite** | ✅ |
| Label | **TextLabel** | ✅ |
| RichText | — | ❌ 缺失 |
| Graphics（2D 矢量绘图） | — | ❌ 缺失 |
| Mask | **Mask** | ✅ |
| MotionStreak（拖尾） | — | ❌ 缺失 |
| ParticleSystem2D | **ParticleEmitter**(2D) | 🔶 需补 2D 模式 |
| TiledMap | **TileMap** | ✅ |
| Spine（sp.Skeleton） | **SpineSkeleton** | ✅ |
| DragonBones | —（留扩展） | 🔶 见 components/09 |
| LabelOutline / LabelShadow | — | ❌ 缺失（可由 effect 实现） |

### UI 交互控件
| 参考基准 | maque | 说明 |
|---|---|---|
| Button | **Button** | ✅ |
| Toggle | **CheckBox** | ✅ |
| ToggleContainer | **RadioGroup** | ✅ |
| EditBox | **EditBox** | ✅ |
| ScrollView | **ScrollView** | ✅ |
| ScrollBar | —（并入 ScrollView） | 🔶 未单列 |
| Slider | — | ❌ 缺失（曾提及未设计） |
| ProgressBar | — | ❌ 缺失 |
| PageView | — | ❌ 缺失 |
| VideoPlayer | — | ❌ 缺失 |
| WebView | — | ❌ 缺失 |

### 3D 渲染 / 光照
| 参考基准 | maque | 说明 |
|---|---|---|
| MeshRenderer | **MeshRenderer** | ✅ |
| SkinnedMeshRenderer | MeshRenderer + **Skeleton** | 🔶 等价（拆为组合） |
| Billboard | — | ❌ 缺失 |
| ParticleSystem（3D） | **ParticleEmitter** | ✅ |
| Camera | **Camera3D / Camera2D** | ✅ |
| Light（Dir/Point/Spot） | **Light** | ✅ |
| ReflectionProbe | — | ❌ 缺失 |
| Skybox / Environment | —（场景级） | ❌ 缺失 |
| Terrain | — | ❌ 缺失 |

### 物理
| 参考基准 | maque | 说明 |
|---|---|---|
| RigidBody / RigidBody2D | **RigidBody** | ✅（维度 enum） |
| 各类 Collider | **Collider**（shape enum） | ✅ |
| Joint（Hinge/Fixed…） | **Joint** | ✅ |
| ConstantForce | — | ❌ 缺失 |

### 动画 / 音频
| 参考基准 | maque | 说明 |
|---|---|---|
| Animation | **AnimationPlayer** | ✅ |
| SkeletalAnimation | **Animator + Skeleton** | ✅ |
| AudioSource | **AudioSource** | ✅ |
| —（无显式 Listener） | **AudioListener** | ➕ |
| —（无导航） | **NavAgent / NavMesh** | ➕ |

### 资产（被引用但未设计）
| 参考基准 资产 | maque | 说明 |
|---|---|---|
| Material / MaterialInstance | **已补**（`assets/01-material.md`） | ✅ 本轮 |
| Effect / Shader | Effect 资产 | ✅ 本轮（`assets/01`） |
| Mesh / Texture / Font | `Mesh/Texture/Font` | 🔶 被引用，待逐一详设 |
| AnimationClip / Skeleton 数据 / Spine 数据 | 资产 | 🔶 待详设 |
| Prefab | **Prefab（= Scene 子树）** | ✅ |

## 2. 缺失清单（建议补齐）

**A. UI 控件（6）**：`Slider`、`ProgressBar`、`PageView`、`ScrollBar`、`SafeArea`、`UIOpacity`、`BlockInputEvents`（7）
**B. 2D 渲染（4）**：`RichText`、`Graphics`（矢量）、`MotionStreak`、`LabelOutline/Shadow`
**C. 3D 渲染（4）**：`Billboard`、`ReflectionProbe`、`Skybox/Environment`、`Terrain`
**D. 物理（1）**：`ConstantForce`
**E. 平台控件（2）**：`VideoPlayer`、`WebView`（各平台能力差异大）
**F. 资产**：`Mesh`、`Texture`、`Font`、`AnimationClip`、`Skeleton`数据、`Spine`数据

> 合计约 **18 个组件 + 6 类资产**待补。

## 3. 建议与取舍

1. **材质已补**（本轮）：Material / Effect / MaterialInstance 三层次，见 `assets/01-material.md`。
   这解决了"渲染组件引用材质但无定义"的核心缺口。
2. **优先补高频**：`Slider`、`ProgressBar`、`RichText`、`Graphics`、`UIOpacity`、`SafeArea`、
   `Billboard`、`ConstantForce` —— 这些在真实项目里几乎必用。
3. **可延后/裁剪**：`Terrain`、`ReflectionProbe`、`VideoPlayer`、`WebView`、`Skybox`
   —— 与"麻雀虽小"定位和嵌入式/WASM 目标冲突较大，列为可选。
4. **资产详设**：与组件同等重要，`Mesh/Texture/Font/AnimationClip/…` 应建 `docs/assets/` 逐一设计。

## 4. 下一步

- 需要我把 **A–D 的高频缺失组件**补齐为组件详设吗？（建议先 A+B+C 高频部分）
- 是否把 **`docs/assets/`（Mesh/Texture/Font/…）** 也纳入系列？
