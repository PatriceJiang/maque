// 通用工具：BRDF / 阴影 / 切线基 / 色调映射 / 深度打包 / 薄膜干涉近似。
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

/* 3×3 PCF（comparison sampler；关闭时恒 1） */
fn shadow_factor(world: vec3f, n_dot_l: f32) -> f32 {
  if (cam.shadow_params.z < 0.5) {
    return 1.0;
  }
  let lc = cam.light_vp * vec4f(world, 1.0);
  if (lc.w <= 0.0) {
    return 1.0;
  }
  let ndc = lc.xyz / lc.w;
  /* 阴影贴图行序：WebGPU NDC +y = 纹理 v=0（顶部），采样需翻 y（plans/19 修复镜像采样）。 */
  let uv = vec2f(ndc.x, -ndc.y) * 0.5 + vec2f(0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
    return 1.0;
  }
  let texel = cam.shadow_params.y;
  let bias = cam.shadow_params.x + cam.shadow_params.y * (1.0 - n_dot_l) * 2.0;
  var sum = 0.0;
  for (var y = -1; y <= 1; y += 1) {
    for (var x = -1; x <= 1; x += 1) {
      sum += textureSampleCompare(shadow_map, shadow_smp, uv + vec2f(f32(x), f32(y)) * texel, ndc.z - bias);
    }
  }
  return sum / 9.0;
}

/* 屏幕空间导数求面法线（flatShading）；朝向相机修正符号。 */
fn flat_normal(w: vec3f) -> vec3f {
  var n = normalize(cross(dpdx(w), dpdy(w)));
  let v = normalize(cam.eye.xyz - w);
  if (dot(n, v) < 0.0) {
    n = -n;
  }
  return n;
}

fn view_normal(n: vec3f) -> vec3f {
  return normalize((cam.view * vec4f(n, 0.0)).xyz);
}

/* ACES + exposure + gamma：Standard/Physical/Basic/Lambert/Phong/Toon/Matcap 用。 */
fn tone_output(color: vec3f, alpha: f32) -> vec4f {
  let c = aces_tonemap(color * cam.eye.w);
  return vec4f(pow(c, vec3f(1.0 / 2.2)), alpha);
}

/* 直接 gamma（不做 tonemap）：Normal/Depth/Shadow 保持可预测的可视化数值。 */
fn output_direct(color: vec3f, alpha: f32) -> vec4f {
  return vec4f(pow(clamp(color, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)), alpha);
}

/* 深度打包（MeshDepthMaterial.depthPacking = RGBA；与 three.js packDepthToRGBA 同思路）。 */
fn pack_depth_rgba(d: f32) -> vec4f {
  let v = vec4f(d, fract(d * 255.0), fract(d * 65025.0), fract(d * 16581375.0));
  return v - vec4f(v.yzw, 0.0) / 255.0;
}

/* 薄膜干涉近似（iridescence）：随厚度/视角变化的彩虹色（观感用，非物理精确）。 */
fn iridescence_tint(thickness_nm: f32, cos_theta: f32) -> vec3f {
  let optical = thickness_nm * (0.55 + 0.45 * cos_theta);
  let phase = 2.0 * PI * optical / 520.0;
  return vec3f(0.5 + 0.5 * cos(phase),
               0.5 + 0.5 * cos(phase + 2.0943951),
               0.5 + 0.5 * cos(phase + 4.1887902));
}
