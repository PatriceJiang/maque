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
    mq_reflect_init();
    const mq_type_desc* t = mq_reflect_type_by_name("scn.Scene");
    CHECK(t != nullptr);
    if (!t) return 1;

    const char* json = R"({
      "title": "demo", "script": "ball.js",
      "width": 400, "height": 300, "seed": 7
    })";
    scn::Scene s{};
    CHECK(mq_persist_load_json(t, json, std::strlen(json), &s) == 0);
    CHECK(s.width == 400 && s.height == 300);
    CHECK(s.script == "ball.js");
    CHECK(s.title == "demo");
    CHECK(s.seed == 7u);

    // 缺失键保留默认；未知键忽略
    const char* json2 = R"({ "width": 500, "nonsense": 42 })";
    scn::Scene s2{};
    CHECK(mq_persist_load_json(t, json2, std::strlen(json2), &s2) == 0);
    CHECK(s2.width == 500);
    CHECK(s2.height == 240);                     // schema 默认
    CHECK(s2.title == "maque scene");            // schema 默认
    CHECK(s2.script.empty());

    // 动态数组 + 嵌套（实体树，反射驱动）
    {
        const char* scene_json = R"({
          "title":"t","width":320,"height":240,
          "entities":[
            { "x":10,"y":20,"radius":16,"r":1,"g":1,"b":1,"texture":"ball.png",
              "children":[ { "x":0,"y":0,"radius":6,"texture":"dot.png" } ] },
            { "x":50,"y":60 }
          ]
        })";
        scn::Scene sc{};
        CHECK(mq_persist_load_json(t, scene_json, std::strlen(scene_json), &sc) == 0);
        CHECK(sc.entities.size() == 2);
        if (sc.entities.size() == 2) {
            CHECK(sc.entities[0].texture == "ball.png");
            CHECK(std::fabs(sc.entities[0].radius - 16.0F) < 1e-4F);
            CHECK(sc.entities[0].children.size() == 1);                 // 嵌套
            CHECK(sc.entities[0].children[0].texture == "dot.png");
            CHECK(std::fabs(sc.entities[1].x - 50.0F) < 1e-4F);
            CHECK(sc.entities[1].children.empty());
        }
    }

    // 非法 JSON
    const char* bad = "{ width: 1 ";
    scn::Scene s3{};
    CHECK(mq_persist_load_json(t, bad, std::strlen(bad), &s3) != 0);

    if (g_failures == 0) { std::print("persist_test: all checks passed\n"); return 0; }
    std::print("persist_test: {} failure(s)\n", g_failures);
    return 1;
}
