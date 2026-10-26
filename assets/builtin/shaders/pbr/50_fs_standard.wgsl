// Standard（MeshStandardMaterial）：metallic-roughness GGX + 环境近似 + 清漆层。
@fragment fn fs_standard(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  let f0 = mix(vec3f(0.04), s.base, s.metallic);
  var n_cc = s.n;
  if ((flags2_bits() & 8) != 0) {
    n_cc = sample_clearcoat_normal(surface_geo_normal(in.n, ff), in.w, in.uv * dr.uv_normal.xy + dr.uv_normal.zw);
  }
  var color = light_standard(s, f0, n_cc, dr.clearcoat.x, dr.clearcoat.y, dr.phys0.w);
  color = color + s.emissive;
  return tone_output(color, s.alpha);
}
