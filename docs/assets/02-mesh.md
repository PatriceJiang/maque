# 资产详设 · 网格（Mesh）

> 上游：`08`（资源/序列化中枢）、`06`（反射/asset 命名空间）、`04`（VFS 加载）、`13`（渲染 WebGPU）、
> `components/03-render.md`（`MeshRenderer` 引用）、`components/07-script-audio-nav.md`（`NavMesh` 几何源）。
> 说明：Mesh 是**资产（Asset）**，不是组件。渲染/导航组件通过 `asset_ref<Mesh>` 引用它。
> 这是补齐"`MeshRenderer.mesh` 已引用、但从未设计"的缺口。

## 1. 定位与归属

- **归属模块**：`renderer`（GPU 消费）；烘焙与导入由 `ai` 模块（`08 §6`）。
- **命名空间**：`asset.Mesh`（`modules/core/data/schema/`）。
- **作用**：承载**几何数据**（顶点位置/法线/UV/顶点色 + 索引 + 子网格划分），供：
  - `MeshRenderer.mesh`（`03 §MeshRenderer`）→ 3D 网格绘制；
  - `NavMesh.geometry`（`07 §NavMesh`）→ 导航烘焙几何源；
  - 骨骼蒙皮（`15 §2`，配 `Skeleton.skinning_matrices`）。

## 2. 源文本（开发期）

- **来源**：美术导出交换格式（glTF / OBJ / FBX…）→ `import` 阶段**规范化**为引擎自有**文本 mesh**。
- **为何引入源文本层**：交换格式多为二进制或不稳定，无法 git diff / AI 读。导入时归一为
  **稳定键序、省略默认、一行一元素**的 JSON（`08 §4`），成为**开发期唯一真相**；原始 FBX 可视为素材、不入库。
- **可再导入**：源文本是网格的权威形态；cook 只做派生，不回写源文本。

```yaml
# 示意：mesh 资产（源文本）
mesh:
  name: tree_lod0
  topology: TriangleList          # TriangleList/LineList/PointList
  vertices:                       # 与 normals/uvs 平行数组（同长）
    - { x: 0,   y: 0,   z: 0 }
    - { x: 0,   y: 1,   z: 0 }
    # …
  normals:
    - { x: 0,   y: 0,   z: 1 }
    # …
  uvs:
    - { x: 0.0, y: 1.0 }
    # …
  indices: [0, 1, 2, 0, 2, 3]
  submeshes:                      # 材质槽（对应 MeshRenderer.material_overrides）
    - { first_index: 0, index_count: 6, material_slot: 0 }
  bounds:                         # 局部包围盒（剔除用，导入时算好，稳定）
    min: { x: -1, y: 0,   z: -1 }
    max: { x:  1, y: 3.0, z:  1 }
```

## 3. 结构 / 字段（schema 风格）

与 `schema/` 的 `asset.Mesh` 对齐，并补足渲染所需（`06 §3` 类型系统）：

```yaml
- namespace: asset
  class_list:
    - enum: Topology
      values: [ { name: TriangleList, value: 0 }, { name: LineList, value: 1 },
                { name: PointList, value: 2 } ]

    - struct: SubMesh             # 子网格（材质槽）划分
      fields:
        - { name: first_index,    type: uint32_t }
        - { name: index_count,    type: uint32_t }
        - { name: material_slot,  type: uint16_t }

    - struct: Aabb               # 局部包围盒
      fields:
        - { name: min, type: Vec3 }
        - { name: max, type: Vec3 }

    - class: Mesh
      fields:
        - { name: vertices,  type: Vec3[] }        # 位置（必填）
        - { name: indices,   type: uint32_t[] }    # 索引（必填）
        - { name: normals,   type: Vec3[] }        # 法线（可空，缺省=面法线/平坦）
        - { name: uvs,       type: Vec2[] }        # 主 UV（可空，缺省={0,0}）
        - { name: colors,    type: Vec4[] }        # 顶点色（可空，缺省白）
        - { name: tangents,  type: Vec4[] }        # 切线（法线贴图用，可空）
        # —— 可选扩展（蒙皮网格）——
        - { name: joints,    type: uint16_t[] }    # 骨骼索引（4/顶点，可空）
        - { name: weights,   type: Vec4[] }        # 蒙皮权重（可空）
        # —— 结构元数据 ——
        - { name: topology,  type: Topology, default: "Topology::TriangleList" }
        - { name: submeshes, type: SubMesh[] }
        - { name: bounds,    type: Aabb }
```

- **平行数组**：`vertices/normals/uvs/colors/tangents/weights` 同长度；缺省属性**省略**（`08 §4`）。
- **索引类型**：源文本用 `uint32[]`（人可读）；**cook 期**按顶点数收窄为 16/32 位。
- **子网格**：划分材质槽，直连 `MeshRenderer.material_overrides`（`03 §MeshRenderer`）。

## 4. 烘焙二进制（发布）

`import → deps → cook` 产出 GPU 友好的 **cooked mesh**（`08 §6`）：

- **交错顶点布局**：按 Effect 输入布局（`13 §5`）重排为 `position|normal|uv|…` 交错缓冲，减少带宽。
- **索引收窄 + 图元优化**：`uint16/uint32` 自动选择；可选顶点缓存优化（须**确定**，禁随机）。
- **量化/压缩（可选）**：位置/法线/UV 半精度或整型量化，**随平台**变体（`05`）。
- **派生字段不入库**：`bounds` 可由源重算，但**缓存进 cooked** 以避免加载期计算。
- **确定**：同一源文本 → 同一字节（排序、无时间戳、内容哈希，`08 §6`）。
- **可 dump**：`dump` 输出机器可读 JSON 摘要（顶点数/子网格/包围盒/哈希），供 AI 检查、diff。

| 形态 | 内容 | 位置 |
|------|------|------|
| 源文本 `.mesh` | schema 字段（可读、可 diff） | 入库 |
| cooked | 交错顶点缓冲 + 索引 + 布局/子网格/包围盒 | 派生（可 gitignore） |

## 5. 引用关系与运行时表示

- **引用**：`MeshRenderer.mesh : asset_ref<Mesh>`、`NavMesh.geometry : asset_ref<Mesh>`（强引用，`07 §6`）。
  - 序列化为**稳定路径**（`res://meshes/tree.mesh`，`08 §5`）；反序列化重建句柄。
- **运行时表示**：
  - **CPU 侧**：网格元数据（子网格、包围盒）+ 可缓存的顶点数据（VFS arena 零拷贝读，`08 §7`）。
  - **GPU 侧**：`renderer/gfx` 据 cooked 创建 `GPUBuffer`（顶点/索引），**异步 + 流式**上传（`13 §5`）。
  - **句柄**：`asset_ref` 存 `{index, generation}`（`07 §3`）；GPU 句柄由 `renderer` 内部持有，**不入组件**。
- **共享**：多对象同 Mesh → 同一 GPU 缓冲，实例化绘制（`13 §5`）。

## 6. 生命周期与确定性

- **加载**：经 VFS 异步（`04`）；大网格分块/预取，**模拟帧内不阻塞**（`04 §6`、`13 §5`）。
- **存活**：`asset_ref` 强引用计数控制；归零即回收 CPU/GPU 资源（`07 §6`、`08 §7`）。
- **热重载**：监听源文本变更 → 重烘 → **帧边界**替换 GPU 缓冲（`08 §7`）。
- **确定性**：网格**只读消费**（渲染/导航），**不回写模拟**、不参与 checksum（`10`、`13 §8`）。
  导航查询在帧内同步、确定（`10 D7`）。cook 结果确定 → 不破坏 A3 回放（`08 §8`）。

## 7. 序列化 / diff 规范

- **源文本**：稳定键序、省略默认、数组一行一元素、浮点最短往返（`08 §4`）。
- **引用**：稳定路径，禁顺序号。
- **禁止入库**：时间戳、生成 uuid、绝对路径、GPU 句柄、设备相关量化中间值。
- **diff 友好**：顶点数组一行一元素便于定位改动的顶点；`submeshes`/`bounds` 独立字段避免噪声。

## 8. 风险

| 风险 | 处理 |
|------|------|
| 大网格文本体积 | 源文本仅 dev；发布用 cooked + 压缩 |
| 交换格式不稳定致源漂移 | import 归一为引擎文本，一次性确立唯一真相 |
| 顶点布局与 Effect 不匹配 | cook 按 Effect 输入布局重排；加载期校验 |
| 量化破坏确定性/精度 | 量化须确定、随平台变体；保留原始源以便重烘 |
| GPU 缓冲加载卡帧 | VFS 异步 + 预取；帧内不阻塞（`04 §6`） |
| 子网格/材质槽错位 | `submeshes` 显式声明 + 加载期范围校验 |
