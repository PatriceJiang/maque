#!/usr/bin/env node
/* 程序化贴图生成（PNG，无第三方依赖）
 *
 *   import { writePNG, makeSky, makeAsphalt, makeGround } from './gen_tex.mjs'
 *
 * 产出都是确定性的：同一参数逐字节一致（noise 用整数哈希，不用 Math.random）。
 */
import zlib from 'node:zlib';
import fs from 'node:fs';
import path from 'node:path';

/* ---------------- PNG ---------------- */
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; ++n) {
    let c = n;
    for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; ++i) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length, 0);
  const t = Buffer.from(type, 'ascii');
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(Buffer.concat([t, data])), 0);
  return Buffer.concat([len, t, data, crc]);
}

export function writePNG(file, w, h, rgba) {
  const stride = w * 4;
  const raw = Buffer.alloc((stride + 1) * h);
  for (let y = 0; y < h; ++y) {
    raw[y * (stride + 1)] = 0;
    rgba.copy ? rgba.copy(raw, y * (stride + 1) + 1, y * stride, y * stride + stride)
              : Buffer.from(rgba.buffer, y * stride, stride).copy(raw, y * (stride + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const png = Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, png);
  return png.length;
}

/* ---------------- 噪声 ---------------- */
function hash2(x, y, seed) {
  let h = x * 374761393 + y * 668265263 + seed * 2147483647;
  h = (h ^ (h >>> 13)) * 1274126177;
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}
function vnoise(x, y, seed) {
  const xi = Math.floor(x), yi = Math.floor(y);
  const xf = x - xi, yf = y - yi;
  const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
  const a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed);
  const c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
  return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}
function fbm(x, y, seed, oct = 4) {
  let s = 0, amp = 0.5, f = 1;
  for (let i = 0; i < oct; ++i) { s += amp * vnoise(x * f, y * f, seed + i * 17); f *= 2; amp *= 0.5; }
  return s;
}
/* 周期化的 fbm：贴图要无缝，采样点按整数周期环绕 */
function pfbm(x, y, seed, px, py, oct = 4) {
  let s = 0, amp = 0.5;
  for (let i = 0; i < oct; ++i) {
    const fx = (x * (1 << i)) % px, fy = (y * (1 << i)) % py;
    const a = hash2(Math.floor((fx / px) * px) & 255, Math.floor((fy / py) * py) & 255, seed + i * 17);
    s += amp * a; amp *= 0.5;
  }
  return s;
}

/* 周期化 value noise：晶格坐标按 (px,py) 取模，保证 u=0 与 u=1 接缝一致 */
function vnoiseP(x, y, px, py, seed) {
  const xi = Math.floor(x), yi = Math.floor(y);
  const xf = x - xi, yf = y - yi;
  const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
  const w = (a, b) => hash2(((a % px) + px) % px, ((b % py) + py) % py, seed);
  const a = w(xi, yi), b = w(xi + 1, yi), c = w(xi, yi + 1), d = w(xi + 1, yi + 1);
  return (a * (1 - u) + b * u) * (1 - v) + (c * (1 - u) + d * u) * v;
}
function pfbmP(x, y, px, py, seed, oct) {
  let s = 0, amp = 0.5, f = 1, P = 1;
  for (let i = 0; i < (oct || 4); ++i) {
    s += amp * vnoiseP(x * f, y * f, px * f, py * f, seed + i * 17);
    f *= 2; amp *= 0.5;
  }
  return s;
}
const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const lerp = (a, b, t) => a + (b - a) * t;

/* ---------------- 天空（等距柱状投影） ---------------- */
/* spec: { zenith:[r,g,b], horizon:[..], ground:[..], sunEl, sunAz, sunSize, sunColor,
 *         clouds:0..1, stars:0..1, seed }  —— 颜色都是 0..1 的 sRGB */
export function makeSky(w, h, spec) {
  const buf = Buffer.alloc(w * h * 4);
  const sunEl = spec.sunEl ?? 18, sunAz = spec.sunAz ?? 200;
  const sunDir = [
    Math.cos((sunEl * Math.PI) / 180) * Math.cos((sunAz * Math.PI) / 180),
    Math.sin((sunEl * Math.PI) / 180),
    Math.cos((sunEl * Math.PI) / 180) * Math.sin((sunAz * Math.PI) / 180),
  ];
  for (let y = 0; y < h; ++y) {
    const el = (0.5 - y / h) * Math.PI;            // +90°..-90°
    for (let x = 0; x < w; ++x) {
      const az = (x / w) * Math.PI * 2;
      const dir = [Math.cos(el) * Math.cos(az), Math.sin(el), Math.cos(el) * Math.sin(az)];
      let r, g, b;
      if (el >= 0) {
        const t = Math.pow(clamp01(el / (Math.PI * 0.5)), 0.55);
        r = lerp(spec.horizon[0], spec.zenith[0], t);
        g = lerp(spec.horizon[1], spec.zenith[1], t);
        b = lerp(spec.horizon[2], spec.zenith[2], t);
      } else {
        const t = clamp01(-el / (Math.PI * 0.5));
        r = lerp(spec.horizon[0], spec.ground[0], Math.pow(t, 0.4));
        g = lerp(spec.horizon[1], spec.ground[1], Math.pow(t, 0.4));
        b = lerp(spec.horizon[2], spec.ground[2], Math.pow(t, 0.4));
      }
      // 太阳：圆盘 + 大范围辉光
      const cosA = dir[0] * sunDir[0] + dir[1] * sunDir[1] + dir[2] * sunDir[2];
      const ang = Math.acos(clamp01(cosA * 0.5 + 0.5)) * 2;
      const disc = clamp01(1 - ang / (spec.sunSize ?? 0.02));
      const glow = Math.pow(clamp01(1 - ang / 0.55), 3.0) * (spec.sunGlow ?? 0.55);
      const sc = spec.sunColor ?? [1.0, 0.92, 0.75];
      r += sc[0] * (disc * 2.2 + glow);
      g += sc[1] * (disc * 2.2 + glow);
      b += sc[2] * (disc * 2.2 + glow);
      // 云：只在上半球，用 fbm 拉成条
      if (spec.clouds > 0 && el > 0.02) {
        const u = x / w * 6, v = (y / h) * 3;
        const n = fbm(u, v * 2.2, spec.seed + 3, 4);
        const cover = clamp01((n - (1 - spec.clouds)) * 3.2) * clamp01(el / 0.25) * (1 - clamp01(el / 1.35));
        r = lerp(r, 0.97, cover * 0.85);
        g = lerp(g, 0.97, cover * 0.85);
        b = lerp(b, 1.0, cover * 0.85);
      }
      // 星：稀疏亮点（夜景）
      if (spec.stars > 0 && el > 0.08) {
        const hsh = hash2(x, y, spec.seed + 91);
        if (hsh > 1 - spec.stars * 0.35) {
          const br = 0.35 + ((hsh * 977) % 1) * 0.55;
          r += br; g += br * 1.0; b += br * 1.05;
        }
      }
      const o = (y * w + x) * 4;
      buf[o] = Math.round(clamp01(r) * 255);
      buf[o + 1] = Math.round(clamp01(g) * 255);
      buf[o + 2] = Math.round(clamp01(b) * 255);
      buf[o + 3] = 255;
    }
  }
  return buf;
}

/* ---------------- 沥青（无缝平铺） ---------------- */
export function makeAsphalt(size, spec) {
  const buf = Buffer.alloc(size * size * 4);
  const base = spec.base ?? 0.88;   // 贴图乘在 base_color 上：贴近白色，只提供颗粒
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const u = x / size, v = y / size;
      // 多尺度颗粒（平铺用 8/16/32 周期）
      const n1 = vnoiseP(u * 8, v * 8, 8, 8, 11);
      const n2 = vnoiseP(u * 32, v * 32, 32, 32, 12);
      const n3 = vnoiseP(u * 96, v * 96, 96, 96, 13);
      let g = base + (n1 - 0.5) * 0.10 + (n2 - 0.5) * 0.08 + (n3 - 0.5) * 0.05;
      // 沥青补丁：低频斑块
      const patch = pfbmP(u * 4, v * 4, 4, 4, 21, 3);
      if (patch > 0.62) g *= 0.88;
      // 细颗粒麻点
      const speck = hash2(x, y, 31);
      if (speck > 0.985) g += 0.10;
      g = clamp01(g);
      const o = (y * size + x) * 4;
      buf[o] = Math.round(g * 255);
      buf[o + 1] = Math.round(g * 255);
      buf[o + 2] = Math.round(g * 255);
      buf[o + 3] = 255;
    }
  }
  return buf;
}

/* ---------------- 地面（沙 / 草 / 城市地砖） ---------------- */
export function makeGround(size, spec) {
  const buf = Buffer.alloc(size * size * 4);
  const c = spec.color;          // [r,g,b] sRGB
  const amount = spec.amount ?? 0.12;
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const u = x / size, v = y / size;
      const n = vnoiseP(u * 16, v * 16, 16, 16, 41) * 0.6 + vnoiseP(u * 48, v * 48, 48, 48, 42) * 0.4;
      const d = 1 + (n - 0.5) * amount * 2;
      const patch = pfbmP(u * 6, v * 6, 6, 6, 43, 3);
      const d2 = patch > 0.6 ? 1 - (patch - 0.6) * 0.5 : 1;
      const o = (y * size + x) * 4;
      const lum = clamp01(d * d2 * 0.94);
      buf[o] = Math.round(lum * 255);
      buf[o + 1] = Math.round(lum * 255);
      buf[o + 2] = Math.round(lum * 255);
      buf[o + 3] = 255;
    }
  }
  return buf;
}



/* ---------------- 楼体立面（漫反射 + 自发光，暖/冷窗混排） ---------------- */
const WIN_WARM = [1.0, 0.84, 0.55];
const WIN_COOL = [0.55, 0.85, 1.0];
export function makeFacade(size, seed, mode) {
  const buf = Buffer.alloc(size * size * 4);
  const cols = 14, rows = 18;
  const cw = size / cols, ch = size / rows;
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      const cx = Math.floor(x / cw), cy = Math.floor(y / ch);
      const gx = x - cx * cw, gy = y - cy * ch;
      const band = gy < ch * 0.14;
      const inWin = !band && gx > cw * 0.22 && gx < cw * 0.78 && gy > ch * 0.28 && gy < ch * 0.86;
      const h = hash2(cx, cy, seed);
      const lit = inWin && h > 0.42;
      const cool = h > 0.78;
      const wc = cool ? WIN_COOL : WIN_WARM;
      const bright = 0.7 + ((h * 977) % 1) * 0.3;
      let r, gg, b;
      if (mode === 'emissive') {
        r = lit ? wc[0] * bright : 0;
        gg = lit ? wc[1] * bright : 0;
        b = lit ? wc[2] * bright : 0;
      } else if (band) {
        r = 0.30; gg = 0.30; b = 0.34;
      } else if (inWin) {
        r = lit ? 0.86 : 0.26; gg = lit ? 0.84 : 0.30; b = lit ? 0.82 : 0.38;
      } else {
        r = 0.34; gg = 0.35; b = 0.40;
      }
      const o = (y * size + x) * 4;
      buf[o] = Math.round(clamp01(r) * 255);
      buf[o + 1] = Math.round(clamp01(gg) * 255);
      buf[o + 2] = Math.round(clamp01(b) * 255);
      buf[o + 3] = 255;
    }
  }
  return buf;
}
