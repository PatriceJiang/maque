// 速度更新：分离/对齐/聚合（全对全，N=256 → 65k 次/帧）+ 软边界 + 限速。
// ch0 = 上一帧位置，ch1 = 上一帧速度（"self" = 本缓冲上一帧）。
@fragment
fn fs_main(@builtin(position) fc: vec4f) -> @location(0) vec4f {
    let i = boid_index(fc);
    if (mq_pass.frame < 0.5) {
        return vec4f(init_vel(i), 0.0);
    }
    let me = load_state(ch0, i).xyz;
    let mv = load_state(ch1, i).xyz;

    var sep = vec3f(0.0);
    var ali = vec3f(0.0);
    var coh = vec3f(0.0);
    var n = 0.0;
    for (var j = 0; j < BOIDS; j = j + 1) {
        if (j == i) { continue; }
        let o = load_state(ch0, j).xyz;
        let d = o - me;
        let d2 = dot(d, d);
        if (d2 > ALI_R * ALI_R || d2 < 1e-6) { continue; }
        n += 1.0;
        ali += load_state(ch1, j).xyz;
        coh += o;
        if (d2 < SEP_R * SEP_R) {
            sep -= d / d2;
        }
    }
    var acc = sep * 90.0;
    if (n > 0.5) {
        acc += (ali / n - mv) * 2.4;
        acc += (coh / n - me) * 1.6;
    }
    // 软边界：越界往回拉（与 JS 参考同量级）
    if (me.x > WORLD - 1.0) { acc.x -= (me.x - (WORLD - 1.0)) * 60.0; }
    if (me.x < -WORLD + 1.0) { acc.x += (-WORLD + 1.0 - me.x) * 60.0; }
    if (me.y > WORLD - 1.0) { acc.y -= (me.y - (WORLD - 1.0)) * 60.0; }
    if (me.y < -WORLD + 1.0) { acc.y += (-WORLD + 1.0 - me.y) * 60.0; }
    if (me.z > WORLD - 1.0) { acc.z -= (me.z - (WORLD - 1.0)) * 60.0; }
    if (me.z < -WORLD + 1.0) { acc.z += (-WORLD + 1.0 - me.z) * 60.0; }

    var v = mv + acc * DT;
    var sp = length(v);
    if (sp > MAX_SPEED) { v *= MAX_SPEED / sp; sp = MAX_SPEED; }
    if (sp < MIN_SPEED) { v *= MIN_SPEED / max(sp, 1e-4); }
    return vec4f(v, 0.0);
}
