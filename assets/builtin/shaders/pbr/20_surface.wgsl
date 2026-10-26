// 表面参数：贴图采样（albedo / MR / AO / emissive / normal）+ alpha test。
// 无贴图时渲染器绑定 1×1 白（颜色/参数乘 1）与 1×1 中性法线 (0.5,0.5,1)。
// 布局 v1 的顶点色（COLOR_0）⊗ baseColor；TEXCOORD_1 由材质 uv1_mask 选择（flags2 bit4 起）。
struct Surface {
  base: vec3f,
  alpha: f32,
  n: vec3f,
  w: vec3f,
  metallic: f32,
  rough: f32,
  ao: f32,
  emissive: vec3f,
  uv: vec2f,
  alpha_mask: f32,   /* 未乘 opacity 的 alpha（MASK+BLEND 近似用） */
};

fn flags2_bits() -> i32 {
  return i32(dr.model_params.z + 0.5);
}

/* uv1_mask 对应位：bit0=baseColor, 1=MR, 2=AO, 3=normal, 4=emissive, 5=matcap,
 * 6=gradient, 7=clearcoat normal。runtime 把 uv1_mask 折进 flags2 的 bit4..12。 */
fn uv1_sel(bit: i32) -> bool {
  return ((flags2_bits() >> u32(4 + bit)) & 1) != 0;
}

fn uv_pick(uv0: vec2f, uv1: vec2f, bit: i32) -> vec2f {
  return select(uv0, uv1, uv1_sel(bit));
}

/* KHR_texture_transform：uv' = T * R * S * uv（先缩放、再旋转、最后平移）。
 * 旋转角由 gfx 打包进 Draw.specular.w（基础贴图组）/ Draw.sheen_color.w（法线组），
 * 避免给 uniform 结构额外扩字段；默认 0 时行为与旧版完全一致。 */
fn rot2(p: vec2f, a: f32) -> vec2f {
  let c = cos(a);
  let s = sin(a);
  return vec2f(c * p.x - s * p.y, s * p.x + c * p.y);
}

fn uv_base(uv: vec2f) -> vec2f {
  return rot2(uv * dr.uv.xy, dr.specular.w) + dr.uv.zw;
}

fn uv_norm(uv: vec2f) -> vec2f {
  return rot2(uv * dr.uv_normal.xy, dr.sheen_color.w) + dr.uv_normal.zw;
}

fn sample_surface_normal(geo_n: vec3f, w: vec3f, uv: vec2f) -> vec3f {
  let nm = textureSample(normal_tex, albedo_smp, uv).xyz * 2.0 - 1.0;
  let nm_s = vec3f(nm.x * dr.mrp.z, nm.y * dr.misc.y, nm.z);
  return normalize(cotangent_frame(geo_n, w, uv) * nm_s);
}

fn sample_clearcoat_normal(geo_n: vec3f, w: vec3f, uv: vec2f) -> vec3f {
  let nm = textureSample(clearcoat_normal_tex, albedo_smp, uv).xyz * 2.0 - 1.0;
  let nm_s = vec3f(nm.x * dr.misc.z, nm.y * dr.misc.z, nm.z);
  return normalize(cotangent_frame(geo_n, w, uv) * nm_s);
}

/* glTF doubleSided：背面渲染时先把几何法线取反再做光照（规范 MUST；three.js faceDirection 同义）。
 * 切线基由取反后的几何法线派生，normal map 的 T/B 随 n 一起翻转，与 three.js 行为一致。 */
fn surface_geo_normal(raw: vec3f, front_facing: bool) -> vec3f {
  var n = normalize(raw);
  if (dr.alpha.z > 0.5 && !front_facing) {
    n = -n;
  }
  return n;
}

fn surface_common(in: VOut, front_facing: bool) -> Surface {
  var s: Surface;
  s.uv = in.uv;
  s.w = in.w;
  /* KHR_texture_transform：基础贴图一组 UV、法线贴图单独一组（glTF 允许各贴图不同）。
   * 每张贴图再按 uv1_mask 决定用 TEXCOORD_0 还是 TEXCOORD_1（布局 v1 才有有效 uv1）。 */
  let uv0 = uv_base(in.uv);
  let uv1 = uv_base(in.uv1);
  let uv_base = uv_pick(uv0, uv1, 0);
  let uv_mr = uv_pick(uv0, uv1, 1);
  let uv_ao = uv_pick(uv0, uv1, 2);
  let uv_em = uv_pick(uv0, uv1, 4);
  let nuv0 = uv_norm(in.uv);
  let nuv1 = uv_norm(in.uv1);
  let uv_n = uv_pick(nuv0, nuv1, 3);
  let alb_s = textureSample(albedo_tex, albedo_smp, uv_base);
  /* glTF COLOR_0：顶点色 ⊗ baseColor（layout0 的 vs 输出 (1,1,1,1)，旧场景不变） */
  s.base = dr.base_color.rgb * alb_s.rgb * in.color.rgb;
  s.alpha_mask = clamp(dr.base_color.a * in.color.a * alb_s.a, 0.0, 1.0);
  s.alpha = clamp(s.alpha_mask * dr.model_params.w, 0.0, 1.0);
  let mr_s = textureSample(mr_tex, albedo_smp, uv_mr);
  s.metallic = clamp(dr.mrp.x * mr_s.b, 0.0, 1.0);
  s.rough = clamp(dr.mrp.y * mr_s.g, 0.02, 1.0);
  /* glTF occlusionTexture.strength 语义 = mix(1, tex.r, strength)，不是 tex.r × strength */
  let ao_s = textureSample(ao_tex, albedo_smp, uv_ao).r;
  s.ao = clamp(1.0 + dr.mrp.w * (ao_s - 1.0), 0.0, 1.0);
  s.emissive = dr.emissive.rgb * dr.emissive.w * textureSample(emissive_tex, albedo_smp, uv_em).rgb;

  var n_geo = surface_geo_normal(in.n, front_facing);
  if ((flags2_bits() & 1) != 0) {
    n_geo = flat_normal(in.w);           /* flat shading 本身已按朝向相机修正符号 */
  }
  s.n = sample_surface_normal(n_geo, in.w, uv_n);
  /* transmission 近似：玻璃中心仍透，但掠射角按 Fresnel 提高不透明度（观感更像玻璃边缘反光）。 */
  if ((flags2_bits() & 0x100000) != 0) {
    let v = normalize(cam.eye.xyz - s.w);
    let n_dot_v = clamp(dot(s.n, v), 0.0, 1.0);
    let fres = pow(1.0 - n_dot_v, 3.0);
    let tf = clamp(dr.phys2.w, 0.0, 1.0);
    s.alpha = clamp(s.alpha + (1.0 - s.alpha) * fres * tf * 0.75, 0.0, 1.0);
  }
  return s;
}

/* clearcoat normal 的 UV（bit7；法线贴图一组 transform） */
fn clearcoat_normal_uv(in: VOut) -> vec2f {
  let nuv0 = uv_norm(in.uv);
  let nuv1 = uv_norm(in.uv1);
  return uv_pick(nuv0, nuv1, 7);
}

/* ---- KHR 扩展材质贴图槽（binding 10..16；presence = flags2 bit12..18）----
 * glTF 语义（通道）：
 *   clearcoatTexture.R × clearcoatFactor
 *   clearcoatRoughnessTexture.G × clearcoatRoughnessFactor
 *   sheenColorTexture.RGB × sheenColorFactor（sRGB）
 *   sheenRoughnessTexture.A × sheenRoughnessFactor
 *   iridescenceTexture.R × iridescenceFactor
 *   iridescenceThicknessTexture.G → mix(thicknessMin, thicknessMax, G)
 *   anisotropyTexture.B × anisotropyStrength（方向 RG 暂不参与近似 BRDF）
 * 缺贴图时绑 white：乘 1 不改变因子；厚度用 presence 决定是否 mix（避免 white G=1 恒取 max）。 */
struct ExtParams {
  clearcoat: vec2f,        // x=factor y=roughness
  sheen_color: vec3f,
  sheen_roughness: f32,
  iridescence: f32,
  iridescence_thickness: f32,   // <0 = 无厚度贴图（调用方走视角近似）
  anisotropy: f32,
};

fn ext_tex_present(bit: u32) -> bool {
  return ((u32(flags2_bits()) >> (12u + bit)) & 1u) != 0u;
}

fn surface_ext_params(uv: vec2f) -> ExtParams {
  var e: ExtParams;
  e.clearcoat = vec2f(dr.clearcoat.x, dr.clearcoat.y);
  if (ext_tex_present(0u)) { e.clearcoat.x *= textureSample(clearcoat_tex, albedo_smp, uv).r; }
  if (ext_tex_present(1u)) { e.clearcoat.y *= textureSample(clearcoat_rough_tex, albedo_smp, uv).g; }
  e.sheen_color = dr.sheen_color.rgb;
  e.sheen_roughness = dr.phys1.y;
  if (ext_tex_present(2u)) { e.sheen_color *= textureSample(sheen_color_tex, albedo_smp, uv).rgb; }
  if (ext_tex_present(3u)) { e.sheen_roughness *= textureSample(sheen_rough_tex, albedo_smp, uv).a; }
  e.iridescence = dr.phys1.z;
  e.iridescence_thickness = -1.0;
  if (ext_tex_present(4u)) { e.iridescence *= textureSample(iridescence_tex, albedo_smp, uv).r; }
  if (ext_tex_present(5u)) {
    let g = textureSample(iridescence_thickness_tex, albedo_smp, uv).g;
    e.iridescence_thickness = mix(dr.phys2.x, dr.phys2.y, clamp(g, 0.0, 1.0));
  }
  e.anisotropy = dr.phys2.z;
  if (ext_tex_present(6u)) { e.anisotropy *= textureSample(anisotropy_tex, albedo_smp, uv).b; }
  e.clearcoat = clamp(e.clearcoat, vec2f(0.0), vec2f(1.0));
  e.sheen_roughness = clamp(e.sheen_roughness, 0.0, 1.0);
  e.iridescence = clamp(e.iridescence, 0.0, 1.0);
  e.anisotropy = clamp(e.anisotropy, 0.0, 1.0);
  return e;
}

/* MASK：alpha 低于 cutoff 直接丢弃（不透明/混合管线里统一做 alpha test）。 */
fn surface_alpha_test(s: Surface) {
  if (i32(dr.alpha.y + 0.5) == 1 && s.alpha < dr.alpha.x) {
    discard;
  }
  /* transmission/diffuse 近似：原 MASK 材质被改成 BLEND，但仍按未乘 opacity 的 alpha 做镂空 */
  if ((flags2_bits() & 0x80000) != 0 && s.alpha_mask < dr.alpha.x) {
    discard;
  }
}
