// Buffer C — the light per cell: Buffer B's exposed radiance above BRIGHT_TH, summed over cells
// of 1/STAR_ROWS of the frame height, with its centroid. ch1 = Buffer B.
// Cell (i, j) sits in texel (i, j): x = energy in pixels of a 720-line frame times radiance, in
// hundreds (the sun alone passes the half-float maximum otherwise), yz = centroid in frame-height
// units from the bottom left, w = the brightest sample (exposed luminance): the sun itself stands
// out by orders of magnitude.
//
// Port: this pass renders into a cell-grid buffer of its own size (ceil(iResolution.x/cs) x
// STAR_ROWS), so `iResolution` here is `screen_resolution` — the frame Buffer B was rendered at.

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let iResolution = mq_pass.screen_resolution;
    let cs = iResolution.y / STAR_ROWS;
    let c = vec2i(fragCoordSH(frag_coord));
    if (f32(c.x) >= ceil(iResolution.x / cs) || f32(c.y) >= STAR_ROWS) {
        return vec4f(0.0);
    }
    let o = vec2f(c) * cs;
    let ns = i32(ceil(cs * 0.5));
    var e = 0.0;
    var pk = 0.0;
    var m = vec2f(0.0);
    // samples 2 px apart on pixel corners: each is the bilinear mean of a 2x2 block; 48 per axis
    // cover a cell up to 2304 lines (4K)
    for (var j = 0; j < 48; j++) {
        if (j >= ns) {
            break;
        }
        for (var i = 0; i < 48; i++) {
            if (i >= ns) {
                break;
            }
            let p = o + 2.0 * vec2f(f32(i), f32(j)) + vec2f(1.0);
            if (p.x > iResolution.x || p.y > iResolution.y) {
                continue;
            }
            let hb = textureSH(ch1, p / iResolution);
            let h = hb.rgb * EXPOSURE * 1.08;
            // light from rock (Buffer B alpha) counts ROCK_STARS as much
            let lu = dot(h, vec3f(0.2126, 0.7152, 0.0722)) * mix(1.0, ROCK_STARS, hb.a);
            let x = max(lu - BRIGHT_TH, 0.0);
            pk = max(pk, lu);
            e += x;
            m += x * p;
        }
    }
    let refScale = 720.0 / iResolution.y;
    return vec4f(0.04 * e * refScale * refScale,
        select(vec2f(0.0), m / (e * iResolution.y), e > 0.0), pk);
}
