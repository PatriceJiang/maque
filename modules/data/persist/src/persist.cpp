#include "core/persist/mq_persist.h"
#include "core/reflect/mq_reflect.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* 极简 JSON 值模型 */
struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
};

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void skip() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool fail() { ok = false; return false; }

    JVal parse() {
        skip();
        JVal v;
        if (p >= end) { fail(); return v; }
        char c = *p;
        if (c == '{') v = parse_obj();
        else if (c == '[') v = parse_arr();
        else if (c == '"') { v.kind = JVal::Str; v.str = parse_str(); }
        else if (c == 't' || c == 'f') v = parse_bool();
        else if (c == 'n') v = parse_null();
        else v = parse_num();
        return v;
    }

    JVal parse_null() {
        JVal v; v.kind = JVal::Null;
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) p += 4; else fail();
        return v;
    }
    JVal parse_bool() {
        JVal v; v.kind = JVal::Bool;
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { v.b = true; p += 4; }
        else if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { v.b = false; p += 5; }
        else fail();
        return v;
    }
    std::string parse_str() {
        std::string s;
        if (p >= end || *p != '"') { fail(); return s; }
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': s.push_back('\n'); break;
                    case 't': s.push_back('\t'); break;
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    default: s.push_back(*p); break;
                }
            } else s.push_back(*p);
            ++p;
        }
        if (p < end) ++p; else fail();
        return s;
    }
    JVal parse_num() {
        JVal v; v.kind = JVal::Num;
        char* endp = nullptr;
        v.num = std::strtod(p, &endp);
        if (endp == p) { fail(); return v; }
        p = endp;
        return v;
    }
    JVal parse_arr() {
        JVal v; v.kind = JVal::Arr;
        ++p; skip();
        if (p < end && *p == ']') { ++p; return v; }
        for (;;) {
            v.arr.push_back(parse());
            if (!ok) return v;
            skip();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; break; }
            fail(); return v;
        }
        return v;
    }
    JVal parse_obj() {
        JVal v; v.kind = JVal::Obj;
        ++p; skip();
        if (p < end && *p == '}') { ++p; return v; }
        for (;;) {
            skip();
            std::string key = parse_str();
            if (!ok) return v;
            skip();
            if (p >= end || *p != ':') { fail(); return v; }
            ++p;
            v.obj.emplace_back(key, parse());
            if (!ok) return v;
            skip();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; break; }
            fail(); return v;
        }
        return v;
    }
};

size_t kind_width(mq_type_kind k) {
    switch (k) {
        case MQ_TK_BOOL: case MQ_TK_I8: case MQ_TK_U8:  return 1;
        case MQ_TK_I16:  case MQ_TK_U16:                return 2;
        case MQ_TK_I32:  case MQ_TK_U32: case MQ_TK_F32: return 4;
        case MQ_TK_I64:  case MQ_TK_U64: case MQ_TK_F64: return 8;
        default: return 0;
    }
}

void write_scalar(void* p, mq_type_kind kind, uint32_t type_id, double num) {
    size_t w = kind_width(kind);
    if (kind == MQ_TK_ENUM) { const mq_type_desc* e = mq_reflect_type_by_id(type_id); w = e ? e->size : 4; }
    if (w == 0) return;
    switch (kind) {
        case MQ_TK_F32: *(float*)p = (float)num; return;
        case MQ_TK_F64: *(double*)p = num; return;
        case MQ_TK_BOOL: *(uint8_t*)p = num != 0.0; return;
        default: break;
    }
    const bool uns = (kind == MQ_TK_U8 || kind == MQ_TK_U16 || kind == MQ_TK_U32 || kind == MQ_TK_U64);
    switch (w) {
        case 1: if (uns) *(uint8_t*)p = (uint8_t)num; else *(int8_t*)p = (int8_t)num; return;
        case 2: if (uns) *(uint16_t*)p = (uint16_t)num; else *(int16_t*)p = (int16_t)num; return;
        case 4: if (uns) *(uint32_t*)p = (uint32_t)num; else *(int32_t*)p = (int32_t)num; return;
        case 8: if (uns) *(uint64_t*)p = (uint64_t)num; else *(int64_t*)p = (int64_t)num; return;
        default: return;
    }
}

void apply_field(const mq_field_desc* f, void* field_ptr, const JVal& v);

void apply_struct(const mq_type_desc* type, void* obj, const JVal& v) {
    if (type->flags & MQ_TYPE_ENUM) {
        if (v.kind == JVal::Num) write_scalar(obj, MQ_TK_ENUM, type->type_id, v.num);
        return;
    }
    if (v.kind != JVal::Obj) return;
    for (auto& kv : v.obj) {
        const mq_field_desc* f = mq_reflect_field(type, kv.first.c_str());
        if (!f) continue;                       /* 忽略未知键 */
        apply_field(f, (uint8_t*)obj + f->offset, kv.second);
    }
}

void apply_field(const mq_field_desc* f, void* fp, const JVal& v) {
    switch (f->kind) {
        case MQ_TK_STRUCT: {
            const mq_type_desc* ft = mq_reflect_type_by_id(f->type_id);
            if (ft) apply_struct(ft, fp, v);
            return;
        }
        case MQ_TK_ENUM:
            if (v.kind == JVal::Num) write_scalar(fp, MQ_TK_ENUM, f->type_id, v.num);
            return;
        case MQ_TK_STRING:
            if (v.kind == JVal::Str) *reinterpret_cast<std::string*>(fp) = v.str;
            return;
        case MQ_TK_ARRAY_FIXED: {
            /* 仅处理"具名 struct 元素"的定长数组（元素类型由 type_id 提供）。 */
            if (v.kind != JVal::Arr) return;
            const mq_type_desc* et = mq_reflect_type_by_id(f->type_id);
            if (!et) return;
            for (uint32_t i = 0; i < f->count && i < v.arr.size(); ++i)
                apply_struct(et, (uint8_t*)fp + (size_t)i * et->size, v.arr[i]);
            return;
        }
        default:
            if (v.kind == JVal::Num)       write_scalar(fp, f->kind, f->type_id, v.num);
            else if (v.kind == JVal::Bool) write_scalar(fp, f->kind, f->type_id, v.b ? 1.0 : 0.0);
            return;
    }
}

} // namespace

extern "C" int mq_persist_load_json(const mq_type_desc* type, const char* json, size_t len, void* out) {
    if (!type || !json || !out) return 1;
    mq_reflect_init();
    Parser parser{ json, json + len };
    JVal root = parser.parse();
    if (!parser.ok || root.kind != JVal::Obj) return 2;
    apply_struct(type, out, root);
    return 0;
}

/* ---- 通用 JSON DOM ---- */
extern "C" mq_json* mq_json_parse(const char* text, size_t len) {
    if (!text) return nullptr;
    Parser parser{ text, text + len };
    auto* root = new JVal(parser.parse());
    if (!parser.ok) { delete root; return nullptr; }
    return reinterpret_cast<mq_json*>(root);
}
extern "C" void mq_json_free(mq_json* doc) { delete reinterpret_cast<JVal*>(doc); }
extern "C" const mq_json* mq_json_get(const mq_json* obj, const char* key) {
    if (!obj || !key) return nullptr;
    const auto* v = reinterpret_cast<const JVal*>(obj);
    if (v->kind != JVal::Obj) return nullptr;
    for (auto& kv : v->obj) if (kv.first == key) return reinterpret_cast<const mq_json*>(&kv.second);
    return nullptr;
}
extern "C" uint32_t mq_json_len(const mq_json* arr) {
    if (!arr) return 0;
    const auto* v = reinterpret_cast<const JVal*>(arr);
    return v->kind == JVal::Arr ? (uint32_t)v->arr.size() : 0u;
}
extern "C" const mq_json* mq_json_at(const mq_json* arr, uint32_t i) {
    if (!arr) return nullptr;
    const auto* v = reinterpret_cast<const JVal*>(arr);
    if (v->kind != JVal::Arr || i >= v->arr.size()) return nullptr;
    return reinterpret_cast<const mq_json*>(&v->arr[i]);
}
extern "C" double mq_json_num(const mq_json* v, double def) {
    if (!v) return def;
    const auto* j = reinterpret_cast<const JVal*>(v);
    if (j->kind == JVal::Num) return j->num;
    if (j->kind == JVal::Bool) return j->b ? 1.0 : 0.0;
    return def;
}
extern "C" const char* mq_json_str(const mq_json* v, const char* def) {
    if (!v) return def;
    const auto* j = reinterpret_cast<const JVal*>(v);
    return j->kind == JVal::Str ? j->str.c_str() : def;
}
