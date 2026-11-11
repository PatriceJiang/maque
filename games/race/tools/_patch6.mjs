// 修：建筑/道具浮空（地面条带被曲率夹窄，外面是虚空）+ 路灯位置导出（供运行时真点光）
import fs from 'node:fs';
const G = 'D:/Github/maque/games/race/';
const log = [];
const P = G + 'tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
function rep(n, a, b) { if (!t.includes(a)) { log.push('MISS ' + n); return; } t = t.split(a).join(b); log.push('ok   ' + n); }

// 1) 远处地面：粗网格，高度取最近赛道采样点 → 道具/楼体下面永远有地
rep('far ground fn', 'function buildProps(spec, f) {', `function buildFarGround(f) {
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

function buildProps(spec, f) {`);

rep('far ground use', '  out.ao = props.ao;', '  out.ao = props.ao;\n  out.far = buildFarGround(f);');

// 2) 节点 + 材质
rep('far node', "  if (spec.lamps) nodes.push(['trk_pole', 'pole'], ['trk_pool', 'pool']);",
  "  if (spec.lamps) nodes.push(['trk_pole', 'pole'], ['trk_pool', 'pool']);\n  nodes.push(['trk_far', 'far']);");
rep('far mat scene', "    ao: { name: 'ao', color: spec.palette.ground.map((v) => v * 0.62), metallic: 0.0, roughness: 1.0, cast: 0 },",
  "    ao: { name: 'ao', color: spec.palette.ground.map((v) => v * 0.62), metallic: 0.0, roughness: 1.0, cast: 0 },\n    far: { name: 'far', color: spec.palette.ground, metallic: 0.0, roughness: 1.0, cast: 0, tex: 'assets/tex/ground_' + spec.id + '.png' },");
rep('far mat main', "    ao: { name: 'ao', color: pal.ground.map((v) => v * 0.62), metallic: 0.0, roughness: 1.0 },",
  "    ao: { name: 'ao', color: pal.ground.map((v) => v * 0.62), metallic: 0.0, roughness: 1.0 },\n    far: { name: 'far', color: pal.ground, metallic: 0.0, roughness: 1.0 },");

rep('lamp pos module', '    scenery: spec.scenery, headlights: !!spec.headlights,', '    scenery: spec.scenery, headlights: !!spec.headlights,\n    lamps: meshes.lampPos || null,');

// 3) 路灯位置导出（运行时用真点光贴近玩家照明）
rep('lamp pos collect', '  if (spec.lamps) {\n    const poleM = newMesh(), poolM = newMesh();',
  '  if (spec.lamps) {\n    const poleM = newMesh(), poolM = newMesh();\n    out.lampPos = [];');
rep('lamp pos push', "      addBox(out.props_b, bx + inx * 1.55, gy + h - 0.28, bz + inz * 1.55, 0.85, 0.16, 0.42, yaw);",
  "      addBox(out.props_b, bx + inx * 1.55, gy + h - 0.28, bz + inz * 1.55, 0.85, 0.16, 0.42, yaw);\n      out.lampPos.push([r3(bx + inx * 1.55), r3(gy + h - 0.35), r3(bz + inz * 1.55)]);");
rep('lamp pos module', '    scenery: spec.scenery, headlights: !!spec.headlights,',
  '    scenery: spec.scenery, headlights: !!spec.headlights,\n    lamps: (mod[spec.id] || {}).lamps || undefined,');

fs.writeFileSync(P, t);
console.log(log.join('\n'));
