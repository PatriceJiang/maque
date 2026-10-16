/* PNG 编码（image 模块的写出侧：截图 / 预览用；解码仍是 Wuffs）。
 *
 * 结构：签名 + IHDR(RGBA8) + IDAT + IEND，每个 chunk 带 CRC32；IDAT 是 zlib 流
 * （deflate + Adler32）。压缩：链接 zlib 时用 compress2（真压缩）；否则退化为 **stored
 * deflate**（不压缩但仍是合法 PNG，任何解码器都能读）。
 * 无模块级可变状态（ADR-0014）：所有缓冲栈上/调用方。 */
#include "core/image/mq_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 与解码侧一致的像素上限（image.c 的 MQ_MAX_PIXELS）：防写出超大图。 */
#define MQ_MAX_PIXELS (64u * 1024u * 1024u)

#ifdef MQ_HAVE_ZLIB
#include <zlib.h>
#endif

static uint32_t crc32_bytes(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* 写一个 chunk：长度 + 类型 + 数据 + CRC（覆盖类型 + 数据）。 */
static int write_chunk(FILE* f, const char type[4], const uint8_t* data, uint32_t len) {
    uint8_t head[8];
    put_be32(head, len);
    memcpy(head + 4, type, 4);
    if (fwrite(head, 1, 8, f) != 8) return 1;
    if (len && fwrite(data, 1, len, f) != len) return 1;
    uint32_t crc = crc32_bytes(0, head + 4, 4);
    if (len) crc = crc32_bytes(crc, data, len);
    uint8_t tail[4];
    put_be32(tail, crc);
    return fwrite(tail, 1, 4, f) == 4 ? 0 : 1;
}

/* 原始扫描线（每行前加 filter 0）→ deflate（zlib 或 stored）→ 追加 Adler32 前的 zlib 头。 */
static uint8_t* make_zlib_stream(const uint8_t* raw, size_t raw_size, size_t* out_size) {
    if (raw_size > 0x7FFFFFFFu) return NULL;                       // deflate 限制
#ifdef MQ_HAVE_ZLIB
    uLongf bound = compressBound((uLong)raw_size);
    uint8_t* out = (uint8_t*)malloc((size_t)bound);
    if (!out) return NULL;
    if (compress2(out, &bound, raw, (uLong)raw_size, 6) != Z_OK) { free(out); return NULL; }
    *out_size = (size_t)bound;
    return out;
#else
    /* stored deflate：zlib 头(2) + 每 65535 字节一块(5 + n) + 最后一块(5 + n) + Adler32(4) */
    const size_t blocks = (raw_size + 65534) / 65535;
    const size_t zlen = 2 + (blocks ? blocks : 1) * 5 + raw_size + 4;
    uint8_t* out = (uint8_t*)malloc(zlen);
    if (!out) return NULL;
    size_t o = 0;
    out[o++] = 0x78; out[o++] = 0x01;                              // zlib：deflate, 无字典, 最快
    size_t off = 0;
    do {
        const size_t n = raw_size - off > 65535 ? 65535 : raw_size - off;
        const int last = (off + n >= raw_size) ? 1 : 0;
        out[o++] = (uint8_t)last;                                   // BFINAL + BTYPE=00（对齐到字节）
        out[o++] = (uint8_t)(n & 0xFF); out[o++] = (uint8_t)(n >> 8);
        out[o++] = (uint8_t)(~n & 0xFF); out[o++] = (uint8_t)((~n >> 8) & 0xFF);
        if (n) memcpy(out + o, raw + off, n);
        o += n;
        off += n;
    } while (off < raw_size);
    uint32_t a = 1, b = 0;                                          // Adler32
    for (size_t i = 0; i < raw_size; ++i) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(out + o, (b << 16) | a);
    o += 4;
    *out_size = o;
    return out;
#endif
}

int mq_image_write_png(const char* path, uint32_t w, uint32_t h, const void* rgba, uint32_t stride) {
    if (!path || !rgba || w == 0 || h == 0) return 1;
    const uint32_t row = stride ? stride : w * 4u;
    if (row < w * 4u) return 1;
    if ((uint64_t)w * h > (uint64_t)MQ_MAX_PIXELS) return 1;
    if ((uint64_t)row * h + h > 0x7FFFFFFFu) return 1;

    /* 原始数据：每行前置 filter 0 */
    const size_t raw_size = (size_t)row * h + h;
    uint8_t* raw = (uint8_t*)malloc(raw_size);
    if (!raw) return 1;
    const uint8_t* src = (const uint8_t*)rgba;
    for (uint32_t y = 0; y < h; ++y) {
        raw[(size_t)y * (row + 1)] = 0;
        memcpy(raw + (size_t)y * (row + 1) + 1, src + (size_t)y * row, row);
    }
    size_t zlen = 0;
    uint8_t* z = make_zlib_stream(raw, raw_size, &zlen);
    free(raw);
    if (!z) return 1;

    FILE* f = fopen(path, "wb");
    if (!f) { free(z); return 1; }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : 1;
    uint8_t ihdr[13];
    put_be32(ihdr, w); put_be32(ihdr + 4, h);
    ihdr[8] = 8;          /* bit depth */
    ihdr[9] = 6;          /* color type: RGBA */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (!rc) rc = write_chunk(f, "IHDR", ihdr, sizeof ihdr);
    if (!rc) rc = write_chunk(f, "IDAT", z, (uint32_t)zlen);
    if (!rc) rc = write_chunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0) rc = 1;
    return rc;
}
