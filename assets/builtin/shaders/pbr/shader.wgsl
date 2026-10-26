// pbr —— M4 贴图版 PBR（plans/18-pbr-3d.md）：metallic-roughness + Cook-Torrance（GGX）
// + albedo 贴图（sRGB 纹理由 create_texture2 的 RGBA8_SRGB 在采样时解码）+ ACES tonemap。
// 法线/金属粗糙度/AO/自发光贴图与 IBL/阴影在 M5；本版 get 到的是“贴图材质”的最小闭环。
//
// 顶点布局（mq_renderer_mesh_gpu layout=0，48B 规范）：
//   location0 pos@0 · location1 normal@12 · location2 uv@40（tangent@24 留给 M5 法线贴图）
struct Camera {
  vp: mat4x4f,
  eye: vec4f,          // xyz = 相机位置；w = exposure
  light_dir: vec4f,
  light_color: vec4f,  // rgb = 颜色 × intensity
  ambient: vec4f,
};
@group(0) @binding(0) var<uniform> cam: Camera;

struct Draw {
  model: mat4x4f,
  base_color: vec4f,   // 线性空间（贴图模式下作为 tint）
  emissive: vec4f,
  mrp: vec4f,          // x=metallic y=roughness z=normal_scale w=occlusion
};
@group(1) @binding(0) var<uniform> dr: Draw;
/* 无 albedo 贴图时绑 1×1 白纹理（基色 × 1，不改变观感） */
@group(1) @binding(1) var albedo_tex: texture_2d<f32>;
@group(1) @binding(2) var albedo_smp: sampler;
/* M4d：metallic-roughness 贴图（G=roughness、B=metallic，glTF 约定）与 AO 贴图（R） */
@group(1) @binding(3) var mr_tex: texture_2d<f32>;
@group(1) @binding(4) var ao_tex: texture_2d<f32>;
/* 法线贴图（切线空间；用屏幕空间导数构造切线基，导入网格无需 TANGENT 属性） */
@group(1) @binding(5) var normal_tex: texture_2d<f32>;
/* 自发光贴图（glTF emissiveTexture；缺省白 → 等于 emissiveFactor） */
@group(1) @binding(6) var emissive_tex: texture_2d<f32>;

const PI: f32 = 3.14159265;

fn fresnel_schlick(cos_theta: f32, f0: vec3f) -> vec3f {
  return f0 + (vec3f(1.0) - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}
fn distribution_ggx(n_dot_h: f32, rough: f32) -> f32 {
  let a = rough * rough;
  let a2 = a * a;
  let d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d + 1e-6);
}
fn geometry_smith(n_dot_v: f32, n_dot_l: f32, rough: f32) -> f32 {
  let k = (rough + 1.0) * (rough + 1.0) / 8.0;
  let gv = n_dot_v / (n_dot_v * (1.0 - k) + k + 1e-6);
  let gl = n_dot_l / (n_dot_l * (1.0 - k) + k + 1e-6);
  return gv * gl;
}
fn aces_tonemap(x: vec3f) -> vec3f {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3f(0.0), vec3f(1.0));
}

/* 由屏幕空间导数求切线基（cotangent frame）：不依赖顶点 TANGENT，导入网格可直接用法线贴图。 */
fn cotangent_frame(n: vec3f, p: vec3f, uv: vec2f) -> mat3x3f {
  let dp1 = dpdx(p);
  let dp2 = dpdy(p);
  let duv1 = dpdx(uv);
  let duv2 = dpdy(uv);
  let dp2perp = cross(dp2, n);
  let dp1perp = cross(n, dp1);
  let t = dp2perp * duv1.x + dp1perp * duv2.x;
  let b = dp2perp * duv1.y + dp1perp * duv2.y;
  let invmax = inverseSqrt(max(max(dot(t, t), dot(b, b)), 1e-8));
  return mat3x3f(t * invmax, b * invmax, n);
}

struct VOut {
  @builtin(position) p: vec4f,
  @location(0) n: vec3f,
  @location(1) w: vec3f,
  @location(2) uv: vec2f,
};

@vertex fn vs(@location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f) -> VOut {
  var o: VOut;
  let wp = dr.model * vec4f(pos, 1.0);
  o.p = cam.vp * wp;
  o.n = normalize((dr.model * vec4f(nrm, 0.0)).xyz);
  o.w = wp.xyz;
  o.uv = uv;
  return o;
}

@fragment fn fs(in: VOut) -> @location(0) vec4f {
  var n = normalize(in.n);
  // 法线贴图：白色 1×1 缺省 → nm = (0,0,1) → 等价于不扰动
  let nm = textureSample(normal_tex, albedo_smp, in.uv).xyz * 2.0 - 1.0;
  let nm_s = vec3f(nm.x * dr.mrp.z, nm.y * dr.mrp.z, nm.z);
  n = normalize(cotangent_frame(n, in.w, in.uv) * nm_s);
  let v = normalize(cam.eye.xyz - in.w);
  let l = normalize(-cam.light_dir.xyz);
  let h = normalize(v + l);
  let n_dot_l = max(dot(n, l), 0.0);
  let n_dot_v = max(dot(n, v), 1e-4);
  let n_dot_h = max(dot(n, h), 0.0);

  // 材质参数 × 贴图（无贴图时绑 1×1 白纹理 → 保持标量值不变）
  let mr_s = textureSample(mr_tex, albedo_smp, in.uv);
  let metallic = clamp(dr.mrp.x * mr_s.b, 0.0, 1.0);
  let rough = clamp(dr.mrp.y * mr_s.g, 0.02, 1.0);
  let ao = clamp(dr.mrp.w * textureSample(ao_tex, albedo_smp, in.uv).r, 0.0, 1.0);
  // albedo：sRGB 纹理采样自动解码为线性（RGBA8UnormSrgb），再乘材质 tint
  let base = dr.base_color.rgb * textureSample(albedo_tex, albedo_smp, in.uv).rgb;

  let f0 = mix(vec3f(0.04), base, metallic);
  let f = fresnel_schlick(max(dot(h, v), 0.0), f0);
  let d = distribution_ggx(n_dot_h, rough);
  let g = geometry_smith(n_dot_v, n_dot_l, rough);
  let spec = d * g * f / (4.0 * n_dot_v * n_dot_l + 1e-4);
  let kd = (vec3f(1.0) - f) * (1.0 - metallic);
  let diffuse = kd * base / PI;

  var color = (diffuse + spec) * cam.light_color.rgb * n_dot_l;
  // 环境项：常量漫反射 + 粗糙度相关的环境高光近似（无 IBL 时的廉价替代；
  // 半球渐变 ambient 会压平粗糙度对比，M5 接真 IBL 后再启用）
  let spec_amb = mix(vec3f(0.04), base, metallic) * cam.ambient.rgb * (1.0 - rough) * 0.35;
  color = color + cam.ambient.rgb * base * ao + spec_amb * ao + dr.emissive.rgb * textureSample(emissive_tex, albedo_smp, in.uv).rgb;
  color = aces_tonemap(color * cam.eye.w);
  return vec4f(pow(color, vec3f(1.0 / 2.2)), dr.base_color.a);
}
