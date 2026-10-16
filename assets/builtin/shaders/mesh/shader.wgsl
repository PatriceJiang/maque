// mesh —— **网格图元**（docs/13）：顶点数组 + 索引 + 页纹理（骨骼动画/任意三角网格）。
//
// 与 textured（实例化矩形/圆）的分工：
//   · textured：一个实例 = 一个矩形/圆，逐实例一次变换（适合精灵/字形/UI）。
//   · mesh    ：逐顶点位置/UV/颜色 + 索引缓冲（适合骨骼动画那种一帧一变形的网格）。
// 两者共用**同一份相机 uniform 布局**（48B：size/view/cam/zoom/rot/pan/eye），
// 且顶点已是"渲染目标像素空间、y 向下"→ 变换与 textured 完全一致（不要再翻 y）。
//
// 顶点缓冲布局（gfx 侧交错打包，stride 32B）：
//   location(0) pos: vec2f  —— 渲染目标像素空间（y 向下）
//   location(1) uv : vec2f  —— 归一化 UV（图集页内）
//   location(2) col: vec4f  —— 逐顶点 RGBA（已含骨骼/槽位/实例颜色）
struct U {
  size: vec2f, view: vec2f, cam: vec2f,
  zoom: f32, rot: f32, pan: f32, eye: f32, pad: vec2f,
};
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var t: texture_2d<f32>;
@group(0) @binding(2) var smp: sampler;

struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f };

@vertex fn vs(@location(0) pos: vec2f, @location(1) uv: vec2f, @location(2) col: vec4f) -> VOut {
  // 与 textured 的顶点变换逐项对应：相机平移 → 缩放 → 相机旋转 → 居中/平移/视口偏移。
  let cr = cos(-u.rot); let sr = sin(-u.rot);
  let c = (pos - u.cam) * u.zoom;
  let cc = vec2f(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
  let pc = cc + vec2f(u.size.x * 0.5, u.size.y * 0.5) + vec2f(u.pan, u.eye) + u.view;
  var o: VOut;
  o.p = vec4f(pc.x / u.size.x * 2.0 - 1.0, 1.0 - pc.y / u.size.y * 2.0, 0., 1.);
  o.uv = uv;
  o.col = col;
  return o;
}

@fragment fn fs(in: VOut) -> @location(0) vec4f {
  // 不做 alpha 丢弃：预乘 alpha 图集（pma）的 rgb 已乘过 alpha，
  // 丢弃会把半透明边缘切出硬边；混合方程由管线按 (blend, pma) 选择。
  let c = textureSample(t, smp, in.uv);
  return c * in.col;
}
