# 18 · 与 Cocos Creator 节点/组件对照

> 目的：以 **Cocos Creator 3.x** 为基准做**完备性体检**，找出 maque 缺失的节点/组件与资产。
> 图例：✅ 已有 · 🔶 等价/部分 · ❌ 缺失 · ➕ maque 独有（Cocos 无）。

## 1. 对照总表

### 结构与 UI 基础
| Cocos Creator | maque | 说明 |
|---|---|---|
| Node | 节点（反射记录） | 🔶 maque 节点=反射记录+组件 |
| UITransform | **Control** | 🔶 等价（2D 盒模型） |
| Canvas（UI 根） | RenderLayer(UI) / 分层 | 🔶 等价 |
| Widget（对齐父级） | **Control**（锚点/偏移）+ FlexItem | 🔶 等价 |
| Layout（H/V/Grid 自动布局） | **Container** / **FlexBox** | ✅ 且更强（含 Flexbox） |
| SafeArea | — | ❌ 缺失 |
| UIOpacity | — | ❌ 缺失（或用 Control.opacity） |
| BlockInputEvents | — | ❌ 缺失 |

### 2D 渲染 / 文本
| Cocos Creator | maque | 说明 |
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
| Cocos Creator | maque | 说明 |
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
| Cocos Creator | maque | 说明 |
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
| Cocos Creator | maque | 说明 |
|---|---|---|
| RigidBody / RigidBody2D | **RigidBody** | ✅（维度 enum） |
| 各类 Collider | **Collider**（shape enum） | ✅ |
| Joint（Hinge/Fixed…） | **Joint** | ✅ |
| ConstantForce | — | ❌ 缺失 |

### 动画 / 音频
| Cocos Creator | maque | 说明 |
|---|---|---|
| Animation | **AnimationPlayer** | ✅ |
| SkeletalAnimation | **Animator + Skeleton** | ✅ |
| AudioSource | **AudioSource** | ✅ |
| —（无显式 Listener） | **AudioListener** | ➕ |
| —（无导航） | **NavAgent / NavMesh** | ➕ |

### 资产（被引用但未设计）
| Cocos Creator 资产 | maque | 说明 |
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
