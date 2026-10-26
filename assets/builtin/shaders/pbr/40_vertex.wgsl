// 顶点阶段：主 pass（vs）与阴影深度 pass（vs_shadow，复用 group0 相机块的 light_vp）。
@vertex fn vs_shadow(@location(0) pos: vec3f) -> @builtin(position) vec4f {
  return cam.light_vp * dr.model * vec4f(pos, 1.0);
}

@vertex fn vs(@location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f) -> VOut {
  var o: VOut;
  let wp = dr.model * vec4f(pos, 1.0);
  o.p = cam.vp * wp;
  o.n = normalize((dr.model * vec4f(nrm, 0.0)).xyz);
  o.w = wp.xyz;
  o.uv = uv;
  return o;
}
