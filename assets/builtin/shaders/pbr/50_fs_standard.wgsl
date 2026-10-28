// Standard（MeshStandardMaterial）：metallic-roughness GGX + 环境近似 + 清漆层。
@fragment fn fs_standard(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  let f0 = mix(vec3f(0.04), s.base, s.metallic);
  let ep = surface_ext_params(uv_base(in.uv));
  var n_cc = s.n;
  if ((flags2_bits() & 8) != 0) {
    n_cc = sample_clearcoat_normal(surface_geo_normal(in.n, ff), in.w, clearcoat_normal_uv(in));
  }
  var color = light_standard(s, f0, n_cc, ep.clearcoat.x, ep.clearcoat.y, dr.phys0.w);
  color = color + s.emissive;
  /* transmission：有 scene color 拷贝（离屏路径）→ 屏幕空间折射；否则回退旧的 Fresnel alpha。 */
  if ((flags2_bits() & 0x100000) != 0 && cam.env_info.w > 0.5) {
    return vec4f(transmission_refraction(in, s, color, f0), 1.0);
  }
  return tone_output(color, s.alpha);
}
