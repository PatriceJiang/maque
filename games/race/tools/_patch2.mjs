// 一次性补丁：给四条赛道加 sky / elevation / exposure，并把贴图与天空盒接进生成器
import fs from 'node:fs';

const G = 'D:/Github/maque/games/race/';
const log = [];

// ---------- 1) 赛道数据 ----------
const skies = {
  desert: { zenith: [0.18, 0.40, 0.78], horizon: [0.86, 0.80, 0.66], ground: [0.55, 0.44, 0.28],
    sunEl: 24, sunAz: 150, sunSize: 0.02, sunGlow: 0.50, clouds: 0.12, seed: 11 },
  beach: { zenith: [0.14, 0.44, 0.90], horizon: [0.82, 0.90, 0.96], ground: [0.30, 0.45, 0.50],
    sunEl: 10, sunAz: 200, sunSize: 0.02, sunGlow: 0.70, clouds: 0.30, seed: 22 },
  forest: { zenith: [0.40, 0.50, 0.46], horizon: [0.74, 0.80, 0.76], ground: [0.26, 0.32, 0.26],
    sunEl: 34, sunAz: 120, sunSize: 0.015, sunGlow: 0.18, clouds: 0.55, seed: 33 },
  city: { zenith: [0.02, 0.02, 0.06], horizon: [0.20, 0.13, 0.28], ground: [0.04, 0.03, 0.07],
    sunEl: 5, sunAz: 250, sunSize: 0.006, sunGlow: 0.22, stars: 0.05, seed: 44 },
};
const elev = {
  desert: { amp: 5.0, waves: [[2, 0.55], [3, 0.30, -1.2], [5, 0.15]] },
  beach: { amp: 2.2, waves: [[2, 0.60], [3, 0.40, 0.8]] },
  forest: { amp: 3.4, waves: [[3, 0.50], [5, 0.30, 2.0]] },
  city: { amp: 0, waves: [] },
};
for (const id of Object.keys(skies)) {
  const p = G + 'data/tracks/' + id + '.track.json';
  const j = JSON.parse(fs.readFileSync(p, 'utf8'));
  j.sky = skies[id];
  j.elevation = elev[id];
  fs.writeFileSync(p, JSON.stringify(j, null, 2) + '\n');
  log.push('spec ' + id + ' sky+elevation');
}

// ---------- 2) 生成器：贴图 + 天空盒 ----------
const P = G + 'tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
function rep(name, from, to) {
  if (!t.includes(from)) { log.push('MISS ' + name); return; }
  t = t.split(from).join(to); log.push('ok   ' + name);
}

rep('import tex', "import url from 'node:url';",
  "import url from 'node:url';\nimport { writePNG, makeSky, makeAsphalt, makeGround } from './gen_tex.mjs';");

// 材质挂贴图
rep('road tex', "    road: { name: 'road', color: spec.palette.road, metallic: 0.0, roughness: 0.76, cast: 0 },",
  "    road: { name: 'road', color: spec.palette.road, metallic: 0.0, roughness: 0.72, cast: 0, tex: 'assets/tex/asphalt.png' },");
rep('ground tex', "    ground: { name: 'ground', color: spec.palette.ground, metallic: 0.0, roughness: 0.94, cast: 0 },",
  "    ground: { name: 'ground', color: spec.palette.ground, metallic: 0.0, roughness: 0.94, cast: 0, tex: 'assets/tex/ground_' + spec.id + '.png' },");
rep('kerb tex', "    kerb: { name: 'kerb', color: spec.palette.kerb, metallic: 0.0, roughness: 0.55, cast: 0 },",
  "    kerb: { name: 'kerb', color: spec.palette.kerb, metallic: 0.0, roughness: 0.55, cast: 0, tex: 'assets/tex/ground_' + spec.id + '.png' },");

// 场景：天空盒
rep('clear mode', '        "orbit_controls": false, "clear_mode": 1,',
  '        "orbit_controls": false, "clear_mode": 0,');
rep('env texture', '  "msaa_samples": 4,',
  '  "env_texture": "assets/sky/' + '${id}' + '.png",\n  "env_background": true,\n  "msaa_samples": 4,');

// 材质里输出贴图字段
rep('mat tex field', '"env_intensity": 1.0,${mm.alpha ? ` "alpha_mode": 2, "opacity": ${mm.alpha},` : \'\'}',
  '"env_intensity": 1.0,${mm.alpha ? ` "alpha_mode": 2, "opacity": ${mm.alpha},` : \'\'}${mm.tex ? ` "base_color_texture": "${mm.tex}",` : \'\'}');

// 主循环里写贴图（在写 tracks.js 之前）
rep('write textures', "fs.mkdirSync(path.join(ROOT, 'scripts', 'gen'), { recursive: true });",
  "writePNG(path.join(ROOT, 'assets', 'tex', 'asphalt.png'), 512, 512, makeAsphalt(512, { base: 0.36 }));\n" +
  "for (const sp of specs) {\n" +
  "  writePNG(path.join(ROOT, 'assets', 'sky', sp.id + '.png'), 1024, 512, makeSky(1024, 512, sp.sky));\n" +
  "  writePNG(path.join(ROOT, 'assets', 'tex', 'ground_' + sp.id + '.png'), 512, 512,\n" +
  "    makeGround(512, { color: sp.palette.ground, amount: 0.16 }));\n" +
  "}\n" +
  "fs.mkdirSync(path.join(ROOT, 'scripts', 'gen'), { recursive: true });");

fs.writeFileSync(P, t);
console.log(log.join('\n'));
