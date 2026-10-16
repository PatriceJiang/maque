// 内建 shader 打包单测（assets/builtin/shaders → 配置期流水线 → 压缩源码）：
//   ① 注册表非空、条目字段自洽（名字/来源/原始大小/取源码函数）
//   ② 解压结果 = 原始 WGSL 文本：非空、含 @vertex/@fragment、strlen == raw_size
//   ③ 解压有缓存：重复调用返回同一指针（零开销）
//   ④ 未编入的名字 → NULL（剔除开关 MAQUE_SHADER_<NAME> 只影响注册表内容）
//   ⑤ **资产 ↔ 打包逐字节一致**（漂移检查）：拿磁盘上的 *.wgsl 重新拼接，与解压文本比对
#include "builtin_shaders.h"

#include <print>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef MQ_SHADER_ASSETS_DIR
#error "需要 MQ_SHADER_ASSETS_DIR（测试编译定义指向 assets/builtin/shaders）"
#endif
#ifndef MQ_SHADER_EXPECT_COUNT
#error "需要 MQ_SHADER_EXPECT_COUNT（CMake 传入本次编入的 shader 数，与 MAQUE_SHADER_* 开关一致）"
#endif

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

/* 与 tools/shader_pack.js 同规则：目录内 *.wgsl 按文件名排序拼接 */
static std::string read_asset(const std::string& name) {
    const fs::path dir = fs::path(MQ_SHADER_ASSETS_DIR) / name;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".wgsl") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::string out;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::ostringstream ss; ss << in.rdbuf();
        out += ss.str();
    }
    return out;
}

int main() {
    // 注册表内容必须**恰好**等于 CMake 的启用开关（默认全部编入；MAQUE_SHADER_<NAME>=OFF 即剔除）
    CHECK(mq_builtin_shader_count == MQ_SHADER_EXPECT_COUNT);
    CHECK(mq_builtin_shaders != nullptr);

    for (unsigned int i = 0; i < mq_builtin_shader_count; ++i) {
        const mq_builtin_shader& s = mq_builtin_shaders[i];
        CHECK(s.name && *s.name);
        CHECK(s.source && *s.source);
        CHECK(s.raw_size > 0);
        CHECK(s.text != nullptr);
        const char* text = s.text ? s.text() : nullptr;
        CHECK(text != nullptr);
        if (!text) continue;
        CHECK(std::strlen(text) == s.raw_size);                // '\0' 结尾且长度等于原始字节数
        CHECK(std::strstr(text, "@vertex") != nullptr);
        CHECK(std::strstr(text, "@fragment") != nullptr);
        CHECK(s.text() == text);                               // 缓存：同一指针
        CHECK(mq_builtin_shader_text(s.name) == text);         // 按名字查表 → 同一条目
    }

    CHECK(mq_builtin_shader_text("nope") == nullptr);          // 未编入/不存在
    CHECK(mq_builtin_shader_text(nullptr) == nullptr);

    // 资产 ↔ 打包逐字节一致（改资产忘了走流水线/拼接规则不一致时立即失败）
    for (unsigned int i = 0; i < mq_builtin_shader_count; ++i) {
        const mq_builtin_shader& s = mq_builtin_shaders[i];
        const std::string asset = read_asset(s.name);
        CHECK(!asset.empty());
        if (asset.empty()) continue;
        const char* text = s.text();
        CHECK(text != nullptr && asset == text);
        if (text && asset != text) {
            std::print("  shader '{}': 资产 {} 字节 vs 打包 {} 字节\n", s.name, asset.size(), std::strlen(text));
        }
    }

    if (g_failures) { std::print("builtin_shader_test: {} failure(s)\n", g_failures); return 1; }
    std::print("builtin_shader_test: ok（{} 个内建 shader）\n", mq_builtin_shader_count);
    return 0;
}
