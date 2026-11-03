// Buffer C — supplemental noise texture used to add diffuse/normal detail.
// Derived from https://www.shadertoy.com/view/7ljcRW by Fewes.

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let fragCoord = fragCoordSH(frag_coord);
    let uv = fragCoord / mq_pass.resolution;

    var color = vec3f(0.0);

    var a = 0.5;
    var f = 2.0;
    for (var i = 0; i < 8; i = i + 1) {
        color += noised(uv * f) * a;
        a *= 0.95;
        f *= 2.0;
    }

    return vec4f(color, 1.0);
}
