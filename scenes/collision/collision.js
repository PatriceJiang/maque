/* 3D 碰撞查询自检（plans/36）：球重叠 / 连续扫掠球 / 定向盒 SAT / 命中详情。
 * 场景：原点一个 scale=2 的 cube（世界半边长 1.2），(5,0,0) 另一个。无动力学。 */
let done = false;

function near(a, b, eps) {
    if (eps === undefined) eps = 1e-3;
    return Math.abs(a - b) <= eps;
}

function check(ok, msg) {
    if (!ok) log("collision: FAIL " + msg);
    return ok;
}

function update() {
    if (done || director_frame() < 2) return;
    done = true;
    const box = entity_find("box");
    let ok = true;

    /* 球重叠：近处命中/远处未命中 */
    ok = check(scene_sphere_overlap(0.0, 0.0, 1.5, 1.0) === box, "sphere overlap hit") && ok;
    ok = check(scene_sphere_overlap(0.0, 0.0, 5.0, 1.0) === -1, "sphere overlap miss") && ok;

    /* 连续扫掠球：从 z=5 向 -Z，半径 0.2 → 球心接触时 z = 1.2 + 0.2 = 1.4（t=3.6）。
     * 注意 cast_point 是**球心**位置，不是表面接触点。 */
    const hit = scene_sphere_cast(0.0, 0.0, 5.0, 0.0, 0.0, -1.0, 0.2, 10.0);
    ok = check(hit === box, "sphere cast hit id") && ok;
    const d = scene_cast_distance();
    ok = check(near(d, 3.6, 0.05), "sphere cast distance " + d) && ok;
    ok = check(near(scene_cast_point_x(), 0.0, 0.05) &&
               near(scene_cast_point_y(), 0.0, 0.05) &&
               near(scene_cast_point_z(), 1.4, 0.05),
               "sphere cast point " + scene_cast_point_z()) && ok;
    ok = check(scene_sphere_cast(0.0, 0.0, 5.0, 0.0, 1.0, 0.0, 0.2, 10.0) === -1,
               "sphere cast miss") && ok;

    /* 定向盒 SAT：同位置命中、远处未命中 */
    ok = check(scene_obb_overlap(0.0, 0.0, 1.5, 1.0, 0.5, 1.0, 0.3) === box, "obb overlap hit") && ok;
    ok = check(scene_obb_overlap(10.0, 0.0, 0.0, 1.0, 0.5, 1.0, 0.3) === -1, "obb overlap miss") && ok;

    /* 射线：距离/命中点与 cast_last 同一口径 */
    ok = check(scene_raycast(0.0, 0.0, 5.0, 0.0, 0.0, -1.0, 10.0) === box, "raycast hit") && ok;
    ok = check(near(scene_cast_distance(), 3.8, 0.05), "raycast distance") && ok;
    ok = check(near(scene_cast_point_z(), 1.2, 0.05), "raycast point z") && ok;

    /* 查询只读：节点数不变 */
    ok = check(entity_count() === 3.0, "entity count unchanged " + entity_count()) && ok;

    log(ok ? "collision: PASS" : "collision: FAIL");
}
