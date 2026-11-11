// 一次性补丁：运行时高程（车 y / 坡度 / 相机跟随）
import fs from 'node:fs';
const P = 'D:/Github/maque/games/race/scripts/race_scene.js';
let t = fs.readFileSync(P, 'utf8');
const log = [];
function rep(name, from, to) {
  if (!t.includes(from)) { log.push('MISS ' + name); return; }
  t = t.split(from).join(to); log.push('ok   ' + name);
}

// 1) 车状态
rep('car state', "        x: 0, z: 0, yaw: 0, vAng: 0, v: 0, steer: 0, spin: 0, slip: 0, dt: 0.016,",
  "        x: 0, z: 0, y: 0, yaw: 0, pitch: 0, vAng: 0, v: 0, steer: 0, spin: 0, slip: 0, dt: 0.016,");

// 2) 坡度：上坡减速、下坡加速
rep('slope accel', "    const vmaxEff = d.vMax * (c.off ? TUNE.offSpeed : 1);",
  "    // 坡度：h 的差分即斜率，直接当重力分量\n    const slope = (T.h[(c.idx + 1) % T.n] - T.h[c.idx]) / T.step;\n    const vmaxEff = d.vMax * (c.off ? TUNE.offSpeed : 1);");
rep('slope into a', "    if (c.off) a -= 0.6;",
  "    if (c.off) a -= 0.6;\n    a -= 9.81 * slope;");

// 3) constrain：写入 y 与 pitch
rep('constrain y', "    c.off = Math.abs(lat) > T.half;",
  "    c.off = Math.abs(lat) > T.half;\n    c.y = T.h[i];\n    c.pitch = Math.atan2(T.h[(i + 1) % T.n] - T.h[i], T.step);");

// 4) 节点：带高度与俯仰
rep('sync pos', "    node_set_local_position(c.root, c.x, 0, c.z);\n    node_set_local_rotation(c.root, 0, c.yaw, 0);",
  "    node_set_local_position(c.root, c.x, c.y, c.z);\n    node_set_local_rotation(c.root, -c.pitch, c.yaw, 0);");

// 5) 相机跟随高度
rep('cam up', "    S.camPos[1] += (up - S.camPos[1]) * k;",
  "    S.camPos[1] += ((c.y + up) - S.camPos[1]) * k;");
rep('cam look', "    node_look_at(S.cam, c.x + fx * 9.0, 1.05, c.z + fz * 9.0);",
  "    node_look_at(S.cam, c.x + fx * 9.0, c.y + 1.05, c.z + fz * 9.0);");
rep('cam init', "    S.camPos = [S.player.x - Math.sin(S.player.yaw) * 8, 3.2, S.player.z - Math.cos(S.player.yaw) * 8];",
  "    S.camPos = [S.player.x - Math.sin(S.player.yaw) * 8, S.player.y + 3.2, S.player.z - Math.cos(S.player.yaw) * 8];");

// 6) 发车格也贴地
rep('grid y', "    car.prog = i * T.step;",
  "    car.prog = i * T.step;\n    car.y = T.h[i];");

fs.writeFileSync(P, t);
console.log(log.join('\n'));
