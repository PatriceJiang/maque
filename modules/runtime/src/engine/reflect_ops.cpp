/* 反射数值读写（docs/06/18）：按字段名读/写标量 + 滑块指针映射
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

float read_num(const mq_type_desc* d, const void* base, const char* field, float def) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (!f) return def;
    /* 标量走 offset 直读：accessor 是不可内联的间接调用（每字段约 4ns），逐帧/逐实体调用是纯开销。
     * 枚举等特殊宽度语义仍回退 accessor / 原分支。 */
    const uint8_t* p = (const uint8_t*)base + f->offset;
    switch (f->kind) {
        case MQ_TK_F32:  return *(const float*)p;
        case MQ_TK_F64:  return (float)*(const double*)p;
        case MQ_TK_I8:   return (float)*(const int8_t*)p;
        case MQ_TK_U8:   return (float)*(const uint8_t*)p;
        case MQ_TK_I16:  return (float)*(const int16_t*)p;
        case MQ_TK_U16:  return (float)*(const uint16_t*)p;
        case MQ_TK_I32:  return (float)*(const int32_t*)p;
        case MQ_TK_U32:  return (float)*(const uint32_t*)p;
        case MQ_TK_I64:  return (float)*(const int64_t*)p;
        case MQ_TK_U64:  return (float)*(const uint64_t*)p;
        case MQ_TK_BOOL: return *p ? 1.0F : 0.0F;
        default: break;
    }
    if (f->accessor && f->accessor->get_f64) return (float)f->accessor->get_f64(base);
    if (f->kind == MQ_TK_ENUM) {
        /* 枚举：按底层整型宽度读（BodyType::Dynamic 等，docs/06） */
        if (f->count == 1) return (float)*(const int8_t*)p;
        if (f->count == 2) return (float)*(const int16_t*)p;
        return (float)*(const int32_t*)p;
    }
    return def;
}

bool write_num(const mq_type_desc* d, void* base, const char* field, double v) {
    const mq_field_desc* f = d ? mq_reflect_field(d, field) : nullptr;
    if (!f) return false;
    uint8_t* p = (uint8_t*)base + f->offset;
    switch (f->kind) {
        case MQ_TK_F32:  *(float*)p = (float)v; return true;
        case MQ_TK_F64:  *(double*)p = v; return true;
        case MQ_TK_I8:   *(int8_t*)p = (int8_t)v; return true;
        case MQ_TK_U8:   *(uint8_t*)p = (uint8_t)v; return true;
        case MQ_TK_I16:  *(int16_t*)p = (int16_t)v; return true;
        case MQ_TK_U16:  *(uint16_t*)p = (uint16_t)v; return true;
        case MQ_TK_I32:  *(int32_t*)p = (int32_t)v; return true;
        case MQ_TK_U32:  *(uint32_t*)p = (uint32_t)v; return true;
        case MQ_TK_I64:  *(int64_t*)p = (int64_t)v; return true;
        case MQ_TK_U64:  *(uint64_t*)p = (uint64_t)v; return true;
        case MQ_TK_BOOL: *(bool*)p = v != 0.0; return true;
        default: break;
    }
    if (f->accessor && f->accessor->set_f64) { f->accessor->set_f64(base, v); return true; }
    if (f->kind == MQ_TK_ENUM) {
        if (f->count == 1) { *(int8_t*)p = (int8_t)v; return true; }
        if (f->count == 2) { *(int16_t*)p = (int16_t)v; return true; }
        *(int32_t*)p = (int32_t)v; return true;
    }
    return false;
}

/* Slider：把指针 x 映射为值（Slider 的 progress 语义）。 */
void slider_apply_pointer(Engine& eng, int id, const mq_type_desc* d, void* base, float px) {
    SceneNode* e = eng.scene->at(id);
    if (!e) return;
    auto* sl = as_component_mut<ui::Slider>(d, base);
    if (!sl) return;
    float w = 100.0F;
    if (auto* c = component<ui::Control>(*e)) w = c->offset_max.x;
    if (w <= 0.0F) w = 1.0F;
    float t = (px - (e->world_x - w * 0.5F)) / w;
    t = t < 0.0F ? 0.0F : (t > 1.0F ? 1.0F : t);
    if (sl->invert) t = 1.0F - t;
    const float lo = sl->min_value;
    const float hi = sl->max_value;
    float v = lo + (hi - lo) * t;
    const float step = sl->step;
    if (step > 0.0F) v = lo + std::round((v - lo) / step) * step;
    sl->value = v;
    sl->ratio = t;
    host::node_emit_number(&eng, id, "changed", v);
}


/* 文本：装配期选字体
 *   MAQUE_FONT → **项目字体（scenes/fonts/ 下首个 .ttf，仓库自带含中文）** → 常见系统字体
 *   → 内置 5x7 位图字体。
 * AGENTS.md 一直这么写，但实现里缺了"项目字体"这一档：只查 MAQUE_FONT 与 Linux 系统路径，
 * 于是 Windows（没有 /usr/share/fonts）**恒回退内置 5x7**，中文与 CJK 全丢，
 * 且依赖真实字宽的排版用例（text_e2e 的折行行数）必然失败。
 * 图集纹理按字号惰性创建并缓存（确定性：同字号同图集，docs/10）。 */

/* ---- 反射读取辅助（不依赖具体类型，仅按字段名） ---- */
bool read_f32(const mq_type_desc* d, const void* blob, const char* name, float* out) {
    const mq_field_desc* f = d ? mq_reflect_field(d, name) : nullptr;
    if (!f || f->kind != MQ_TK_F32) return false;
    *out = *(const float*)((const uint8_t*)blob + f->offset);
    return true;
}
const std::string* read_str(const mq_type_desc* d, const void* blob, const char* name) {
    const mq_field_desc* f = d ? mq_reflect_field(d, name) : nullptr;
    if (!f || f->kind != MQ_TK_STRING) return nullptr;
    return (const std::string*)((const uint8_t*)blob + f->offset);
}

bool write_ppm(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::print(f, "P6\n{} {}\n255\n", w, h);
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        rgb[i * 3] = rgba[i * 4]; rgb[i * 3 + 1] = rgba[i * 4 + 1]; rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    return true;
}

}  // namespace maque::rt
