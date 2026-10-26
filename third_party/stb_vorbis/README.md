# stb_vorbis

- 上游：https://github.com/nothings/stb （`stb_vorbis.c`，v1.22）
- 许可：public domain（文件头与文末均有声明）
- 用途：OGG Vorbis 解码（`modules/world/audio/src/decode_ogg.cpp`，整段解码为 float）
- 更新方式：替换 `stb_vorbis.c` 后跑 `ctest -R audio`；不要改上游文件。
