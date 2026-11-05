/* maque::profile 单测：解析器（无需真的 perf 权限）+ 状态接口的构建形态。
 *
 * 覆盖：
 *   ① `parse_report_text` 的标准行：百分比 / Samples / DSO / 模板符号；
 *   ② 没有 Samples 列的旧版 perf 输出；
 *   ③ 同符号聚合、排序、表头/警告/空行/恶意行跳过；
 *   ④ `supported()` 与编译宏一致；未支持时 start/stop 返回可读原因而不崩；
 *   ⑤ `hotspots_json` 是合法 JSON 形态（含状态/路径/hotspots 数组）。
 */
#include "core/profile/mq_profile.h"

#include <cmath>
#include <print>
#include <string>

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fail;                                                            \
            std::print("FAIL {}:{}: {}\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

using namespace maque::profile;

static void test_parse_standard() {
    const std::string text =
        "# Overhead  Samples  Command  Shared Object      Symbol\n"
        "# ........  .......  .......  .................  ......................\n"
        "#\n"
        "   31.25%      125  maque_player  maque_player  [.] maque::rt::Engine::step_frame()\n"
        "   12.00%       48  maque_player  maque_player  [.] maque::rt::Scene::visit()\n"
        "   31.25%      125  maque_player  maque_player  [.] maque::rt::Engine::step_frame()\n"
        "    0.50%        2  maque_player  libc.so.6     [.] memcpy\n"
        "Warning:\n"
        "  corrupted line without symbol\n"
        "\n";
    const auto hs = parse_report_text(text);
    CHECK(hs.size() == 3);
    /* 排序：step_frame 聚合后 62.5% > Scene::visit 12% > memcpy 0.5% */
    CHECK(hs[0].symbol.find("step_frame") != std::string::npos);
    CHECK(std::abs(hs[0].percent - 62.5) < 1e-9);
    CHECK(hs[0].samples == 250);
    CHECK(hs[0].dso == "maque_player");
    CHECK(hs[1].symbol.find("Scene::visit") != std::string::npos);
    CHECK(std::abs(hs[1].percent - 12.0) < 1e-9);
    CHECK(hs[2].dso == "libc.so.6");
}

static void test_parse_no_samples_column() {
    /* 老 perf / 没加 --show-nr-samples：只有 Overhead + Command + DSO + Symbol。 */
    const std::string text =
        "   22.22%  maque_player  maque_player  [.] maque::gfx::Device::render()\n"
        "    3.33%  maque_player  libquickjs.so  [.] js_call\n";
    const auto hs = parse_report_text(text);
    CHECK(hs.size() == 2);
    CHECK(std::abs(hs[0].percent - 22.22) < 1e-9);
    CHECK(hs[0].samples == 0);
    CHECK(hs[0].dso == "maque_player");
    CHECK(hs[1].dso == "libquickjs.so");
}

static void test_parse_weird_symbols() {
    /* C++ 模板/匿名命名空间/带括号的符号：标记之后的整段都是符号。 */
    const std::string text =
        "    9.09%  4  maque_player  maque_player  [.] std::vector<maque::Foo, std::allocator<maque::Foo>>::push_back(maque::Foo const&)\n"
        "    1.00%  1  maque_player  maque_player  [.] (anonymous namespace)::helper(int, char const*)\n";
    const auto hs = parse_report_text(text);
    CHECK(hs.size() == 2);
    CHECK(hs[0].symbol.find("std::vector<") == 0);
    CHECK(hs[0].symbol.find("push_back") != std::string::npos);
    CHECK(hs[0].samples == 4);
    CHECK(hs[1].symbol.find("anonymous namespace") != std::string::npos);
}

static void test_parse_garbage() {
    CHECK(parse_report_text("").empty());
    CHECK(parse_report_text("this is not perf output\n").empty());
    CHECK(parse_report_text("  99.9%  [unknown]\n").empty());   // 缺 DSO / Symbol 标记
}

static void test_api_shape() {
#if MQ_PROFILE_ENABLED && defined(__linux__)
    CHECK(supported());
    CHECK(std::string(support_reason()).empty());
#else
    CHECK(!supported());
    CHECK(!std::string(support_reason()).empty());
#endif
    CHECK(std::string(state_name(State::idle)) == "idle");
    CHECK(std::string(state_name(State::recording)) == "recording");

    if (!supported()) {
        std::string err;
        Options o;
        o.output_dir = "/tmp/maque-profile-should-not-exist";
        CHECK(!start(o, &err));
        CHECK(!err.empty());
        CHECK(!stop(true, &err));
    }
    const std::string js = hotspots_json(3);
    CHECK(js.find("\"backend\":\"perf\"") != std::string::npos);
    CHECK(js.find("\"hotspots\":[") != std::string::npos);
    CHECK(js.find("\"supported\":") != std::string::npos);
}

int main() {
    test_parse_standard();
    test_parse_no_samples_column();
    test_parse_weird_symbols();
    test_parse_garbage();
    test_api_shape();
    std::print("profile: {} checks, {} failed (MQ_PROFILE_ENABLED={} supported={})\n",
               g_checks, g_fail, MQ_PROFILE_ENABLED, supported());
    return g_fail == 0 ? 0 : 1;
}
