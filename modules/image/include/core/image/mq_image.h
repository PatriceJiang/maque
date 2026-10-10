/* maque 图像解码（Wuffs，docs/08/13）：PNG/JPEG/GIF/BMP → RGBA8(非预乘)。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 解码图片为 RGBA8(非预乘)。成功 0；*out_rgba 由 mq_image_free 释放。 */
int  mq_image_decode(const void* data, size_t size,
                     uint8_t** out_rgba, uint32_t* out_w, uint32_t* out_h);
void mq_image_free(void* p);

const char* mq_image_last_error(void);

#ifdef __cplusplus
}
#endif
