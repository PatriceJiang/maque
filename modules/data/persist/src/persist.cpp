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
        /* 引用：固定布局 POD（EntityRef{u32,u32} / AssetRef{u64}）→ 8 字节（docs/06 §3） */
        case MQ_TK_ENTITY_REF:
        case MQ_TK_ASSET_REF: return 8;
        default: return 0;
    }
}

void write_scalar(mq_reflect_ctx* ctx, void* p, mq_type_kind kind, uint32_t type_id, double num) {
    size_t w = kind_width(kind);
    if (kind == MQ_TK_ENUM) { const mq_type_desc* e = mq_reflect_type_by_id(ctx, type_id); w = e ? e->size : 4; }
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

void apply_field(mq_reflect_ctx* ctx, const mq_field_desc* f, void* field_ptr, const JVal& v);

void apply_struct(mq_reflect_ctx* ctx, const mq_type_desc* type, void* obj, const JVal& v) {
    if (type->flags & MQ_TYPE_ENUM) {
        if (v.kind == JVal::Num) write_scalar(ctx, obj, MQ_TK_ENUM, type->type_id, v.num);
        return;
    }
    if (v.kind != JVal::Obj) return;
    for (auto& kv : v.obj) {
        const mq_field_desc* f = mq_reflect_field(type, kv.first.c_str());
        if (!f) continue;                       /* 忽略未知键 */
        apply_field(ctx, f, (uint8_t*)obj + f->offset, kv.second);
    }
}

void apply_ref(mq_reflect_ctx* ctx, const mq_field_desc* f, void* fp, const JVal& v);

void apply_field(mq_reflect_ctx* ctx, const mq_field_desc* f, void* fp, const JVal& v) {
    switch (f->kind) {
        case MQ_TK_ASSET_REF:
        case MQ_TK_ENTITY_REF:
            apply_ref(ctx, f, fp, v);
            return;
        case MQ_TK_STRUCT: {
            const mq_type_desc* ft = mq_reflect_type_by_id(ctx, f->type_id);
            if (ft) apply_struct(ctx, ft, fp, v);
            return;
        }
        case MQ_TK_ENUM:
            if (v.kind == JVal::Num) write_scalar(ctx, fp, MQ_TK_ENUM, f->type_id, v.num);
            return;
        case MQ_TK_STRING:
            if (v.kind == JVal::Str) *reinterpret_cast<std::string*>(fp) = v.str;
            return;
        case MQ_TK_ARRAY_FIXED: {
            /* 仅处理"具名 struct 元素"的定长数组（元素类型由 type_id 提供）。 */
            if (v.kind != JVal::Arr) return;
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            if (!et) return;
            for (uint32_t i = 0; i < f->count && i < v.arr.size(); ++i)
                apply_struct(ctx, et, (uint8_t*)fp + (size_t)i * et->size, v.arr[i]);
            return;
        }
        case MQ_TK_ARRAY_DYN: {
            /* 动态数组：经容器算子（codegen 生成）类型化地 resize + 逐元素反序列化。 */
            if (v.kind != JVal::Arr || !f->container_resize || !f->container_elem) return;
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->elem_type_id);
            if (!et) return;
            f->container_resize(fp, (uint32_t)v.arr.size());
            for (uint32_t i = 0; i < (uint32_t)v.arr.size(); ++i)
                apply_struct(ctx, et, f->container_elem(fp, i), v.arr[i]);
            return;
        }
        default:
            if (v.kind == JVal::Num)       write_scalar(ctx, fp, f->kind, f->type_id, v.num);
            else if (v.kind == JVal::Bool) write_scalar(ctx, fp, f->kind, f->type_id, v.b ? 1.0 : 0.0);
            return;
    }
}

/* ---- 反射 → JSON 文本（与 load_json 往返一致；确定性顺序） ---- */
size_t kind_width(mq_type_kind k);

void write_json_scalar(std::string& out, const void* p, mq_type_kind kind) {
    switch (kind) {
        case MQ_TK_BOOL: out += (*(const uint8_t*)p) ? "true" : "false"; return;
        case MQ_TK_F32: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.9g", (double)*(const float*)p);
            out += buf;
            return;
        }
        case MQ_TK_F64: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", *(const double*)p);
            out += buf;
            return;
        }
        case MQ_TK_I8:  out += std::to_string((int)*(const int8_t*)p); return;
        case MQ_TK_U8:  out += std::to_string((unsigned)*(const uint8_t*)p); return;
        case MQ_TK_I16: out += std::to_string(*(const int16_t*)p); return;
        case MQ_TK_U16: out += std::to_string(*(const uint16_t*)p); return;
        case MQ_TK_I32: out += std::to_string(*(const int32_t*)p); return;
        case MQ_TK_U32: out += std::to_string(*(const uint32_t*)p); return;
        case MQ_TK_I64: out += std::to_string(*(const int64_t*)p); return;
        case MQ_TK_U64: out += std::to_string(*(const uint64_t*)p); return;
        /* 引用（{u32,u32} / {u64}）：输出 **十六进制字符串** —— 64 位内容哈希超出 double 精度
         * （2^53），走数值会在文本往返时丢位（docs/06 §3 稳定 ID）。 */
        case MQ_TK_ENTITY_REF:
        case MQ_TK_ASSET_REF: {
            uint64_t v = 0;
            std::memcpy(&v, p, 8);
            char hex[32];
            std::snprintf(hex, sizeof hex, "\"0x%016llx\"", (unsigned long long)v);
            out += hex;
            return;
        }
        default: out += "0"; return;
    }
}

void write_json_string(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)c);
                    out += buf;
                } else out += c;
        }
    }
    out += '"';
}

void write_json_value(mq_reflect_ctx* ctx, const mq_type_desc* t, const void* obj, std::string& out);

void write_json_field(mq_reflect_ctx* ctx, const mq_field_desc* f, const void* fp, std::string& out) {
    switch (f->kind) {
        case MQ_TK_STRUCT: {
            const mq_type_desc* ft = mq_reflect_type_by_id(ctx, f->type_id);
            if (ft) write_json_value(ctx, ft, fp, out); else out += "null";
            return;
        }
        case MQ_TK_ENUM: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            const size_t w = kind_width(f->kind);
            (void)w;
            if (et) write_json_scalar(out, fp, et->size == 8 ? MQ_TK_I64 : MQ_TK_I32);
            else out += "0";
            return;
        }
        case MQ_TK_STRING: write_json_string(out, *reinterpret_cast<const std::string*>(fp)); return;
        case MQ_TK_ARRAY_FIXED: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            out += '[';
            for (uint32_t i = 0; i < f->count; ++i) {
                if (i) out += ',';
                if (et) write_json_value(ctx, et, (const uint8_t*)fp + (size_t)i * et->size, out);
                else write_json_scalar(out, (const uint8_t*)fp + (size_t)i * kind_width(f->kind), f->kind);
            }
            out += ']';
            return;
        }
        case MQ_TK_ARRAY_DYN: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->elem_type_id);
            out += '[';
            if (et && f->container_size && f->container_elem) {
                const uint32_t n = f->container_size(fp);
                for (uint32_t i = 0; i < n; ++i) {
                    if (i) out += ',';
                    write_json_value(ctx, et, f->container_elem(const_cast<void*>(fp), i), out);
                }
            }
            out += ']';
            return;
        }
        default: write_json_scalar(out, fp, f->kind); return;
    }
}

void write_json_value(mq_reflect_ctx* ctx, const mq_type_desc* t, const void* obj, std::string& out) {
    if (t->flags & MQ_TYPE_ENUM) { write_json_scalar(out, obj, t->size == 8 ? MQ_TK_I64 : MQ_TK_I32); return; }
    out += '{';
    bool first = true;
    for (uint32_t i = 0; i < t->field_count; ++i) {
        const mq_field_desc* f = &t->fields[i];
        if (f->attrs_flags & MQ_ATTR_TRANSIENT) continue;      // 瞬态不落盘
        if (!first) out += ',';
        first = false;
        write_json_string(out, f->name ? f->name : "");
        out += ':';
        write_json_field(ctx, f, (const uint8_t*)obj + f->offset, out);
    }
    out += '}';
}

/* 引用赋值：字符串 "0x…" 优先（保真），数值回退兼容（≤2^53 的旧文本）。 */
void apply_ref(mq_reflect_ctx* ctx, const mq_field_desc* f, void* fp, const JVal& v) {
    (void)ctx; (void)f;
    uint64_t out = 0;
    if (v.kind == JVal::Str) {
        const char* q = v.str.c_str();
        if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) q += 2;
        out = std::strtoull(q, nullptr, 16);
    } else if (v.kind == JVal::Num) {
        out = (uint64_t)v.num;
    } else {
        return;
    }
    std::memcpy(fp, &out, 8);
}

} // namespace

extern "C" size_t mq_persist_write_json(mq_reflect_ctx* ctx, const mq_type_desc* type, const void* obj,
                                        char* out, size_t cap) {
    if (!ctx || !type || !obj) return 0;
    std::string s;
    write_json_value(ctx, type, obj, s);
    if (!out || cap == 0) return s.size();
    if (s.size() + 1 > cap) return s.size() + 1;               // 需要更大缓冲（不写入）
    std::memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return s.size();
}

extern "C" int mq_persist_load_json(mq_reflect_ctx* ctx, const mq_type_desc* type, const char* json, size_t len, void* out) {
    if (!ctx || !type || !json || !out) return 1;
    Parser parser{ json, json + len };
    JVal root = parser.parse();
    if (!parser.ok || root.kind != JVal::Obj) return 2;
    apply_struct(ctx, type, out, root);
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
extern "C" uint32_t mq_json_object_each(const mq_json* obj, mq_json_kv_fn fn, void* user) {
    if (!obj || !fn) return 0;
    const auto* v = reinterpret_cast<const JVal*>(obj);
    if (v->kind != JVal::Obj) return 0;
    uint32_t n = 0;
    for (auto& kv : v->obj) {
        std::string text;
        switch (kv.second.kind) {
            case JVal::Str:  text = kv.second.str; break;
            case JVal::Bool: text = kv.second.b ? "1" : "0"; break;
            case JVal::Num:  text = std::to_string(kv.second.num); break;
            case JVal::Null: text = ""; break;
            default: continue;                       // 容器值不参与键值文本遍历
        }
        ++n;
        if (fn(user, kv.first.c_str(), text.c_str()) != 0) break;
    }
    return n;
}

extern "C" int mq_persist_apply_json(mq_reflect_ctx* ctx, const mq_type_desc* type, const mq_json* node, void* out) {
    if (!ctx || !type || !node || !out) return 1;
    apply_struct(ctx, type, out, *reinterpret_cast<const JVal*>(node));
    return 0;
}
