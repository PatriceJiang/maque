// quartz cave 💎 — by aladiN (Shadertoy).
//
// WGSL port for maque's scene-resource **shader graph** (`render.ShaderGraph`).
// This file is the shared "Common" tab: the runtime prepends it to every pass source.
//
// Porting notes (structural, not stylistic):
//   · GLSL `#define` → `const`; the `#if XXX` switches become `const XXX: bool` (WGSL folds them).
//   · Shadertoy `iChannel0..3` → `ch0..ch3` (group0 binding1..4). Buffers are RGBA32F, which
//     WebGPU cannot filter, so `texture()` is emulated by `textureSH()` (manual bilinear) and
//     `texelFetch()` by `texelFetchSH()`.
//   · Shadertoy's texture space has the origin at the bottom-left; WebGPU writes rows starting
//     at the top. `texelFetchSH` / `textureSH` flip y, so all Shadertoy-style math above them
//     keeps working unchanged (including the pass's own framebuffer: `fragCoordSH`).
//   · Buffers keep their Shadertoy **role** but not their size: this port gives each one just the
//     texels it uses (the drag state is 1x6, the light cells are 64x24) instead of
//     iResolution-sized textures with a used corner. `mq_pass.screen_resolution` is the space the
//     original called `iResolution` in those passes.
//   · GLSL row-vector products (`v * M`) become `transpose(M) * v`; `mat2` constructors keep their
//     column-major argument order (`rotM`), `rotV`/`rotVR` spell out the two products.
//   · WGSL rejects assignments to swizzles and reversed `smoothstep` edges, hence `ss()` and the
//     explicit component reconstruction in the few places the original wrote `v.xy = ...`.

struct MqShaderGraphPass {
    resolution: vec2f,        // this pass's render size (px): a buffer's size, or the main target's
    time: f32,                // component `time` (s) = Shadertoy iTime
    frame: f32,               // frame counter = Shadertoy iFrame
    mouse: vec4f,             // xy = pointer in screen px (origin top-left); z = pressed(1); w unused
    keys: vec4f,              // x = Backspace, y = Shift, z = Enter (0/1); w unused
    screen_resolution: vec2f, // main target size: the space mouse.xy lives in
    pad: vec2f,               // padding to 64B
};

@group(0) @binding(0) var<uniform> mq_pass: MqShaderGraphPass;
@group(0) @binding(1) var ch0: texture_2d<f32>;
@group(0) @binding(2) var ch1: texture_2d<f32>;
@group(0) @binding(3) var ch2: texture_2d<f32>;
@group(0) @binding(4) var ch3: texture_2d<f32>;
@group(0) @binding(5) var mq_sampler: sampler;

/// helpers
const tau: f32 = atan(1.0) * 8.0; // 2.*acos( -1. )
const pi: f32 = tau / 2.0;
const tauq: f32 = tau / 4.0;

// GLSL-style smoothstep: the original calls it with edge0 > edge1 (reversed ramp) in the caustic
// mask, where WGSL's built-in is undefined.
fn ss(edge0: f32, edge1: f32, x: f32) -> f32 {
    let t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// GLSL `v * mat2(c, s, -s, c)` (row vector) and its column-vector twin `mat2(...) * v`.
fn rotM(a: f32) -> mat2x2f {
    let c = cos(a * tau);
    let s = sin(a * tau);
    return mat2x2f(vec2f(c, -s), vec2f(s, c));
}
fn rotV(a: f32, v: vec2f) -> vec2f {
    return rotM(a) * v;
}
fn rotVR(a: f32, v: vec2f) -> vec2f {
    return transpose(rotM(a)) * v;
}

// -----------------------------------------------------------------------------
// Options (top of the original Common tab)
// -----------------------------------------------------------------------------

const DISPERSION: f32 = 5.0; // dispersion of the quartz: 1 = physical, more = wider rainbows
const NLAMBDA: i32 = 7; // wavelengths for the dispersed sunlight, 4..12
const BOUNCES: i32 = 4; // segments a ray follows inside a crystal
const CRACKS: bool = true; // true = fractures with thin-film rainbows (iris quartz)
const CAUSTICS: bool = true; // rainbow caustics of the big crystal on the rock
const DUST: bool = true; // dust motes in the light shaft
const DUST_SIZE: f32 = 0.65; // 1 = 2.5 mm radius (far off at least .8 pixel)
const DUST_BRIGHT: f32 = 0.75; // lower = more transparent
const DUST_GLINT: f32 = 1.0; // 0 = no flashes
const DUST_SWAY: f32 = 1.0; // 0 = still, 1 = 6 cm sideways, 12.5 cm up and down
const STARS: bool = true; // a star on each of the NSRC brightest light centres
const STAR_GAIN: f32 = 4.0; // brightness of the stars
const STAR_GLOW: f32 = 1.32; // glow round every light centre, value / distance
const STAR_GLOW_POW: f32 = 2.0; // 1 = value / distance, 2 = 1/r^2 (glare)
const SRC_GLOW: f32 = 0.6; // glow round the sun over the whole frame
const BRIGHT_TH: f32 = 1.5; // exposed radiance above which light counts
const STAR_APART: f32 = 0.133; // centres closer than this (frame heights) count as one
const ROCK_STARS: f32 = 0.67; // stars from light on the rock: 1 = as from crystal
const STAR_E0: f32 = 0.81; // energy unit of the stars
const STAR_KNEE: f32 = 9.92; // level E/STAR_E0 / (E/STAR_E0 + KNEE)
const LEN_K: f32 = 200.0; // arm length per unit of STAR_E0 energy, 720-line pixels
const LEN_MAX: f32 = 267.0; // longest arm, 720-line pixels
const STAR_GLOW_R: f32 = 16.0; // the glow's value / distance scale, 720-line pixels
const STAR_PEAK: f32 = 3.0; // brightness of the sub-pixel core and the rays' centre lines
const STAR_W: f32 = 0.2; // rays' width at the centre, 720-line pixels
const STAR_AREA: f32 = 4.0; // light spread over this many 720-line pixels = half way soft
const SHAFT_BRIGHT: f32 = 1.0; // brightness of the light shaft in the air (original `SHAFT`)
const SUN_SWAY: f32 = 1.0; // 0 = sun fixed, 1 = the sun drifts and the caustics wander
const CAM_SWAY: f32 = 1.0; // 0 = camera fixed, 1 = slow drift
const ZOOM_IN: f32 = 0.441; // dragging up brings the camera to this share of its distance
const EXPOSURE: f32 = 1.4; // exposure before the tone map
const AA: i32 = 1; // samples per pixel axis: 1 = one, 2 = four
const TIME_SPEED: f32 = 1.0 / 4.0; // animation speed (original spEED), 1 = real time
const CLICK_RESET: bool = true; // a click (no drag) puts the camera back to the start view

// ---------- fine tuning ----------
const YAW_PER: f32 = 0.5; // turns per drag across the frame's short side
const PITCH_PER: f32 = 0.25; // turns of elevation per drag across the short side
const PITCH_LO: f32 = -0.05; // lowest stored vertical offset
const PITCH_HI: f32 = 0.5; // highest stored vertical offset
const PITCH_SOFT: f32 = 0.08; // up to this offset the elevation follows the drag 1:1, then it eases
const PITCH_MAX: f32 = 0.26; // ... smoothly towards this offset (~28 deg): no hard stop
const AIM_UP: vec3f = vec3f(0.35, 1.55, 0.38); // the orbit centre at the top of the drag
const AIM_LOOK: vec3f = vec3f(0.3, 1.0, 0.35); // where it looks at the top: into the rainbows
const CLICK_PX: f32 = 6.0; // a press that moves less than this many pixels is a click
const STATE_STAMP: f32 = 1.0; // never 0; change it with the texel map
const DRAG_WRAP: vec2f = vec2f(1.0 / YAW_PER, 0.0); // offset per full turn: yaw wraps
const NSRC: i32 = 6; // light centres found per frame, at most 6
const STAR_ROWS: f32 = 24.0; // cells per frame height in which the light is summed
const STAR_CORE: f32 = 0.3; // core scale, 720-line pixels
const SRC_PEAK: f32 = 15000.0; // a centre whose brightest sample passes this is the sun itself

// mouse pixels <-> normalised units (frame centre 0, the short side spans 1)
fn dragN(p: vec2f) -> vec2f {
    let r = mq_pass.screen_resolution;
    return (p - r * 0.5) / min(r.x, r.y);
}

// state: Buffer A column 0 (a sticky drag offset)
const TX_OFFSET: vec2i = vec2i(0, 0); // .xy the camera offset: live while held, committed after
const TX_DOWN: vec2i = vec2i(0, 1); // .x held last frame, .y drag length since the press (px)
const TX_ANCHOR: vec2i = vec2i(0, 2); // .xy the mouse at the press
const TX_START: vec2i = vec2i(0, 3); // .xy the offset at the press
const TX_HELD: vec2i = vec2i(0, 4); // .xy the mouse at the last held frame
const TX_STAMP: vec2i = vec2i(0, 5); // .x STATE_STAMP
const STATE_ROWS: i32 = 6;

// whole turns off the yaw: RGBA16F has 11 mantissa bits, an offset that grows gets coarse
fn wrapOffset(o: vec2f) -> vec2f {
    let w = DRAG_WRAP;
    return mix(o, o - w * floor(o / max(w, vec2f(1e-3)) + vec2f(0.5)), step(vec2f(1e-3), w));
}

// ---------- Shadertoy-style texture access (origin bottom-left, clamp-to-edge, bilinear) ----------

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
fn iMouse() -> vec4f {
    let m = mq_pass.mouse;
    return vec4f(m.x, mq_pass.screen_resolution.y - m.y, select(-1.0, 1.0, m.z > 0.5), 0.0);
}

// Shadertoy fragCoord: (0,0) at the bottom-left corner of the pass target.
fn fragCoordSH(frag_coord: vec4f) -> vec2f {
    return vec2f(frag_coord.x, mq_pass.resolution.y - frag_coord.y);
}

// The original's `T` macro: animation time.
fn T() -> f32 {
    return mq_pass.time * TIME_SPEED;
}
