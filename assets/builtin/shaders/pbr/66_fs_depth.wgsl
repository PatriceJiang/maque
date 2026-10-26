// Depth（MeshDepthMaterial）：NDC 深度灰度（Basic）或 RGBA 打包（RGBA）；不做 tonemap。
@fragment fn fs_depth(in: VOut) -> @location(0) vec4f {
  var s = surface_common(in);
  surface_alpha_test(s);
  let d = clamp(in.p.z, 0.0, 1.0);
  if (i32(dr.misc.x + 0.5) == 1) {
    return pack_depth_rgba(d);
  }
  return vec4f(d, d, d, s.alpha);
}
