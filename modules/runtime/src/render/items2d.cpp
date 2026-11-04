/* 2D 提取（docs/13 §8）：组件角色 → mq_renderer_item；只读，不回写模拟
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。 */
#include "../runtime_engine.h"

namespace maque::rt {

void Scene::extract(Engine& eng, std::vector<mq_renderer_item>& out) {
    /* 粒子（docs/15 §4）：只读提取，alpha 随寿命衰减；在实体图元之后按稳定顺序输出 */
    for (auto& p : eng.particles) {
        const float t = p.max_life > 0 ? p.life / p.max_life : 1.0F;
        mq_renderer_item it{};
        it.x = p.x; it.y = (float)eng.H - p.y; it.size = p.size;
        it.r = p.r; it.g = p.g; it.b = p.b;
        it.a = t < 1.0F ? 1.0F - t : 0.0F;
        it.texture = -1; it.layer = 0; it.z = 100000;      // 粒子在世界层最上
        out.push_back(it);
    }
    visit([&](int idx, SceneNode& e, int) {
        if (!e.visible) return;                       // 隐藏整棵子树（update_world 已沿树累积）
        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = mq_reflect_type_by_id(eng.reflect, tid);
            if (!d) continue;
            /* UI 控件：overlay 层（layer=1）——屏幕空间绘制，不受相机影响（docs/13 §分层） */
            if (d->role & MQ_ROLE_UI_BUTTON) {
                const auto* btn = as_component<scn::Button>(d, blob.data());
                if (!btn) continue;
                const float x = btn->x, y = btn->y, w = btn->width, h = btn->height;
                const float r = btn->r, g = btn->g, b = btn->b;
                // 矩形控件以"中心 + 半尺寸"图元表达（half_w/half_h 都 >0 时渲染器画矩形；
                // 只填 size 会退化成正方形 —— 96x28 的按钮曾显示成 96x96 方块）
                mq_renderer_item it{};
                it.x = e.world_x + x + w * 0.5F; it.y = (float)eng.H - (e.world_y + y) - h * 0.5F;
                it.size = w * 0.5F > h * 0.5F ? w * 0.5F : h * 0.5F;
                it.half_w = w * 0.5F;
                it.half_h = h * 0.5F;
                it.r = r; it.g = g; it.b = b; it.a = 1.0F;
                it.texture = -1; it.layer = 1; it.z = btn->z;
                out.push_back(it);
                continue;
            }
            if (d->role & (MQ_ROLE_UI_LABEL | MQ_ROLE_UI_TEXT)) {
                eng.extract_text(eng, idx, e, d, out, e.world_opacity);   // 真实字形渲染（docs/13/17）
                continue;
            }
            if (!(d->role & MQ_ROLE_SPRITE)) continue;
            /* 节点属性（active/angle/scale/opacity）此前被**整个忽略** ——
             * 实测：node_set_angle 不旋转、node_set_active(0) 不隐藏、node_set_scale /
             * node_set_opacity 对 sprite 无效。渲染项 ABI 本来就有这些字段
             *（rotation/a/half_w/half_h），只是提取时没填。 */
            /* 节点有效属性由 update_world 的树访问累积（角度相加、缩放相乘、透明度相乘）。 */
            const float node_angle = e.world_angle;
            const float node_scale = e.world_scale;
            const float node_opacity = e.world_opacity;
            const auto* spr = as_component<scn::Sprite>(d, blob.data());
            if (!spr) continue;
            float radius = spr->radius, r = spr->r, g = spr->g, b = spr->b;
            const float sw = spr->width, sh = spr->height;
            /* 只读提取（docs/13 §8 / docs/15 §1）：纹理与材质已在装配期预解析并缓存，
             * 此处仅查表，**不得**触发加载或改写任何引擎状态。 */
            std::string texref = spr->texture;
            if (!spr->material.empty()) {
                auto mi = eng.material_cache.find(spr->material);
                if (mi != eng.material_cache.end()) {
                    r = mi->second.base_color.x; g = mi->second.base_color.y; b = mi->second.base_color.z;
                    if (!mi->second.texture.empty()) texref = mi->second.texture;
                }
            }
            int tex = -1;
            if (!texref.empty()) {
                const std::string resolved = eng.scoped_texture_ref(idx, texref);
                auto ti = eng.tex_cache.find(resolved);
                tex = ti == eng.tex_cache.end() ? -1 : ti->second;
            }
            mq_renderer_item it{};
            it.x = e.world_x; it.y = (float)eng.H - e.world_y;
            /* width/height > 0 → **矩形**图元（瓦片/方块/非方形贴图）；否则 radius 的圆。
             * 节点缩放对两者都生效（半径/半宽半高同时乘）。 */
            const bool rect = sw > 0.0F && sh > 0.0F;
            it.size = radius * node_scale;
            it.half_w = rect ? sw * 0.5F * node_scale : 0.0F;
            it.half_h = rect ? sh * 0.5F * node_scale : 0.0F;
            /* 角度：实体存度，渲染项要弧度，且 y 轴已翻到屏幕空间 → 取负保持视觉方向一致
             *（世界 y 向上、屏幕 y 向下；不取负的话"抬头"会变成"低头"）。 */
            it.rotation = -node_angle * 3.14159265358979323846F / 180.0F;
            it.r = r; it.g = g; it.b = b;
            it.a = node_opacity;
            it.texture = tex;
            if (tex >= 0) {
                for (const auto& rt : eng.render_textures)
                    if (rt.handle == tex) { it.filter = rt.filter == 0 ? 1 : 0; break; }
            }
            it.layer = 0;                                        // 世界层
            /* 节点 z_index 必须参与排序（docs/bugs/013）：此前只写实体 id，
             * 于是 scn.Sprite 图层永远按创建顺序画、z_index 静默无效 —— 树无法盖住坦克。
             * 与 UI/Spine 用同一套 "z_index*100000+id" 稳定键（同 z 时仍按 id，禁指针序）。 */
            it.z = e.z_index * 100000 + (int)idx;
            out.push_back(it);
        }
    });
}

}  // namespace maque::rt
