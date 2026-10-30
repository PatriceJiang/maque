'use strict';

function bytes_equal(a, b) {
    if (!(a instanceof Uint8Array) || !(b instanceof Uint8Array) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; ++i) if (a[i] !== b[i]) return false;
    return true;
}

function on_start() {
    if (!zstd.available() || !zlib.available()) {
        log('compression_e2e unavailable');
        quit();
        return;
    }
    const raw = new Uint8Array(8192);
    for (let i = 0; i < raw.length; ++i) raw[i] = i % 31 === 0 ? 0 : (i % 7) + 65;
    raw[7] = 255;

    const zs = zstd.compress(raw, 3);
    const zr = zstd.decompress(zs);
    const zl = zlib.compress(raw.buffer, 6);
    const zlr = zlib.decompress(zl);
    const text = zstd.decompress(zstd.compress('A\0B', 0));
    const ok = zs.length > 0 && zl.length > 0 && zs.length < raw.length && zl.length < raw.length
        && bytes_equal(raw, zr) && bytes_equal(raw, zlr)
        && text.length === 3 && text[0] === 65 && text[1] === 0 && text[2] === 66;
    log('compression_e2e ' + (ok ? 'ok' : 'FAIL') + ' lang=js zstd=' + zs.length + ' zlib=' + zl.length);
    quit();
}

function update() {}
