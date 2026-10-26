// unlit3d —— M2 3D 基础管线（plans/18-pbr-3d.md）：方向光 Lambert + 环境光（无贴图）。
// M3 会替换为完整 PBR（GGX/tonemap/材质贴图）；这里只验证 depth/相机/提取/直呈链路。
//
// 顶点布局（与 mq_renderer_mesh_gpu layout=0 的 48B 规范一致，本 shader 只消费前两项）：
//   location 0: pos    vec3  offset 0
//   location 1: normal vec3  offset 12
//   （tangent vec4 @24 / uv vec2 @40 留给 M3 PBR 的贴图通路）
struct Camera {
  vp: mat4x4f,          // view * proj（WebGPU NDC：z ∈ [0,1]）
  eye: vec4f,
  light_dir: vec4f,     // 方向光传播方向（世界空间，单位向量）
  light_color: vec4f,   // rgb × intensity
  ambient: vec4f,
};
@group(0) @binding(0) var<uniform> cam: Camera;

struct Draw {
  model: mat4x4f,
  base_color: vec4f,    // 线性空间
};
@group(1) @binding(0) var<uniform> dr: Draw;

struct VOut {
  @builtin(position) p: vec4f,
  @location(0) n: vec3f,
  @location(1) w: vec3f,
};

@vertex fn vs(@location(0) pos: vec3f, @location(1) nrm: vec3f) -> VOut {
  var o: VOut;
  let wp = dr.model * vec4f(pos, 1.0);
  o.p = cam.vp * wp;
  o.n = normalize((dr.model * vec4f(nrm, 0.0)).xyz);
  o.w = wp.xyz;
  return o;
}

@fragment fn fs(in: VOut) -> @location(0) vec4f {
  // 方向光：light_dir 是光的传播方向 → 入射方向 = -light_dir
  let lambert = max(dot(normalize(in.n), normalize(-cam.light_dir.xyz)), 0.0);
  let lit = cam.ambient.rgb + cam.light_color.rgb * lambert;
  return vec4f(dr.base_color.rgb * lit, dr.base_color.a);
}
