# ToyCar —— Khronos glTF 官方 PBR 示例（已迁移为 maque 场景）

**来源**：KhronosGroup / glTF-Sample-Assets —— `Models/ToyCar`
（https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/ToyCar ）
**许可**：CC BY 4.0（上游资产元数据；保留此出处与许可说明）
**内容**：玩具车 + 红布两个部件，3 个 primitive、77k 顶点、3 个 PBR 材质、7 张 PNG 贴图
（baseColor / metallicRoughness / occlusion / normal / emissive 等）。

## 生成方式（离线导入，确定性）

```bash
# 原始 glTF 目录（ToyCar.gltf + ToyCar.bin + *.png）放到任意目录，然后：
python tools/gltf_import.py <源目录>/ToyCar.gltf \
    --out scenes/khronos/ToyCar --name ToyCar --scene --compact
```

- `--scene`：把 glTF 节点树烘焙成平级实体（世界变换写入 `scene.Transform3D.local`），
  自动生成相机（按包围盒取景）与方向光；每个 (node, primitive) 一个实体。
- `--compact`：网格 JSON 紧凑输出（大模型显著减小体积；运行时解析格式不变）。
- 产物：`ToyCar.scn` + `assets/*.mesh.json`（48B 顶点布局）+ `textures/*.png`。

## 运行

```bash
cd <repo 根>
./build/samples/player/maque_player scenes/khronos/ToyCar/ToyCar.scn
# 截图
./build/samples/player/maque_player --shot /tmp/toycar.png scenes/khronos/ToyCar/ToyCar.scn
```

`preview.png` 是当前实现的离屏渲染预览（1280×720，方向光 + GGX/ACES/albedo/MR 贴图；
尚无阴影/IBL/clearcoat/transmission —— 上游 glTF 的 `KHR_materials_*` 扩展目前被忽略）。
