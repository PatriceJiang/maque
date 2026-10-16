/* 内建 shader 解压（gfx 内部，非生成物）：把打包流水线产出的 Blob 解成 WGSL 文本。
 *
 * 打包：tools/shader_pack.js（zstd -19）；解压：libzstd（MQ_HAVE_ZSTD）。
 * 未链接 libzstd 时流水线会**直嵌原文**（compressed=0，本文件直接返回）→ 功能不变、体积略大。 */
#ifndef MAQUE_GFX_SHADER_DECOMPRESS_H
#define MAQUE_GFX_SHADER_DECOMPRESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 解出内建 shader 源码（'\0' 结尾的 WGSL 文本）。
 * 按 data 指针缓存 → 同一条目重复调用零开销、返回同一指针；失败（解压错误/无 zstd）返回 NULL。 */
const char* mq_shader_unpack(const char* name, const unsigned char* data, unsigned int size,
                             unsigned int raw_size, int compressed);

#ifdef __cplusplus
}
#endif
#endif /* MAQUE_GFX_SHADER_DECOMPRESS_H */
