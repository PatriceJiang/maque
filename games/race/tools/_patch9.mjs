// 夜景全局提亮：立面双贴图（漫反射 + 自发光窗）、调色板提亮、环境光加大
import fs from 'node:fs';
const G = 'D:/Github/maque/games/race/';
const log = [];

// ---------- 1) 立面贴图拆成两张 ----------
const T = G + 'tools/gen_tex.mjs';
let g = fs.readFileSync(T, 'utf8');
const start = g.indexOf('/* ---------------- 楼体立面');
if (start >= 0) g = g.slice(0, start);
g += `
/* ---------------- 楼体立面（两张：漫反射 + 自发光窗） ----------------
 * 漫反射要"亮"：它乘在 base_color 上，太暗会把整栋楼吃成黑块。
 * 自发光只保留亮窗：给 emissive_texture，夜里窗户真的发光。 */
export function makeFacade(size, seed, mode) {
  const buf = Buffer.alloc(size * size * 4);
  const cols = 8, rows = 10;
  const cw = size / cols, ch = size / rows;
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const cx = Math.floor(x / cw), cy = Math.floor(y / ch);
      const gx = x - cx * cw, gy = y - cy * ch;
      const band = gy < ch * 0.16;
      const inWinX = gx > cw * 0.2 && gx < cw * 0.8;
      const inWinY = gy > ch * 0.3 && gy < ch * 0.84;
      const isWin = !band && inWinX && inWinY;
      const lit = isWin && hash2(cx, cy, seed) > 0.35;
      let r, gg, b;
      if (mode === 'emissive') {
        r = lit ? 1.0 : 0.0; gg = lit ? 0.78 : 0.0; b = lit ? 0.45 : 0.0;
      } else {
        if (band) { r = 0.62; gg = 0.62; b = 0.66; }          // 楼层线
        else if (isWin) { r = lit ? 0.9 : 0.42; gg = lit ? 0.85 : 0.46; b = lit ? 0.78 : 0.56; }
        else { r = 0.78; gg = 0.78; b = 0.82; }                // 墙
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
log.push('facade 双贴图');

// ---------- 2) 生成器：写入两张 + 材质带 emissive_texture ----------
const P = G + 'tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
function rep(n, a, b) { if (!t.includes(a)) { log.push('MISS ' + n); return; } t = t.split(a).join(b); log.push('ok   ' + n); }

rep('facade albedo', "if (sp.id === 'city') writePNG(path.join(ROOT, 'assets', 'tex', 'facade.png'), 512, 512, makeFacade(512, 7));",
  "if (sp.id === 'city') {\n    writePNG(path.join(ROOT, 'assets', 'tex', 'facade.png'), 512, 512, makeFacade(512, 7, 'albedo'));\n    writePNG(path.join(ROOT, 'assets', 'tex', 'facade_lit.png'), 512, 512, makeFacade(512, 7, 'emissive'));\n  }");

rep('props_a mat', "props_a: { name: 'props_a', color: spec.palette.propA, metallic: 0.0, roughness: 0.78, cast: 1, tex: spec.id === 'city' ? 'assets/tex/facade.png' : '' },",
  "props_a: { name: 'props_a', color: spec.palette.propA, metallic: 0.0, roughness: 0.78, cast: 1,\n      tex: spec.id === 'city' ? 'assets/tex/facade.png' : '',\n      emisTex: spec.id === 'city' ? 'assets/tex/facade_lit.png' : '', emInt: spec.id === 'city' ? 1.6 : 1.0 },");

rep('node emissive tex', '"emissive": { "x": ${em[0]}, "y": ${em[1]}, "z": ${em[2]} } } ] }`;',
  '"emissive": { "x": ${em[0]}, "y": ${em[1]}, "z": ${em[2]} }${mm.emisTex ? `, "emissive_texture": "${mm.emisTex}"` : \'\'} } ] }`;');

fs.writeFileSync(P, t);
console.log(log.join('\n'));
