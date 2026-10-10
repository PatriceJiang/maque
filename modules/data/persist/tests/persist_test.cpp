// 持久化(JSON→反射)单元测试
#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"
#include "maque_types.h"

#include <cstdio>
#include <cmath>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    mq_reflect_init();
    const mq_type_desc* t = mq_reflect_type_by_name("scn.BallScene");
    CHECK(t != nullptr);
    if (!t) return 1;

    const char* json = R"({
      "width": 400, "height": 300,
      "gravity": 1000.5, "restitution": 0.4, "radius": 10,
      "remove_delay": 3,
      "click": { "x": 123, "y": 45 }
    })";
    scn::BallScene s{};
    CHECK(mq_persist_load_json(t, json, std::strlen(json), &s) == 0);
    CHECK(s.width == 400 && s.height == 300);
    CHECK(std::fabs(s.gravity - 1000.5F) < 1e-3F);
    CHECK(std::fabs(s.restitution - 0.4F) < 1e-4F);
    CHECK(std::fabs(s.radius - 10.0F) < 1e-4F);
    CHECK(std::fabs(s.remove_delay - 3.0F) < 1e-4F);
    CHECK(std::fabs(s.click.x - 123.0F) < 1e-4F && std::fabs(s.click.y - 45.0F) < 1e-4F);

    // 缺失键保留默认；未知键忽略
    const char* json2 = R"({ "gravity": 5, "nonsense": 42 })";
    scn::BallScene s2{};
    CHECK(mq_persist_load_json(t, json2, std::strlen(json2), &s2) == 0);
    CHECK(std::fabs(s2.gravity - 5.0F) < 1e-4F);
    CHECK(std::fabs(s2.restitution - 0.6F) < 1e-4F);   // schema 默认
    CHECK(s2.width == 320);                            // schema 默认

    // 非法 JSON
    const char* bad = "{ width: 1 ";
    scn::BallScene s3{};
    CHECK(mq_persist_load_json(t, bad, std::strlen(bad), &s3) != 0);

    if (g_failures == 0) { std::printf("persist_test: all checks passed\n"); return 0; }
    std::printf("persist_test: %d failure(s)\n", g_failures);
    return 1;
}
