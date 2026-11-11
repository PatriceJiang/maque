// 对齐 threejs-impl 的材质规格：车漆/玻璃/轮胎/轮毂/灯 + 更密的窗格贴图
import fs from 'node:fs';
const G = 'D:/Github/maque/games/race/';
const log = [];

// ---------- 1) cars.js：补 rim 颜色（参考实现的 colors.rim） ----------
const CP = G + 'scripts/data/cars.js';
let c = fs.readFileSync(CP, 'utf8');
const rims = {
  viper: '#b8bcc0', comet: '#c9d1d4', bandit: '#8f9296', dune: '#3a3f44',
  kite: '#d5d8cf', hound: '#2c3339', nova: '#9aa7b0', brute: '#6b7076',
};
if (!c.includes('rim:')) {
  for (const [id, hex] of Object.entries(rims)) {
    const re = new RegExp("(id: '" + id + "'[\\s\\S]*?accent: \\[[^\\]]+\\],)");
    c = c.replace(re, '$1\n    rim: ' + JSON.stringify(hex) + ',');
  }
  fs.writeFileSync(CP, c);
  log.push('cars rim color');
}

// ---------- 2) 窗格贴图：暖/冷两色窗、更高密度 ----------
const T = G + 'tools/gen_tex.mjs';
let g = fs.readFileSync(T, 'utf8');
const s = g.indexOf('/* ---------------- 楼体立面');
if (s >= 0) g = g.slice(0, s);
g += `
/* ---------------- 楼体立面（漫反射 + 自发光，暖/冷窗混排） ---------------- */
const WIN_WARM = [1.0, 0.84, 0.55];
const WIN_COOL = [0.55, 0.85, 1.0];
export function makeFacade(size, seed, mode) {
  const buf = Buffer.alloc(size * size * 4);
  const cols = 14, rows = 18;
  const cw = size / cols, ch = size / rows;
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const cx = Math.floor(x / cw), cy = Math.floor(y / ch);
      const gx = x - cx * cw, gy = y - cy * ch;
      const band = gy < ch * 0.14;
      const inWin = !band && gx > cw * 0.22 && gx < cw * 0.78 && gy > ch * 0.28 && gy < ch * 0.86;
      const h = hash2(cx, cy, seed);
      const lit = inWin && h > 0.42;
      const cool = h > 0.78;
      const wc = cool ? WIN_COOL : WIN_WARM;
      const bright = 0.7 + ((h * 977) % 1) * 0.3;
      let r, gg, b;
      if (mode === 'emissive') {
        r = lit ? wc[0] * bright : 0;
        gg = lit ? wc[1] * bright : 0;
        b = lit ? wc[2] * bright : 0;
      } else if (band) {
        r = 0.30; gg = 0.30; b = 0.34;
      } else if (inWin) {
        r = lit ? 0.86 : 0.26; gg = lit ? 0.84 : 0.30; b = lit ? 0.82 : 0.38;
      } else {
        r = 0.34; gg = 0.35; b = 0.40;
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
fs.writeFileSync(T, g);
log.push('facade 窗格 14x18 暖冷混排');

// ---------- 3) 轮毂网格 ----------
const P = G + 'tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
if (!t.includes("'rim.mesh.json'")) {
  t = t.replace("// 共享网格：轮子（轴沿 X）",
    `// 共享网格：轮毂（比轮胎窄一点的圆盘，挂在轮子节点下，跟着一起转）
const rimM = wheelMesh(0.22, 0.30, 12);
writeMesh(path.join(ROOT, 'assets', 'shared', 'rim.mesh.json'), 'rim', rimM,
  { name: 'rim', color: [0.75, 0.77, 0.78], metallic: 0.85, roughness: 0.28 });

// 共享网格：轮子（轴沿 X）`);
  fs.writeFileSync(P, t);
  log.push('rim mesh');
}
console.log(log.join('\n'));
