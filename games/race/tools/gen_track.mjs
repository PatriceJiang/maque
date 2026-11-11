#!/usr/bin/env node
/* 赛道/场景生成器（唯一内容管线）
 *
 *   node tools/gen_track.mjs
 *
 * 输入：data/tracks/<id>.track.json（中心线 + 配色 + 太阳/雾/环境 + 道具规则）
 * 输出：
 *   assets/tracks/<id>/{road,ground,kerb,line,rail,props_a,props_b,water}.mesh.json
 *   scripts/gen/tracks.js      —— 运行时唯一真相（采样后的中心线/法线/曲率/主题参数）
 *   race_<id>.scn              —— 每套主题一个场景（相机/太阳/补光/赛道网格节点）
 *
 * 约束：确定性输出（无时间戳、无随机源、数值 3 位小数、键序固定）。
 */
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';
import { writePNG, makeSky, makeAsphalt, makeGround, makeFacade } from './gen_tex.mjs';

const ROOT = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), '..');
const TAU = Math.PI * 2;
const r3 = (v) => Math.round(v * 1000) / 1000;
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
/* 调色板按 sRGB 写，引擎吃线性值 —— 在这里统一换算，避免"颜色发白" */
const s2l = (c) => (c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4));
const lin = (arr) => arr.map((c) => r3(s2l(c)));

/* ---------------- 确定性随机 ---------------- */
function mulberry32(a) {
  return function () {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/* ---------------- 曲线 ---------------- */
function catmullClosed(pts, subdiv) {
  const n = pts.length, out = [];
  for (let i = 0; i < n; ++i) {
    const p0 = pts[(i - 1 + n) % n], p1 = pts[i], p2 = pts[(i + 1) % n], p3 = pts[(i + 2) % n];
    for (let s = 0; s < subdiv; ++s) {
      const t = s / subdiv, t2 = t * t, t3 = t2 * t;
      out.push([
        0.5 * ((2 * p1[0]) + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3),
        0.5 * ((2 * p1[1]) + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3),
      ]);
    }
  }
  return out;
}

function resampleClosed(dense, step) {
  const n = dense.length;
  const seg = [];
  let total = 0;
  for (let i = 0; i < n; ++i) {
    const a = dense[i], b = dense[(i + 1) % n];
    const d = Math.hypot(b[0] - a[0], b[1] - a[1]);
    seg.push(d); total += d;
  }
  const count = Math.max(32, Math.round(total / step));
  const real = total / count;
  const out = [];
  let si = 0, acc = 0;
  for (let k = 0; k < count; ++k) {
    const target = k * real;
    while (acc + seg[si] < target) { acc += seg[si]; si = (si + 1) % n; }
    const t = seg[si] > 1e-6 ? (target - acc) / seg[si] : 0;
    const a = dense[si], b = dense[(si + 1) % n];
    out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]);
  }
  return { pts: out, len: total, step: real };
}

function buildFrames(sampled, elevation) {
  const { pts, len, step } = sampled;
  const n = pts.length;
  const nrm = [], tan = [], curv = [];
  for (let i = 0; i < n; ++i) {
    const a = pts[(i - 1 + n) % n], b = pts[(i + 1) % n];
    let tx = b[0] - a[0], tz = b[1] - a[1];
    const l = Math.hypot(tx, tz) || 1;
    tx /= l; tz /= l;
    tan.push([tx, tz]);
    nrm.push([-tz, tx]);              // 左手法线（+normal = 赛道左侧）
  }
  for (let i = 0; i < n; ++i) {
    const a = tan[(i - 1 + n) % n], b = tan[(i + 1) % n];
    let d = Math.atan2(b[1], b[0]) - Math.atan2(a[1], a[0]);
    while (d > Math.PI) d -= TAU;
    while (d < -Math.PI) d += TAU;
    curv.push(d / (2 * step));
  }
  // 高程：整数频率的正弦叠加 → 闭环、可微；生成器与运行时用同一份采样（f.h）
  const h = [];
  const el = elevation || { amp: 0, waves: [] };
  for (let i = 0; i < n; ++i) {
    let y = 0;
    for (const w of el.waves) {
      y += w[1] * Math.sin(TAU * w[0] * ((i * step) / len) + (w[2] || 0));
    }
    h.push(r3(y * (el.amp || 1)));
  }
  return { pts, nrm, tan, curv, len, step, n, h };
}

/* ---------------- 网格构造 ---------------- */
function newMesh() { return { pos: [], nrm: [], uv: [], idx: [] }; }
function addVert(m, p, n, uv) {
  m.pos.push(p[0], p[1], p[2]);
  m.nrm.push(n[0], n[1], n[2]);
  m.uv.push(uv ? uv[0] : 0, uv ? uv[1] : 0);
  return m.pos.length / 3 - 1;
}
function addTri(m, a, b, c, n, uva, uvb, uvc) {
  const ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
  const vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
  const cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
  const flip = cx * n[0] + cy * n[1] + cz * n[2] < 0;
  const i0 = addVert(m, a, n, uva), i1 = addVert(m, b, n, uvb), i2 = addVert(m, c, n, uvc);
  if (flip) m.idx.push(i0, i2, i1); else m.idx.push(i0, i1, i2);
}
function addQuadN(m, p0, p1, p2, p3, n0, n1, n2, n3, uv0, uv1, uv2, uv3) {
  // 逐顶点法线（高度场需要连续法线，否则每个 2m 段一条亮带）
  const avg = [(n0[0] + n2[0]) * 0.5, (n0[1] + n2[1]) * 0.5, (n0[2] + n2[2]) * 0.5];
  const ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
  const bx = p3[0] - p0[0], by = p3[1] - p0[1], bz = p3[2] - p0[2];
  const cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
  const flip = cx * avg[0] + cy * avg[1] + cz * avg[2] < 0;
  const i0 = addVert(m, p0, n0, uv0), i1 = addVert(m, p1, n1, uv1);
  const i2 = addVert(m, p2, n2, uv2), i3 = addVert(m, p3, n3, uv3);
  if (flip) { m.idx.push(i0, i2, i1, i0, i3, i2); }
  else { m.idx.push(i0, i1, i2, i0, i2, i3); }
}
function addQuad(m, a, b, c, d, n, uva, uvb, uvc, uvd) {
  addTri(m, a, b, c, n, uva, uvb, uvc);
  addTri(m, a, c, d, n, uva, uvc, uvd);
}

function offsetY(m, fromVert, dy) {
  for (let i = fromVert; i < m.pos.length / 3; ++i) m.pos[i * 3 + 1] += dy;
}

function rotY(p, yaw) {
  const c = Math.cos(yaw), s = Math.sin(yaw);
  return [p[0] * c + p[2] * s, p[1], -p[0] * s + p[2] * c];
}
const UV = (a, b) => [a / 4, b / 4];   // 4m 一个贴图周期
function addBox(m, cx, cy, cz, sx, sy, sz, yaw = 0) {
  const hx = sx / 2, hy = sy / 2, hz = sz / 2;
  const P = (x, y, z) => { const p = rotY([x, y, z], yaw); return [p[0] + cx, p[1] + cy, p[2] + cz]; };
  const N = (x, y, z) => rotY([x, y, z], yaw);
  addQuad(m, P(hx, -hy, hz), P(hx, hy, hz), P(hx, hy, -hz), P(hx, -hy, -hz), N(1, 0, 0),
    UV(hz, -hy), UV(hz, hy), UV(-hz, hy), UV(-hz, -hy));
  addQuad(m, P(-hx, -hy, -hz), P(-hx, hy, -hz), P(-hx, hy, hz), P(-hx, -hy, hz), N(-1, 0, 0),
    UV(-hz, -hy), UV(-hz, hy), UV(hz, hy), UV(hz, -hy));
  addQuad(m, P(-hx, hy, hz), P(-hx, hy, -hz), P(hx, hy, -hz), P(hx, hy, hz), N(0, 1, 0),
    UV(-hx, hz), UV(-hx, -hz), UV(hx, -hz), UV(hx, hz));
  addQuad(m, P(-hx, -hy, -hz), P(-hx, -hy, hz), P(hx, -hy, hz), P(hx, -hy, -hz), N(0, -1, 0),
    UV(-hx, -hz), UV(-hx, hz), UV(hx, hz), UV(hx, -hz));
  addQuad(m, P(-hx, -hy, hz), P(-hx, hy, hz), P(hx, hy, hz), P(hx, -hy, hz), N(0, 0, 1),
    UV(-hx, -hy), UV(-hx, hy), UV(hx, hy), UV(hx, -hy));
  addQuad(m, P(hx, -hy, -hz), P(hx, hy, -hz), P(-hx, hy, -hz), P(-hx, -hy, -hz), N(0, 0, -1),
    UV(hx, -hy), UV(hx, hy), UV(-hx, hy), UV(-hx, -hy));
}

function addCylY(m, cx, cy, cz, rad, h, seg) {
  for (let i = 0; i < seg; ++i) {
    const a0 = (i / seg) * TAU, a1 = ((i + 1) / seg) * TAU;
    const p0 = [cx + Math.cos(a0) * rad, cy, cz + Math.sin(a0) * rad];
    const p1 = [cx + Math.cos(a1) * rad, cy, cz + Math.sin(a1) * rad];
    const q0 = [p0[0], cy + h, p0[2]], q1 = [p1[0], cy + h, p1[2]];
    const n0 = [Math.cos(a0), 0, Math.sin(a0)], n1 = [Math.cos(a1), 0, Math.sin(a1)];
    const i0 = addVert(m, p0, n0), i1 = addVert(m, p1, n1), i2 = addVert(m, q1, n1), i3 = addVert(m, q0, n0);
    m.idx.push(i0, i1, i2, i0, i2, i3);
  }
}

function addConeY(m, cx, cy, cz, rad, h, seg) {
  const apex = [cx, cy + h, cz];
  for (let i = 0; i < seg; ++i) {
    const a0 = (i / seg) * TAU, a1 = ((i + 1) / seg) * TAU;
    const p0 = [cx + Math.cos(a0) * rad, cy, cz + Math.sin(a0) * rad];
    const p1 = [cx + Math.cos(a1) * rad, cy, cz + Math.sin(a1) * rad];
    const nx = Math.cos((a0 + a1) / 2), nz = Math.sin((a0 + a1) / 2);
    const n = [nx * 0.8, 0.6, nz * 0.8];
    addTri(m, p0, p1, apex, n);
    // 底面
    addTri(m, p1, p0, [cx, cy, cz], [0, -1, 0]);
  }
}

/* 轮子：轴沿 X 的圆柱（这样脚本只需绕 X 旋转即可滚动） */
function wheelMesh(rad, width, seg) {
  const m = newMesh();
  const hw = width / 2;
  for (let i = 0; i < seg; ++i) {
    const a0 = (i / seg) * TAU, a1 = ((i + 1) / seg) * TAU;
    const y0 = Math.cos(a0) * rad, z0 = Math.sin(a0) * rad;
    const y1 = Math.cos(a1) * rad, z1 = Math.sin(a1) * rad;
    const n0 = [0, Math.cos(a0), Math.sin(a0)], n1 = [0, Math.cos(a1), Math.sin(a1)];
    const v0 = addVert(m, [-hw, y0, z0], n0), v1 = addVert(m, [-hw, y1, z1], n1);
    const v2 = addVert(m, [hw, y1, z1], n1), v3 = addVert(m, [hw, y0, z0], n0);
    m.idx.push(v0, v1, v2, v0, v2, v3);
    // 侧盖（扇形）
    const c0 = addVert(m, [-hw, 0, 0], [-1, 0, 0]);
    const c1 = addVert(m, [hw, 0, 0], [1, 0, 0]);
    const l0 = addVert(m, [-hw, y0, z0], [-1, 0, 0]), l1 = addVert(m, [-hw, y1, z1], [-1, 0, 0]);
    m.idx.push(c0, l0, l1);
    const r0 = addVert(m, [hw, y0, z0], [1, 0, 0]), r1 = addVert(m, [hw, y1, z1], [1, 0, 0]);
    m.idx.push(c1, r1, r0);
  }
  return m;
}

/* ---------------- 网格序列化 ---------------- */
function writeMesh(file, name, mesh, material) {
  const n = mesh.pos.length / 3;
  const verts = [];
  let mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
  for (let i = 0; i < n; ++i) {
    const p = [r3(mesh.pos[i * 3]), r3(mesh.pos[i * 3 + 1]), r3(mesh.pos[i * 3 + 2])];
    const nr = [r3(mesh.nrm[i * 3]), r3(mesh.nrm[i * 3 + 1]), r3(mesh.nrm[i * 3 + 2])];
    verts.push([p[0], p[1], p[2], nr[0], nr[1], nr[2], 1.0, 0.0, 0.0, 1.0,
      r3(mesh.uv[i * 2] || 0), r3(mesh.uv[i * 2 + 1] || 0)]);
    for (let k = 0; k < 3; ++k) { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
  }
  const c = lin(material.color);
  const em = material.emissive ? lin(material.emissive) : [0, 0, 0];
  const doc = {
    bounds_max: mx.map(r3),
    bounds_min: mn.map(r3),
    indices: mesh.idx,
    materials: [{
      alpha_cutoff: 0.5, alpha_mode: 0,
      base_color: { a: 1.0, b: c[2], g: c[1], r: c[0] },
      double_sided: material.side === 2,
      emissive: { x: em[0], y: em[1], z: em[2] },
      metallic: material.metallic, name: material.name, roughness: material.roughness, side: material.side ?? 0,
      type: 'asset.PbrMaterial',
    }],
    name,
    vertex_layout: 0,
    vertex_stride: 48,
    vertices: verts,
  };
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(doc, null, 1).replace(/\n\s+(?=[-\d])/g, ' ') + '\n');
  return { verts: n, tris: mesh.idx.length / 3 };
}

/* ---------------- 赛道几何 ---------------- */
function bandQuad(m, f, i, offA, offB, y, colorNormal = [0, 1, 0], uvScale = 0) {
  const a = f.pts[i], b = f.pts[(i + 1) % f.n];
  const na = f.nrm[i], nb = f.nrm[(i + 1) % f.n];
  const ya = f.h ? f.h[i] : 0, yb = f.h ? f.h[(i + 1) % f.n] : 0;
  const p0 = [a[0] + na[0] * offA, y + ya, a[1] + na[1] * offA];
  const p1 = [a[0] + na[0] * offB, y + ya, a[1] + na[1] * offB];
  const p2 = [b[0] + nb[0] * offB, y + yb, b[1] + nb[1] * offB];
  const p3 = [b[0] + nb[0] * offA, y + yb, b[1] + nb[1] * offA];
  // 逐采样点法线：高度场 n = normalize(up - slope * tangent)，相邻段共用 → 无横向亮带
  const nAt = (k) => {
    const kp = (k - 1 + f.n) % f.n, kn = (k + 1) % f.n;
    const sl = (f.h[kn] - f.h[kp]) / (2 * f.step);
    const tg = [f.nrm[k][1], -f.nrm[k][0]];
    const L = Math.hypot(-sl * tg[0], 1, -sl * tg[1]) || 1;
    return [-sl * tg[0] / L, 1 / L, -sl * tg[1] / L];
  };
  const na2 = nAt(i), nb2 = nAt((i + 1) % f.n);
  if (uvScale > 0) {
    // u 横跨路宽（0..1，按 2.2m 一格铺），v 沿赛道（每 uvScale 米一轮）
    const uA = offA / uvScale, uB = offB / uvScale;   // 横跨方向的纹素密度与纵向一致
    const vA = (i * f.step) / uvScale, vB = ((i + 1) * f.step) / uvScale;
    addQuad(m, p0, p1, p2, p3, colorNormal,
      [uA, vA], [uB, vA], [uB, vB], [uA, vB]);
  } else {
    addQuadN(m, p0, p1, p2, p3, na2, na2, nb2, nb2, [0,0], [0,0], [0,0], [0,0]);
  }
}

function buildTrackMeshes(spec, f) {
  const half = spec.width / 2;
  const out = {};
  const M = {};

  // 路面
  M.road = newMesh();
  for (let i = 0; i < f.n; ++i) bandQuad(M.road, f, i, -half, half, 0.02, [0, 1, 0], 6.0);

  // 路肩（两侧）
  M.kerb = newMesh();
  for (let i = 0; i < f.n; ++i) {
    bandQuad(M.kerb, f, i, half, half + 0.55, 0.05, [0, 1, 0], 2.5);
    bandQuad(M.kerb, f, i, -half - 0.55, -half, 0.05, [0, 1, 0], 2.5);
  }

  // 中线虚线 + 起跑线
  M.line = newMesh();
  for (let i = 0; i < f.n; i += 3) {
    if ((i / 3) % 2 !== 0) continue;
    bandQuad(M.line, f, i, -0.18, 0.18, 0.04, [0, 1, 0], 4.0);
  }
  for (let k = 0; k < 2; ++k) bandQuad(M.line, f, (f.n - 1 + k) % f.n, -half, half, 0.045);

  // 护栏（内侧面 + 顶面）
  M.rail = newMesh();
  const railOff = half + 1.15, railH = 0.72, railT = 0.28;
  for (let i = 0; i < f.n; ++i) {
    const j = (i + 1) % f.n;
    for (const s of [1, -1]) {
      const a = f.pts[i], b = f.pts[j], na = f.nrm[i], nb = f.nrm[j];
      const ya = f.h[i], yb = f.h[j];
      const oa = s * railOff, ob = s * railOff;
      // 内侧竖面（法线朝赛道内 = -s*normal）
      const nIn = [-na[0] * s, 0, -na[1] * s];
      addQuad(M.rail,
        [a[0] + na[0] * oa, ya, a[1] + na[1] * oa],
        [a[0] + na[0] * oa, ya + railH, a[1] + na[1] * oa],
        [b[0] + nb[0] * ob, yb + railH, b[1] + nb[1] * ob],
        [b[0] + nb[0] * ob, yb, b[1] + nb[1] * ob], nIn);
      // 顶面
      addQuad(M.rail,
        [a[0] + na[0] * oa, ya + railH, a[1] + na[1] * oa],
        [a[0] + na[0] * (oa - s * railT), ya + railH, a[1] + na[1] * (oa - s * railT)],
        [b[0] + nb[0] * (ob - s * railT), yb + railH, b[1] + nb[1] * (ob - s * railT)],
        [b[0] + nb[0] * ob, yb + railH, b[1] + nb[1] * ob], [0, 1, 0]);
    }
  }

  // 地面：一律用跟随高程的宽条带（有起伏时铺大平面，路面会浮在空中）
  M.ground = newMesh();
  const gk = spec.ground ?? { kind: 'band', width: 160 };
  {
    // 条带宽度不能超过最小转弯半径，否则内侧会自我折叠（看到一条条亮带/摩尔纹）
    let maxK = 1e-4;
    for (const v of f.curv) maxK = Math.max(maxK, Math.abs(v));
    const bandHalf = Math.min(gk.width ?? 160, 0.72 / maxK);
    for (let i = 0; i < f.n; ++i) {
      bandQuad(M.ground, f, i, -bandHalf, bandHalf, -0.02, [0, 1, 0], 10.0);
    }
  }

  // 水（仅海滩）：大平面 + 岸边浪花条
  if (spec.water) {
    M.water = newMesh();
    const w = 1200, wy = spec.water.y ?? -0.45;
    addQuad(M.water, [-w, wy, -w], [w, wy, -w], [w, wy, w], [-w, wy, w], [0, 1, 0]);
    const foam = newMesh();
    for (let i = 0; i < f.n; i += 4) {
      const s = i % 8 === 0 ? 1 : -1;
      bandQuad(foam, f, i, s * (half + 2.2), s * (half + 3.4), wy + 0.02);
    }
    out.foam = foam;
  }

  // 道具
  const props = buildProps(spec, f);
  out.props_a = props.a;
  out.props_b = props.b;
  out.ao = props.ao;
  out.far = buildFarGround(f);

  // 路灯（夜景主题）：沿赛道等距、左右交替；灯头进 props_b（自发光），
  // 地面光斑单独一个 alpha 网格，避免用真实灯（8 盏预算不够铺满一条街）。
  if (spec.lamps) {
    const poleM = newMesh(), poolM = newMesh();
    out.lampPos = [];
    const step = spec.lamps.spacing ?? 26;
    const h = spec.lamps.height ?? 7.0;
    const n = Math.max(4, Math.floor(f.len / step));
    for (let i = 0; i < n; ++i) {
      const si = (Math.floor((i * step) / f.step)) % f.n;
      const side = (i % 2 === 0) ? 1 : -1;
      const off = half + 1.9;
      const gy = f.h[si];
      const bx = f.pts[si][0] + f.nrm[si][0] * off * side;
      const bz = f.pts[si][1] + f.nrm[si][1] * off * side;
      const inx = -f.nrm[si][0] * side, inz = -f.nrm[si][1] * side;   // 指向赛道内
      const yaw = Math.atan2(inx, inz);
      addBox(poleM, bx, gy + h * 0.5, bz, 0.18, h, 0.18, 0);
      addBox(poleM, bx + inx * 0.75, gy + h - 0.10, bz + inz * 0.75, 1.7, 0.14, 0.16, yaw);
      addBox(out.props_b, bx + inx * 1.55, gy + h - 0.28, bz + inz * 1.55, 0.85, 0.16, 0.42, yaw);
      out.lampPos.push([r3(bx + inx * 1.55), r3(gy + h - 0.35), r3(bz + inz * 1.55)]);
      // 地面光斑（暖白椭圆，贴在路面/地面上方一点）
      const cx = bx + inx * 2.6, cz = bz + inz * 2.6;
      const rx = 4.6, rz = 6.0, seg = 10, py = gy + 0.028;
      for (let k = 0; k < seg; ++k) {
        const a0 = (k / seg) * TAU, a1 = ((k + 1) / seg) * TAU;
        addTri(poolM, [cx, py, cz],
          [cx + Math.cos(a1) * rx, py, cz + Math.sin(a1) * rz],
          [cx + Math.cos(a0) * rx, py, cz + Math.sin(a0) * rz], [0, 1, 0]);
      }
    }
    out.pole = poleM;
    out.pool = poolM;
  }
  return Object.assign(out, M);
}

function buildFarGround(f) {
  const N = 40, S = 760;
  const m = newMesh();
  const near = (x, z) => {
    let best = 0, bd = 1e18;
    for (let i = 0; i < f.n; ++i) {
      const dx = x - f.pts[i][0], dz = z - f.pts[i][1];
      const d = dx * dx + dz * dz;
      if (d < bd) { bd = d; best = i; }
    }
    return f.h[best];
  };
  const grid = [];
  for (let gz = 0; gz <= N; ++gz) {
    grid.push([]);
    for (let gx = 0; gx <= N; ++gx) {
      const x = -S + (2 * S * gx) / N, z = -S + (2 * S * gz) / N;
      grid[gz].push([x, near(x, z) - 0.35, z]);
    }
  }
  for (let gz = 0; gz < N; ++gz) {
    for (let gx = 0; gx < N; ++gx) {
      const a = grid[gz][gx], b = grid[gz][gx + 1], c = grid[gz + 1][gx + 1], d = grid[gz + 1][gx];
      addQuad(m, a, b, c, d, [0, 1, 0],
        [a[0] / 8, a[2] / 8], [b[0] / 8, b[2] / 8], [c[0] / 8, c[2] / 8], [d[0] / 8, d[2] / 8]);
    }
  }
  return m;
}

function buildProps(spec, f) {
  const rnd = mulberry32(spec.seed);
  const A = newMesh(), B = newMesh(), AO = newMesh();

  /* 假 AO：没有实时阴影，就在每个道具脚下烘一块更暗的地面 —— 一个 draw 换回整片场景的"落地感" */
  function aoPatch(cx, cz, rx, rz) {
    const y = 0.004, seg = 8;
    const c = [cx, y, cz];
    for (let i = 0; i < seg; ++i) {
      const a0 = (i / seg) * Math.PI * 2, a1 = ((i + 1) / seg) * Math.PI * 2;
      addTri(AO, c,
        [cx + Math.cos(a1) * rx, y, cz + Math.sin(a1) * rz],
        [cx + Math.cos(a0) * rx, y, cz + Math.sin(a0) * rz], [0, 1, 0]);
    }
  }
  const sc = spec.scenery ?? { kind: 'desert', count: 100, band: [16, 90], scale: [0.8, 1.4] };
  const half = spec.width / 2;
  for (let i = 0; i < sc.count; ++i) {
    const s = rnd() * f.len;
    const si = Math.floor(s / f.step) % f.n;
    const gy = f.h[si];
    const vA = A.pos.length / 3, vB = B.pos.length / 3, vAO = AO.pos.length / 3;
    const side = rnd() < 0.5 ? -1 : 1;
    const off = half + sc.band[0] + rnd() * (sc.band[1] - sc.band[0]);
    const px = f.pts[si][0] + f.nrm[si][0] * off * side;
    const pz = f.pts[si][1] + f.nrm[si][1] * off * side;
    const yaw = rnd() * TAU;
    const k = sc.scale[0] + rnd() * (sc.scale[1] - sc.scale[0]);
    switch (sc.kind) {
      case 'desert': {
        if (rnd() < 0.55) {           // 仙人掌
          const h = 2.2 * k;
          addCylY(A, px, 0, pz, 0.28 * k, h, 6);
          addBox(A, px + 0.42 * k, h * 0.55, pz, 0.75 * k, 0.22 * k, 0.22 * k, yaw);
          addBox(A, px - 0.34 * k, h * 0.42, pz, 0.7 * k, 0.22 * k, 0.22 * k, yaw + 1.2);
          addCylY(A, px + 0.75 * k, h * 0.55, pz, 0.2 * k, 0.9 * k, 5);
          addCylY(A, px - 0.62 * k, h * 0.42, pz, 0.18 * k, 0.75 * k, 5);
          aoPatch(px, pz, 1.5 * k, 1.5 * k);
        } else {                      // 岩柱
          addBox(A, px, 0.5 * k, pz, 1.5 * k, 1.0 * k, 1.2 * k, yaw);
          addBox(A, px + 0.3 * k, 1.35 * k, pz, 0.9 * k, 0.9 * k, 0.8 * k, yaw + 0.6);
          aoPatch(px, pz, 1.8 * k, 1.6 * k);
        }
        break;
      }
      case 'beach': {
        if (rnd() < 0.7) {            // 棕榈：细高干 + 宽而扁的树冠
          const h = 3.6 * k;
          addCylY(A, px, 0, pz, 0.22 * k, h, 6);
          addConeY(A, px, h - 0.25, pz, 2.3 * k, 0.95 * k, 7);
          addConeY(A, px, h + 0.35, pz, 1.55 * k, 0.85 * k, 7);
          addConeY(A, px, h + 0.95, pz, 0.75 * k, 0.7 * k, 6);
          aoPatch(px, pz, 2.0 * k, 2.0 * k);
        } else {                      // 礁石
          addBox(A, px, 0.35 * k, pz, 1.8 * k, 0.7 * k, 1.4 * k, yaw);
          aoPatch(px, pz, 1.5 * k, 1.3 * k);
        }
        break;
      }
      case 'forest': {
        if (rnd() < 0.8) {            // 针叶树
          const h = 1.5 * k;
          addCylY(A, px, 0, pz, 0.2 * k, h, 5);
          addConeY(A, px, h, pz, 1.7 * k, 2.6 * k, 7);
          addConeY(A, px, h + 1.3 * k, pz, 1.25 * k, 2.3 * k, 7);
          addConeY(A, px, h + 2.6 * k, pz, 0.8 * k, 1.8 * k, 7);
          aoPatch(px, pz, 1.7 * k, 1.7 * k);
        } else {                      // 灌木
          addBox(A, px, 0.45 * k, pz, 1.4 * k, 0.9 * k, 1.2 * k, yaw);
          aoPatch(px, pz, 1.2 * k, 1.1 * k);
        }
        break;
      }
      case 'city': {
        if (rnd() < 0.8) {            // 楼体 + 发光窗带
          const w = (6 + rnd() * 7) * k, d = (6 + rnd() * 7) * k, h = (10 + rnd() * 34) * k;
          // 楼体footprint 大：按对角线外推，保证不压到赛道
          const pad = 6 + Math.max(w, d) * 0.65;
          const bx = px + f.nrm[si][0] * side * pad;
          const bz = pz + f.nrm[si][1] * side * pad;
          addBox(A, bx, h / 2, bz, w, h, d, 0);
          // 底部商业灯带 + 橱窗：夜里街道两侧的"基础光"，比只有高层窗带可信得多
          addBox(B, bx, 2.2, bz, w + 0.16, 1.1, d + 0.16, 0);
          addBox(B, bx, 4.9, bz, w + 0.14, 0.30, d + 0.14, 0);
          const bands = 2 + Math.floor(rnd() * 3);
          for (let b = 0; b < bands; ++b) {
            const by = 8 + rnd() * Math.max(2, h - 11);
            addBox(B, bx, by, bz, w + 0.12, 0.45, d + 0.12, 0);
            if (rnd() < 0.55) addBox(B, bx + (rnd() < 0.5 ? -1 : 1) * w * 0.22, by + 1.6, bz, w * 0.10, 2.6, d + 0.14, 0);
          }
          aoPatch(bx, bz, w * 0.78, d * 0.78);
        } else {                      // 路灯
          addBox(A, px, 3.2, pz, 0.18, 6.4, 0.18, 0);
          addBox(B, px, 6.5, pz, 0.9, 0.16, 0.9, 0);
          aoPatch(px, pz, 0.7 * k, 0.7 * k);
        }
        break;
      }
    }
    offsetY(A, vA, gy);
    offsetY(B, vB, gy);
    offsetY(AO, vAO, gy);
  }
  return { a: A, b: B, ao: AO };
}

/* ---------------- 场景 ---------------- */
function sceneText(spec, id) {
  const half = spec.width / 2;
  const nodes = [
    ['trk_road', 'road'], ['trk_ground', 'ground'], ['trk_kerb', 'kerb'], ['trk_line', 'line'],
    ['trk_rail', 'rail'], ['trk_props_a', 'props_a'], ['trk_props_b', 'props_b'], ['trk_ao', 'ao'],
  ];
  if (spec.water) nodes.push(['trk_water', 'water'], ['trk_foam', 'foam']);
  if (spec.lamps) nodes.push(['trk_pole', 'pole'], ['trk_pool', 'pool']);
  nodes.push(['trk_far', 'far']);
  const MAT = {
    road: { name: 'road', color: spec.palette.road, metallic: 0.0, roughness: 0.72, cast: 0, tex: 'assets/tex/asphalt.png' },
    ground: { name: 'ground', color: spec.palette.ground, metallic: 0.0, roughness: 0.94, cast: 0, tex: 'assets/tex/ground_' + spec.id + '.png' },
    kerb: { name: 'kerb', color: spec.palette.kerb, metallic: 0.0, roughness: 0.55, cast: 0, tex: 'assets/tex/ground_' + spec.id + '.png' },
    line: { name: 'line', color: spec.palette.line, metallic: 0.0, roughness: 0.45, emissive: spec.palette.emissive, emInt: 0.6, cast: 0 },
    rail: { name: 'rail', color: spec.palette.rail, metallic: 0.75, roughness: 0.28, cast: 1 },
    props_a: { name: 'props_a', color: spec.palette.propA, metallic: 0.0, roughness: 0.78, cast: 1,
      tex: spec.id === 'city' ? 'assets/tex/facade.png' : '',
      emisTex: spec.id === 'city' ? 'assets/tex/facade_lit.png' : '', emInt: spec.id === 'city' ? 1.6 : 1.0 },
    props_b: { name: 'props_b', color: spec.palette.propB, metallic: 0.25, roughness: 0.45, emissive: spec.palette.emissive, emInt: spec.palette.emissiveIntensity ?? 1.8, cast: 1 },
    ao: { name: 'ao', color: spec.palette.ground.map((v) => v * 0.62), metallic: 0.0, roughness: 1.0, cast: 0 },
    far: { name: 'far', color: spec.palette.ground, metallic: 0.0, roughness: 1.0, cast: 0, tex: 'assets/tex/ground_' + spec.id + '.png' },
    pole: { name: 'pole', color: [0.20, 0.21, 0.24], metallic: 0.65, roughness: 0.35, cast: 1 },
    pool: { name: 'pool', color: [1.0, 0.80, 0.46], metallic: 0.0, roughness: 1.0, cast: 0, alpha: 0.30 },
    water: { name: 'water', color: spec.palette.water ?? [0.1, 0.32, 0.42], metallic: 0.15, roughness: 0.06, cast: 0 },
    foam: { name: 'foam', color: spec.palette.foam ?? [0.9, 0.95, 0.96], metallic: 0.0, roughness: 0.5, cast: 0 },
  };
  const trackNodes = nodes.map(([name, file]) => {
    const mm = MAT[file];
    const lc = lin(mm.color);
    const em = lin(mm.emissive ?? [0, 0, 0]);
    return `    { "name": "${name}", "components": [
      { "type": "scene.Transform3D", "local": { "position": { "x": 0.0, "y": 0.0, "z": 0.0 } } },
      { "type": "render.MeshRenderer", "mesh_path": "assets/tracks/${id}/${file}.mesh.json", "cast_shadow": ${mm.cast}, "receive_shadow": true },
      { "type": "asset.PbrMaterial", "metallic": ${mm.metallic}, "roughness": ${mm.roughness}, "side": 0,
        "emissive_intensity": ${mm.emInt ?? 1.0}, "env_intensity": 1.0,${mm.alpha ? ` "alpha_mode": 2, "opacity": ${mm.alpha},` : ''}${mm.tex ? ` "base_color_texture": "${mm.tex}",` : ''}
        "base_color": { "r": ${lc[0]}, "g": ${lc[1]}, "b": ${lc[2]}, "a": 1.0 },
        "emissive": { "x": ${em[0]}, "y": ${em[1]}, "z": ${em[2]} }${mm.emisTex ? `, "emissive_texture": "${mm.emisTex}"` : ''} } ] }`;
  }).join(',\n');

  const camY = 3.6, camBack = 9.5;
  const p0 = spec.startPoint;
  const camX = r3(p0[0] - Math.sin(spec.startYaw) * camBack);
  const camZ = r3(p0[1] - Math.cos(spec.startYaw) * camBack);
  const camRotX = -0.14;
  const eL = lin(spec.env.color), eS = lin(spec.env.sky), eG = lin(spec.env.ground);
  const fL = lin(spec.fog.color), cL = lin(spec.clear), suL = lin(spec.sun.color);

  return `{
  "title": "race_${id}",
  "script": "themes/${id}.js",
  "width": 1280,
  "height": 720,
  "views": 1,
  "seed": ${spec.seed},
  "env_intensity": ${spec.env.intensity},
  "env_color": { "r": ${eL[0]}, "g": ${eL[1]}, "b": ${eL[2]}, "a": 1.0 },
  "env_sky_color": { "r": ${eS[0]}, "g": ${eS[1]}, "b": ${eS[2]}, "a": 1.0 },
  "env_ground_color": { "r": ${eG[0]}, "g": ${eG[1]}, "b": ${eG[2]}, "a": 1.0 },
  "env_highlight": ${spec.env.highlight ?? 1.0},
  "fog_mode": ${spec.fog.mode},
  "fog_color": { "r": ${fL[0]}, "g": ${fL[1]}, "b": ${fL[2]}, "a": 1.0 },
  "fog_near": ${spec.fog.near},
  "fog_far": ${spec.fog.far},
  "fog_density": ${spec.fog.density ?? 0.0},
  "env_texture": "assets/sky/${id}.png",
  "env_background": true,
  "msaa_samples": 4,
  "entities": [
    { "name": "cam", "components": [
      { "type": "scene.Transform3D", "local": {
          "position": { "x": ${camX}, "y": ${camY}, "z": ${camZ} },
          "rotation": { "x": ${camRotX}, "y": ${r3(-spec.startYaw)}, "z": 0.0 } } },
      { "type": "render.Camera3D", "fov_y": 62.0, "near": 0.3, "far": 700.0,
        "exposure": ${spec.exposure ?? 1.0},
        "orbit_controls": false, "clear_mode": 0,
        "clear_color": { "r": ${cL[0]}, "g": ${cL[1]}, "b": ${cL[2]}, "a": 1.0 } }
    ]},
    { "name": "sun", "components": [
      { "type": "scene.Transform3D", "local": { "rotation": { "x": ${spec.sun.rx}, "y": ${spec.sun.ry}, "z": 0.0 } } },
      { "type": "render.Light", "type": 0, "intensity": ${spec.sun.intensity},
        "color": { "r": ${suL[0]}, "g": ${suL[1]}, "b": ${suL[2]}, "a": 1.0 },
        "cast_shadow": true, "shadow_mode": 2, "shadow_distance": 90.0 }
    ]},
    { "name": "fill", "components": [
      { "type": "scene.Transform3D", "local": { "rotation": { "x": 0.5, "y": -2.2, "z": 0.0 } } },
      { "type": "render.Light", "type": 0, "intensity": ${spec.fill ?? 0.25},
        "color": { "r": 0.7, "g": 0.78, "b": 0.95, "a": 1.0 }, "cast_shadow": false }
    ]},
${trackNodes}
  ]
}
`;
}

/* ---------------- 主流程 ---------------- */
function loadSpecs() {
  const dir = path.join(ROOT, 'data', 'tracks');
  return fs.readdirSync(dir).filter((f) => f.endsWith('.track.json'))
    .sort()
    .map((f) => JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8')));
}

function ovalPoints(p) {
  const pts = [];
  const rad = p.radial ?? new Array(p.points).fill(1);
  for (let i = 0; i < p.points; ++i) {
    const a = (i / p.points) * TAU;
    const k = rad[i % rad.length];
    pts.push([p.rx * Math.cos(a) * k, p.rz * Math.sin(a) * k]);
  }
  return pts;
}

const specs = loadSpecs();
const mod = {};
const report = [];

for (const spec of specs) {
  const ctrl = spec.path.kind === 'oval' ? ovalPoints(spec.path) : spec.path.points;
  const dense = catmullClosed(ctrl, 24);
  const sampled = resampleClosed(dense, 2.0);
  const f = buildFrames(sampled, spec.elevation);

  spec.startPoint = f.pts[0];
  spec.startYaw = Math.atan2(f.tan[0][0], f.tan[0][1]);

  const meshes = buildTrackMeshes(spec, f);
  const pal = spec.palette;
  const MAT = {
    road: { name: 'road', color: pal.road, metallic: 0.0, roughness: 0.92 },
    ground: { name: 'ground', color: pal.ground, metallic: 0.0, roughness: 0.96 },
    kerb: { name: 'kerb', color: pal.kerb, metallic: 0.0, roughness: 0.75 },
    line: { name: 'line', color: pal.line, metallic: 0.0, roughness: 0.6, emissive: pal.emissive },
    rail: { name: 'rail', color: pal.rail, metallic: 0.55, roughness: 0.42 },
    props_a: { name: 'props_a', color: pal.propA, metallic: 0.0, roughness: 0.85 },
    props_b: { name: 'props_b', color: pal.propB, metallic: 0.25, roughness: 0.45, emissive: pal.emissive },
    ao: { name: 'ao', color: pal.ground.map((v) => v * 0.62), metallic: 0.0, roughness: 1.0 },
    far: { name: 'far', color: pal.ground, metallic: 0.0, roughness: 1.0 },
    pole: { name: 'pole', color: [0.20, 0.21, 0.24], metallic: 0.65, roughness: 0.35 },
    pool: { name: 'pool', color: [1.0, 0.82, 0.48], metallic: 0.0, roughness: 1.0 },
    water: { name: 'water', color: pal.water ?? [0.1, 0.32, 0.42], metallic: 0.1, roughness: 0.08 },
    foam: { name: 'foam', color: pal.foam ?? [0.9, 0.95, 0.96], metallic: 0.0, roughness: 0.5 },
  };

  let totalVerts = 0, totalTris = 0;
  const counts = {};
  for (const [key, mesh] of Object.entries(meshes)) {
    if (!mesh || !mesh.pos || mesh.pos.length === 0) continue;
    const st = writeMesh(path.join(ROOT, 'assets', 'tracks', spec.id, key + '.mesh.json'),
      `${spec.id}_${key}`, mesh, MAT[key]);
    counts[key] = `${st.verts}v/${st.tris}t`;
    totalVerts += st.verts; totalTris += st.tris;
  }

  fs.writeFileSync(path.join(ROOT, 'race_' + spec.id + '.scn'), sceneText(spec, spec.id));
  fs.mkdirSync(path.join(ROOT, 'themes'), { recursive: true });
  fs.writeFileSync(path.join(ROOT, 'themes', spec.id + '.js'),
    `import { boot } from '../scripts/race_scene.js';\n\nexport default function () {\n  return boot('${spec.id}');\n}\n`);

  mod[spec.id] = {
    id: spec.id, name: spec.name, laps: spec.laps, width: spec.width, half: spec.width / 2,
    len: r3(f.len), step: r3(f.step), n: f.n,
    pts: f.pts.map((p) => [r3(p[0]), r3(p[1])]),
    nrm: f.nrm.map((p) => [r3(p[0]), r3(p[1])]),
    curv: f.curv.map((v) => r3(v)),
    h: f.h.map((v) => r3(v)),
    start: { x: r3(spec.startPoint[0]), z: r3(spec.startPoint[1]), yaw: r3(spec.startYaw) },
    palette: spec.palette, sun: spec.sun, fog: spec.fog, env: spec.env, clear: spec.clear,
    scenery: spec.scenery, headlights: !!spec.headlights,
    lamps: (mod[spec.id] || {}).lamps || undefined,
    lamps: meshes.lampPos || null,
  };
  report.push(`${spec.id.padEnd(8)} len=${Math.round(f.len)}m step=${f.step.toFixed(2)} samples=${f.n} ` +
    `verts=${totalVerts} tris=${totalTris}  ${JSON.stringify(counts)}`);
}

const header = `/* 由 tools/gen_track.mjs 生成 —— 不要手改。
 * 重新生成：node tools/gen_track.mjs
 * pts/nrm/curv 与 assets/tracks/*.mesh.json 同源（同一份采样），改一处必须重跑生成器。 */
`;
writePNG(path.join(ROOT, 'assets', 'tex', 'asphalt.png'), 512, 512, makeAsphalt(512, { base: 0.92 }));
for (const sp of specs) {
  writePNG(path.join(ROOT, 'assets', 'sky', sp.id + '.png'), 1024, 512, makeSky(1024, 512, sp.sky));
  if (sp.id === 'city') {
    writePNG(path.join(ROOT, 'assets', 'tex', 'facade.png'), 512, 512, makeFacade(512, 7, 'albedo'));
    writePNG(path.join(ROOT, 'assets', 'tex', 'facade_lit.png'), 512, 512, makeFacade(512, 7, 'emissive'));
  }
  writePNG(path.join(ROOT, 'assets', 'tex', 'ground_' + sp.id + '.png'), 512, 512,
    makeGround(512, { color: sp.palette.ground, amount: 0.16 }));
}
fs.mkdirSync(path.join(ROOT, 'scripts', 'gen'), { recursive: true });
fs.writeFileSync(path.join(ROOT, 'scripts', 'gen', 'tracks.js'),
  header + 'export const TRACKS = ' + JSON.stringify(mod, null, 1) + ';\n');

// 共享网格：车头灯光斑（梯形，从车头 1.2m 铺到 20m）
const beamM = newMesh();
addQuad(beamM, [-1.4, 0, 1.2], [1.4, 0, 1.2], [5.4, 0, 20.0], [-5.4, 0, 20.0], [0, 1, 0]);
const beamStat = writeMesh(path.join(ROOT, 'assets', 'shared', 'beam.mesh.json'), 'beam', beamM,
  { name: 'beam', color: [1.0, 0.90, 0.66], metallic: 0.0, roughness: 1.0 });
console.log('beam: ' + beamStat.verts + 'v/' + beamStat.tris + 't');
// 共享网格：轮毂（比轮胎窄一点的圆盘，挂在轮子节点下，跟着一起转）
const rimM = wheelMesh(0.22, 0.30, 12);
writeMesh(path.join(ROOT, 'assets', 'shared', 'rim.mesh.json'), 'rim', rimM,
  { name: 'rim', color: [0.75, 0.77, 0.78], metallic: 0.85, roughness: 0.28 });

// 共享网格：轮子（轴沿 X）
const wm = wheelMesh(0.34, 0.28, 14);
const wheelStat = writeMesh(path.join(ROOT, 'assets', 'shared', 'wheel.mesh.json'), 'wheel', wm,
  { name: 'tire', color: [0.08, 0.08, 0.09], metallic: 0.0, roughness: 0.85 });

console.log(report.join('\n'));
console.log(`wheel: ${wheelStat.verts}v/${wheelStat.tris}t`);
console.log('ok');
