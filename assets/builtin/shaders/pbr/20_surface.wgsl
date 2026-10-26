// 表面参数：贴图采样（albedo / MR / AO / emissive / normal）+ alpha test。
// 无贴图时渲染器绑定 1×1 白（颜色/参数乘 1）与 1×1 中性法线 (0.5,0.5,1)。
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

fn surface_common(in: VOut) -> Surface {
  var s: Surface;
  s.uv = in.uv;
  s.w = in.w;
  /* KHR_texture_transform：基础贴图一组 UV、法线贴图单独一组（glTF 允许各贴图不同） */
  let uv = in.uv * dr.uv.xy + dr.uv.zw;
  let uv_n = in.uv * dr.uv_normal.xy + dr.uv_normal.zw;
  let alb_s = textureSample(albedo_tex, albedo_smp, uv);
  s.base = dr.base_color.rgb * alb_s.rgb;
  s.alpha = clamp(dr.base_color.a * dr.model_params.w * alb_s.a, 0.0, 1.0);
  let mr_s = textureSample(mr_tex, albedo_smp, uv);
  s.metallic = clamp(dr.mrp.x * mr_s.b, 0.0, 1.0);
  s.rough = clamp(dr.mrp.y * mr_s.g, 0.02, 1.0);
  s.ao = clamp(dr.mrp.w * textureSample(ao_tex, albedo_smp, uv).r, 0.0, 1.0);
  s.emissive = dr.emissive.rgb * dr.emissive.w * textureSample(emissive_tex, albedo_smp, uv).rgb;

  var n_geo = normalize(in.n);
  if ((flags2_bits() & 1) != 0) {
    n_geo = flat_normal(in.w);
  }
  s.n = sample_surface_normal(n_geo, in.w, uv_n);
  return s;
}

/* MASK：alpha 低于 cutoff 直接丢弃（不透明/混合管线里统一做 alpha test）。 */
fn surface_alpha_test(s: Surface) {
  if (i32(dr.alpha.y + 0.5) == 1 && s.alpha < dr.alpha.x) {
    discard;
  }
}
