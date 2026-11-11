// 夜景增强：更亮的天空盒 / 更密的路灯 / 楼体底部商业灯光
import fs from 'node:fs';

const G = 'D:/Github/maque/games/race/';
const log = [];

// ---------- 1) 城市数据 ----------
const p = G + 'data/tracks/city.track.json';
const j = JSON.parse(fs.readFileSync(p, 'utf8'));
j.sky = {
  zenith: [0.06, 0.06, 0.15], horizon: [0.38, 0.22, 0.46], ground: [0.07, 0.05, 0.10],
  sunEl: 6, sunAz: 250, sunSize: 0.010, sunGlow: 0.34, stars: 0.030, seed: 44,
};
j.sun.intensity = 1.15;
j.sun.rx = -0.30;
j.env.intensity = 0.90;
j.env.highlight = 0.9;
j.fill = 0.55;
j.exposure = 1.15;
j.fog = { mode: 1, color: [0.23, 0.17, 0.33], near: 55, far: 360, density: 0.0 };
j.lamps = { spacing: 18, height: 8.0 };
j.palette.emissiveIntensity = 2.6;
fs.writeFileSync(p, JSON.stringify(j, null, 2) + '\n');
log.push('city spec');

// ---------- 2) 生成器 ----------
const P = G + 'tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
function rep(n, a, b) { if (!t.includes(a)) { log.push('MISS ' + n); return; } t = t.split(a).join(b); log.push('ok   ' + n); }

// props_b / line 的自发光强度改为读调色板
rep('props_b emInt', "props_b: { name: 'props_b', color: spec.palette.propB, metallic: 0.25, roughness: 0.45, emissive: spec.palette.emissive, emInt: 1.8, cast: 1 },",
  "props_b: { name: 'props_b', color: spec.palette.propB, metallic: 0.25, roughness: 0.45, emissive: spec.palette.emissive, emInt: spec.palette.emissiveIntensity ?? 1.8, cast: 1 },");

// 光斑更大更亮
rep('pool size', '      const rx = 3.4, rz = 4.6, seg = 10, py = gy + 0.028;',
  '      const rx = 4.6, rz = 6.0, seg = 10, py = gy + 0.028;');
rep('pool alpha', "    pool: { name: 'pool', color: [1.0, 0.82, 0.48], metallic: 0.0, roughness: 1.0, cast: 0, alpha: 0.20 },",
  "    pool: { name: 'pool', color: [1.0, 0.80, 0.46], metallic: 0.0, roughness: 1.0, cast: 0, alpha: 0.30 },");

// 楼体：底部商业灯带 + 更多的窗带
rep('building bands',
  `          addBox(A, bx, h / 2, bz, w, h, d, 0);
          const bands = 1 + Math.floor(rnd() * 3);
          for (let b = 0; b < bands; ++b) {
            const by = 3 + rnd() * (h - 6);
            addBox(B, bx, by, bz, w + 0.12, 0.5, d + 0.12, 0);
          }`,
  `          addBox(A, bx, h / 2, bz, w, h, d, 0);
          // 底部商业灯带：夜里街道两侧的"基础光"
          addBox(B, bx, 2.6, bz, w + 0.16, 1.5, d + 0.16, 0);
          addBox(B, bx, 4.6, bz, w + 0.14, 0.35, d + 0.14, 0);
          const bands = 2 + Math.floor(rnd() * 3);
          for (let b = 0; b < bands; ++b) {
            const by = 7 + rnd() * (h - 10);
            addBox(B, bx, by, bz, w + 0.12, 0.45, d + 0.12, 0);
            if (rnd() < 0.5) addBox(B, bx + w * 0.26, by + 1.2, bz, w * 0.12, 2.2, d + 0.14, 0);
          }`);

fs.writeFileSync(P, t);
console.log(log.join('\n'));
