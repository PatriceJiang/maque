// Basic（MeshBasicMaterial / KHR_materials_unlit）：无光照，仅 base_color × albedo × 顶点色（+alpha）。
// unlit 语义是"直接显示材质颜色"：不再走 ACES（Khronos 参考图的 unlit 色块与 baseColor 一致；
// 旧版对 unlit 也做 ACES 导致橙→偏黄/蓝→偏青，见 plans/20 审计与迁移报告）。
@fragment fn fs_basic(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  return output_direct(s.base, s.alpha);
}
