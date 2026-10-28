# basis_universal（vendored subset）

只用于 KTX2 / Basis Universal 解码（runtime `modules/image`），来源：

- 上游：https://github.com/BinomialLLC/basis_universal
- commit：`ee771a7f913a3233a252eb6d281b4446f6a3c9f5`（2026-10-23）
- 保留：`transcoder/`（basisu_transcoder.cpp + headers + tables）、`LICENSE`、`NOTICE`
- 未保留：编码器/CLI、`zstd/`（本工程改用已有 libzstd；`zstd/zstd.h` 只是转发头）

构建：见本目录 `CMakeLists.txt`；`maque_image` 链接 `maque::basisu`。
KTX2 解码入口：`modules/image/src/ktx2_decode.cpp`（BasisU → RGBA8；未压缩 2D RGBA/RGB/BGRA 直接解析）。
