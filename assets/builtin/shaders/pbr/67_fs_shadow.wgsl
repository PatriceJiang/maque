// Shadow（ShadowMaterial）：只显示方向光阴影的透明材质；alpha = (1-shadow) × opacity。
@fragment fn fs_shadow(in: VOut) -> @location(0) vec4f {
  var s = surface_common(in);
  surface_alpha_test(s);
  let factor = shadow_factor(s.w, 1.0);
  let a = clamp((1.0 - factor) * s.alpha, 0.0, 1.0);
  return output_direct(dr.base_color.rgb, a);
}
