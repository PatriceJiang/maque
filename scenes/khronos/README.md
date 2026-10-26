# Khronos glTF-Sample-Assets · 模型迁移索引

本目录把 [KhronosGroup/glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) `Models/` 下的模型逐个迁移为 maque 场景。每个模型一个子目录：

- `<Model>/<Model>.scn` —— maque 场景（自动机位 + 方向光 + 引擎 analytic studio 环境）；
- `<Model>/preview.jpg` —— 本引擎渲染的 800×450 预览（提交）；`preview.png` 为 1280×720 原图（本地，不入库）；
- `<Model>/README.md` —— **独立笔记**：来源/许可/模型统计/生成与运行命令/与官方 screenshot 的定量对比与差异归因/已知差距/踩坑；
- `<Model>/assets/`、`textures/` —— 导入产物（`.mesh.json`/贴图，体积大，**不入库**，由 `python tools/migrate_gltf_models.py --only <Model> --force` 从 `.deps/gltf-assets` 重新生成）。

迁移流程、踩坑与经验：见 [docs/31-gltf-model-migration.md](../../docs/31-gltf-model-migration.md)。机器可读清单：[manifest.json](manifest.json)；**逐模型官方参考图（large 优先）vs 迁移预览对比报告：[reports/khronos-comparison/index.html](../../reports/khronos-comparison/index.html)**。

## 总览

| 项 | 值 |
|---|---|
| 模型数 | 149 |
| 迁移成功（含仅导入） | 148 |
| blocked（主变体必需 Draco 等） | 0 |
| 失败/异常 | 1 |
| 派生资产总大小（本地，不入库） | 974.2 MB |

> ⚠️ 与官方 `screenshot/` 对比**不是像素级 pass/fail**：官方图多为离线渲染（常带 HDRI studio、软阴影、不同机位/背景），maque 暂无 HDRI IBL/transmission/软阴影/动画骨骼；每个模型的 README 给出定量指标与差异归因。

## 模型列表

| 模型 | 标签 | 顶点/三角形 | 材质/纹理 | 已知差距 | 状态 | 预览 | 笔记 |
|---|---|---|---|---|---|---|---|
| [ABeautifulGame](ABeautifulGame/README.md) | showcase、video、extension | 408,691 / 574,528 | 15 / 43 | 2 | `ok` | [图](ABeautifulGame/preview.jpg) | [笔记](ABeautifulGame/README.md) |
| [AlphaBlendModeTest](AlphaBlendModeTest/README.md) | core、testing | 137 / 61 | 6 / 4 | 1 | `ok` | [图](AlphaBlendModeTest/preview.jpg) | [笔记](AlphaBlendModeTest/README.md) |
| [AnimatedColorsCube](AnimatedColorsCube/README.md) | extension、testing | 96 / 48 | 4 / 0 | 2 | `ok` | [图](AnimatedColorsCube/preview.jpg) | [笔记](AnimatedColorsCube/README.md) |
| [AnimatedCube](AnimatedCube/README.md) | core、testing | 36 / 12 | 1 / 1 | 1 | `ok` | [图](AnimatedCube/preview.jpg) | [笔记](AnimatedCube/README.md) |
| [AnimatedMorphCube](AnimatedMorphCube/README.md) | core、testing | 24 / 12 | 1 / 0 | 2 | `ok` | [图](AnimatedMorphCube/preview.jpg) | [笔记](AnimatedMorphCube/README.md) |
| [AnimatedTriangle](AnimatedTriangle/README.md) | core、testing、written | 3 / 1 | 0 / 0 | 1 | `ok` | [图](AnimatedTriangle/preview.jpg) | [笔记](AnimatedTriangle/README.md) |
| [AnimationPointerUVs](AnimationPointerUVs/README.md) | extension、testing | 178,821 / 59,607 | 82 / 61 | 14 | `ok` | [图](AnimationPointerUVs/preview.jpg) | [笔记](AnimationPointerUVs/README.md) |
| [AnisotropyBarnLamp](AnisotropyBarnLamp/README.md) | showcase、extension | 7,712 / 10,203 | 3 / 4 | 4 | `ok` | [图](AnisotropyBarnLamp/preview.jpg) | [笔记](AnisotropyBarnLamp/README.md) |
| [AnisotropyDiscTest](AnisotropyDiscTest/README.md) | extension、testing | 3,279 / 2,788 | 12 / 1 | 1 | `ok` | [图](AnisotropyDiscTest/preview.jpg) | [笔记](AnisotropyDiscTest/README.md) |
| [AnisotropyRotationTest](AnisotropyRotationTest/README.md) | extension、testing | 5,799 / 7,476 | 6 / 7 | 1 | `ok` | [图](AnisotropyRotationTest/preview.jpg) | [笔记](AnisotropyRotationTest/README.md) |
| [AnisotropyStrengthTest](AnisotropyStrengthTest/README.md) | extension、testing | 53,311 / 97,240 | 50 / 1 | 1 | `ok` | [图](AnisotropyStrengthTest/preview.jpg) | [笔记](AnisotropyStrengthTest/README.md) |
| [AntiqueCamera](AntiqueCamera/README.md) | core、testing、issues | 21,178 / 20,066 | 2 / 6 | 0 | `ok` | [图](AntiqueCamera/preview.jpg) | [笔记](AntiqueCamera/README.md) |
| [AttenuationTest](AttenuationTest/README.md) | testing、extension | 464 / 232 | 18 / 3 | 2 | `ok` | [图](AttenuationTest/preview.jpg) | [笔记](AttenuationTest/README.md) |
| [Avocado](Avocado/README.md) | core、testing | 406 / 682 | 1 / 3 | 0 | `ok` | [图](Avocado/preview.jpg) | [笔记](Avocado/README.md) |
| [BarramundiFish](BarramundiFish/README.md) | core、testing | 2,188 / 3,864 | 1 / 3 | 0 | `ok` | [图](BarramundiFish/preview.jpg) | [笔记](BarramundiFish/README.md) |
| [BoomBox](BoomBox/README.md) | core、testing | 3,575 / 6,036 | 1 / 4 | 0 | `ok` | [图](BoomBox/preview.jpg) | [笔记](BoomBox/README.md) |
| [BoomBoxWithAxes](BoomBoxWithAxes/README.md) | core、testing | 10,230 / 16,688 | 2 / 5 | 0 | `ok` | [图](BoomBoxWithAxes/preview.jpg) | [笔记](BoomBoxWithAxes/README.md) |
| [Box](Box/README.md) | core、testing | 24 / 12 | 1 / 0 | 0 | `ok` | [图](Box/preview.jpg) | [笔记](Box/README.md) |
| [Box With Spaces](Box With Spaces/README.md) | core、testing | 24 / 12 | 1 / 3 | 0 | `ok` | [图](Box With Spaces/preview.jpg) | [笔记](Box With Spaces/README.md) |
| [BoxAnimated](BoxAnimated/README.md) | core、testing | 320 / 254 | 2 / 0 | 1 | `ok` | [图](BoxAnimated/preview.jpg) | [笔记](BoxAnimated/README.md) |
| [BoxInterleaved](BoxInterleaved/README.md) | core、testing | 24 / 12 | 1 / 0 | 0 | `ok` | [图](BoxInterleaved/preview.jpg) | [笔记](BoxInterleaved/README.md) |
| [BoxTextured](BoxTextured/README.md) | core、issues、testing | 24 / 12 | 1 / 1 | 0 | `ok` | [图](BoxTextured/preview.jpg) | [笔记](BoxTextured/README.md) |
| [BoxTexturedNonPowerOfTwo](BoxTexturedNonPowerOfTwo/README.md) | core、issues、testing | 24 / 12 | 1 / 1 | 0 | `ok` | [图](BoxTexturedNonPowerOfTwo/preview.jpg) | [笔记](BoxTexturedNonPowerOfTwo/README.md) |
| [BoxVertexColors](BoxVertexColors/README.md) | core、testing | 24 / 12 | 0 / 0 | 0 | `ok` | [图](BoxVertexColors/preview.jpg) | [笔记](BoxVertexColors/README.md) |
| [BrainStem](BrainStem/README.md) | core、testing | 34,159 / 61,666 | 59 / 0 | 2 | `ok` | [图](BrainStem/preview.jpg) | [笔记](BrainStem/README.md) |
| [Cameras](Cameras/README.md) | core、testing | 4 / 2 | 0 / 0 | 1 | `ok` | [图](Cameras/preview.jpg) | [笔记](Cameras/README.md) |
| [CarConcept](CarConcept/README.md) | showcase、extension | 162,766 / 213,347 | 29 / 15 | 5 | `ok` | [图](CarConcept/preview.jpg) | [笔记](CarConcept/README.md) |
| [CarbonFibre](CarbonFibre/README.md) | extension | 2,129 / 3,968 | 1 / 3 | 1 | `ok` | [图](CarbonFibre/preview.jpg) | [笔记](CarbonFibre/README.md) |
| [CesiumMan](CesiumMan/README.md) | core、issues、testing | 3,273 / 4,672 | 1 / 1 | 2 | `ok` | [图](CesiumMan/preview.jpg) | [笔记](CesiumMan/README.md) |
| [CesiumMilkTruck](CesiumMilkTruck/README.md) | core、issues、testing | 3,995 / 2,856 | 4 / 2 | 1 | `ok` | [图](CesiumMilkTruck/preview.jpg) | [笔记](CesiumMilkTruck/README.md) |
| [ChairDamaskPurplegold](ChairDamaskPurplegold/README.md) | extension | 6,275 / 9,984 | 4 / 9 | 3 | `ok` | [图](ChairDamaskPurplegold/preview.jpg) | [笔记](ChairDamaskPurplegold/README.md) |
| [ChronographWatch](ChronographWatch/README.md) | showcase、extension | 104,765 / 100,002 | 29 / 8 | 4 | `ok` | [图](ChronographWatch/preview.jpg) | [笔记](ChronographWatch/README.md) |
| [ClearCoatCarPaint](ClearCoatCarPaint/README.md) | extension、testing | 1,728 / 3,072 | 1 / 1 | 2 | `ok` | [图](ClearCoatCarPaint/preview.jpg) | [笔记](ClearCoatCarPaint/README.md) |
| [ClearCoatTest](ClearCoatTest/README.md) | testing、extension | 20,106 / 37,116 | 19 / 6 | 1 | `ok` | [图](ClearCoatTest/preview.jpg) | [笔记](ClearCoatTest/README.md) |
| [ClearcoatWicker](ClearcoatWicker/README.md) | extension、testing | 1,728 / 3,072 | 1 / 4 | 1 | `ok` | [图](ClearcoatWicker/preview.jpg) | [笔记](ClearcoatWicker/README.md) |
| [CommercialRefrigerator](CommercialRefrigerator/README.md) | showcase、extension | 116,820 / 208,679 | 6 / 10 | 3 | `ok` | [图](CommercialRefrigerator/preview.jpg) | [笔记](CommercialRefrigerator/README.md) |
| [CompareAlphaCoverage](CompareAlphaCoverage/README.md) | core、testing、pbrtest | 108 / 54 | 4 / 6 | 1 | `ok` | [图](CompareAlphaCoverage/preview.jpg) | [笔记](CompareAlphaCoverage/README.md) |
| [CompareAmbientOcclusion](CompareAmbientOcclusion/README.md) | core、testing、pbrtest | 90,118 / 124,862 | 5 / 3 | 0 | `ok` | [图](CompareAmbientOcclusion/preview.jpg) | [笔记](CompareAmbientOcclusion/README.md) |
| [CompareAnisotropy](CompareAnisotropy/README.md) | extension、testing、pbrtest | 19,142 / 36,864 | 4 / 5 | 2 | `ok` | [图](CompareAnisotropy/preview.jpg) | [笔记](CompareAnisotropy/README.md) |
| [CompareBaseColor](CompareBaseColor/README.md) | testing、pbrtest | 27,648 / 9,216 | 2 / 2 | 1 | `ok` | [图](CompareBaseColor/preview.jpg) | [笔记](CompareBaseColor/README.md) |
| [CompareClearcoat](CompareClearcoat/README.md) | extension、testing、pbrtest | 2,019 / 3,840 | 3 / 2 | 1 | `ok` | [图](CompareClearcoat/preview.jpg) | [笔记](CompareClearcoat/README.md) |
| [CompareDispersion](CompareDispersion/README.md) | extension、testing、pbrtest | 198 / 66 | 3 / 2 | 2 | `ok` | [图](CompareDispersion/preview.jpg) | [笔记](CompareDispersion/README.md) |
| [CompareEmissiveStrength](CompareEmissiveStrength/README.md) | extension、testing、pbrtest | 1,346 / 2,560 | 2 / 1 | 0 | `ok` | [图](CompareEmissiveStrength/preview.jpg) | [笔记](CompareEmissiveStrength/README.md) |
| [CompareIor](CompareIor/README.md) | extension、testing、pbrtest | 1,350 / 2,562 | 3 / 4 | 3 | `ok` | [图](CompareIor/preview.jpg) | [笔记](CompareIor/README.md) |
| [CompareIridescence](CompareIridescence/README.md) | extension、testing、pbrtest | 1,346 / 2,560 | 2 / 3 | 1 | `ok` | [图](CompareIridescence/preview.jpg) | [笔记](CompareIridescence/README.md) |
| [CompareMetallic](CompareMetallic/README.md) | core、testing、pbrtest | 1,346 / 2,560 | 2 / 2 | 0 | `ok` | [图](CompareMetallic/preview.jpg) | [笔记](CompareMetallic/README.md) |
| [CompareNormal](CompareNormal/README.md) | core、testing、pbrtest | 3,266 / 6,144 | 2 / 1 | 0 | `ok` | [图](CompareNormal/preview.jpg) | [笔记](CompareNormal/README.md) |
| [CompareRoughness](CompareRoughness/README.md) | core、testing、pbrtest | 1,346 / 2,560 | 2 / 2 | 0 | `ok` | [图](CompareRoughness/preview.jpg) | [笔记](CompareRoughness/README.md) |
| [CompareSheen](CompareSheen/README.md) | extension、testing、pbrtest | 1,346 / 2,560 | 2 / 3 | 1 | `ok` | [图](CompareSheen/preview.jpg) | [笔记](CompareSheen/README.md) |
| [CompareSpecular](CompareSpecular/README.md) | extension、testing、pbrtest | 5,250 / 10,240 | 2 / 2 | 1 | `ok` | [图](CompareSpecular/preview.jpg) | [笔记](CompareSpecular/README.md) |
| [CompareTransmission](CompareTransmission/README.md) | extension、testing、pbrtest | 6,250 / 11,778 | 4 / 4 | 1 | `ok` | [图](CompareTransmission/preview.jpg) | [笔记](CompareTransmission/README.md) |
| [CompareVolume](CompareVolume/README.md) | extension、testing、pbrtest | 6,250 / 11,778 | 4 / 4 | 2 | `ok` | [图](CompareVolume/preview.jpg) | [笔记](CompareVolume/README.md) |
| [Corset](Corset/README.md) | core、testing | 11,505 / 18,324 | 1 / 3 | 0 | `ok` | [图](Corset/preview.jpg) | [笔记](Corset/README.md) |
| [Cube](Cube/README.md) | core、testing | 36 / 12 | 1 / 1 | 0 | `ok` | [图](Cube/preview.jpg) | [笔记](Cube/README.md) |
| [CubeVisibility](CubeVisibility/README.md) | testing | 72 / 36 | 3 / 0 | 3 | `ok` | [图](CubeVisibility/preview.jpg) | [笔记](CubeVisibility/README.md) |
| [DamagedHelmet](DamagedHelmet/README.md) | core、showcase、testing、video | 14,556 / 15,452 | 1 / 5 | 0 | `ok` | [图](DamagedHelmet/preview.jpg) | [笔记](DamagedHelmet/README.md) |
| [DiffuseTransmissionPlant](DiffuseTransmissionPlant/README.md) | showcase、extension | 50,369 / 68,409 | 3 / 7 | 5 | `ok` | [图](DiffuseTransmissionPlant/preview.jpg) | [笔记](DiffuseTransmissionPlant/README.md) |
| [DiffuseTransmissionTeacup](DiffuseTransmissionTeacup/README.md) | showcase、extension | 84,552 / 28,184 | 2 / 6 | 1 | `ok` | [图](DiffuseTransmissionTeacup/preview.jpg) | [笔记](DiffuseTransmissionTeacup/README.md) |
| [DiffuseTransmissionTest](DiffuseTransmissionTest/README.md) | video、written、extension | 5,530 / 4,980 | 29 / 3 | 2 | `ok` | [图](DiffuseTransmissionTest/preview.jpg) | [笔记](DiffuseTransmissionTest/README.md) |
| [DirectionalLight](DirectionalLight/README.md) | testing | 16,122 / 31,800 | 3 / 0 | 2 | `ok` | [图](DirectionalLight/preview.jpg) | [笔记](DirectionalLight/README.md) |
| [DispersionTest](DispersionTest/README.md) | extension、testing | 66,786 / 51,827 | 27 / 2 | 2 | `ok` | [图](DispersionTest/preview.jpg) | [笔记](DispersionTest/README.md) |
| [DragonAttenuation](DragonAttenuation/README.md) | extension | 139,379 / 134,995 | 3 / 2 | 3 | `ok` | [图](DragonAttenuation/preview.jpg) | [笔记](DragonAttenuation/README.md) |
| [DragonDispersion](DragonDispersion/README.md) | extension | 139,449 / 134,995 | 2 / 2 | 2 | `ok` | [图](DragonDispersion/preview.jpg) | [笔记](DragonDispersion/README.md) |
| [Duck](Duck/README.md) | core、testing | 2,399 / 4,212 | 1 / 1 | 1 | `ok` | [图](Duck/preview.jpg) | [笔记](Duck/README.md) |
| [EmissiveStrengthTest](EmissiveStrengthTest/README.md) | testing、extension | 188 / 90 | 6 / 1 | 0 | `ok` | [图](EmissiveStrengthTest/preview.jpg) | [笔记](EmissiveStrengthTest/README.md) |
| [EnvironmentTest](EnvironmentTest/README.md) | testing、extension | 13,794 / 25,344 | 3 / 2 | 1 | `ok` | [图](EnvironmentTest/preview.jpg) | [笔记](EnvironmentTest/README.md) |
| [FlightHelmet](FlightHelmet/README.md) | showcase、testing | 55,392 / 94,722 | 6 / 15 | 1 | `ok` | [图](FlightHelmet/preview.jpg) | [笔记](FlightHelmet/README.md) |
| [Fox](Fox/README.md) | core、testing | 1,728 / 576 | 1 / 1 | 2 | `ok` | [图](Fox/preview.jpg) | [笔记](Fox/README.md) |
| [GlamVelvetSofa](GlamVelvetSofa/README.md) | video、extension | 3,118 / 4,196 | 7 / 2 | 5 | `ok` | [图](GlamVelvetSofa/preview.jpg) | [笔记](GlamVelvetSofa/README.md) |
| [GlassBrokenWindow](GlassBrokenWindow/README.md) | video、extension | 984 / 987 | 3 / 5 | 2 | `ok` | [图](GlassBrokenWindow/preview.jpg) | [笔记](GlassBrokenWindow/README.md) |
| [GlassHurricaneCandleHolder](GlassHurricaneCandleHolder/README.md) | video、written、showcase、extension | 2,386 / 4,126 | 2 / 3 | 2 | `ok` | [图](GlassHurricaneCandleHolder/preview.jpg) | [笔记](GlassHurricaneCandleHolder/README.md) |
| [GlassVaseFlowers](GlassVaseFlowers/README.md) | video、extension | 5,989 / 10,218 | 3 / 4 | 3 | `ok` | [图](GlassVaseFlowers/preview.jpg) | [笔记](GlassVaseFlowers/README.md) |
| [IORTestGrid](IORTestGrid/README.md) | extension | 92,835 / 30,945 | 23 / 1 | 3 | `ok` | [图](IORTestGrid/preview.jpg) | [笔记](IORTestGrid/README.md) |
| [InterpolationTest](InterpolationTest/README.md) | core、testing | 28 / 14 | 2 / 1 | 1 | `ok` | [图](InterpolationTest/preview.jpg) | [笔记](InterpolationTest/README.md) |
| [IridescenceAbalone](IridescenceAbalone/README.md) | extension | 3,159 / 6,036 | 1 / 4 | 1 | `ok` | [图](IridescenceAbalone/preview.jpg) | [笔记](IridescenceAbalone/README.md) |
| [IridescenceDielectricSpheres](IridescenceDielectricSpheres/README.md) | testing、extension | 329,635 / 617,406 | 344 / 1 | 1 | `ok` | [图](IridescenceDielectricSpheres/preview.jpg) | [笔记](IridescenceDielectricSpheres/README.md) |
| [IridescenceLamp](IridescenceLamp/README.md) | showcase、extension | 10,683 / 18,866 | 3 / 3 | 3 | `ok` | [图](IridescenceLamp/preview.jpg) | [笔记](IridescenceLamp/README.md) |
| [IridescenceMetallicSpheres](IridescenceMetallicSpheres/README.md) | testing、extension | 329,635 / 617,406 | 344 / 1 | 1 | `ok` | [图](IridescenceMetallicSpheres/preview.jpg) | [笔记](IridescenceMetallicSpheres/README.md) |
| [IridescenceSuzanne](IridescenceSuzanne/README.md) | testing、extension | 9,963 / 11,808 | 3 / 1 | 4 | `ok` | [图](IridescenceSuzanne/preview.jpg) | [笔记](IridescenceSuzanne/README.md) |
| [IridescentDishWithOlives](IridescentDishWithOlives/README.md) | showcase、video、extension | 14,863 / 24,448 | 4 / 11 | 6 | `ok` | [图](IridescentDishWithOlives/preview.jpg) | [笔记](IridescentDishWithOlives/README.md) |
| [Lantern](Lantern/README.md) | core、testing | 4,145 / 5,394 | 1 / 4 | 0 | `ok` | [图](Lantern/preview.jpg) | [笔记](Lantern/README.md) |
| [LightVisibility](LightVisibility/README.md) | testing | 4 / 2 | 0 / 0 | 4 | `ok` | [图](LightVisibility/preview.jpg) | [笔记](LightVisibility/README.md) |
| [LightsPunctualLamp](LightsPunctualLamp/README.md) | testing、extension | 4,555 / 6,000 | 3 / 7 | 2 | `ok` | [图](LightsPunctualLamp/preview.jpg) | [笔记](LightsPunctualLamp/README.md) |
| [MandarinOrange](MandarinOrange/README.md) | extension、testing | 24,138 / 40,836 | 1 / 4 | 1 | `ok` | [图](MandarinOrange/preview.jpg) | [笔记](MandarinOrange/README.md) |
| [MaterialsVariantsShoe](MaterialsVariantsShoe/README.md) | extension、showcase、written | 13,540 / 22,700 | 3 / 5 | 1 | `ok` | [图](MaterialsVariantsShoe/preview.jpg) | [笔记](MaterialsVariantsShoe/README.md) |
| [MeshPrimitiveModes](MeshPrimitiveModes/README.md) | testing | 49 / 20 | 0 / 0 | 1 | `ok` | [图](MeshPrimitiveModes/preview.jpg) | [笔记](MeshPrimitiveModes/README.md) |
| [MeshoptCubeTest](MeshoptCubeTest/README.md) | extension、testing | 640 / 320 | 10 / 10 | 4 | `ok` | [图](MeshoptCubeTest/preview.jpg) | [笔记](MeshoptCubeTest/README.md) |
| [MetalRoughSpheres](MetalRoughSpheres/README.md) | core、testing | 255,914 / 501,776 | 1 / 2 | 0 | `ok` | [图](MetalRoughSpheres/preview.jpg) | [笔记](MetalRoughSpheres/README.md) |
| [MetalRoughSpheresNoTextures](MetalRoughSpheresNoTextures/README.md) | core、testing | 528,291 / 1,040,409 | 98 / 0 | 0 | `ok` | [图](MetalRoughSpheresNoTextures/preview.jpg) | [笔记](MetalRoughSpheresNoTextures/README.md) |
| [MorphPrimitivesTest](MorphPrimitivesTest/README.md) | core、testing | 30 / 32 | 2 / 1 | 1 | `ok` | [图](MorphPrimitivesTest/preview.jpg) | [笔记](MorphPrimitivesTest/README.md) |
| [MorphStressTest](MorphStressTest/README.md) | core、testing | 1,528 / 2,412 | 2 / 3 | 2 | `ok` | [图](MorphStressTest/preview.jpg) | [笔记](MorphStressTest/README.md) |
| [MosquitoInAmber](MosquitoInAmber/README.md) | showcase、extension | 18,678 / 14,349 | 3 / 5 | 2 | `ok` | [图](MosquitoInAmber/preview.jpg) | [笔记](MosquitoInAmber/README.md) |
| [MultiUVTest](MultiUVTest/README.md) | core、testing | 24 / 12 | 1 / 2 | 1 | `ok` | [图](MultiUVTest/preview.jpg) | [笔记](MultiUVTest/README.md) |
| [MultipleScenes](MultipleScenes/README.md) | core、testing | 7 / 3 | 0 / 0 | 0 | `ok` | [图](MultipleScenes/preview.jpg) | [笔记](MultipleScenes/README.md) |
| [NegativeScaleTest](NegativeScaleTest/README.md) | core、testing | 2,032 / 3,884 | 6 / 2 | 0 | `ok` | [图](NegativeScaleTest/preview.jpg) | [笔记](NegativeScaleTest/README.md) |
| [NodePerformanceTest](NodePerformanceTest/README.md) | testing | 731,850 / 268,268 | 10000 / 10000 | 2 | `ok` | [图](NodePerformanceTest/preview.jpg) | [笔记](NodePerformanceTest/README.md) |
| [NodeVisibilityTest](NodeVisibilityTest/README.md) | testing | 32 / 16 | 8 / 8 | 1 | `ok` | [图](NodeVisibilityTest/preview.jpg) | [笔记](NodeVisibilityTest/README.md) |
| [NormalTangentMirrorTest](NormalTangentMirrorTest/README.md) | core、testing | 2,770 / 5,240 | 1 / 3 | 0 | `ok` | [图](NormalTangentMirrorTest/preview.jpg) | [笔记](NormalTangentMirrorTest/README.md) |
| [NormalTangentTest](NormalTangentTest/README.md) | core、testing | 3,983 / 7,774 | 1 / 3 | 0 | `ok` | [图](NormalTangentTest/preview.jpg) | [笔记](NormalTangentTest/README.md) |
| [OrientationTest](OrientationTest/README.md) | core、testing | 1,048 / 524 | 7 / 0 | 0 | `ok` | [图](OrientationTest/preview.jpg) | [笔记](OrientationTest/README.md) |
| [PlaysetLightTest](PlaysetLightTest/README.md) | test | 89,141 / 129,693 | 6 / 19 | 2 | `ok` | [图](PlaysetLightTest/preview.jpg) | [笔记](PlaysetLightTest/README.md) |
| [PointLightIntensityTest](PointLightIntensityTest/README.md) | extension、testing | 296 / 280 | 3 / 1 | 1 | `ok` | [图](PointLightIntensityTest/preview.jpg) | [笔记](PointLightIntensityTest/README.md) |
| [PotOfCoals](PotOfCoals/README.md) | showcase、extension | 54,669 / 18,223 | 2 / 8 | 1 | `ok` | [图](PotOfCoals/preview.jpg) | [笔记](PotOfCoals/README.md) |
| [PotOfCoalsAnimationPointer](PotOfCoalsAnimationPointer/README.md) | showcase、extension | 54,933 / 18,311 | 3 / 10 | 7 | `ok` | [图](PotOfCoalsAnimationPointer/preview.jpg) | [笔记](PotOfCoalsAnimationPointer/README.md) |
| [PrimitiveModeNormalsTest](PrimitiveModeNormalsTest/README.md) | core、issues、testing | 1,050,220 / 352,082 | 2 / 1 | 2 | `ok` | [图](PrimitiveModeNormalsTest/preview.jpg) | [笔记](PrimitiveModeNormalsTest/README.md) |
| [RecursiveSkeletons](RecursiveSkeletons/README.md) | core、testing、issues | 40 / 76 | 1 / 0 | 2 | `ok` | [图](RecursiveSkeletons/preview.jpg) | [笔记](RecursiveSkeletons/README.md) |
| [RiggedFigure](RiggedFigure/README.md) | core、testing | 370 / 256 | 1 / 0 | 2 | `ok` | [图](RiggedFigure/preview.jpg) | [笔记](RiggedFigure/README.md) |
| [RiggedSimple](RiggedSimple/README.md) | core、testing | 160 / 188 | 1 / 0 | 2 | `ok` | [图](RiggedSimple/preview.jpg) | [笔记](RiggedSimple/README.md) |
| [ScatteringSkull](ScatteringSkull/README.md) | testing | 97,880 / 188,871 | 1 / 1 | 2 | `ok` | [图](ScatteringSkull/preview.jpg) | [笔记](ScatteringSkull/README.md) |
| [SciFiHelmet](SciFiHelmet/README.md) | core、testing | 70,074 / 23,358 | 1 / 4 | 0 | `ok` | [图](SciFiHelmet/preview.jpg) | [笔记](SciFiHelmet/README.md) |
| [SheenChair](SheenChair/README.md) | showcase、video、extension | 22,459 / 39,936 | 6 / 7 | 3 | `ok` | [图](SheenChair/preview.jpg) | [笔记](SheenChair/README.md) |
| [SheenCloth](SheenCloth/README.md) | showcase、video、extension | 58,081 / 115,200 | 1 / 5 | 2 | `ok` | [图](SheenCloth/preview.jpg) | [笔记](SheenCloth/README.md) |
| [SheenTestGrid](SheenTestGrid/README.md) | extension | 64,938 / 21,646 | 19 / 1 | 1 | `ok` | [图](SheenTestGrid/preview.jpg) | [笔记](SheenTestGrid/README.md) |
| [SheenWoodLeatherSofa](SheenWoodLeatherSofa/README.md) | showcase、extension | 139,476 / 46,492 | 6 / 15 | 4 | `ok` | [图](SheenWoodLeatherSofa/preview.jpg) | [笔记](SheenWoodLeatherSofa/README.md) |
| [SimpleInstancing](SimpleInstancing/README.md) | extension、testing | 24 / 12 | 0 / 0 | 1 | `ok` | [图](SimpleInstancing/preview.jpg) | [笔记](SimpleInstancing/README.md) |
| [SimpleMaterial](SimpleMaterial/README.md) | core、testing、written | 3 / 1 | 1 / 0 | 0 | `ok` | [图](SimpleMaterial/preview.jpg) | [笔记](SimpleMaterial/README.md) |
| [SimpleMeshes](SimpleMeshes/README.md) | core、testing、written | 3 / 1 | 0 / 0 | 0 | `ok` | [图](SimpleMeshes/preview.jpg) | [笔记](SimpleMeshes/README.md) |
| [SimpleMorph](SimpleMorph/README.md) | core、testing | 3 / 1 | 0 / 0 | 2 | `ok` | [图](SimpleMorph/preview.jpg) | [笔记](SimpleMorph/README.md) |
| [SimpleSkin](SimpleSkin/README.md) | core、testing、written | 10 / 8 | 0 / 0 | 2 | `ok` | [图](SimpleSkin/preview.jpg) | [笔记](SimpleSkin/README.md) |
| [SimpleSparseAccessor](SimpleSparseAccessor/README.md) | core、testing | 14 / 12 | 0 / 0 | 0 | `ok` | [图](SimpleSparseAccessor/preview.jpg) | [笔记](SimpleSparseAccessor/README.md) |
| [SimpleTexture](SimpleTexture/README.md) | core、testing、written | 4 / 2 | 1 / 1 | 0 | `ok` | [图](SimpleTexture/preview.jpg) | [笔记](SimpleTexture/README.md) |
| [SpecGlossVsMetalRough](SpecGlossVsMetalRough/README.md) | testing、extension | 5,114 / 9,028 | 4 / 8 | 1 | `ok` | [图](SpecGlossVsMetalRough/preview.jpg) | [笔记](SpecGlossVsMetalRough/README.md) |
| [SpecularSilkPouf](SpecularSilkPouf/README.md) | showcase、extension | 41,832 / 81,280 | 1 / 2 | 2 | `ok` | [图](SpecularSilkPouf/preview.jpg) | [笔记](SpecularSilkPouf/README.md) |
| [SpecularTest](SpecularTest/README.md) | testing、extension | 23,831 / 44,828 | 24 / 4 | 1 | `ok` | [图](SpecularTest/preview.jpg) | [笔记](SpecularTest/README.md) |
| [Sponza](Sponza/README.md) | core | 192,496 / 262,267 | 25 / 69 | 1 | `ok` | [图](Sponza/preview.jpg) | [笔记](Sponza/README.md) |
| [StainedGlassLamp](StainedGlassLamp/README.md) | showcase、extension | 51,150 / 35,132 | 13 / 19 | 5 | `ok` | [图](StainedGlassLamp/preview.jpg) | [笔记](StainedGlassLamp/README.md) |
| [SunglassesKhronos](SunglassesKhronos/README.md) | showcase、extension | 7,420 / 13,396 | 5 / 1 | 3 | `ok` | [图](SunglassesKhronos/preview.jpg) | [笔记](SunglassesKhronos/README.md) |
| [Suzanne](Suzanne/README.md) | core、testing | 11,808 / 3,936 | 1 / 2 | 0 | `ok` | [图](Suzanne/preview.jpg) | [笔记](Suzanne/README.md) |
| [TextureCoordinateTest](TextureCoordinateTest/README.md) | core、testing | 20 / 10 | 5 / 1 | 0 | `ok` | [图](TextureCoordinateTest/preview.jpg) | [笔记](TextureCoordinateTest/README.md) |
| [TextureEncodingTest](TextureEncodingTest/README.md) | core、testing | 2,468 / 3,844 | 14 / 8 | 1 | `ok` | [图](TextureEncodingTest/preview.jpg) | [笔记](TextureEncodingTest/README.md) |
| [TextureLinearInterpolationTest](TextureLinearInterpolationTest/README.md) | core、testing | 414 / 642 | 3 / 2 | 1 | `ok` | [图](TextureLinearInterpolationTest/preview.jpg) | [笔记](TextureLinearInterpolationTest/README.md) |
| [TextureSettingsTest](TextureSettingsTest/README.md) | core、testing | 144 / 72 | 10 / 9 | 0 | `ok` | [图](TextureSettingsTest/preview.jpg) | [笔记](TextureSettingsTest/README.md) |
| [TextureTransformMultiTest](TextureTransformMultiTest/README.md) | testing、extension | 2,302 / 3,956 | 29 / 4 | 2 | `ok` | [图](TextureTransformMultiTest/preview.jpg) | [笔记](TextureTransformMultiTest/README.md) |
| [TextureTransformTest](TextureTransformTest/README.md) | testing、extension | 36 / 18 | 9 / 5 | 1 | `ok` | [图](TextureTransformTest/preview.jpg) | [笔记](TextureTransformTest/README.md) |
| [ToyCar](ToyCar/README.md) | hand-tuned | 0 / 0 | 0 / 0 | 0 | `ok (hand-tuned)` | [图](ToyCar/preview.jpg) | [笔记](ToyCar/README.md) |
| [TrafficCone](TrafficCone/README.md) | extension | 10,539 / 17,224 | 4 / 7 | 4 | `ok` | [图](TrafficCone/preview.jpg) | [笔记](TrafficCone/README.md) |
| [TransmissionOrderTest](TransmissionOrderTest/README.md) | extension、testing | 63,401 / 44,786 | 6 / 4 | 3 | `ok` | [图](TransmissionOrderTest/preview.jpg) | [笔记](TransmissionOrderTest/README.md) |
| [TransmissionRoughnessTest](TransmissionRoughnessTest/README.md) | testing、extension | 47,260 / 77,792 | 9 / 9 | 2 | `ok` | [图](TransmissionRoughnessTest/preview.jpg) | [笔记](TransmissionRoughnessTest/README.md) |
| [TransmissionTest](TransmissionTest/README.md) | testing、extension | 66,902 / 128,775 | 14 / 9 | 3 | `ok` | [图](TransmissionTest/preview.jpg) | [笔记](TransmissionTest/README.md) |
| [TransmissionThinwallTestGrid](TransmissionThinwallTestGrid/README.md) | extension | 41,946 / 13,982 | 13 / 1 | 2 | `ok` | [图](TransmissionThinwallTestGrid/preview.jpg) | [笔记](TransmissionThinwallTestGrid/README.md) |
| [Triangle](Triangle/README.md) | core、testing、written | 3 / 1 | 0 / 0 | 0 | `ok` | [图](Triangle/preview.jpg) | [笔记](Triangle/README.md) |
| [TriangleWithoutIndices](TriangleWithoutIndices/README.md) | core、testing | 3 / 1 | 0 / 0 | 0 | `ok` | [图](TriangleWithoutIndices/preview.jpg) | [笔记](TriangleWithoutIndices/README.md) |
| [TwoSidedPlane](TwoSidedPlane/README.md) | core | 6 / 2 | 1 / 3 | 0 | `ok` | [图](TwoSidedPlane/preview.jpg) | [笔记](TwoSidedPlane/README.md) |
| [USDShaderBallForGltf](USDShaderBallForGltf/README.md) | showcase、extension | 17,442 / 32,996 | 3 / 4 | 2 | `ok` | [图](USDShaderBallForGltf/preview.jpg) | [笔记](USDShaderBallForGltf/README.md) |
| [UnlitTest](UnlitTest/README.md) | testing、extension | 192 / 88 | 2 / 0 | 0 | `ok` | [图](UnlitTest/preview.jpg) | [笔记](UnlitTest/README.md) |
| [VertexColorTest](VertexColorTest/README.md) | core、testing | 72 / 36 | 2 / 2 | 0 | `ok` | [图](VertexColorTest/preview.jpg) | [笔记](VertexColorTest/README.md) |
| [VirtualCity](VirtualCity/README.md) | core、testing | 19,483 / 8,383 | 167 / 28 | 2 | `ok` | [图](VirtualCity/preview.jpg) | [笔记](VirtualCity/README.md) |
| [WaterBottle](WaterBottle/README.md) | core、testing、written | 2,549 / 4,510 | 1 / 4 | 0 | `ok` | [图](WaterBottle/preview.jpg) | [笔记](WaterBottle/README.md) |
| [XmpMetadataRoundedCube](XmpMetadataRoundedCube/README.md) | extension、testing | 3,456 / 1,724 | 1 / 0 | 0 | `ok` | [图](XmpMetadataRoundedCube/preview.jpg) | [笔记](XmpMetadataRoundedCube/README.md) |

## blocked / 失败明细

- `ToyCar`：手写调校场景（机位/灯光/环境按参考图手调，不参与自动机位批量生成）

## 复现

```bash
# 1) 下载/解包 Khronos 资产（本机 Node TLS 可用；shell 的 schannel 出网在本环境不稳定）
node .deps/download-gltf-assets.js .deps/gltf-sample-assets.zip
tar -xf .deps/gltf-sample-assets.zip -C .deps/gltf-assets
# 2) 批量迁移 + 预览 + 笔记 + 索引
python tools/migrate_gltf_models.py
# 单个模型 / 重新导入
python tools/migrate_gltf_models.py --only Box --force
```
