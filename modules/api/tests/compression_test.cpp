#include "maque_api.h"

#include <cstdint>
#include <cstring>
#include <print>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void release(mq_script_bytes_result& value) {
    if (value.release) value.release(value.release_user, value.data);
    value = {};
}

static bool equal(const mq_script_bytes_result& value, const std::vector<uint8_t>& expected) {
    return value.size == expected.size() &&
           (expected.empty() || (value.data && std::memcmp(value.data, expected.data(), expected.size()) == 0));
}

int main() {
    if (!zstd::available() || !zlib::available()) {
        std::print("compression_test: skipped (zstd/zlib unavailable)\n");
        return 0;
    }

    std::vector<uint8_t> raw(8192);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = i % 31 == 0 ? 0 : (uint8_t)('A' + i % 7);
    raw[7] = 0xff;
    const mq_script_bytes_view input{ raw.data(), raw.size() };

    auto zs = zstd::compress(input, 3);
    CHECK(zs.data != nullptr && zs.size > 0 && zs.size < raw.size());
    auto zr = zstd::decompress({ zs.data, zs.size });
    CHECK(equal(zr, raw));

    auto zl = zlib::compress(input, 6);
    CHECK(zl.data != nullptr && zl.size > 0 && zl.size < raw.size());
    auto zlr = zlib::decompress({ zl.data, zl.size });
    CHECK(equal(zlr, raw));

    const uint8_t bad[] = { 0, 1, 2, 3, 4 };
    auto bad_zstd = zstd::decompress({ bad, sizeof bad });
    auto bad_zlib = zlib::decompress({ bad, sizeof bad });
    CHECK(bad_zstd.data == nullptr && bad_zstd.size == 0);
    CHECK(bad_zlib.data == nullptr && bad_zlib.size == 0);

    release(zs);
    release(zr);
    release(zl);
    release(zlr);
    release(bad_zstd);
    release(bad_zlib);

    if (g_failures == 0) {
        std::print("compression_test: all checks passed\n");
        return 0;
    }
    std::print("compression_test: {} failure(s)\n", g_failures);
    return 1;
}
