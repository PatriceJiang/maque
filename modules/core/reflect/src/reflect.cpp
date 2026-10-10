#include "core/reflect/mq_reflect.h"

#include <vector>
#include <string>
#include <unordered_map>

namespace {

std::vector<const mq_type_desc*>&           reg_list()    { static std::vector<const mq_type_desc*> v; return v; }
std::unordered_map<uint32_t, const mq_type_desc*>&  reg_by_id()   { static std::unordered_map<uint32_t, const mq_type_desc*> m; return m; }
std::unordered_map<std::string, const mq_type_desc*>& reg_by_name() { static std::unordered_map<std::string, const mq_type_desc*> m; return m; }
bool& inited() { static bool b = false; return b; }

size_t scalar_size(mq_type_kind k) {
    switch (k) {
        case MQ_TK_BOOL: case MQ_TK_I8: case MQ_TK_U8:  return 1;
        case MQ_TK_I16:  case MQ_TK_U16:                return 2;
        case MQ_TK_I32:  case MQ_TK_U32: case MQ_TK_F32: return 4;
        case MQ_TK_I64:  case MQ_TK_U64: case MQ_TK_F64: return 8;
        default: return 0;
    }
}

/* 解析字段的"标量宽度"：数值/布尔/枚举返回字节数，其它返回 0。 */
size_t field_scalar_size(const mq_field_desc* f) {
    size_t s = scalar_size(f->kind);
    if (s) return s;
    if (f->kind == MQ_TK_ENUM) {
        const mq_type_desc* e = mq_reflect_type_by_id(f->type_id);
        return e && e->size ? e->size : 4;
    }
    return 0;
}

/* 以给定宽度把指针处读作有符号/无符号整数或浮点，转 double。 */
double load_as_double(const uint8_t* p, mq_type_kind kind, size_t width) {
    if (kind == MQ_TK_F32) { float v; __builtin_memcpy(&v, p, 4); return v; }
    if (kind == MQ_TK_F64) { double v; __builtin_memcpy(&v, p, 8); return v; }
    if (kind == MQ_TK_BOOL) return *p ? 1.0 : 0.0;
    /* 整数 / 枚举：按宽度与 kind 解释 */
    const bool uns = (kind == MQ_TK_U8 || kind == MQ_TK_U16 || kind == MQ_TK_U32 || kind == MQ_TK_U64);
    switch (width) {
        case 1: return uns ? (double)*(const uint8_t*)p : (double)*(const int8_t*)p;
        case 2: return uns ? (double)*(const uint16_t*)p : (double)*(const int16_t*)p;
        case 4: return uns ? (double)*(const uint32_t*)p : (double)*(const int32_t*)p;
        case 8: return uns ? (double)*(const uint64_t*)p : (double)*(const int64_t*)p;
        default: return 0.0;
    }
}

bool store_from_double(uint8_t* p, mq_type_kind kind, size_t width, double v) {
    if (kind == MQ_TK_F32) { float f = (float)v; __builtin_memcpy(p, &f, 4); return true; }
    if (kind == MQ_TK_F64) { __builtin_memcpy(p, &v, 8); return true; }
    if (kind == MQ_TK_BOOL) { *p = v != 0.0 ? 1 : 0; return true; }
    const bool uns = (kind == MQ_TK_U8 || kind == MQ_TK_U16 || kind == MQ_TK_U32 || kind == MQ_TK_U64);
    switch (width) {
        case 1: { if (uns) *(uint8_t*)p = (uint8_t)v; else *(int8_t*)p = (int8_t)v; return true; }
        case 2: { if (uns) *(uint16_t*)p = (uint16_t)v; else *(int16_t*)p = (int16_t)v; return true; }
        case 4: { if (uns) *(uint32_t*)p = (uint32_t)v; else *(int32_t*)p = (int32_t)v; return true; }
        case 8: { if (uns) *(uint64_t*)p = (uint64_t)v; else *(int64_t*)p = (int64_t)v; return true; }
        default: return false;
    }
}

} // namespace

extern "C" {

int mq_reflect_init(void) {
    if (inited()) return 0;
    for (uint32_t i = 0; i < mq_schema_type_count; ++i)
        mq_reflect_register(mq_schema_types[i]);
    inited() = true;
    return 0;
}

int mq_reflect_register(const mq_type_desc* type) {
    if (!type) return 1;
    auto it = reg_by_id().find(type->type_id);
    if (it != reg_by_id().end()) return it->second == type ? 0 : 1;
    reg_by_id()[type->type_id] = type;
    if (type->name) reg_by_name()[type->name] = type;
    reg_list().push_back(type);
    return 0;
}

const mq_type_desc* mq_reflect_type_by_id(uint32_t type_id) {
    mq_reflect_init();
    auto it = reg_by_id().find(type_id);
    return it == reg_by_id().end() ? nullptr : it->second;
}

const mq_type_desc* mq_reflect_type_by_name(const char* fq_name) {
    if (!fq_name) return nullptr;
    mq_reflect_init();
    auto it = reg_by_name().find(fq_name);
    return it == reg_by_name().end() ? nullptr : it->second;
}

const mq_field_desc* mq_reflect_field(const mq_type_desc* type, const char* field_name) {
    if (!type || !field_name) return nullptr;
    for (uint32_t i = 0; i < type->field_count; ++i)
        if (type->fields[i].name && __builtin_strcmp(type->fields[i].name, field_name) == 0)
            return &type->fields[i];
    return nullptr;
}

const mq_field_desc* mq_reflect_field_path(const mq_type_desc* type, const char* path,
                                           uint32_t* out_absolute_offset) {
    if (!type || !path) return nullptr;
    uint32_t acc = 0;
    const char* seg = path;
    while (*seg) {
        const char* dot = seg;
        while (*dot && *dot != '.') ++dot;
        std::string name(seg, (size_t)(dot - seg));
        const mq_field_desc* f = mq_reflect_field(type, name.c_str());
        if (!f) return nullptr;
        acc += f->offset;
        if (*dot == '\0') {
            if (out_absolute_offset) *out_absolute_offset = acc;
            return f;
        }
        /* 继续下钻：字段必须是具名 struct */
        if (f->kind != MQ_TK_STRUCT) return nullptr;
        const mq_type_desc* ft = mq_reflect_type_by_id(f->type_id);
        if (!ft) return nullptr;
        type = ft;
        seg = dot + 1;
    }
    return nullptr;
}

int mq_reflect_get_f64(const void* obj, const mq_field_desc* field, double* out) {
    if (!obj || !field || !out) return 1;
    const size_t w = field_scalar_size(field);
    if (!w) return 1;
    *out = load_as_double((const uint8_t*)obj + field->offset, field->kind, w);
    return 0;
}

int mq_reflect_set_f64(void* obj, const mq_field_desc* field, double value) {
    if (!obj || !field) return 1;
    const size_t w = field_scalar_size(field);
    if (!w) return 1;
    return store_from_double((uint8_t*)obj + field->offset, field->kind, w, value) ? 0 : 1;
}

int mq_reflect_get_i64(const void* obj, const mq_field_desc* field, int64_t* out) {
    double d;
    if (mq_reflect_get_f64(obj, field, &d)) return 1;
    *out = (int64_t)d;
    return 0;
}

int mq_reflect_set_i64(void* obj, const mq_field_desc* field, int64_t value) {
    return mq_reflect_set_f64(obj, field, (double)value);
}

int mq_reflect_visit(void* obj, const mq_type_desc* type, const mq_visitor* visitor) {
    if (!type || !visitor) return 1;
    if (visitor->on_enter && visitor->on_enter(visitor->user, type, obj)) return 1;
    for (uint32_t i = 0; i < type->field_count; ++i) {
        const mq_field_desc* f = &type->fields[i];
        uint8_t* fp = (uint8_t*)obj + f->offset;
        if (f->kind == MQ_TK_STRUCT) {
            const mq_type_desc* ft = mq_reflect_type_by_id(f->type_id);
            if (ft) { if (mq_reflect_visit(fp, ft, visitor)) return 1; continue; }
        } else if (f->kind == MQ_TK_ARRAY_FIXED) {
            const mq_type_desc* et = mq_reflect_type_by_id(f->type_id);
            if (et && f->count) {
                for (uint32_t j = 0; j < f->count; ++j)
                    if (mq_reflect_visit(fp + (size_t)j * et->size, et, visitor)) return 1;
                continue;
            }
        }
        if (visitor->on_field && visitor->on_field(visitor->user, f, fp)) return 1;
    }
    if (visitor->on_exit && visitor->on_exit(visitor->user, type, obj)) return 1;
    return 0;
}

} // extern "C"
