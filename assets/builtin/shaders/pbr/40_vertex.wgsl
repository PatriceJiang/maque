// 顶点阶段：主 pass（vs 布局 0 / vs1 布局 1）与阴影深度 pass（vs_shadow/vs_shadow1）。
// 布局 v1（stride 56B）：pos@0 · normal@12 · color@24(RGBA) · uv0@40 · uv1@48 —— 用于 COLOR_0/TEXCOORD_1。
// 阴影 fragment 需要 uv（+顶点色 alpha / uv1）做 alpha mask 镂空投影。
struct ShadowVOut {
  @builtin(position) p: vec4f,
  @location(0) uv: vec2f,
  @location(1) uv1: vec2f,
  @location(2) color: vec4f,
};

// 布局 0（旧 48B：pos3|normal3|tangent4(未用)|uv2）
@vertex fn vs_shadow(@location(0) pos: vec3f, @location(2) uv: vec2f) -> ShadowVOut {
  var o: ShadowVOut;
  o.p = cam.light_vp * dr.model * vec4f(pos, 1.0);
  o.uv = uv;
  o.uv1 = vec2f(0.0);
  o.color = vec4f(1.0);
  return o;
}

// 布局 1（56B）：阴影里需要 color/uv1 才能正确 alpha-mask（顶点色 alpha / TEXCOORD_1 贴图）
@vertex fn vs_shadow1(@location(0) pos: vec3f, @location(1) color: vec4f,
                      @location(2) uv: vec2f, @location(3) uv1: vec2f) -> ShadowVOut {
  var o: ShadowVOut;
  o.p = cam.light_vp * dr.model * vec4f(pos, 1.0);
  o.uv = uv;
  o.uv1 = uv1;
  o.color = color;
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

/* 注意：不能从普通函数返回含 @builtin(position) 的 VOut（naga 会报
 * "builtin attribute in non-entry function"）→ 先返回无 builtin 的中间结构，入口再组 VOut。 */
struct VSCommon {
  clip: vec4f,
  nrm: vec3f,
  world: vec3f,
};

fn vs_common(pos: vec3f, nrm: vec3f) -> VSCommon {
  let wp = dr.model * vec4f(pos, 1.0);
  var nn = model_normal_matrix(dr.model) * nrm;
  if (dot(nn, nn) < 1e-20) {                 /* 退化（零缩放）时退回模型矩阵并靠 normalize 防御 */
    nn = (dr.model * vec4f(nrm, 0.0)).xyz;
  }
  var o: VSCommon;
  o.clip = cam.vp * wp;
  o.nrm = normalize(nn);
  o.world = wp.xyz;
  return o;
}

// 布局 0
@vertex fn vs(@location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f) -> VOut {
  let c = vs_common(pos, nrm);
  var o: VOut;
  o.p = c.clip;
  o.n = c.nrm;
  o.w = c.world;
  o.uv = uv;
  o.color = vec4f(1.0);
  o.uv1 = vec2f(0.0);
  return o;
}

// 布局 1（glTF COLOR_0 / TEXCOORD_1）
@vertex fn vs1(@location(0) pos: vec3f, @location(1) nrm: vec3f, @location(2) uv: vec2f,
               @location(3) color: vec4f, @location(4) uv1: vec2f) -> VOut {
  let c = vs_common(pos, nrm);
  var o: VOut;
  o.p = c.clip;
  o.n = c.nrm;
  o.w = c.world;
  o.uv = uv;
  o.color = color;
  o.uv1 = uv1;
  return o;
}
