// Basic（MeshBasicMaterial）：无光照，仅 base_color × albedo（+alpha）。
@fragment fn fs_basic(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  return tone_output(s.base, s.alpha);
}
