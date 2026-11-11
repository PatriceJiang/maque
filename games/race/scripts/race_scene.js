/* 3D 街机赛车 · 比赛场景运行时（一份逻辑，四套主题共用）
 *
 * 由 themes/<id>.js 调用 boot(themeId)。
 * 流程：装配 8 台车 → 倒计时 → 3 圈 → 结算。
 * 确定性：只读 update(dt) 的固定步长，不读墙钟、不用 Math.random。
 */
import { TRACKS } from './gen/tracks.js';
import { CARS } from './data/cars.js';
import { srgb3 } from './lib/color.js';

const VIEW_W = 1280;
const VIEW_H = 720;

const TUNE = {
  aLatMax: 27.0,        // 车能承受的横向加速度上限（m/s²）→ 高速转弯半径
  aLatAI: 23.0,         // AI 目标横向加速度
  drag: 0.0021,         // 风阻系数（a = drag * v²）
  roll: 0.30,           // 滚阻系数
  gripRate: 7.4,        // 正常抓地：速度方向追上车头的速率（1/s）
  gripRateHB: 1.45,     // 手刹：甩尾
  offGrip: 0.55,        // 出赛道后抓地衰减
  offSpeed: 0.86,       // 出赛道后极速倍率
  wallGap: 0.95,        // 护栏到路面边缘的间隙
  carR: 1.35,           // 车车碰撞半径
  aiLook: [10.0, 46.0],
};

const AUTOPILOT = false;   // 回归用：把玩家车也交给 AI（跑整场验证时临时改）

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const sat = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const wrapPi = (a) => {
  while (a > Math.PI) a -= Math.PI * 2;
  while (a < -Math.PI) a += Math.PI * 2;
  return a;
};

function fmtTime(t) {
  if (!(t > 0)) return '--:--.--';
  const m = Math.floor(t / 60);
  const s = t - m * 60;
  const ss = s.toFixed(2);
  return (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + ss;
}

export function boot(themeId) {
  const T = TRACKS[themeId];
  const tanOf = (i) => [T.nrm[i][1], -T.nrm[i][0]];   // 法线转 90° = 切线

  const S = {
    noHud: false, t: 0, frame: 0, phase: 'countdown', timer: 3.8, msgTimer: 0, msg: '',
    cars: [], player: null, cam: null, camMode: 0,
    goTime: 0, camPos: [0, 3, 0], order: [], labels: {}, best: 0,
    finishPos: 0, finishOrder: [],
  };

  /* ---------------- 节点工具 ---------------- */
  function hex3(hex) {
    const v = parseInt(hex.slice(1), 16);
    return srgb3([((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255]);
  }

  function setMat(id, rgb, metallic, rough, emissive, emInt, clearcoat) {
    node_add_component(id, 'asset.PbrMaterial');
    node_set_number(id, 'asset.PbrMaterial', 'base_color.r', rgb[0]);
    node_set_number(id, 'asset.PbrMaterial', 'base_color.g', rgb[1]);
    node_set_number(id, 'asset.PbrMaterial', 'base_color.b', rgb[2]);
    node_set_number(id, 'asset.PbrMaterial', 'metallic', metallic);
    node_set_number(id, 'asset.PbrMaterial', 'roughness', rough);
    node_set_number(id, 'asset.PbrMaterial', 'side', 0);
    if (clearcoat) {
      node_set_number(id, 'asset.PbrMaterial', 'clearcoat', clearcoat);
      node_set_number(id, 'asset.PbrMaterial', 'clearcoat_roughness', 0.06);
    }
    if (emissive) {
      node_set_number(id, 'asset.PbrMaterial', 'emissive.x', emissive[0]);
      node_set_number(id, 'asset.PbrMaterial', 'emissive.y', emissive[1]);
      node_set_number(id, 'asset.PbrMaterial', 'emissive.z', emissive[2]);
      node_set_number(id, 'asset.PbrMaterial', 'emissive_intensity', emInt || 1.0);
    }
  }

  function addBox(parent, x, y, z, sx, sy, sz, rgb, metallic, rough, castShadow, clearcoat) {
    const id = entity_spawn();
    node_set_parent(id, parent);
    node_set_local_position(id, x, y, z);
    node_add_component(id, 'render.MeshRenderer');
    node_set_number(id, 'render.MeshRenderer', 'geometry', 0);   // Cube
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.x', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.y', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.z', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', castShadow ? 1 : 0);
    node_set_local_scale(id, sx, sy, sz);
    setMat(id, rgb, metallic, rough, null, 1.0, clearcoat);
    return id;
  }

  /* 自发光小件：车灯 / 灯带。base 压暗，靠 emissive_intensity 亮起来 */
  function addGlow(parent, x, y, z, sx, sy, sz, rgb, intensity) {
    const id = entity_spawn();
    node_set_parent(id, parent);
    node_set_local_position(id, x, y, z);
    node_add_component(id, 'render.MeshRenderer');
    node_set_number(id, 'render.MeshRenderer', 'geometry', 0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.x', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.y', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.z', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', 0);
    node_set_local_scale(id, sx, sy, sz);
    setMat(id, [0.03, 0.03, 0.03], 0.0, 0.4, rgb, intensity);
    return id;
  }

  /* 接地暗斑：这个 build 的网格不投实时阴影，用一张半透明暗面把车"按"在地上 */
  function addContactShadow(parent, w, l) {
    const id = entity_spawn();
    node_set_parent(id, parent);
    node_set_local_position(id, 0, 0.035, 0);
    node_add_component(id, 'render.MeshRenderer');
    node_set_number(id, 'render.MeshRenderer', 'geometry', 2);   // Plane（XZ 水平面）
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.x', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.y', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'geometry_size.z', 1.0);
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', 0);
    node_set_local_scale(id, w, 1.0, l);
    setMat(id, [0.0, 0.0, 0.0], 0.0, 1.0);
    node_set_number(id, 'asset.PbrMaterial', 'side', 2);         // 双面：Plane 法线可能朝下
    node_set_number(id, 'asset.PbrMaterial', 'alpha_mode', 2);   // Blend
    node_set_number(id, 'asset.PbrMaterial', 'opacity', 0.42);
    return id;
  }

  function addWheel(parent, x, y, z, rad, wid, rimHex) {
    const id = entity_spawn();
    node_set_parent(id, parent);
    node_set_local_position(id, x, y, z);
    node_add_component(id, 'render.MeshRenderer');
    node_set_string(id, 'render.MeshRenderer', 'mesh_path', 'assets/shared/wheel.mesh.json');
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', 0);
    node_set_local_scale(id, wid / 0.28, rad / 0.34, rad / 0.34);
    setMat(id, srgb3([0.067, 0.071, 0.078]), 0.02, 0.92);   // 轮胎：橡胶
    // 轮毂：子节点，跟着父节点一起转
    const rim = entity_spawn();
    node_set_parent(rim, id);
    node_set_local_position(rim, 0, 0, 0);
    node_add_component(rim, 'render.MeshRenderer');
    node_set_string(rim, 'render.MeshRenderer', 'mesh_path', 'assets/shared/rim.mesh.json');
    node_set_number(rim, 'render.MeshRenderer', 'cast_shadow', 0);
    setMat(rim, rimHex ? hex3(rimHex) : srgb3([0.75, 0.77, 0.78]), 0.85, 0.28);
    return id;
  }

  function addLabel(x, y, size, text, rgb) {
    const id = entity_spawn();
    node_add_component(id, 'scn.Transform2D');
    node_set_number(id, 'scn.Transform2D', 'x', x);
    node_set_number(id, 'scn.Transform2D', 'y', y);
    node_add_component(id, 'scn.Label');
    node_set_number(id, 'scn.Label', 'font_size', size);
    node_set_string(id, 'scn.Label', 'text', text);
    node_set_number(id, 'scn.Label', 'r', rgb[0]);
    node_set_number(id, 'scn.Label', 'g', rgb[1]);
    node_set_number(id, 'scn.Label', 'b', rgb[2]);
    return id;
  }

  function setLabel(id, text) {
    if (id >= 0) node_set_string(id, 'scn.Label', 'text', text);
  }

  /* ---------------- 装配 ---------------- */
  function buildCar(data) {
    const root = entity_spawn();
    node_set_local_position(root, 0, 0, -500);

    const bodyCol = srgb3(data.color);
    const accCol = srgb3(data.accent);
    const cabCol = srgb3(data.cabin).map((v) => v * 0.30);   // 深色玻璃：压暗，别抢车身色
    const bodyH = data.hgt * 0.46;
    const bodyY = data.wheelR + bodyH * 0.5 + 0.04;

    // 车漆：金属漆 + 清漆层（clearcoat）——这是"像车漆"和"像塑料块"的分界
    const bodyId = addBox(root, 0, bodyY, 0, data.wid, bodyH, data.len, bodyCol, 0.30, 0.35, true, 0.72);
    node_set_number(bodyId, 'asset.PbrMaterial', 'clearcoat_roughness', 0.22);
    node_set_number(bodyId, 'asset.PbrMaterial', 'env_intensity', 1.4);
    const cabinH = data.hgt * 0.34;
    // 玻璃：低粗糙 + 偏高金属度 → 吃环境高光
    const glassId = addBox(root, 0, bodyY + bodyH * 0.5 + cabinH * 0.5 - 0.02, data.len * data.cabinZ,
      data.wid * 0.64, cabinH, data.len * 0.34, srgb3([0.027, 0.075, 0.11]), 0.72, 0.09, false, 1.0);
    node_set_number(glassId, 'asset.PbrMaterial', 'clearcoat_roughness', 0.05);
    node_set_number(glassId, 'asset.PbrMaterial', 'env_intensity', 1.8);
    if (data.nose > 0) {
      addBox(root, 0, data.wheelR + data.hgt * 0.12, data.len * 0.36,
        data.wid * 0.92, data.hgt * 0.22, data.len * (0.16 + 0.10 * data.nose), accCol, 0.36, 0.35, false, 0.0);
    }
    if (data.spoiler > 0) {
      addBox(root, 0, bodyY + bodyH * 0.5 + 0.26, -data.len * 0.44,
        data.wid * 0.86, 0.07, data.len * (0.06 + 0.05 * data.spoiler), accCol, 0.36, 0.35, false, 0.0);
    }
    // 灯：前白后红（自发光，不占灯光数量）
    const ly = data.wheelR + data.hgt * 0.16;
    addGlow(root, -data.wid * 0.30, ly, data.len * 0.503, 0.34, 0.11, 0.06, [1.0, 0.945, 0.74], 2.4);
    addGlow(root, data.wid * 0.30, ly, data.len * 0.503, 0.34, 0.11, 0.06, [1.0, 0.945, 0.74], 2.4);
    addGlow(root, 0, ly + 0.02, -data.len * 0.506, data.wid * 0.62, 0.09, 0.05, [0.90, 0.05, 0.08], 0.45);
    const wx = data.wid * 0.5 - 0.04;
    const wz = data.len * 0.30;
    addContactShadow(root, data.wid * 1.12, data.len * 1.06);
    const wheels = [
      addWheel(root, -wx, data.wheelR, wz, data.wheelR, data.wheelW, data.rim),
      addWheel(root, wx, data.wheelR, wz, data.wheelR, data.wheelW, data.rim),
      addWheel(root, -wx, data.wheelR, -wz, data.wheelR, data.wheelW, data.rim),
      addWheel(root, wx, data.wheelR, -wz, data.wheelR, data.wheelW, data.rim),
    ];
    return { root, wheels };
  }

  function placeOnGrid(car, slot, isPlayer) {
    // 沿赛道弧长摆格：直线上摆会在大弯道起点的窄赛道上把后排摆出路面
    const row = Math.floor(slot / 2), col = slot % 2;
    let back = 8.0 + row * 9.2;
    let lat = (col ? 1 : -1) * (T.half * 0.42);
    // 玩家单独占最后一排（居中、再退 4.5m）：追车相机不会被旁边那台车糊住
    if (isPlayer) { back += 4.5; lat = 0; }
    const steps = Math.max(1, Math.round(back / T.step));
    const i = (T.n - steps) % T.n;
    const p = T.pts[i], n = T.nrm[i], t = tanOf(i);
    car.x = p[0] + n[0] * lat;
    car.z = p[1] + n[1] * lat;
    car.yaw = Math.atan2(t[0], t[1]);
    car.vAng = car.yaw;
    car.prog = i * T.step;
    car.y = T.h[i];
    car.idx = 0;
  }

  /* ---------------- 赛道投影 ---------------- */
  function nearestIndex(x, z, hint) {
    let best = hint, bestD = 1e18;
    for (let k = -28; k <= 28; ++k) {
      const i = (hint + k + T.n) % T.n;
      const dx = x - T.pts[i][0], dz = z - T.pts[i][1];
      const d = dx * dx + dz * dz;
      if (d < bestD) { bestD = d; best = i; }
    }
    return best;
  }

  /* ---------------- 物理 ---------------- */
  function stepCar(c, dt) {
    const d = c.d, inp = c.inp;
    const speed = c.v;

    const maxSteer = 0.62 - 0.44 * sat(Math.abs(speed) / d.vMax);
    const target = inp.steer * maxSteer;
    const rate = d.steerRate * (inp.hb ? 1.3 : 1) * dt;
    c.steer += clamp(target - c.steer, -rate, rate);

    // 坡度：h 的差分即斜率，直接当重力分量
    const slope = (T.h[(c.idx + 1) % T.n] - T.h[c.idx]) / T.step;
    const vmaxEff = d.vMax * (c.off ? TUNE.offSpeed : 1);
    const dragK = (d.accel / (d.vMax * d.vMax)) * 1.15;
    let a = inp.throttle * d.accel * Math.max(0, 1 - 0.22 * sat(speed / vmaxEff));
    a -= inp.brake * d.brake;
    a -= dragK * speed * Math.abs(speed);
    a -= TUNE.roll * speed * 0.12 * (c.off ? 1.4 : 1);
    if (c.off) a -= 0.6;
    a -= 9.81 * slope;
    const minV = (c === S.player) ? -6.0 : 0.0;
    c.v = clamp(c.v + a * dt, minV, d.vMax * 1.06);

    const wb = d.len * 0.62;
    const raw = (c.v * Math.tan(c.steer)) / wb;
    const cap = (TUNE.aLatMax * (c.off ? 0.72 : 1)) / Math.max(Math.abs(c.v), 4.0);
    const yawRate = clamp(raw, -cap, cap) * (inp.hb ? 1.32 : 1);
    c.yaw += yawRate * dt;

    const gripRate = (inp.hb ? TUNE.gripRateHB : TUNE.gripRate)
      * (c.off ? TUNE.offGrip : 1) * (d.grip / 16.0);
    const slip = wrapPi(c.yaw - c.vAng);
    c.vAng += slip * (1 - Math.exp(-gripRate * dt));
    c.slip = slip;
    c.v -= Math.abs(Math.sin(slip)) * 5.0 * dt;

    c.x += Math.sin(c.vAng) * c.v * dt;
    c.z += Math.cos(c.vAng) * c.v * dt;
    c.spin += (c.v / d.wheelR) * dt;
  }

  function constrain(c) {
    const i = nearestIndex(c.x, c.z, c.idx);    c.idx = i;
    const p = T.pts[i], n = T.nrm[i], t = tanOf(i);
    const dx = c.x - p[0], dz = c.z - p[1];
    const lat = dx * n[0] + dz * n[1];
    const along = dx * t[0] + dz * t[1];

    c.off = Math.abs(lat) > T.half;
    c.y = T.h[i];
    c.pitch = Math.atan2(T.h[(i + 1) % T.n] - T.h[i], T.step);

    const lim = T.half + TUNE.wallGap;
    if (Math.abs(lat) > lim) {
      const s = Math.sign(lat);
      c.x = p[0] + n[0] * s * lim + t[0] * along;
      c.z = p[1] + n[1] * s * lim + t[1] * along;
      const vdx = Math.sin(c.vAng), vdz = Math.cos(c.vAng);
      if ((vdx * n[0] + vdz * n[1]) * s > 0) {
        const dir = c.v >= 0 ? 1 : -1;
        c.vAng = Math.atan2(t[0] * dir, t[1] * dir);
        c.v *= 0.80;
        c.hit = 1;
      }
    }
    c.prog = (i + clamp(along / T.step, -1, 1)) * T.step;
    if (c.prog < 0) c.prog += T.len;
    if (c.prog >= T.len) c.prog -= T.len;
  }

  function lapCheck(c) {
    const prev = c.prevProg;
    c.prevProg = c.prog;
    if (prev === undefined) return;
    if (prev > T.len * 0.72 && c.prog < T.len * 0.28) {
      c.lap += 1;
      if (c.lap === 1) {            // 第一次过线 = 正式起跑，起步段不计圈速
        c.lapStart = S.t;
        return;
      }
      const lt = S.t - c.lapStart;
      c.lapStart = S.t;
      if (lt > 0 && (c.best === 0 || lt < c.best)) c.best = lt;
      c.lapTimes.push(lt);
      if (c.lap > T.laps && !c.finished) {
        c.finished = true;
        c.finishTime = S.t;
        S.finishOrder.push(c);
      }
    }
  }

  function collideCars() {
    const cars = S.cars;
    for (let a = 0; a < cars.length; ++a) {
      for (let b = a + 1; b < cars.length; ++b) {
        const A = cars[a], B = cars[b];
        if (A.ghostT > 0 || B.ghostT > 0) continue;
        const dx = B.x - A.x, dz = B.z - A.z;
        const d2 = dx * dx + dz * dz;
        const R = TUNE.carR * 2;
        if (d2 > R * R || d2 < 1e-6) continue;
        const d = Math.sqrt(d2);
        const nx = dx / d, nz = dz / d;
        const push = (R - d) * 0.5;
        A.x -= nx * push; A.z -= nz * push;
        B.x += nx * push; B.z += nz * push;
        const rel = (Math.sin(B.vAng) * B.v - Math.sin(A.vAng) * A.v) * nx +
          (Math.cos(B.vAng) * B.v - Math.cos(A.vAng) * A.v) * nz;
        if (rel < 0) {
          const imp = -rel * 0.5;
          A.v -= imp * 0.25;
          B.v -= imp * 0.25;
        }
      }
    }
  }

  /* ---------------- 输入 ---------------- */
  const KEY = { W: 87, A: 65, S: 83, D: 68, UP: 8, DOWN: 9, LEFT: 6, RIGHT: 7, SPACE: 2, C: 67, R: 82, ESC: 1, ENTER: 3 };
  const key = (k) => input_key(k) > 0;

  function readPlayerInput(c) {
    let steer = 0, th = 0, br = 0;
    if (key(KEY.A) || key(KEY.LEFT)) steer -= 1;
    if (key(KEY.D) || key(KEY.RIGHT)) steer += 1;
    if (key(KEY.W) || key(KEY.UP)) th = 1;
    if (key(KEY.S) || key(KEY.DOWN)) br = 1;
    const hb = key(KEY.SPACE) ? 1 : 0;

    if (input_gamepad_connected() > 0) {
      const ax = input_axis(0);
      if (Math.abs(ax) > 0.2) steer = clamp(ax, -1, 1);
      const lt = input_axis(4), rt = input_axis(5);
      if (rt > 0.1) th = sat(rt);
      if (lt > 0.1) br = sat(lt);
    }
    c.inp.steer = steer;
    c.inp.throttle = th;
    c.inp.brake = br;
    c.inp.hb = hb;
  }

  function readAIInput(c) {
    const l0 = TUNE.aiLook[0], l1 = TUNE.aiLook[1];
    const look = clamp(l0 + Math.abs(c.v) * 0.55, l0, l1);
    const j = (c.idx + Math.max(1, Math.round(look / T.step))) % T.n;
    const lane = c.lane + (c.avoidT > 0 ? c.avoidDir * 3.2 : 0);
    const target = T.pts[j];
    const nTarget = T.nrm[j];
    const tx = target[0] + nTarget[0] * lane;
    const tz = target[1] + nTarget[1] * lane;
    const desired = Math.atan2(tx - c.x, tz - c.z);
    c.inp.steer = clamp(wrapPi(desired - c.yaw) * 2.1, -1, 1);

    const k = Math.abs(T.curv[j]) + 1e-4;
    let vt = Math.sqrt((TUNE.aLatAI * c.skill) / k);
    vt = Math.min(vt, c.d.vMax * c.skill) * c.rubber;
    // 起步 1.2s 内收敛油门：避免 7 台 AI 同时满油挤成一团
    const ramp = clamp((S.t - S.goTime) / 1.2, 0.35, 1.0);
    c.inp.throttle = clamp((vt - c.v) / 5.0, 0, 1) * ramp;
    c.inp.brake = clamp((c.v - vt) / 4.0, 0, 1);
    c.inp.hb = 0;
    if (c.avoidT > 0) c.avoidT -= c.dt;
  }

  /* ---------------- HUD ---------------- */
  function buildHud() {
    const white = [0.95, 0.96, 0.98];
    const dim = [0.72, 0.76, 0.82];
    S.labels.speed = addLabel(VIEW_W - 250, 92, 46, '0', white);
    S.labels.unit = addLabel(VIEW_W - 88, 108, 22, 'KM/H', dim);
    S.labels.lap = addLabel(40, 96, 34, 'LAP 1/3', white);
    S.labels.pos = addLabel(40, 150, 30, 'POS 1/8', dim);
    S.labels.time = addLabel(40, 204, 30, 'TIME 00:00.00', dim);
    S.labels.best = addLabel(40, 256, 26, 'BEST --:--.--', dim);
    S.labels.center = addLabel(VIEW_W * 0.5 - 96, VIEW_H * 0.5 + 24, 72, '3', [1.0, 0.86, 0.35]);
    S.labels.hint = addLabel(40, 24, 20, 'WASD  SPACE=DRIFT  C=CAM  R=RESTART  ESC=MENU', dim);
  }

  function updateHud() {
    const p = S.player;
    setLabel(S.labels.speed, String(Math.round(Math.abs(p.v) * 3.6)));
    setLabel(S.labels.lap, 'LAP ' + clamp(p.lap, 1, T.laps) + '/' + T.laps);
    const pos = S.order.indexOf(p) + 1;
    setLabel(S.labels.pos, 'POS ' + pos + '/' + S.cars.length);
    setLabel(S.labels.time, 'TIME ' + fmtTime(p.lapStart > 0 ? S.t - p.lapStart : 0));
    setLabel(S.labels.best, 'BEST ' + fmtTime(p.best));

    if (S.phase === 'countdown') {
      const n = Math.ceil(S.timer - 0.8);
      setLabel(S.labels.center, n > 0 ? String(n) : 'GO!');
    } else if (S.msgTimer > 0) {
      setLabel(S.labels.center, S.msg);
      S.msgTimer -= 0.25;
    } else {
      setLabel(S.labels.center, '');
    }
  }

  /* ---------------- 相机 ---------------- */
  function updateCamera(dt) {
    const c = S.player;
    const fx = Math.sin(c.yaw), fz = Math.cos(c.yaw);
    let back = 7.2 + Math.abs(c.v) * 0.045;
    let up = 2.55 + Math.abs(c.v) * 0.010;
    if (S.camMode === 1) { back = 0.35; up = 1.45; }
    const tx = c.x - fx * back, tz = c.z - fz * back;
    const k = 1 - Math.exp(-9.0 * dt);
    S.camPos[0] += (tx - S.camPos[0]) * k;
    S.camPos[1] += ((c.y + up) - S.camPos[1]) * k;
    S.camPos[2] += (tz - S.camPos[2]) * k;
    node_set_local_position(S.cam, S.camPos[0], S.camPos[1], S.camPos[2]);
    node_look_at(S.cam, c.x + fx * 9.0, c.y + 1.05, c.z + fz * 9.0);
    node_set_number(S.cam, 'render.Camera3D', 'fov_y', 60 + 12 * sat(Math.abs(c.v) / c.d.vMax));
  }

  function syncCarNodes(c) {
    node_set_local_position(c.root, c.x, c.y, c.z);
    node_set_local_rotation(c.root, -c.pitch, c.yaw, 0);
    for (let i = 0; i < 4; ++i) {
      node_set_local_rotation(c.wheels[i], c.spin, i < 2 ? c.steer * 0.7 : 0, 0);
    }
  }

  /* ---------------- 生命周期 ---------------- */
  /* 车头灯光斑：这个 build 的 spot light 在动态节点上不稳定（实测强度改了画面不变），
   * 所以用一块贴在路面前方的暖色半透明面片，视觉效果稳定且只花 1 个 draw。 */
  function addHeadlightPool(car) {
    const id = entity_spawn();
    node_set_parent(id, car.root);
    node_set_local_position(id, 0, 0.035, 0);
    node_add_component(id, 'render.MeshRenderer');
    node_set_string(id, 'render.MeshRenderer', 'mesh_path', 'assets/shared/beam.mesh.json');
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', 0);
    const warm = srgb3([1.0, 0.90, 0.66]);
    setMat(id, warm, 0.0, 1.0, warm, 1.3);   // 自发光：光斑要比路面亮，才像灯而不是贴纸
    node_set_number(id, 'asset.PbrMaterial', 'side', 2);
    node_set_number(id, 'asset.PbrMaterial', 'alpha_mode', 2);
    node_set_number(id, 'asset.PbrMaterial', 'opacity', 0.26);
    return id;
  }
  /* 路灯真点光：整条街的灯不可能都开（8 盏上限），所以只开两盏，
   * 每帧把它们挪到离玩家最近的两个灯位 —— 玩家身边永远是"有灯照亮的路"。 */
  function addLampLight() {
    const id = entity_spawn();
    node_add_component(id, 'render.Light');
    node_set_number(id, 'render.Light', 'type', 1);        // Point
    node_set_number(id, 'render.Light', 'intensity', 70.0);
    node_set_number(id, 'render.Light', 'range', 20.0);
    node_set_number(id, 'render.Light', 'shadow_mode', 0);
    node_set_number(id, 'render.Light', 'color.r', 1.0);
    node_set_number(id, 'render.Light', 'color.g', 0.80);
    node_set_number(id, 'render.Light', 'color.b', 0.52);
    return id;
  }

  function on_start() {
    S.cam = entity_find('cam');

    // 选车：菜单写入 global；未选（直接跑 race_*.scn）时回落到 0 号车
    const wantId = global_get_text('race.car');
    let first = 0;
    for (let i = 0; i < CARS.length; ++i) if (CARS[i].id === wantId) first = i;
    const order = [];
    for (let i = 0; i < CARS.length; ++i) if (i !== first) order.push(i);
    order.push(first);                 // 玩家放最后一格：追车视角不被后车糊住，比赛从后往前打

    for (let slot = 0; slot < CARS.length; ++slot) {
      const data = CARS[order[slot]];
      const rig = buildCar(data);
      const car = {
        id: slot, d: data, root: rig.root, wheels: rig.wheels,
        x: 0, z: 0, y: 0, yaw: 0, pitch: 0, vAng: 0, v: 0, steer: 0, spin: 0, slip: 0, dt: 0.016,
        idx: 0, prog: 0, prevProg: undefined, lap: 0, off: false, hit: 0,
        lapStart: 0, lapTimes: [], best: 0, finished: false, finishTime: 0,
        inp: { steer: 0, throttle: 0, brake: 0, hb: 0 },
        skill: 0.94 + 0.012 * slot,
        lane: ((slot % 3) - 1) * T.half * 0.34,
        rubber: 1.0, avoidT: 0, avoidDir: 1, slowT: 0, ghostT: 0,
      };
      S.cars.push(car);
      placeOnGrid(car, slot, slot === CARS.length - 1);
      syncCarNodes(car);
    }
    S.player = S.cars[S.cars.length - 1];
    if (T.lamps && T.lamps.length) {
      S.lampLights = [addLampLight(), addLampLight()];
    }
    if (T.headlights) {
      addHeadlightPool(S.player);
    }
    S.order = S.cars.slice();
    S.camPos = [S.player.x - Math.sin(S.player.yaw) * 8, S.player.y + 3.2, S.player.z - Math.cos(S.player.yaw) * 8];

    buildHud();
    log('race ready theme=' + themeId + ' car=' + S.player.d.id + ' len=' + Math.round(T.len) +
      ' samples=' + T.n);
  }

  function update(dt) {
    S.t += dt;
    S.frame++;
    const p = S.player;

    if (S.phase === 'countdown') {
      S.timer -= dt;
      readPlayerInput(p);
      p.inp.throttle = 0;
      p.inp.brake = 0;
      p.inp.hb = 0;
      p.v = 0;
      p.vAng = p.yaw;
      if (S.timer <= 0) {
        S.phase = 'racing';
        S.goTime = S.t;
        for (const c of S.cars) c.lapStart = S.t;
        S.msg = 'GO!';
        S.msgTimer = 1.2;
        readPlayerInput(p);
      }
    } else {
      readPlayerInput(p);
    }

    const running = S.phase !== 'countdown';
    for (const c of S.cars) {
      c.dt = dt;
      if (c.ghostT > 0) c.ghostT -= dt;
      if (c === p && !AUTOPILOT) {
        /* 玩家的 inp 已在上面的 readPlayerInput 里写好 */
      } else if (running) {
        readAIInput(c);
      } else {
        c.inp.steer = 0; c.inp.throttle = 0; c.inp.brake = 0; c.inp.hb = 0;
      }
      if (running) {
        stepCar(c, dt);
        constrain(c);
        lapCheck(c);

        // 卡住自救：AI 低速卡死 3s → 放回中心线（只依赖本车计时器，确定性）
        if (c !== p) {
          if (Math.abs(c.v) < 1.5) c.slowT += dt; else c.slowT = 0;
          if (c.slowT > 3.0) {
            const pp = T.pts[c.idx], tt = tanOf(c.idx);
            const off = (c.id % 3 - 1) * 2.0;
            c.x = pp[0] + T.nrm[c.idx][0] * off;
            c.z = pp[1] + T.nrm[c.idx][1] * off;
            c.yaw = Math.atan2(tt[0], tt[1]);
            c.vAng = c.yaw;
            c.v = 6.0;
            c.slowT = 0;
            c.ghostT = 1.2;
          }
        }
      }
      syncCarNodes(c);
    }
    collideCars();
    for (const c of S.cars) { if (c !== p && c.v < 0) c.v = 0; }

    if (S.frame % 12 === 0) {
      S.order = S.cars.slice().sort((a, b) =>
        (b.lap * T.len + b.prog) - (a.lap * T.len + a.prog));
      for (const c of S.cars) {
        if (c === p) continue;
        const gap = (p.lap * T.len + p.prog) - (c.lap * T.len + c.prog);
        c.rubber = clamp(1.0 + gap * 0.00006, 0.94, 1.06);
      }
    }

    if (S.phase === 'racing' && p.finished) {
      S.phase = 'finished';
      S.finishPos = S.order.indexOf(p) + 1;
      S.msg = 'FINISH  P' + S.finishPos;
      S.msgTimer = 8.0;
    }

    // 两盏点光贴在最近的灯位上
    if (S.lampLights && T.lamps && S.frame % 3 === 0) {
      let best = [-1, -1], bd = [1e18, 1e18];
      for (let i = 0; i < T.lamps.length; ++i) {
        const L = T.lamps[i];
        const dx = L[0] - p.x, dz = L[2] - p.z;
        const d = dx * dx + dz * dz;
        if (d < bd[0]) { bd[1] = bd[0]; best[1] = best[0]; bd[0] = d; best[0] = i; }
        else if (d < bd[1]) { bd[1] = d; best[1] = i; }
      }
      for (let k = 0; k < 2; ++k) {
        const L = T.lamps[best[k]];
        if (!L) continue;
        node_set_local_position(S.lampLights[k], L[0], L[1], L[2]);
      }
    }
    updateCamera(dt);
    if (S.frame % 4 === 0 && !S.noHud) updateHud();

    if (S.frame % 300 === 0) {
      let line = 'race t=' + S.t.toFixed(1) + ' phase=' + S.phase;
      for (const c of S.cars) {
        line += ' |' + c.id + ' L' + (c.lap + 1) + ' p' + Math.round(c.prog) + ' v' + Math.round(c.v * 3.6);
      }
      log(line);
    }
  }

  function scenePath(name) {
    const d = scene_dir();
    if (!d) return name;
    return (d.charAt(d.length - 1) === '/' ? d : d + '/') + name;
  }

  function on_key_down(target, k) {
    if (k === KEY.R) {
      director_load_scene(scenePath('race_' + themeId + '.scn'));
    } else if (k === KEY.ESC) {
      director_load_scene(scenePath('menu.scn'));
    } else if (k === KEY.C) {
      S.camMode = (S.camMode + 1) % 2;
    }
  }

  let dbgLogged = false;
  function safeUpdate(dt) {
    try {
      update(dt);
    } catch (e) {
      if (!dbgLogged) {
        dbgLogged = true;
        log('EXC ' + (e && e.message ? e.message : e) + ' | ' + (e && e.stack ? e.stack : ''));
      }
    }
  }

  return { on_start, update: safeUpdate, on_key_down };
}
