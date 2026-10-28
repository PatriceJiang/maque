// "Mandelbulb" by Inigo Quilez — 2013
// License: Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported
//          https://creativecommons.org/licenses/by-nc-sa/3.0/
// Original: https://www.shadertoy.com/view/MdXSWn
//
// WGSL port for maque's scene-resource fullscreen pass (`render.ShaderPass`).
// Reference source: the author-authorized OpenCL port from the SIGGRAPH 2012 WebCL
// course (examples/shadertoy/mandelbulb.cl); porting notes:
//   * Only the fragment entry point `fs_main` is provided; the engine supplies the
//     fullscreen-triangle vertex shader.
//   * iTime -> mq_pass.time (no extra scaling; the 20s camera orbit period is below).
//   * WebGPU `@builtin(position)` is origin top-left / y down; Shadertoy fragCoord is
//     origin bottom-left / y up, so the y coordinate is flipped below.
//   * rsqrt -> inverseSqrt; cospi/sinpi -> cos/sin(PI*x); `out` params -> ptr<function, ...>.
//   * Power-8 Mandelbulb distance estimator via polynomial expansion (NumIte=7,
//     Bailout=100), bounding sphere radius 1.25, gradient normal via 3 extra DE taps,
//     two lights, orbit-trap material mix, shadow ray through the DE, AO, gamma, vignette.

struct MqShaderPass {
    resolution: vec2f,   // render-target size in pixels (width, height)
    time: f32,           // render.ShaderPass.time in seconds (= Shadertoy iTime)
    frame: f32,          // frame counter
    mouse: vec4f,        // xy = pointer in target pixels (origin top-left), zw reserved
};

@group(0) @binding(0) var<uniform> mq_pass: MqShaderPass;

const NUM_ITE: i32 = 7;
const BAILOUT: f32 = 100.0;
const EPS: f32 = 0.001;
const PI: f32 = 3.141592;

const LIGHT1: vec3f = vec3f(0.577, 0.577, 0.577);
const LIGHT2: vec3f = vec3f(-0.707, 0.0, 0.707);

const MATERIAL1: vec3f = vec3f(0.8, 0.6, 0.2);
const MATERIAL2: vec3f = vec3f(0.8, 0.3, 0.3);
const MATERIAL3: vec3f = vec3f(0.7, 0.4, 0.3);

// Ray/bounding-sphere intersection. Returns false on a miss.
fn isphere(sph: vec4f, ro: vec3f, rd: vec3f, t: ptr<function, vec2f>) -> bool {
    let oc = ro - sph.xyz;
    let b = dot(oc, rd);
    let c = dot(oc, oc) - sph.w * sph.w;
    let h = b * b - c;
    if (h < 0.0) {
        return false;
    }
    let g = sqrt(h);
    *t = vec2f(-b - g, -b + g);
    return true;
}

// Mandelbulb potential + orbit trap. Returns true if the point is inside the set
// (i.e. the orbit stays bounded -> distance 0 -> the raymarcher treats it as a hit).
fn iterate(q: vec3f, res_pot: ptr<function, f32>, res_color: ptr<function, vec4f>) -> bool {
    var trap = vec4f(100.0);
    var zz = q;
    var m = dot(zz, zz);
    if (m > BAILOUT) {
        *res_pot = 0.5 * log(m);
        *res_color = vec4f(1.0);
        return false;
    }

    for (var i = 1; i < NUM_ITE; i = i + 1) {
        let x = zz.x;
        let x2 = x * x;
        let x4 = x2 * x2;
        let y = zz.y;
        let y2 = y * y;
        let y4 = y2 * y2;
        let z = zz.z;
        let z2 = z * z;
        let z4 = z2 * z2;

        let k3 = x2 + z2;
        let k2 = inverseSqrt(k3 * k3 * k3 * k3 * k3 * k3 * k3);
        let k1 = x4 + y4 + z4 - 6.0 * y2 * z2 - 6.0 * x2 * y2 + 2.0 * z2 * x2;
        let k4 = x2 - y2 + z2;

        zz = vec3f(
            q.x + 64.0 * x * y * z * (x2 - z2) * k4 * (x4 - 6.0 * x2 * z2 + z4) * k1 * k2,
            q.y + -16.0 * y2 * k3 * k4 * k4 + k1 * k1,
            q.z + -8.0 * y * k4 * (x4 * x4 - 28.0 * x4 * x2 * z2 + 70.0 * x4 * z4 - 28.0 * x2 * z2 * z4 + z4 * z4) * k1 * k2);
        m = dot(zz, zz);

        trap = min(trap, vec4f(zz * zz, m));

        if (m > BAILOUT) {
            *res_color = trap;
            *res_pot = 0.5 * log(m) / pow(8.0, f32(i));
            return false;
        }
    }

    *res_color = trap;
    *res_pot = 0.0;
    return true;
}

// Ray-march the fractal. On a hit writes distance (rest), normal, orbit trap color.
fn ifractal(ro: vec3f, rd: vec3f, rest: ptr<function, f32>, maxt: f32,
            res_nor: ptr<function, vec3f>, res_col: ptr<function, vec4f>, fov: f32) -> bool {
    let sph = vec4f(0.0, 0.0, 0.0, 1.25);
    var dis = vec2f(0.0, 0.0);

    // bounding sphere
    if (!isphere(sph, ro, rd, &dis)) {
        return false;
    }
    // early skip
    if (dis.y < EPS) {
        return false;
    }
    // clip to near / far
    var t = dis.x;
    if (t < EPS) {
        t = EPS;
    }
    var tend = dis.y;
    if (tend > maxt) {
        tend = maxt;
    }

    let fovfactor = 1.0 / sqrt(1.0 + fov * fov);

    var gra = vec3f(0.0, 1.0, 0.0);   // GLSL leaves this uninitialized; keep it finite
    var color = vec4f(0.0);
    var col2 = vec4f(0.0);
    var pot1 = 0.0;
    var pot2 = 0.0;
    var pot3 = 0.0;
    var pot4 = 0.0;

    // raymarch
    while (t < tend) {
        let p = ro + rd * t;
        let surface = clamp(0.001 * t * fovfactor, 0.000001, 0.005);
        let eps = surface * 0.1;

        if (iterate(p, &pot1, &color)) {
            *rest = t;
            *res_nor = normalize(gra);
            *res_col = color;
            return true;
        }

        let _o2 = iterate(p + vec3f(eps, 0.0, 0.0), &pot2, &col2);
        let _o3 = iterate(p + vec3f(0.0, eps, 0.0), &pot3, &col2);
        let _o4 = iterate(p + vec3f(0.0, 0.0, eps), &pot4, &col2);

        gra = vec3f(pot2 - pot1, pot3 - pot1, pot4 - pot1);
        let dt = 0.5 * pot1 * eps / length(gra);

        if (dt < surface) {
            *res_col = color;
            *res_nor = normalize(gra);
            *rest = t;
            return true;
        }

        t = t + dt;
    }

    return false;
}

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let res = mq_pass.resolution;
    // Shadertoy fragCoord orientation: origin bottom-left, y up (WebGPU is y down).
    let frag = vec2f(frag_coord.x, res.y - frag_coord.y);
    let p = -vec2f(1.0) + 2.0 * frag / res;
    let s = p * vec2f(1.33, 1.0);
    let time = mq_pass.time;

    // camera: orbit with a 20s period
    let fov = 1.0;
    let phase = PI * 2.0 * time / 20.0;   // cospi(2*time/20), sinpi(2*time/20)
    let ct = cos(phase);
    let st = sin(phase);
    let r = 1.4 + 0.2 * ct;
    let campos = vec3f(r * st, 0.3 - 0.4 * st, r * ct);
    let camtar = vec3f(0.0, 0.1, 0.0);

    // camera matrix
    let cw = normalize(camtar - campos);
    let cp = vec3f(0.0, 1.0, 0.0);
    let cu = normalize(cross(cw, cp));
    let cv = normalize(cross(cu, cw));

    // ray
    let rd = normalize(s.x * cu + s.y * cv + 1.5 * cw);

    var t = 0.0;
    var nor = vec3f(0.0);
    var col = vec4f(0.0);
    let hit = ifractal(campos, rd, &t, 1e20, &nor, &col, fov);

    var rgb = vec3f(1.0);
    if (!hit) {
        // background
        rgb = 1.3 * vec3f(1.0, 0.98, 0.9) * (0.7 + 0.3 * rd.y);
    } else {
        // intersection point
        let xyz = campos + t * rd;

        // sun light
        var dif1 = clamp(0.2 + 0.8 * dot(LIGHT1, nor), 0.0, 1.0);
        dif1 = dif1 * dif1;

        // back light
        let dif2 = clamp(0.3 + 0.7 * dot(LIGHT2, nor), 0.0, 1.0);

        // ambient occlusion from the orbit trap
        let aot = clamp(1.25 * col.w - 0.4, 0.0, 1.0);
        let ao = 0.5 * aot * (aot + 1.0);

        // shadow: cast a light ray from the intersection point
        if (dif1 > EPS) {
            var lt1 = 0.0;
            var ln = vec3f(0.0);
            var lc = vec4f(0.0);
            if (ifractal(xyz, LIGHT1, &lt1, 1e20, &ln, &lc, fov)) {
                dif1 = 0.1;
            }
        }

        // material color from the orbit trap
        rgb = mix(rgb, MATERIAL1, sqrt(col.x) * 1.25);
        rgb = mix(rgb, MATERIAL2, sqrt(col.y) * 1.25);
        rgb = mix(rgb, MATERIAL3, sqrt(col.z) * 1.25);

        // lighting
        rgb = rgb * ((0.5 + 0.5 * nor.y) * vec3f(0.14, 0.15, 0.16) * 0.8
                   + dif1 * vec3f(1.0, 0.85, 0.4)
                   + 0.5 * dif2 * vec3f(0.08, 0.10, 0.14));
        rgb = rgb * vec3f(pow(ao, 0.8), ao, pow(ao, 1.1));

        // gamma
        rgb = 1.5 * (rgb * 0.15 + 0.85 * sqrt(rgb));
    }

    // vignette + clamp
    let uv = 0.5 * (p + vec2f(1.0));
    rgb = rgb * (0.7 + 0.3 * 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y));
    rgb = clamp(rgb, vec3f(0.0), vec3f(1.0));

    return vec4f(rgb, 1.0);
}
