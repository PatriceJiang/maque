// 顶点阶段：主 pass（vs）与阴影深度 pass（vs_shadow，复用 group0 相机块的 light_vp）。
// 阴影 fragment 需要 uv 做 alpha mask 镂空投影，故 vs_shadow 同时输出 uv。
struct ShadowVOut {
  @builtin(position) p: vec4f,
  @location(0) uv: vec2f,
};

@vertex fn vs_shadow(@location(0) pos: vec3f, @location(2) uv: vec2f) -> ShadowVOut {
  var o: ShadowVOut;
  o.p = cam.light_vp * dr.model * vec4f(pos, 1.0);
  o.uv = uv;
  return o;
}

/* 法线矩阵 = inverse-transpose(model 3x3)（非均匀/镜像缩放下必须用它，直接用 model 会错）。
 * 恒等式：M^{-T} = (1/det)·[b×c, c×a, a×b]（a/b/c 为 M 的三列）。WGSL 无矩阵求逆，这里取伴随矩阵；
 * det 是标量、normalize 后可忽略，但镜像（det<0）要先乘 sign(det) 才能得到正确方向的 M^{-T}。 */
fn model_normal_matrix(m: mat4x4f) -> mat3x3f {
  let c0 = m[0].xyz;
  let c1 = m[1].xyz;
  let c2 = m[2].xyz;
  let det = dot(c0, cross(c1, c2));
  let s = select(-1.0, 1.0, det >= 0.0);
  return mat3x3f(cross(c1, c2) * s, cross(c2, c0) * s, cross(c0, c1) * s);
}

@vertex fn vs(@location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f) -> VOut {
  var o: VOut;
  let wp = dr.model * vec4f(pos, 1.0);
  o.p = cam.vp * wp;
  var nn = model_normal_matrix(dr.model) * nrm;
  if (dot(nn, nn) < 1e-20) {                 /* 退化（零缩放）时退回模型矩阵并靠 normalize 防御 */
    nn = (dr.model * vec4f(nrm, 0.0)).xyz;
  }
  o.n = normalize(nn);
  o.w = wp.xyz;
  o.uv = uv;
  return o;
}
