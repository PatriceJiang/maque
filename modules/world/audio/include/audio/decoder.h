/* 解码器接口：字节 → Clip（交错 float32）。
 *
 * 分层/解耦：
 *   - 这里不依赖 VFS / 引擎 / SDL；调用方（runtime）负责按资产路径读字节；
 *   - 加一种第三方解码库（stb_vorbis / dr_mp3 / libflac…）= 新增一个 DecodeFn 并登记到表里；
 *   - P0 只有 WAV；OGG/MP3 是 P1/P2，不动混音器与设备后端。
 */
#pragma once

#include "audio/mixer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace maque::audio {

/* 解码成功返回 true 并写 out；失败返回 false 并写 error（面向日志的一句话原因）。 */
using DecodeFn = bool (*)(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out,
                          std::string& error);

bool decode_wav(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out, std::string& error);
bool decode_ogg(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out, std::string& error);
bool decode_mp3(const uint8_t* data, size_t size, std::shared_ptr<Clip>& out, std::string& error);

/* 按扩展名（大小写不敏感）选择解码器；hint 为空/未知时依次尝试（WAV 自带 RIFF 校验）。 */
bool decode_audio(const void* data, size_t size, const char* hint, std::shared_ptr<Clip>& out,
                  std::string& error);

} // namespace maque::audio
