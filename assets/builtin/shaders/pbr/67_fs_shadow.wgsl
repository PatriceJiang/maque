// Shadow（ShadowMaterial）：只显示方向光阴影的透明材质；alpha = (1-shadow) × opacity。
@fragment fn fs_shadow(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  let factor = shadow_factor(s.w, 1.0);
  let a = clamp((1.0 - factor) * s.alpha, 0.0, 1.0);
  return output_direct(dr.base_color.rgb, a);
}

/* 阴影深度 pass 的 fragment：只做 alpha mask 镂空（无颜色输出；配合 vs_shadow / vs_shadow1 的 ShadowVOut）。
 * MASK 材质在深度 pass 以前没有 fragment → 树叶/栅栏/发卡等镂空会投出实心方块阴影。
 * 布局 v1：baseColor 贴图可能用 TEXCOORD_1（uv1_mask bit0），且顶点色 alpha 也参与 alpha。 */
@fragment fn fs_shadow_alpha(in: ShadowVOut) {
  let uv0 = in.uv * dr.uv.xy + dr.uv.zw;
  let uv1 = in.uv1 * dr.uv.xy + dr.uv.zw;
  let uv = select(uv0, uv1, ((flags2_bits() >> u32(4)) & 1) != 0);
  let a = clamp(dr.base_color.a * in.color.a * dr.model_params.w
                * textureSample(albedo_tex, albedo_smp, uv).a, 0.0, 1.0);
  if (i32(dr.alpha.y + 0.5) == 1 && a < dr.alpha.x) {
    discard;
  }
}
