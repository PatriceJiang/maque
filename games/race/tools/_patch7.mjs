// 运行时：城市路灯真点光（两盏，跟随玩家最近的灯位）
import fs from 'node:fs';
const P = 'D:/Github/maque/games/race/scripts/race_scene.js';
let t = fs.readFileSync(P, 'utf8');
const log = [];
function rep(n, a, b) { if (!t.includes(a)) { log.push('MISS ' + n); return; } t = t.split(a).join(b); log.push('ok   ' + n); }

rep('lamp light fn', '  function on_start() {', `  /* 路灯真点光：整条街的灯不可能都开（8 盏上限），所以只开两盏，
   * 每帧把它们挪到离玩家最近的两个灯位 —— 玩家身边永远是"有灯照亮的路"。 */
  function addLampLight() {
    const id = entity_spawn();
    node_add_component(id, 'render.Light');
    node_set_number(id, 'render.Light', 'type', 1);        // Point
    node_set_number(id, 'render.Light', 'intensity', 30.0);
    node_set_number(id, 'render.Light', 'range', 24.0);
    node_set_number(id, 'render.Light', 'shadow_mode', 0);
    node_set_number(id, 'render.Light', 'color.r', 1.0);
    node_set_number(id, 'render.Light', 'color.g', 0.80);
    node_set_number(id, 'render.Light', 'color.b', 0.52);
    return id;
  }

  function on_start() {`);

rep('lamp light spawn', '    if (T.headlights) {',
  '    if (T.lamps && T.lamps.length) {\n      S.lampLights = [addLampLight(), addLampLight()];\n    }\n    if (T.headlights) {');

rep('lamp light update', '    updateCamera(dt);',
  `    // 两盏点光贴在最近的灯位上
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
    updateCamera(dt);`);

fs.writeFileSync(P, t);
console.log(log.join('\n'));
