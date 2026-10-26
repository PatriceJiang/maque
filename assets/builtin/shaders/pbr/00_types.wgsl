// pbr —— 3D 材质 shader 模块（plans/19-threejs-materials.md）。
// 一个模块多个 fragment 入口：gfx 按材质 shading_model 选入口（fs_standard/fs_physical/fs_basic/
// fs_lambert/fs_phong/fs_toon/fs_normal/fs_matcap/fs_depth/fs_shadow），共享顶点/光照/贴图代码。
// 目录内 *.wgsl 按文件名排序拼接：00 类型/绑定 → 10 工具 → 20 表面 → 30 光照 → 40 顶点 → 50+ 入口。
//
// 顶点布局（mq_renderer_mesh_gpu layout=0，48B 规范）：
//   location0 pos@0 · location1 normal@12 · location2 uv@40

struct GpuLight {
  pos: vec4f,       // xyz=世界位置（dir 光不用）; w=类型（1=dir 2=point 3=spot；0=关闭）
  dir: vec4f,       // xyz=传播方向（point 不用）
  color: vec4f,     // rgb × intensity
  params: vec4f,    // x=range, y=cos(inner), z=cos(outer), w=unused
};

struct Camera {
  vp: mat4x4f,
  eye: vec4f,          // xyz = 相机位置；w = exposure
  light_dir: vec4f,
  light_color: vec4f,  // rgb = 颜色 × intensity
  ambient: vec4f,
  lights: array<GpuLight, 8>,
  light_vp: mat4x4f,    // 方向光阴影 view*proj
  shadow_params: vec4f, // x=bias y=1/1024 z=enabled
  view: mat4x4f,        // 视矩阵（matcap 用 view-space 法线）
  env2: vec4f,          // xyz=天空色（已乘环境强度）w=软光箱强度
  env3: vec4f,          // xyz=地面色（已乘环境强度）w=环境旋转（弧度）
};
@group(0) @binding(0) var<uniform> cam: Camera;
@group(0) @binding(1) var shadow_map: texture_depth_2d;
@group(0) @binding(2) var shadow_smp: sampler_comparison;

struct Draw {
  model: mat4x4f,
  base_color: vec4f,   // 线性空间（贴图模式下作为 tint）
  emissive: vec4f,     // rgb=emissive; w=emissive_intensity
  mrp: vec4f,          // x=metallic y=roughness z=normal_scale w=occlusion
  alpha: vec4f,        // x=alpha_cutoff y=alpha_mode(0 opaque/1 mask/2 blend) z=flags(1=double_sided)
  clearcoat: vec4f,    // x=clearcoat factor y=clearcoat roughness
  model_params: vec4f, // x=shading_model y=side z=flags2 w=opacity
                       // flags2: bit0 flat_shading / bit1 has_matcap / bit2 has_gradient / bit3 has_clearcoat_normal
  phys0: vec4f,        // x=specular_intensity y=ior z=shininess w=env_intensity
  phys1: vec4f,        // x=sheen y=sheen_roughness z=iridescence w=iridescence_ior
  phys2: vec4f,        // x=irid_thickness_min y=irid_thickness_max z=anisotropy w=anisotropy_rotation
  misc: vec4f,         // x=depth_packing y=normal_scale_y z=clearcoat_normal_scale w=toon_steps
  specular: vec4f,     // rgb = specular color（Phong/Physical）
  sheen_color: vec4f,  // rgb = sheen color（Physical）
  uv: vec4f,           // xy = uv_scale, zw = uv_offset（albedo/MR/AO/emissive，KHR_texture_transform）
  uv_normal: vec4f,    // xy = 法线贴图 uv_scale, zw = uv_offset
};
@group(1) @binding(0) var<uniform> dr: Draw;
@group(1) @binding(1) var albedo_tex: texture_2d<f32>;
@group(1) @binding(2) var albedo_smp: sampler;
@group(1) @binding(3) var mr_tex: texture_2d<f32>;          // G=roughness B=metallic
@group(1) @binding(4) var ao_tex: texture_2d<f32>;          // R
@group(1) @binding(5) var normal_tex: texture_2d<f32>;      // 切线空间（缺省中性法线）
@group(1) @binding(6) var emissive_tex: texture_2d<f32>;
@group(1) @binding(7) var matcap_tex: texture_2d<f32>;      // sRGB
@group(1) @binding(8) var gradient_tex: texture_2d<f32>;    // toon 渐变（256×1）
@group(1) @binding(9) var clearcoat_normal_tex: texture_2d<f32>;
// KHR 扩展材质贴图槽（binding 10..16；存在时 runtime 置 flags2 bit12..18，缺失绑 white）
@group(1) @binding(10) var clearcoat_tex: texture_2d<f32>;            // R = clearcoat factor
@group(1) @binding(11) var clearcoat_rough_tex: texture_2d<f32>;      // G = clearcoat roughness
@group(1) @binding(12) var sheen_color_tex: texture_2d<f32>;          // RGB（sRGB）
@group(1) @binding(13) var sheen_rough_tex: texture_2d<f32>;          // A = sheen roughness
@group(1) @binding(14) var iridescence_tex: texture_2d<f32>;          // R = iridescence factor
@group(1) @binding(15) var iridescence_thickness_tex: texture_2d<f32>; // G = thickness 0..1
@group(1) @binding(16) var anisotropy_tex: texture_2d<f32>;           // B = anisotropy strength

struct VOut {
  @builtin(position) p: vec4f,
  @location(0) n: vec3f,
  @location(1) w: vec3f,
  @location(2) uv: vec2f,
  // 布局 v1（COLOR_0/TEXCOORD_1）用：layout0 的 vs 写 (1,1,1,1)/0，逐项乘法/选择不改变旧行为
  @location(3) color: vec4f,
  @location(4) uv1: vec2f,
};
