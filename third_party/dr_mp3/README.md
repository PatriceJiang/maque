# dr_mp3

- 上游：https://github.com/mackron/dr_libs （`dr_mp3.h`，v0.7.4）
- 许可：public domain / MIT-0（双许可，见文件末尾）
- 用途：MP3 解码（`modules/world/audio/src/decode_mp3.cpp`，`DR_MP3_IMPLEMENTATION` 只在一个 TU 展开）
- 更新方式：替换 `dr_mp3.h` 后跑 `ctest -R audio`；不要改上游文件。
