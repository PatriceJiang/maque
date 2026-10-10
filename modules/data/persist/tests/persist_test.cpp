// 持久化(JSON→反射)单元测试
#include "core/persist/mq_persist.h"
#include <print>
#include "core/reflect/mq_reflect.h"
#include "maque_types.h"

#include <cstdio>
#include <cmath>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

int main() {
    mq_reflect_ctx* C = mq_reflect_create();
    mq_reflect_load_schema(C);
    const mq_type_desc* t = mq_reflect_type_by_name(C, "scn.Scene");
    CHECK(t != nullptr);
    if (!t) return 1;

    const char* json = R"({
      "title": "demo", "script": "ball.js",
      "width": 400, "height": 300, "seed": 7
    })";
    scn::Scene s{};
    CHECK(mq_persist_load_json(C, t, json, std::strlen(json), &s) == 0);
    CHECK(s.width == 400 && s.height == 300);
    CHECK(s.script == "ball.js");
    CHECK(s.title == "demo");
    CHECK(s.seed == 7u);

    // 缺失键保留默认；未知键忽略
    const char* json2 = R"({ "width": 500, "nonsense": 42 })";
    scn::Scene s2{};
    CHECK(mq_persist_load_json(C, t, json2, std::strlen(json2), &s2) == 0);
    CHECK(s2.width == 500);
    CHECK(s2.height == 240);                     // schema 默认
    CHECK(s2.title == "maque scene");            // schema 默认
    CHECK(s2.script.empty());

    // 组件：按 type 反射反序列化（异构组件走 apply_json）
    {
        const mq_type_desc* st = mq_reflect_type_by_name(C, "scn.Sprite");
        CHECK(st != nullptr);
        const char* j = R"({"radius":16,"r":1,"g":0.6,"b":0.2,"texture":"ball.png"})";
        scn::Sprite sp{};
        CHECK(mq_persist_load_json(C, st, j, std::strlen(j), &sp) == 0);
        CHECK(std::fabs(sp.radius - 16.0F) < 1e-4F);
        CHECK(sp.texture == "ball.png");

        mq_json* dom = mq_json_parse(j, std::strlen(j));
        CHECK(dom != nullptr);
        scn::Sprite sp2{};
        CHECK(mq_persist_apply_json(C, st, dom, &sp2) == 0);
        CHECK(std::fabs(sp2.g - 0.6F) < 1e-4F);
        mq_json_free(dom);
    }

    // 非法 JSON
    const char* bad = "{ width: 1 ";
    scn::Scene s3{};
    CHECK(mq_persist_load_json(C, t, bad, std::strlen(bad), &s3) != 0);

    mq_reflect_destroy(C);
    if (g_failures == 0) { std::print("persist_test: all checks passed\n"); return 0; }
    std::print("persist_test: {} failure(s)\n", g_failures);
    return 1;
}
