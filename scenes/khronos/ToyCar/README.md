# ToyCar —— Khronos glTF 官方 PBR 示例（已迁移为 maque 场景）

**来源**：KhronosGroup / glTF-Sample-Assets —— `Models/ToyCar`
（https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar ）
**许可**：CC BY 4.0（上游资产元数据；保留此出处与许可说明）
**内容**：玩具车 + 红布两个部件，3 个 primitive、77k 顶点、3 个 PBR 材质、7 张 PNG 贴图
（baseColor / metallicRoughness / occlusion / normal / emissive 等）。

## 生成方式（离线导入，确定性）

```bash
# 原始 glTF 目录（ToyCar.gltf + ToyCar.bin + *.png）放到任意目录，然后：
# ⚠️ --scene 会重新生成 ToyCar.scn（当前入库的 scn 已手工调过相机/灯光/环境光，别直接覆盖）；
#    只更新素材/材质时去掉 --scene：
python tools/gltf_import.py <源目录>/ToyCar.gltf \
    --out scenes/khronos/ToyCar --name ToyCar --compact
```

- `--scene`：把 glTF 节点树烘焙成平级实体（世界变换写入 `scene.Transform3D.local`），
  自动生成相机（按包围盒取景）与方向光；每个 (node, primitive) 一个实体。
- `--compact`：网格 JSON 紧凑输出（大模型显著减小体积；运行时解析格式不变）。
- 产物：`ToyCar.scn` + `assets/*.mesh.json`（48B 顶点布局）+ `textures/*.png`。
- 材质扩展（plans/19）：`assets/*.mesh.json` 的 materials 已由 importer 写入
  `KHR_materials_clearcoat`（ToyCar 车漆 → Physical + clearcoat=1）与
  `KHR_materials_sheen`（Fabric → Physical + sheen=1/sheenColor=红）；
  `KHR_materials_transmission`（Glass）暂不支持。运行时材质以 `ToyCar.scn` 节点上的
  `asset.PbrMaterial` 组件为准（可在此手调观感）。

## 运行

```bash
cd <repo 根>
./build/samples/player/maque_player scenes/khronos/ToyCar/ToyCar.scn
# 截图
./build/samples/player/maque_player --shot /tmp/toycar.png scenes/khronos/ToyCar/ToyCar.scn
```

`preview.png` 是当前实现的离屏渲染预览（1280×720，方向光 + GGX/ACES + albedo/MR/normal/emissive 贴图
+ clearcoat + 方向光阴影；尚无 IBL/transmission/clearcoat 贴图）。
