// Mouse-Paint Eroded Mountains — https://www.shadertoy.com/view/sf23W1
// by Rune Skovbo Johansen (2026). Shadertoy original license: see the source tabs.
//   · Phacelle Noise + erosion filter: Mozilla Public License 2.0
//     (copyright (c) 2025 Rune Skovbo Johansen, https://mozilla.org/MPL/2.0/)
//   · terrain rendering: derived from https://www.shadertoy.com/view/7ljcRW by Fewes
//   · BRDF: https://www.shadertoy.com/view/XlKSDR ; camera: https://www.shadertoy.com/view/XsB3Rm
//
// WGSL port for maque's scene-resource **shader graph** (`render.ShaderGraph`).
// This file is the shared "Common" tab: the runtime prepends it to every pass source.
//
// Porting notes (differences that are structural, not stylistic):
//   · GLSL `#define` → `const`; the debug/comparison switches (SHOW_*, GREYSCALE,
//     COMPARISON_SLIDER) were off in the original and are not ported. FIXED_SUN / WATER /
//     TREES / DETAIL_TEXTURE / DRAINAGE / SHADOWS were on and are ported unconditionally.
//   · Shadertoy `iChannel0..3` → `ch0..ch3` (group0 binding1..4). Buffers are RGBA32F,
//     which WebGPU cannot filter, so `texture()` is emulated by `textureSH()` (manual
//     bilinear) and `texelFetch()` by `texelFetchSH()`.
//   · Shadertoy's texture space has the origin at the bottom-left; WebGPU renders into
//     rows starting at the top. `texelFetchSH` flips y, so all Shadertoy-style math above
//     it keeps working unchanged.
//   · `iChannel1` in the original is Shadertoy's keyboard texture (Backspace/Shift/Enter).
//     Here those three states come from the uniform (`keys`), which is equivalent.
//   · Fragments flip y at entry (`fragCoord`) because Shadertoy's `fragCoord` is
//     bottom-left while WebGPU's `@builtin(position)` is top-left.
//   · `iFrameRate` is the fixed simulation rate (60 Hz), not a wall-clock measurement —
//     the engine is fixed-step, so this keeps painting speed deterministic.

struct MqShaderGraphPass {
    resolution: vec2f,        // 本阶段渲染尺寸（像素）：中间缓冲边长，或主目标尺寸
    time: f32,                // 组件 time（秒）= Shadertoy iTime
    frame: f32,               // 帧号 = Shadertoy iFrame
    mouse: vec4f,             // xy = 指针像素（原点左上，**屏幕空间**）；z = 按下(1)/松开(0)；w 预留
    keys: vec4f,              // x = Backspace，y = Shift，z = Enter（0/1）；w 预留
    screen_resolution: vec2f, // 主目标尺寸：mouse.xy 所在的空间（中间缓冲阶段据此还原射线）
    pad: vec2f,               // 对齐填充（64B）
};

@group(0) @binding(0) var<uniform> mq_pass: MqShaderGraphPass;
@group(0) @binding(1) var ch0: texture_2d<f32>;
@group(0) @binding(2) var ch1: texture_2d<f32>;
@group(0) @binding(3) var ch2: texture_2d<f32>;
@group(0) @binding(4) var ch3: texture_2d<f32>;
@group(0) @binding(5) var mq_sampler: sampler;

// -----------------------------------------------------------------------------
// Paint options
// -----------------------------------------------------------------------------

const DEFAULT_HEIGHT: f32 = 0.45;
const BRUSH_SIZE: f32 = 0.2;

// -----------------------------------------------------------------------------
// Renderer settings
// -----------------------------------------------------------------------------

const WATER_HEIGHT: f32 = 0.46;
const GRASS_HEIGHT: f32 = 0.465;
const DRAINAGE_WIDTH: f32 = 0.3;
const RAYMARCH_QUALITY: f32 = 2.0;

// -----------------------------------------------------------------------------
// Camera settings
// -----------------------------------------------------------------------------

const CAMERA_DIST: f32 = 3.25;
const CAMERA_FOV: f32 = 11.0;
const CAMERA_ANGLE: f32 = -0.45;
const CAMERA_ELEVATION: f32 = -0.43;
const CAMERA_LOOKAT = vec3f(0.0, 0.40, 0.0);
const TIME_CAM_SPIN: f32 = 1.0 / 60.0;
const TIME_CAM_WOBBLE: f32 = 0.0;

// -----------------------------------------------------------------------------
// Material and color values
// -----------------------------------------------------------------------------

const M_GROUND: i32 = 0;
const M_STRATA: i32 = 1;
const M_WATER: i32 = 2;

const CLIFF_COLOR = vec3f(0.22, 0.2, 0.2);
const DIRT_COLOR = vec3f(0.6, 0.5, 0.4);
const TREE_COLOR = vec3f(0.12, 0.26, 0.1);
const GRASS_COLOR1 = vec3f(0.15, 0.3, 0.1);
const GRASS_COLOR2 = vec3f(0.4, 0.5, 0.2);
const SAND_COLOR = vec3f(0.8, 0.7, 0.6);

const WATER_COLOR = vec3f(0.0, 0.05, 0.1);
const WATER_SHORE_COLOR = vec3f(0.0, 0.25, 0.25);

const SUN_COLOR = vec3f(1.0, 0.98, 0.95) * 2.0;
const AMBIENT_COLOR = vec3f(0.3, 0.5, 0.7) * 0.1;

// -----------------------------------------------------------------------------
// Misc utility functions
// -----------------------------------------------------------------------------

const PI: f32 = 3.14159265358979;
const DEG_TO_RAD: f32 = PI / 180.0;

fn clamp01(x: f32) -> f32 { return clamp(x, 0.0, 1.0); }
fn sq(x: f32) -> f32 { return x * x; }

// GLSL-compatible smoothstep. The original shader also calls it with edge0 > edge1
// (reversed ramp), where WGSL's built-in is documented as undefined — this form is
// the literal GLSL implementation and stays well-defined for both orders.
fn smoothstep_(edge0: f32, edge1: f32, x: f32) -> f32 {
    let t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

fn smoothstep3_(edge0: f32, edge1: f32, x: vec3f) -> vec3f {
    let t = clamp((x - vec3f(edge0)) / vec3f(edge1 - edge0), vec3f(0.0), vec3f(1.0));
    return t * t * (vec3f(3.0) - 2.0 * t);
}

fn hash(x_in: vec2f) -> vec2f {
    let k = vec2f(0.3183099, 0.3678794);
    let x = x_in * k + k.yx;
    return -1.0 + 2.0 * fract(16.0 * k * fract(x.x * x.y * (x.x + x.y)));
}

// Returns gradient noise (in x) and its derivatives (in yz).
// From https://www.shadertoy.com/view/XdXBRH
fn noised(p: vec2f) -> vec3f {
    let i = floor(p);
    let f = fract(p);

    let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    let du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

    let ga = hash(i + vec2f(0.0, 0.0));
    let gb = hash(i + vec2f(1.0, 0.0));
    let gc = hash(i + vec2f(0.0, 1.0));
    let gd = hash(i + vec2f(1.0, 1.0));

    let va = dot(ga, f - vec2f(0.0, 0.0));
    let vb = dot(gb, f - vec2f(1.0, 0.0));
    let vc = dot(gc, f - vec2f(0.0, 1.0));
    let vd = dot(gd, f - vec2f(1.0, 1.0));

    return vec3f(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
        ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
        du * (u.yx * (va - vb - vc + vd) + vec2f(vb, vc) - va));
}

// From https://iquilezles.org/articles/intersectors
fn boxIntersection(ro: vec3f, rd: vec3f, boxSize: vec3f, outNormal: ptr<function, vec3f>) -> vec2f {
    let m = vec3f(1.0) / rd;
    let n = m * ro;
    let k = abs(m) * boxSize;
    let t1 = -n - k;
    let t2 = -n + k;
    let tN = max(max(t1.x, t1.y), t1.z);
    let tF = min(min(t2.x, t2.y), t2.z);
    if (tN > tF || tF < 0.0) {
        return vec2f(-1.0);
    }
    *outNormal = -sign(rd) * step(t1.yzx, t1.xyz) * step(t1.zxy, t1.xyz);
    return vec2f(tN, tF);
}

// ==========================================================================================
// Set up camera
// ==========================================================================================

// From https://www.shadertoy.com/view/XsB3Rm
fn CameraRay(fov: f32, size: vec2f, pos: vec2f) -> vec3f {
    let xy = pos - size * 0.5;
    let cot_half_fov = tan((90.0 - fov * 0.5) * DEG_TO_RAD);
    let z = size.y * 0.5 * cot_half_fov;
    return normalize(vec3f(xy, -z));
}

fn CameraRotation(angle: vec2f) -> mat3x3f {
    let c = cos(angle);
    let s = sin(angle);
    // GLSL mat3(a,b,c, d,e,f, g,h,i) is column-major: columns (a,b,c) (d,e,f) (g,h,i).
    return mat3x3f(
        vec3f(c.y, 0.0, -s.y),
        vec3f(s.y * s.x, c.x, c.y * s.x),
        vec3f(s.y * c.x, -s.x, c.y * c.x));
}

struct Ray {
    ro: vec3f,
    rd: vec3f,
};

fn GetRay(iTime: f32, iMouse: vec4f, iResolution: vec2f, fragCoord: vec2f, debugWidth: f32) -> Ray {
    let iRevolution = iTime * 2.0 * PI;
    let cameraAngle = vec2f(
        iRevolution * TIME_CAM_SPIN
            + sin(iRevolution / 6.0) * TIME_CAM_WOBBLE * 6.0
            + CAMERA_ANGLE * 2.0 * PI,
        CAMERA_ELEVATION);
    let cameraDistance = CAMERA_DIST;

    let rot = CameraRotation(cameraAngle.yx);

    var r: Ray;
    // Ray direction.
    r.rd = CameraRay(CAMERA_FOV, iResolution, fragCoord - vec2f(debugWidth / 2.0, 0.0));
    r.rd = rot * r.rd;
    // Ray origin.
    r.ro = CAMERA_LOOKAT + rot * vec3f(0.0, 0.0, cameraDistance);
    return r;
}

fn GetMouseWorldPos(ro: vec3f, rd: vec3f, planeHeight: f32) -> vec3f {
    return ro + rd * (planeHeight - ro.y) / rd.y;
}

fn SkyColor(rd: vec3f, sun: vec3f) -> vec3f {
    let costh = dot(rd, sun);
    return AMBIENT_COLOR * PI * (1.0 - abs(costh) * 0.8);
}

fn Tonemap_ACES(x: vec3f) -> vec3f {
    // Narkowicz 2015, "ACES Filmic Tone Mapping Curve"
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}

// -----------------------------------------------------------------------------
// BRDF functionality from https://www.shadertoy.com/view/XlKSDR
// -----------------------------------------------------------------------------

fn pow5(x: f32) -> f32 {
    let x2 = x * x;
    return x2 * x2 * x;
}

fn D_GGX(linearRoughness: f32, NoH: f32) -> f32 {
    // Walter et al. 2007, "Microfacet Models for Refraction through Rough Surfaces"
    let oneMinusNoHSquared = 1.0 - NoH * NoH;
    let a = NoH * linearRoughness;
    let k = linearRoughness / (oneMinusNoHSquared + a * a);
    let d = k * k * (1.0 / PI);
    return d;
}

fn V_SmithGGXCorrelated(linearRoughness: f32, NoV: f32, NoL: f32) -> f32 {
    // Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"
    let a2 = linearRoughness * linearRoughness;
    let GGXV = NoL * sqrt((NoV - a2 * NoV) * NoV + a2);
    let GGXL = NoV * sqrt((NoL - a2 * NoL) * NoL + a2);
    return 0.5 / (GGXV + GGXL);
}

fn F_Schlick3(f0: vec3f, VoH: f32) -> vec3f {
    // Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
    return f0 + (vec3f(1.0) - f0) * pow5(1.0 - VoH);
}

fn F_Schlick1(f0: f32, f90: f32, VoH: f32) -> f32 {
    return f0 + (f90 - f0) * pow5(1.0 - VoH);
}

fn Fd_Burley(linearRoughness: f32, NoV: f32, NoL: f32, LoH: f32) -> f32 {
    // Burley 2012, "Physically-Based Shading at Disney"
    let f90 = 0.5 + 2.0 * linearRoughness * LoH * LoH;
    let lightScatter = F_Schlick1(1.0, f90, NoL);
    let viewScatter = F_Schlick1(1.0, f90, NoV);
    return lightScatter * viewScatter * (1.0 / PI);
}

fn Fd_Lambert() -> f32 {
    return 1.0 / PI;
}

fn Shade(diffuse: vec3f, f0: vec3f, smoothness: f32, n: vec3f, v: vec3f, l: vec3f, lc: vec3f) -> vec3f {
    let h = normalize(v + l);

    let NoV = abs(dot(n, v)) + 1e-5;
    let NoL = clamp01(dot(n, l));
    let NoH = clamp01(dot(n, h));
    let LoH = clamp01(dot(l, h));

    let roughness = 1.0 - smoothness;
    let linearRoughness = roughness * roughness;
    let D = D_GGX(linearRoughness, NoH);
    let V = V_SmithGGXCorrelated(linearRoughness, NoV, NoL);
    let F = F_Schlick3(f0, LoH);
    let Fr = (D * V) * F;

    let Fd = diffuse * Fd_Burley(linearRoughness, NoV, NoL, LoH);

    return (Fd + Fr) * lc * NoL;
}

// ------------------------------------------------------------------------------
// Atmosphere
// ------------------------------------------------------------------------------

const C_RAYLEIGH = vec3f(5.802, 13.558, 33.100) * 1e-6;
const C_MIE = vec3f(3.996, 3.996, 3.996) * 1e-6;

fn PhaseRayleigh(costh: f32) -> f32 {
    return 3.0 * (1.0 + costh * costh) / (16.0 * PI);
}

fn PhaseMie(costh_in: f32, g_in: f32) -> f32 {
    var g = min(g_in, 0.9381);
    let k = 1.55 * g - 0.55 * g * g * g;
    let kcosth = k * costh_in;
    return (1.0 - k * k) / ((4.0 * PI) * (1.0 - kcosth) * (1.0 - kcosth));
}

// ------------------------------------------------------------------------------
// Packing
// ------------------------------------------------------------------------------

// Pack a vec4 (0-1) into the bits of a single 32-bit float. The buffer target must be
// RGBA32F (no blending) for the bits to survive; this is why the graph buffers are float32.
fn pack4(rgba: vec4f) -> f32 {
    let red = u32(clamp(rgba.r * 255.0, 0.0, 255.0));
    let green = u32(clamp(rgba.g * 255.0, 0.0, 255.0));
    let blue = u32(clamp(rgba.b * 255.0, 0.0, 255.0));
    let alpha = u32(clamp(rgba.a * 255.0, 0.0, 255.0));
    return bitcast<f32>((red << 24u) | (green << 16u) | (blue << 8u) | alpha);
}

fn unpack4(col: f32) -> vec4f {
    let val = bitcast<u32>(col);
    return vec4f(
        f32((val >> 24u) & 255u) / 255.0,
        f32((val >> 16u) & 255u) / 255.0,
        f32((val >> 8u) & 255u) / 255.0,
        f32(val & 255u) / 255.0);
}

// ------------------------------------------------------------------------------
// Shadertoy-style texture access (origin bottom-left, clamp-to-edge, bilinear)
// ------------------------------------------------------------------------------

fn texDims(t: texture_2d<f32>) -> vec2i {
    return vec2i(textureDimensions(t));
}

// texelFetch(iChannel, p, 0) with Shadertoy's bottom-left origin.
fn texelFetchSH(t: texture_2d<f32>, p: vec2i) -> vec4f {
    let d = texDims(t);
    let q = clamp(p, vec2i(0), d - vec2i(1));
    return textureLoad(t, vec2i(q.x, d.y - 1 - q.y), 0);
}

// texture(iChannel, uv) with Shadertoy's bottom-left origin + clamp-to-edge.
// Manual bilinear: WebGPU cannot filter RGBA32F textures.
fn textureSH(t: texture_2d<f32>, uv: vec2f) -> vec4f {
    let dims = vec2f(texDims(t));
    let p = uv * dims - vec2f(0.5);
    let f = fract(p);
    let i = vec2i(floor(p));
    let c00 = texelFetchSH(t, i);
    let c10 = texelFetchSH(t, i + vec2i(1, 0));
    let c01 = texelFetchSH(t, i + vec2i(0, 1));
    let c11 = texelFetchSH(t, i + vec2i(1, 1));
    return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

// Shadertoy iMouse: xy = pixel position with origin bottom-left, z = pressed (1) / released (-1).
// mouse.xy is in screen pixels (that is what the engine maps the pointer to), so the flip uses
// the screen height rather than this pass's resolution.
fn iMouse() -> vec4f {
    let m = mq_pass.mouse;
    return vec4f(m.x, mq_pass.screen_resolution.y - m.y, select(-1.0, 1.0, m.z > 0.5), 0.0);
}

// Shadertoy fragCoord: (0,0) at the bottom-left corner of the pass target.
fn fragCoordSH(frag_coord: vec4f) -> vec2f {
    return vec2f(frag_coord.x, mq_pass.resolution.y - frag_coord.y);
}
