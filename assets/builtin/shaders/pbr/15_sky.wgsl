// envmap 天空背景：全屏三角形，按相机射线采样 equirect 环境贴图。
// 仅在场景启用 env_texture（或 MAQUE_ENV_IBL=1）时绘制；默认 analytic 背景仍是 clear_color。
struct SkyVOut {
  @builtin(position) p: vec4f,
  @location(0) ndc: vec2f,
};

@vertex fn vs_sky(@builtin(vertex_index) i: u32) -> SkyVOut {
  var pos = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var o: SkyVOut;
  o.p = vec4f(pos[i], 0.0, 1.0);
  o.ndc = pos[i];
  return o;
}

@fragment fn fs_sky(in: SkyVOut) -> @location(0) vec4f {
  /* inv_vp 反推世界坐标：clip = inv_vp * (ndc, 1, 1)；方向 = world - eye。 */
  let clip = vec4f(in.ndc.x, in.ndc.y, 1.0, 1.0);
  let wpos = cam.inv_vp * clip;
  let dir = normalize(wpos.xyz / max(wpos.w, 1e-6) - cam.eye.xyz);
  let env = env_sample(dir, 0.0) * cam.env_info.x;
  return tone_output(env, 1.0);
}
