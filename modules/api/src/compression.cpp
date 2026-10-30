/* zstd / zlib script APIs (schema/data/{zstd,zlib}.yaml).
 * Binary values are copied by the generated binding into the target VM. */
#include "maque_api.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#ifdef MQ_API_HAVE_ZSTD
#include <zstd.h>
#endif
#ifdef MQ_API_HAVE_ZLIB
#include <zlib.h>
#endif

namespace {

constexpr size_t kMaxBytes = 64u * 1024u * 1024u;
constexpr size_t kChunkBytes = 64u * 1024u;
constexpr uint8_t kEmptyByte = 0;

bool valid_input(std::span<const uint8_t> data) {
    return data.size() <= kMaxBytes;
}

std::span<const uint8_t> byte_span(mq_script_bytes_view view) {
    if (view.size != 0 && !view.data) return {};
    return { view.data ? static_cast<const uint8_t*>(view.data) : &kEmptyByte, view.size };
}

void release_bytes(void*, void* data) {
    std::free(data);
}

mq_script_bytes_result make_result(std::vector<uint8_t>&& value) {
    mq_script_bytes_result result{};
    if (value.empty()) return result;
    result.data = std::malloc(value.size());
    if (!result.data) {
        result.size = value.size();
        return result;
    }
    std::memcpy(result.data, value.data(), value.size());
    result.size = value.size();
    result.release = release_bytes;
    return result;
}

} // namespace

namespace zstd {

bool available() {
#ifdef MQ_API_HAVE_ZSTD
    return true;
#else
    return false;
#endif
}

static std::vector<uint8_t> compress_impl(std::span<const uint8_t> data, int32_t level) {
#ifdef MQ_API_HAVE_ZSTD
    if (!valid_input(data)) return {};
    const int actual_level = level == 0 ? ZSTD_CLEVEL_DEFAULT
                                        : std::clamp((int)level, ZSTD_minCLevel(), ZSTD_maxCLevel());
    const size_t bound = ZSTD_compressBound(data.size());
    if (ZSTD_isError(bound)) return {};
    std::vector<uint8_t> out(bound);
    const size_t used = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), actual_level);
    if (ZSTD_isError(used)) return {};
    out.resize(used);
    return out;
#else
    (void)data;
    (void)level;
    return {};
#endif
}

static std::vector<uint8_t> decompress_impl(std::span<const uint8_t> data) {
#ifdef MQ_API_HAVE_ZSTD
    if (data.empty() || !valid_input(data)) return {};
    const unsigned long long frame_size = ZSTD_getFrameContentSize(data.data(), data.size());
    if (frame_size == ZSTD_CONTENTSIZE_ERROR || frame_size > kMaxBytes) return {};
    if (frame_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        const size_t expected = (size_t)frame_size;
        std::vector<uint8_t> out(std::max<size_t>(expected, 1u));
        const size_t used = ZSTD_decompress(out.data(), expected, data.data(), data.size());
        if (ZSTD_isError(used) || used != expected) return {};
        out.resize(expected);
        return out;
    }

    ZSTD_DStream* stream = ZSTD_createDStream();
    if (!stream) return {};
    size_t state = ZSTD_initDStream(stream);
    if (ZSTD_isError(state)) {
        ZSTD_freeDStream(stream);
        return {};
    }
    ZSTD_inBuffer in{ data.data(), data.size(), 0 };
    std::array<uint8_t, kChunkBytes> chunk{};
    std::vector<uint8_t> out;
    bool ok = false;
    while (in.pos < in.size || state != 0) {
        const size_t before = in.pos;
        ZSTD_outBuffer dst{ chunk.data(), chunk.size(), 0 };
        state = ZSTD_decompressStream(stream, &dst, &in);
        if (ZSTD_isError(state) || out.size() + dst.pos > kMaxBytes) break;
        out.insert(out.end(), chunk.data(), chunk.data() + dst.pos);
        if (state == 0 && in.pos == in.size) {
            ok = true;
            break;
        }
        if (before == in.pos && dst.pos == 0) break;
    }
    ZSTD_freeDStream(stream);
    return ok ? out : std::vector<uint8_t>{};
#else
    (void)data;
    return {};
#endif
}

mq_script_bytes_result compress(mq_script_bytes_view input, int32_t level) {
    if (input.size != 0 && !input.data) return {};
    return make_result(compress_impl(byte_span(input), level));
}

mq_script_bytes_result decompress(mq_script_bytes_view input) {
    if (input.size != 0 && !input.data) return {};
    return make_result(decompress_impl(byte_span(input)));
}

} // namespace zstd

namespace zlib {

bool available() {
#ifdef MQ_API_HAVE_ZLIB
    return true;
#else
    return false;
#endif
}

static std::vector<uint8_t> compress_impl(std::span<const uint8_t> data, int32_t level) {
#ifdef MQ_API_HAVE_ZLIB
    if (!valid_input(data) || data.size() > (size_t)std::numeric_limits<uLong>::max()) return {};
    const int actual_level = level < -1 || level > 9 ? Z_DEFAULT_COMPRESSION : (int)level;
    const uLong source_size = (uLong)data.size();
    uLongf bound = compressBound(source_size);
    std::vector<uint8_t> out((size_t)bound);
    if (compress2(out.data(), &bound, data.data(), source_size, actual_level) != Z_OK) return {};
    out.resize((size_t)bound);
    return out;
#else
    (void)data;
    (void)level;
    return {};
#endif
}

static std::vector<uint8_t> decompress_impl(std::span<const uint8_t> data) {
#ifdef MQ_API_HAVE_ZLIB
    if (data.empty() || !valid_input(data) || data.size() > (size_t)std::numeric_limits<uInt>::max()) return {};
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    stream.avail_in = (uInt)data.size();
    if (inflateInit(&stream) != Z_OK) return {};

    std::vector<uint8_t> out;
    bool ok = false;
    for (;;) {
        if (out.size() == kMaxBytes) break;
        const size_t old_size = out.size();
        const size_t take = std::min(kChunkBytes, kMaxBytes - old_size);
        out.resize(old_size + take);
        stream.next_out = reinterpret_cast<Bytef*>(out.data() + old_size);
        stream.avail_out = (uInt)take;
        const uLong before_in = stream.total_in;
        const uLong before_out = stream.total_out;
        const int rc = inflate(&stream, Z_NO_FLUSH);
        out.resize(old_size + (take - stream.avail_out));
        if (rc == Z_STREAM_END) {
            ok = stream.avail_in == 0;
            break;
        }
        if (rc != Z_OK || (before_in == stream.total_in && before_out == stream.total_out)) break;
    }
    inflateEnd(&stream);
    return ok ? out : std::vector<uint8_t>{};
#else
    (void)data;
    return {};
#endif
}

mq_script_bytes_result compress(mq_script_bytes_view input, int32_t level) {
    if (input.size != 0 && !input.data) return {};
    return make_result(compress_impl(byte_span(input), level));
}

mq_script_bytes_result decompress(mq_script_bytes_view input) {
    if (input.size != 0 && !input.data) return {};
    return make_result(decompress_impl(byte_span(input)));
}

} // namespace zlib
