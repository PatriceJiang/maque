// 3D 粒子入口（plans/28 §4.5）：实例化 billboard（TriangleStrip，4 顶点）。
// 两个顶点入口：vs_particle（视平面）/ vs_particle_y（绕 Y 轴锁定）—— gfx 按
// particle_system.facing 选管线；顶点布局 stride 48B、stepMode=Instance。
@vertex fn vs_particle(@builtin(vertex_index) vi: u32,
                       @location(0) pos_size: vec4f,
                       @location(1) color: vec4f,
                       @location(2) misc: vec4f) -> POut {
  let e = particle_emit(vi, pos_size, color, misc, false);
  var o: POut;
  o.p = e.clip;
  o.uv = e.uv;
  o.color = e.color;
  o.world = e.world;
  return o;
}

@vertex fn vs_particle_y(@builtin(vertex_index) vi: u32,
                         @location(0) pos_size: vec4f,
                         @location(1) color: vec4f,
                         @location(2) misc: vec4f) -> POut {
  let e = particle_emit(vi, pos_size, color, misc, true);
  var o: POut;
  o.p = e.clip;
  o.uv = e.uv;
  o.color = e.color;
  o.world = e.world;
  return o;
}

// 纹理是 sRGB（textureSample 已解码到线性），顶点色也是线性 → 直接相乘；
// alpha 走直通混合（gfx 管线按 blend 模式设置 src/dst factor）。
// 输出前做场景雾 + sRGB 编码（与 pbr 的 Basic unlit 同口径；目标纹理是 UNORM）。
@fragment fn fs_particle(in: POut) -> @location(0) vec4f {
  let tex = textureSample(ptex, psmp, in.uv);
  let lit = particle_fog(tex.rgb * in.color.rgb, in.world);
  return vec4f(particle_srgb_encode(lit), tex.a * in.color.a);
}
