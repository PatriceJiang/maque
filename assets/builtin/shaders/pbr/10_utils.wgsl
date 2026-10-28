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

/* KHR_materials_anisotropy 的 GGX 法线分布（Khronos 规范实现附录，Burley 各向异性形式）：
 *   at = mix(alphaRoughness, 1, strength²)，ab = alphaRoughness（alphaRoughness = roughness²）
 *   D = a2 * (a2 / dot(f,f))² / π，f = (ab·TdotH, at·BdotH, a2·NdotH)
 * 注意 at ≥ ab：沿切线方向粗糙度更大，高光被拉长成条/弧（roughness→0 时 ab→0，高光退化成一条细弧）。 */
fn distribution_ggx_aniso(n_dot_h: f32, t_dot_h: f32, b_dot_h: f32, at: f32, ab: f32) -> f32 {
  let a2 = at * ab;
  let f = vec3f(ab * t_dot_h, at * b_dot_h, a2 * n_dot_h);
  let w2 = a2 / max(dot(f, f), 1e-12);
  return a2 * w2 * w2 / PI;
}

/* 各向异性 GGX 可见性项（规范附录 V_GGX_anisotropic；已含 1/(4·NdotL·NdotV) 的 BRDF 归一）。
 * 返回 V*D 里的 V，shader 用 `fresnel * D * V` 直接得镜面 BRDF。 */
fn visibility_ggx_aniso(n_dot_l: f32, n_dot_v: f32, b_dot_v: f32, t_dot_v: f32,
                        t_dot_l: f32, b_dot_l: f32, at: f32, ab: f32) -> f32 {
  let ggx_v = n_dot_l * length(vec3f(at * t_dot_v, ab * b_dot_v, n_dot_v));
  let ggx_l = n_dot_v * length(vec3f(at * t_dot_l, ab * b_dot_l, n_dot_l));
  return clamp(0.5 / (ggx_v + ggx_l + 1e-6), 0.0, 1.0);
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

/* 由屏幕空间导数求切线基（cotangent frame）：不依赖顶点 TANGENT，导入网格可直接用法线贴图。
 * WebGPU 的 dpdy 以 framebuffer 坐标为准（y 向下），与 GLSL dFdy（y 向上）反号；本公式来自 GLSL 语义，
 * 故 dp2/duv2 先取负。否则 T、B 同时取反 = 法线贴图在切线空间旋转 180°（glTF/three.js 约定 T=+∂P/∂u、B=+∂P/∂v）。
 * t/b 必须**各自归一化**（不要用共同的 1/|max|）：|t|² 是「世界/像素 × UV/像素」量纲，小尺度场景
 *（ToyCar 1e-4）下 |t|² 可以远小于 1e-8 → 共同 epsilon 会把 T/B 缩成 0.07 长度，法线贴图效果被削掉 ~85%。 */
fn cotangent_frame(n: vec3f, p: vec3f, uv: vec2f) -> mat3x3f {
  let dp1 = dpdx(p);
  let dp2 = -dpdy(p);
  let duv1 = dpdx(uv);
  let duv2 = -dpdy(uv);
  let dp2perp = cross(dp2, n);
  let dp1perp = cross(n, dp1);
  let t = dp2perp * duv1.x + dp1perp * duv2.x;
  let b = dp2perp * duv1.y + dp1perp * duv2.y;
  let t2 = dot(t, t);
  let b2 = dot(b, b);
  let tiny = 1e-30;
  let it = inverseSqrt(max(t2, tiny));
  let ib = inverseSqrt(max(b2, tiny));
  return mat3x3f(t * it, b * ib, n);
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
      /* `CompareLevel`：显式 LOD0 比较采样，不依赖隐式导数 → 不受上面的非一致控制流 return 影响
       *（WGSL 对 textureSampleCompare 要求 uniform control flow）。 */
      sum += textureSampleCompareLevel(shadow_map, shadow_smp, uv + vec2f(f32(x), f32(y)) * texel, ndc.z - bias);
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

/* clip 空间位置（mat4 × vec4）→ 屏幕 uv（0..1，y 向下，与 fragCoord / 采样纹理一致）。
 * transmission 折射用：把玻璃内出射点投影回屏幕，去 scene_color 取背景。 */
fn ndc_to_uv(c: vec4f) -> vec2f {
  let inv_w = 1.0 / max(c.w, 1e-4);
  let ndc = c.xy * inv_w;
  return vec2f(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

/* 精确 sRGB OETF（three.js 输出色彩空间转换；旧版用 pow(1/2.2) 近似）。 */
fn srgb_encode(c: vec3f) -> vec3f {
  let x = clamp(c, vec3f(0.0), vec3f(1.0));
  let lo = x * 12.92;
  let hi = 1.055 * pow(x, vec3f(1.0 / 2.4)) - 0.055;
  return select(hi, lo, x <= vec3f(0.0031308));
}

/* ACES + exposure + sRGB：Standard/Physical/Basic/Lambert/Phong/Toon/Matcap 用。 */
fn tone_output(color: vec3f, alpha: f32) -> vec4f {
  let c = aces_tonemap(color * cam.eye.w);
  return vec4f(srgb_encode(c), alpha);
}

/* 直接 sRGB（不做 tonemap）：Normal/Shadow 保持可预测的可视化数值。 */
fn output_direct(color: vec3f, alpha: f32) -> vec4f {
  return vec4f(srgb_encode(color), alpha);
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

/* ---- analytic studio IBL（env 贴图缺失时的回退；plans/19 PBR 正确性）----
 * 有 `env_texture` 时镜面走 PMREM GGX 预过滤 mip、漫反射走 irradiance 贴图（见 env_sample/env_irradiance）；
 * 这里保留「天空-地面渐变 + 3 个软光箱」解析近似：
 * 漫反射用半球辐照度，镜面用反射方向采样并按 roughness 模糊 + Karis env BRDF 近似。
 * 数据来自 scn.Scene.env_sky_color/env_ground_color/env_highlight/env_rotation（gfx 已乘强度）。
 */
fn env_avg_sky_ground(d: vec3f) -> vec3f {
  let t = smoothstep(-0.10, 0.55, d.y);
  return mix(cam.env3.rgb, cam.env2.rgb, t);
}

fn env_rotate_y(d: vec3f, a: f32) -> vec3f {
  let c = cos(a);
  let s = sin(a);
  return vec3f(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

fn env_softbox(d: vec3f, center: vec3f, sharp: f32) -> f32 {
  return pow(clamp(dot(d, center), 0.0, 1.0), sharp);
}

/* 环境镜面辐（roughness 越大越模糊；软光箱随粗糙度衰减）。 */
/* ---- equirect env map IBL：PMREM 预过滤结果 ----
 * env_tex 的 mip 链是装配期 CPU 用 GGX importance sampling 按粗糙度预过滤的 radiance
 *（level = roughness × (levels-1)）；binding5 是余弦卷积的漫反射 irradiance（RGBA16F）。
 * 方向→UV 与 runtime build_pmrem 保持逐式一致（经度 wrap、纬度 clamp）。 */
fn env_equirect_uv(d: vec3f) -> vec2f {
  let dir = normalize(d);
  let u = atan2(dir.z, dir.x) / (2.0 * 3.14159265) + 0.5;
  let v = acos(clamp(dir.y, -1.0, 1.0)) / 3.14159265;
  return vec2f(u, v);
}

fn env_sample(d: vec3f, rough: f32) -> vec3f {
  let r = env_rotate_y(d, cam.env3.w);
  /* 第 26 轮：cubemap env 优先（同样按 roughness 查 GGX 预过滤 mip；cube 采样直接用方向）。 */
  if (cam.env4.x > 0.5) {
    let clevels = f32(textureNumLevels(env_cube));
    let clod = clamp(rough, 0.0, 1.0) * max(clevels - 1.0, 0.0);
    return textureSampleLevel(env_cube, env_smp, r, clod).rgb;
  }
  let levels = f32(textureNumLevels(env_tex));
  let lod = clamp(rough, 0.0, 1.0) * max(levels - 1.0, 0.0);
  let uv = env_equirect_uv(r);
  return textureSampleLevel(env_tex, env_smp, uv, lod).rgb;
}


fn env_radiance(d0: vec3f, rough: f32) -> vec3f {
  if (cam.env_info.y > 0.5 || cam.env4.x > 0.5) { return env_sample(d0, rough) * cam.env_info.x; }
  let blur = rough * rough;
  let d = normalize(mix(d0, vec3f(0.0, 1.0, 0.0), blur * 0.55));
  let r = env_rotate_y(d, cam.env3.w);
  var c = env_avg_sky_ground(r);
  let sharp = mix(140.0, 6.0, rough);
  let amp = cam.env2.w * (1.0 - 0.8 * rough);
  c += env_softbox(r, normalize(vec3f(-0.55, 0.78, 0.55)), sharp) * vec3f(1.0, 0.97, 0.92) * amp * 3.6;
  c += env_softbox(r, normalize(vec3f(-0.30, 0.84, 0.62)), sharp * 0.8) * vec3f(1.0, 0.98, 0.95) * amp * 1.8;
  c += env_softbox(r, normalize(vec3f(0.80, 0.30, 0.45)), sharp * 0.55) * vec3f(0.85, 0.92, 1.0) * amp * 1.4;
  c += env_softbox(r, normalize(vec3f(0.05, 0.25, -0.95)), sharp * 0.7) * vec3f(0.92, 0.95, 1.0) * amp * 1.0;
  return c;
}

/* 环境漫反射辐照度：有 PMREM irradiance 贴图时径向查表（余弦卷积结果）；
 * 否则回退 analytic studio（旧基线，保证 148 个迁移场景逐像素不变）。 */
fn env_irradiance(n: vec3f) -> vec3f {
  /* 第 26 轮：cubemap 优先（irradiance cube 按方向查表）。 */
  if (cam.env4.x > 0.5) {
    return textureSampleLevel(env_irr_cube, env_smp, env_rotate_y(n, cam.env3.w), 0.0).rgb * cam.env_info.x;
  }
  if (cam.env_info.z > 0.5) {
    let uv = env_equirect_uv(env_rotate_y(n, cam.env3.w));
    return textureSampleLevel(env_irr_tex, env_smp, uv, 0.0).rgb * cam.env_info.x;
  }
  let t = n.y * 0.5 + 0.5;
  var c = mix(cam.env3.rgb, cam.env2.rgb, t);
  let r = env_rotate_y(n, cam.env3.w);
  c += env_softbox(r, normalize(vec3f(-0.55, 0.78, 0.55)), 3.0) * cam.env2.w * 0.35 * vec3f(1.0, 0.97, 0.92);
  return c;
}

/* Karis 解析 env BRDF（UE4 近似）：镜面环境项 = env * (f0*A + B)。 */
/* KHR_materials_volume：Beer-Lambert 透过率
 * T = exp(-(1 - attenuationColor) * thickness / attenuationDistance)，attenuationDistance <= 0 = 无限（无吸收）。
 * thickness 与 attenuationDistance 同处模型局部尺度 → 比值无量纲，节点缩放相互抵消。 */
fn volume_transmittance(thickness: f32, atten: vec3f, dist: f32) -> vec3f {
  if (dist <= 1e-6) {
    return vec3f(1.0);
  }
  /* 数值保护：极薄介质参数（如 thicknessFactor=8.9 / attenuationDistance=0.01）光程可达数百，
   * 直接 exp 会归零黑掉（USDShaderBallForGltf 官方参考是通透玻璃）→ 光学深度上限 3.0（T>=0.05）。
   * 正常玻璃/琥珀的光学深度远小于 3，此上限不改变它们的观感。 */
  let d = min(max(1.0 - atten, vec3f(0.0)) * (max(thickness, 0.0) / dist), vec3f(3.0));
  return exp(-d);
}

fn env_brdf_approx(f0: vec3f, rough: f32, n_dot_v: f32) -> vec3f {
  let c0 = vec4f(-1.0, -0.0275, -0.572, 0.022);
  let c1 = vec4f(1.0, 0.0425, 1.04, -0.04);
  let r = rough * c0 + c1;
  let a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
  let ab = vec2f(-1.04, 1.04) * a004 + r.zw;
  return f0 * ab.x + ab.y;
}
