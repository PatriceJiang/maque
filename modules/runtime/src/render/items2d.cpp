/* 2D 提取（docs/13 §8）：组件角色 → mq_renderer_item；只读，不回写模拟
 *
 * 从 runtime.cpp 拆出（plans/25 第 1 阶段，纯搬运）：实现与顺序不变。
 * UI 裁剪（ui.Mask）与 9-patch 在提取期展开：一个逻辑矩形 → 1..N 个普通图元，
 * 每块都有自己的裁剪矩形；gfx 按 BatchKey（贴图+采样+裁剪）合批。 */
#include "../runtime_engine.h"
#include "nine_patch.h"

namespace maque::rt {
namespace {

/* 世界 y 向上 → 屏幕 y 向下；ClipRect 的 (x0,y0,x1,y1) 在这里翻成 item 的左上+宽高。 */
void set_item_clip(mq_renderer_item& it, const ClipRect& r, float H) {
    it.clip_x = r.x0;
    it.clip_y = H - r.y1;
    it.clip_w = r.x1 - r.x0;
    it.clip_h = r.y1 - r.y0;
}

/* 图元 AABB（屏幕设计坐标）与裁剪矩形是否相交。reverse 会把可见区拆成多条带；
 * 只把**与该带相交**的块放进该带，否则 9-patch 的 9 块会在每条带里各来一份：
 * 图元数 = 9×条带、且不同带的块 AABB 相交 → 合批被顺序安全切碎（实测 29 draws）。
 * 裁剪后每条带只保留真正可见的块 → 图元数≈可见块数，且同一带内仍是一次 drawcall。 */
bool item_intersects_clip(const mq_renderer_item& it, const ClipRect& r, float H) {
    const float hw = it.half_w > 0.0F ? it.half_w : it.size;
    const float hh = it.half_h > 0.0F ? it.half_h : it.size;
    if (!(hw > 0.0F) || !(hh > 0.0F)) return false;
    const float c = std::abs(std::cos(it.rotation)), s = std::abs(std::sin(it.rotation));
    const float ex = hw * c + hh * s, ey = hw * s + hh * c;
    const float x0 = it.x - ex, x1 = it.x + ex;
    const float y0 = it.y - ey, y1 = it.y + ey;
    const float cx0 = r.x0, cy0 = H - r.y1, cx1 = r.x1, cy1 = H - r.y0;
    return x0 < cx1 && x1 > cx0 && y0 < cy1 && y1 > cy0;
}

/* 一个逻辑图元 → 每个可见裁剪区一份（reverse Mask 的差集会给出多块）。
 * 不裁剪时只推一份（零 clip = 不裁剪，旧调用方语义）。 */
void emit_with_clip(std::vector<mq_renderer_item>& out, const mq_renderer_item& base,
                    const ClipSet& clip, float H) {
    if (!clip.active) { out.push_back(base); return; }
    for (const ClipRect& r : clip.rects) {
        if (!item_intersects_clip(base, r, H)) continue;
        mq_renderer_item it = base;
        set_item_clip(it, r, H);
        out.push_back(it);
    }
}

float item_extent(float half_w, float half_h, float size) {
    const float hw = half_w > 0.0F ? half_w : size;
    const float hh = half_h > 0.0F ? half_h : size;
    return hw > hh ? hw : hh;
}

}  // namespace

void Scene::extract(Engine& eng, std::vector<mq_renderer_item>& out) {
    /* 程序化 alpha mask 链：0 号恒为 identity（item.mask=0 不需要查表）。 */
    eng.ui_masks.clear();
    {
        mq_renderer_mask id{};
        id.x0 = 0; id.y0 = 0; id.x1 = (float)eng.W; id.y1 = (float)eng.H;
        id.opacity = 1.0F; id.curve = 1; id.next = -1;
        eng.ui_masks.push_back(id);
    }
    /* 粒子（docs/15 §4）：只读提取，alpha 随寿命衰减；在实体图元之后按稳定顺序输出 */
    for (auto& p : eng.systems.particles) {
        const float t = p.max_life > 0 ? p.life / p.max_life : 1.0F;
        mq_renderer_item it{};
        it.x = p.x; it.y = (float)eng.H - p.y; it.size = p.size;
        it.r = p.r; it.g = p.g; it.b = p.b;
        it.a = t < 1.0F ? 1.0F - t : 0.0F;
        it.texture = -1; it.layer = 0; it.z = 100000;      // 粒子在世界层最上
        out.push_back(it);
    }

    /* 递归提取（不能再用扁平 visit）：裁剪是**子树继承**的，必须在进入子节点时带着
     * 父节点的可见矩形集合。遍历顺序与 visit 完全一致（根升序、子升序）。 */
    std::function<void(int, const ClipSet&, int)> walk = [&](int idx, const ClipSet& parent_clip,
                                                            int parent_mask) {
        SceneNode* ep = at(idx);
        if (!ep) return;
        SceneNode& e = *ep;
        if (!e.visible) return;                       // 隐藏整棵子树（update_world 已沿树累积）

        ClipSet self_clip, child_clip;
        if (eng.has_ui_components)
            compute_ui_clip(e, parent_clip, (float)eng.W, (float)eng.H, self_clip, child_clip);
        else { self_clip = parent_clip; child_clip = parent_clip; }

        /* 程序化 alpha mask：ListView 把视口 + 四边 fade + content_alpha 推入链；
         * 子节点继承，嵌套 ListView 自动相乘。滚动条/百分比使用父 mask（不随内容淡出）。 */
        int self_mask = parent_mask, child_mask = parent_mask;
        static const bool alpha_mask_enabled = [] {
            const char* v = std::getenv("MAQUE_UI_ALPHA_MASK");
            return !v || std::strcmp(v, "0") != 0;
        }();
        if (const auto* lv = component<ui::ListView>(e); lv && alpha_mask_enabled) {
            const auto* ctl = component<ui::Control>(e);
            const bool has_fade = lv->fade_top > 0.0F || lv->fade_bottom > 0.0F ||
                                  lv->fade_left > 0.0F || lv->fade_right > 0.0F ||
                                  lv->content_alpha < 0.999F;
            if (ctl && has_fade) {
                const float s = e.world_scale > 0.0F ? e.world_scale : 1.0F;
                const float vw = ctl->offset_max.x * s, vh = ctl->offset_max.y * s;
                if (vw > 0.0F && vh > 0.0F) {
                    mq_renderer_mask m{};
                    m.x0 = e.world_x;
                    m.y0 = (float)eng.H - (e.world_y + vh);
                    m.x1 = e.world_x + vw;
                    m.y1 = (float)eng.H - e.world_y;
                    m.fade_l = lv->fade_left * s; m.fade_t = lv->fade_top * s;
                    m.fade_r = lv->fade_right * s; m.fade_b = lv->fade_bottom * s;
                    m.opacity = lv->content_alpha;
                    m.curve = (int)lv->fade_curve;
                    m.next = parent_mask;
                    eng.ui_masks.push_back(m);
                    child_mask = (int)eng.ui_masks.size() - 1;
                }
            }
        }
        if (e.ui_mask_override >= -1) { self_mask = e.ui_mask_override; child_mask = e.ui_mask_override; }

        /* 节点级剔除：ListView 滚出视口的行 Label 不再排版/提交（text_h 是上一帧度量）。
         * 只对未旋转标签做保守 AABB；描边/阴影留 2px 余量。 */
        if (self_clip.active && e.world_angle == 0.0F) {
            if (const auto* l = component<scn::Label>(e); l && l->text_h > 0.0F && l->text_w > 0.0F) {
                const float pad = std::max(0.0F, l->outline_size) + 2.0F;
                const float x0 = e.world_x + l->x - pad;
                const float x1 = x0 + l->text_w + pad * 2.0F;
                const float ytop = e.world_y + l->y + pad;
                const float y0 = (float)eng.H - ytop;
                const float y1 = y0 + l->text_h + pad * 2.0F;
                if (!clip_intersects_screen_aabb(self_clip, x0, y0, x1, y1, (float)eng.H))
                    return;
            }
        }

        for (auto& [tid, blob] : e.comps) {
            const mq_type_desc* d = blob.desc;
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
                it.mask = self_mask;
                emit_with_clip(out, it, self_clip, (float)eng.H);
                continue;
            }
            if (d->role & (MQ_ROLE_UI_LABEL | MQ_ROLE_UI_TEXT)) {
                eng.extract_text(eng, idx, e, d, out, e.world_opacity, self_clip, self_mask);
                continue;
            }
            /* UI 图片（ui.Image，docs/17）：layer=1 的带纹理矩形。
             * 贴图走 load_ui_texture（create_texture2 通道）→ gfx 的 **UI 动态合图**可以把多张
             * 小图并进预分配的 1024² 图集 → 一次 drawcall（与字体图集分开，文字仍用字体图集）。
             * 尺寸：ui.Control 的内容尺寸优先；没有 Control 时用装配期缓存的贴图尺寸。
             * 九宫格：边距按**源图像素**，展开成最多 9 个同贴图/同裁剪的图元 → 仍然一次合批。 */
            if (const auto* img = as_component<ui::Image>(d, blob.data())) {
                if (img->texture.empty()) continue;
                const auto* ctl = component<ui::Control>(e);
                float w = ctl ? ctl->offset_max.x : 0.0F;
                float h = ctl ? ctl->offset_max.y : 0.0F;
                if (w <= 0.0F || h <= 0.0F) { w = img->texture_size.x; h = img->texture_size.y; }
                if (w <= 0.0F || h <= 0.0F) continue;
                const std::string resolved = eng.scoped_texture_ref(idx, img->texture);
                auto ti = eng.tex_cache.find(resolved);   // 与精灵共用 2D 贴图缓存（含 rendertexture://）
                if (ti == eng.tex_cache.end() || ti->second < 0) continue;
                const float scale = e.world_scale > 0.0F ? e.world_scale : 1.0F;
                /* UV 子矩形：region = (u0,v0,u1,v1) 归一化；退化 → 整张贴图。 */
                float u0 = img->region.x, v0 = img->region.y;
                float u1 = img->region.z, v1 = img->region.w;
                if (!(u1 > u0 && v1 > v0)) { u0 = 0.0F; v0 = 0.0F; u1 = 1.0F; v1 = 1.0F; }
                mq_renderer_item base{};
                base.x = e.world_x + w * 0.5F * scale;
                base.y = (float)eng.H - e.world_y - h * 0.5F * scale;
                base.half_w = w * 0.5F * scale;
                base.half_h = h * 0.5F * scale;
                base.size = item_extent(base.half_w, base.half_h, 0.0F);
                base.r = img->modulate.x; base.g = img->modulate.y;
                base.b = img->modulate.z; base.a = img->modulate.w * e.world_opacity;
                base.texture = ti->second;
                base.filter = img->texture.find("filter=nearest") != std::string::npos ? 1 : 0;
                base.snap = 0;
                base.rotation = 0.0F;                       /* ui.Image 暂不支持旋转（轴心是节点左下角） */
                base.layer = 1;
                base.z = ctl ? ctl->z_index : e.z_index;    /* 默认 0 = 文字（z=100+）之下 */
                base.mask = self_mask;

                const float tex_w = img->texture_size.x, tex_h = img->texture_size.y;
                const bool nine = (img->nine_patch_margin.x > 0.0F || img->nine_patch_margin.y > 0.0F ||
                                   img->nine_patch_margin.z > 0.0F || img->nine_patch_margin.w > 0.0F) &&
                                  tex_w > 0.0F && tex_h > 0.0F;
                if (nine) {
                    NinePatchParams np{};
                    np.dst_w = w; np.dst_h = h;
                    np.tex_w = tex_w; np.tex_h = tex_h;
                    np.src_x = u0 * tex_w; np.src_y = v0 * tex_h;
                    np.src_w = (u1 - u0) * tex_w; np.src_h = (v1 - v0) * tex_h;
                    np.margin_l = img->nine_patch_margin.x;   /* {左,上,右,下} 源图像素 */
                    np.margin_t = img->nine_patch_margin.y;
                    np.margin_r = img->nine_patch_margin.z;
                    np.margin_b = img->nine_patch_margin.w;
                    np.flip_h = img->flip_h; np.flip_v = img->flip_v;
                    const std::vector<NinePatchQuad> quads = nine_patch_quads(np);
                    for (const NinePatchQuad& q : quads) {
                        mq_renderer_item it = base;
                        it.x = e.world_x + (w * 0.5F + q.cx) * scale;
                        it.y = (float)eng.H - e.world_y - h * 0.5F * scale + q.cy * scale;
                        it.half_w = q.hw * scale;
                        it.half_h = q.hh * scale;
                        it.size = item_extent(it.half_w, it.half_h, 0.0F);
                        it.uv0 = q.u0; it.uv1 = q.v0; it.uv2 = q.u1; it.uv3 = q.v1;
                        emit_with_clip(out, it, self_clip, (float)eng.H);
                    }
                    continue;
                }
                if (img->flip_h) { const float t = u0; u0 = u1; u1 = t; }
                if (img->flip_v) { const float t = v0; v0 = v1; v1 = t; }
                base.uv0 = u0; base.uv1 = v0; base.uv2 = u1; base.uv3 = v1;
                emit_with_clip(out, base, self_clip, (float)eng.H);
                continue;
            }
            /* ListView 滚动条：轨道 + 滑块（solid 图元，layer=1，z 高于文字）。
             * 滑块高度按“视口/内容”比例，位置按 scroll_offset；只在内容超过视口时出现。 */
            if (const auto* lv = as_component<ui::ListView>(d, blob.data())) {
                const auto* ctl = component<ui::Control>(e);
                if (ctl) {
                    const float s = e.world_scale > 0.0F ? e.world_scale : 1.0F;
                    const float vw = ctl->offset_max.x * s, vh = ctl->offset_max.y * s;
                    const ListBarGeometry bar = list_scrollbar_geometry(*lv, e.world_x, e.world_y, vw, vh, s);
                    if (bar.visible) {
                        /* 视口内裁剪（再与父 Mask/ListView 裁剪求交）。 */
                        ClipSet clip = self_clip;
                        const ClipRect vr{ e.world_x, e.world_y, e.world_x + vw, e.world_y + vh };
                        clip = clip_apply(clip, vr, false, (float)eng.W, (float)eng.H);
                        auto solid = [&](float cx_, float cy_, float hw, float hh,
                                         const math::Vec4& c, int z) {
                            mq_renderer_item it{};
                            it.x = cx_; it.y = cy_;
                            it.half_w = hw; it.half_h = hh;
                            it.size = item_extent(hw, hh, 0.0F);
                            it.r = c.x; it.g = c.y; it.b = c.z;
                            it.a = c.w * e.world_opacity;
                            it.texture = -1; it.layer = 1; it.z = z;
                            it.mask = self_mask;
                            emit_with_clip(out, it, clip, (float)eng.H);
                        };
                        const float cx = (bar.x0 + bar.x1) * 0.5F;
                        const float track_cy = (float)eng.H - (bar.y0 + bar.y1) * 0.5F;
                        const float thumb_cy = (float)eng.H - (bar.thumb_y0 + bar.thumb_y1) * 0.5F;
                        solid(cx, track_cy, (bar.x1 - bar.x0) * 0.5F, (bar.y1 - bar.y0) * 0.5F,
                              lv->scrollbar_track_color, 800);
                        solid(cx, thumb_cy, (bar.x1 - bar.x0) * 0.5F, bar.thumb_h * 0.5F,
                              lv->scrollbar_color, 801);
                        if (lv->show_scroll_percent) {
                            /* 百分比数字由 ui_step 生成的 label 画；这里只铺一个暗底保证可读。 */
                            const float pill_cx = e.world_x + vw - 64.0F;
                            const float pill_cy = (float)eng.H - (e.world_y + 15.0F);
                            solid(pill_cx, pill_cy, 20.0F, 8.0F,
                                  lv->scrollbar_track_color, 899);
                        }
                    }
                }
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
            it.mask = self_mask;
            /* 节点 z_index 必须参与排序（docs/bugs/013）：此前只写实体 id，
             * 于是 scn.Sprite 图层永远按创建顺序画、z_index 静默无效 —— 树无法盖住坦克。
             * 与 UI/Spine 用同一套 "z_index*100000+id" 稳定键（同 z 时仍按 id，禁指针序）。 */
            it.z = e.z_index * 100000 + (int)idx;
            emit_with_clip(out, it, self_clip, (float)eng.H);
        }

        for (int k : e.children) walk(k, child_clip, child_mask);
    };

    for (int r : roots) walk(r, ClipSet{}, 0);
}

}  // namespace maque::rt
