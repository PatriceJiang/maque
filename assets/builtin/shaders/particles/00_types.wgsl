// particles —— 3D 粒子批渲染 shader 模块（plans/28 §4.5）。
// 目录内 *.wgsl 按文件名排序拼接：00 类型/绑定 → 10 公共函数 → 20 入口。
//
// 几何：**无顶点缓冲**。每个实例 = 一个 billboard quad，4 个角由 @builtin(vertex_index)
// 生成（TriangleStrip：(vi&1, (vi>>1)&1) → 四角）。实例数据 48B（stepMode=Instance）：
//   location 0: pos_size = (世界位置 xyz, 世界直径 w)
//   location 1: color    = 线性 RGBA（已含寿命曲线/淡出）
//   location 2: misc     = (面内旋转 rad, 图集帧索引（预留）, 速度拉伸（预留）, 预留)
//
// 与 pbr 的 Camera（944B）刻意解耦：这里只有 128B（vp + view），改大气/相机布局不会波及粒子。
struct ParticleGlobals {
  vp: mat4x4f,     // 64B 世界 → clip
  view: mat4x4f,   // 64B 世界 → 视图（列主序；transpose 后取行 = 相机基）
  eye: vec4f,      // xyz = 相机世界位置（雾的距离项）
  atmos0: vec4f,   // x=fog_mode(0 none/1 linear/2 exp2) y=fog_near z=fog_far w=fog_density
  atmos1: vec4f,   // rgb=雾颜色（线性） w=tone_mapping（粒子只做 fog + sRGB，与 Basic unlit 同口径）
};
@group(0) @binding(0) var<uniform> pg: ParticleGlobals;
@group(1) @binding(0) var ptex: texture_2d<f32>;
@group(1) @binding(1) var psmp: sampler;
