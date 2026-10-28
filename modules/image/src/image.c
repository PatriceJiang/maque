/* Wuffs 单文件实现 + 统一解码入口。编译为 C11。 */
#define WUFFS_IMPLEMENTATION
#include "wuffs-v0.4.c"

#include "core/image/mq_image.h"

/* KTX2/Basis Universal（C++，modules/image/src/ktx2_decode.cpp）。 */
extern int mq_ktx2_decode(const void* data, size_t size, uint8_t** out_rgba,
                          uint32_t* out_w, uint32_t* out_h, char* err, size_t err_cap);

#include <stdlib.h>
#include <string.h>



void mq_image_free(void* p) { free(p); }

#define MQ_MAX_PIXELS (64u * 1024u * 1024u)

/* 错误写入调用方缓冲（可 NULL）—— 模块内无任何可变静态状态（ADR-0014）。 */
static void set_err(char* err, size_t cap, const char* msg) {
    if (!err || cap == 0) return;
    size_t i = 0;
    for (; msg[i] && i + 1 < cap; ++i) err[i] = msg[i];
    err[i] = '\0';
}

static int decode_with(wuffs_base__image_decoder* dec, const void* data, size_t size,
                       uint8_t** out_rgba, uint32_t* out_w, uint32_t* out_h,
                       char* err, size_t err_cap) {
    wuffs_base__io_buffer src =
        wuffs_base__ptr_u8__reader((uint8_t*)data, size, true);

    wuffs_base__image_config ic = ((wuffs_base__image_config){});
    wuffs_base__status st = wuffs_base__image_decoder__decode_image_config(dec, &ic, &src);
    if (!wuffs_base__status__is_ok(&st)) { set_err(err, err_cap, wuffs_base__status__message(&st)); return 1; }

    uint32_t w = wuffs_base__pixel_config__width(&ic.pixcfg);
    uint32_t h = wuffs_base__pixel_config__height(&ic.pixcfg);
    if (w == 0 || h == 0 || (uint64_t)w * h > MQ_MAX_PIXELS) { set_err(err, err_cap, "image too large"); return 1; }

    // 统一为 RGBA(非预乘)
    wuffs_base__pixel_config__set(&ic.pixcfg, WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL,
                                  WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, w, h);

    uint64_t plen = wuffs_base__pixel_config__pixbuf_len(&ic.pixcfg);
    uint8_t* pix = (uint8_t*)malloc(plen);
    if (!pix) { set_err(err, err_cap, "oom"); return 1; }
    wuffs_base__slice_u8 pixbuf = wuffs_base__make_slice_u8(pix, plen);

    wuffs_base__pixel_buffer pb = ((wuffs_base__pixel_buffer){});
    st = wuffs_base__pixel_buffer__set_from_slice(&pb, &ic.pixcfg, pixbuf);
    if (!wuffs_base__status__is_ok(&st)) { set_err(err, err_cap, wuffs_base__status__message(&st)); free(pix); return 1; }

    uint64_t wlen = wuffs_base__image_decoder__workbuf_len(dec).max_incl;
    if (wlen > MQ_MAX_PIXELS) { set_err(err, err_cap, "workbuf too large"); free(pix); return 1; }
    uint8_t* work = wlen ? (uint8_t*)malloc(wlen) : NULL;
    wuffs_base__slice_u8 workbuf = wuffs_base__make_slice_u8(work, wlen);

    bool seen_ok = false;
    for (;;) {
        wuffs_base__frame_config fc = ((wuffs_base__frame_config){});
        st = wuffs_base__image_decoder__decode_frame_config(dec, &fc, &src);
        if (!wuffs_base__status__is_ok(&st)) {
            if (st.repr != wuffs_base__note__end_of_data || !seen_ok) {
                set_err(err, err_cap, wuffs_base__status__message(&st)); free(work); free(pix); return 1;
            }
            break;
        }
        st = wuffs_base__image_decoder__decode_frame(dec, &pb, &src,
                                                     WUFFS_BASE__PIXEL_BLEND__SRC, workbuf, NULL);
        if (!wuffs_base__status__is_ok(&st)) {
            set_err(err, err_cap, wuffs_base__status__message(&st)); free(work); free(pix); return 1;
        }
        seen_ok = true;
        break;  // 只取第一帧
    }
    free(work);

    *out_rgba = pix; *out_w = w; *out_h = h;
    return 0;
}

int mq_image_decode(const void* data, size_t size,
                    uint8_t** out_rgba, uint32_t* out_w, uint32_t* out_h,
                    char* err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (out_rgba) *out_rgba = NULL;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!data || size < 8) { set_err(err, err_cap, "bad input"); return 1; }
    const uint8_t* p = (const uint8_t*)data;

    /* KTX2 标识：AB 4B 54 58 20 32 30 BB 0D 0A 1A 0A */
    if (size >= 12 && p[0] == 0xAB && p[1] == 'K' && p[2] == 'T' && p[3] == 'X' &&
        p[4] == ' ' && p[5] == '2' && p[6] == '0' && p[7] == 0xBB &&
        p[8] == '\r' && p[9] == '\n' && p[10] == 0x1A && p[11] == '\n') {
        return mq_ktx2_decode(data, size, out_rgba, out_w, out_h, err, err_cap);
    }

    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
        wuffs_png__decoder* d = wuffs_png__decoder__alloc();
        if (!d) { set_err(err, err_cap, "oom"); return 1; }
        wuffs_base__status st = wuffs_png__decoder__initialize(d, sizeof(*d), WUFFS_VERSION, 0);
        int rc = wuffs_base__status__is_ok(&st)
            ? decode_with(wuffs_png__decoder__upcast_as__wuffs_base__image_decoder(d),
                          data, size, out_rgba, out_w, out_h, err, err_cap) : (set_err(err, err_cap, "init"), 1);
        free(d); return rc;
    }
    if (p[0] == 0xFF && p[1] == 0xD8) {
        wuffs_jpeg__decoder* d = wuffs_jpeg__decoder__alloc();
        if (!d) { set_err(err, err_cap, "oom"); return 1; }
        wuffs_base__status st = wuffs_jpeg__decoder__initialize(d, sizeof(*d), WUFFS_VERSION, 0);
        int rc = wuffs_base__status__is_ok(&st)
            ? decode_with(wuffs_jpeg__decoder__upcast_as__wuffs_base__image_decoder(d),
                          data, size, out_rgba, out_w, out_h, err, err_cap) : (set_err(err, err_cap, "init"), 1);
        free(d); return rc;
    }
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F') {
        wuffs_gif__decoder* d = wuffs_gif__decoder__alloc();
        if (!d) { set_err(err, err_cap, "oom"); return 1; }
        wuffs_base__status st = wuffs_gif__decoder__initialize(d, sizeof(*d), WUFFS_VERSION, 0);
        int rc = wuffs_base__status__is_ok(&st)
            ? decode_with(wuffs_gif__decoder__upcast_as__wuffs_base__image_decoder(d),
                          data, size, out_rgba, out_w, out_h, err, err_cap) : (set_err(err, err_cap, "init"), 1);
        free(d); return rc;
    }
    if (p[0] == 'B' && p[1] == 'M') {
        wuffs_bmp__decoder* d = wuffs_bmp__decoder__alloc();
        if (!d) { set_err(err, err_cap, "oom"); return 1; }
        wuffs_base__status st = wuffs_bmp__decoder__initialize(d, sizeof(*d), WUFFS_VERSION, 0);
        int rc = wuffs_base__status__is_ok(&st)
            ? decode_with(wuffs_bmp__decoder__upcast_as__wuffs_base__image_decoder(d),
                          data, size, out_rgba, out_w, out_h, err, err_cap) : (set_err(err, err_cap, "init"), 1);
        free(d); return rc;
    }
    set_err(err, err_cap, "unsupported image format");
    return 1;
}
