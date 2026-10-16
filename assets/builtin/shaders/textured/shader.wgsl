struct U {
  size: vec2f, view: vec2f, cam: vec2f,
  zoom: f32, rot: f32, pan: f32, eye: f32, pad: vec2f,
};
@group(0) @binding(0) var<uniform> u: U;
struct Inst { pos: vec2f, radius: f32, rot: f32, color: vec4f, uv: vec4f, half: vec2f, shape: f32, snap: f32 };
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
@group(0) @binding(2) var t: texture_2d<f32>;
@group(0) @binding(3) var smp: sampler;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f, @location(2) shape: f32 };
@vertex fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
  var c = array<vec2f,6>(vec2f(-1.,-1.), vec2f(1.,-1.), vec2f(-1.,1.),
                         vec2f(-1.,1.), vec2f(1.,-1.), vec2f(1.,1.));
  let inst = insts[ii];
  // 图元尺寸：half = (半宽, 半高) 矩形（字形/图集），退化 → radius 的方形/圆形
  let hw = select(vec2f(inst.radius), inst.half, inst.half.x > 0.0 && inst.half.y > 0.0);
  var corner = c[vi] * hw;
  let cs = cos(inst.rot); let sn = sin(inst.rot);
  corner = vec2f(corner.x * cs - corner.y * sn, corner.x * sn + corner.y * cs);
  let cr = cos(-u.rot); let sr = sin(-u.rot);        // 相机旋转（docs/13 §4）：逆旋转到相机空间
  /* 顶点 = **中心** + 顶点偏移。吸附必须作用在"图元中心"上（且只算一次），
   * 否则逐顶点吸附会把每个顶点按"自己是中心"平移 → 整个矩形偏移半像素
   * （1:1 nearest 下表现为部分行/列取错纹素 → 文字发虚/乱码；块状图元看不出来）。 */
  let center = (inst.pos - u.cam) * u.zoom;
  var cc = vec2f(center.x * cr - center.y * sr, center.x * sr + center.y * cr);
  let kc0 = corner * u.zoom;
  let kc = vec2f(kc0.x * cr - kc0.y * sr, kc0.x * sr + kc0.y * cr);
  var pc = cc + vec2f(u.size.x * 0.5, u.size.y * 0.5) + vec2f(u.pan, u.eye) + u.view;
  // 像素对齐（pixel-perfect）：把矩形**左上角**吸附到整数目标像素（1 纹素 ↔ 1 像素）
  if (inst.snap > 0.5) {
    let tl = floor(pc - hw + vec2f(0.5));
    pc = tl + hw;
  }
  let px = pc + kc;
  var o: VOut;
  o.p = vec4f(px.x / u.size.x * 2.0 - 1.0, 1.0 - px.y / u.size.y * 2.0, 0., 1.);
  // 图集子矩形：uv.x/y = 左上，uv.z/w = 右下；退化（0,0,0,0）→ 整张贴图
  let t = c[vi] * 0.5 + 0.5;
  let full = inst.uv.z <= inst.uv.x || inst.uv.w <= inst.uv.y;
  o.uv = select(vec2f(mix(inst.uv.x, inst.uv.z, t.x), mix(inst.uv.y, inst.uv.w, t.y)), t, full);
  o.col = inst.color; o.shape = inst.shape; return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  let c = textureSample(t, smp, in.uv);
  if (c.a < 0.01) { discard; }
  return c * in.col;                                // 直通 alpha（混合在管线做）
}
