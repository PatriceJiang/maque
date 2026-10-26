// pbr —— M3 材质系统（plans/18-pbr-3d.md）：metallic-roughness + Cook-Torrance（GGX）+ ACES tonemap。
// M4 起接入贴图（albedo/MR/normal/AO/emissive）与 IBL；本版只用基色/金属度/粗糙度/自发光/环境色。
//
// 顶点布局（mq_renderer_mesh_gpu layout=0，48B 规范）：location0 pos@0、location1 normal@12
// （tangent@24 / uv@40 留给 M4 贴图与法线贴图）
struct Camera {
  vp: mat4x4f,
  eye: vec4f,          // xyz = 相机位置；w = exposure
  light_dir: vec4f,    // 方向光传播方向（世界空间，单位向量）
  light_color: vec4f,  // rgb = 颜色 × intensity
  ambient: vec4f,      // rgb = 环境光（经 env 颜色 × intensity）
};
@group(0) @binding(0) var<uniform> cam: Camera;

struct Draw {
  model: mat4x4f,
  base_color: vec4f,   // 线性空间
  emissive: vec4f,
  mrp: vec4f,          // x=metallic y=roughness z=normal_scale w=occlusion
};
@group(1) @binding(0) var<uniform> dr: Draw;

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
/* ACES（Narkowicz 拟合）→ 输出 LDR；目标 RGBA8 非 sRGB，故再手写 gamma。 */
fn aces_tonemap(x: vec3f) -> vec3f {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3f(0.0), vec3f(1.0));
}

struct VOut {
  @builtin(position) p: vec4f,
  @location(0) n: vec3f,
  @location(1) w: vec3f,
};

@vertex fn vs(@location(0) pos: vec3f, @location(1) nrm: vec3f) -> VOut {
  var o: VOut;
  let wp = dr.model * vec4f(pos, 1.0);
  o.p = cam.vp * wp;
  o.n = normalize((dr.model * vec4f(nrm, 0.0)).xyz);
  o.w = wp.xyz;
  return o;
}

@fragment fn fs(in: VOut) -> @location(0) vec4f {
  let n = normalize(in.n);
  let v = normalize(cam.eye.xyz - in.w);
  let l = normalize(-cam.light_dir.xyz);
  let h = normalize(v + l);
  let n_dot_l = max(dot(n, l), 0.0);
  let n_dot_v = max(dot(n, v), 1e-4);
  let n_dot_h = max(dot(n, h), 0.0);

  let metallic = clamp(dr.mrp.x, 0.0, 1.0);
  let rough = clamp(dr.mrp.y, 0.02, 1.0);
  let ao = clamp(dr.mrp.w, 0.0, 1.0);
  let base = dr.base_color.rgb;

  // Cook-Torrance：F0 电介质 0.04 / 金属取基色
  let f0 = mix(vec3f(0.04), base, metallic);
  let f = fresnel_schlick(max(dot(h, v), 0.0), f0);
  let d = distribution_ggx(n_dot_h, rough);
  let g = geometry_smith(n_dot_v, n_dot_l, rough);
  let spec = d * g * f / (4.0 * n_dot_v * n_dot_l + 1e-4);
  let kd = (vec3f(1.0) - f) * (1.0 - metallic);
  let diffuse = kd * base / PI;

  var color = (diffuse + spec) * cam.light_color.rgb * n_dot_l;
  // 环境项（M3：常量环境色近似；M4 换成 IBL）
  color = color + cam.ambient.rgb * base * ao;
  color = color + dr.emissive.rgb;
  color = aces_tonemap(color * cam.eye.w);
  return vec4f(pow(color, vec3f(1.0 / 2.2)), dr.base_color.a);
}
