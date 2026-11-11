// 车辆材质对齐 threejs-impl：paint/accent/glass/tire/rim/lights + 轮毂节点
import fs from 'node:fs';
const P = 'D:/Github/maque/games/race/scripts/race_scene.js';
let t = fs.readFileSync(P, 'utf8');
const log = [];
function rep(n, a, b) { if (!t.includes(a)) { log.push('MISS ' + n); return; } t = t.split(a).join(b); log.push('ok   ' + n); }

// addWheel 增加轮毂子节点
rep('addWheel rim', `  function addWheel(parent, x, y, z, rad, wid) {
    const id = entity_spawn();
    node_set_parent(id, parent);
    node_set_local_position(id, x, y, z);
    node_add_component(id, 'render.MeshRenderer');
    node_set_string(id, 'render.MeshRenderer', 'mesh_path', 'assets/shared/wheel.mesh.json');
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', 0);
    node_set_local_scale(id, wid / 0.28, rad / 0.34, rad / 0.34);
    setMat(id, [0.035, 0.035, 0.04], 0.35, 0.42);   // 轮胎+轮毂折中：低金属、半哑光，边缘有高光
    return id;
  }`,
`  function addWheel(parent, x, y, z, rad, wid, rimHex) {
    const id = entity_spawn();
    node_set_parent(id, parent);
    node_set_local_position(id, x, y, z);
    node_add_component(id, 'render.MeshRenderer');
    node_set_string(id, 'render.MeshRenderer', 'mesh_path', 'assets/shared/wheel.mesh.json');
    node_set_number(id, 'render.MeshRenderer', 'cast_shadow', 0);
    node_set_local_scale(id, wid / 0.28, rad / 0.34, rad / 0.34);
    setMat(id, srgb3([0.067, 0.071, 0.078]), 0.02, 0.92);   // 轮胎：橡胶
    // 轮毂：子节点，跟着父节点一起转
    const rim = entity_spawn();
    node_set_parent(rim, id);
    node_set_local_position(rim, 0, 0, 0);
    node_add_component(rim, 'render.MeshRenderer');
    node_set_string(rim, 'render.MeshRenderer', 'mesh_path', 'assets/shared/rim.mesh.json');
    node_set_number(rim, 'render.MeshRenderer', 'cast_shadow', 0);
    setMat(rim, rimHex ? hex3(rimHex) : srgb3([0.75, 0.77, 0.78]), 0.85, 0.28);
    return id;
  }`);

// hex 解析工具
rep('hex3', "  function setMat(id, rgb, metallic, rough, emissive, emInt, clearcoat) {",
  `  function hex3(hex) {
    const v = parseInt(hex.slice(1), 16);
    return srgb3([((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255]);
  }

  function setMat(id, rgb, metallic, rough, emissive, emInt, clearcoat) {`);

// 车身材质：paint（clearcoat 0.72 / ccRough 0.22）
rep('paint', `    const bodyId = addBox(root, 0, bodyY, 0, data.wid, bodyH, data.len, bodyCol, 0.45, 0.26, true, 1.0);
    node_set_number(bodyId, 'asset.PbrMaterial', 'env_intensity', 1.6);   // 车漆吃环境光：暗面不至于死黑`,
`    const bodyId = addBox(root, 0, bodyY, 0, data.wid, bodyH, data.len, bodyCol, 0.30, 0.35, true, 0.72);
    node_set_number(bodyId, 'asset.PbrMaterial', 'clearcoat_roughness', 0.22);
    node_set_number(bodyId, 'asset.PbrMaterial', 'env_intensity', 1.4);`);

// 玻璃：固定 #07131c，metal .72 rough .09 clearcoat 1
rep('glass', `    const glassId = addBox(root, 0, bodyY + bodyH * 0.5 + cabinH * 0.5 - 0.02, data.len * data.cabinZ,
      data.wid * 0.64, cabinH, data.len * 0.34, cabCol, 0.35, 0.08, false);
    node_set_number(glassId, 'asset.PbrMaterial', 'env_intensity', 1.9);`,
`    const glassId = addBox(root, 0, bodyY + bodyH * 0.5 + cabinH * 0.5 - 0.02, data.len * data.cabinZ,
      data.wid * 0.64, cabinH, data.len * 0.34, srgb3([0.027, 0.075, 0.11]), 0.72, 0.09, false, 1.0);
    node_set_number(glassId, 'asset.PbrMaterial', 'clearcoat_roughness', 0.05);
    node_set_number(glassId, 'asset.PbrMaterial', 'env_intensity', 1.8);`);

// 进气口 / 尾翼：accent（带一点自发光）
rep('accent nose', `        data.wid * 0.92, data.hgt * 0.22, data.len * (0.16 + 0.10 * data.nose), accCol, 0.65, 0.30, false, 0.6);`,
`        data.wid * 0.92, data.hgt * 0.22, data.len * (0.16 + 0.10 * data.nose), accCol, 0.36, 0.35, false, 0.0);`);
rep('accent spoiler', `        data.wid * 0.86, 0.07, data.len * (0.06 + 0.05 * data.spoiler), accCol, 0.70, 0.28, false, 0.6);`,
`        data.wid * 0.86, 0.07, data.len * (0.06 + 0.05 * data.spoiler), accCol, 0.36, 0.35, false, 0.0);`);

// 车灯颜色对齐
rep('head color', `    addGlow(root, -data.wid * 0.30, ly, data.len * 0.503, 0.34, 0.11, 0.06, [1.0, 0.94, 0.80], 2.2);
    addGlow(root, data.wid * 0.30, ly, data.len * 0.503, 0.34, 0.11, 0.06, [1.0, 0.94, 0.80], 2.2);`,
`    addGlow(root, -data.wid * 0.30, ly, data.len * 0.503, 0.34, 0.11, 0.06, [1.0, 0.945, 0.74], 2.4);
    addGlow(root, data.wid * 0.30, ly, data.len * 0.503, 0.34, 0.11, 0.06, [1.0, 0.945, 0.74], 2.4);`);
rep('brake color', `    addGlow(root, 0, ly + 0.02, -data.len * 0.506, data.wid * 0.62, 0.09, 0.05, [1.0, 0.10, 0.05], 1.6);`,
`    addGlow(root, 0, ly + 0.02, -data.len * 0.506, data.wid * 0.62, 0.09, 0.05, [1.0, 0.14, 0.21], 1.1);`);

// 轮子调用带上轮毂颜色
rep('wheel calls', `      addWheel(root, -wx, data.wheelR, wz, data.wheelR, data.wheelW),
      addWheel(root, wx, data.wheelR, wz, data.wheelR, data.wheelW),
      addWheel(root, -wx, data.wheelR, -wz, data.wheelR, data.wheelW),
      addWheel(root, wx, data.wheelR, -wz, data.wheelR, data.wheelW),`,
`      addWheel(root, -wx, data.wheelR, wz, data.wheelR, data.wheelW, data.rim),
      addWheel(root, wx, data.wheelR, wz, data.wheelR, data.wheelW, data.rim),
      addWheel(root, -wx, data.wheelR, -wz, data.wheelR, data.wheelW, data.rim),
      addWheel(root, wx, data.wheelR, -wz, data.wheelR, data.wheelW, data.rim),`);

fs.writeFileSync(P, t);
console.log(log.join('\n'));
