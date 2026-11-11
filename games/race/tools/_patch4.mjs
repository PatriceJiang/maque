import fs from 'node:fs';
const P = 'D:/Github/maque/games/race/tools/gen_tex.mjs';
let t = fs.readFileSync(P, 'utf8');
const log = [];
function rep(n, a, b) { if (!t.includes(a)) { log.push('MISS ' + n); return; } t = t.split(a).join(b); log.push('ok   ' + n); }

rep('asphalt base', 'const base = spec.base ?? 0.34;',
  'const base = spec.base ?? 0.88;   // 贴图乘在 base_color 上：贴近白色，只提供颗粒');
rep('ground luminance',
  '      buf[o] = Math.round(clamp01(c[0] * d * d2) * 255);\n      buf[o + 1] = Math.round(clamp01(c[1] * d * d2) * 255);\n      buf[o + 2] = Math.round(clamp01(c[2] * d * d2) * 255);',
  '      const lum = clamp01(d * d2 * 0.94);\n      buf[o] = Math.round(lum * 255);\n      buf[o + 1] = Math.round(lum * 255);\n      buf[o + 2] = Math.round(lum * 255);');
fs.writeFileSync(P, t);
console.log(log.join('\n'));
