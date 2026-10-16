// 图像解码(Wuffs) 单元测试：内嵌 2x2 PNG
#include "core/image/mq_image.h"
#include <print>

#include <cstdio>
#include <cstdint>
#include <cstring>

static const unsigned char kPng2x2[77] = {
137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,
114,182,13,36,0,0,0,20,73,68,65,84,120,218,99,248,207,192,240,31,12,129,52,16,
48,252,7,0,71,202,8,248,91,154,164,190,0,0,0,0,73,69,78,68,174,66,96,130
};

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    char err[128] = { 0 };
    int rc = mq_image_decode(kPng2x2, sizeof(kPng2x2), &rgba, &w, &h, err, sizeof err);
    if (rc != 0) { std::print("decode failed: {}\n", err); ++g_failures; }
    CHECK(rc == 0);
    CHECK(w == 2 && h == 2);
    if (rgba) {
        auto px = [&](int i, int c) { return (int)rgba[i * 4 + c]; };
        CHECK(px(0, 0) == 255 && px(0, 1) == 0 && px(0, 2) == 0 && px(0, 3) == 255);   // 红
        CHECK(px(1, 1) == 255 && px(1, 0) == 0);                                       // 绿
        CHECK(px(2, 2) == 255 && px(2, 0) == 0);                                       // 蓝
        CHECK(px(3, 0) == 255 && px(3, 1) == 255 && px(3, 2) == 0);                    // 黄
        mq_image_free(rgba);
    }
    // 非法输入
    uint8_t* bad = nullptr; uint32_t bw = 0, bh = 0;
    char err2[128] = { 0 };
    CHECK(mq_image_decode("xxxx", 4, &bad, &bw, &bh, err2, sizeof err2) != 0);

    /* ---- PNG 写出 → 用本模块解码器读回（自证编码有效；截图/预览路径）---- */
    {
        uint8_t src[2 * 2 * 4] = { 0 };                       // 2x2 RGBA
        for (int i = 0; i < 4; ++i) {
            src[i * 4 + 0] = (uint8_t)(40 + i * 50);
            src[i * 4 + 1] = (uint8_t)(200 - i * 30);
            src[i * 4 + 2] = 128;
            src[i * 4 + 3] = 255;
        }
        const char* out = "maque_image_write_test.png";
        CHECK(mq_image_write_png(out, 2, 2, src, 0) == 0);
        FILE* f = fopen(out, "rb");
        CHECK(f != nullptr);
        if (f) {
            uint8_t buf[4096]; const size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
            CHECK(n > 40);
            uint8_t* back = nullptr; uint32_t bw2 = 0, bh2 = 0; char e3[128] = { 0 };
            CHECK(mq_image_decode(buf, n, &back, &bw2, &bh2, e3, sizeof e3) == 0);
            CHECK(bw2 == 2 && bh2 == 2);
            if (back) {
                CHECK(memcmp(back, src, sizeof src) == 0);    // 逐字节往返（2*2*4）
                mq_image_free(back);
            }
            remove(out);
        }
    }
    CHECK(err2[0] != '\0');                       // 失败原因必须写入调用方缓冲（无全局错误态）

    if (g_failures == 0) { std::print("image_test: all checks passed\n"); return 0; }
    std::print("image_test: {} failure(s)\n", g_failures);
    return 1;
}
