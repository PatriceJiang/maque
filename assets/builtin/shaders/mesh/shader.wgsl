// mesh —— **网格图元**（docs/13）：顶点数组 + 索引 + 页纹理（骨骼动画/任意三角网格）。
//
// 与 textured（实例化矩形/圆）的分工：
//   · textured：一个实例 = 一个矩形/圆，逐实例一次变换（适合精灵/字形/UI）。
//   · mesh    ：逐顶点位置/UV/颜色 + 索引缓冲（适合骨骼动画那种一帧一变形的网格）。
// 两者共用**同一份相机 uniform 布局**（48B：size/view/cam/zoom/rot/pan/eye），
// 且顶点已是"渲染目标像素空间、y 向下"→ 变换与 textured 完全一致（不要再翻 y）。
//
// 顶点缓冲布局（gfx 侧交错打包，stride 48B）：
//   location(0) pos : vec2f  —— 渲染目标像素空间（y 向下）
//   location(1) uv  : vec2f  —— 归一化 UV（图集页内）
//   location(2) col : vec4f  —— 逐顶点 RGBA（**light/亮色**：骨骼 × 槽位 × 附件颜色）
//   location(3) col2: vec4f  —— 逐顶点 RGBA（**dark/暗色**，两色着色 TintBlack）
//
// 两色着色（Tint Black）公式出处 —— **不是推导的，是照抄官方参考实现**：
//   spine-runtimes 4.2 分支官方 GLFW 前端 `spine-glfw/src/spine-glfw.cpp`
//   （代码内嵌 GLSL：顶点 L264-283 的 `layout (location = 3) in vec4 aDarkColor`，
//     片元 L284-298；CPU 侧按 RenderCommand.darkColors 逐顶点填充见 L337-347）。
//   取回命令（本仓库已存档一份，便于复核）：
//     curl.exe -sSL https://raw.githubusercontent.com/EsotericSoftware/spine-runtimes/4.2/spine-glfw/src/spine-glfw.cpp
//   dark 一路的**数值来源**（哪些槽有暗色、没有时用什么）见 spine2d.cpp 与官方
//   `third_party/spine-cpp/src/spine/SkeletonRenderer.cpp:212-221`。
//
// 为什么长这样（官方公式的两个关键性质，实测过）：
//   ① 用**纹理 alpha 与 rgb** 共同参与，不是简单的 `mix(dark, light, tex.a)`；
//   ② 当 dark = (0,0,0,1)（即槽没有启用 TintBlack 时的默认值）时，公式**精确退化成**
//      `rgb = tex.rgb * light.rgb`、`a = tex.a * light.a` —— 与加两色着色之前的实现逐字节一致，
//      所以未启用该特性的素材（spineboy / tank / flappy）不会回归。
struct U {
  size: vec2f, view: vec2f, cam: vec2f,
  zoom: f32, rot: f32, pan: f32, eye: f32, pad: vec2f,
};
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var t: texture_2d<f32>;
@group(0) @binding(2) var smp: sampler;

struct VOut {
  @builtin(position) p: vec4f,
  @location(0) uv: vec2f,
  @location(1) col: vec4f,
  @location(2) col2: vec4f,
};

@vertex fn vs(@location(0) pos: vec2f, @location(1) uv: vec2f,
              @location(2) col: vec4f, @location(3) col2: vec4f) -> VOut {
  // 与 textured 的顶点变换逐项对应：相机平移 → 缩放 → 相机旋转 → 居中/平移/视口偏移。
  let cr = cos(-u.rot); let sr = sin(-u.rot);
  let c = (pos - u.cam) * u.zoom;
  let cc = vec2f(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
  let pc = cc + vec2f(u.size.x * 0.5, u.size.y * 0.5) + vec2f(u.pan, u.eye) + u.view;
  var o: VOut;
  o.p = vec4f(pc.x / u.size.x * 2.0 - 1.0, 1.0 - pc.y / u.size.y * 2.0, 0., 1.);
  o.uv = uv;
  o.col = col;
  o.col2 = col2;
  return o;
}

@fragment fn fs(in: VOut) -> @location(0) vec4f {
  // 不做 alpha 丢弃：预乘 alpha 图集（pma）的 rgb 已乘过 alpha，
  // 丢弃会把半透明边缘切出硬边；混合方程由管线按 (blend, pma) 选择。
  let c = textureSample(t, smp, in.uv);
  // 两色着色（Tint Black）—— **逐字照抄官方 GLSL 的等价式**（出处见文件顶部注释）：
  //   GLSL: vec4 texColor = texture(uTexture, texCoord);
  //         float alpha = texColor.a * lightColor.a;
  //         fragColor.rgb = ((texColor.a - 1.0) * darkColor.a + 1.0 - texColor.rgb)
  //                         * darkColor.rgb + texColor.rgb * lightColor.rgb;
  // 注意这里是 **light/dark 各自 rgb 与 alpha 分工**：alpha 由 light 决定（dark 的 a 只作为
  // rgb 插值的权重项），不是 `mix(dark, light, tex.a)`。
  // dark = (0,0,0,1) 时 rgb 项 == tex.rgb * light.rgb、alpha == tex.a * light.a（精确退化）。
  let alpha = c.a * in.col.a;
  let rgb = ((c.a - 1.0) * in.col2.a + 1.0 - c.rgb) * in.col2.rgb + c.rgb * in.col.rgb;
  return vec4f(rgb, alpha);
}
