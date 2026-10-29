struct BlitVertexOut {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs(@builtin(vertex_index) index: u32) -> BlitVertexOut {
    var positions = array<vec2f, 3>(
        vec2f(-1.0, -1.0),
        vec2f( 3.0, -1.0),
        vec2f(-1.0,  3.0),
    );
    var uvs = array<vec2f, 3>(
        vec2f(0.0,  1.0),
        vec2f(2.0,  1.0),
        vec2f(0.0, -1.0),
    );
    var out: BlitVertexOut;
    out.position = vec4f(positions[index], 0.0, 1.0);
    out.uv = uvs[index];
    return out;
}

@group(0) @binding(0) var source_texture: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;

@fragment
fn fs(in: BlitVertexOut) -> @location(0) vec4f {
    return textureSample(source_texture, source_sampler, in.uv);
}
