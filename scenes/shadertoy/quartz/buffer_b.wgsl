// Buffer B — the renderer: the scene in linear HDR. ch0 = Buffer A (the camera offset).
// Description and credits: top of the Image tab (quartz/README.md here).
//
// WGSL port notes beyond the general ones in common.wgsl:
//   · GLSL row-vector products (`v * M`) → `transpose(M) * v`; the source spells them out.
//   · The desktop-GL branch of the original's `#ifdef GL_ES` block is used: plain arrays with
//     dynamic indices and `ZERO == 0` (WebGPU compiles the rolled loops fine; the ES branch
//     existed to keep shadertoy.com's ANGLE compile time down, ~19 s vs ~47 s).
//   · `smoothstep(w, -w, x)` (reversed edges, undefined in WGSL) → `ss(w, -w, x)`.

const IOR: f32 = 1.5443; // quartz, ordinary ray at 589 nm
const FOCAL: f32 = 1.9;
const SHAFT_R: f32 = 0.72; // radius of the light shaft (Gaussian)
const SUN_CORE: f32 = 0.006; // the sun's own angular radius, rad (a few pixels: it carries the light)
const SUN_HALO: f32 = 20.0; // radiance of the halo round it, at its centre
const SUN_HALO_C: f32 = 0.004; // the halo's value / distance scale, rad ...
const SUN_REACH: f32 = 0.08; // ... reaching zero at this angle
const SUNE: f32 = 14.0; // irradiance of the sun inside the shaft
const OPEN: f32 = 0.17; // half angle of the cave opening seen from the crystals
const SKY_L: f32 = 2.6; // radiance of the daylight in the opening
const SUNCOL: vec3f = vec3f(1.0, 0.80, 0.55);
const SKYCOL: vec3f = vec3f(0.85, 1.0, 0.82);
const AMBC: vec3f = vec3f(0.10, 0.105, 0.12); // irradiance bounced round the cave
const ABSORB: vec3f = vec3f(0.05, 0.07, 0.11); // clear quartz, very slightly smoky
const HAZE: f32 = 0.35; // milky inclusions per unit length
const EPS: f32 = 1e-3;

// sunlight direction, shaft cross-section basis, a point on its axis (set in the entry point)
var<private> L: vec3f;
var<private> sU: vec3f;
var<private> sV: vec3f;
var<private> sA: vec3f;
var<private> camR: vec3f;
var<private> camU: vec3f;
// share of the pixel that is rock (floor, boulders, wall): Buffer C weights its stars
var<private> gRock: f32 = 0.0;

// ---------- hashes and noise (integer lattice: the Intel driver folds float hashes) ----------

fn hashI(i: vec3i) -> f32 {
    let u = vec3u(i) * vec3u(1597334673u, 3812015801u, 2798796415u);
    var n = (u.x ^ u.y ^ u.z) * 1597334673u;
    n ^= n >> 16u;
    n *= 2246822519u;
    n ^= n >> 13u;
    return f32(n) * (1.0 / 4294967296.0);
}

fn vnoise2(p: vec2f) -> f32 {
    let i = vec2i(floor(p));
    let f = fract(p);
    let u = f * f * (vec2f(3.0) - 2.0 * f);
    return mix(
        mix(hashI(vec3i(i, 0)), hashI(vec3i(i + vec2i(1, 0), 0)), u.x),
        mix(hashI(vec3i(i + vec2i(0, 1), 0)), hashI(vec3i(i + vec2i(1, 1), 0)), u.x),
        u.y);
}

fn vnoise3(p: vec3f) -> f32 {
    let i = vec3i(floor(p));
    let f = fract(p);
    let u = f * f * (vec3f(3.0) - 2.0 * f);
    return mix(
        mix(
            mix(hashI(i), hashI(i + vec3i(1, 0, 0)), u.x),
            mix(hashI(i + vec3i(0, 1, 0)), hashI(i + vec3i(1, 1, 0)), u.x),
            u.y),
        mix(
            mix(hashI(i + vec3i(0, 0, 1)), hashI(i + vec3i(1, 0, 1)), u.x),
            mix(hashI(i + vec3i(0, 1, 1)), hashI(i + vec3i(1, 1, 1)), u.x),
            u.y),
        u.z);
}

// value noise with its analytic gradient (iq): x = value, yzw = gradient
fn noised(x: vec3f) -> vec4f {
    let i = vec3i(floor(x));
    let f = fract(x);
    let u = f * f * (vec3f(3.0) - 2.0 * f);
    let du = 6.0 * f * (vec3f(1.0) - f);
    let a = hashI(i);
    let b = hashI(i + vec3i(1, 0, 0));
    let c = hashI(i + vec3i(0, 1, 0));
    let d = hashI(i + vec3i(1, 1, 0));
    let e = hashI(i + vec3i(0, 0, 1));
    let g = hashI(i + vec3i(1, 0, 1));
    let h = hashI(i + vec3i(0, 1, 1));
    let k = hashI(i + vec3i(1, 1, 1));
    let k1 = b - a;
    let k2 = c - a;
    let k3 = e - a;
    let k4 = a - b - c + d;
    let k5 = a - c - e + h;
    let k6 = a - b - e + g;
    let k7 = -a + b + c - d + e - g - h + k;
    return vec4f(
        a + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x
            + k7 * u.x * u.y * u.z,
        du * vec3f(
            k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
            k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
            k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y));
}

fn fbmD(p: vec3f) -> vec4f {
    var s = vec4f(0.0);
    var a = 0.5;
    var f = 1.0;
    for (var i = 0; i < 4; i++) {
        let n = noised(p * f + f32(i) * 7.31);
        s += a * vec4f(n.x, n.yzw * f);
        a *= 0.5;
        f *= 2.07;
    }
    return s;
}

// ---------- optics ----------

// exact unpolarised Fresnel; ci = cosine on the air side, n = quartz / air
fn fresDiel(ci: f32, n: f32) -> f32 {
    let ct2 = 1.0 - (1.0 - ci * ci) / (n * n);
    if (ct2 <= 0.0) {
        return 1.0;
    }
    let ct = sqrt(ct2);
    let rs = (ci - n * ct) / (ci + n * ct);
    let rp = (n * ci - ct) / (n * ci + ct);
    return 0.5 * (rs * rs + rp * rp);
}

fn phaseHG(c: f32, g: f32) -> f32 {
    let d = 1.0 + g * g - 2.0 * g * c;
    return (1.0 - g * g) / (2.0 * tau * d * sqrt(d));
}

// Cauchy index, lam in micrometres; B fitted to quartz's n_F - n_C, scaled by DISPERSION
fn iorAt(lam: f32) -> f32 {
    let B = 0.004083 * DISPERSION;
    return IOR - B / (0.5893 * 0.5893) + B / (lam * lam);
}

// Alan Zucconi's six-bump spectrum fit, x in 0..1 over 400..700 nm
fn spectrum(x: f32) -> vec3f {
    let c1 = vec3f(3.54585104, 2.93225262, 2.41593945);
    let x1 = vec3f(0.69549072, 0.49228336, 0.27699880);
    let y1 = vec3f(0.02312639, 0.15225084, 0.52607955);
    let c2 = vec3f(3.90307140, 3.21182957, 3.96587128);
    let x2 = vec3f(0.11748627, 0.86755042, 0.66077860);
    let y2 = vec3f(0.84897130, 0.88445281, 0.73949448);
    let a = c1 * (x - x1);
    let b = c2 * (x - x2);
    return clamp(clamp(vec3f(1.0) - a * a - y1, vec3f(0.0), vec3f(1.0))
        + clamp(vec3f(1.0) - b * b - y2, vec3f(0.0), vec3f(1.0)), vec3f(0.0), vec3f(1.0));
}

// the sun seen along a direction: a core of its own size carrying its light, and round it a halo
// that falls as value / distance, reaching zero at SUN_REACH
fn sunLight(cosT: f32) -> f32 {
    let th = acos(clamp(cosT, -1.0, 1.0));
    if (th >= SUN_REACH) {
        return 0.0;
    }
    let core = (1.0 - ss(0.7 * SUN_CORE, 1.3 * SUN_CORE, th)) * SUNE / (pi * SUN_CORE * SUN_CORE);
    let k = SUN_HALO_C / (SUN_REACH + SUN_HALO_C);
    let halo = SUN_HALO * max(SUN_HALO_C / (th + SUN_HALO_C) - k, 0.0) / (1.0 - k);
    return core + halo;
}

fn erf_(x: f32) -> f32 {
    let a = x * x;
    return sign(x) * sqrt(1.0 - exp(-a * (4.0 / pi + 0.147 * a) / (1.0 + 0.147 * a)));
}

// ---------- the light shaft ----------

fn streak(s: vec2f) -> f32 {
    return 0.3 + 0.7 * ss(0.25, 0.75, vnoise2(s * vec2f(2.7, 2.7) + vec2f(T() * 0.12, 0.0)));
}

fn shaftMask(p: vec3f) -> f32 {
    let w = p - sA;
    let s = vec2f(dot(w, sU), dot(w, sV));
    return exp(-dot(s, s) / (SHAFT_R * SHAFT_R)) * streak(s);
}

// the shaft's density integrated from the camera to t1, closed form
fn shaftVol(ro: vec3f, rd: vec3f, t1: f32) -> f32 {
    let w = ro - sA;
    let s0 = vec2f(dot(w, sU), dot(w, sV));
    let k = vec2f(dot(rd, sU), dot(rd, sV));
    let a = dot(k, k);
    let b = dot(s0, k);
    let c = dot(s0, s0);
    var I = 0.0;
    var tc = 0.0;
    if (a < 1e-5) {
        I = exp(-c / (SHAFT_R * SHAFT_R)) * t1;
        tc = 0.5 * t1;
    } else {
        let tm = -b / a;
        let cm = max(c - b * b / a, 0.0);
        let sa = sqrt(a) / SHAFT_R;
        // .8862269 = sqrt( pi )/2 as a literal: the computed one differs by one ulp
        I = exp(-cm / (SHAFT_R * SHAFT_R)) * 0.8862269 / sa
            * (erf_((t1 - tm) * sa) - erf_(-tm * sa));
        tc = clamp(tm, 0.0, t1);
    }
    return I * streak(s0 + k * tc);
}

// branches and leaf clumps over the sky, swaying a little; the sun sits in a gap they sway across
fn leavesAt(s: vec2f) -> f32 {
    let t = T();
    let q = s + 0.05 * vec2f(sin(t * 1.3), cos(t * 1.1));
    let br = abs(vnoise2(q * 2.6 + 1.7) - 0.5);
    let tw = abs(vnoise2(q * 7.5 + 5.3) - 0.5);
    let lines = max(1.0 - ss(0.012, 0.035, br), 0.8 * (1.0 - ss(0.008, 0.025, tw)));
    let clump = ss(0.42, 0.58, 0.65 * vnoise2(q * 5.0 + 2.0) + 0.35 * vnoise2(q * 16.0))
        * (1.0 - ss(0.05, 0.22, br));
    return max(lines, clump) * ss(0.05, 0.3, length(q));
}

// the opening seen along d, in its own units: a short tunnel whose wall the daylight lights more
// and more towards its far end, then the sky with branches.
// x = how open the cave wall is, y = share of sky, z = radiance
fn holeLook(d: vec3f) -> vec3f {
    let c = dot(d, L);
    if (c < cos(OPEN * 1.25)) {
        return vec3f(0.0);
    }
    let s = vec2f(dot(d, sU), dot(d, sV)) / OPEN;
    let r = length(s);
    let e = r + 0.22 * (vnoise2(s / max(r, 1e-3) * 2.2 + 3.0) - 0.5)
        + 0.08 * (vnoise2(s * 7.0) - 0.5);
    let open = 1.0 - ss(0.9, 0.97, e);
    let sky = 1.0 - ss(0.5, 0.56, e + 0.08 * (vnoise2(s * 4.0) - 0.5));
    // the tunnel wall: rock lit from its far end, dark cracks, a shadowed lip at the rim
    let rock = 0.6 * vnoise2(s * 9.0) + 0.4 * vnoise2(s * 23.0);
    let wall = pow(max(1.0 - ss(0.5, 0.97, e), 0.0), 1.6) * ss(0.25, 0.75, rock) * 0.75
        * (1.0 - 0.8 * (1.0 - ss(0.0, 0.05, abs(vnoise2(s * 5.0 + 9.0) - 0.5))));
    let lv = leavesAt(s);
    let sun = sunLight(c);
    let l = sky * ((1.0 - 0.93 * lv) * SKY_L * (1.2 + 1.3 * max(1.0 - r / 0.55, 0.0))
        + sun * (1.0 - lv)) + (1.0 - sky) * wall * SKY_L;
    return vec3f(open, sky, open * l);
}

// daylight in the opening and the sun disc, seen along d (the crystals sit near sA)
fn windowLum(d: vec3f) -> f32 {
    return holeLook(d).z;
}

// ---------- crystals ----------

struct Cry {
    b: vec3f,
    ax: vec3f,
    tw: f32,
    r: f32,
    h: f32,
    hc: f32,
    bot: f32,
};

const NCRY: i32 = 6;

fn cry(i: i32) -> Cry {
    if (i == 0) {
        return Cry(vec3f(0.25, -0.25, 0.30), normalize(vec3f(0.20, 1.0, 0.06)), 0.30, 0.36, 1.75, 0.52, -0.30);
    }
    if (i == 1) {
        return Cry(vec3f(-0.38, -0.15, 0.62), normalize(vec3f(-0.10, 1.0, -0.05)), 0.10, 0.17, 1.05, 0.30, -0.30);
    }
    if (i == 2) {
        return Cry(vec3f(-0.30, 0.18, 0.05), normalize(vec3f(-1.0, 0.42, -0.25)), 0.60, 0.15, 0.80, 0.26, -0.40);
    }
    if (i == 3) {
        return Cry(vec3f(-0.28, 0.10, 0.30), normalize(vec3f(-0.65, 0.80, -0.10)), 0.45, 0.13, 0.55, 0.22, -0.40);
    }
    if (i == 4) {
        return Cry(vec3f(0.20, -0.05, -0.30), normalize(vec3f(0.60, 0.75, -0.35)), 0.20, 0.12, 0.38, 0.20, -0.30);
    }
    return Cry(vec3f(-0.08, 0.25, 0.15), normalize(vec3f(-0.20, 1.0, -0.30)), 0.80, 0.09, 0.32, 0.15, -0.40);
}

fn cryFrame(c: Cry) -> mat3x3f {
    let y = c.ax;
    let x = normalize(cross(y, vec3f(0.0, 0.0, 1.0)));
    let z = cross(x, y);
    let cs = vec2f(cos(c.tw), sin(c.tw));
    let x2 = x * cs.x + z * cs.y;
    return mat3x3f(x2, y, cross(x2, y)); // columns: local -> world
}

fn cryBound(c: Cry) -> vec4f {
    let hl = 0.5 * (c.h + c.hc - c.bot);
    return vec4f(c.b + c.ax * (c.bot + hl), sqrt(1.3333 * c.r * c.r + hl * hl));
}

fn clipPl(ro: vec3f, rd: vec3f, n: vec3f, d: f32,
          tN: ptr<function, f32>, tF: ptr<function, f32>,
          nN: ptr<function, vec3f>, nF: ptr<function, vec3f>) {
    var dn = dot(rd, n);
    dn = select(max(dn, 1e-8), min(dn, -1e-8), dn < 0.0);
    let t = (d - dot(ro, n)) / dn;
    if (dn < 0.0) {
        if (t > *tN) {
            *tN = t;
            *nN = n;
        }
    } else {
        if (t < *tF) {
            *tF = t;
            *nF = n;
        }
    }
}

// ray in the crystal's local space; tt = entry, exit; nN / nF their local normals
fn cryHit(c: Cry, ro: vec3f, rd: vec3f, tt: ptr<function, vec2f>,
          nN: ptr<function, vec3f>, nF: ptr<function, vec3f>) -> bool {
    var tN = -1e9;
    var tF = 1e9;
    *nN = vec3f(0.0, -1.0, 0.0);
    *nF = vec3f(0.0, -1.0, 0.0);
    var cs = vec2f(1.0, 0.0);
    let R60 = mat2x2f(vec2f(0.5, 0.8660254), vec2f(-0.8660254, 0.5));
    for (var i = 0; i < 6; i++) {
        clipPl(ro, rd, vec3f(cs.x, 0.0, cs.y), c.r, &tN, &tF, nN, nF);
        // rhombohedral cap: three large faces meet at the tip, three small ones sit lower
        let hk = c.h + c.hc * select(1.16, 1.0, (i & 1) == 0);
        let m = normalize(vec3f(cs.x * c.hc, c.r, cs.y * c.hc));
        clipPl(ro, rd, m, m.y * hk, &tN, &tF, nN, nF);
        cs = R60 * cs;
    }
    clipPl(ro, rd, vec3f(0.0, -1.0, 0.0), -c.bot, &tN, &tF, nN, nF);
    *tt = vec2f(tN, tF);
    return tN < tF && tF > 0.0;
}

// distance from a surface point to the nearest edge: the second smallest gap to the planes
fn cryEdge(c: Cry, p: vec3f) -> f32 {
    var d1 = 1e9;
    var d2 = 1e9;
    var cs = vec2f(1.0, 0.0);
    let R60 = mat2x2f(vec2f(0.5, 0.8660254), vec2f(-0.8660254, 0.5));
    for (var i = 0; i < 6; i++) {
        let hk = c.h + c.hc * select(1.16, 1.0, (i & 1) == 0);
        let m = normalize(vec3f(cs.x * c.hc, c.r, cs.y * c.hc));
        let g = vec2f(c.r - dot(p, vec3f(cs.x, 0.0, cs.y)), m.y * hk - dot(p, m));
        let a = min(g.x, g.y);
        let b = max(g.x, g.y);
        d2 = min(d2, max(d1, a));
        d1 = min(d1, a);
        d2 = min(d2, b);
        cs = R60 * cs;
    }
    return d2;
}

// the exit of a ray that starts inside the crystal: the nearest plane it leaves through, exactly as
// cryHit's far side (same planes, order and 1e-8 floor), without the entry side
fn cryExit(c: Cry, ro: vec3f, rd: vec3f, nF: ptr<function, vec3f>) -> f32 {
    var tF = 1e9;
    *nF = vec3f(0.0, -1.0, 0.0);
    var cs = vec2f(1.0, 0.0);
    let R60 = mat2x2f(vec2f(0.5, 0.8660254), vec2f(-0.8660254, 0.5));
    for (var i = 0; i < 6; i++) {
        let n = vec3f(cs.x, 0.0, cs.y);
        var dn = dot(rd, n);
        if (dn >= 0.0) {
            let t = (c.r - dot(ro, n)) / max(dn, 1e-8);
            if (t < tF) {
                tF = t;
                *nF = n;
            }
        }
        let hk = c.h + c.hc * select(1.16, 1.0, (i & 1) == 0);
        let m = normalize(vec3f(cs.x * c.hc, c.r, cs.y * c.hc));
        dn = dot(rd, m);
        if (dn >= 0.0) {
            let t = (m.y * hk - dot(ro, m)) / max(dn, 1e-8);
            if (t < tF) {
                tF = t;
                *nF = m;
            }
        }
        cs = R60 * cs;
    }
    let dn = -rd.y;
    if (dn >= 0.0) {
        let t = (-c.bot + ro.y) / max(dn, 1e-8);
        if (t < tF) {
            tF = t;
            *nF = vec3f(0.0, -1.0, 0.0);
        }
    }
    return tF;
}

fn sphereBound(ro: vec3f, rd: vec3f, s: vec4f, tmax: f32) -> bool {
    let oc = ro - s.xyz;
    let b = dot(oc, rd);
    let c = dot(oc, oc) - s.w * s.w;
    let h = b * b - c;
    return h > 0.0 && -b - sqrt(h) < tmax && -b + sqrt(h) > 0.0;
}

// ---------- boulders: planes clipped by a sphere, squashed ----------

const NBOULD: i32 = 12;

fn bould(i: i32) -> vec4f {
    if (i == 0) { return vec4f(-2.35, -0.45, -1.25, 1.35); }
    if (i == 1) { return vec4f(-2.10, 0.10, 2.40, 1.70); }
    if (i == 2) { return vec4f(2.40, -0.10, 2.10, 1.75); }
    if (i == 3) { return vec4f(2.35, -0.55, -0.40, 1.00); }
    if (i == 4) { return vec4f(0.40, -0.40, 3.80, 1.90); }
    if (i == 5) { return vec4f(-0.70, -0.50, 1.40, 0.75); }
    // low slabs that break the floor
    if (i == 6) { return vec4f(1.30, -0.33, -1.00, 0.70); }
    if (i == 7) { return vec4f(-1.05, -0.36, -1.55, 0.80); }
    if (i == 8) { return vec4f(0.95, -0.30, 1.15, 0.65); }
    if (i == 9) { return vec4f(-1.35, -0.30, 0.25, 0.70); }
    if (i == 10) { return vec4f(0.15, -0.52, -1.95, 0.90); }
    return vec4f(2.00, -0.30, 0.85, 0.70);
}

const BSQ: vec3f = vec3f(1.0, 0.62, 1.0);

fn boulderHit(i: i32, ro: vec3f, rd: vec3f, t: ptr<function, f32>, nw: ptr<function, vec3f>) -> bool {
    *nw = vec3f(0.0, 1.0, 0.0);
    let b = bould(i);
    var o = (ro - b.xyz) / BSQ;
    var d = rd / BSQ; // t is unchanged by the scaling
    let ang = hashI(vec3i(i, 5, 1));
    o = vec3f(rotVR(ang, o.xz).x, o.y, rotVR(ang, o.xz).y);
    d = vec3f(rotVR(ang, d.xz).x, d.y, rotVR(ang, d.xz).y);
    let R = b.w * 1.12;
    let bb = dot(o, d);
    let aa = dot(d, d);
    let cc = dot(o, o) - R * R;
    var h = bb * bb - aa * cc;
    if (h < 0.0) {
        return false;
    }
    h = sqrt(h);
    var tN = (-bb - h) / aa;
    var tF = (-bb + h) / aa;
    if (tF < 0.0 || tN > *t) {
        return false;
    }
    var nN = vec3f(0.0);
    var nF = vec3f(0.0);
    for (var k = 0; k < 10; k++) {
        let z = 1.0 - (2.0 * f32(k) + 1.0) / 10.0;
        let ph = f32(k) * 2.3999632 + f32(i);
        var n = vec3f(sqrt(1.0 - z * z) * vec2f(cos(ph), sin(ph)), z).xzy;
        n = normalize(n + 0.5 * (vec3f(hashI(vec3i(i, k, 2)), hashI(vec3i(i, k, 3)),
                                         hashI(vec3i(i, k, 4))) - vec3f(0.5)));
        clipPl(o, d, n, b.w * (0.74 + 0.2 * hashI(vec3i(i, k, 6))), &tN, &tF, &nN, &nF);
    }
    if (tN > tF || tN < 0.0 || tN > *t) {
        return false;
    }
    *t = tN;
    var n = select((o + d * tN) / R, nN, dot(nN, nN) > 0.0);
    let nr = rotVR(-ang, n.xz);
    *nw = normalize(vec3f(nr.x, n.y, nr.y) / BSQ);
    return true;
}

// ---------- the cave ----------

const CAVE_C: vec3f = vec3f(0.5, 0.5, 2.5);
const CAVE_R: vec3f = vec3f(8.0, 4.2, 8.0);

fn caveHit(ro: vec3f, rd: vec3f, n: ptr<function, vec3f>) -> f32 {
    let o = (ro - CAVE_C) / CAVE_R;
    let d = rd / CAVE_R;
    let a = dot(d, d);
    let b = dot(o, d);
    let c = dot(o, o) - 1.0;
    let t = (-b + sqrt(max(b * b - a * c, 0.0))) / a;
    *n = -normalize((o + d * t) / CAVE_R);
    return t;
}

// ---------- rock ----------

fn aoCrystals(p: vec3f) -> f32 {
    var ao = 1.0;
    for (var i = 0; i < NCRY; i++) {
        let c = cry(i);
        let q = c.b + c.ax * clamp(dot(p - c.b, c.ax), c.bot, c.h);
        let d = length(p - q);
        ao *= 1.0 - 0.55 * exp(-d * d / (4.0 * c.r * c.r));
    }
    return ao;
}

// light that left the big crystal through a 60 degree pair of prism faces and lands at p
fn caustic(p: vec3f, n: vec3f) -> vec3f {
    var acc = vec3f(0.0);
    if (CAUSTICS) {
        let c = cry(0);
        let M = cryFrame(c);
        let pl = transpose(M) * (p - c.b);
        let Ll = transpose(M) * L;
        for (var i = 0; i < 6; i++) {
            let ai = f32(i) * tau / 6.0;
            let nA = vec3f(cos(ai), 0.0, sin(ai));
            if (dot(nA, Ll) < 0.05) {
                continue;
            }
            for (var s = -1; s <= 1; s += 2) {
                let aj = ai + f32(s) * tau / 3.0;
                let nB = vec3f(cos(aj), 0.0, sin(aj));
                for (var l = 0; l < NLAMBDA; l++) {
                    let x = (f32(l) + 0.5) / f32(NLAMBDA);
                    let n_ = iorAt(0.4 + 0.3 * x);
                    let di = refract(-Ll, nA, 1.0 / n_);
                    let e = refract(di, -nB, n_);
                    if (dot(e, e) < 0.5) {
                        continue; // TIR at the exit face
                    }
                    let dn = dot(nB, e);
                    if (dn < 1e-3) {
                        continue;
                    }
                    let tb = (dot(nB, pl) - c.r) / dn; // back along -e to face B
                    if (tb < 0.0) {
                        continue;
                    }
                    let qb = pl - e * tb;
                    let sa = dot(nA, -di);
                    if (sa < 1e-3) {
                        continue;
                    }
                    let qa = qb - di * ((c.r - dot(nA, qb)) / sa); // back along -di to face A
                    // inside both faces: lateral half width r/sqrt(3), heights from the rock to the cap
                    let tA = vec3f(-nA.z, 0.0, nA.x);
                    let tB = vec3f(-nB.z, 0.0, nB.x);
                    let w = 0.004 + 0.012 * tb;
                    let m = ss(w, -w, abs(dot(qb, tB)) - c.r * 0.57735)
                        * ss(w, -w, abs(dot(qa, tA)) - c.r * 0.57735)
                        * ss(-w, w, (c.b + M * qb).y - 0.03)
                        * ss(w, -w, qb.y - c.h) * ss(w, -w, qa.y - c.h);
                    if (m <= 0.0) {
                        continue;
                    }
                    let cA = dot(nA, Ll);
                    let cB = dn;
                    let tr = (1.0 - fresDiel(cA, n_)) * (1.0 - fresDiel(cB, n_));
                    acc += spectrum(x) * m * tr * max(dot(n, -(M * e)), 0.0) * shaftMask(c.b + M * qa);
                }
            }
        }
        acc *= 2.2 / f32(NLAMBDA); // the spectrum's white over NLAMBDA bins is ~ NLAMBDA/2.2 per channel
    }
    return acc;
}

fn sunBlockedByCrystal(p: vec3f) -> bool {
    for (var i = 0; i < NCRY; i++) {
        let c = cry(i);
        if (!sphereBound(p, L, cryBound(c), 1e9)) {
            continue;
        }
        let M = cryFrame(c);
        var tt = vec2f(0.0);
        var a = vec3f(0.0);
        var b = vec3f(0.0);
        if (cryHit(c, transpose(M) * (p - c.b), transpose(M) * L, &tt, &a, &b) && tt.x > 0.0) {
            return true;
        }
    }
    return false;
}

fn rockShade(p: vec3f, n: vec3f, rd: vec3f, t: f32, floorLike: bool) -> vec3f {
    let fade = 1.0 - ss(6.0, 14.0, t); // far rock goes soft, like a shallow focus
    let f1 = fbmD(p * 5.3);
    let f2 = noised(p * 41.0);
    let g = (f1.yzw * 0.45 + f2.yzw * 0.03) * fade;
    let nb = normalize(n - (g - n * dot(g, n)));
    // basalt, grey crust, ochre deposits round the crystal bases
    let crust = ss(0.45, 0.75, f1.x + 0.3 * f2.x);
    var alb = mix(vec3f(0.030, 0.029, 0.028), vec3f(0.085, 0.078, 0.070), crust);
    let nearC = exp(-dot(p.xz - vec2f(0.05, 0.25), p.xz - vec2f(0.05, 0.25)) * 1.2);
    let ochre = ss(0.55, 0.9, vnoise3(p * 4.0)) * (0.25 + 0.75 * nearC)
        * select(0.3, 1.0, floorLike);
    alb = mix(alb, vec3f(0.15, 0.11, 0.07), ochre * 0.8);
    alb *= 0.7 + 0.6 * f2.x;
    let sh = shaftMask(p);
    var vis = 1.0;
    if (sh > 0.01 && sunBlockedByCrystal(p + n * 0.01)) {
        vis = 0.12;
    }
    var ao = select(1.0, aoCrystals(p), floorLike);
    ao *= 0.55 + 0.45 * clamp(0.5 + 1.4 * (f1.x - 0.5), 0.0, 1.0);
    let nl = max(dot(nb, L), 0.0);
    let v = -rd;
    let hv = normalize(L + v);
    // wet rock: GGX at roughness .3, Schlick with water's F0
    let a = 0.3 * 0.3;
    let nh = max(dot(nb, hv), 0.0);
    let nv = max(dot(nb, v), 0.2);
    let dd = nh * nh * (a * a - 1.0) + 1.0;
    let spec = a * a / (pi * dd * dd) * (0.02 + 0.98 * pow(max(1.0 - dot(v, hv), 0.0), 5.0))
        / (4.0 * nv * max(nl, 1e-3) + 1e-3) * nl;
    let E = SUNCOL * SUNE * sh * vis;
    let w = p - sA;
    let broad = exp(-(dot(w, w) - dot(w, L) * dot(w, L)) / (9.0 * SHAFT_R * SHAFT_R));
    let Ew = mix(SKYCOL, SUNCOL, vec3f(0.6)) * 2.0 * broad * (0.5 + 0.5 * vis);
    var col = alb / pi * ((E + Ew) * nl + AMBC * (0.45 + 0.25 * nb.y) * pi * ao)
        + (E + Ew) * spec * (0.6 + 0.8 * crust);
    // wet rock mirrors the lit air of the shaft (rough: the shaft is wide anyway)
    {
        let R = reflect(rd, nb);
        let wet = ss(0.35, 0.75, f1.x + 0.3 * vnoise3(p * 1.7)) * fade;
        let Fw = 0.02 + 0.98 * pow(max(1.0 - nv, 0.0), 5.0);
        col += wet * Fw * SUNCOL * SUNE * 0.07 * SHAFT_BRIGHT * phaseHG(dot(R, L), 0.5)
            * shaftVol(p + n * 0.01, R, 4.0) * 1.5;
    }
    // mineral glints: small mirror facets with random normals
    let gp = p * 28.0;
    let gi = vec3i(floor(gp));
    if (hashI(gi + vec3i(11)) > 0.7) {
        let gn = normalize(nb + 1.1 * (vec3f(hashI(gi), hashI(gi + vec3i(3)), hashI(gi + vec3i(7)))
            - vec3f(0.5)));
        let gl = pow(max(dot(reflect(-L, gn), v), 0.0), 60.0);
        let sz = 1.0 - ss(0.15, 0.35, length(fract(gp) - vec3f(0.5)));
        col += (E + Ew) * gl * sz * 1.5 * fade;
    }
    if (CAUSTICS) {
        if (sh > 0.01) {
            col += alb / pi * SUNCOL * SUNE * caustic(p, nb) * 9.0;
        }
    }
    return col;
}

// rough look of what a ray leaving a crystal sees, without the bright opening (that one is spectral)
fn envDim(p: vec3f, d: vec3f) -> vec3f {
    if (d.y < -0.01) {
        let q = p + d * (-p.y / d.y);
        let sh = shaftMask(q);
        let alb = mix(vec3f(0.045), vec3f(0.20, 0.13, 0.06),
            0.5 * exp(-dot(q.xz - vec2f(0.05, 0.25), q.xz - vec2f(0.05, 0.25)) * 1.5));
        return alb / pi * (SUNCOL * SUNE * sh * L.y + AMBC * pi * 0.6);
    }
    return vec3f(0.012, 0.012, 0.014) + AMBC * 0.08 * (0.5 + 0.5 * d.y);
}

// ---------- the crystal: reflection, the path inside, the dispersed light ----------

fn medium(c: Cry, ci: i32, M: mat3x3f, q: vec3f, d: vec3f, seg: f32,
          att: ptr<function, vec3f>, fr: f32, col: ptr<function, vec3f>) {
    let wq = c.b + M * q;
    let wd = M * d;
    let mid = wq + wd * seg * 0.5;
    let sh = shaftMask(mid);
    let feather = vnoise3(mid * 7.0 + f32(ci) * 3.7);
    let base = 1.0 - ss(0.0, 0.45, (q.y + d.y * seg * 0.5 - c.bot) / (c.h - c.bot));
    let dens = HAZE * (0.45 + 1.1 * feather * feather) + 2.0 * base;
    let sun = SUNCOL * SUNE * sh;
    var ins = (1.0 - exp(-dens * seg))
        * (sun * (phaseHG(dot(wd, L), 0.45) + 0.05) + AMBC * 0.35);
    ins *= mix(vec3f(1.0), vec3f(0.75, 0.5, 0.28), base);
    *col += *att * fr * ins;
    *att *= exp(-(ABSORB + vec3f(0.4, 0.7, 1.1) * base) * seg - 0.6 * dens * seg);
    if (CRACKS) {
        let pe = wq + wd * seg;
        let rr = abs(vnoise3(pe * 13.0 + f32(ci)) - 0.5) + 0.5 * abs(vnoise3(pe * 31.0) - 0.5);
        if (rr < 0.05) { // off the thin lines the crackle is exactly 0
            let line = (1.0 - ss(0.0, 0.05, rr)) * ss(0.35, 0.65, vnoise3(pe * 3.5 + 2.0 * f32(ci)));
            *col += *att * fr * line * (SUNCOL * SUNE * shaftMask(pe) * 0.06 + AMBC * 0.3);
            *att *= 1.0 - 0.3 * line;
        }
    }
    if (CRACKS) {
        let Ll = transpose(M) * L;
        let nk = select(3, 9, ci == 0);
        for (var k = 0; k < 9; k++) {
            if (k >= nk) {
                break;
            }
            let h3 = vec3f(hashI(vec3i(ci, k, 20)), hashI(vec3i(ci, k, 21)), hashI(vec3i(ci, k, 22)));
            let nc = normalize(h3 * 2.0 - vec3f(1.0) + vec3f(0.0, 0.3, 0.0));
            let cc = vec3f((h3.y - 0.5) * c.r, mix(c.bot + 0.3 * (c.h - c.bot), c.h, h3.z),
                (h3.x - 0.5) * c.r);
            let rc = c.r * (0.6 + 0.9 * hashI(vec3i(ci, k, 23)));
            let dn = dot(d, nc);
            if (abs(dn) < 1e-4) {
                continue;
            }
            let tc = dot(cc - q, nc) / dn;
            if (tc < 0.0 || tc > seg) {
                continue;
            }
            let r = q + d * tc - cc;
            let e1 = normalize(cross(nc, vec3f(0.3, 1.0, 0.1)));
            let e2 = cross(nc, e1);
            let uv = vec2f(dot(r, e1), dot(r, e2)) / rc;
            if (dot(uv, uv) > 1.2122) {
                continue; // outside 1.101: the ragged rim (+- .25) is exactly 0
            }
            let m = (1.0 - ss(0.7, 0.85, length(uv)
                + 0.5 * (vnoise2(uv * 2.6 + f32(k) * 9.0) - 0.5)))
                * (0.35 + 0.65 * ss(0.3, 0.7, vnoise2(vec2f(uv.x * 14.0, uv.y * 2.0) + 3.3 * f32(k))));
            if (m <= 0.0) {
                continue;
            }
            // an air film 0.2-1.3 um thick: two-beam interference at three wavelengths
            let th = 0.22 + 0.45 * vnoise2(uv * 1.6 + 5.0 * f32(k));
            let ct = abs(dn);
            let film = vec3f(0.5) - 0.5 * cos(2.0 * tau * th * ct / vec3f(0.61, 0.54, 0.465));
            let spec = pow(max(dot(reflect(-Ll, nc), -d), 0.0), 18.0);
            let shc = shaftMask(c.b + M * (q + d * tc));
            let add = SUNCOL * SUNE * shc
                * (film * film * spec * 2.5 + 0.14 * phaseHG(dot(wd, L), 0.3) + 0.004)
                + AMBC * 0.25;
            *col += *att * fr * m * add * 0.5;
            *att *= 1.0 - 0.35 * m;
        }
    }
}

fn shadeCrystal(ci: i32, ro: vec3f, rd: vec3f, t: f32, nl: vec3f, pix: f32) -> vec3f {
    let c = cry(ci);
    let M = cryFrame(c);
    let pw = ro + rd * t;
    let nw = M * nl;
    let rdl = transpose(M) * rd;
    let pl = transpose(M) * (pw - c.b);
    let cosi = max(-dot(rd, nw), 1e-4);
    let F = fresDiel(cosi, IOR);
    let R = reflect(rd, nw);
    let shp = shaftMask(pw);
    var col = F * (envDim(pw, R) + SUNCOL * (shp * sunLight(dot(R, L))));

    // the reference path, its facets and the attenuation up to each exit
    var seqN: array<vec3f, BOUNCES>;
    var seqA: array<vec3f, BOUNCES>;
    var d = refract(rdl, nl, 1.0 / IOR);
    var q = pl - nl * EPS;
    var att = vec3f(1.0);
    var fr = 1.0 - F;
    for (var j = 0; j < BOUNCES; j++) {
        var nF = vec3f(0.0);
        let seg = max(cryExit(c, q, d, &nF), 0.0);
        medium(c, ci, M, q, d, seg, &att, fr, &col);
        q += d * seg;
        seqN[j] = nF;
        seqA[j] = att;
        let e = refract(d, -nF, IOR);
        var Fx = 1.0;
        if (dot(e, e) > 0.5) {
            Fx = fresDiel(dot(e, nF), IOR);
            col += att * fr * (1.0 - Fx) * envDim(c.b + M * q, M * e);
        }
        fr *= Fx;
        d = reflect(d, nF);
        q -= nF * EPS;
    }
    col += att * fr * envDim(c.b + M * q, M * d);

    // replay per wavelength: own entry, own exits, own Fresnel and TIR, against the bright light
    var acc = vec3f(0.0);
    var wsum = vec3f(0.0);
    for (var l = 0; l < NLAMBDA; l++) {
        let x = (f32(l) + 0.5) / f32(NLAMBDA);
        let n_ = iorAt(0.4 + 0.3 * x);
        let W = spectrum(x);
        var dl = refract(rdl, nl, 1.0 / n_);
        var tp = 1.0 - fresDiel(cosi, n_);
        var s = vec3f(0.0);
        for (var j = 0; j < BOUNCES; j++) {
            let nf = seqN[j];
            let e = refract(dl, -nf, n_);
            var Fx = 1.0;
            if (dot(e, e) > 0.5) {
                Fx = fresDiel(dot(e, nf), n_);
                s += seqA[j] * tp * (1.0 - Fx) * windowLum(M * e);
            }
            tp *= Fx;
            dl = reflect(dl, nf);
        }
        acc += W * s;
        wsum += W;
    }
    col += SUNCOL * acc / wsum;

    // edges: slightly chipped, they catch the sun and the opening
    {
        let de = cryEdge(c, pl);
        let chip = ss(0.35, 0.8, vnoise3(pw * 22.0));
        let edge = (1.0 - ss(0.0, pix * (0.6 + 1.4 * chip), de)) * (0.25 + 0.75 * chip);
        col += edge * (SUNCOL * SUNE * shp * 0.0105 + AMBC * 0.042);
    }
    // dirt crust on the lowest part of each crystal
    let yrel = (pl.y - c.bot) / (c.h - c.bot);
    let dirt = 1.0 - ss(0.08, 0.38, yrel + 0.25 * (vnoise3(pw * 9.0) - 0.5));
    if (dirt > 0.0) {
        let alb = mix(vec3f(0.20, 0.14, 0.07), vec3f(0.07, 0.06, 0.05), vnoise3(pw * 25.0));
        let dc = alb / pi * (SUNCOL * SUNE * shp * max(dot(nw, L), 0.0) + AMBC * pi * 0.6);
        col = mix(col, dc, dirt * 0.85);
    }
    return col;
}

// ---------- the scene ----------

fn render(ro: vec3f, rd: vec3f) -> vec3f {
    var nrm = vec3f(0.0);
    var t = caveHit(ro, rd, &nrm);
    var kind = 0;
    if (rd.y < 0.0) {
        let tf = -ro.y / rd.y;
        if (tf < t) {
            t = tf;
            kind = 1;
            nrm = vec3f(0.0, 1.0, 0.0);
        }
    }
    for (var i = 0; i < NBOULD; i++) {
        var nb = vec3f(0.0);
        if (boulderHit(i, ro, rd, &t, &nb)) {
            kind = 2;
            nrm = nb;
        }
    }
    var ci = -1;
    var cn = vec3f(0.0);
    for (var i = 0; i < NCRY; i++) {
        let c = cry(i);
        if (!sphereBound(ro, rd, cryBound(c), t)) {
            continue;
        }
        let M = cryFrame(c);
        var tt = vec2f(0.0);
        var nN = vec3f(0.0);
        var nF = vec3f(0.0);
        if (cryHit(c, transpose(M) * (ro - c.b), transpose(M) * rd, &tt, &nN, &nF)
            && tt.x > 0.0 && tt.x < t) {
            t = tt.x;
            ci = i;
            cn = nN;
            kind = 3;
        }
    }

    let p = ro + rd * t;
    var col = vec3f(0.0);
    if (kind != 3) {
        gRock += 1.0;
    }
    if (kind == 3) {
        let res = mq_pass.resolution;
        col = shadeCrystal(ci, ro, rd, t, cn, t * 1.5 / (FOCAL * res.y));
    } else {
        // one call site for all rock (D3D inlines each one): the wall, the floor, the boulders
        col = rockShade(p, nrm, rd, t, kind == 1) * select(1.0, 0.6, kind == 0);
        if (kind == 0) {
            let dO = normalize(p - sA);
            let ho = holeLook(dO);
            let ang = acos(clamp(dot(dO, L), -1.0, 1.0));
            // daylight spilling onto the ceiling
            col += vec3f(0.16, 0.13, 0.10) * exp(-max(ang - OPEN, 0.0) * 14.0)
                * (0.3 + 0.7 * vnoise3(p * 6.0));
            col = mix(col, ho.z * mix(vec3f(0.95, 0.78, 0.58), SKYCOL, ho.y), ho.x);
        }
    }

    // the shaft in the air, in front of everything it crosses
    col += SUNCOL * SUNE * 0.07 * SHAFT_BRIGHT * phaseHG(dot(rd, L), 0.5) * shaftVol(ro, rd, t);

    if (DUST) {
        // dust motes as points in the air: the ray is cut to the shaft's cylinder (2 radii) and walks
        // through cells of DCELL there (DDA), one mote per cell at most, each floating on its own
        // sines in x, y and z -- twice as far up and down as sideways. Hidden behind what the ray hit.
        let DCELL = 0.3;
        let res = mq_pass.resolution;
        let tt = T();
        let w = ro - sA;
        let s0 = vec2f(dot(w, sU), dot(w, sV));
        let kk = vec2f(dot(rd, sU), dot(rd, sV));
        let qa = dot(kk, kk);
        let qb = dot(s0, kk);
        let qc = dot(s0, s0) - 4.0 * SHAFT_R * SHAFT_R;
        var qh = qb * qb - qa * qc;
        var t0 = 0.3;
        var t1 = t;
        if (qa > 1e-6) {
            if (qh > 0.0) {
                qh = sqrt(qh);
                t0 = max((-qb - qh) / qa, 0.3);
                t1 = min((-qb + qh) / qa, t);
            } else {
                t1 = 0.0;
            }
        }
        if (t1 > t0) {
            let pd = (ro + rd * t0) / DCELL;
            var id = vec3i(floor(pd));
            let sg = vec3f(select(1.0, -1.0, rd.x < 0.0), select(1.0, -1.0, rd.y < 0.0),
                select(1.0, -1.0, rd.z < 0.0));
            let ird = sg / max(abs(rd), vec3f(1e-6));
            var tMax = (floor(pd) + vec3f(0.5) + 0.5 * sg - pd) * ird;
            let tDel = abs(ird);
            let tEnd = (t1 - t0) / DCELL;
            var dust = vec3f(0.0);
            // a mote counts within 3 radii of the ray; its radius grows with the distance
            let rG = 3.0 * DUST_SIZE * max(0.0025, 0.8 * t / (FOCAL * res.y)) * 1.01 + 1e-4;
            let lxz = length(rd.xz);
            for (var i = 0; i < 48; i++) {
                var hm = hashI(id + vec3i(0, 0, 7919));
                if (hm > 0.25) {
                    // the mote's x and z first: a projection never lengthens a distance, so a ray that
                    // passes farther than rG in the xz plane misses it -- most cells end here
                    let sp = 1.0 + 0.6 * hashI(id + vec3i(13, 17, 19));
                    let Pxz = (vec2f(f32(id.x), f32(id.z)) + vec2f(0.22)
                        + 0.56 * vec2f(hashI(id + vec3i(50, 0, 0)), hashI(id + vec3i(0, 0, 70))))
                        * DCELL
                        + DUST_SWAY * 0.0625 * sin(tt * 6.0 * sp
                            * (vec2f(0.9) + 0.5 * vec2f(hashI(id + vec3i(72, 1, 0)),
                                                       hashI(id + vec3i(1, 0, 92))))
                            + tau * vec2f(hashI(id + vec3i(71, 0, 0)), hashI(id + vec3i(0, 0, 91))));
                    let dq = Pxz - ro.xz;
                    if (abs(dq.x * rd.z - dq.y * rd.x) >= rG * lxz) {
                        hm = 0.0;
                    }
                }
                if (hm > 0.25) {
                    let o = vec3f(0.22, 0.43, 0.22)
                        + vec3f(0.56, 0.14, 0.56) * vec3f(hashI(id + vec3i(50, 0, 0)),
                                                          hashI(id + vec3i(0, 60, 0)),
                                                          hashI(id + vec3i(0, 0, 70)));
                    let ph = tau * vec3f(hashI(id + vec3i(71, 0, 0)), hashI(id + vec3i(0, 81, 0)),
                                         hashI(id + vec3i(0, 0, 91)));
                    let fr2 = 0.9 + 0.5 * vec3f(hashI(id + vec3i(72, 1, 0)),
                                                hashI(id + vec3i(0, 82, 1)),
                                                hashI(id + vec3i(1, 0, 92)));
                    let sp = 1.0 + 0.6 * hashI(id + vec3i(13, 17, 19)); // each mote its own pace
                    let P = (vec3f(id) + o) * DCELL
                        + DUST_SWAY * 0.0625 * vec3f(1.0, 2.0, 1.0) * sin(tt * 6.0 * sp * fr2 + ph);
                    let v = P - ro;
                    let tp = dot(v, rd);
                    if (tp > 0.3 && tp < t) {
                        let dd = length(v - rd * tp);
                        let pix = tp / (FOCAL * res.y);
                        let rr0 = max(0.0025, 0.8 * pix);
                        let rr = DUST_SIZE * rr0; // smaller, at the brightness of the size-1 mote
                        if (dd < 3.0 * rr) {
                            let wp = P - sA;
                            let sp2 = vec2f(dot(wp, sU), dot(wp, sV));
                            let dens = exp(-dot(sp2, sp2) / (SHAFT_R * SHAFT_R));
                            // now and then a mote turns a face to the sun: a short flash
                            let fl = fract(tt / 3.75 + hashI(id + vec3i(95, 3, 5)));
                            let br = DUST_BRIGHT * (1.0 + 8.0 * DUST_GLINT * ss(0.0, 0.004, fl)
                                * (1.0 - ss(0.004, 0.014, fl)));
                            dust += dens * exp(-dd * dd / (rr * rr))
                                * (0.0025 * 0.0025) / (rr0 * rr0) * (hm - 0.25) * 5.3 * br;
                        }
                    }
                }
                if (tMax.x < tMax.y && tMax.x < tMax.z) {
                    if (tMax.x > tEnd) {
                        break;
                    }
                    tMax.x += tDel.x;
                    id.x += i32(sg.x);
                } else if (tMax.y < tMax.z) {
                    if (tMax.y > tEnd) {
                        break;
                    }
                    tMax.y += tDel.y;
                    id.y += i32(sg.y);
                } else {
                    if (tMax.z > tEnd) {
                        break;
                    }
                    tMax.z += tDel.z;
                    id.z += i32(sg.z);
                }
            }
            col += SUNCOL * SUNE * phaseHG(dot(rd, L), 0.6) * dust;
        }
    }
    return col;
}

@fragment
fn fs_main(@builtin(position) frag_coord: vec4f) -> @location(0) vec4f {
    let fragCoord = fragCoordSH(frag_coord);
    let iResolution = mq_pass.resolution;
    let tNow = T();

    L = normalize(vec3f(-0.55 + 0.06 * sin(tNow * 0.7) * SUN_SWAY, 0.78,
                        0.42 + 0.05 * cos(tNow * 0.5) * SUN_SWAY));
    sU = normalize(cross(L, vec3f(0.0, 0.0, 1.0)));
    sV = cross(L, sU);
    sA = vec3f(0.12, 1.15, 0.32);

    let sw = CAM_SWAY;
    var look = vec3f(0.0);
    var ta = vec3f(0.05, 0.9, 0.2);
    var ro = vec3f(-0.2 + 0.45 * sin(tNow * 0.4) * sw, 1.3 + 0.1 * sin(tNow * 0.27) * sw, -4.6);
    {
        // the stored drag offset orbits the camera round the cluster: yaw free, elevation clamped
        let m = texelFetchSH(ch0, TX_OFFSET).xy;
        let o = ro - ta;
        var dist = length(o);
        // the elevation follows the drag, then eases towards PITCH_MAX (value and slope continuous);
        // over the upper half of the drag the camera moves in towards the crystals, smoothly
        let py = select(PITCH_SOFT + (PITCH_MAX - PITCH_SOFT)
            * (1.0 - exp(-(m.y - PITCH_SOFT) / (PITCH_MAX - PITCH_SOFT))), m.y, m.y <= PITCH_SOFT);
        var zu = clamp(m.y / PITCH_HI, 0.0, 1.0);
        zu = zu * zu * (3.0 - 2.0 * zu);
        dist *= mix(1.0, ZOOM_IN, zu);
        look = mix(ta, AIM_LOOK, vec3f(zu)); // ... the view tilts down into the crystal's rainbows
        ta = mix(ta, AIM_UP, vec3f(zu)); // ... and the camera rises along the big crystal
        let yaw = atan2(o.x, o.z) - m.x * YAW_PER * tau;
        let pit = asin(o.y / length(o)) + py * PITCH_PER * tau;
        ro = ta + dist * vec3f(cos(pit) * sin(yaw), sin(pit), cos(pit) * cos(yaw));
    }
    // the camera always looks at the cluster; PITCH_LO .. PITCH_HI keep it inside the cave and the
    // opening the sun shines through out of the frame at every yaw
    let fw = normalize(look - ro);
    camR = normalize(cross(vec3f(0.0, 1.0, 0.0), fw));
    camU = cross(fw, camR);

    var col = vec3f(0.0);
    for (var sy = 0; sy < AA; sy++) {
        for (var sx = 0; sx < AA; sx++) {
            let off = (vec2f(f32(sx), f32(sy)) + vec2f(0.5)) / f32(AA) - vec2f(0.5);
            let uv = (fragCoord + off - 0.5 * iResolution) / iResolution.y;
            let rd = normalize(uv.x * camR + uv.y * camU + FOCAL * fw);
            col += render(ro, rd);
        }
    }
    col /= f32(AA * AA);

    // rgb: the sun's core passes the half-float maximum (65504); a: rock share
    return vec4f(min(col, vec3f(6e4)), gRock / f32(AA * AA));
}
