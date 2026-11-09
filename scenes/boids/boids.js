/* Boids锛圧eynolds 鍒嗙/瀵归綈/鑱氬悎锛? render.ParticleField3D 姣忓抚 upload銆? *
 * 3D 绮掑瓙鍦哄彧鍋氣€滄樉绀衡€濓細妯℃嫙鍦ㄨ剼鏈噷锛堝浐瀹氭闀裤€佸紩鎿?rng 鍒濆鍖?鈫?纭畾鎬э級锛? * 姣忓抚鎶?N 涓矑瀛愭墦鍖呮垚 48B/绮掑瓙锛坧os3,size,color4,rot,pad3锛夌殑 Float32Array 涓婁紶銆? * 绌洪棿缃戞牸锛?6鲁锛宑ell=2.5锛夐伩鍏?O(N虏)锛汵 鐢?BOIDS_N 鎺у埗锛堥粯璁?1500锛屼笂闄?4000锛夈€? * 鍙负韪╂覆鏌?鑴氭湰閫氳矾锛氫笉鍋氶殰纰嶇墿銆佷笉鍐欏洖鍦烘櫙缁勪欢銆?*/
const N = Math.max(100, Math.min(4000, parseInt(env("BOIDS_N") || "300", 10) || 300));
const CELL = 2.5;
const GI = 16;                 // 16鲁 缃戞牸锛岃鐩?[-20,20]
const GOFF = 8;
const SEP2 = 1.5 * 1.5;
const ALI2 = 3.0 * 3.0;
const MAX_NEIGHBORS = 32;      // 閲囨牱涓婇檺锛氶伩鍏?Debug/ASan 涓?JS 閭诲眳寰幆鎴愪负鐡堕

const px = new Float32Array(N), py = new Float32Array(N), pz = new Float32Array(N);
const vx = new Float32Array(N), vy = new Float32Array(N), vz = new Float32Array(N);
const next = new Int32Array(N);
const cellHead = new Int32Array(GI * GI * GI);
const data = new Float32Array(N * 12);
let flock = -1;
let inited = false;
let lastFrame = -1;

function cell_index(x, y, z) {
    const ix = Math.floor(x / CELL) + GOFF;
    const iy = Math.floor(y / CELL) + GOFF;
    const iz = Math.floor(z / CELL) + GOFF;
    if (ix < 0 || ix >= GI || iy < 0 || iy >= GI || iz < 0 || iz >= GI) return -1;
    return (ix * GI + iy) * GI + iz;
}

function init() {
    for (let i = 0; i < N; ++i) {
        px[i] = (rng() * 2.0 - 1.0) * 7.5;
        py[i] = (rng() * 2.0 - 1.0) * 7.5;
        pz[i] = (rng() * 2.0 - 1.0) * 7.5;
        const a = rng() * Math.PI * 2.0;
        const b = rng() * Math.PI - Math.PI * 0.5;
        const s = 2.0 + rng() * 2.0;
        vx[i] = Math.cos(a) * Math.cos(b) * s;
        vy[i] = Math.sin(b) * s;
        vz[i] = Math.sin(a) * Math.cos(b) * s;
    }
    log("[boids] N=" + N + " field=" + flock + " cell=" + CELL);
    inited = true;
}

function update(dt) {
    if (flock < 0) flock = entity_find("flock");
    if (flock < 0) return;
    if (!inited) init();
    if (dt <= 0.0) return;
    const h = Math.min(dt, 0.05);

    /* 缃戞牸閲嶅缓锛堥摼琛紝闆跺垎閰嶏級 */
    cellHead.fill(-1);
    for (let i = 0; i < N; ++i) {
        const c = cell_index(px[i], py[i], pz[i]);
        next[i] = c >= 0 ? cellHead[c] : -2;
        if (c >= 0) cellHead[c] = i;
    }

    const frame = director_frame();
    for (let i = 0; i < N; ++i) {
        const x = px[i], y = py[i], z = pz[i];
        let ax = 0.0, ay = 0.0, az = 0.0;
        let sx = 0.0, sy = 0.0, sz = 0.0;
        let axp = 0.0, ayp = 0.0, azp = 0.0;
        let cx = 0.0, cy = 0.0, cz = 0.0;
        let n = 0;
        const ix = Math.floor(x / CELL) + GOFF;
        const iy = Math.floor(y / CELL) + GOFF;
        const iz = Math.floor(z / CELL) + GOFF;
        outer:
        for (let gx = -1; gx <= 1; ++gx) {
            const jx = ix + gx;
            if (jx < 0 || jx >= GI) continue;
            for (let gy = -1; gy <= 1; ++gy) {
                const jy = iy + gy;
                if (jy < 0 || jy >= GI) continue;
                for (let gz = -1; gz <= 1; ++gz) {
                    const jz = iz + gz;
                    if (jz < 0 || jz >= GI) continue;
                    const c = (jx * GI + jy) * GI + jz;
                    for (let j = cellHead[c]; j >= 0; j = next[j]) {
                        if (j === i) continue;
                        const dx = px[j] - x, dy = py[j] - y, dz = pz[j] - z;
                        const d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 > ALI2 || d2 < 1e-8) continue;
                        n++;
                        axp += vx[j]; ayp += vy[j]; azp += vz[j];
                        cx += px[j]; cy += py[j]; cz += pz[j];
                        if (d2 < SEP2) {
                            const inv = 1.0 / d2;
                            sx -= dx * inv; sy -= dy * inv; sz -= dz * inv;
                        }
                    }
                    if (n >= MAX_NEIGHBORS) break outer;
                }
            }
        }
        ax = sx * 2.2; ay = sy * 2.2; az = sz * 2.2;
        if (n > 0) {
            const inv = 1.0 / n;
            ax += (axp * inv - vx[i]) * 1.1;
            ay += (ayp * inv - vy[i]) * 1.1;
            az += (azp * inv - vz[i]) * 1.1;
            ax += (cx * inv - x) * 0.8;
            ay += (cy * inv - y) * 0.8;
            az += (cz * inv - z) * 0.8;
        }
        /* 杞竟鐣岋細瓒婄晫寰€鍥炴媺锛堜笉鍋氱‖鍙嶅脊锛岄伩鍏嶈钀藉爢绉級 */
        if (x > 8.5) ax -= (x - 10.5) * 3.0;
        if (x < -8.5) ax += (-10.5 - x) * 3.0;
        if (y > 8.5) ay -= (y - 10.5) * 3.0;
        if (y < -8.5) ay += (-10.5 - y) * 3.0;
        if (z > 8.5) az -= (z - 10.5) * 3.0;
        if (z < -8.5) az += (-10.5 - z) * 3.0;

        let nvx = vx[i] + ax * h;
        let nvy = vy[i] + ay * h;
        let nvz = vz[i] + az * h;
        let sp = Math.sqrt(nvx * nvx + nvy * nvy + nvz * nvz);
        if (sp > 5.0) { nvx *= 5.0 / sp; nvy *= 5.0 / sp; nvz *= 5.0 / sp; sp = 5.0; }
        if (sp < 1.2) {
            const k = sp > 1e-4 ? 1.2 / sp : 0.0;
            nvx *= k; nvy *= k; nvz *= k; sp = 1.2;
        }
        vx[i] = nvx; vy[i] = nvy; vz[i] = nvz;
        px[i] = x + nvx * h; py[i] = y + nvy * h; pz[i] = z + nvz * h;

        /* 鎵撳寘锛歱os3,size,color4,rot,pad3锛?2脳f32 = 48B锛?*/
        const o = i * 12;
        const t = Math.min(1.0, Math.max(0.0, (sp - 1.2) / 3.8));
        data[o + 0] = px[i]; data[o + 1] = py[i]; data[o + 2] = pz[i];
        data[o + 3] = 0.16 + 0.12 * t;
        data[o + 4] = 0.30 + 0.55 * t;
        data[o + 5] = 0.70 + 0.30 * t;
        data[o + 6] = 1.0 - 0.25 * t;
        data[o + 7] = 0.80;
        data[o + 8] = Math.atan2(nvy, nvx);
        data[o + 9] = data[o + 10] = data[o + 11] = 0.0;
    }

    particles3d_upload(flock, new Uint8Array(data.buffer));
    if (frame !== lastFrame && (frame % 120) === 0) {
        lastFrame = frame;
        log("[boids] frame=" + frame + " count=" + particles3d_count(flock));
    }
}
