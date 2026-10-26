// Toon（MeshToonMaterial）：NdotL 分档（gradient_texture 或 toon_steps 量化）。
@fragment fn fs_toon(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  var s = surface_common(in, ff);
  surface_alpha_test(s);
  let has_gradient = (flags2_bits() & 4) != 0;
  let steps = i32(dr.misc.w + 0.5);
  var color = light_toon(s, steps, has_gradient, dr.phys0.w) + s.emissive;
  return tone_output(color, s.alpha);
}
