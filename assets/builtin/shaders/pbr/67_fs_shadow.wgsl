// Shadow（ShadowMaterial）：只显示方向光阴影的透明材质；alpha = (1-shadow) × opacity。
@fragment fn fs_shadow(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  let factor = shadow_factor(s.w, 1.0);
  let a = clamp((1.0 - factor) * s.alpha, 0.0, 1.0);
  return output_direct(dr.base_color.rgb, a);
}

/* 阴影深度 pass 的 fragment：只做 alpha mask 镂空（无颜色输出；配合 vs_shadow 的 ShadowVOut）。
 * MASK 材质在深度 pass 以前没有 fragment → 树叶/栅栏/发卡等镂空会投出实心方块阴影。 */
@fragment fn fs_shadow_alpha(in: ShadowVOut) {
  let uv = in.uv * dr.uv.xy + dr.uv.zw;
  let a = clamp(dr.base_color.a * dr.model_params.w * textureSample(albedo_tex, albedo_smp, uv).a, 0.0, 1.0);
  if (i32(dr.alpha.y + 0.5) == 1 && a < dr.alpha.x) {
    discard;
  }
}
