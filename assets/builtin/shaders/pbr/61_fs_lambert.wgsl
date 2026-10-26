// Lambert（MeshLambertMaterial）：仅漫反射 + 环境 + 自发光。
@fragment fn fs_lambert(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  var color = light_lambert(s, dr.phys0.w) + s.emissive;
  return tone_output(color, s.alpha);
}
