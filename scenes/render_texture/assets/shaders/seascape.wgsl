// "Seascape" by Alexander Alekseev aka TDM - 2014
// License: Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported
//          https://creativecommons.org/licenses/by-nc-sa/3.0/
// Contact: tdmaav@gmail.com
// Original: https://www.shadertoy.com/view/Ms2SD1
//
// WGSL port for maque's scene-resource fullscreen pass (`render.ShaderPass`).
// This file is a *demo resource* (lives under scenes/), not an engine builtin shader.
//
// Porting notes:
//   * Only the fragment entry point `fs_main` is provided; the engine supplies the
//     fullscreen-triangle vertex shader.
//   * Shadertoy time semantics restored: time = iTime*0.3 + iMouse.x*0.01
//     (the intermediate GLSL port used uDrift*5, a port-side change we do not keep).
//   * WebGPU `@builtin(position)` is origin top-left / y down; Shadertoy fragCoord is
//     origin bottom-left / y up, so the y coordinate is flipped below.
//   * GLSL row-vector * matrix (e.g. `dir * fromEuler(ang)`, `uv *= octave_m`) is
//     written as `transpose(M) * v` in WGSL (matrices are column-major in both).
//   * The GLSL out-parameter of heightMapTracing becomes `ptr<function, vec3f>`.

struct MqShaderPass {
    resolution: vec2f,   // render-target size in pixels (width, height)
    time: f32,           // render.ShaderPass.time in seconds
    frame: f32,          // frame counter
    mouse: vec4f,        // xy = pointer in target pixels (origin top-left), zw reserved
};

@group(0) @binding(0) var<uniform> mq_pass: MqShaderPass;

const NUM_STEPS: i32 = 8;
const PI: f32 = 3.141592;
const EPSILON: f32 = 1e-3;
// EPSILON_NRM = 0.1 / resolution.x is computed in fs_main (it depends on the uniform).

// sea
const ITER_GEOMETRY: i32 = 3;
const ITER_FRAGMENT: i32 = 5;
const SEA_HEIGHT: f32 = 0.6;
const SEA_CHOPPY: f32 = 4.0;
const SEA_SPEED: f32 = 0.8;
const SEA_FREQ: f32 = 0.16;
const SEA_BASE: vec3f = vec3f(0.1, 0.19, 0.22);
const SEA_WATER_COLOR: vec3f = vec3f(0.8, 0.9, 0.6);
// GLSL mat2(1.6,1.2,-1.2,1.6) is column-major: columns (1.6,1.2) and (-1.2,1.6).
const OCTAVE_M: mat2x2f = mat2x2f(1.6, 1.2, -1.2, 1.6);

fn sea_time() -> f32 {
    // GLSL: #define SEA_TIME (1.0 + uTime * SEA_SPEED)
    return 1.0 + mq_pass.time * SEA_SPEED;
}

// math
fn from_euler(ang: vec3f) -> mat3x3f {
    let a1 = vec2f(sin(ang.x), cos(ang.x));
    let a2 = vec2f(sin(ang.y), cos(ang.y));
    let a3 = vec2f(sin(ang.z), cos(ang.z));
    let c0 = vec3f(a1.y * a3.y + a1.x * a2.x * a3.x,
                   a1.y * a2.x * a3.x + a3.y * a1.x,
                   -a2.y * a3.x);
    let c1 = vec3f(-a2.y * a1.x, a1.y * a2.y, a2.x);
    let c2 = vec3f(a3.y * a1.x * a2.x + a1.y * a3.x,
                   a1.x * a3.x - a1.y * a3.y * a2.x,
                   a2.y * a3.y);
    return mat3x3f(c0, c1, c2);
}

fn hash(p: vec2f) -> f32 {
    let h = dot(p, vec2f(127.1, 311.7));
    return fract(sin(h) * 43758.5453123);
}

fn noise(p: vec2f) -> f32 {
    let i = floor(p);
    let f = fract(p);
    let u = f * f * (3.0 - 2.0 * f);
    return -1.0 + 2.0 * mix(
        mix(hash(i + vec2f(0.0, 0.0)), hash(i + vec2f(1.0, 0.0)), u.x),
        mix(hash(i + vec2f(0.0, 1.0)), hash(i + vec2f(1.0, 1.0)), u.x),
        u.y);
}

// lighting
fn diffuse(n: vec3f, l: vec3f, p: f32) -> f32 {
    return pow(dot(n, l) * 0.4 + 0.6, p);
}

fn specular(n: vec3f, l: vec3f, e: vec3f, s: f32) -> f32 {
    let nrm = (s + 8.0) / (PI * 8.0);
    return pow(max(dot(reflect(e, n), l), 0.0), s) * nrm;
}

// sky
fn get_sky_color(e: vec3f) -> vec3f {
    // GLSL mutates e.y = max(e.y, 0.0); parameters are immutable in WGSL.
    let ey = max(e.y, 0.0);
    return vec3f(pow(1.0 - ey, 2.0), 1.0 - ey, 0.6 + (1.0 - ey) * 0.4);
}

// sea
fn sea_octave(uv_in: vec2f, choppy: f32) -> f32 {
    // GLSL: uv += noise(uv)  (scalar added to both components)
    let uv = uv_in + vec2f(noise(uv_in));
    let wv = vec2f(1.0) - abs(sin(uv));
    let swv = abs(cos(uv));
    let w = mix(wv, swv, wv);
    return pow(1.0 - pow(w.x * w.y, 0.65), choppy);
}

fn map(p: vec3f) -> f32 {
    var freq = SEA_FREQ;
    var amp = SEA_HEIGHT;
    var choppy = SEA_CHOPPY;
    var uv = p.xz;
    uv.x = uv.x * 0.75;

    let st = sea_time();
    var d = 0.0;
    var h = 0.0;
    for (var i = 0; i < ITER_GEOMETRY; i = i + 1) {
        d = sea_octave((uv + vec2f(st)) * freq, choppy);
        d = d + sea_octave((uv - vec2f(st)) * freq, choppy);
        h = h + d * amp;
        uv = transpose(OCTAVE_M) * uv;   // GLSL: uv *= octave_m
        freq = freq * 1.9;
        amp = amp * 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return p.y - h;
}

fn map_detailed(p: vec3f) -> f32 {
    var freq = SEA_FREQ;
    var amp = SEA_HEIGHT;
    var choppy = SEA_CHOPPY;
    var uv = p.xz;
    uv.x = uv.x * 0.75;

    let st = sea_time();
    var d = 0.0;
    var h = 0.0;
    for (var i = 0; i < ITER_FRAGMENT; i = i + 1) {
        d = sea_octave((uv + vec2f(st)) * freq, choppy);
        d = d + sea_octave((uv - vec2f(st)) * freq, choppy);
        h = h + d * amp;
        uv = transpose(OCTAVE_M) * uv;   // GLSL: uv *= octave_m
        freq = freq * 1.9;
        amp = amp * 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return p.y - h;
}

fn get_sea_color(p: vec3f, n: vec3f, l: vec3f, eye: vec3f, dist: vec3f) -> vec3f {
    var fresnel = clamp(1.0 - dot(n, -eye), 0.0, 1.0);
    fresnel = pow(fresnel, 3.0) * 0.65;

    let reflected = get_sky_color(reflect(eye, n));
    let refracted = SEA_BASE + diffuse(n, l, 80.0) * SEA_WATER_COLOR * 0.12;

    var color = mix(refracted, reflected, fresnel);

    let atten = max(1.0 - dot(dist, dist) * 0.001, 0.0);
    color = color + SEA_WATER_COLOR * (p.y - SEA_HEIGHT) * 0.18 * atten;

    color = color + vec3f(specular(n, l, eye, 60.0));

    return color;
}

// tracing
fn get_normal(p: vec3f, eps: f32) -> vec3f {
    var n = vec3f(0.0);
    n.y = map_detailed(p);
    n.x = map_detailed(vec3f(p.x + eps, p.y, p.z)) - n.y;
    n.z = map_detailed(vec3f(p.x, p.y, p.z + eps)) - n.y;
    n.y = eps;
    return normalize(n);
}

fn height_map_tracing(ori: vec3f, dir: vec3f, p: ptr<function, vec3f>) -> f32 {
    var tm = 0.0;
    var tx = 1000.0;
    var hx = map(ori + dir * tx);
    if (hx > 0.0) {
        return tx;
    }
    var hm = map(ori + dir * tm);
    var tmid = 0.0;
    for (var i = 0; i < NUM_STEPS; i = i + 1) {
        tmid = mix(tm, tx, hm / (hm - hx));
        *p = ori + dir * tmid;
        let hmid = map(*p);
        if (hmid < 0.0) {
            tx = tmid;
            hx = hmid;
        } else {
            tm = tmid;
            hm = hmid;
        }
    }
    return tmid;
}

// GLSL smoothstep(0.0, -0.05, dir.y) uses reversed edges; compute it explicitly so the
// behavior does not depend on how a backend implements smoothstep with edge0 > edge1.
fn smoothstep_rev(edge0: f32, edge1: f32, x: f32) -> f32 {
    let t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// main
@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let res = mq_pass.resolution;
    // Shadertoy fragCoord orientation: origin bottom-left, y up (WebGPU is y down).
    let frag = vec2f(frag_coord.x, res.y - frag_coord.y);
    var uv = frag / res;
    // WGSL has no vec-scalar +/- (only *), so subtract a vector.
    uv = uv * 2.0 - vec2f(1.0);
    uv.x = uv.x * (res.x / res.y);
    // Shadertoy original: float time = iTime * 0.3 + iMouse.x * 0.01;
    let time = mq_pass.time * 0.3 + mq_pass.mouse.x * 0.01;

    // ray
    let ang = vec3f(sin(time * 3.0) * 0.1, sin(time) * 0.2 + 0.3, time);
    let ori = vec3f(0.0, 3.5, time * 5.0);
    var dir = normalize(vec3f(uv, -2.0));
    dir.z = dir.z + length(uv) * 0.15;
    // GLSL: dir = normalize(dir) * fromEuler(ang)  (row-vector * matrix)
    dir = transpose(from_euler(ang)) * normalize(dir);

    // tracing
    var p = vec3f(0.0);
    // heightMapTracing always writes p; its return value is unused (as in the original).
    let _tmid = height_map_tracing(ori, dir, &p);
    let dist = p - ori;
    let epsilon_nrm = 0.1 / res.x;
    let n = get_normal(p, dot(dist, dist) * epsilon_nrm);
    let light = normalize(vec3f(0.0, 1.0, 0.8));

    // color
    let color = mix(
        get_sky_color(dir),
        get_sea_color(p, n, light, dir, dist),
        pow(smoothstep_rev(0.0, -0.05, dir.y), 0.3));

    // post
    return vec4f(pow(color, vec3f(0.75)), 1.0);
}
