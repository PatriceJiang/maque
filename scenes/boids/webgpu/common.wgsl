// Boids · WebGPU shader graph 公共头（scenes/boids/boids-webgpu.scn）。
// 模拟：pos/vel 两张 N×1 RGBA32F 状态纹理；每帧两个全屏 pass 做 O(N²) 邻域力 + 积分。
// 渲染：将 N 个 boid 以软点累加到 384×216 HDR 图，再上采样/tonemap 到屏幕。
// 全部在 shader 里：没有 JS、没有 CPU 回读。N 必须是编译期常量（两个 pass 的循环上界一致）。
struct MqShaderGraphPass {
    resolution: vec2f,
    time: f32,
    frame: f32,
    mouse: vec4f,
    keys: vec4f,
    screen_resolution: vec2f,
    pad: vec2f,
};

@group(0) @binding(0) var<uniform> mq_pass: MqShaderGraphPass;
@group(0) @binding(1) var ch0: texture_2d<f32>;
@group(0) @binding(2) var ch1: texture_2d<f32>;
@group(0) @binding(3) var ch2: texture_2d<f32>;
@group(0) @binding(4) var ch3: texture_2d<f32>;
@group(0) @binding(5) var mq_sampler: sampler;

const BOIDS: i32 = 256;
const DT: f32 = 1.0 / 60.0;
const WORLD: f32 = 9.0;
const SEP_R: f32 = 1.5;
const ALI_R: f32 = 3.2;
const MAX_SPEED: f32 = 5.0;
const MIN_SPEED: f32 = 1.2;

fn hash11(n: u32) -> f32 {
    var x = n * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    x = (x >> 22u) ^ x;
    return f32(x & 0xffffffu) / 16777216.0;
}

fn init_pos(i: i32) -> vec3f {
    let u = u32(i);
    return vec3f(
        (hash11(u * 3u + 1u) * 2.0 - 1.0) * WORLD,
        (hash11(u * 3u + 2u) * 2.0 - 1.0) * WORLD,
        (hash11(u * 3u + 3u) * 2.0 - 1.0) * WORLD);
}

fn init_vel(i: i32) -> vec3f {
    let u = u32(i);
    let az = hash11(u * 7u + 11u) * 6.2831853;
    let el = (hash11(u * 7u + 13u) * 2.0 - 1.0) * 1.2;
    let sp = 2.0 + hash11(u * 7u + 17u) * 2.0;
    let c = cos(el);
    return vec3f(cos(az) * c, sin(el), sin(az) * c) * sp;
}

fn boid_index(fc: vec4f) -> i32 {
    return clamp(i32(fc.x), 0, BOIDS - 1);
}

fn load_state(t: texture_2d<f32>, i: i32) -> vec4f {
    return textureLoad(t, vec2i(i, 0), 0);
}

fn aces(x: vec3f) -> vec3f {
    let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3f(0.0), vec3f(1.0));
}

/* 双线性采样（RGBA32F 不可过滤）。 */
fn sample_bilinear(t: texture_2d<f32>, uv: vec2f) -> vec4f {
    let dims = vec2f(textureDimensions(t));
    let idim = vec2i(i32(dims.x), i32(dims.y));
    let p = clamp(uv * dims - 0.5, vec2f(0.0), dims - 1.0);
    let i0 = vec2i(floor(p));
    let i1 = min(i0 + vec2i(1), idim - 1);
    let f = fract(p);
    let a = textureLoad(t, i0, 0);
    let b = textureLoad(t, vec2i(i1.x, i0.y), 0);
    let c = textureLoad(t, vec2i(i0.x, i1.y), 0);
    let d = textureLoad(t, i1, 0);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
