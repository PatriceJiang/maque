struct U {
  size: vec2f, view: vec2f, cam: vec2f,
  zoom: f32, rot: f32, pan: f32, eye: f32, zoom_y: f32, pad: f32,
};
@group(0) @binding(0) var<uniform> u: U;
struct Inst { pos: vec2f, radius: f32, rot: f32, color: vec4f, uv: vec4f, half: vec2f, shape: f32, snap: f32, mask: i32, pad0: i32, pad1: i32, pad2: i32 };
struct Mask {
  rect: vec4f, fade: vec4f, opacity: f32, curve: i32, next: i32, pad: i32,
};
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
@group(0) @binding(2) var<storage, read> masks: array<Mask>;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f, @location(2) shape: f32, @location(3) mpos: vec2f, @location(4) @interpolate(flat) mask_idx: i32 };
fn side_fade(d: f32, fade: f32, curve: i32) -> f32 {
  if (fade <= 0.0) { return 1.0; }
  var t = clamp(d / fade, 0.0, 1.0);
  if (curve == 1) { t = t * t * (3.0 - 2.0 * t); }
  return t;
}
fn mask_alpha(p: vec2f, m: Mask) -> f32 {
  let dl = p.x - m.rect.x;
  let dr = m.rect.z - p.x;
  let dt = p.y - m.rect.y;
  let db = m.rect.w - p.y;
  if (dl < 0.0 || dr < 0.0 || dt < 0.0 || db < 0.0) { return 0.0; }
  var a = m.opacity;
  a *= side_fade(dl, m.fade.x, m.curve);
  a *= side_fade(dt, m.fade.y, m.curve);
  a *= side_fade(dr, m.fade.z, m.curve);
  a *= side_fade(db, m.fade.w, m.curve);
  return a;
}
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
  let zoom = vec2f(u.zoom, select(u.zoom, u.zoom_y, u.zoom_y > 0.0));
  let center = inst.pos - u.cam;
  let cc0 = vec2f(center.x * cr - center.y * sr, center.x * sr + center.y * cr);
  var cc = cc0 * zoom;
  let kc0 = vec2f(corner.x * cr - corner.y * sr, corner.x * sr + corner.y * cr);
  let kc = kc0 * zoom;
  var pc = cc + vec2f(u.size.x * 0.5, u.size.y * 0.5) + vec2f(u.pan, u.eye) + u.view;
  // 像素对齐（pixel-perfect）：把矩形**左上角**吸附到整数目标像素（1 纹素 ↔ 1 像素）
  if (inst.snap > 0.5) {
    let hw_t = hw * zoom;
    let tl = floor(pc - hw_t + vec2f(0.5));
    pc = tl + hw_t;
  }
  let px = pc + kc;
  var o: VOut;
  o.p = vec4f(px.x / u.size.x * 2.0 - 1.0, 1.0 - px.y / u.size.y * 2.0, 0., 1.);
  o.uv = c[vi]; o.col = inst.color; o.shape = inst.shape;
  o.mpos = inst.pos + corner;
  o.mask_idx = inst.mask;
  return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  if (in.shape < 0.5 && dot(in.uv, in.uv) > 1.0) { discard; }   // 圆形图元才裁角
  var a = 1.0;
  var mi = in.mask_idx;
  for (var i = 0; i < 8; i++) {
    if (mi <= 0 || mi >= i32(arrayLength(&masks))) { break; }
    let m = masks[mi];
    a *= mask_alpha(in.mpos, m);
    mi = m.next;
  }
  return vec4f(in.col.rgb, in.col.a * a);
}
