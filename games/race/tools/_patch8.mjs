// 建筑贴图：盒体加 UV + 程序化立面（窗格）
import fs from 'node:fs';
const G = 'D:/Github/maque/games/race/';
const log = [];

// 1) gen_tex: 立面贴图
const TXT = G + 'tools/gen_tex.mjs';
let g = fs.readFileSync(TXT, 'utf8');
if (!g.includes('export function makeFacade')) {
  g += `
/* ---------------- 楼体立面（窗格，无缝） ---------------- */
export function makeFacade(size, seed) {
  const buf = Buffer.alloc(size * size * 4);
  const cols = 8, rows = 10;             // 8 列 × 10 层，一贴图 = 8m × 8m
  const cw = size / cols, ch = size / rows;
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const cx = Math.floor(x / cw), cy = Math.floor(y / ch);
      const gx = x - cx * cw, gy = y - cy * ch;
      // 楼层横带
      const band = gy < ch * 0.18;
      let r = 0.16, gg = 0.16, b = 0.19;
      if (band) { r = 0.10; gg = 0.10; b = 0.12; }
      // 窗：每格中间 62% × 55%
      const inWinX = gx > cw * 0.19 && gx < cw * 0.81;
      const inWinY = gy > ch * 0.30 && gy < ch * 0.85;
      if (!band && inWinX && inWinY) {
        const h = hash2(cx, cy, seed);
        if (h > 0.42) {                    // 亮窗
          const w = 0.65 + ((h * 977) % 1) * 0.35;
          r = 1.0 * w; gg = 0.82 * w; b = 0.55 * w;
        } else {                           // 暗窗（玻璃反一点夜色）
          r = 0.10; gg = 0.11; b = 0.16;
        }
      }
      const o = (y * size + x) * 4;
      buf[o] = Math.round(clamp01(r) * 255);
      buf[o + 1] = Math.round(clamp01(gg) * 255);
      buf[o + 2] = Math.round(clamp01(b) * 255);
      buf[o + 3] = 255;
    }
  }
  return buf;
}
`;
  fs.writeFileSync(TXT, g);
  log.push('facade tex fn');
}

// 2) gen_track: addBox 带 UV
const P = G + 'tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
if (!t.includes('const UV = (a, b) => [a / 4, b / 4];')) {
  t = t.replace('function addBox(m, cx, cy, cz, sx, sy, sz, yaw = 0) {',
    'const UV = (a, b) => [a / 4, b / 4];   // 4m 一个贴图周期\nfunction addBox(m, cx, cy, cz, sx, sy, sz, yaw = 0) {');
  log.push('uv helper');
}
// 六个面补 uv（按局部坐标 → 贴图 4m 一格）
t = t.replace(
  'addQuad(m, P(hx, -hy, hz), P(hx, hy, hz), P(hx, hy, -hz), P(hx, -hy, -hz), N(1, 0, 0));',
  'addQuad(m, P(hx, -hy, hz), P(hx, hy, hz), P(hx, hy, -hz), P(hx, -hy, -hz), N(1, 0, 0),\n    UV(hz, -hy), UV(hz, hy), UV(-hz, hy), UV(-hz, -hy));');
t = t.replace(
  'addQuad(m, P(-hx, -hy, -hz), P(-hx, hy, -hz), P(-hx, hy, hz), P(-hx, -hy, hz), N(-1, 0, 0));',
  'addQuad(m, P(-hx, -hy, -hz), P(-hx, hy, -hz), P(-hx, hy, hz), P(-hx, -hy, hz), N(-1, 0, 0),\n    UV(-hz, -hy), UV(-hz, hy), UV(hz, hy), UV(hz, -hy));');
t = t.replace(
  'addQuad(m, P(-hx, hy, hz), P(-hx, hy, -hz), P(hx, hy, -hz), P(hx, hy, hz), N(0, 1, 0));',
  'addQuad(m, P(-hx, hy, hz), P(-hx, hy, -hz), P(hx, hy, -hz), P(hx, hy, hz), N(0, 1, 0),\n    UV(-hx, hz), UV(-hx, -hz), UV(hx, -hz), UV(hx, hz));');
t = t.replace(
  'addQuad(m, P(-hx, -hy, -hz), P(-hx, -hy, hz), P(hx, -hy, hz), P(hx, -hy, -hz), N(0, -1, 0));',
  'addQuad(m, P(-hx, -hy, -hz), P(-hx, -hy, hz), P(hx, -hy, hz), P(hx, -hy, -hz), N(0, -1, 0),\n    UV(-hx, -hz), UV(-hx, hz), UV(hx, hz), UV(hx, -hz));');
t = t.replace(
  'addQuad(m, P(-hx, -hy, hz), P(-hx, hy, hz), P(hx, hy, hz), P(hx, -hy, hz), N(0, 0, 1));',
  'addQuad(m, P(-hx, -hy, hz), P(-hx, hy, hz), P(hx, hy, hz), P(hx, -hy, hz), N(0, 0, 1),\n    UV(-hx, -hy), UV(-hx, hy), UV(hx, hy), UV(hx, -hy));');
t = t.replace(
  'addQuad(m, P(hx, -hy, -hz), P(hx, hy, -hz), P(-hx, hy, -hz), P(-hx, -hy, -hz), N(0, 0, -1));',
  'addQuad(m, P(hx, -hy, -hz), P(hx, hy, -hz), P(-hx, hy, -hz), P(-hx, -hy, -hz), N(0, 0, -1),\n    UV(hx, -hy), UV(hx, hy), UV(-hx, hy), UV(-hx, -hy));');
log.push('box uv');

// 3) city 楼体材质挂立面贴图
t = t.replace("props_a: { name: 'props_a', color: spec.palette.propA, metallic: 0.0, roughness: 0.78, cast: 1 },",
  "props_a: { name: 'props_a', color: spec.palette.propA, metallic: 0.0, roughness: 0.78, cast: 1, tex: spec.id === 'city' ? 'assets/tex/facade.png' : '' },");

// 4) 写贴图
t = t.replace("  writePNG(path.join(ROOT, 'assets', 'tex', 'ground_' + sp.id + '.png'), 512, 512,",
  "  if (sp.id === 'city') writePNG(path.join(ROOT, 'assets', 'tex', 'facade.png'), 512, 512, makeFacade(512, 7));\n  writePNG(path.join(ROOT, 'assets', 'tex', 'ground_' + sp.id + '.png'), 512, 512,");
t = t.replace("import { writePNG, makeSky, makeAsphalt, makeGround } from './gen_tex.mjs';",
  "import { writePNG, makeSky, makeAsphalt, makeGround, makeFacade } from './gen_tex.mjs';");

fs.writeFileSync(P, t);
console.log(log.join('\n'));
