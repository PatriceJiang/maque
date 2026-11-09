// 位置积分：ch0 = 上一帧位置（self），ch1 = 速度。首帧写确定性初始位置。
@fragment
fn fs_main(@builtin(position) fc: vec4f) -> @location(0) vec4f {
    let i = boid_index(fc);
    if (mq_pass.frame < 0.5) {
        return vec4f(init_pos(i), 0.0);
    }
    var p = load_state(ch0, i).xyz + load_state(ch1, i).xyz * DT;
    // 硬边界兜底（速度 pass 已软回拉）
    p = clamp(p, vec3f(-WORLD), vec3f(WORLD));
    return vec4f(p, 0.0);
}
