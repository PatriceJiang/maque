// 上采样 384×216 HDR → 屏幕：双线性 + 轻微暗角 + ACES + gamma。
@fragment
fn fs_main(@builtin(position) fc: vec4f) -> @location(0) vec4f {
    let res = mq_pass.resolution;
    let uv = fc.xy / res;
    var c = sample_bilinear(ch0, uv).rgb;
    let q = uv - vec2f(0.5);
    c *= 1.0 - 0.35 * dot(q, q);
    c = aces(c * 1.25);
    c = pow(max(c, vec3f(0.0)), vec3f(1.0 / 2.2));
    return vec4f(c, 1.0);
}
