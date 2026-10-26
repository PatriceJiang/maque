// Normal（MeshNormalMaterial）：视空间法线 → RGB（不做 tonemap）。
@fragment fn fs_normal(in: VOut) -> @location(0) vec4f {
  var s = surface_common(in);
  surface_alpha_test(s);
  let nv = view_normal(s.n);
  let c = nv * 0.5 + vec3f(0.5);
  return output_direct(c, s.alpha);
}
