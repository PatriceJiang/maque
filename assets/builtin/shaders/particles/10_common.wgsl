// 公共：billboard 基向量 / 角点 / 实例 → 裁剪空间（plans/28 §4.5）。
// 注意：不能从普通函数返回含 @builtin(position) 的结构（naga 报
// "builtin attribute in non-entry function"）→ 先返回无 builtin 的中间结构，入口再组 POut。

struct POut {
  @builtin(position) p: vec4f,
  @location(0) uv: vec2f,
  @location(1) color: vec4f,
  @location(2) world: vec3f,
};

struct PEmit {
  clip: vec4f,
  uv: vec2f,
  color: vec4f,
  world: vec3f,
};

// 视平面基（view-plane）：相机右/上基向量（世界空间）。view 是正交的（无缩放），
// transpose(view) 的行即相机基；w 分量为 0 的齐次行直接取 xyz。
fn particle_basis_view() -> mat3x3f {
  let vt = transpose(pg.view);
  return mat3x3f(normalize(vt[0].xyz), normalize(vt[1].xyz), normalize(vt[2].xyz));
}

// 绕 Y 轴锁定（YAxisLocked）：billboard 始终竖直，right = fwd × up0、up = right × fwd。
// 相机几乎俯视（|fwd.y| > 0.99）时退回视平面基，避免 right 退化。
fn particle_basis_y() -> mat3x3f {
  let vt = transpose(pg.view);
  let fwd = normalize(-vt[2].xyz);
  let up0 = vec3f(0.0, 1.0, 0.0);
  var right = cross(fwd, up0);
  if (dot(right, right) < 1e-6) {
    return particle_basis_view();
  }
  right = normalize(right);
  let up = normalize(cross(right, fwd));
  return mat3x3f(right, up, fwd);
}

// 4 个角（TriangleStrip）：(-1,-1) (1,-1) (-1,1) (1,1)。
fn particle_corner(vi: u32) -> vec2f {
  let x = select(-1.0, 1.0, (vi & 1u) == 1u);
  let y = select(-1.0, 1.0, (vi & 2u) == 2u);
  return vec2f(x, y);
}

fn particle_emit(vi: u32, pos_size: vec4f, color: vec4f, misc: vec4f, y_locked: bool) -> PEmit {
  // select() 只接受标量/向量（矩阵用 var + if）。
  var basis = particle_basis_view();
  if (y_locked) { basis = particle_basis_y(); }
  let right = basis[0];
  let up = basis[1];
  let corner = particle_corner(vi);
  // 面内旋转（misc.x，弧度）：只转 quad 的两个局部轴，不影响世界位置。
  let cr = cos(misc.x);
  let sr = sin(misc.x);
  let local = vec2f(corner.x * cr - corner.y * sr, corner.x * sr + corner.y * cr);
  // pos_size.w = 世界直径 → 半边长 × 0.5。
  let world = pos_size.xyz + (right * local.x + up * local.y) * (pos_size.w * 0.5);
  var e: PEmit;
  e.clip = pg.vp * vec4f(world, 1.0);
  // 局部 → UV：左上角 (0,0)、右下角 (1,1)（世界 up = +local.y 对应 UV v 变小）。
  e.uv = vec2f((corner.x + 1.0) * 0.5, (1.0 - corner.y) * 0.5);
  e.color = color;
  e.world = world;
  return e;
}

/* 场景雾（与 pbr 的 Basic unlit 同口径：雾在 sRGB 编码**之前**按线性空间混合）。
 * 粒子不参与 tone mapping —— 与 fs_basic 一致（unlit 参考色不做 ACES）。 */
fn particle_fog(color: vec3f, world: vec3f) -> vec3f {
  let mode = i32(pg.atmos0.x + 0.5);
  if (mode == 0) { return color; }
  let d = length(pg.eye.xyz - world);
  var f = 0.0;
  if (mode == 1) {
    let near = pg.atmos0.y;
    let far = max(pg.atmos0.z, near + 1e-4);
    f = smoothstep(near, far, d);
  } else {
    let t = max(pg.atmos0.w, 0.0) * d;
    f = 1.0 - exp(-t * t);
  }
  return mix(color, pg.atmos1.xyz, clamp(f, 0.0, 1.0));
}

/* 目标纹理是 **UNORM**（RGBA8/BGRA8），shader 必须自己写 sRGB 编码值（与 pbr 全部入口一致）。 */
fn particle_srgb_encode(c: vec3f) -> vec3f {
  let x = clamp(c, vec3f(0.0), vec3f(1.0));
  let lo = x * 12.92;
  let hi = 1.055 * pow(x, vec3f(1.0 / 2.4)) - 0.055;
  return select(hi, lo, x <= vec3f(0.0031308));
}
