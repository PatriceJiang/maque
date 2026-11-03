// Buffer B — Advanced Terrain Erosion Filter (Shadertoy "Buffer B" tab).
//
// Advanced terrain erosion filter based on stacked faded gullies, with controls for
// erosion strength, detail, ridge and crease rounding, and a ridge map output useful
// for e.g. water drainage. For more on the technique, see:
// https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html
//
// This buffer has three parts: Phacelle Noise, the erosion function, and the demo.
// Phacelle Noise function copyright (c) 2025 Rune Skovbo Johansen — MPL 2.0.
// Advanced Terrain Erosion Filter copyright (c) 2025 Rune Skovbo Johansen — MPL 2.0.
// Derived from versions by Clay John (MtGcWh) and Fewes (7ljcRW).
//
// Inputs: ch0 = Buffer A raw height map (height + slope). The original also sampled the
// keyboard texture (Enter toggles erosion); here that state is `keys.z`.

// -----------------------------------------------------------------------------
// PHACELLE NOISE FUNCTION
// -----------------------------------------------------------------------------

const TAU: f32 = 6.28318530717959;

// The Simple Phacelle Noise function produces a stripe pattern aligned with the input vector.
//  - p is the input point being evaluated.
//  - normDir is the direction of the stripes at this point. It must be a normalized vector.
//  - freq is the frequency of the stripes within each cell.
//  - offset is the phase offset of the stripes, where 1.0 is a full cycle.
//  - normalization is the degree of normalization applied, between 0 and 1.
fn PhacelleNoise(p: vec2f, normDir: vec2f, freq: f32, offset_in: f32, normalization: f32) -> vec4f {
    // Get a vector orthogonal to the input direction, with a
    // magnitude proportional to the frequency of the stripes.
    let sideDir = normDir.yx * vec2f(-1.0, 1.0) * freq * TAU;
    let offset = offset_in * TAU;

    // Iterate over 4x4 cells, calculating a stripe pattern for each and blending between them.
    let pInt = floor(p);
    let pFrac = fract(p);
    var phaseDir = vec2f(0.0);
    var weightSum = 0.0;
    for (var i = -1; i <= 2; i = i + 1) {
        for (var j = -1; j <= 2; j = j + 1) {
            let gridOffset = vec2f(f32(i), f32(j));

            // The final cell point is gridPoint + randomOffset; relative vector:
            // p - (gridPoint + randomOffset) = pFrac - gridOffset - randomOffset
            let gridPoint = pInt + gridOffset;
            let randomOffset = hash(gridPoint) * 0.5;
            let vectorFromCellPoint = pFrac - gridOffset - randomOffset;

            // Bell-shaped weight function which is 1 at dist 0 and nearly 0 at dist 1.5.
            let sqrDist = dot(vectorFromCellPoint, vectorFromCellPoint);
            var weight = exp(-sqrDist * 2.0);
            // Subtract 0.01111 to make the function actually 0 at distance 1.5.
            weight = max(0.0, weight - 0.01111);
            weightSum += weight;

            // Add this cell's cosine and sine wave contributions to the interpolated value.
            let waveInput = dot(vectorFromCellPoint, sideDir) + offset;
            phaseDir += vec2f(cos(waveInput), sin(waveInput)) * weight;
        }
    }

    // Get the raw interpolated value.
    let interpolated = phaseDir / weightSum;
    // Interpret the value as a vector whose length represents the magnitude of both waves.
    var magnitude = sqrt(dot(interpolated, interpolated));
    // Apply a lower threshold to show small magnitudes we're going to fully normalize.
    magnitude = max(1.0 - normalization, magnitude);
    // Return normalized cosine/sine waves plus the direction vector.
    return vec4f(interpolated / magnitude, sideDir);
}

// -----------------------------------------------------------------------------
// EROSION FUNCTION
// -----------------------------------------------------------------------------

fn pow_inv(t: f32, power: f32) -> f32 {
    // Flip, raise to the specified power, and flip back.
    return 1.0 - pow(1.0 - clamp01(t), power);
}

fn ease_out(t: f32) -> f32 {
    // Flip by subtracting from one, raise to a power of two, flip back.
    let v = 1.0 - clamp01(t);
    return 1.0 - v * v;
}

fn smooth_start(t: f32, smoothing: f32) -> f32 {
    if (t >= smoothing) {
        return t - 0.5 * smoothing;
    }
    return 0.5 * t * t / smoothing;
}

fn safe_normalize(n: vec2f) -> vec2f {
    // A div-by-zero-safe replacement for normalize.
    let l = length(n);
    return select(n, n / l, abs(l) > 1e-10);
}

struct ErosionResult {
    delta: vec4f,     // xyz = height/slope delta, w = magnitude
    ridgeMap: f32,
    debug: f32,
};

fn ErosionFilter(
    // Input parameters that vary per pixel.
    p: vec2f, heightAndSlope_in: vec3f, fadeTarget_in: f32,
    // Stylistic parameters that may vary per pixel.
    strength_in: f32, gullyWeight: f32, detail: f32, rounding: vec4f, onset: vec4f, assumedSlope: vec2f,
    // Scale related parameters that do not support variation per pixel.
    scale: f32, octaves: i32, lacunarity: f32,
    // Other parameters.
    gain_in: f32, cellScale: f32, normalization: f32) -> ErosionResult {

    var strength = strength_in * scale;
    var fadeTarget = clamp(fadeTarget_in, -1.0, 1.0);
    var gain = gain_in;

    let inputHeightAndSlope = heightAndSlope_in;
    var heightAndSlope = heightAndSlope_in;
    var freq = 1.0 / (scale * cellScale);
    let slopeLength = max(length(heightAndSlope.yz), 1e-10);
    var magnitude = 0.0;
    var roundingMult = 1.0;

    let roundingForInput = mix(rounding.y, rounding.x, clamp01(fadeTarget + 0.5)) * rounding.z;
    // The combined accumulating mask: first from the initial slope, later also from each
    // octave's slope.
    var combiMask = ease_out(smooth_start(slopeLength * onset.x, roundingForInput * onset.x));

    // Initialize the ridgeMap fadeTarget and mask.
    var ridgeMapCombiMask = ease_out(slopeLength * onset.z);
    var ridgeMapFadeTarget = fadeTarget;

    // Determining the strength of the initial slope used for gully directions
    // based on the specified mix of the actual slope and an assumed slope.
    var gullySlope = mix(heightAndSlope.yz, heightAndSlope.yz / slopeLength * assumedSlope.x, assumedSlope.y);

    for (var i = 0; i < octaves; i = i + 1) {
        // Calculate and add gullies to the height and slope.
        var phacelle = PhacelleNoise(p * freq, safe_normalize(gullySlope), cellScale, 0.25, normalization);
        // Multiply with freq since p was multiplied with freq.
        // Negate since we use slope directions that point down.
        phacelle = vec4f(phacelle.x, phacelle.y, phacelle.z * -freq, phacelle.w * -freq);
        // Amount of slope as value from 0 to 1.
        let sloping = abs(phacelle.y);

        // Add non-masked, normalized slope to gullySlope, for use by subsequent octaves.
        gullySlope += sign(phacelle.y) * phacelle.zw * strength * gullyWeight;

        // Gullies has height offset (from -1 to 1) in x and derivative in yz.
        let gullies = vec3f(phacelle.x, phacelle.y * phacelle.zw);
        // Fade gullies towards fadeTarget based on combiMask.
        let fadedGullies = mix(vec3f(fadeTarget, 0.0, 0.0), gullies * gullyWeight, combiMask);
        // Apply height offset and derivative (slope) according to strength of current octave.
        heightAndSlope += fadedGullies * strength;
        magnitude += strength;

        // Update fadeTarget to include the new octave.
        fadeTarget = fadedGullies.x;

        // Update the mask to include the new octave.
        let roundingForOctave = mix(rounding.y, rounding.x, clamp01(phacelle.x + 0.5)) * roundingMult;
        let newMask = ease_out(smooth_start(sloping * onset.y, roundingForOctave * onset.y));
        combiMask = pow_inv(combiMask, detail) * newMask;

        // Update the ridgeMap fadeTarget and mask.
        ridgeMapFadeTarget = mix(ridgeMapFadeTarget, gullies.x, ridgeMapCombiMask);
        let newRidgeMapMask = ease_out(sloping * onset.w);
        ridgeMapCombiMask = ridgeMapCombiMask * newRidgeMapMask;

        // Prepare the next octave.
        strength *= gain;
        freq *= lacunarity;
        roundingMult *= rounding.w;
    }

    var result: ErosionResult;
    result.ridgeMap = ridgeMapFadeTarget * (1.0 - ridgeMapCombiMask);
    result.debug = fadeTarget;
    result.delta = vec4f(heightAndSlope - inputHeightAndSlope, magnitude);
    return result;
}

// -----------------------------------------------------------------------------
// DEMONSTRATION
// -----------------------------------------------------------------------------

// Used for tree coverage on the height map.
fn GetTreesAmount(height: f32, normalY: f32, occlusion: f32, ridgeMap: f32) -> f32 {
    return ((
        smoothstep_(GRASS_HEIGHT + 0.05, GRASS_HEIGHT + 0.01,
            height + 0.01 + (occlusion - 0.8) * 0.05)
        * smoothstep_(0.0, 0.4, occlusion)
        * smoothstep_(0.95, 1.0, normalY)
        * smoothstep_(-1.4, 0.0, ridgeMap)
        * smoothstep_(WATER_HEIGHT + 0.000, WATER_HEIGHT + 0.007, height)
    ) - 0.5) / 0.6;
}

fn Heightmap(p: vec2f) -> vec4f {
    // ------------------------------------------------------------------------
    // Erosion parameters.
    // ------------------------------------------------------------------------

    // The scale of the erosion effect, affecting it both horizontally and vertically.
    let EROSION_SCALE = 0.15;
    // The strength of the erosion effect, affecting the magnitude of all octaves,
    // and indirectly the directions of the gullies as a result.
    let EROSION_STRENGTH = 0.22;
    // The magnitude of the gullies as a weight value from 0 to 1.
    let EROSION_GULLY_WEIGHT = 0.5;
    // The overall detail of the erosion.
    let EROSION_DETAIL = 1.5;

    // x: rounding of ridges, y: rounding of creases,
    // z: multiplier applied to the initial height function,
    // w: multiplier applied to each subsequent gully octave.
    let EROSION_ROUNDING = vec4f(0.1, 0.0, 0.1, 2.0);
    // x: onset on the initial height function, y: onset on each gully octave,
    // z: ridge-map onset on the initial height function, w: ridge-map onset per octave.
    let EROSION_ONSET = vec4f(0.7, 1.25, 2.8, 1.5);
    // x: assumed slope value, y: amount (0..1) to override the actual slope.
    let EROSION_ASSUMED_SLOPE = vec2f(0.7, 1.0);

    let EROSION_CELL_SCALE = 0.7;
    let EROSION_NORMALIZATION = 0.5;

    let EROSION_OCTAVES: i32 = 5;
    let EROSION_LACUNARITY = 2.0;
    let EROSION_GAIN = 0.5;

    // ------------------------------------------------------------------------
    // Logic for whether erosion is enabled or not (Enter toggles).
    // ------------------------------------------------------------------------
    var erosion = true;
    if (mq_pass.keys.z > 0.5) {
        erosion = false;
    }

    // ------------------------------------------------------------------------
    // Heightmap implementation.
    // ------------------------------------------------------------------------

    // Get height and slope from painted values in Buffer A.
    let n = textureSH(ch0, p).xyz;

    // Define the erosion fade target based on the altitude of the pre-eroded terrain.
    // The fade target should strive to be -1 at valleys and 1 at peaks.
    let fadeTarget = clamp((n.x - DEFAULT_HEIGHT) / 0.15, -1.0, 1.0);

    // Store erosion in x (height delta), yz (slope delta), w (magnitude).
    // The output ridge map is -1 on creases and 1 on ridges.
    let er = ErosionFilter(
        p, n, fadeTarget,
        EROSION_STRENGTH, EROSION_GULLY_WEIGHT, EROSION_DETAIL,
        EROSION_ROUNDING, EROSION_ONSET, EROSION_ASSUMED_SLOPE,
        EROSION_SCALE, EROSION_OCTAVES, EROSION_LACUNARITY,
        EROSION_GAIN, EROSION_CELL_SCALE, EROSION_NORMALIZATION);

    var h = er.delta;
    var ridgeMap = er.ridgeMap;
    let debug = er.debug;

    if (!erosion) {
        h = vec4f(0.0);
        ridgeMap = 1.0;
    }

    // Offset according to the height offset parameter by multiplying it with the magnitude.
    // TERRAIN_HEIGHT_OFFSET = vec2(0.0, 0.0) in the original.
    let offset = 0.0;
    var eroded = n.x + h.x + offset;

    // Add trees to terrain.
    var trees = -1.0;
    let deriv = n.yz + h.yz;
    let normalY = 1.0 / sqrt(1.0 + dot(deriv, deriv));
    let treesAmount = GetTreesAmount(eroded, normalY, h.x / h.w + 0.5, ridgeMap);
    trees = (1.0 - pow(noised((p + 0.5) * 200.0).x * 0.5 + 0.5, 2.0) - 1.0 + 1.0 * treesAmount) * 1.5;
    if (trees > 0.0) {
        eroded += trees / 300.0;
    }

    // Pack four floats into a single channel to be able to get more data out of this buffer.
    let packed = pack4(vec4f(
        clamp01(h.x / h.w * 0.5 + 0.5), // Erosion delta as [0, 1] value.
        clamp01(ridgeMap * 0.5 + 0.5),  // Ridge map as [0, 1] value.
        clamp01(trees * 0.5 + 0.5),     // Tree value as [0, 1] value.
        clamp01(debug * 0.5 + 0.5)));   // Debug value.

    return vec4f(eroded, 0.0, 0.0, packed);
}

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let fragCoord = fragCoordSH(frag_coord);
    let uv = fragCoord / mq_pass.resolution;
    // Get the map data.
    return Heightmap(uv);
}
