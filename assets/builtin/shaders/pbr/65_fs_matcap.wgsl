// Matcap（MeshMatcapMaterial）：视空间法线查 matcap 贴图 × base_color（不做光照）。
@fragment fn fs_matcap(in: VOut) -> @location(0) vec4f {
  var s = surface_common(in);
  surface_alpha_test(s);
  var color = s.base;
  if ((flags2_bits() & 2) != 0) {
    let nv = view_normal(s.n);
    let uv = clamp(vec2f(nv.x, -nv.y) * 0.5 + vec2f(0.5), vec2f(0.001), vec2f(0.999));
    let mc = textureSample(matcap_tex, albedo_smp, uv).rgb;
    color = mc * dr.base_color.rgb;
  }
  return tone_output(color, s.alpha);
}
