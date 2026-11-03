// Image — the tone map and a star on each light centre. ch1 = Buffer B (the scene in HDR),
// ch3 = Buffer D (the light centres). Options: common.wgsl.
//
// Port: the original ran this at iResolution with Buffer B at the same size, so `texelFetch` lined
// up 1:1. Here Buffer B keeps the scene's aspect (960x540 at native size) while the main target
// follows the window, so the Buffer B texel is fetched with `fragCoord / resolution * bufferSize`
// instead — identical when the two sizes match, and a plain nearest upscale when they do not
// (Buffer B is what it is at native resolution; the stars are re-drawn at the target's res).

// w/(|x| + w) averaged over one pixel [d - .5, d + .5] (d, w in screen pixels), closed form: a
// profile narrower than a pixel puts its light into that pixel exactly -- very bright at the
// centre, then the long value / distance fall -- with no shimmer as it moves
fn pixVD(dIn: f32, w: f32) -> f32 {
    var d = abs(dIn);
    if (d >= 0.5) {
        return w * log((d + 0.5 + w) / (d - 0.5 + w));
    }
    return w * (log((d + 0.5 + w) / w) + log((0.5 - d + w) / w));
}

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let iResolution = mq_pass.resolution;
    let fragCoord = fragCoordSH(frag_coord);
    let bDims = vec2f(textureDimensions(ch1));
    let bUV = fragCoord * bDims / iResolution / bDims;
    var col = textureSH(ch1, bUV).rgb * EXPOSURE * 1.08;
    if (STARS) {
        // a star on each light centre, every profile value / distance -- never a Gaussian, never cut:
        // each falloff is  w/(d + w) - w/(D + w),  value / distance minus its own value at its end D,
        // so it reaches zero there smoothly. The level x/(x + STAR_KNEE) saturates (value / distance
        // too, never a white blob); the length grows so that level x length stays proportional to the
        // light: a star merged from several carries their sum, up to LEN_MAX. The more spread the
        // light, the wider and dimmer the arms (the same light). Hidden light is no light.
        let uv = fragCoord / iResolution.y;
        for (var k = 0; k < NSRC; k++) {
            let s = texelFetchSH(ch3, vec2i(k, 0));
            if (s.z <= 0.0) {
                continue;
            }
            let x = s.z / STAR_E0;
            let lv = (6.7 * x) / pow(3.33 * x + STAR_KNEE, 1.25); // level
            let area = 100.0 * s.z / max(s.w - BRIGHT_TH, 1e-3); // 720-line pixels the light covers
            let spread = mix(1.0, 3.0, area / (area + STAR_AREA)); // 1 = a point, 3 = spread out
            // the sun itself (its peak is far above any reflection): rays 3x as wide, 2x as long
            let src = ss(0.5 * SRC_PEAK, SRC_PEAK, s.w);
            let wid = mix(1.0, 3.0, src) * spread;
            let soft = mix(1.0, 2.0, src);
            let dp = (uv - s.xy) * 720.0; // pixels of a 720-line frame
            let r = length(dp);
            let len = min(LEN_K * (x + STAR_KNEE), LEN_MAX) * soft;
            // the sun lights the whole frame from its centre: value / distance
            col += vec3f(1.0, 0.80, 0.55) * SRC_GLOW * src * lv * 20.0 / (r + 20.0);
            if (src >= 1.0 || r > len) {
                continue; // the sun itself gets no star, only its glow
            }
            let kp = iResolution.y / 720.0; // screen pixels per 720-line pixel
            // the core: sub-pixel, its pixel-averaged value / distance (radial: the distance to the
            // pixel's own mean, at least half a pixel), minus its value at the reach
            let cr = STAR_CORE * spread * kp;
            let cR = 0.5 * len * kp;
            let rp = sqrt(r * r * kp * kp + 0.16);
            var st = STAR_PEAK * 0.25 * max(cr / (rp + cr) - cr / (cR + cr), 0.0);
            // the light's own glow round the centre: value / distance over the star's whole reach.
            // Its scale grows with the star and stays small against its length (sub-pixel on small
            // stars), so the glow is a sharp 1/r on every star, never a round hump.
            let gc = max(STAR_GLOW_R * len / 150.0, 0.1);
            let rg = sqrt(r * r + 0.16 / (kp * kp));
            // value / distance raised to STAR_GLOW_POW (2: falls as 1/r^2), reaching zero at half the
            // star's length; the rays reach further
            let gR = 0.5 * len;
            let g0 = gc / (gR + gc);
            let gq = max(gc / (rg + gc) - g0, 0.0) / (1.0 - g0);
            st += STAR_GLOW * pow(gq, STAR_GLOW_POW);
            for (var j = 0; j < 3; j++) {
                let an = f32(j) * tau / 6.0 + 0.3;
                let dir = vec2f(cos(an), sin(an));
                let al = abs(dot(dp, dir));
                let ac = abs(dot(dp, vec2f(-dir.y, dir.x)));
                // across: sub-pixel wide, pixel-averaged value / distance, reaching zero 40 widths
                // out; along: value / distance over the length
                let w = (STAR_W / 2.0 + 0.0001 * al) * wid * kp;
                let lw = 0.35 * len;
                let across = max(pixVD(ac * kp, w) - w * log(1.0 + 1.0 / (40.0 * w + 0.5)), 0.0);
                st += pow(STAR_PEAK * 1.67 * across
                    * max(lw / (al + lw) - lw / (len + lw), 0.0), 1.36);
            }
            col += vec3f(1.0, 0.80, 0.55) * STAR_GAIN * lv * st / spread * (1.0 - src);
        }
    }
    col = col / (vec3f(1.0) + col);
    col = pow(max(col, vec3f(0.0)), vec3f(0.4545));
    col *= vec3f(1.035, 1.0, 0.955);
    let q = fragCoord / iResolution;
    col *= 0.55 + 0.45 * pow(max(16.0 * q.x * q.y * (2.0 - q.x) * (1.0 - q.y), 0.0), 0.2);
    return vec4f(col, 1.0);
}
