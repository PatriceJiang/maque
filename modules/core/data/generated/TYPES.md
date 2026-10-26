# maque 数据类型参考（自动生成）

> ⚠️ 由 `modules/core/data/parser.js` 从 `schema/` 生成，**请勿手工编辑**。
> 这些类型同时生成 C++ 头（`maque_types.h`）与 C ABI 描述符（`maque_desc.*`）；`role` 表示引擎会通用处理该组件的语义。

共 **247** 个类型。字段格式：`名称: 类型 = 默认值 (attrs)`。

## anim（29） · `world/anim.yaml`

### anim.AnimatedSprite — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `frames` | asset_ref<SpriteFrames> | — | ref: SpriteFrames |
| `animation` | uint32_t | 0 |  |
| `autoplay` | bool | true |  |
| `play_mode` | PlayMode | PlayMode::Loop |  |
| `fps` | float | 10.0F | range: 0.0001,240 |
| `speed` | float | 1.0F | range: 0,100 |
| `frame` | uint16_t | 0 | range: 0,65535 |
| `time` | float | 0.0F | readonly, unit: s |
| `playing` | bool | false |  |
| `flip` | FlipMode | FlipMode::None |  |
| `color` | math.Color | — |  |
| `pivot` | math.Vec2 | {0.5F, 0.5F} | unit: normalized |
| `z_index` | int32_t | 0 |  |
| `material` | asset_ref<Material> | — | ref: Material |

### anim.AnimationPlayer — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `clip` | asset_ref<AnimationClip> | — | ref: AnimationClip |
| `autoplay` | bool | true |  |
| `loop` | LoopMode | LoopMode::Loop |  |
| `update_mode` | UpdateMode | UpdateMode::Process |  |
| `speed` | float | 1.0F | range: 0,100 |
| `playing` | bool | false |  |
| `time` | float | 0.0F | readonly, unit: s |
| `track_mask` | uint64_t | 0xFFFFFFFFFFFFFFFFULL |  |
| `event_sink` | entity_ref | — | ref: Script |

### anim.Animator — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `controller` | asset_ref<AnimatorController> | — | ref: AnimatorController |
| `parameters` | AnimParam[16] | — |  |
| `param_count` | uint8_t | 0 | readonly |
| `current_state` | uint32_t | 0 | transient, readonly |
| `normalized_time` | float | 0.0F | transient, readonly, range: 0,1 |
| `speed` | float | 1.0F | range: 0,100 |
| `apply_physics` | bool | false |  |
| `target` | entity_ref | — | ref: AnimationPlayer |

### anim.AnimParam — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name_hash` | uint32_t | 0 |  |
| `type` | AnimParamType | AnimParamType::Float |  |
| `value` | float | 0.0F |  |
| `ivalue` | int32_t | 0 |  |

### anim.AnimParamType — enum

| 值 | 数值 |
|----|------|
| `Float` | 0 |
| `Int` | 1 |
| `Bool` | 2 |
| `Trigger` | 3 |

### anim.BlendMode — enum

| 值 | 数值 |
|----|------|
| `Alpha` | 0 |
| `Additive` | 1 |
| `Multiply` | 2 |

### anim.BonePose — struct (40B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `position` | math.Vec3 | — |  |
| `rotation` | math.Quat | — |  |
| `scale` | math.Vec3 | {1.0F, 1.0F, 1.0F} |  |

### anim.Curve — struct (136B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `interp` | InterpMode | InterpMode::Linear |  |
| `count` | uint8_t | 0 | range: 0,8 |
| `keys` | CurveKey[8] | — |  |

### anim.CurveKey — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `t` | float | 0.0F |  |
| `value` | float | 0.0F |  |
| `in_tangent` | float | 0.0F |  |
| `out_tangent` | float | 0.0F |  |

### anim.FlipMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Horizontal` | 1 |
| `Vertical` | 2 |
| `Both` | 3 |

### anim.Gradient — struct (164B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `count` | uint8_t | 0 | range: 0,8 |
| `stops` | GradientStop[8] | — |  |

### anim.GradientStop — struct (20B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `t` | float | 0.0F | range: 0,1 |
| `color` | math.Rgba | — |  |

### anim.InterpMode — enum

| 值 | 数值 |
|----|------|
| `Inherit` | -1 |
| `Step` | 0 |
| `Linear` | 1 |
| `Cubic` | 2 |

### anim.LoopMode — enum

| 值 | 数值 |
|----|------|
| `Once` | 0 |
| `Loop` | 1 |
| `PingPong` | 2 |
| `ClampForever` | 3 |

### anim.ParticleEmitter — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `mode` | ParticleMode | ParticleMode::CPU |  |
| `sim_space` | SimSpace | SimSpace::Local |  |
| `texture` | asset_ref<Texture> | — | ref: Texture |
| `material` | asset_ref<Material> | — | ref: Material |
| `blend` | BlendMode | BlendMode::Additive |  |
| `shape` | ShapeType | ShapeType::Point |  |
| `shape_size` | math.Vec3 | — | unit: m |
| `shape_angle` | float | 0.0F | unit: deg, range: 0,180 |
| `rate` | float | 10.0F | range: 0,100000, unit: 1/s |
| `burst_count` | uint16_t | 0 |  |
| `lifetime` | Curve | — | unit: s |
| `start_speed` | Curve | — | unit: m/s |
| `gravity` | math.Vec3 | {0.0F, -9.8F, 0.0F} | unit: m/s2 |
| `start_size` | Curve | — |  |
| `size_over_life` | Curve | — |  |
| `color_over_life` | Gradient | — |  |
| `max_particles` | uint32_t | 1000 | range: 0,1000000 |
| `seed` | uint32_t | 0 |  |
| `prewarm` | float | 0.0F | unit: s |
| `emit` | bool | true |  |
| `sort_mode` | SortMode | SortMode::None |  |
| `rng_state` | uint64_t | 0 | transient |

### anim.ParticleMode — enum

| 值 | 数值 |
|----|------|
| `CPU` | 0 |
| `GPU` | 1 |

### anim.PlayMode — enum

| 值 | 数值 |
|----|------|
| `Once` | 0 |
| `Loop` | 1 |
| `PingPong` | 2 |
| `ClampForever` | 3 |

### anim.ShapeType — enum

| 值 | 数值 |
|----|------|
| `Point` | 0 |
| `Circle` | 1 |
| `Box` | 2 |
| `Sphere` | 3 |
| `Hemisphere` | 4 |
| `Cone` | 5 |
| `Edge` | 6 |

### anim.SimSpace — enum

| 值 | 数值 |
|----|------|
| `Local` | 0 |
| `World` | 1 |

### anim.Skeleton — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `skeleton` | asset_ref<SkeletonData> | — | ref: SkeletonData |
| `bone_root` | entity_ref | — | ref: scene.Node |
| `skinning_mode` | SkinningMode | SkinningMode::Linear |  |
| `poses` | BonePose[] | — | transient |
| `skinning_matrices` | math.Mat4[] | — | transient |
| `bounds` | math.Aabb | — | transient, readonly |

### anim.SkinningMode — enum

| 值 | 数值 |
|----|------|
| `Linear` | 0 |
| `DualQuat` | 1 |

### anim.SortMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `ByDepth` | 1 |
| `ByAge` | 2 |

### anim.SpineAnimState — struct (12B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `time` | float | 0.0F | unit: s |
| `track` | int32_t | 0 |  |
| `loop` | bool | false |  |

### anim.SpineBlend — enum

| 值 | 数值 |
|----|------|
| `Setup` | 0 |
| `First` | 1 |
| `Replace` | 2 |
| `Add` | 3 |
| `Multiply` | 4 |
| `Screen` | 5 |

### anim.SpinePhysTimestep — enum

| 值 | 数值 |
|----|------|
| `Inherit` | 0 |
| `Fixed` | 1 |
| `NoPhysics` | 2 |

### anim.SpineSkeleton — struct (变长（含动态成员）)

role: `spine_skeleton`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `skeleton` | string |  |  |
| `atlas` | string |  |  |
| `skin` | uint32_t | 0 |  |
| `animation` | uint32_t | 0 |  |
| `autoplay` | bool | true |  |
| `play_mode` | PlayMode | PlayMode::Loop |  |
| `speed` | float | 1.0F | range: 0,100 |
| `blend` | SpineBlend | SpineBlend::Replace |  |
| `update_mode` | SpineUpdate | SpineUpdate::Process |  |
| `phys_timestep` | SpinePhysTimestep | SpinePhysTimestep::Inherit |  |
| `flip` | FlipMode | FlipMode::None |  |
| `color` | math.Color | — |  |
| `z_index` | int32_t | 0 |  |
| `material` | string |  |  |
| `slots_from` | int32_t | -1 |  |
| `slots_to` | int32_t | -1 |  |
| `default_mix` | float | 0.0F | unit: s, range: 0,10 |
| `time_scale` | float | 1.0F | range: 0,100 |
| `two_color` | bool | false |  |
| `state` | SpineAnimState | — | transient, readonly |
| `instance` | uint64_t | 0 | transient, readonly |

### anim.SpineUpdate — enum

| 值 | 数值 |
|----|------|
| `Process` | 0 |
| `Physics` | 1 |

### anim.TransitionMode — enum

| 值 | 数值 |
|----|------|
| `Interrupt` | 0 |
| `WhenDone` | 1 |

### anim.UpdateMode — enum

| 值 | 数值 |
|----|------|
| `Process` | 0 |
| `Physics` | 1 |

## asset（46） · `asset.yaml`

### asset.AnimationClip — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `length` | float | 0.0F | unit: s, readonly |
| `loop` | anim.LoopMode | anim.LoopMode::Loop |  |
| `interpolation` | anim.InterpMode | anim.InterpMode::Linear |  |
| `fps` | float | 30.0F |  |
| `tracks` | Track[] | — |  |

### asset.AnimatorController — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `states` | string[] | — |  |
| `parameters` | string[] | — |  |
| `transitions` | string[] | — |  |

### asset.AudioBus — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `volume` | float | 1.0F |  |
| `mute` | bool | false |  |

### asset.AudioClip — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `source` | string | — |  |
| `length` | float | 0.0F | unit: s |
| `sample_rate` | uint32_t | 0 |  |
| `channels` | uint8_t | 1 |  |

### asset.AudioCurve — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `points` | math.Vec2[] | — |  |

### asset.BlendFactor — enum

| 值 | 数值 |
|----|------|
| `Zero` | 0 |
| `One` | 1 |
| `SrcAlpha` | 2 |
| `OneMinusSrcAlpha` | 3 |
| `DstAlpha` | 4 |
| `OneMinusDstAlpha` | 5 |

### asset.BoneB — struct (112B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name_hash` | uint32_t | 0 |  |
| `parent` | int16_t | -1 |  |
| `bind_position` | math.Vec3 | — |  |
| `bind_rotation` | math.Quat | — |  |
| `bind_scale` | math.Vec3 | {1.0F, 1.0F, 1.0F} |  |
| `inverse_bind` | math.Mat4 | — |  |

### asset.CharsetPreset — enum

| 值 | 数值 |
|----|------|
| `Ascii` | 0 |
| `Latin` | 1 |
| `LatinAndCJK` | 2 |
| `AllOfFont` | 3 |

### asset.CullMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Front` | 1 |
| `Back` | 2 |

### asset.DepthPacking — enum

| 值 | 数值 |
|----|------|
| `Basic` | 0 |
| `RGBA` | 1 |

### asset.Effect — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `wgsl` | string | — |  |
| `params` | EffectParam[] | — |  |
| `blend_src` | BlendFactor | BlendFactor::SrcAlpha |  |
| `blend_dst` | BlendFactor | BlendFactor::OneMinusSrcAlpha |  |
| `depth_test` | bool | true |  |
| `depth_write` | bool | true |  |
| `cull_mode` | CullMode | CullMode::Back |  |
| `primitive` | Topology | Topology::TriangleList |  |

### asset.EffectParam — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `type` | ParamType | ParamType::Float |  |
| `default_value` | string | — |  |

### asset.FileFormat — enum

| 值 | 数值 |
|----|------|
| `PNG` | 0 |
| `JPG` | 1 |
| `GIF` | 2 |

### asset.FilterMode — enum

| 值 | 数值 |
|----|------|
| `Nearest` | 0 |
| `Linear` | 1 |

### asset.Font — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `source` | string | — |  |
| `mode` | FontMode | FontMode::SDF |  |
| `sizes` | uint32_t[] | — |  |
| `charset_preset` | CharsetPreset | CharsetPreset::Latin |  |
| `charset_ranges` | string[] | — |  |
| `charset_extra` | string[] | — |  |
| `sdf_spread` | float | 8.0F |  |
| `sdf_px_range` | float | 4.0F |  |
| `atlas_max_size` | uint32_t | 2048 |  |
| `atlas_padding` | uint32_t | 2 |  |
| `bold` | bool | false |  |
| `italic` | bool | false |  |
| `fallback` | string[] | — |  |

### asset.FontMode — enum

| 值 | 数值 |
|----|------|
| `Bitmap` | 0 |
| `SDF` | 1 |
| `MSDF` | 2 |

### asset.Key — struct (28B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `time` | float | 0.0F | unit: s |
| `value` | math.Vec4 | — |  |
| `in_tangent` | float | 0.0F |  |
| `out_tangent` | float | 0.0F |  |

### asset.Material — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `effect` | asset_ref<Effect> | — | ref: Effect |
| `params` | MaterialParam[] | — |  |

### asset.MaterialInstance — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `parent` | asset_ref<Material> | — | ref: Material |
| `overrides` | MaterialParam[] | — |  |

### asset.MaterialParam — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `type` | ParamType | ParamType::Float |  |
| `value` | string | — |  |

### asset.MaterialShadingModel — enum

| 值 | 数值 |
|----|------|
| `Standard` | 0 |
| `Physical` | 1 |
| `Basic` | 2 |
| `Lambert` | 3 |
| `Phong` | 4 |
| `Toon` | 5 |
| `Normal` | 6 |
| `Matcap` | 7 |
| `Depth` | 8 |
| `Shadow` | 9 |

### asset.MaterialSide — enum

| 值 | 数值 |
|----|------|
| `Front` | 0 |
| `Back` | 1 |
| `Double` | 2 |

### asset.Mesh — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `vertices` | math.Vec3[] | — |  |
| `indices` | uint32_t[] | — |  |
| `normals` | math.Vec3[] | — |  |
| `uvs` | math.Vec2[] | — |  |
| `colors` | math.Vec4[] | — |  |
| `tangents` | math.Vec4[] | — |  |
| `joints` | uint16_t[] | — |  |
| `weights` | math.Vec4[] | — |  |
| `topology` | Topology | Topology::TriangleList |  |
| `submeshes` | SubMesh[] | — |  |
| `bounds` | math.Aabb | — |  |

### asset.NavMeshData — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `vertices` | math.Vec3[] | — |  |
| `indices` | uint32_t[] | — |  |
| `cell_size` | float | 0.3F | unit: m |

### asset.ParamType — enum

| 值 | 数值 |
|----|------|
| `Float` | 0 |
| `Int` | 1 |
| `UInt` | 2 |
| `Vec2` | 3 |
| `Vec3` | 4 |
| `Vec4` | 5 |
| `Color` | 6 |
| `Mat3` | 7 |
| `Mat4` | 8 |
| `Texture` | 9 |
| `Sampler` | 10 |

### asset.PbrAlphaMode — enum

| 值 | 数值 |
|----|------|
| `Opaque` | 0 |
| `Mask` | 1 |
| `Blend` | 2 |

### asset.PbrMaterial — struct (变长（含动态成员）)

role: `pbr_material`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string |  |  |
| `base_color` | math.Color | {1.0F, 1.0F, 1.0F, 1.0F} | unit: linear |
| `metallic` | float | 0.0F | range: 0,1 |
| `roughness` | float | 0.5F | range: 0.02,1 |
| `emissive` | math.Vec3 | {0.0F, 0.0F, 0.0F} |  |
| `normal_scale` | float | 1.0F |  |
| `occlusion_strength` | float | 1.0F | range: 0,1 |
| `alpha_mode` | PbrAlphaMode | PbrAlphaMode::Opaque |  |
| `alpha_cutoff` | float | 0.5F | range: 0,1 |
| `double_sided` | bool | false |  |
| `clearcoat` | float | 0.0F | range: 0,1 |
| `clearcoat_roughness` | float | 0.03F | range: 0,1 |
| `base_color_texture` | string |  |  |
| `metallic_roughness_texture` | string |  |  |
| `normal_texture` | string |  |  |
| `occlusion_texture` | string |  |  |
| `emissive_texture` | string |  |  |
| `shading_model` | MaterialShadingModel | MaterialShadingModel::Standard |  |
| `opacity` | float | 1.0F | range: 0,1 |
| `side` | MaterialSide | MaterialSide::Double |  |
| `flat_shading` | bool | false |  |
| `emissive_intensity` | float | 1.0F |  |
| `env_intensity` | float | 1.0F |  |
| `normal_scale_y` | float | 1.0F |  |
| `specular` | math.Vec3 | {1.0F, 1.0F, 1.0F} |  |
| `shininess` | float | 30.0F |  |
| `specular_intensity` | float | 1.0F |  |
| `ior` | float | 1.5F |  |
| `sheen` | float | 0.0F | range: 0,1 |
| `sheen_roughness` | float | 0.5F |  |
| `sheen_color` | math.Vec3 | {0.0F, 0.0F, 0.0F} |  |
| `iridescence` | float | 0.0F | range: 0,1 |
| `iridescence_ior` | float | 1.3F |  |
| `iridescence_thickness_min` | float | 100.0F | unit: nm |
| `iridescence_thickness_max` | float | 400.0F | unit: nm |
| `anisotropy` | float | 0.0F | range: 0,1 |
| `anisotropy_rotation` | float | 0.0F |  |
| `clearcoat_normal_scale` | float | 1.0F |  |
| `toon_steps` | int32_t | 3 |  |
| `depth_packing` | DepthPacking | DepthPacking::Basic |  |
| `matcap_texture` | string |  |  |
| `gradient_texture` | string |  |  |
| `clearcoat_normal_texture` | string |  |  |
| `uv_scale` | math.Vec2 | {1.0F, 1.0F} |  |
| `uv_offset` | math.Vec2 | {0.0F, 0.0F} |  |
| `normal_uv_scale` | math.Vec2 | {1.0F, 1.0F} |  |
| `normal_uv_offset` | math.Vec2 | {0.0F, 0.0F} |  |
| `uv1_mask` | int32_t | 0 |  |
| `clearcoat_texture` | string |  |  |
| `clearcoat_roughness_texture` | string |  |  |
| `sheen_color_texture` | string |  |  |
| `sheen_roughness_texture` | string |  |  |
| `iridescence_texture` | string |  |  |
| `iridescence_thickness_texture` | string |  |  |
| `anisotropy_texture` | string |  |  |
| `uv_rotation` | float | 0.0F |  |
| `normal_uv_rotation` | float | 0.0F |  |
| `alpha_mask_in_blend` | bool | false |  |

### asset.PixelFormat — enum

| 值 | 数值 |
|----|------|
| `RGBA8` | 0 |
| `RGBA8_SRGB` | 1 |
| `RGBA16F` | 2 |
| `R8` | 3 |
| `BC7` | 4 |
| `ETC2_RGBA` | 5 |
| `ASTC_4x4` | 6 |

### asset.Prefab — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `root` | entity_ref | — | ref: scene.Node |

### asset.RenderTarget — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `width` | uint32_t | 0 |  |
| `height` | uint32_t | 0 |  |

### asset.Scene — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `root` | entity_ref | — | ref: scene.Node |

### asset.ScriptAsset — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `language` | script.ScriptLanguage | script::ScriptLanguage::JS |  |
| `source` | string | — |  |

### asset.SkeletonData — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `bones` | BoneB[] | — |  |
| `skinning` | anim.SkinningMode | anim::SkinningMode::Linear |  |
| `constraints` | string[] | — |  |

### asset.SpineAtlas — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `textures` | asset_ref<Texture>[] | — | ref: Texture |
| `regions` | string[] | — |  |

### asset.SpineData — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `skeleton` | string | — |  |
| `atlas` | asset_ref<SpineAtlas> | — | ref: SpineAtlas |
| `textures` | asset_ref<Texture>[] | — | ref: Texture |
| `skins` | string[] | — |  |
| `animations` | string[] | — |  |

### asset.SpriteFrame — struct (20B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `region` | math.Rect | {0.0F, 0.0F, 1.0F, 1.0F} |  |
| `duration` | float | 0.1F | unit: s, range: 0,100 |

### asset.SpriteFrames — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `frames` | SpriteFrame[] | — |  |

### asset.SubMesh — struct (12B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `first_index` | uint32_t | 0 |  |
| `index_count` | uint32_t | 0 |  |
| `material_slot` | uint16_t | 0 |  |

### asset.TerrainData — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `height_map` | asset_ref<Texture> | — | ref: Texture |
| `width` | uint32_t | 0 |  |
| `height` | uint32_t | 0 |  |
| `layer_count` | uint8_t | 1 |  |

### asset.Texture — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `width` | uint32_t | 0 |  |
| `height` | uint32_t | 0 |  |
| `file_format` | FileFormat | FileFormat::PNG |  |
| `data` | uint8_t[] | — |  |
| `pixel_format` | PixelFormat | PixelFormat::RGBA8 |  |
| `mip_levels` | uint8_t | 1 |  |
| `srgb` | bool | true |  |
| `filter` | FilterMode | FilterMode::Linear |  |
| `wrap_u` | WrapMode | WrapMode::Repeat |  |
| `wrap_v` | WrapMode | WrapMode::Repeat |  |

### asset.TileSet — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `name` | string | — |  |
| `texture` | asset_ref<Texture> | — | ref: Texture |
| `tile_size` | math.Vec2 | {16.0F, 16.0F} |  |
| `tile_count` | uint32_t | 0 |  |

### asset.Topology — enum

| 值 | 数值 |
|----|------|
| `TriangleList` | 0 |
| `LineList` | 1 |
| `PointList` | 2 |

### asset.Track — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `kind` | TrackKind | TrackKind::Property |  |
| `field_path` | string |  |  |
| `bone_index` | uint16_t | 0 |  |
| `value_type` | ValueType | ValueType::Float |  |
| `interpolation` | anim.InterpMode | anim.InterpMode::Inherit |  |
| `key_count` | uint16_t | 0 |  |
| `keys` | Key[] | — |  |

### asset.TrackKind — enum

| 值 | 数值 |
|----|------|
| `Property` | 0 |
| `Bone` | 1 |

### asset.ValueType — enum

| 值 | 数值 |
|----|------|
| `Float` | 0 |
| `Vec2` | 1 |
| `Vec3` | 2 |
| `Quat` | 3 |
| `Color` | 4 |
| `Bool` | 5 |

### asset.WrapMode — enum

| 值 | 数值 |
|----|------|
| `Repeat` | 0 |
| `ClampToEdge` | 1 |
| `MirroredRepeat` | 2 |

## audio（5） · `world/audio.yaml`

### audio.AttenuationModel — enum

| 值 | 数值 |
|----|------|
| `InverseDistance` | 0 |
| `Linear` | 1 |
| `Exponential` | 2 |
| `CustomCurve` | 3 |

### audio.AudioListener — struct (24B)

role: `audio_listener`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `active` | bool | true |  |
| `master_volume` | float | 1.0F | range: 0,2, unit: linear |
| `doppler_scale` | float | 1.0F | range: 0,2 |
| `listener` | uint64_t | 0 | transient |

### audio.AudioPlayState — enum

| 值 | 数值 |
|----|------|
| `Stopped` | 0 |
| `Playing` | 1 |
| `Paused` | 2 |

### audio.AudioSource — struct (变长（含动态成员）)

role: `audio_source`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `clip` | string | — |  |
| `bus` | string | — |  |
| `autoplay` | bool | false |  |
| `loop` | bool | false |  |
| `volume` | float | 1.0F | range: 0,4, unit: linear |
| `pitch` | float | 1.0F | range: 0.01,4 |
| `panning` | PanningMode | PanningMode::Stereo2D |  |
| `attenuation` | AttenuationModel | AttenuationModel::InverseDistance |  |
| `curve` | string | — |  |
| `min_distance` | float | 1.0F | unit: m |
| `max_distance` | float | 50.0F | unit: m |
| `doppler` | float | 0.0F | range: 0,2 |
| `state` | AudioPlayState | AudioPlayState::Stopped | transient |
| `playhead` | float | 0.0F | transient, unit: s |
| `voice` | uint64_t | 0 | transient |

### audio.PanningMode — enum

| 值 | 数值 |
|----|------|
| `Stereo2D` | 0 |
| `Spatial3D` | 1 |
| `Spatial3D_HRTF` | 2 |

## input（3） · `input/input.yaml`

### input.CursorShape — enum

| 值 | 数值 |
|----|------|
| `Arrow` | 0 |
| `Hand` | 1 |
| `Text` | 2 |
| `Crosshair` | 3 |
| `Move` | 4 |
| `ResizeH` | 5 |
| `ResizeV` | 6 |
| `NotAllowed` | 7 |

### input.InputTarget — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `pickable` | bool | true |  |
| `pick_layer` | uint32_t | 1 |  |
| `drag_enabled` | bool | false |  |
| `hover_cursor` | CursorShape | CursorShape::Arrow |  |

### input.PointerType — enum

| 值 | 数值 |
|----|------|
| `Mouse` | 0 |
| `Touch` | 1 |
| `Pen` | 2 |

## math（14） · `math.yaml`

### math.Aabb — struct (24B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `min` | Vec3 | {0.0F, 0.0F, 0.0F} |  |
| `max` | Vec3 | {0.0F, 0.0F, 0.0F} |  |

### math.Color — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `r` | float | 1.0F |  |
| `g` | float | 1.0F |  |
| `b` | float | 1.0F |  |
| `a` | float | 1.0F |  |

### math.Mat3 — struct (36B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `m` | float[9] | {0.0F} |  |

### math.Mat4 — struct (64B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `m` | float[16] | {0.0F} |  |

### math.Plane — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `normal` | Vec3 | {0.0F, 1.0F, 0.0F} |  |
| `distance` | float | 0.0F |  |

### math.Quat — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `z` | float | 0.0F |  |
| `w` | float | 1.0F |  |

### math.Ray — struct (24B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `origin` | Vec3 | {0.0F, 0.0F, 0.0F} |  |
| `direction` | Vec3 | {0.0F, 0.0F, -1.0F} |  |

### math.Rect — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `width` | float | 0.0F |  |
| `height` | float | 0.0F |  |

### math.Rgba — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `r` | float | 0.0F |  |
| `g` | float | 0.0F |  |
| `b` | float | 0.0F |  |
| `a` | float | 1.0F |  |

### math.Transform2D — struct (20B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `position` | Vec2 | {0.0F, 0.0F} |  |
| `rotation` | float | 0.0F | unit: radian |
| `scale` | Vec2 | {1.0F, 1.0F} |  |

### math.Transform3D — struct (36B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `position` | Vec3 | {0.0F, 0.0F, 0.0F} |  |
| `rotation` | Vec3 | {0.0F, 0.0F, 0.0F} | unit: radian, tooltip: Euler XYZ |
| `scale` | Vec3 | {1.0F, 1.0F, 1.0F} |  |

### math.Vec2 — struct (8B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |

### math.Vec3 — struct (12B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `z` | float | 0.0F |  |

### math.Vec4 — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `z` | float | 0.0F |  |
| `w` | float | 0.0F |  |

## nav（6） · `world/nav.yaml`

### nav.NavAgent — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `navmesh` | asset_ref<NavMeshData> | — | ref: NavMeshData |
| `target_entity` | entity_ref | — | ref: scene.Node |
| `target_position` | math.Vec3 | — | unit: m |
| `speed` | float | 3.5F | range: 0,100, unit: m/s |
| `acceleration` | float | 8.0F | unit: m/s2 |
| `radius` | float | 0.5F | unit: m |
| `height` | float | 1.8F | unit: m |
| `stopping_distance` | float | 0.5F | unit: m |
| `max_slope` | float | 45.0F | unit: deg |
| `step_height` | float | 0.4F | unit: m |
| `avoidance` | NavAvoidance | NavAvoidance::None |  |
| `state` | NavAgentState | NavAgentState::Idle | readonly |
| `path` | math.Vec3[] | — | transient |
| `path_index` | uint16_t | 0 | transient |
| `desired_velocity` | math.Vec3 | — | transient |
| `query` | uint64_t | 0 | transient |

### nav.NavAgentState — enum

| 值 | 数值 |
|----|------|
| `Idle` | 0 |
| `Moving` | 1 |
| `PathPending` | 2 |
| `Arrived` | 3 |
| `OffMesh` | 4 |
| `Failed` | 5 |

### nav.NavAvoidance — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `RVO2` | 1 |

### nav.NavMesh — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `source` | NavMeshSource | NavMeshSource::Geometry |  |
| `geometry` | asset_ref<Mesh> | — | ref: Mesh |
| `data` | asset_ref<NavMeshData> | — | ref: NavMeshData |
| `cell_size` | float | 0.3F | unit: m |
| `cell_height` | float | 0.2F | unit: m |
| `agent_radius` | float | 0.5F | unit: m |
| `agent_height` | float | 1.8F | unit: m |
| `max_slope` | float | 45.0F | unit: deg |
| `max_edge_length` | float | 12.0F | unit: m |
| `region_min_size` | float | 8.0F | unit: m2 |
| `bake_state` | NavMeshBakeState | NavMeshBakeState::Baked | readonly |
| `accel` | uint64_t | 0 | transient |

### nav.NavMeshBakeState — enum

| 值 | 数值 |
|----|------|
| `Dirty` | 0 |
| `Baking` | 1 |
| `Baked` | 2 |
| `Failed` | 3 |

### nav.NavMeshSource — enum

| 值 | 数值 |
|----|------|
| `Geometry` | 0 |
| `Colliders` | 1 |
| `Explicit` | 2 |

## physics（15） · `world/physics.yaml`

### physics.Area — struct (56B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `monitoring` | bool | true |  |
| `monitorable` | bool | true |  |
| `priority` | int32_t | 0 |  |
| `gravity_space_override` | SpaceOverride | SpaceOverride::Disabled |  |
| `gravity_direction` | math.Vec3 | {0.0F, -1.0F, 0.0F} |  |
| `gravity` | float | 9.81F |  |
| `linear_damping_override` | float | 0.0F |  |
| `angular_damping_override` | float | 0.0F |  |
| `collision_layer` | uint32_t | 1 |  |
| `collision_mask` | uint32_t | 4294967295 |  |
| `overlap_count` | uint32_t | 0 | transient, readonly |
| `area_handle` | uint64_t | 0 | transient |

### physics.BodyType — enum

| 值 | 数值 |
|----|------|
| `Static` | 0 |
| `Kinematic` | 1 |
| `Dynamic` | 2 |

### physics.CharacterBody — struct (88B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `desired_velocity` | math.Vec3 | — |  |
| `up_direction` | math.Vec3 | {0.0F, 1.0F, 0.0F} |  |
| `floor_max_angle` | float | 45.0F | unit: deg, range: 0,90 |
| `step_height` | float | 0.25F |  |
| `snap_length` | float | 0.0F |  |
| `max_slides` | uint32_t | 4 | range: 0,16 |
| `auto_apply_gravity` | bool | true |  |
| `gravity_scale` | float | 1.0F |  |
| `velocity` | math.Vec3 | — | transient, readonly |
| `on_floor` | bool | false | transient, readonly |
| `on_wall` | bool | false | transient, readonly |
| `on_ceiling` | bool | false | transient, readonly |
| `floor_normal` | math.Vec3 | — | transient, readonly |
| `controller_handle` | uint64_t | 0 | transient |

### physics.Collider — struct (变长（含动态成员）)

role: `physics_collider`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `shape_type` | ShapeType | ShapeType::Box |  |
| `half_extents` | math.Vec3 | {0.5F, 0.5F, 0.5F} |  |
| `radius` | float | 0.5F |  |
| `half_height` | float | 0.5F |  |
| `convex_mesh` | asset_ref<Mesh> | — | ref: Mesh |
| `trimesh` | asset_ref<Mesh> | — | ref: Mesh |
| `height_scale` | math.Vec2 | {1.0F, 1.0F} |  |
| `density` | float | 1.0F | unit: kg/m3 |
| `friction` | float | 0.5F |  |
| `restitution` | float | 0.0F |  |
| `friction_combine` | CombineRule | CombineRule::Average |  |
| `restitution_combine` | CombineRule | CombineRule::Average |  |
| `is_sensor` | bool | false |  |
| `collision_layer` | uint32_t | 1 |  |
| `collision_mask` | uint32_t | 4294967295 |  |
| `offset_position` | math.Vec3 | — |  |
| `offset_rotation` | math.Quat | — |  |
| `collider_handle` | uint64_t | 0 | transient |
| `owner_body` | uint64_t | 0 | transient |

### physics.CombineRule — enum

| 值 | 数值 |
|----|------|
| `Average` | 0 |
| `Min` | 1 |
| `Multiply` | 2 |
| `Max` | 3 |

### physics.ConstantForce — struct (36B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `force` | math.Vec3 | — | unit: N |
| `torque` | math.Vec3 | — | unit: N.m |
| `space` | ForceSpace | ForceSpace::World |  |
| `apply_mode` | ForceApplyMode | ForceApplyMode::Continuous |  |
| `enabled` | bool | true |  |
| `only_when_awake` | bool | true |  |
| `impulse_consumed` | bool | false | transient, readonly |

### physics.DampingOverride — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `linear_damping` | float | 0.0F | unit: 1/s |
| `angular_damping` | float | 0.0F | unit: 1/s |
| `enabled` | bool | true |  |
| `priority` | int32_t | 0 |  |

### physics.ForceApplyMode — enum

| 值 | 数值 |
|----|------|
| `Continuous` | 0 |
| `Impulse` | 1 |

### physics.ForceSpace — enum

| 值 | 数值 |
|----|------|
| `World` | 0 |
| `Local` | 1 |

### physics.Joint — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `joint_type` | JointType | JointType::Fixed |  |
| `body_a` | entity_ref | — | ref: RigidBody |
| `body_b` | entity_ref | — | ref: RigidBody |
| `anchor_a` | math.Vec3 | — |  |
| `anchor_b` | math.Vec3 | — |  |
| `axis` | math.Vec3 | {0.0F, 1.0F, 0.0F} |  |
| `limits_enabled` | bool | false |  |
| `limit_min` | float | 0.0F |  |
| `limit_max` | float | 0.0F |  |
| `motor_enabled` | bool | false |  |
| `motor_target_vel` | float | 0.0F |  |
| `motor_max_force` | float | 0.0F |  |
| `break_force` | float | 0.0F |  |
| `break_torque` | float | 0.0F |  |
| `joint_handle` | uint64_t | 0 | transient |

### physics.JointType — enum

| 值 | 数值 |
|----|------|
| `Fixed` | 0 |
| `Revolute` | 1 |
| `Prismatic` | 2 |
| `Ball` | 3 |
| `Rope` | 4 |
| `Spring` | 5 |
| `Generic` | 6 |

### physics.LinearVelocityLimit — struct (20B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `max_linear_speed` | float | 0.0F | unit: m/s |
| `max_angular_speed` | float | 0.0F | unit: rad/s |
| `enabled` | bool | true |  |
| `linear_speed` | float | 0.0F | transient, readonly |
| `angular_speed` | float | 0.0F | transient, readonly |

### physics.RigidBody — struct (72B)

role: `physics_body`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `body_type` | BodyType | BodyType::Static |  |
| `mass` | float | 1.0F | unit: kg |
| `linear_damping` | float | 0.0F | unit: 1/s |
| `angular_damping` | float | 0.0F | unit: 1/s |
| `gravity_scale` | float | 1.0F |  |
| `ccd_enabled` | bool | false |  |
| `can_sleep` | bool | true |  |
| `locked_translation` | uint8_t | 0 | range: 0,7 |
| `locked_rotation` | uint8_t | 0 | range: 0,7 |
| `linear_velocity` | math.Vec3 | — | transient, readonly |
| `angular_velocity` | math.Vec3 | — | transient, readonly |
| `sleeping` | bool | false | transient, readonly |
| `body_handle` | uint64_t | 0 | transient |
| `world_slot` | uint32_t | 0 | transient |

### physics.ShapeType — enum

| 值 | 数值 |
|----|------|
| `Box` | 0 |
| `Circle` | 1 |
| `Capsule` | 2 |
| `Cylinder` | 3 |
| `Convex` | 4 |
| `TriMesh` | 5 |
| `HeightField` | 6 |

### physics.SpaceOverride — enum

| 值 | 数值 |
|----|------|
| `Disabled` | 0 |
| `Combine` | 1 |
| `Replace` | 2 |

## render（45） · `render/render.yaml`

### render.Billboard — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `facing` | BillboardFacing | BillboardFacing::YAxisLocked |  |
| `pivot` | BillboardPivot | BillboardPivot::Center |  |
| `lock_scale` | bool | false |  |
| `align_up` | math.Vec3 | {0.0F, 1.0F, 0.0F} |  |
| `target_camera` | entity_ref | — | ref: Camera3D, weak |
| `enabled` | bool | true |  |

### render.BillboardFacing — enum

| 值 | 数值 |
|----|------|
| `FullScreen` | 0 |
| `YAxisLocked` | 1 |
| `ViewPlane` | 2 |
| `LookAtCamera` | 3 |

### render.BillboardPivot — enum

| 值 | 数值 |
|----|------|
| `Center` | 0 |
| `Bottom` | 1 |

### render.BlendMode — enum

| 值 | 数值 |
|----|------|
| `Alpha` | 0 |
| `Additive` | 1 |
| `Multiply` | 2 |

### render.Camera2D — struct (60B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `active` | bool | true |  |
| `priority` | int32_t | 0 |  |
| `zoom` | math.Vec2 | {1.0F, 1.0F} |  |
| `offset` | math.Vec2 | — |  |
| `viewport` | math.Rect | {0.0F, 0.0F, 1.0F, 1.0F} | unit: normalized |
| `layer_mask` | uint32_t | 4294967295 | bitmask |
| `clear_color` | math.Color | — |  |

### render.Camera3D — struct (72B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `active` | bool | true |  |
| `priority` | int32_t | 0 |  |
| `projection` | CameraProjection | CameraProjection::Perspective |  |
| `fov_y` | float | 60.0F | unit: deg |
| `ortho_height` | float | 2.0F |  |
| `near` | float | 0.1F |  |
| `far` | float | 1000.0F |  |
| `viewport` | math.Rect | {0.0F, 0.0F, 1.0F, 1.0F} | unit: normalized |
| `culling_mask` | uint32_t | 4294967295 | bitmask |
| `clear_mode` | ClearMode | ClearMode::Skybox |  |
| `clear_color` | math.Color | — |  |
| `exposure` | float | 1.0F |  |

### render.CameraProjection — enum

| 值 | 数值 |
|----|------|
| `Perspective` | 0 |
| `Orthographic` | 1 |

### render.ClearMode — enum

| 值 | 数值 |
|----|------|
| `Skybox` | 0 |
| `Color` | 1 |
| `DepthOnly` | 2 |
| `None` | 3 |

### render.DrawCommand — struct (52B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `op` | GraphicsOp | GraphicsOp::Line |  |
| `first` | uint32_t | 0 |  |
| `count` | uint32_t | 0 |  |
| `flags` | uint32_t | 0 |  |
| `color` | math.Color | — |  |
| `param` | math.Vec4 | — |  |
| `width` | float | 0.0F |  |

### render.EnvironmentSource — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Color` | 1 |
| `Gradient` | 2 |
| `Texture` | 3 |

### render.FillRule — enum

| 值 | 数值 |
|----|------|
| `NonZero` | 0 |
| `EvenOdd` | 1 |

### render.FlipMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Horizontal` | 1 |
| `Vertical` | 2 |
| `Both` | 3 |

### render.Graphics — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `commands` | DrawCommand[] | — |  |
| `points` | math.Vec2[] | — |  |
| `point_colors` | math.Color[] | — |  |
| `fill_color` | math.Color | — |  |
| `line_color` | math.Color | — |  |
| `width` | float | 1.0F | unit: px |
| `line_join` | LineJoin | LineJoin::Miter |  |
| `line_cap` | LineCap | LineCap::Butt |  |
| `fill_rule` | FillRule | FillRule::NonZero |  |
| `miter_limit` | float | 4.0F | range: 1,16 |
| `curve_tolerance` | float | 0.25F | unit: px |
| `antialiased` | bool | true |  |
| `material` | asset_ref<Material> | — | ref: Material |
| `z_index` | int32_t | 0 |  |
| `cache_geometry` | bool | true |  |
| `geometry` | math.Vec2[] | — | transient |
| `indices` | uint32_t[] | — | transient |
| `dirty` | bool | true | transient |

### render.GraphicsFlags — enum

| 值 | 数值 |
|----|------|
| `Fill` | 1 |
| `Stroke` | 2 |
| `Closed` | 4 |

### render.GraphicsOp — enum

| 值 | 数值 |
|----|------|
| `Line` | 0 |
| `Polyline` | 1 |
| `Polygon` | 2 |
| `Rect` | 3 |
| `Circle` | 4 |
| `Ellipse` | 5 |
| `Arc` | 6 |
| `QuadraticBezier` | 7 |
| `CubicBezier` | 8 |
| `ConvexPoly` | 9 |

### render.ImageAlign — enum

| 值 | 数值 |
|----|------|
| `Baseline` | 0 |
| `Center` | 1 |
| `Top` | 2 |
| `Bottom` | 3 |

### render.LayerKind — enum

| 值 | 数值 |
|----|------|
| `Canvas2D` | 0 |
| `World3D` | 1 |
| `UI` | 2 |

### render.Light — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `type` | LightType | LightType::Directional |  |
| `color` | math.Color | — |  |
| `intensity` | float | 1.0F |  |
| `range` | float | 10.0F | unit: m |
| `inner_angle` | float | 20.0F | unit: deg |
| `outer_angle` | float | 30.0F | unit: deg |
| `shadow_mode` | ShadowMode | ShadowMode::Off |  |
| `shadow_bias` | float | 0.001F |  |
| `shadow_distance` | float | 50.0F | unit: m |
| `culling_mask` | uint32_t | 4294967295 | bitmask |
| `priority` | int32_t | 0 |  |
| `layer` | entity_ref | — | ref: RenderLayer, weak |

### render.LightType — enum

| 值 | 数值 |
|----|------|
| `Directional` | 0 |
| `Point` | 1 |
| `Spot` | 2 |

### render.LineCap — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Butt` | 1 |
| `Round` | 2 |
| `Square` | 3 |

### render.LineJoin — enum

| 值 | 数值 |
|----|------|
| `Miter` | 0 |
| `Bevel` | 1 |
| `Round` | 2 |

### render.MeshRenderer — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `mesh` | asset_ref<Mesh> | — | ref: Mesh |
| `mesh_path` | string |  |  |
| `material` | asset_ref<Material> | — | ref: Material |
| `material_overrides` | asset_ref<Material>[] | — | ref: Material |
| `cast_shadow` | ShadowCastMode | ShadowCastMode::On |  |
| `receive_shadow` | bool | true |  |
| `visible` | bool | true |  |
| `culling_mask` | uint32_t | 4294967295 | bitmask |
| `layer` | entity_ref | — | ref: RenderLayer, weak |

### render.MotionStreak — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `mode` | StreakMode | StreakMode::Ribbon |  |
| `width` | float | 16.0F | unit: px |
| `width_curve` | float | 1.0F | range: 0,4 |
| `color_start` | math.Color | — |  |
| `color_end` | math.Color | {1.0F, 1.0F, 1.0F, 0.0F} |  |
| `fade_mode` | StreakFade | StreakFade::Both |  |
| `lifetime` | float | 0.5F | unit: s |
| `min_distance` | float | 2.0F | unit: px |
| `max_points` | uint32_t | 64 | range: 2,1024 |
| `points_per_sec` | float | 0.0F |  |
| `point_size` | float | 4.0F | unit: px |
| `texture` | asset_ref<Texture> | — | ref: Texture |
| `material` | asset_ref<Material> | — | ref: Material |
| `z_index` | int32_t | 0 |  |
| `emit` | bool | true |  |
| `samples` | StreakSample[] | — | transient |
| `geometry` | math.Vec2[] | — | transient |
| `indices` | uint32_t[] | — | transient |

### render.PivotAnchor — enum

| 值 | 数值 |
|----|------|
| `TopLeft` | 0 |
| `Center` | 1 |
| `BottomCenter` | 2 |
| `Custom` | 3 |

### render.ProbeShape — enum

| 值 | 数值 |
|----|------|
| `Sphere` | 0 |
| `Box` | 1 |

### render.ProbeSource — enum

| 值 | 数值 |
|----|------|
| `Baked` | 0 |
| `Realtime` | 1 |

### render.ProbeUpdateMode — enum

| 值 | 数值 |
|----|------|
| `OnLoad` | 0 |
| `EveryFrame` | 1 |
| `Interval` | 2 |

### render.ReflectionProbe — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `source` | ProbeSource | ProbeSource::Baked |  |
| `update_mode` | ProbeUpdateMode | ProbeUpdateMode::OnLoad |  |
| `interval` | float | 1.0F | unit: s |
| `resolution` | uint16_t | 128 | range: 32,1024 |
| `shape` | ProbeShape | ProbeShape::Sphere |  |
| `radius` | float | 10.0F | unit: m |
| `box_size` | math.Vec3 | {10.0F, 10.0F, 10.0F} | unit: m |
| `blend_distance` | float | 1.0F | unit: m |
| `intensity` | float | 1.0F |  |
| `priority` | int32_t | 0 |  |
| `culling_mask` | uint32_t | 4294967295 | bitmask |
| `layer` | entity_ref | — | ref: RenderLayer, weak |
| `baked_cubemap` | asset_ref<Texture> | — | ref: Texture, transient |

### render.RenderLayer — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `kind` | LayerKind | LayerKind::World3D |  |
| `order` | int32_t | 0 |  |
| `visible` | bool | true |  |
| `camera` | entity_ref | — | ref: Camera2D, weak |
| `viewport` | math.Rect | {0.0F, 0.0F, 1.0F, 1.0F} | unit: normalized |
| `clear` | bool | true |  |
| `clear_color` | math.Color | — |  |
| `target` | asset_ref<RenderTarget> | — | ref: RenderTarget |
| `follow_viewport` | bool | true |  |
| `offset` | math.Vec2 | — | unit: px |
| `blend` | BlendMode | BlendMode::Alpha |  |

### render.RichRun — struct (36B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `tag` | RichTagType | RichTagType::Text |  |
| `text_begin` | uint32_t | 0 |  |
| `text_len` | uint32_t | 0 |  |
| `color` | math.Color | — |  |
| `font_size` | float | 16.0F |  |
| `link` | uint32_t | 4294967295 |  |

### render.RichTagType — enum

| 值 | 数值 |
|----|------|
| `Text` | 0 |
| `Color` | 1 |
| `Bold` | 2 |
| `Italic` | 3 |
| `Underline` | 4 |
| `Strikethrough` | 5 |
| `Font` | 6 |
| `Size` | 7 |
| `Outline` | 8 |
| `Image` | 9 |
| `Link` | 10 |

### render.RichText — struct (变长（含动态成员）)

role: `ui_text`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `text` | string | — |  |
| `parse_tags` | bool | true |  |
| `max_width` | float | 0.0F | unit: px, transient: false |
| `font` | asset_ref<Font> | — | ref: Font |
| `font_size` | float | 16.0F | unit: px |
| `color` | math.Color | {1.0F, 1.0F, 1.0F, 1.0F} |  |
| `outline_size` | float | 0.0F | unit: px |
| `outline_color` | math.Color | {0.0F, 0.0F, 0.0F, 1.0F} |  |
| `align_h` | ui.Align | ui.Align::Begin |  |
| `align_v` | ui.Align | ui.Align::Begin |  |
| `wrap_mode` | ui.TextWrap | ui.TextWrap::Word |  |
| `line_spacing` | float | 0.0F | unit: px |
| `image_align` | ImageAlign | ImageAlign::Baseline |  |
| `meta_underlined` | bool | true |  |
| `content_size` | math.Vec2 | — | transient |
| `runs` | RichRun[] | — | transient |
| `links` | string[] | — | transient |
| `line_count` | int32_t | 0 | transient |

### render.ShadowCastMode — enum

| 值 | 数值 |
|----|------|
| `Off` | 0 |
| `On` | 1 |
| `DoubleSided` | 2 |

### render.ShadowMode — enum

| 值 | 数值 |
|----|------|
| `Off` | 0 |
| `Hard` | 1 |
| `Soft` | 2 |

### render.Skybox — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `skybox_mode` | SkyboxMode | SkyboxMode::Color |  |
| `skybox_texture` | asset_ref<Texture> | — | ref: Texture |
| `skybox_color` | math.Color | {0.05F, 0.05F, 0.08F, 1.0F} |  |
| `skybox_rotation` | float | 0.0F | unit: deg, range: 0,360 |
| `skybox_intensity` | float | 1.0F |  |
| `env_source` | EnvironmentSource | EnvironmentSource::Color |  |
| `env_texture` | asset_ref<Texture> | — | ref: Texture |
| `env_color` | math.Color | {0.2F, 0.2F, 0.22F, 1.0F} |  |
| `env_sky_color` | math.Color | {0.3F, 0.4F, 0.6F, 1.0F} |  |
| `env_ground_color` | math.Color | {0.2F, 0.18F, 0.15F, 1.0F} |  |
| `env_intensity` | float | 1.0F |  |
| `diffuse_contribution` | float | 1.0F | range: 0,1 |
| `specular_contribution` | float | 1.0F | range: 0,1 |
| `priority` | int32_t | 0 |  |
| `active` | bool | true |  |

### render.SkyboxMode — enum

| 值 | 数值 |
|----|------|
| `Procedural` | 0 |
| `Cubemap` | 1 |
| `Equirect` | 2 |
| `Color` | 3 |

### render.Sprite — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `texture` | asset_ref<Texture> | — | ref: Texture |
| `region` | math.Rect | {0.0F, 0.0F, 1.0F, 1.0F} |  |
| `hframes` | uint16_t | 1 | range: 1,1024 |
| `vframes` | uint16_t | 1 | range: 1,1024 |
| `frame` | uint16_t | 0 | range: 0,65535 |
| `flip` | FlipMode | FlipMode::None |  |
| `color` | math.Color | — |  |
| `pivot` | math.Vec2 | {0.5F, 0.5F} | unit: normalized |
| `pivot_anchor` | PivotAnchor | PivotAnchor::Center |  |
| `material` | asset_ref<Material> | — | ref: Material |
| `z_index` | int32_t | 0 |  |

### render.StreakFade — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Head` | 1 |
| `Tail` | 2 |
| `Both` | 3 |

### render.StreakMode — enum

| 值 | 数值 |
|----|------|
| `Ribbon` | 0 |
| `Line` | 1 |
| `Points` | 2 |

### render.StreakSample — struct (16B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `pos` | math.Vec2 | — |  |
| `age` | float | 0.0F |  |
| `width` | float | 0.0F |  |

### render.Terrain — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `terrain_data` | asset_ref<TerrainData> | — | ref: TerrainData |
| `layer_material` | asset_ref<Material> | — | ref: Material |
| `tile_size` | float | 16.0F | unit: m |
| `lod_mode` | TerrainLodMode | TerrainLodMode::Auto |  |
| `lod_levels` | TerrainLevel[] | — |  |
| `fixed_lod` | uint8_t | 0 |  |
| `lod_bias` | float | 0.0F |  |
| `height_scale` | float | 1.0F |  |
| `cast_shadow` | bool | true |  |
| `receive_shadow` | bool | true |  |
| `visible` | bool | true |  |
| `culling_mask` | uint32_t | 4294967295 | bitmask |
| `layer` | entity_ref | — | ref: RenderLayer, weak |
| `lod_chunks` | uint16_t[] | — | transient |

### render.TerrainLevel — struct (12B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `level` | uint8_t | 0 |  |
| `max_distance` | float | 0.0F | unit: m |
| `resolution` | uint16_t | 0 |  |

### render.TerrainLodMode — enum

| 值 | 数值 |
|----|------|
| `Auto` | 0 |
| `Fixed` | 1 |

### render.TileCoord — struct (8B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | int32_t | 0 |  |
| `y` | int32_t | 0 |  |

### render.TileMap — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `tileset` | asset_ref<TileSet> | — | ref: TileSet |
| `cell_size` | math.Vec2 | {16.0F, 16.0F} | unit: px |
| `columns` | uint32_t | 0 | readonly |
| `rows` | uint32_t | 0 |  |
| `tile_ids` | uint32_t[] | — |  |
| `origin` | TileCoord | — |  |
| `modulate` | math.Color | — |  |
| `z_index` | int32_t | 0 |  |
| `collision_source` | asset_ref<TileSet> | — | ref: TileSet, deprecated |

## scene（9） · `world/scene.yaml`

### scene.Active — struct (2B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `enabled` | bool | true | tooltip: 本节点是否生效 |
| `active_in_hierarchy` | bool | true | transient, readonly |

### scene.Meta — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `stable_id` | uint64_t | 0 | readonly, tooltip: 稳定节点 ID |
| `entries` | map<string, string> | — | ref: interned_string |
| `comment` | char[256] | "" |  |
| `source_scene` | asset_ref<asset.Scene> | null | ref: asset.Scene |
| `source_prefab` | asset_ref<Prefab> | null | ref: Prefab |

### scene.Name — struct (68B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `value` | char[64] | "" | tooltip: 节点显示名（UTF-8，定长 64 字节） |
| `hash` | uint32_t | 0 | transient, readonly |

### scene.Node — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `parent` | entity_ref | — | ref: Node |
| `children` | entity_ref[] | — | ref: Node |

### scene.PrefabInstance — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `source` | asset_ref<Prefab> | null | ref: Prefab |
| `overrides` | PrefabOverride[] | — |  |
| `is_variant` | bool | false |  |
| `override_count` | uint32_t | 0 | transient, readonly |
| `dirty` | bool | false | transient, readonly |

### scene.PrefabOverride — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `node` | entity_ref | — |  |
| `source_stable_id` | uint64_t | 0 |  |
| `component_type_id` | uint32_t | 0 |  |
| `field_path` | char[128] | "" |  |

### scene.Tags — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `values` | string[] | — | ref: interned_string |
| `count` | uint32_t | 0 | transient, readonly |

### scene.Transform2D — struct (60B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `local` | math.Transform2D | — |  |
| `world` | math.Mat3 | — | transient |
| `world_dirty` | uint8_t | 1 | transient |

### scene.Transform3D — struct (120B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `local` | math.Transform3D | — |  |
| `world` | math.Mat4 | — | transient |
| `rotation_quat` | math.Quat | — | transient |
| `world_dirty` | uint8_t | 1 | transient |

## scn（11） · `world/scn.yaml`

### scn.AnimationPlayer — struct (变长（含动态成员）)

role: `anim_player`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `clip` | string |  |  |
| `time` | float | 0.0F |  |
| `speed` | float | 1.0F |  |
| `loop` | bool | true |  |
| `playing` | bool | true |  |
| `duration` | float | 0.0F |  |

### scn.Button — struct (变长（含动态成员）)

role: `ui_button`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `width` | float | 96.0F |  |
| `height` | float | 28.0F |  |
| `z` | int32_t | 0 |  |
| `label` | string | Button |  |
| `r` | float | 0.22F |  |
| `g` | float | 0.26F |  |
| `b` | float | 0.36F |  |
| `pressed` | bool | false |  |
| `hovered` | bool | false |  |
| `enabled` | bool | true |  |

### scn.Camera — struct (20B)

role: `camera`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `zoom` | float | 1.0F |  |
| `rotation` | float | 0.0F |  |
| `active` | bool | true |  |

### scn.Label — struct (变长（含动态成员）)

role: `ui_label`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |
| `z` | int32_t | 0 |  |
| `text` | string |  |  |
| `r` | float | 1.0F |  |
| `g` | float | 1.0F |  |
| `b` | float | 1.0F |  |
| `a` | float | 1.0F |  |
| `font_size` | float | 16.0F | unit: px, range: 1,512 |
| `align` | ui.Align | ui::Align::Begin |  |
| `wrap` | ui.TextWrap | ui::TextWrap::None |  |
| `max_width` | float | 0.0F | unit: px |
| `line_spacing` | float | 0.0F | unit: px |
| `outline_size` | float | 0.0F | unit: px |
| `outline_r` | float | 0.0F |  |
| `outline_g` | float | 0.0F |  |
| `outline_b` | float | 0.0F |  |
| `font` | string |  | ref: Font |
| `text_w` | float | 0.0F | transient |
| `text_h` | float | 0.0F | transient |
| `line_count` | int32_t | 0 | transient |

### scn.NavAgent — struct (24B)

role: `nav_agent`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `tx` | float | 0.0F |  |
| `ty` | float | 0.0F |  |
| `speed` | float | 60.0F |  |
| `arrive_dist` | float | 2.0F |  |
| `repath` | bool | true |  |
| `has_target` | bool | false |  |
| `state` | int32_t | 0 |  |

### scn.ParticleEmitter — struct (44B)

role: `particle_emitter`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `rate` | float | 20.0F |  |
| `lifetime` | float | 1.0F |  |
| `speed` | float | 60.0F |  |
| `spread` | float | 0.5F |  |
| `gravity` | float | 120.0F |  |
| `size` | float | 3.0F |  |
| `r` | float | 1.0F |  |
| `g` | float | 0.6F |  |
| `b` | float | 0.2F |  |
| `max_count` | uint32_t | 256 |  |
| `enabled` | bool | true |  |

### scn.PrefabInstance — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `source` | string |  |  |
| `detached` | bool | false |  |

### scn.Scene — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `title` | string | maque scene |  |
| `script` | string |  |  |
| `scripts` | string[] | — |  |
| `width` | int32_t | 320 |  |
| `height` | int32_t | 240 |  |
| `seed` | uint32_t | 0 |  |
| `views` | int32_t | 1 |  |
| `eye_sep` | float | 0.0F |  |
| `env_color` | math.Color | {0.27F, 0.28F, 0.31F, 1.0F} |  |
| `env_intensity` | float | 1.6F |  |
| `env_sky_color` | math.Color | {0.0F, 0.0F, 0.0F, 1.0F} |  |
| `env_ground_color` | math.Color | {0.0F, 0.0F, 0.0F, 1.0F} |  |
| `env_highlight` | float | 1.0F |  |
| `env_rotation` | float | 0.0F |  |
| `gravity_y` | float | -900.0F |  |
| `restitution` | float | 0.86F |  |

### scn.Sprite — struct (变长（含动态成员）)

role: `sprite`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `radius` | float | 8.0F |  |
| `width` | float | 0.0F | unit: px |
| `height` | float | 0.0F | unit: px |
| `r` | float | 0.4F |  |
| `g` | float | 0.8F |  |
| `b` | float | 1.0F |  |
| `texture` | string |  |  |
| `material` | string |  |  |

### scn.SpriteMaterial — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `base_color` | math.Vec4 | {1.0F,1.0F,1.0F,1.0F} |  |
| `texture` | string |  |  |

### scn.Transform2D — struct (8B)

role: `transform2d`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `x` | float | 0.0F |  |
| `y` | float | 0.0F |  |

## script（5） · `script.yaml`

### script.PropertyVariant — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `type` | PropertyVariantType | PropertyVariantType::Bool |  |
| `b` | bool | false |  |
| `i` | int64_t | 0 |  |
| `f` | double | 0 |  |
| `v` | math.Vec4 | — |  |
| `s` | string | — |  |

### script.PropertyVariantType — enum

| 值 | 数值 |
|----|------|
| `Bool` | 0 |
| `Int` | 1 |
| `Float` | 2 |
| `Vec2` | 3 |
| `Vec3` | 4 |
| `Color` | 5 |
| `String` | 6 |
| `EntityRef` | 7 |
| `AssetRef` | 8 |

### script.Script — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `source` | asset_ref<ScriptAsset> | — | ref: ScriptAsset |
| `language` | ScriptLanguage | ScriptLanguage::JS |  |
| `enabled` | bool | true |  |
| `callbacks` | uint32_t | 62 |  |
| `exposed` | map<string, PropertyVariant> | — |  |
| `vm` | uint64_t | 0 | transient |
| `expose_defs` | uint64_t | 0 | transient, readonly |
| `init_err` | uint16_t | 0 | transient, readonly |

### script.ScriptCallback — enum

| 值 | 数值 |
|----|------|
| `OnEnterTree` | 1 |
| `Ready` | 2 |
| `Process` | 4 |
| `PhysicsProcess` | 8 |
| `OnExitTree` | 16 |
| `OnDestroy` | 32 |

### script.ScriptLanguage — enum

| 值 | 数值 |
|----|------|
| `JS` | 0 |
| `Lua` | 1 |
| `Python` | 2 |
| `V8` | 3 |

## ui（59） · `ui/ui.yaml`

### ui.Align — enum

| 值 | 数值 |
|----|------|
| `Begin` | 0 |
| `Center` | 1 |
| `End` | 2 |

### ui.AlignContent — enum

| 值 | 数值 |
|----|------|
| `Stretch` | 0 |
| `Start` | 1 |
| `End` | 2 |
| `Center` | 3 |
| `SpaceBetween` | 4 |
| `SpaceAround` | 5 |

### ui.AlignItems — enum

| 值 | 数值 |
|----|------|
| `Stretch` | 0 |
| `Start` | 1 |
| `End` | 2 |
| `Center` | 3 |
| `Baseline` | 4 |

### ui.AlignSelf — enum

| 值 | 数值 |
|----|------|
| `Auto` | 0 |
| `Start` | 1 |
| `End` | 2 |
| `Center` | 3 |
| `Stretch` | 4 |
| `Baseline` | 5 |

### ui.Anchor — enum

| 值 | 数值 |
|----|------|
| `TopLeft` | 0 |
| `TopCenter` | 1 |
| `TopRight` | 2 |
| `CenterLeft` | 3 |
| `Center` | 4 |
| `CenterRight` | 5 |
| `BottomLeft` | 6 |
| `BottomCenter` | 7 |
| `BottomRight` | 8 |
| `FullRect` | 9 |
| `Custom` | 10 |

### ui.Button — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `text` | string | — |  |
| `icon` | asset_ref<Texture> | — | ref: Texture |
| `font` | asset_ref<Font> | — | ref: Font |
| `font_size` | float | 16.0F | unit: px |
| `icon_align` | Align | Align::Begin |  |
| `text_color` | math.Vec4 | — |  |
| `style_normal` | asset_ref<Texture> | — | ref: Texture |
| `style_hover` | asset_ref<Texture> | — | ref: Texture |
| `style_pressed` | asset_ref<Texture> | — | ref: Texture |
| `style_disabled` | asset_ref<Texture> | — | ref: Texture |
| `bg_color` | math.Vec4 | {0.3F, 0.3F, 0.3F, 1.0F} |  |
| `toggle_mode` | bool | false |  |
| `button_pressed` | bool | false |  |
| `disabled` | bool | false |  |
| `action` | string | — |  |
| `state` | ButtonState | ButtonState::Normal | transient |

### ui.ButtonState — enum

| 值 | 数值 |
|----|------|
| `Normal` | 0 |
| `Hover` | 1 |
| `Pressed` | 2 |
| `Disabled` | 3 |

### ui.Canvas — struct (88B)

role: `ui_canvas`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `design_width` | float | 0.0F | unit: px |
| `design_height` | float | 0.0F | unit: px |
| `fit_mode` | FitMode | FitMode::ShowAll |  |
| `align` | Align | Align::Center |  |
| `dpi_scale` | bool | true |  |
| `render_scale` | float | 0.0F | range: 0,8 |
| `min_scale` | float | 0.0F | unit: px, doc: 缩放下限（0=不限） |
| `max_scale` | float | 0.0F | unit: px, doc: 缩放上限（0=不限） |
| `scale` | float | 1.0F | transient |
| `scale_x` | float | 1.0F | transient |
| `scale_y` | float | 1.0F | transient |
| `offset_x` | float | 0.0F | transient |
| `offset_y` | float | 0.0F | transient |
| `viewport_w` | float | 0.0F | transient |
| `viewport_h` | float | 0.0F | transient |
| `window_w` | float | 0.0F | transient, doc: 绘制区（设备像素） |
| `window_h` | float | 0.0F | transient |
| `device_scale` | float | 1.0F | transient, doc: 显示器内容缩放（HiDPI） |
| `safe_left` | float | 0.0F | transient, doc: 安全区内缩（刘海/圆角，设备像素） |
| `safe_top` | float | 0.0F | transient |
| `safe_right` | float | 0.0F | transient |
| `safe_bottom` | float | 0.0F | transient |

### ui.CheckBox — struct (变长（含动态成员）)

role: `ui_check`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `checked` | bool | false |  |
| `tristate` | bool | false |  |
| `indeterminate` | bool | false |  |
| `disabled` | bool | false |  |
| `text` | string | — |  |
| `font` | asset_ref<Font> | — | ref: Font |
| `font_size` | float | 16.0F | unit: px, range: 1,512 |
| `text_color` | math.Vec4 | — |  |
| `icon_checked` | asset_ref<Texture> | — | ref: Texture |
| `icon_unchecked` | asset_ref<Texture> | — | ref: Texture |
| `icon_indeterminate` | asset_ref<Texture> | — | ref: Texture |
| `box_position` | Align | Align::Begin |  |
| `state` | CheckState | CheckState::Unchecked | transient |

### ui.CheckState — enum

| 值 | 数值 |
|----|------|
| `Unchecked` | 0 |
| `Checked` | 1 |
| `Indeterminate` | 2 |

### ui.CompareFunc — enum

| 值 | 数值 |
|----|------|
| `Always` | 0 |
| `Equal` | 1 |
| `NotEqual` | 2 |
| `Less` | 3 |
| `Greater` | 4 |

### ui.Container — struct (48B)

role: `ui_layout`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `mode` | ContainerMode | ContainerMode::Free |  |
| `padding` | math.Vec4 | — | unit: px |
| `spacing` | float | 0.0F | unit: px |
| `align_main` | Align | Align::Begin |  |
| `align_cross` | Align | Align::Begin |  |
| `columns` | int32_t | 1 | range: 1,128 |
| `grid_cell` | math.Vec2 | — | unit: px |
| `fit_content` | bool | false |  |

### ui.ContainerMode — enum

| 值 | 数值 |
|----|------|
| `Free` | 0 |
| `HBox` | 1 |
| `VBox` | 2 |
| `Grid` | 3 |

### ui.Control — struct (108B)

role: `ui_control`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `anchor_preset` | Anchor | Anchor::TopLeft |  |
| `anchor_min` | math.Vec2 | — | range: 0,1 |
| `anchor_max` | math.Vec2 | — | range: 0,1 |
| `offset_min` | math.Vec2 | — | unit: px |
| `offset_max` | math.Vec2 | {100.0F, 30.0F} | unit: px |
| `pivot` | math.Vec2 | — | range: 0,1 |
| `min_size` | math.Vec2 | — | unit: px |
| `max_size` | math.Vec2 | — | unit: px |
| `size_flags_h` | SizeFlag | SizeFlag::ShrinkBegin |  |
| `size_flags_v` | SizeFlag | SizeFlag::ShrinkBegin |  |
| `visible` | bool | true |  |
| `mouse_filter` | MouseFilter | MouseFilter::Stop |  |
| `z_index` | int32_t | 0 |  |
| `rect` | math.Vec4 | — | transient |
| `scale` | math.Vec2 | {1.0F, 1.0F} |  |
| `rotation` | float | 0.0F | unit: rad |

### ui.EditBox — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `text` | string | — |  |
| `placeholder` | string | — |  |
| `line_mode` | EditLineMode | EditLineMode::SingleLine |  |
| `max_length` | int32_t | 0 |  |
| `editable` | bool | true |  |
| `secret` | bool | false |  |
| `secret_char` | uint32_t | 8226 |  |
| `select_all_on_focus` | bool | false |  |
| `clear_button` | bool | false |  |
| `text_color` | math.Vec4 | — |  |
| `placeholder_color` | math.Vec4 | {1.0F, 1.0F, 1.0F, 0.5F} |  |
| `selection_color` | math.Vec4 | {0.3F, 0.5F, 1.0F, 0.4F} |  |
| `caret_color` | math.Vec4 | — |  |
| `caret_width` | float | 1.0F | unit: px |
| `font` | asset_ref<Font> | — | ref: Font |
| `font_size` | float | 16.0F | unit: px |
| `align` | TextAlign | TextAlign::Left |  |
| `overflow` | TextOverflow | TextOverflow::Clip |  |
| `wrap_mode` | WrapMode | WrapMode::Word |  |
| `line_spacing` | float | 0.0F | unit: px |
| `caret_pos` | int32_t | 0 | transient |
| `sel_start` | int32_t | -1 | transient |
| `sel_end` | int32_t | -1 | transient |
| `scroll_offset` | math.Vec2 | — | transient |
| `focused` | bool | false | transient |
| `ime_composing` | bool | false | transient |

### ui.EditLineMode — enum

| 值 | 数值 |
|----|------|
| `SingleLine` | 0 |
| `MultiLine` | 1 |

### ui.FillMode — enum

| 值 | 数值 |
|----|------|
| `LeftToRight` | 0 |
| `RightToLeft` | 1 |
| `TopToBottom` | 2 |
| `BottomToTop` | 3 |
| `Radial` | 4 |

### ui.FitMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `FitWidth` | 1 |
| `FitHeight` | 2 |
| `ShowAll` | 3 |
| `NoBorder` | 4 |
| `Exact` | 5 |

### ui.FlexBasisMode — enum

| 值 | 数值 |
|----|------|
| `Auto` | 0 |
| `Points` | 1 |
| `Percent` | 2 |

### ui.FlexBox — struct (48B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `direction` | FlexDirection | FlexDirection::Row |  |
| `wrap` | FlexWrap | FlexWrap::NoWrap |  |
| `justify_content` | JustifyContent | JustifyContent::Start |  |
| `align_items` | AlignItems | AlignItems::Stretch |  |
| `align_content` | AlignContent | AlignContent::Stretch |  |
| `gap_main` | float | 0.0F | unit: px |
| `gap_cross` | float | 0.0F | unit: px |
| `padding` | math.Vec4 | — |  |
| `reverse_safe` | bool | false |  |
| `dirty` | bool | true | transient |

### ui.FlexDirection — enum

| 值 | 数值 |
|----|------|
| `Row` | 0 |
| `RowReverse` | 1 |
| `Column` | 2 |
| `ColumnReverse` | 3 |

### ui.FlexItem — struct (32B)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `grow` | float | 0.0F |  |
| `shrink` | float | 1.0F |  |
| `basis_mode` | FlexBasisMode | FlexBasisMode::Auto |  |
| `basis_value` | float | 0.0F | unit: px |
| `align_self` | AlignSelf | AlignSelf::Auto |  |
| `order` | int32_t | 0 |  |
| `min_main` | float | 0.0F | unit: px |
| `max_main` | float | -1.0F | unit: px |

### ui.FlexWrap — enum

| 值 | 数值 |
|----|------|
| `NoWrap` | 0 |
| `Wrap` | 1 |
| `WrapReverse` | 2 |

### ui.Image — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `texture` | asset_ref<Texture> | — | ref: Texture |
| `region` | math.Vec4 | — |  |
| `modulate` | math.Vec4 | {1.0F, 1.0F, 1.0F, 1.0F} |  |
| `stretch_mode` | StretchMode | StretchMode::Scale |  |
| `flip_h` | bool | false |  |
| `flip_v` | bool | false |  |
| `tile_scale` | math.Vec2 | {1.0F, 1.0F} |  |
| `nine_patch_margin` | math.Vec4 | — | unit: px |
| `texture_size` | math.Vec2 | — | transient |

### ui.JustifyContent — enum

| 值 | 数值 |
|----|------|
| `Start` | 0 |
| `End` | 1 |
| `Center` | 2 |
| `SpaceBetween` | 3 |
| `SpaceAround` | 4 |
| `SpaceEvenly` | 5 |

### ui.ListView — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `item_template` | asset_ref<asset.Scene> | — | ref: asset.Scene |
| `data_source` | entity_ref | — | ref: Script |
| `item_count` | int32_t | 0 | range: 0,1000000 |
| `item_extent` | float | 40.0F | unit: px, range: 0,8192 |
| `orientation` | Orientation | Orientation::Vertical |  |
| `spacing` | float | 0.0F | unit: px |
| `overscan` | int32_t | 2 | range: 0,64 |
| `selection_mode` | SelectionMode | SelectionMode::Single |  |
| `selected_index` | int32_t | -1 | range: -1,1000000 |
| `scroll` | entity_ref | — | ref: ScrollView |
| `pool_size` | int32_t | 0 | transient, readonly |

### ui.Mask — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `mask_mode` | MaskMode | MaskMode::Rect |  |
| `clip_children` | bool | true |  |
| `corner_radius` | math.Vec4 | — | unit: px |
| `mask_texture` | asset_ref<Texture> | — | ref: Texture |
| `texture_channel` | MaskChannel | MaskChannel::Alpha |  |
| `invert` | bool | false |  |
| `stencil_ref` | int32_t | 0 | range: 0,255 |
| `stencil_write` | bool | true |  |
| `stencil_compare` | CompareFunc | CompareFunc::Equal |  |
| `feather` | float | 0.0F | unit: px, range: 0,64 |

### ui.MaskChannel — enum

| 值 | 数值 |
|----|------|
| `Alpha` | 0 |
| `Luminance` | 1 |
| `Red` | 2 |

### ui.MaskMode — enum

| 值 | 数值 |
|----|------|
| `Rect` | 0 |
| `RoundedRect` | 1 |
| `Texture` | 2 |
| `Stencil` | 3 |

### ui.MouseFilter — enum

| 值 | 数值 |
|----|------|
| `Stop` | 0 |
| `Pass` | 1 |
| `Ignore` | 2 |

### ui.OpacityMode — enum

| 值 | 数值 |
|----|------|
| `Normal` | 0 |
| `Gamma` | 1 |

### ui.Orientation — enum

| 值 | 数值 |
|----|------|
| `Vertical` | 0 |
| `Horizontal` | 1 |

### ui.PageTransition — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Slide` | 1 |
| `Fade` | 2 |
| `Cursor` | 3 |

### ui.PageView — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `current_page` | int32_t | 0 | range: 0,65535 |
| `direction` | ScrollDirection | ScrollDirection::Horizontal |  |
| `loop` | bool | false |  |
| `transition` | PageTransition | PageTransition::Slide |  |
| `transition_time` | float | 0.3F | unit: s, range: 0,10 |
| `drag_threshold` | float | 0.25F | range: 0,1 |
| `fling_threshold` | float | 400.0F | unit: px/s |
| `deadzone` | int32_t | 0 | unit: px |
| `snap` | bool | true |  |
| `clip_content` | bool | true |  |
| `page_count` | int32_t | 0 | transient, readonly |
| `page_ratio` | float | 0.0F | range: 0,1, transient |
| `current_node` | entity_ref | — | ref: Control, transient |

### ui.ProgressBar — struct (变长（含动态成员）)

role: `ui_progress`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `value` | float | 0.0F | range: 0,1 |
| `min_display` | float | 0.0F | range: 0,1 |
| `mode` | ProgressMode | ProgressMode::Determinate |  |
| `fill_mode` | FillMode | FillMode::LeftToRight |  |
| `radial_center` | math.Vec2 | {0.5F, 0.5F} | range: 0,1 |
| `radial_begin` | float | 0.0F | unit: rad |
| `radial_fill` | float | -6.2831853F | unit: rad |
| `indeterminate_speed` | float | 1.0F | unit: 1/s, range: 0,10 |
| `overlay` | bool | false |  |
| `back_texture` | asset_ref<Texture> | — | ref: Texture |
| `fill_texture` | asset_ref<Texture> | — | ref: Texture |
| `back_color` | math.Vec4 | {0.2F, 0.2F, 0.2F, 1.0F} |  |
| `fill_color` | math.Vec4 | {0.4F, 0.8F, 0.4F, 1.0F} |  |
| `show_percentage` | bool | false |  |
| `font` | asset_ref<Font> | — | ref: Font |
| `phase` | float | 0.0F | range: 0,1, transient |

### ui.ProgressMode — enum

| 值 | 数值 |
|----|------|
| `Determinate` | 0 |
| `Indeterminate` | 1 |

### ui.RadioGroup — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `selected_index` | int32_t | -1 | range: -1,65535 |
| `allow_unselect` | bool | false |  |
| `loop` | bool | true |  |
| `orientation` | Orientation | Orientation::Vertical |  |
| `disabled` | bool | false |  |
| `selected` | entity_ref | — | ref: Control, transient |
| `member_count` | int32_t | 0 | transient, readonly |

### ui.SafeArea — struct (变长（含动态成员）)

role: `ui_safe_area`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `mode` | SafeAreaMode | SafeAreaMode::Device |  |
| `anchor` | SafeAreaAnchor | SafeAreaAnchor::Children |  |
| `target` | entity_ref | — | ref: Control |
| `inset_left` | float | 0.0F | unit: px |
| `inset_top` | float | 0.0F | unit: px |
| `inset_right` | float | 0.0F | unit: px |
| `inset_bottom` | float | 0.0F | unit: px |
| `min_size` | math.Vec2 | — | unit: px |
| `debug_draw` | bool | false |  |
| `resolved_inset` | math.Vec4 | — | unit: px, transient |

### ui.SafeAreaAnchor — enum

| 值 | 数值 |
|----|------|
| `Self` | 0 |
| `Children` | 1 |

### ui.SafeAreaMode — enum

| 值 | 数值 |
|----|------|
| `Device` | 0 |
| `Custom` | 1 |

### ui.ScrollDirection — enum

| 值 | 数值 |
|----|------|
| `Horizontal` | 0 |
| `Vertical` | 1 |

### ui.ScrollMode — enum

| 值 | 数值 |
|----|------|
| `Disabled` | 0 |
| `Auto` | 1 |
| `Always` | 2 |

### ui.ScrollOrientation — enum

| 值 | 数值 |
|----|------|
| `Horizontal` | 0 |
| `Vertical` | 1 |
| `Both` | 2 |

### ui.ScrollView — struct (变长（含动态成员）)

role: `ui_scroll`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `content` | entity_ref | — | ref: Control |
| `scroll_h` | float | 0.0F | range: 0,1 |
| `scroll_v` | float | 0.0F | range: 0,1 |
| `scroll_mode_h` | ScrollMode | ScrollMode::Auto |  |
| `scroll_mode_v` | ScrollMode | ScrollMode::Auto |  |
| `scroll_step` | math.Vec2 | {16.0F, 16.0F} | unit: px |
| `follow_focus` | bool | false |  |
| `deadzone` | int32_t | 0 | unit: px |
| `smooth` | bool | false |  |
| `bar_thickness` | float | 8.0F | unit: px |
| `scroll_offset` | math.Vec2 | — | unit: px, transient |
| `content_size` | math.Vec2 | — | unit: px, transient |

### ui.SelectionMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Single` | 1 |
| `Multi` | 2 |

### ui.Side — enum

| 值 | 数值 |
|----|------|
| `Top` | 0 |
| `Bottom` | 1 |
| `Left` | 2 |
| `Right` | 3 |

### ui.SizeFlag — enum

| 值 | 数值 |
|----|------|
| `ShrinkBegin` | 0 |
| `ShrinkCenter` | 1 |
| `ShrinkEnd` | 2 |
| `Expand` | 3 |
| `ExpandFill` | 4 |

### ui.Slider — struct (变长（含动态成员）)

role: `ui_slider`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `min_value` | float | 0.0F |  |
| `max_value` | float | 1.0F |  |
| `value` | float | 0.0F |  |
| `step` | float | 0.0F |  |
| `step_mode` | StepMode | StepMode::None |  |
| `orientation` | SliderOrientation | SliderOrientation::Horizontal |  |
| `invert` | bool | false |  |
| `exp_edit` | bool | false |  |
| `allow_greater` | bool | false |  |
| `disabled` | bool | false |  |
| `track_texture` | asset_ref<Texture> | — | ref: Texture |
| `grabber_texture` | asset_ref<Texture> | — | ref: Texture |
| `grabber_radius` | float | 8.0F | unit: px |
| `show_value_label` | bool | false |  |
| `font` | asset_ref<Font> | — | ref: Font |
| `dragging` | bool | false | transient |
| `ratio` | float | 0.0F | range: 0,1, transient |

### ui.SliderOrientation — enum

| 值 | 数值 |
|----|------|
| `Horizontal` | 0 |
| `Vertical` | 1 |

### ui.StepMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Steps` | 1 |

### ui.StretchMode — enum

| 值 | 数值 |
|----|------|
| `Scale` | 0 |
| `KeepAspect` | 1 |
| `KeepAspectCentered` | 2 |
| `KeepAspectCovered` | 3 |
| `Tile` | 4 |
| `KeepCentered` | 5 |

### ui.Tabs — struct (变长（含动态成员）)

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `current_tab` | int32_t | 0 | range: 0,65535 |
| `tab_bar` | entity_ref | — | ref: Control |
| `side` | Side | Side::Top |  |
| `transition` | TabTransition | TabTransition::None |  |
| `transition_time` | float | 0.15F | unit: s, range: 0,10 |
| `tab_count` | int32_t | 0 | transient, readonly |
| `current_page` | entity_ref | — | ref: Control, transient |

### ui.TabTransition — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Fade` | 1 |
| `Slide` | 2 |

### ui.TextAlign — enum

| 值 | 数值 |
|----|------|
| `Left` | 0 |
| `Center` | 1 |
| `Right` | 2 |
| `Justify` | 3 |

### ui.TextLabel — struct (变长（含动态成员）)

role: `ui_text`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `text` | string | — |  |
| `font` | asset_ref<Font> | — | ref: Font |
| `font_size` | float | 16.0F | unit: px, range: 1,512 |
| `color` | math.Vec4 | {1.0F, 1.0F, 1.0F, 1.0F} |  |
| `align_h` | Align | Align::Begin |  |
| `align_v` | Align | Align::Begin |  |
| `wrap_mode` | TextWrap | TextWrap::None |  |
| `line_spacing` | float | 0.0F | unit: px |
| `outline_size` | float | 0.0F | unit: px |
| `outline_color` | math.Vec4 | {0.0F, 0.0F, 0.0F, 1.0F} |  |
| `uppercase` | bool | false |  |
| `text_size` | math.Vec2 | — | transient |

### ui.TextOverflow — enum

| 值 | 数值 |
|----|------|
| `Clip` | 0 |
| `Ellipsis` | 1 |
| `Scroll` | 2 |

### ui.TextWrap — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Word` | 1 |
| `Char` | 2 |
| `WordSmart` | 3 |

### ui.UIOpacity — struct (变长（含动态成员）)

role: `ui_opacity`

| 字段 | 类型 | 默认值 | attrs |
|------|------|--------|-------|
| `opacity` | float | 1.0F | range: 0,1 |
| `mode` | OpacityMode | OpacityMode::Normal |  |
| `affect_children` | bool | true |  |
| `root` | entity_ref | — | ref: Control |
| `visible_threshold` | float | 0.0F | range: 0,1 |
| `final_alpha` | float | 1.0F | range: 0,1, transient |

### ui.WrapMode — enum

| 值 | 数值 |
|----|------|
| `None` | 0 |
| `Word` | 1 |
| `Char` | 2 |
