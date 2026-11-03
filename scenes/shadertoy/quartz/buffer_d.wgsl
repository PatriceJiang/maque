// Buffer D — the NSRC brightest separate light centres of the frame, brightest first: where the
// Image draws its stars. ch2 = Buffer C.
// Texel (k, 0): xy = position in frame-height units from the bottom left, z = energy (0 = none),
// w = its brightest sample. A centre sums the light of every cell within STAR_APART that no
// brighter centre took, so merging stars keeps their light.
//
// Port: the buffer is exactly NSRC x 1 texels; `screen_resolution` is the frame Buffer C summed.

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let px = vec2i(fragCoordSH(frag_coord));
    if (px.y != 0 || px.x >= NSRC) {
        return vec4f(0.0);
    }
    let iResolution = mq_pass.screen_resolution;
    let cs = iResolution.y / STAR_ROWS;
    let n = vec2i(i32(ceil(iResolution.x / cs)), i32(STAR_ROWS));
    // the centres found so far, two per vec4 (no array: an indexed write in a loop unrolls on ANGLE)
    var p01 = vec4f(-1e3);
    var p23 = vec4f(-1e3);
    var p45 = vec4f(-1e3);
    for (var k = 0; k <= px.x; k++) {
        // the brightest cell that is not part of a centre found before
        var b = vec4f(-1e3, -1e3, 0.0, 0.0);
        for (var j = 0; j < n.y; j++) {
            for (var i = 0; i < n.x; i++) {
                let c = texelFetchSH(ch2, vec2i(i, j));
                if (c.x <= b.z) {
                    continue;
                }
                let q = c.yz;
                let d = min(
                    min(min(length(q - p01.xy), length(q - p01.zw)),
                        min(length(q - p23.xy), length(q - p23.zw))),
                    min(length(q - p45.xy), length(q - p45.zw)));
                if (d >= STAR_APART) {
                    b = vec4f(q, c.x, c.w);
                }
            }
        }
        if (k == px.x) {
            // the centre carries all the light round it that no brighter centre took: merged stars
            // add up to the light of the separate ones, the position is that light's centroid
            if (b.z <= 0.0) {
                return b;
            }
            var e = 0.0;
            var pk = 0.0;
            var mc = vec2f(0.0);
            for (var j = 0; j < n.y; j++) {
                for (var i = 0; i < n.x; i++) {
                    let c = texelFetchSH(ch2, vec2i(i, j));
                    if (c.x <= 0.0 || length(c.yz - b.xy) >= STAR_APART) {
                        continue;
                    }
                    let q = c.yz;
                    let d = min(
                        min(min(length(q - p01.xy), length(q - p01.zw)),
                            min(length(q - p23.xy), length(q - p23.zw))),
                        min(length(q - p45.xy), length(q - p45.zw)));
                    if (d < STAR_APART) {
                        continue;
                    }
                    e += c.x;
                    mc += c.x * c.yz;
                    pk = max(pk, c.w);
                }
            }
            // the sun alone nears the half-float maximum
            return vec4f(mc / e, min(e, 6e4), pk);
        }
        if (k == 0) {
            p01 = vec4f(b.xy, p01.zw);
        } else if (k == 1) {
            p01 = vec4f(p01.xy, b.xy);
        } else if (k == 2) {
            p23 = vec4f(b.xy, p23.zw);
        } else if (k == 3) {
            p23 = vec4f(p23.xy, b.xy);
        } else if (k == 4) {
            p45 = vec4f(b.xy, p45.zw);
        } else {
            p45 = vec4f(p45.xy, b.xy);
        }
    }
    return vec4f(0.0);
}
