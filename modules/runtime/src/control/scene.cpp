/* 控制面 · 场景（entities/get/set/spawn/destroy/emit/bake/save + 实例 docs/32）
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段 S3，纯搬运）：
 * 注册表仍由 schema 的 control_list 生成（maque_control_gen.cpp），引擎只实现语义。 */
#include "../runtime_engine.h"

using namespace maque::rt;

extern "C" int mq_control_Scene_info(void* u, const mq_json*, char* out, size_t cap) {
    auto& d = ((Engine*)u)->scene->desc;
    std::snprintf(out, cap, "{\"title\":\"%s\",\"script\":\"%s\",\"width\":%d,\"height\":%d,\"views\":%d}",
                  d.title.c_str(), d.script.c_str(), d.width, d.height, d.views);
    return 0;
}

/* Scene.emit：宿主/工具 → 脚本事件（与脚本内 node_emit_number 同一路径）。
 * 多语言下按**处理器名**路由：哪个 VM 注册了该处理器就由谁处理（跨语言/宿主→脚本共用一条路）。 */
extern "C" int mq_control_Scene_emit(void* u, const mq_json* params, char* out, size_t cap) {
    auto* eng = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const char* event = mq_json_str(mq_json_get(params, "event"), nullptr);
    const float value = (float)mq_json_num(mq_json_get(params, "value"), 0);
    if (id < 0 || !event || !*event) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"bad args\"}"); return 0; }
    const float hits = host::node_emit_number(eng, (float)id, event, value);
    std::snprintf(out, cap, "{\"ok\":true,\"event\":\"%s\",\"id\":%d,\"value\":%.4g,\"handlers\":%.0f}",
                  event, id, (double)value, (double)hits);
    return 0;
}

/* Scene.pick / Scene.raycast：3D 拾取（只读；与脚本 scene_pick/scene_raycast 同一实现）。
 * 命中返回实体 / 名字 / 距离 / 世界命中点；未命中 entity=-1。 */
static void write_pick_json(Engine& eng, const Engine::PickHit& hit, char* out, size_t cap) {
    if (hit.entity < 0) {
        std::snprintf(out, cap, "{\"ok\":true,\"entity\":-1}");
        return;
    }
    const SceneNode* node = eng.scene ? eng.scene->at(hit.entity) : nullptr;
    std::snprintf(out, cap,
                  "{\"ok\":true,\"entity\":%d,\"name\":\"%s\",\"distance\":%.5g,"
                  "\"point\":{\"x\":%.5g,\"y\":%.5g,\"z\":%.5g}}",
                  hit.entity, node ? node->name.c_str() : "", (double)hit.distance,
                  (double)hit.point[0], (double)hit.point[1], (double)hit.point[2]);
}

extern "C" int mq_control_Scene_pick(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const float x = (float)mq_json_num(mq_json_get(params, "x"), 0.0);
    const float y = (float)mq_json_num(mq_json_get(params, "y"), 0.0);
    write_pick_json(*e, e->pick_screen(x, y), out, cap);
    return 0;
}

extern "C" int mq_control_Scene_raycast(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const float origin[3] = {
        (float)mq_json_num(mq_json_get(params, "ox"), 0.0),
        (float)mq_json_num(mq_json_get(params, "oy"), 0.0),
        (float)mq_json_num(mq_json_get(params, "oz"), 0.0),
    };
    const float dir[3] = {
        (float)mq_json_num(mq_json_get(params, "dx"), 0.0),
        (float)mq_json_num(mq_json_get(params, "dy"), 0.0),
        (float)mq_json_num(mq_json_get(params, "dz"), 0.0),
    };
    const float max_distance = (float)mq_json_num(mq_json_get(params, "max_distance"), 0.0);
    write_pick_json(*e, e->pick_ray(origin, dir, max_distance), out, cap);
    return 0;
}

/* Scene.entities：实体/组件 dump（AI 可读；docs/12）。 */
extern "C" int mq_control_Scene_entities(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    std::string o = "[";
    bool first = true;
    char item[320];
    e->scene->visit([&](int idx, SceneNode& ent, int depth) {   // 前序：父一定在子之前
        std::snprintf(item, sizeof item,
                      "%s{\"id\":%d,\"name\":\"%s\",\"parent\":%d,\"depth\":%d,\"x\":%.4f,\"y\":%.4f,\"components\":%zu}",
                      first ? "" : ",", idx, ent.name.c_str(), ent.parent, depth,
                      ent.world_x, ent.world_y, ent.comps.size());
        first = false;
        o += item;
    });
    o += "]";
    std::snprintf(out, cap, "%s", o.c_str());
    return 0;
}

/* Scene.get / Scene.set：按实体 + 字段路径读写组件字段（反射驱动；编辑器/AI 的编辑面）。
 * params: { id, path, value? }。path 形式 "Type.field" 或裸 "field"（docs/11 §3/§4）。 */
extern "C" int mq_control_Scene_get(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    /* path == "" → **dump 整个实体**（反射驱动：组件 + 全部标量字段）——inspector 属性面板用 */
    if (path && !*path) {
        SceneNode* ent = e->scene ? e->scene->at(id) : nullptr;
        if (!ent) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no entity\"}"); return 0; }
        size_t n = 0;
        n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"components\":[",
                                   id, ent->name.c_str());
        bool first_c = true;
        for (auto& [tid, blob] : ent->comps) {
            const mq_type_desc* d = blob.desc;
            if (!d) continue;
            n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "%s{\"type\":\"%s\",\"fields\":{",
                                       first_c ? "" : ",", d->name);
            first_c = false;
            bool first_f = true;
            for (uint32_t fi = 0; fi < d->field_count; ++fi) {
                const mq_field_desc* f = &d->fields[fi];
                void* fp = blob.data() + f->offset;
                char val[160] = { 0 };
                switch (f->kind) {
                    case MQ_TK_F32: std::snprintf(val, sizeof val, "%.4g", *(float*)fp); break;
                    case MQ_TK_I32: std::snprintf(val, sizeof val, "%d", *(int32_t*)fp); break;
                    case MQ_TK_U32: std::snprintf(val, sizeof val, "%u", *(uint32_t*)fp); break;
                    case MQ_TK_I64: std::snprintf(val, sizeof val, "%lld", (long long)*(int64_t*)fp); break;
                    case MQ_TK_U64: std::snprintf(val, sizeof val, "%llu", (unsigned long long)*(uint64_t*)fp); break;
                    case MQ_TK_BOOL: std::snprintf(val, sizeof val, "%s", *(bool*)fp ? "true" : "false"); break;
                    case MQ_TK_STRING: std::snprintf(val, sizeof val, "\"%s\"", ((std::string*)fp)->c_str()); break;
                    default: std::snprintf(val, sizeof val, "null"); break;   /* 复合类型：面板暂不展开 */
                }
                n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "%s\"%s\":%s",
                                           first_f ? "" : ",", f->name, val);
                first_f = false;
            }
            n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "}}");
        }
        n += (size_t)std::snprintf(out + n, n < cap ? cap - n : 0, "]}");
        return 0;
    }
    const mq_type_desc* tt = nullptr; const mq_field_desc* tf = nullptr; void* base = nullptr;
    if (id < 0 || !path || !resolve_field_path(*e, id, path, &tt, &tf, &base)) {
        std::snprintf(out, cap, "{\"ok\":false}");
        return 0;
    }
    void* fp = (uint8_t*)base + tf->offset;
    if (tf->kind == MQ_TK_F32)        std::snprintf(out, cap, "{\"ok\":true,\"value\":%.6f}", (double)*(float*)fp);
    else if (tf->kind == MQ_TK_STRING) std::snprintf(out, cap, "{\"ok\":true,\"value\":\"%s\"}", ((std::string*)fp)->c_str());
    else if (tf->kind == MQ_TK_STRUCT) {
        const mq_type_desc* vt = mq_reflect_type_by_id(e->reflect, tf->type_id);
        std::string vals = "{\"ok\":true,\"value\":{";
        if (vt) for (uint32_t i = 0; i < vt->field_count; ++i) {
            const mq_field_desc* vf = &vt->fields[i];
            if (vf->kind != MQ_TK_F32) continue;
            vals += std::string(i ? ",\"" : "\"") + vf->name + "\":" + std::to_string(field_get_f32(vf, fp));
        }
        vals += "}}";
        std::snprintf(out, cap, "%s", vals.c_str());
    } else {
        int64_t iv = 0;
        mq_reflect_get_i64(e->reflect, base, tf, &iv);
        std::snprintf(out, cap, "{\"ok\":true,\"value\":%lld}", (long long)iv);
    }
    return 0;
}

extern "C" int mq_control_Scene_set(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    const mq_json* vj = mq_json_get(params, "value");
    const mq_type_desc* tt = nullptr; const mq_field_desc* tf = nullptr; void* base = nullptr;
    if (id < 0 || !path || !vj || !resolve_field_path(*e, id, path, &tt, &tf, &base)) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"not found\"}");
        return 0;
    }
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true}"); return 0; }
    void* fp = (uint8_t*)base + tf->offset;
    if (tf->kind == MQ_TK_STRING) {
        const char* s = mq_json_str(vj, "");
        (void)field_set_str(tf, base, s ? s : "");
    } else {
        mq_reflect_set_f64(e->reflect, base, tf, mq_json_num(vj, 0.0));
    }
    std::snprintf(out, cap, "{\"ok\":true,\"id\":%d,\"path\":\"%s\"}", id, path);
    return 0;
}

/* Scene.spawn / Scene.destroy：实体增删（编辑器/AI 编辑；返回句柄与 undo 信息，docs/11 §4）。 */
extern "C" int mq_control_Scene_spawn(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    const char* name = mq_json_str(mq_json_get(params, "name"), "");
    const double parent = mq_json_num(mq_json_get(params, "parent"), -1);
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true,\"would_spawn\":\"%s\"}", name); return 0; }
    const int id = e->scene->spawn();
    if (id < 0) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"spawn failed\"}"); return 0; }
    e->entity_instance[id] = 0;                 // 控制面编辑：归世界（根实例）
    e->scene->attach(id, (int)parent);
    if (SceneNode* ent = e->scene->at(id)) {
        ent->name = name;
        if (const char* tn = mq_json_str(mq_json_get(params, "component"), nullptr)) attach_component(*e, id, tn);
    }
    std::snprintf(out, cap, "{\"ok\":true,\"id\":%d,\"undo\":{\"destroy\":%d}}", id, id);
    return 0;
}

extern "C" int mq_control_Scene_destroy(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "id"), -1);
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    SceneNode* ent = e->scene->at(id);
    if (!ent) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no such entity\"}"); return 0; }
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true,\"name\":\"%s\"}", ent->name.c_str()); return 0; }
    e->scene->remove(id);
    std::snprintf(out, cap, "{\"ok\":true,\"destroyed\":%d,\"entities\":%d}", id, e->scene->count());
    return 0;
}

/* Scene.expand_prefabs：把 PrefabInstance 展开（detached=true）→ 场景自包含（docs/12 §5）。
 * 旧名 Scene.bake 保留为兼容别名（语义完全相同；"bake" 一词不再用于世界展平）。 */
extern "C" int mq_control_Scene_expand_prefabs(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    uint32_t expanded = 0;
    for (auto& [idx, ent] : e->scene->nodes) {
        for (auto& [tid, blob] : ent.comps) {
            const mq_type_desc* d = blob.desc;
            if (!d || std::strcmp(d->name, "scn.PrefabInstance") != 0) continue;
            const std::string* src = read_str(d, blob.data(), "source");
            if (!src || src->empty()) continue;
            const mq_field_desc* fd = mq_reflect_field(d, "detached");
            if (!fd) continue;
            if (field_get_bool(fd, blob.data())) continue;
            if (!dry) field_set_bool(fd, blob.data(), true);
            ++expanded;
        }
    }
    std::snprintf(out, cap, "{\"ok\":true,\"expanded\":%u,\"dry_run\":%s}", expanded, dry ? "true" : "false");
    return 0;
}

/* Scene.bake：Scene.expand_prefabs 的兼容别名（已弃用；docs/12 §5 改名） */
extern "C" int mq_control_Scene_bake(void* u, const mq_json* params, char* out, size_t cap) {
    return mq_control_Scene_expand_prefabs(u, params, out, cap);
}

/* Scene.save：把当前世界写回 .scn（编辑器落盘；docs/11 §4）。
 * docs/32 §6.2：场景描述符走**反射**（全部字段零遗漏）；嵌套实例默认**保引用不烘焙**
 * （只写 SceneInstance 组件，生成的子树不落盘）；detached/已烘焙实例才写子树。 */
extern "C" int mq_control_Scene_save(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    if (!path) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path\"}"); return 0; }
    if (!e->scene || !e->scene_type) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no scene\"}"); return 0; }

    /* 1) 描述符：反射序列化全部字段（含 scripts/overlay_script/env/views/...；docs/32 §6.1） */
    std::vector<char> dbuf(8192);
    size_t dneed = mq_persist_write_json(e->reflect, e->scene_type, &e->scene->desc, dbuf.data(), dbuf.size());
    if (dneed + 1 > dbuf.size()) {
        dbuf.resize(dneed + 1);
        dneed = mq_persist_write_json(e->reflect, e->scene_type, &e->scene->desc, dbuf.data(), dbuf.size());
    }
    std::string json;
    if (dneed >= 2 && dbuf[0] == '{') json.assign(dbuf.data(), dneed - 1);   // 去掉最后的 '}'
    else json = "{\"version\":2";

    /* 2) 实体：只写根实例拥有的节点；未被烘焙的子实例内容不写（由 source 重建）。 */
    const auto included = [&](int id) {
        auto it = e->entity_instance.find(id);
        if (it == e->entity_instance.end()) return true;    // 无归属信息（老场景/宿主手工节点）
        int owner = it->second;
        if (owner <= 0) return true;
        /* 只有**本实例已烘焙**才把内容写进父场景；未烘焙实例的内容由 source 重建，
         * 即使其祖先已烘焙也不写（否则重载会重复实例化）。 */
        SceneInstanceRuntime* cur = e->instance_at(owner);
        return cur && cur->detached;
    };
    json += ",\n  \"entities\": [";

    std::vector<int> ids;
    e->scene->collect_preorder(ids);
    std::function<void(int, int)> emit = [&](int id, int indent) {
        SceneNode* ent = e->scene->at(id);
        if (!ent) return;
        const std::string pad((size_t)indent * 2, ' ');
        json += "\n" + pad + "{ \"name\": ";
        write_json_string_pub(json, ent->name);
        json += ", \"components\": [";
        std::vector<uint32_t> tids;
        for (auto& [t, b] : ent->comps) tids.push_back(t);
        std::sort(tids.begin(), tids.end());
        std::vector<char> buf(16384);
        bool f2 = true;
        for (uint32_t t : tids) {
            const mq_type_desc* d = mq_reflect_type_by_id(e->reflect, t);
            if (!d) continue;
            size_t need = mq_persist_write_json(e->reflect, d, ent->comps[t].data(), buf.data(), buf.size());
            if (need + 1 > buf.size()) { buf.resize(need + 1); need = mq_persist_write_json(e->reflect, d, ent->comps[t].data(), buf.data(), buf.size()); }
            if (need == 0 || need + 1 > buf.size()) continue;
            if (!f2) json += ", ";
            f2 = false;
            json += "{ \"type\": ";
            write_json_string_pub(json, d->name);
            json += ", ";
            json += std::string(buf.data() + 1, need - 2);          // 去掉外层 { }
            json += " }";
        }
        json += "]";
        std::vector<int> kids;
        for (int k : ent->children) if (included(k)) kids.push_back(k);
        if (!kids.empty()) {
            json += ", \"children\": [";
            for (size_t i = 0; i < kids.size(); ++i) { if (i) json += ","; emit(kids[i], indent + 1); }
            json += "\n" + pad + "]";
        }
        json += " }";
    };
    bool first = true;
    for (int id : ids) {
        SceneNode* ent = e->scene->at(id);
        if (!ent || (ent->parent >= 0 && e->scene->at(ent->parent))) continue;   // 仅从根开始
        if (!included(id)) continue;
        if (!first) json += ",";
        first = false;
        emit(id, 2);
    }
    json += "\n  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"cannot write\"}"); return 0; }
    f << json;
    std::snprintf(out, cap, "{\"ok\":true,\"path\":\"%s\",\"entities\":%d,\"bytes\":%zu}",
                  path, e->scene->count(), json.size());
    return 0;
}

/* ---- 场景实例控制面（docs/32 §7.2）：查询只读；变更类走与脚本同一条帧边界队列 ---- */
extern "C" int mq_control_Scene_instances(void* u, const mq_json*, char* out, size_t cap) {
    auto* e = (Engine*)u;
    int live = 0;
    for (auto& [id, up] : e->instances) {
        (void)id;
        if (up && up->state != 2) ++live;
    }
    std::string json = "{\"ok\":true,\"count\":" + std::to_string(live) + ",\"instances\":[";
    bool first = true;
    for (auto& [id, up] : e->instances) {
        (void)id;
        if (!up || up->state == 2) continue;
        if (!first) json += ',';
        first = false;
        char head[256];
        std::snprintf(head, sizeof head,
                      "{\"id\":%d,\"parent\":%d,\"attach\":%d,\"paused\":%s,\"active\":%s,\"state\":%d,"
                      "\"error\":\"%s\",\"source\":",
                      up->id, up->parent, up->attach_entity, up->paused ? "true" : "false",
                      up->active ? "true" : "false", up->state, up->error.c_str());
        json += head;
        write_json_string_pub(json, up->source);
        json += ",\"scripts\":[";
        for (size_t i = 0; i < up->scripts.size(); ++i) {
            if (i) json += ',';
            write_json_string_pub(json, up->scripts[i]);
        }
        json += "],\"roots\":[";
        for (size_t i = 0; i < up->roots.size(); ++i) {
            if (i) json += ',';
            json += std::to_string(up->roots[i]);
        }
        json += "]}";
    }
    json += "]}";
    std::snprintf(out, cap, "%s", json.c_str());
    return 0;
}

extern "C" int mq_control_Scene_instance_info(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "instance"), -1);
    SceneInstanceRuntime* up = e->instance_at(id);
    if (!up) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no such instance\"}"); return 0; }
    std::string json = "{\"ok\":true,\"instance\":{";
    char head[320];
    std::snprintf(head, sizeof head,
                  "\"id\":%d,\"parent\":%d,\"attach\":%d,\"paused\":%s,\"active\":%s,\"state\":%d,"
                  "\"error\":\"%s\",\"source\":",
                  up->id, up->parent, up->attach_entity, up->paused ? "true" : "false",
                  up->active ? "true" : "false", up->state, up->error.c_str());
    json += head;
    write_json_string_pub(json, up->source);
    json += ",\"scripts\":[";
    for (size_t i = 0; i < up->scripts.size(); ++i) {
        if (i) json += ',';
        write_json_string_pub(json, up->scripts[i]);
    }
    json += "],\"roots\":[";
    for (size_t i = 0; i < up->roots.size(); ++i) { if (i) json += ','; json += std::to_string(up->roots[i]); }
    json += "]}}";
    std::snprintf(out, cap, "%s", json.c_str());
    return 0;
}

extern "C" int mq_control_Scene_instantiate(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const char* path = mq_json_str(mq_json_get(params, "path"), nullptr);
    const char* iparams = mq_json_str(mq_json_get(params, "params"), nullptr);
    const int parent = (int)mq_json_num(mq_json_get(params, "parent"), -1);
    if (!path || !*path) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"missing path\"}"); return 0; }
    const float id = host::scene_instantiate(e, (float)parent, path, iparams);
    if (id < 0) {
        std::snprintf(out, cap, "{\"ok\":false,\"error\":\"%s\"}", e->scene_last_error.c_str());
        return 0;
    }
    std::snprintf(out, cap, "{\"ok\":true,\"instance\":%d,\"deferred\":true}", (int)id);
    return 0;
}

extern "C" int mq_control_Scene_unload_instance(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "instance"), -1);
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    SceneInstanceRuntime* up = e->instance_at(id);
    if (!up || up->state == 2) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"no such instance\"}"); return 0; }
    if (dry) { std::snprintf(out, cap, "{\"ok\":true,\"dry_run\":true}"); return 0; }
    if (id == 0) { std::snprintf(out, cap, "{\"ok\":false,\"error\":\"root instance cannot be unloaded\"}"); return 0; }
    const float ok = host::scene_unload(e, (float)id);
    std::snprintf(out, cap, "{\"ok\":%s,\"deferred\":true}", ok != 0.0F ? "true" : "false");
    return 0;
}

extern "C" int mq_control_Scene_reload_instance(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const int id = (int)mq_json_num(mq_json_get(params, "instance"), -1);
    const float ok = host::scene_reload(e, (float)id);
    std::snprintf(out, cap, "{\"ok\":%s,\"deferred\":true}", ok != 0.0F ? "true" : "false");
    return 0;
}

/* 把 SceneInstance 标记 detached=true（烘焙；与 Scene.expand_prefabs 对齐）。 */
extern "C" int mq_control_Scene_bake_instances(void* u, const mq_json* params, char* out, size_t cap) {
    auto* e = (Engine*)u;
    const bool dry = mq_json_num(mq_json_get(params, "dry_run"), 0) != 0.0;
    const mq_type_desc* sid = mq_reflect_type_by_name(e->reflect, "scn.SceneInstance");
    int marked = 0;
    if (sid && e->scene) {
        for (auto& [eid, ent] : e->scene->nodes) {
            auto it = ent.comps.find(sid->type_id);
            if (it == ent.comps.end()) continue;
            const mq_field_desc* f = mq_reflect_field(sid, "detached");
            if (!f) continue;
            if (field_get_bool(f, it->second.data())) continue;
            if (!dry) field_set_bool(f, it->second.data(), true);
            if (!dry) {
                if (SceneInstanceRuntime* up = e->instance_by_attach(eid)) up->detached = true;
            }
            ++marked;
        }
    }
    std::snprintf(out, cap, "{\"ok\":true,\"marked\":%d,\"dry_run\":%s}", marked, dry ? "true" : "false");
    return 0;
}
