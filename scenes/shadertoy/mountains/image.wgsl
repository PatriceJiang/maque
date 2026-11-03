// Image — raymarched terrain rendering (Shadertoy "Image" tab).
//
// The Image buffer handles raymarched terrain rendering based on the eroded heightmap
// from Buffer B (which is based on the raw heightmap from Buffer A) and the noise
// texture from Buffer C. Terrain rendering derived from https://www.shadertoy.com/view/7ljcRW
// by Fewes, with several changes (normals here, cheap march map, packed layers, drainage
// patterns and trees, water cut-off fix).
//
// Channels in the graph: ch0 = Buffer B (height + packed data), ch1 = Buffer C (detail
// noise). The original also used ch2 (a dither noise texture); the port replaces that
// with a deterministic hash of the pixel position — same 1/255 magnitude.

// -----------------------------------------------------------------------------
// Functions for retrieving map data from Buffer B and C
// -----------------------------------------------------------------------------

// Buffer textures are exactly BUFFER_SIZE² (square); Shadertoy instead stored the square
// inside an iResolution-sized texture and rescaled uv. Here uv maps 1:1.
fn bufferSize() -> vec2f {
    return vec2f(textureDimensions(ch0));
}

// Get the map channels from Buffer B, except the packed data.
// This is cheaper for usages that don't need the packed data anyway.
fn GetChannel0(uv: vec2f) -> vec3f {
    return textureSH(ch0, uv).xyz;
}

// Get detail texture from Buffer C.
fn GetChannel1(uv: vec2f) -> vec4f {
    return textureSH(ch1, uv);
}

struct MapChannels {
    tex: vec3f,
    data: vec4f,
};

// Get map channels from Buffer B, including the packed data.
fn GetChannel0Data(uv: vec2f) -> MapChannels {
    let p = uv * bufferSize() - vec2f(0.5);

    let i = vec2i(p);
    let b = fract(p);
    let a = vec2f(1.0) - b;

    // Get the four texels directly, rather than an interpolated result.
    let AA = texelFetchSH(ch0, i);
    let AB = texelFetchSH(ch0, i + vec2i(0, 1));
    let BA = texelFetchSH(ch0, i + vec2i(1, 0));
    let BB = texelFetchSH(ch0, i + vec2i(1, 1));
    // Use regular interpolation for the normal return value.
    // The w channel should not be used.
    let ret = (AA * a.y + AB * b.y) * a.x + (BA * a.y + BB * b.y) * b.x;

    // Unpack each of the four samples into a vec4.
    let AAdata = unpack4(AA.w);
    let ABdata = unpack4(AB.w);
    let BAdata = unpack4(BA.w);
    let BBdata = unpack4(BB.w);
    // Interpolate the four vec4s so each piece of data is correctly interpolated.
    let data = (AAdata * a.y + ABdata * b.y) * a.x + (BAdata * a.y + BBdata * b.y) * b.x;

    var result: MapChannels;
    result.tex = ret.xyz;
    result.data = data;
    return result;
}

fn GetUV(p: vec3f) -> vec2f {
    let pixel = vec2f(1.0) / bufferSize();
    var uv = p.xz * (vec2f(1.0) - pixel * 2.0) + vec2f(0.5);
    uv = clamp(uv, pixel, vec2f(1.0) - pixel);
    return uv;
}

// Get just the map height. This is faster for ray marching.
fn mapHeight(uv: vec2f) -> f32 {
    return GetChannel0(uv).x;
}

struct MapData {
    height: f32,
    normal: vec3f,
    erosion: f32,
    ridgemap: f32,
    trees: f32,
    debug: f32,
};

// Get all map channels. This is needed for texturing and shading.
fn mapTerrain(uv: vec2f) -> MapData {
    let ch = GetChannel0Data(uv);
    let tex = ch.tex;

    let height = tex.x;

    // Calculate an accurate normal from the neighbouring points.
    let dims = bufferSize();
    let uv1 = uv + vec2f(1.0 / dims.x, 0.0);
    let uv2 = uv + vec2f(0.0, 1.0 / dims.y);
    let h1 = GetChannel0(uv1).x;
    let h2 = GetChannel0(uv2).x;
    let v1 = vec3f(uv1 - uv, (h1 - height));
    let v2 = vec3f(uv2 - uv, (h2 - height));
    let normal = normalize(cross(v1, v2)).xzy;

    var result: MapData;
    result.height = height;
    result.normal = normal;
    result.erosion = ch.data.x * 2.0 - 1.0;
    result.ridgemap = ch.data.y;
    result.trees = ch.data.z;
    result.debug = ch.data.w;
    return result;
}

// -----------------------------------------------------------------------------
// Ray marching the terrain height function
// -----------------------------------------------------------------------------

// The box size the terrain is contained inside.
const boxSize = vec3f(0.5, 1.0, 0.5);

struct MarchResult {
    t: f32,
    normal: vec3f,
    material: i32,
    s_t: f32,
};

fn march(ro: vec3f, rd: vec3f) -> MarchResult {
    var res: MarchResult;
    res.t = -1.0;
    res.normal = vec3f(0.0);
    res.material = M_GROUND;
    res.s_t = 9999.0;

    var boxNormal = vec3f(0.0);
    let box = boxIntersection(ro, rd, boxSize, &boxNormal);

    let tStart = max(0.0, box.x) + 1e-2;
    let tEnd = box.y - 1e-2;

    var stepSize = 0.0;
    var stepScale = 1.0 / RAYMARCH_QUALITY;
    let samples = i32(48.0 * RAYMARCH_QUALITY);
    var t = tStart;

    for (var i = 0; i < samples; i = i + 1) {
        let pos = ro + rd * t;

        let h = mapHeight(GetUV(pos));
        let altitude = pos.y - h;

        res.s_t = max(0.0, min(res.s_t, altitude / t));

        if (altitude < 0.0) {
            if (i < 1) { // Sides
                if (pos.y < 0.35) { // Flat bottom
                    res.s_t = 9999.0;
                    return res;
                }
                res.normal = boxNormal;
                res.material = M_STRATA;
                break;
            }
        }

        if (altitude < 0.0) {
            // Step back (contact/edge refinement).
            stepScale *= 0.5;
            t -= stepSize * stepScale;
        } else {
            // Step forward, accelerating by distance to terrain.
            // Refinement above keeps this from aliasing badly.
            stepSize = abs(altitude) + min(1e-2, abs(altitude) * 0.01);
            t += stepSize * stepScale;
        }
    }

    var waterNormal = vec3f(0.0);
    let water = boxIntersection(ro, rd, vec3f(boxSize.x, WATER_HEIGHT, boxSize.z), &waterNormal);
    if ((water.y > 0.0 && (water.x < t || t < 0.0)) && res.material != M_STRATA) {
        t = max(0.0, water.x);
        res.normal = waterNormal;
        res.material = M_WATER;
    }

    if (box.y < 0.0) {
        res.s_t = 9999.0;
        return res;
    }

    if (t > tEnd) {
        return res;
    }

    res.t = t;
    return res;
}

fn GetReflection(p: vec3f, r: vec3f, sun: vec3f, smoothness: f32) -> vec3f {
    let refl = SkyColor(r, sun) * 4.0;
    let hit = march(p, r);
    return refl * (1.0 - exp(-hit.s_t * 10.0 * sq(smoothness)));
}

// -----------------------------------------------------------------------------
// Main image output
// -----------------------------------------------------------------------------

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let fragCoord = fragCoordSH(frag_coord);
    let iResolution = mq_pass.resolution;

    // ------------------------------------------------------------------------
    // Set up camera
    // ------------------------------------------------------------------------

    let mouse = iMouse();
    let ray = GetRay(mq_pass.time, mouse, iResolution, fragCoord, 0.0);
    let mouseRay = GetRay(mq_pass.time, mouse, iResolution, mouse.xy, 0.0);
    let cursor = GetMouseWorldPos(ray.ro, mouseRay.rd, 0.45);

    // ------------------------------------------------------------------------
    // Ray march
    // ------------------------------------------------------------------------

    let hit = march(ray.ro, ray.rd);
    let t = hit.t;
    var normal = hit.normal;
    let material = hit.material;

    // ------------------------------------------------------------------------
    // Coloring and shading
    // ------------------------------------------------------------------------

    let sun = normalize(vec3f(-1.0, 0.4, 0.05));

    let fogColor = vec3f(1.0) - exp(-SkyColor(ray.rd, sun) * 2.0);

    var color = vec3f(0.0);

    if (t < 0.0) {
        // Sky
        color = fogColor * (1.0 + pow(fragCoord.y / iResolution.y, 3.0) * 3.0) * 0.5;
    } else {
        let pos = ray.ro + ray.rd * t;

        let mapData = mapTerrain(GetUV(pos));
        let erosion = mapData.erosion;
        let ridgemap = mapData.ridgemap;
        let trees = mapData.trees;

        let drainage = clamp01((1.0 - clamp01(ridgemap / DRAINAGE_WIDTH)) * 1.5);
        let diff = pos.y - mapData.height;

        var breakup = 0.0;
        let breakupTex = GetChannel1(GetUV(pos));
        breakup = breakupTex.x;
        if (material == M_WATER) {
            normal = vec3f(normal.x + breakupTex.z * 0.1, normal.y, normal.z + breakupTex.y * 0.1);
            normal = normalize(normal);
        }

        let f0 = vec3f(0.04);
        var smoothness = 0.0;
        var occlusion = 1.0;

        let r = reflect(ray.rd, normal);

        // Color used when grayscale debug option is used:
        var diffuseColor = vec3f(0.5);

        if (material == M_GROUND) {
            normal = mapData.normal;

            occlusion = clamp01(erosion + 0.5);

            // Cliffs / Dirt
            diffuseColor = CLIFF_COLOR * smoothstep_(0.4, 0.52, pos.y);
            diffuseColor = mix(diffuseColor, DIRT_COLOR, smoothstep_(0.6, 0.0, occlusion + breakup * 1.5));

            // Snow
            diffuseColor = mix(diffuseColor, vec3f(1.0), smoothstep_(0.53, 0.6, pos.y + breakup * 0.1));
            // Sand (beach)
            diffuseColor = mix(diffuseColor, SAND_COLOR,
                smoothstep_(WATER_HEIGHT + 0.005, WATER_HEIGHT, pos.y + breakup * 0.01));

            // Grass
            let grassMix = mix(GRASS_COLOR1, GRASS_COLOR2,
                smoothstep_(0.4, 0.6, pos.y - erosion * 0.05 + breakup * 0.3));
            diffuseColor = mix(diffuseColor, grassMix,
                smoothstep_(GRASS_HEIGHT + 0.05, GRASS_HEIGHT + 0.02,
                    pos.y + 0.01 + (occlusion - 0.8) * 0.05 - breakup * 0.02)
                * smoothstep_(0.8, 1.0, 1.0 - (1.0 - normal.y) * (1.0 - trees) + breakup * 0.1));

            // Trees
            diffuseColor = mix(diffuseColor, TREE_COLOR * pow(trees, 8.0),
                clamp01(trees * 2.2 - 0.8) * 0.6);

            diffuseColor *= 1.0 + breakup * 0.5;

            // Drainage (rivers, creeks, debris flow)
            diffuseColor = mix(diffuseColor, vec3f(1.0), drainage);
        } else if (material == M_STRATA) {
            let strata = smoothstep3_(0.0, 1.0, cos(diff * vec3f(130.0, 190.0, 250.0)));
            diffuseColor = vec3f(0.3);
            diffuseColor = mix(diffuseColor, vec3f(0.50), strata.x);
            diffuseColor = mix(diffuseColor, vec3f(0.55), strata.y);
            diffuseColor = mix(diffuseColor, vec3f(0.60), strata.z);

            diffuseColor *= exp(diff * 10.0) * vec3f(1.0, 0.9, 0.7);
        } else if (material == M_WATER) {
            var shore = 0.0;
            var foam = 0.0;
            if (normal.y > 1e-2) {
                shore = exp(-diff * 60.0);
                foam = smoothstep_(0.005, 0.0, diff + breakup * 0.005);
            }

            diffuseColor = mix(WATER_COLOR, WATER_SHORE_COLOR, shore);
            diffuseColor = mix(diffuseColor, vec3f(1.0), foam);

            smoothness = 0.95;
        }

        // Brush radius indicator
        if (mouse.z > 0.0 && material != M_STRATA) {
            let cursorDist = distance(pos.xz, cursor.xz);
            let val = 1.0 - clamp01(min(200.0 * abs(cursorDist - BRUSH_SIZE * 0.2),
                0.5 + 200.0 * abs(cursorDist - BRUSH_SIZE)));
            diffuseColor = mix(diffuseColor, vec3f(0.0, 1.0, 1.0), val);
        }

        var shadow = 1.0;

        if (material != M_STRATA) {
            // Shadow ray
            let shadowHit = march(pos + vec3f(0.0, 1.0, 0.0) * 1e-4, sun);
            shadow = 1.0 - exp(-shadowHit.s_t * 20.0);
        }

        // Ambient
        color = diffuseColor * SkyColor(normal, sun) * Fd_Lambert();
        color *= occlusion;
        // Direct
        color += Shade(diffuseColor, f0, smoothness, normal, -ray.rd, sun, SUN_COLOR * shadow);
        // Bounce
        color += diffuseColor * SUN_COLOR
            * (dot(normal, sun * vec3f(1.0, -1.0, 1.0)) * 0.5 + 0.5)
            * Fd_Lambert() / PI;
        // Reflection
        color += GetReflection(pos, r, sun, smoothness)
            * F_Schlick3(f0, dot(-ray.rd, normal));
    }

    // ------------------------------------------------------------------------
    // Atmosphere rendering
    // ------------------------------------------------------------------------

    var boxNormal = vec3f(0.0);
    let box = boxIntersection(ray.ro, ray.rd, boxSize, &boxNormal);

    let costh = dot(ray.rd, sun);
    let phaseR = PhaseRayleigh(costh);
    let phaseM = PhaseMie(costh, 0.6);

    var od = vec2f(0.0);
    var tsm = vec3f(0.0);
    var sct = vec3f(0.0);
    let rayLength = select(box.y, t, t > 0.0) - box.x;
    let stepSize = rayLength / 16.0;
    for (var i = 0.0; i < 16.0; i = i + 1.0) {
        let p = ray.ro + ray.rd * (box.x + (i + 0.5) * stepSize);

        let h = max(0.0, p.y - 0.35);
        var d = 1.0 - clamp01(h / 0.2);

        if (p.y < 0.35) {
            d = 0.0;
        }

        let densityR = d * 1e5;
        let densityM = d * 1e5;

        od += stepSize * vec2f(densityR, densityM);

        tsm = exp(-(od.x * C_RAYLEIGH + od.y * C_MIE));

        sct += tsm * C_RAYLEIGH * phaseR * densityR * stepSize;
        sct += tsm * C_MIE * phaseM * densityM * stepSize;
    }

    color = color * tsm + sct * 10.0;

    // ------------------------------------------------------------------------
    // Tone mapping and dithering
    // ------------------------------------------------------------------------

    color = Tonemap_ACES(color);
    color = pow(color, vec3f(1.0 / 2.2));

    // Dither. The original tiled a noise texture from iChannel2; a deterministic hash of
    // the pixel position has the same magnitude (1/255) and no texture channel.
    let dither = hash(fragCoord).x * 0.5 + 0.5;
    color += vec3f(dither / 255.0);

    return vec4f(color, 1.0);
}
