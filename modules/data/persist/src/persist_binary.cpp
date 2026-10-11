/* 二进制 codec + 快照池实现（docs/10 §4/§5）。
 * 编码按**声明顺序逐字段**写入（无 padding、无地址、无墙钟）→ 逐位可逆、可跨进程比较。 */
#include "core/persist/mq_persist_binary.h"

#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <optional>

namespace {

/* ---------------- 写 ---------------- */

struct Writer {
    uint8_t* out;
    size_t   cap;
    size_t   pos = 0;
    bool     overflow = false;

    void raw(const void* p, size_t n) {
        if (overflow) return;
        if (pos + n > cap) { overflow = true; return; }
        std::memcpy(out + pos, p, n);
        pos += n;
    }
    template <class T> void pod(T v) { raw(&v, sizeof(T)); }
    void u32(uint32_t v) { pod(v); }
    void u64(uint64_t v) { pod(v); }
    void str(const std::string& s) { u64(s.size()); if (!s.empty()) raw(s.data(), s.size()); }
};

/* ---------------- 读 ---------------- */

struct Reader {
    const uint8_t* in;
    size_t         size;
    size_t         pos = 0;
    bool           bad = false;

    bool raw(void* p, size_t n) {
        if (bad || pos + n > size) { bad = true; return false; }
        std::memcpy(p, in + pos, n);
        pos += n;
        return true;
    }
    template <class T> bool pod(T& v) { return raw(&v, sizeof(T)); }
    bool u32(uint32_t& v) { return pod(v); }
    bool u64(uint64_t& v) { return pod(v); }
    bool str(std::string& s) {
        uint64_t n = 0;
        if (!u64(n)) return false;
        if (pos + n > size) { bad = true; return false; }
        s.assign((const char*)(in + pos), (size_t)n);
        pos += (size_t)n;
        return true;
    }
};

size_t kind_width(mq_type_kind k) {
    switch (k) {
        case MQ_TK_BOOL: case MQ_TK_I8: case MQ_TK_U8:   return 1;
        case MQ_TK_I16:  case MQ_TK_U16:                 return 2;
        case MQ_TK_I32:  case MQ_TK_U32: case MQ_TK_F32: return 4;
        case MQ_TK_I64:  case MQ_TK_U64: case MQ_TK_F64: return 8;
        /* 引用：固定布局 POD（maque::EntityRef = {u32,u32}，maque::AssetRef = {u64}）→ 8 字节。
         * 必须序列化，否则实体/资产引用在快照/回滚中丢失（docs/08 §序列化、docs/10 §4）。 */
        case MQ_TK_ENTITY_REF:
        case MQ_TK_ASSET_REF: return 8;
        /* map/optional：元素大小未在描述符中给出（无容器算子）→ 不参与二进制序列化，
         * 由调用方改用 JSON 或为其补容器算子（见 docs/23 已知缺口）。 */
        case MQ_TK_MAP:
        case MQ_TK_OPTIONAL:
        default: return 0;
    }
}

void enc_value(mq_reflect_ctx* ctx, const mq_type_desc* t, const void* obj, Writer& w);

void enc_field(mq_reflect_ctx* ctx, const mq_field_desc* f, const void* fp, Writer& w) {
    switch (f->kind) {
        case MQ_TK_STRUCT: {
            const mq_type_desc* ft = mq_reflect_type_by_id(ctx, f->type_id);
            if (ft) enc_value(ctx, ft, fp, w);
            return;
        }
        case MQ_TK_ENUM: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            w.raw(fp, et && et->size ? et->size : 4);
            return;
        }
        case MQ_TK_STRING:
            w.str(*reinterpret_cast<const std::string*>(fp));
            return;
        case MQ_TK_ARRAY_FIXED: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            if (et) { for (uint32_t i = 0; i < f->count; ++i) enc_value(ctx, et, (const uint8_t*)fp + (size_t)i * et->size, w); }
            else    { w.raw(fp, kind_width(f->kind) * f->count); }   // 标量定长数组
            return;
        }
        case MQ_TK_ARRAY_DYN: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->elem_type_id);
            uint32_t n = 0;
            if (et && f->container_size) n = f->container_size(fp);
            w.u32(n);
            if (!et || !f->container_elem) return;
            /* 容器算子返回可变指针；编码只读（docs/13 §8：渲染/序列化不写模拟状态） */
            for (uint32_t i = 0; i < n; ++i)
                enc_value(ctx, et, static_cast<const void*>(f->container_elem(const_cast<void*>(fp), i)), w);
            return;
        }
        case MQ_TK_MAP:
        case MQ_TK_OPTIONAL:
            return;                                  // 未支持：跳过（不写垃圾字节，往返不会错位）
        default:
            w.raw(fp, kind_width(f->kind));
            return;
    }
}

void enc_value(mq_reflect_ctx* ctx, const mq_type_desc* t, const void* obj, Writer& w) {
    if (t->flags & MQ_TYPE_ENUM) { w.raw(obj, t->size ? t->size : 4); return; }
    for (uint32_t i = 0; i < t->field_count; ++i) {
        const mq_field_desc* f = &t->fields[i];
        if (f->attrs_flags & MQ_ATTR_TRANSIENT) continue;   // 瞬态字段不入快照
        enc_field(ctx, f, (const uint8_t*)obj + f->offset, w);
    }
}

void dec_value(mq_reflect_ctx* ctx, const mq_type_desc* t, void* obj, Reader& r);

void dec_field(mq_reflect_ctx* ctx, const mq_field_desc* f, void* fp, Reader& r) {
    switch (f->kind) {
        case MQ_TK_STRUCT: {
            const mq_type_desc* ft = mq_reflect_type_by_id(ctx, f->type_id);
            if (ft) dec_value(ctx, ft, fp, r);
            return;
        }
        case MQ_TK_ENUM: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            r.raw(fp, et && et->size ? et->size : 4);
            return;
        }
        case MQ_TK_STRING:
            r.str(*reinterpret_cast<std::string*>(fp));
            return;
        case MQ_TK_ARRAY_FIXED: {
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->type_id);
            if (et) { for (uint32_t i = 0; i < f->count; ++i) dec_value(ctx, et, (uint8_t*)fp + (size_t)i * et->size, r); }
            else    { r.raw(fp, kind_width(f->kind) * f->count); }
            return;
        }
        case MQ_TK_ARRAY_DYN: {
            uint32_t n = 0;
            if (!r.u32(n)) return;
            const mq_type_desc* et = mq_reflect_type_by_id(ctx, f->elem_type_id);
            if (!et || !f->container_resize || !f->container_elem) return;
            if (n > 1u << 24) { r.bad = true; return; }      // 防御：损坏输入不得触发巨量分配
            f->container_resize(fp, n);
            for (uint32_t i = 0; i < n; ++i) dec_value(ctx, et, f->container_elem(fp, i), r);
            return;
        }
        case MQ_TK_MAP:
        case MQ_TK_OPTIONAL:
            return;                                  // 与编码一致：跳过
        default:
            r.raw(fp, kind_width(f->kind));
            return;
    }
}

void dec_value(mq_reflect_ctx* ctx, const mq_type_desc* t, void* obj, Reader& r) {
    if (t->flags & MQ_TYPE_ENUM) { r.raw(obj, t->size ? t->size : 4); return; }
    for (uint32_t i = 0; i < t->field_count; ++i) {
        const mq_field_desc* f = &t->fields[i];
        if (f->attrs_flags & MQ_ATTR_TRANSIENT) continue;
        dec_field(ctx, f, (uint8_t*)obj + f->offset, r);
    }
}

} // namespace

extern "C" {

size_t mq_persist_encode(mq_reflect_ctx* ctx, const mq_type_desc* type, const void* obj,
                         void* out, size_t out_cap) {
    if (!ctx || !type || !obj || !out) return 0;
    Writer w{ (uint8_t*)out, out_cap, 0, false };
    enc_value(ctx, type, obj, w);
    return w.overflow ? 0 : w.pos;
}

long mq_persist_decode(mq_reflect_ctx* ctx, const mq_type_desc* type, const void* in,
                       size_t in_size, void* out) {
    if (!ctx || !type || !in || !out) return -1;
    Reader r{ (const uint8_t*)in, in_size, 0, false };
    dec_value(ctx, type, out, r);
    return r.bad ? -1 : (long)r.pos;
}

} // extern "C"

/* ---------------- 快照池 ---------------- */

struct mq_snapshot {
    std::vector<uint8_t> buf;
    size_t   used = 0;
    uint64_t frame = 0;
    std::string desc;
};

struct mq_snapshot_pool {
    std::vector<mq_snapshot*> slots;    // 预分配，循环复用
    size_t   capacity = 0;
    uint32_t cursor = 0;
};

extern "C" {

mq_snapshot_pool* mq_snapshot_pool_create(uint32_t count, size_t capacity_bytes) {
    auto* p = new mq_snapshot_pool();
    p->capacity = capacity_bytes;
    p->slots.reserve(count ? count : 1);
    for (uint32_t i = 0; i < (count ? count : 1); ++i) {
        auto* s = new mq_snapshot();
        s->buf.resize(capacity_bytes);
        p->slots.push_back(s);
    }
    return p;
}

void mq_snapshot_pool_destroy(mq_snapshot_pool* pool) {
    if (!pool) return;
    for (auto* s : pool->slots) delete s;
    delete pool;
}

mq_snapshot* mq_snapshot_pool_acquire(mq_snapshot_pool* pool) {
    if (!pool || pool->slots.empty()) return nullptr;
    mq_snapshot* s = pool->slots[pool->cursor];
    pool->cursor = (pool->cursor + 1) % (uint32_t)pool->slots.size();   // 循环复用（避免抖动）
    s->used = 0;
    s->desc.clear();
    return s;
}

mq_snapshot* mq_snapshot_create(void) { return new mq_snapshot(); }
void         mq_snapshot_destroy(mq_snapshot* s) { delete s; }

void* mq_snapshot_begin(mq_snapshot* s, size_t want) {
    if (!s) return nullptr;
    if (s->buf.size() < want) s->buf.resize(want);   // 容量足够时不重新分配
    s->used = 0;
    return s->buf.data();
}

void mq_snapshot_commit(mq_snapshot* s, size_t used) {
    if (!s) return;
    s->used = used <= s->buf.size() ? used : s->buf.size();
}

const void* mq_snapshot_bytes(mq_snapshot* s, size_t* out_size) {
    if (out_size) *out_size = s ? s->used : 0;
    return s && s->used ? (const void*)s->buf.data() : nullptr;
}

void        mq_snapshot_set_frame(mq_snapshot* s, uint64_t frame) { if (s) s->frame = frame; }
uint64_t    mq_snapshot_frame(mq_snapshot* s) { return s ? s->frame : 0; }
void        mq_snapshot_set_desc(mq_snapshot* s, const char* desc) { if (s) s->desc = desc ? desc : ""; }
const char* mq_snapshot_desc(mq_snapshot* s) { return s ? s->desc.c_str() : ""; }

mq_snapshot* mq_snapshot_clone(mq_snapshot* s) {
    if (!s) return nullptr;
    auto* c = new mq_snapshot();
    c->buf = s->buf;
    c->used = s->used;
    c->frame = s->frame;
    c->desc = s->desc;
    return c;
}

uint64_t mq_snapshot_checksum(mq_snapshot* s) {
    if (!s) return 0;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < s->used; ++i) { h ^= s->buf[i]; h *= 1099511628211ull; }
    return h;
}

} // extern "C"
