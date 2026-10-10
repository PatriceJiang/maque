// maque 反射运行时单元测试（独立：仅依赖 maque::reflect + 生成的类型/描述符）
#include "core/reflect/mq_reflect.h"
#include <print>
#include "maque_types.h"

#include <cstdio>
#include <cstddef>
#include <cstdint>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::print("FAIL {}:{}  {}\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static int count_enter = 0, count_exit = 0, count_field = 0;
static int on_enter(void*, const mq_type_desc*, void*) { ++count_enter; return 0; }
static int on_exit (void*, const mq_type_desc*, void*) { ++count_exit;  return 0; }
static int on_field(void*, const mq_field_desc*, const void*) { ++count_field; return 0; }

int main() {
    CHECK(mq_reflect_init() == 0);

    // 1. 按名查询 + 布局
    const mq_type_desc* vec3 = mq_reflect_type_by_name("math.Vec3");
    CHECK(vec3 != nullptr);
    CHECK(vec3 && vec3->size == 12);
    CHECK(vec3 && (vec3->flags & MQ_TYPE_POD));

    // 2. 按 id 查询（stable type_id）
    CHECK(vec3 && mq_reflect_type_by_id(vec3->type_id) == vec3);

    // 3. 字段查找 + 偏移
    const mq_field_desc* fx = mq_reflect_field(vec3, "x");
    CHECK(fx != nullptr);
    CHECK(fx && fx->offset == offsetof(math::Vec3, x));

    // 4. 标量读写
    math::Vec3 v{};
    CHECK(mq_reflect_set_f64(&v, fx, 1.5) == 0);
    CHECK(v.x == 1.5f);
    double back = 0; CHECK(mq_reflect_get_f64(&v, fx, &back) == 0);
    CHECK(back == 1.5);

    // 5. 字段路径（跨 struct）
    const mq_type_desc* t3 = mq_reflect_type_by_name("scene.Transform3D");
    CHECK(t3 != nullptr);
    uint32_t abs = 0;
    const mq_field_desc* px = mq_reflect_field_path(t3, "local.position.x", &abs);
    CHECK(px != nullptr && px->kind == MQ_TK_F32);
    const uint32_t expect = (uint32_t)(offsetof(scene::Transform3D, local)
                                     + offsetof(math::Transform3D, position)
                                     + offsetof(math::Vec3, x));
    CHECK(abs == expect);
    scene::Transform3D st{};
    CHECK(mq_reflect_set_f64(&st, px, 42.0) == 0);
    CHECK(st.local.position.x == 42.0f);

    // 6. transient 属性
    const mq_field_desc* fworld = mq_reflect_field(t3, "world");
    CHECK(fworld != nullptr && (fworld->attrs_flags & MQ_ATTR_TRANSIENT));

    // 7. 枚举读写
    const mq_type_desc* light = mq_reflect_type_by_name("render.Light");
    CHECK(light != nullptr);
    const mq_field_desc* ftype = mq_reflect_field(light, "type");
    CHECK(ftype != nullptr && ftype->kind == MQ_TK_ENUM);
    render::Light L{};
    CHECK(mq_reflect_set_i64(&L, ftype, (int64_t)render::LightType::Spot) == 0);
    CHECK(L.type == render::LightType::Spot);

    // 8. visit：math::Transform3D = 根 + 3×Vec3(各 3 float) → enter=4, field=9
    count_enter = count_exit = count_field = 0;
    mq_visitor vis{}; vis.on_enter = on_enter; vis.on_exit = on_exit; vis.on_field = on_field;
    const mq_type_desc* mt3 = mq_reflect_type_by_name("math.Transform3D");
    math::Transform3D m{};
    CHECK(mq_reflect_visit(&m, mt3, &vis) == 0);
    CHECK(count_enter == 4 && count_exit == 4 && count_field == 9);

    // 9. 动态注册（模拟模块注册自有类型）
    struct MyComp { float hp; };
    static const mq_field_desc my_fields[] = {
        { "hp", offsetof(MyComp, hp), MQ_TK_F32, 0, 0, 0 }
    };
    static const mq_type_desc my_type = {
        "game.Health", 0x1234ABCDu, (uint32_t)sizeof(MyComp), 4,
        1, my_fields, MQ_TYPE_POD
    };
    CHECK(mq_reflect_register(&my_type) == 0);
    CHECK(mq_reflect_type_by_id(0x1234ABCDu) == &my_type);
    CHECK(mq_reflect_type_by_name("game.Health") == &my_type);
    CHECK(mq_reflect_register(&my_type) == 0);  // 幂等

    if (g_failures == 0) { std::print("reflect_test: all checks passed\n"); return 0; }
    std::print("reflect_test: {} failure(s)\n", g_failures);
    return 1;
}
