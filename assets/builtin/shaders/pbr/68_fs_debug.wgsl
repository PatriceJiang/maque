// F4 调试渲染模式：不采样材质贴图，只保留基色与顶点色。
// solid = 固定方向光的简洁明暗；wireframe = 无光照线段颜色。
@fragment fn fs_debug_solid(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  let n = normalize(select(in.n, -in.n, !ff));
  let ndl = abs(dot(n, normalize(vec3f(0.35, 0.85, 0.40))));
  let base = dr.base_color.rgb * in.color.rgb;
  return tone_output(base * (0.30 + 0.70 * ndl), 1.0);
}

@fragment fn fs_debug_wireframe(in: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4f {
  return output_direct(dr.base_color.rgb * in.color.rgb, 1.0);
}
