// 主输出阶段后处理（PostEffect；docs/13 §后处理 / docs/28 F4 面板）。
//
// 输入 = 一块已成像的 LDR 显示空间纹理：默认是 **3D 层**（shader pass + 3D + 天空），
// `include_2d=1` 时是 3D+2D/UI 同帧合成后的整帧；输出 = 同一尺寸的目标
// （swapchain / 离屏回读纹理）。
// 顺序：采样（含色差）→ 曝光 → 色调映射 → Bloom → 暗角 → 色彩分级。
// 所有效果由 uniform 的 bitmask + 参数驱动；`enabled=0` 时 gfx 不跑这个 pass（旧画面逐字节不变）。
//
// uniform（64B，与 C 侧 PostUniform 逐字段对齐）：
//   0  exposure(f32)      4  tonemap(i32: 0=none 1=ACES)
//   8  enabled(i32)      12  effects(u32: bit0 bloom / bit1 vignette / bit2 chromatic / bit3 grade)
//  16  bloom_intensity  20  bloom_threshold  24  bloom_radius  28  vignette
//  32  aberration       36  saturation       40  contrast      44  grayscale
//  48  resolution(vec2f) 56  texel(vec2f)

struct PostUniform {
    exposure: f32,
    tonemap: i32,
    enabled: i32,
    effects: u32,
    bloom_intensity: f32,
    bloom_threshold: f32,
    bloom_radius: f32,
    vignette: f32,
    aberration: f32,
    saturation: f32,
    contrast: f32,
    grayscale: f32,
    resolution: vec2f,
    texel: vec2f,
};

@group(0) @binding(0) var<uniform> post: PostUniform;
@group(0) @binding(1) var source_texture: texture_2d<f32>;
@group(0) @binding(2) var source_sampler: sampler;

struct PostVertexOut {
    @builtin(position) position: vec4f,
};

// 全屏三角形（无顶点缓冲）：覆盖整个目标，片元里用 frag_coord 还原 UV。
@vertex
fn vs(@builtin(vertex_index) index: u32) -> PostVertexOut {
    var positions = array<vec2f, 3>(
        vec2f(-1.0, -1.0),
        vec2f( 3.0, -1.0),
        vec2f(-1.0,  3.0),
    );
    var out: PostVertexOut;
    out.position = vec4f(positions[index], 0.0, 1.0);
    return out;
}

fn luma(c: vec3f) -> f32 {
    return dot(c, vec3f(0.2126, 0.7152, 0.0722));
}

// ACES 近似（与 pbr shader 的 tonemap 同一曲线，光栅化后仍连续）。
fn tonemap_aces(c: vec3f) -> vec3f {
    let a = 2.51;
    let b = 0.03;
    let cc = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((c * (a * c + b)) / (c * (cc * c + d) + e), vec3f(0.0), vec3f(1.0));
}

// 曝光 + 色调映射（所有采样点走同一函数：Bloom 阈值比较的是同一色彩空间）。
fn shade(c: vec3f) -> vec3f {
    var v = c * post.exposure;
    if (post.tonemap == 1) {
        v = tonemap_aces(v);
    }
    return v;
}

fn sample_at(uv: vec2f) -> vec3f {
    return shade(textureSample(source_texture, source_sampler, clamp(uv, vec2f(0.0), vec2f(1.0))).rgb);
}

@fragment
fn fs(@builtin(position) frag: vec4f) -> @location(0) vec4f {
    let uv = frag.xy * post.texel;
    var col = sample_at(uv);

    // 色差：R/B 沿"离屏幕中心"方向反向偏移，G 保持在原位置（径向色散）。
    if ((post.effects & 4u) != 0u && post.aberration > 0.0) {
        let off = (uv - vec2f(0.5, 0.5)) * post.aberration;
        col.r = sample_at(uv - off).r;
        col.b = sample_at(uv + off).b;
    }

    // Bloom：阈值以上的亮度按 12 个黄金角螺旋采样累积（固定序列 → 每帧确定、无噪声抖动）。
    if ((post.effects & 1u) != 0u && post.bloom_intensity > 0.0) {
        let radius = max(post.bloom_radius, 1.0e-5);
        // uv 的 y 分量约占目标高度；x 按宽高比压缩 → 采样圆在像素空间是正圆。
        let aspect = max(post.resolution.x, 1.0) / max(post.resolution.y, 1.0);
        var acc = vec3f(0.0);
        for (var i = 0; i < 12; i = i + 1) {
            let fi = f32(i);
            let ang = fi * 2.3999632;                     // 黄金角（rad）
            let rad = sqrt((fi + 0.5) / 12.0) * radius;   // 圆盘内近似均匀
            let dir = vec2f(cos(ang) / aspect, sin(ang));
            let c = sample_at(uv + dir * rad);
            acc = acc + c * max(luma(c) - post.bloom_threshold, 0.0);
        }
        col = col + acc * (post.bloom_intensity / 12.0);
    }

    // 暗角：按归一化半径压暗边缘（中心不动）。
    if ((post.effects & 2u) != 0u && post.vignette > 0.0) {
        let q = (uv - vec2f(0.5, 0.5)) * 2.0;
        let falloff = 1.0 - post.vignette * smoothstep(0.55, 1.45, length(q));
        col = col * clamp(falloff, 0.0, 1.0);
    }

    // 色彩分级：饱和度 → 对比度（绕 0.5）→ 灰度混合。
    if ((post.effects & 8u) != 0u) {
        let l = luma(col);
        col = mix(vec3f(l), col, post.saturation);
        col = (col - vec3f(0.5, 0.5, 0.5)) * post.contrast + vec3f(0.5, 0.5, 0.5);
        col = mix(col, vec3f(luma(col)), clamp(post.grayscale, 0.0, 1.0));
    }

    return vec4f(clamp(col, vec3f(0.0), vec3f(1.0)), 1.0);
}
