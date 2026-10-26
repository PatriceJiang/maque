// Phong（MeshPhongMaterial）：Lambert 漫反射 + Blinn-Phong 高光（specular / shininess）。
@fragment fn fs_phong(in: VOut) -> @location(0) vec4f {
  var s = surface_common(in);
  surface_alpha_test(s);
  var color = light_phong(s, dr.specular.rgb, dr.phys0.z, dr.phys0.w) + s.emissive;
  return tone_output(color, s.alpha);
}
