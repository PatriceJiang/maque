/* 3D 变换 typed API 自检（plans/27 M1-4）：
 * 局部/世界读写、localToWorld / worldToLocal、lookAt（含父链补偿）、值类型返回值形状、
 * 与反射路径（node_get_number）读数一致。结果由 node_transform_api_e2e 断言。 */
let done = false;

function near(a, b, eps) {
    if (eps === undefined) eps = 1e-3;
    return Math.abs(a - b) <= eps;
}

function check(ok, msg) {
    if (!ok) log("transform_api: FAIL " + msg);
    return ok;
}

function update() {
    if (done || director_frame() < 2) return;
    done = true;
    const root = entity_find("root");
    const child = entity_find("child");
    const looker = entity_find("looker");
    const plain = entity_find("plain");
    const camera = entity_find("camera");
    const flat_child = entity_find("flat_child");
    const flat_fwd = entity_find("flat_fwd");

    let ok = true;
    ok = check(root >= 0 && child >= 0 && looker >= 0 && plain >= 0, "entity_find") && ok;

    /* ---- 值类型返回形状：JS 对象 {x,y,z} ---- */
    const lp = node_local_position(child);
    ok = check(typeof lp === "object" && lp !== null && "x" in lp && "y" in lp && "z" in lp,
               "value return shape") && ok;
    ok = check(near(lp.x, 1.0) && near(lp.y, 0.0) && near(lp.z, 0.0), "local_position") && ok;

    /* ---- 世界位置：父 yaw=+90°、scale=2，子局部 (1,0,0) → (10,0,-7) ---- */
    const wp = node_world_position(child);
    ok = check(near(wp.x, 10.0) && near(wp.y, 0.0) && near(wp.z, -7.0),
               "world_position " + JSON.stringify(wp)) && ok;

    /* ---- 写局部位置后立刻可读（不依赖上一帧缓存） ---- */
    node_set_local_position(child, 0.0, 1.0, 0.0);
    const wp2 = node_world_position(child);
    ok = check(near(wp2.x, 10.0) && near(wp2.y, 2.0) && near(wp2.z, -5.0),
               "world_position after set " + JSON.stringify(wp2)) && ok;

    /* ---- localToWorld / worldToLocal 互逆 ---- */
    const origin_w = node_local_to_world(child, { x: 0.0, y: 0.0, z: 0.0 });
    ok = check(near(origin_w.x, wp2.x) && near(origin_w.y, wp2.y) && near(origin_w.z, wp2.z),
               "local_to_world origin") && ok;
    const local_pt = { x: 1.5, y: -0.5, z: 2.0 };
    const world_pt = node_local_to_world(child, local_pt);
    const back_pt = node_world_to_local(child, world_pt);
    ok = check(near(back_pt.x, local_pt.x) && near(back_pt.y, local_pt.y) && near(back_pt.z, local_pt.z),
               "world_to_local roundtrip " + JSON.stringify(back_pt)) && ok;

    /* ---- 值类型参数也接受 3 个数字（生成期不展开重载，运行期在编组层兼容） ---- */
    const world_pt2 = node_local_to_world(child, local_pt.x, local_pt.y, local_pt.z);
    ok = check(near(world_pt2.x, world_pt.x) && near(world_pt2.y, world_pt.y) &&
               near(world_pt2.z, world_pt.z), "value arg three floats") && ok;

    /* ---- 反射路径读数一致（same storage） ---- */
    const rx = node_get_number(child, "scene.Transform3D", "local.position.x");
    const ry = node_get_number(child, "scene.Transform3D", "local.position.y");
    ok = check(near(rx, 0.0) && near(ry, 1.0), "reflection consistency") && ok;

    /* ---- lookAt：世界 -Z 指向前方 → 单位旋转 ---- */
    node_set_local_position(looker, 0.0, 0.0, 0.0);
    node_look_at(looker, 0.0, 0.0, -5.0);
    let r = node_local_rotation(looker);
    ok = check(near(r.x, 0.0, 1e-3) && near(r.y, 0.0, 1e-3) && near(r.z, 0.0, 1e-3),
               "look_at forward " + JSON.stringify(r)) && ok;

    /* ---- lookAt +X：本地 -Z 指向 +X → yaw = -90° ---- */
    node_look_at(looker, 5.0, 0.0, 0.0);
    r = node_local_rotation(looker);
    ok = check(near(r.x, 0.0, 1e-3) && near(r.y, -Math.PI / 2, 1e-3) && near(r.z, 0.0, 1e-3),
               "look_at +x " + JSON.stringify(r)) && ok;

    /* ---- lookAt 在旋转父节点下补偿：世界朝向必须回到单位旋转 ---- */
    const target = { x: wp2.x, y: wp2.y, z: wp2.z - 5.0 };
    node_look_at(child, target.x, target.y, target.z);
    const wr = node_world_rotation(child);
    ok = check(near(wr.x, 0.0, 1e-2) && near(wr.y, 0.0, 1e-2) && near(wr.z, 0.0, 1e-2),
               "look_at under parent " + JSON.stringify(wr)) && ok;

    /* ---- 局部缩放读写 ---- */
    node_set_local_scale(child, 1.0, 2.0, 3.0);
    const sc = node_local_scale(child);
    ok = check(near(sc.x, 1.0) && near(sc.y, 2.0) && near(sc.z, 3.0), "local_scale") && ok;

    /* ---- 平级 `"parent": "名字"`（plans/36）：后向与前向引用都要挂上 ---- */
    ok = check(node_parent(flat_child) === root, "flat parent (after root)") && ok;
    ok = check(node_parent(flat_fwd) === root, "flat parent (before root)") && ok;

    /* ---- 主相机 Orbit 开关字段（plans/36）：反射读取 = 0（场景显式关闭） ---- */
    ok = check(node_get_number(camera, "render.Camera3D", "orbit_controls") == 0,
               "camera orbit_controls=false") && ok;

    /* ---- 无 Transform3D 的节点：set_local_* 自动补挂组件 ---- */
    node_set_local_position(plain, 1.0, 2.0, 3.0);
    ok = check(node_has_component(plain, "scene.Transform3D") == 1, "auto attach Transform3D") && ok;
    const pp = node_local_position(plain);
    ok = check(near(pp.x, 1.0) && near(pp.y, 2.0) && near(pp.z, 3.0), "plain local position") && ok;

    log(ok ? "transform_api: PASS" : "transform_api: FAIL");
}
