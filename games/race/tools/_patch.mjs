// 一次性补丁：几何侧（UV / 高程 / 地面条带 / 道具跟随高程）—— 按行号定位，避免整段匹配
import fs from 'node:fs';

const P = 'D:/Github/maque/games/race/tools/gen_track.mjs';
let t = fs.readFileSync(P, 'utf8');
const log = [];
function rep(name, from, to) {
  if (!t.includes(from)) { log.push('MISS ' + name); return; }
  t = t.split(from).join(to);
  log.push('ok   ' + name);
}

// ---- 逐行替换（单行，稳） ----
rep('kerb uv l', '    bandQuad(M.kerb, f, i, half, half + 0.55, 0.05);',
  '    bandQuad(M.kerb, f, i, half, half + 0.55, 0.05, [0, 1, 0], 2.5);');
rep('kerb uv r', '    bandQuad(M.kerb, f, i, -half - 0.55, -half, 0.05);',
  '    bandQuad(M.kerb, f, i, -half - 0.55, -half, 0.05, [0, 1, 0], 2.5);');
rep('line uv', '    bandQuad(M.line, f, i, -0.18, 0.18, 0.04);',
  '    bandQuad(M.line, f, i, -0.18, 0.18, 0.04, [0, 1, 0], 4.0);');

// ---- 地面：数组行，用 \n 拼接（真实换行） ----
const gFrom = [
  '  M.ground = newMesh();',
  "  const gk = spec.ground ?? { kind: 'plane', size: 900 };",
  "  if (gk.kind === 'band') {",
  '    const bandHalf = gk.width ?? 70;',
  '    for (let i = 0; i < f.n; ++i) {',
  '      bandQuad(M.ground, f, i, -bandHalf, bandHalf, -0.02);',
  '    }',
  '  } else {',
  '    const s = gk.size ?? 900;',
  '    addQuad(M.ground, [-s, -0.02, -s], [s, -0.02, -s], [s, -0.02, s], [-s, -0.02, s], [0, 1, 0]);',
  '  }',
].join('\n');
const gTo = [
  '  M.ground = newMesh();',
  "  const gk = spec.ground ?? { kind: 'band', width: 160 };",
  '  {',
  '    // 一律用跟随高程的条带：有起伏时铺大平面，路面会"浮"在空中',
  '    const bandHalf = gk.width ?? 160;',
  '    for (let i = 0; i < f.n; ++i) {',
  '      bandQuad(M.ground, f, i, -bandHalf, bandHalf, -0.02, [0, 1, 0], 10.0);',
  '    }',
  '  }',
].join('\n');
rep('ground band', gFrom, gTo);

// ---- 护栏：逐行替换 y ----
rep('rail ya', '      const oa = s * railOff, ob = s * railOff;',
  '      const ya = f.h[i], yb = f.h[j];\n      const oa = s * railOff, ob = s * railOff;');
rep('rail inner 1', '        [a[0] + na[0] * oa, 0.0, a[1] + na[1] * oa],',
  '        [a[0] + na[0] * oa, ya, a[1] + na[1] * oa],');
rep('rail inner 2', '        [a[0] + na[0] * oa, railH, a[1] + na[1] * oa],',
  '        [a[0] + na[0] * oa, ya + railH, a[1] + na[1] * oa],');
rep('rail inner 3', '        [b[0] + nb[0] * ob, railH, b[1] + nb[1] * ob],',
  '        [b[0] + nb[0] * ob, yb + railH, b[1] + nb[1] * ob],');
rep('rail inner 4', '        [b[0] + nb[0] * ob, 0.0, b[1] + nb[1] * ob], nIn);',
  '        [b[0] + nb[0] * ob, yb, b[1] + nb[1] * ob], nIn);');
rep('rail top 1', '        [a[0] + na[0] * (oa - s * railT), railH, a[1] + na[1] * (oa - s * railT)],',
  '        [a[0] + na[0] * (oa - s * railT), ya + railH, a[1] + na[1] * (oa - s * railT)],');
rep('rail top 2', '        [b[0] + nb[0] * (ob - s * railT), railH, b[1] + nb[1] * (ob - s * railT)],',
  '        [b[0] + nb[0] * (ob - s * railT), yb + railH, b[1] + nb[1] * (ob - s * railT)],');
rep('rail top 3', '        [b[0] + nb[0] * ob, railH, b[1] + nb[1] * ob], [0, 1, 0]);',
  '        [b[0] + nb[0] * ob, yb + railH, b[1] + nb[1] * ob], [0, 1, 0]);');

// ---- 道具：每个道具按采样点高程整体抬升 ----
rep('props start', '    const si = Math.floor(s / f.step) % f.n;',
  '    const si = Math.floor(s / f.step) % f.n;\n    const gy = f.h[si];\n    const vA = A.pos.length / 3, vB = B.pos.length / 3, vAO = AO.pos.length / 3;');
rep('props end', '  return { a: A, b: B, ao: AO };',
  '    offsetY(A, vA, gy);\n    offsetY(B, vB, gy);\n    offsetY(AO, vAO, gy);\n  }\n  return { a: A, b: B, ao: AO };');
// 上面把 return 前缩进断了：后处理修正（把多出来的一个 '}' 行删掉）
t = t.replace('    }\n    offsetY(A, vA, gy);\n    offsetY(B, vB, gy);\n    offsetY(AO, vAO, gy);\n  }\n  return { a: A, b: B, ao: AO };',
  '    }\n    offsetY(A, vA, gy);\n    offsetY(B, vB, gy);\n    offsetY(AO, vAO, gy);\n  }\n  return { a: A, b: B, ao: AO };');

// ---- 路灯 ----
rep('lamp gy', '      const bx = f.pts[si][0] + f.nrm[si][0] * off * side;',
  '      const gy = f.h[si];\n      const bx = f.pts[si][0] + f.nrm[si][0] * off * side;');
rep('lamp pole', '      addBox(poleM, bx, h * 0.5, bz, 0.18, h, 0.18, 0);',
  '      addBox(poleM, bx, gy + h * 0.5, bz, 0.18, h, 0.18, 0);');
rep('lamp arm', '      addBox(poleM, bx + inx * 0.75, h - 0.10, bz + inz * 0.75, 1.7, 0.14, 0.16, yaw);',
  '      addBox(poleM, bx + inx * 0.75, gy + h - 0.10, bz + inz * 0.75, 1.7, 0.14, 0.16, yaw);');
rep('lamp head', '      addBox(out.props_b, bx + inx * 1.55, h - 0.28, bz + inz * 1.55, 0.85, 0.16, 0.42, yaw);',
  '      addBox(out.props_b, bx + inx * 1.55, gy + h - 0.28, bz + inz * 1.55, 0.85, 0.16, 0.42, yaw);');

fs.writeFileSync(P, t);
console.log(log.join('\n'));
