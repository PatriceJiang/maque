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
  let uv0 = in.uv * dr.uv.xy + dr.uv.zw;
  let uv1 = in.uv1 * dr.uv.xy + dr.uv.zw;
  let uv_base = uv_pick(uv0, uv1, 0);
  let uv_mr = uv_pick(uv0, uv1, 1);
  let uv_ao = uv_pick(uv0, uv1, 2);
  let uv_em = uv_pick(uv0, uv1, 4);
  let nuv0 = in.uv * dr.uv_normal.xy + dr.uv_normal.zw;
  let nuv1 = in.uv1 * dr.uv_normal.xy + dr.uv_normal.zw;
  let uv_n = uv_pick(nuv0, nuv1, 3);
  let alb_s = textureSample(albedo_tex, albedo_smp, uv_base);
  /* glTF COLOR_0：顶点色 ⊗ baseColor（layout0 的 vs 输出 (1,1,1,1)，旧场景不变） */
  s.base = dr.base_color.rgb * alb_s.rgb * in.color.rgb;
  s.alpha = clamp(dr.base_color.a * in.color.a * dr.model_params.w * alb_s.a, 0.0, 1.0);
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
  return s;
}

/* clearcoat normal 的 UV（bit7；法线贴图一组 transform） */
fn clearcoat_normal_uv(in: VOut) -> vec2f {
  let nuv0 = in.uv * dr.uv_normal.xy + dr.uv_normal.zw;
  let nuv1 = in.uv1 * dr.uv_normal.xy + dr.uv_normal.zw;
  return uv_pick(nuv0, nuv1, 7);
}

/* MASK：alpha 低于 cutoff 直接丢弃（不透明/混合管线里统一做 alpha test）。 */
fn surface_alpha_test(s: Surface) {
  if (i32(dr.alpha.y + 0.5) == 1 && s.alpha < dr.alpha.x) {
    discard;
  }
}
