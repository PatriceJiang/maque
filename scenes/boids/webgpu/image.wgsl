// 把 N 个 boid 画成软点（320×180 HDR 缓冲）：正交投影 x/y，z 只影响大小/亮度。
// O(像素×N) splat；N=256、320×180 → 14.7M 次内层迭代/帧，RTX 级 GPU 可实时。
@fragment
fn fs_main(@builtin(position) fc: vec4f) -> @location(0) vec4f {
    let res = mq_pass.resolution;
    let uv = fc.xy / res;
    var col = vec3f(0.008, 0.012, 0.025) + vec3f(0.010, 0.020, 0.045) * (1.0 - uv.y);

    for (var j = 0; j < BOIDS; j = j + 1) {
        let p = load_state(ch0, j).xyz;
        let v = load_state(ch1, j).xyz;
        let sp = length(v);
        // 正交投影：世界 ±WORLD → 屏幕约 ±0.4；z 只用于大小/远近
        let sx = 0.5 + p.x * 0.045;
        let sy = 0.5 - p.y * 0.045;
        let d = (uv - vec2f(sx, sy)) * vec2f(res.x, res.y);
        let depth = clamp((p.z + WORLD) / (2.0 * WORLD), 0.0, 1.0);
        let rad = 1.4 + 1.8 * depth;
        let glow = exp(-dot(d, d) / (rad * rad));
        let t = clamp((sp - 1.2) / 3.8, 0.0, 1.0);
        let c = mix(vec3f(0.16, 0.42, 1.0), vec3f(0.85, 0.95, 1.0), t);
        col += c * glow * (0.9 + 0.8 * t);
    }
    return vec4f(col, 1.0);
}
