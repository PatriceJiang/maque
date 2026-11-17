/* DragonBones 5.x 2D 运行时（docs/36）
 *
 * 分层（与 spine2d 完全同构，docs/27 §架构）：
 *   *_ske.json + *_tex.json ──► 本模块解析（骨架/皮肤/附件/时间轴）
 *   图集页纹理 ──► 引擎侧读文件 + image 解码 + gfx 上传 → 回调回填句柄/像素尺寸
 *   每帧 step(dt) → 求值姿势（骨骼世界矩阵 + 槽显示/颜色）→ draw() 导出顶点/UV/颜色/索引/批次
 *   纹理与 GPU 全在引擎侧：本模块**不依赖** gfx/image（docs/02 模块分层）。
 *
 * 事实来源（都已核对，不要凭印象改）：
 *   · 格式：官方 `DragonBones/Tools/doc/dragonbones_json_format_5.5.md`
 *   · 语义：DragonBonesJS（master）的 ObjectDataParser / BaseTimelineState / Bone / Slot / Transform
 *
 * 关键语义（每条都在实现里落地）：
 *   1) **y 向下**（flash 系）：骨骼/顶点的 y 正方向是屏幕下方，与引擎 2D 渲染目标一致，
 *      所以导出顶点**不翻 y**（spine 是 y 向上，必须翻，见 docs/27）。
 *   2) `skX`/`skY`（度）：rotation = skY，skew = skX - skY；两者都 normalize 到 (-π, π]。
 *   3) 骨骼动画帧是**相对 setup 姿势的增量**：x/y 相加、rotation/skew 相加、scale 相乘。
 *   4) 缺失 `tweenEasing`（或 null）= 不补间（step）；0 = 线性；<0 = QuadIn；0..1 = QuadOut；
 *      >1 = QuadInOut；`curve` = 三次贝塞尔（控制点序列 [x1,y1,x2,y2,...]）。
 *   5) 时间轴最后一帧的补间**回绕到第 0 帧**（循环闭合）；rotate 的差值在回绕时按最短路径规范化。
 *      导出器常在末尾放一个 `duration: 0` 的帧携带闭合值 —— 它是**下一帧**，不会成为当前帧。
 *   6) 矩阵约定：`Mat{a,b,c,d,tx,ty}`，行向量：p' = (a*x + c*y + tx, b*x + d*y + ty)；
 *      `concat(m, parent)` = m * parent（先子后父）。
 *   7) 图像附件的画法（官方 SFMLSlot/Slot）：quad 只覆盖**裁剪后的区域**（region.w × region.h），
 *      整块再减去 pivot（pivot 由**未裁剪帧**尺寸算出：pivotX = pivot.x * frameW + frameX）。
 *      局部坐标 = u - pivot，u ∈ [0, regionW] —— 裁剪量只进 pivot，不进几何。
 *      这一条与官方 SFMLSlot::_updateFrame / Slot::_updateTransform 一致。
 *
 * P0 明确未支持（runtime 会打印并计入诊断，不静默）：
 *   · 带权重网格（`weights`）：跳过该附件；`mq_db_data_weighted_count` 计数
 *   · 嵌套子骨架（display type "armature"）：跳过；`..._nested_count` 计数
 *   · 颜色 offset（rO/gO/bO/aO）：只乘 rM/gM/bM/aM（offset 需要着色器加法项）
 *   · IK 约束 / zOrder 时间轴 / 动画 action 事件 / 骨骼 inherit* 标志的完整语义
 *   · 二进制 .dbbin（只接受 JSON）
 */
#include "anim/mq_dragonbones.h"

#include "abi/persist.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace maque::db {

constexpr float kPi = 3.14159265358979323846F;

/* ---------------- 基础几何（与 DragonBonesJS geom/Transform|Matrix 同式） ---------------- */

struct Transform {
    float x = 0.0F, y = 0.0F;
    float skew = 0.0F, rotation = 0.0F;
    float scale_x = 1.0F, scale_y = 1.0F;
};

struct Mat {
    float a = 1.0F, b = 0.0F, c = 0.0F, d = 1.0F, tx = 0.0F, ty = 0.0F;
};

float normalize_radian(float value) {
    /* Transform.normalizeRadian：((v + PI) % 2PI) 后 +(-PI)/+PI → 结果落在 (-π, π] */
    value = std::fmod(value + kPi, 2.0F * kPi);
    value += value > 0.0F ? -kPi : kPi;
    return value;
}

Mat to_matrix(const Transform& t) {
    Mat m;
    if (t.rotation == 0.0F) { m.a = 1.0F; m.b = 0.0F; }
    else { m.a = std::cos(t.rotation); m.b = std::sin(t.rotation); }
    if (t.skew == 0.0F) { m.c = -m.b; m.d = m.a; }
    else { m.c = -std::sin(t.skew + t.rotation); m.d = std::cos(t.skew + t.rotation); }
    m.a *= t.scale_x; m.b *= t.scale_x;
    m.c *= t.scale_y; m.d *= t.scale_y;
    m.tx = t.x; m.ty = t.y;
    return m;
}

/* m = m * parent（行向量约定；先应用 m 再应用 parent） */
void concat(Mat& m, const Mat& v) {
    Mat r;
    r.a = m.a * v.a + m.b * v.c;
    r.b = m.b * v.d + m.a * v.b;
    r.c = m.c * v.a + m.d * v.c;
    r.d = m.d * v.d + m.c * v.b;
    r.tx = m.tx * v.a + m.ty * v.c + v.tx;
    r.ty = m.ty * v.d + m.tx * v.b + v.ty;
    m = r;
}

void apply(const Mat& m, float x, float y, float& ox, float& oy) {
    ox = m.a * x + m.c * y + m.tx;
    oy = m.b * x + m.d * y + m.ty;
}

/* ---------------- 数据模型 ---------------- */

struct ColorTransform {
    float a_m = 1.0F, r_m = 1.0F, g_m = 1.0F, b_m = 1.0F;
    float a_o = 0.0F, r_o = 0.0F, g_o = 0.0F, b_o = 0.0F;   // 解析保留；P0 不参与着色
};

struct Tween {
    enum Type { None = 0, Line = 1, QuadIn = 2, QuadOut = 3, QuadInOut = 4, Curve = 5 };
    int type = None;
    float easing = 1.0F;
    float curve[4] = { 0.0F, 0.0F, 1.0F, 1.0F };
};

struct TextureData {
    std::string name;
    int page = 0;
    float x = 0.0F, y = 0.0F, w = 0.0F, h = 0.0F;      // 图集内（旋转时宽高是**存放**尺寸）
    bool has_frame = false;
    float frame_x = 0.0F, frame_y = 0.0F, frame_w = 0.0F, frame_h = 0.0F;
    bool rotated = false;
};

struct PageData {
    std::string name;
    int width = 0;
    int height = 0;
    uint64_t texture = 0;
};

struct BoneData {
    std::string name;
    int parent = -1;
    Transform setup;
    bool inherit_translation = true;
    bool inherit_rotation = true;
    bool inherit_scale = true;
    bool inherit_reflection = false;
};

struct SlotData {
    std::string name;
    int bone = -1;
    int display_index = 0;
    int blend = MQ_DB_BLEND_NORMAL;
    ColorTransform color;
};

struct DisplayData {
    enum Kind { Image = 0, Mesh = 1, Armature = 2, BoundingBox = 3 };
    int kind = Image;
    std::string name;          // 显示名（= 图集子纹理名或共享网格名）
    int texture = -1;          // SkeletonData::textures 下标（-1 = 图集里找不到）
    Transform t;               // 相对槽骨骼的变换
    float pivot_x = 0.5F, pivot_y = 0.5F;
    std::vector<float> verts;  // mesh 顶点（局部，x,y）
    std::vector<float> uvs;    // mesh uv（归一化到**子纹理**区域，[0,1]）
    std::vector<uint16_t> tris;
    bool weighted = false;
};

struct SkinData {
    std::string name;
    std::vector<std::vector<DisplayData>> displays;   // per slot
};

struct Vec2Frame {
    float start = 0.0F, duration = 0.0F;
    float x = 0.0F, y = 0.0F;
    Tween tween;
};

struct RotFrame {
    float start = 0.0F, duration = 0.0F;
    float rotate = 0.0F, skew = 0.0F;
    Tween tween;
};

struct ScaleFrame {
    float start = 0.0F, duration = 0.0F;
    float x = 1.0F, y = 1.0F;
    Tween tween;
};

struct DisplayFrame {
    float start = 0.0F, duration = 0.0F;
    int value = 0;
};

struct ColorFrame {
    float start = 0.0F, duration = 0.0F;
    ColorTransform value;
    Tween tween;
};

struct FfdFrame {
    float start = 0.0F, duration = 0.0F;
    int offset = 0;
    std::vector<float> deltas;
    Tween tween;
};

struct BoneTimeline {
    int bone = -1;
    std::vector<Vec2Frame> translate;
    std::vector<RotFrame> rotate;
    std::vector<ScaleFrame> scale;
};

struct SlotTimeline {
    int slot = -1;
    std::vector<DisplayFrame> display;
    std::vector<ColorFrame> color;
};

struct FfdTimeline {
    int slot = -1;
    int display = -1;          // 该皮肤里这个槽的显示下标
    int skin = -1;             // -1 = 任何皮肤
    std::string name;          // 网格名（诊断）
    std::vector<FfdFrame> frames;
};

struct AnimationData {
    std::string name;
    int play_times = 1;
    float duration_frames = 1.0F;
    float duration = 0.0F;                                   // 秒
    std::vector<BoneTimeline> bones;
    std::vector<SlotTimeline> slots;
    std::vector<FfdTimeline> ffd;
};

struct ArmatureData {
    std::string name;
    float frame_rate = 24.0F;
    std::vector<BoneData> bones;
    std::vector<SlotData> slots;
    std::vector<int> bone_order;                    // 拓扑序（父在前）
    std::vector<SkinData> skins;
    std::vector<AnimationData> animations;
    bool needs_inherit_fallback = false;             // 有 inherit*=false 的骨骼（P0 近似）
};

struct SkeletonData {
    std::string name;
    float frame_rate = 24.0F;
    std::vector<ArmatureData> armatures;
    std::vector<PageData> pages;
    std::vector<TextureData> textures;
    std::unordered_map<std::string, int> texture_by_name;
    int mesh_count = 0;
    int weighted_count = 0;
    int nested_count = 0;
    int missing_texture_count = 0;
};

/* ---------------- JSON 小工具（只用 persist C ABI 的 mq_json_*） ---------------- */

const mq_json* jget(const mq_json* o, const char* k) { return o ? mq_json_get(o, k) : nullptr; }

int jint(const mq_json* o, const char* k, int def) {
    const mq_json* v = jget(o, k);
    return v ? (int)mq_json_num(v, (double)def) : def;
}

float jfloat(const mq_json* o, const char* k, float def) {
    const mq_json* v = jget(o, k);
    return v ? (float)mq_json_num(v, (double)def) : def;
}

std::string jstr(const mq_json* o, const char* k, const char* def = "") {
    const mq_json* v = jget(o, k);
    const char* s = v ? mq_json_str(v, nullptr) : nullptr;
    return s ? std::string(s) : std::string(def ? def : "");
}

bool jhas(const mq_json* o, const char* k) { return jget(o, k) != nullptr; }

int jlen(const mq_json* a) { return a ? (int)mq_json_len(a) : 0; }
const mq_json* jat(const mq_json* a, int i) { return a ? mq_json_at(a, (uint32_t)i) : nullptr; }

/* skX/skY（度）→ Transform；两者都 normalize（官方 _parseTransform） */
Transform jtransform(const mq_json* node, float scale) {
    Transform t;
    const mq_json* tr = node;
    if (!tr) return t;
    t.x = jfloat(tr, "x", 0.0F) * scale;
    t.y = jfloat(tr, "y", 0.0F) * scale;
    if (jhas(tr, "rotate") || jhas(tr, "skew")) {
        t.rotation = normalize_radian(jfloat(tr, "rotate", 0.0F) * kPi / 180.0F);
        t.skew = normalize_radian(jfloat(tr, "skew", 0.0F) * kPi / 180.0F);
    } else if (jhas(tr, "skX") || jhas(tr, "skY")) {
        t.rotation = normalize_radian(jfloat(tr, "skY", 0.0F) * kPi / 180.0F);
        t.skew = normalize_radian(jfloat(tr, "skX", 0.0F) * kPi / 180.0F) - t.rotation;
    }
    t.scale_x = jfloat(tr, "scX", 1.0F);
    t.scale_y = jfloat(tr, "scY", 1.0F);
    return t;
}

ColorTransform jcolor(const mq_json* node) {
    ColorTransform c;
    if (!node) return c;
    c.a_m = jfloat(node, "aM", 100.0F) * 0.01F;
    c.r_m = jfloat(node, "rM", 100.0F) * 0.01F;
    c.g_m = jfloat(node, "gM", 100.0F) * 0.01F;
    c.b_m = jfloat(node, "bM", 100.0F) * 0.01F;
    c.a_o = jfloat(node, "aO", 0.0F);
    c.r_o = jfloat(node, "rO", 0.0F);
    c.g_o = jfloat(node, "gO", 0.0F);
    c.b_o = jfloat(node, "bO", 0.0F);
    return c;
}

/* 补间解析（官方 ObjectDataParser::_parseTweenFrame）：
 *   curve 存在 → 贝塞尔采样；否则 tweenEasing 缺失/null → None；0 → Line；
 *   <0 → QuadIn（easing=-v）；(0,1] → QuadOut；>1 → QuadInOut（easing=v-1）。 */
Tween jtween(const mq_json* frame) {
    Tween tw;
    const mq_json* curve = jget(frame, "curve");
    if (curve && mq_json_len(curve) >= 4) {
        tw.type = Tween::Curve;
        for (int i = 0; i < 4; ++i) tw.curve[i] = (float)mq_json_num(mq_json_at(curve, (uint32_t)i), 0.0);
        if (tw.curve[2] == 0.0F && tw.curve[3] == 0.0F) { tw.curve[2] = 1.0F; tw.curve[3] = 1.0F; }
        return tw;
    }
    const mq_json* easing = jget(frame, "tweenEasing");
    const float v = easing ? (float)mq_json_num(easing, -2.0) : -2.0F;
    if (v == -2.0F) { tw.type = Tween::None; return tw; }
    if (v == 0.0F) { tw.type = Tween::Line; return tw; }
    if (v < 0.0F) { tw.type = Tween::QuadIn; tw.easing = -v; return tw; }
    if (v <= 1.0F) { tw.type = Tween::QuadOut; tw.easing = v; return tw; }
    tw.type = Tween::QuadInOut;
    tw.easing = v - 1.0F;
    return tw;
}

/* ---------------- 解析：图集 ---------------- */

int parse_page(SkeletonData& d, const mq_json* root, mq_db_texture_cb cb, void* user) {
    if (!root) return MQ_DB_ERR_FORMAT;
    PageData page;
    page.name = jstr(root, "imagePath");
    if (page.name.empty()) page.name = jstr(root, "name");
    page.width = jint(root, "width", 0);
    page.height = jint(root, "height", 0);

    const mq_json* subs = jget(root, "SubTexture");
    if (!subs) return MQ_DB_ERR_FORMAT;
    const int n = jlen(subs);
    for (int i = 0; i < n; ++i) {
        const mq_json* s = jat(subs, i);
        if (!s) continue;
        TextureData t;
        t.name = jstr(s, "name");
        t.page = 0;
        t.x = jfloat(s, "x", 0.0F);
        t.y = jfloat(s, "y", 0.0F);
        t.w = jfloat(s, "width", 0.0F);
        t.h = jfloat(s, "height", 0.0F);
        t.rotated = jint(s, "rotated", 0) != 0;
        if (jhas(s, "frameWidth")) {
            t.has_frame = true;
            t.frame_x = jfloat(s, "frameX", 0.0F);
            t.frame_y = jfloat(s, "frameY", 0.0F);
            t.frame_w = jfloat(s, "frameWidth", t.w);
            t.frame_h = jfloat(s, "frameHeight", t.h);
        }
        if (t.name.empty() || t.w <= 0.0F || t.h <= 0.0F) continue;
        const int idx = (int)d.textures.size();
        d.texture_by_name.emplace(t.name, idx);
        d.textures.push_back(std::move(t));
    }
    /* 图集 json 可能不写 width/height（如官方 weapon_1000）→ 用图片真实尺寸（回调给出）兜底，
     * 再不行按子纹理的最大边界估算，保证 UV 归一化不会除 0。 */
    if (cb) {
        uint64_t tex = 0;
        int w = page.width, h = page.height;
        if (cb(user, page.name.c_str(), &tex, &w, &h) == 0) {
            page.texture = tex;
            if (w > 0) page.width = w;
            if (h > 0) page.height = h;
        }
    }
    if (page.width <= 0 || page.height <= 0) {
        float max_x = 0.0F, max_y = 0.0F;
        for (const TextureData& t : d.textures) {
            max_x = std::max(max_x, t.x + t.w);
            max_y = std::max(max_y, t.y + t.h);
        }
        if (page.width <= 0) page.width = (int)std::ceil(max_x);
        if (page.height <= 0) page.height = (int)std::ceil(max_y);
        if (page.width <= 0) page.width = 1;
        if (page.height <= 0) page.height = 1;
    }
    d.pages.push_back(std::move(page));
    return MQ_DB_OK;
}

/* ---------------- 解析：皮肤 / 显示 ---------------- */

DisplayData parse_display(const SkeletonData& d, const mq_json* disp, float scale) {
    DisplayData out;
    const std::string type = jstr(disp, "type", "image");
    if (type == "mesh") out.kind = DisplayData::Mesh;
    else if (type == "armature") out.kind = DisplayData::Armature;
    else if (type == "boundingBox") out.kind = DisplayData::BoundingBox;
    else out.kind = DisplayData::Image;

    out.name = jstr(disp, "path");
    if (out.name.empty()) out.name = jstr(disp, "name");
    out.t = jtransform(jget(disp, "transform"), scale);
    const mq_json* pivot = jget(disp, "pivot");
    if (pivot) {
        out.pivot_x = jfloat(pivot, "x", 0.5F);
        out.pivot_y = jfloat(pivot, "y", 0.5F);
    }
    if (out.kind == DisplayData::Image || out.kind == DisplayData::Mesh) {
        auto it = d.texture_by_name.find(out.name);
        out.texture = (it != d.texture_by_name.end()) ? it->second : -1;
    }
    if (out.kind == DisplayData::Mesh) {
        const mq_json* verts = jget(disp, "vertices");
        const mq_json* uvs = jget(disp, "uvs");
        const mq_json* tris = jget(disp, "triangles");
        const mq_json* weights = jget(disp, "weights");
        const int vn = jlen(verts);
        out.verts.resize((size_t)vn);
        for (int i = 0; i < vn; ++i) out.verts[(size_t)i] = (float)mq_json_num(jat(verts, i), 0.0);
        const int un = jlen(uvs);
        out.uvs.resize((size_t)un);
        for (int i = 0; i < un; ++i) out.uvs[(size_t)i] = (float)mq_json_num(jat(uvs, i), 0.0);
        const int tn = jlen(tris);
        out.tris.resize((size_t)tn);
        for (int i = 0; i < tn; ++i) out.tris[(size_t)i] = (uint16_t)mq_json_num(jat(tris, i), 0.0);
        /* weights 布局：[boneCount, boneIndex, weight, ...] 每顶点一段；有它 = 蒙皮网格 */
        out.weighted = weights && mq_json_len(weights) > 0;
    }
    return out;
}

/* 皮肤槽：display 数组；同名槽在多个皮肤里各有一份（P0 用当前皮肤那份，不合并）。 */
int parse_skin(ArmatureData& arm, const SkeletonData& d, const mq_json* skin_node, float scale) {
    SkinData skin;
    skin.name = jstr(skin_node, "name");
    skin.displays.resize(arm.slots.size());
    const mq_json* slot_arr = jget(skin_node, "slot");
    const int sn = jlen(slot_arr);
    for (int si = 0; si < sn; ++si) {
        const mq_json* s = jat(slot_arr, si);
        if (!s) continue;
        const std::string slot_name = jstr(s, "name");
        int slot_index = -1;
        for (size_t k = 0; k < arm.slots.size(); ++k)
            if (arm.slots[k].name == slot_name) { slot_index = (int)k; break; }
        if (slot_index < 0) continue;
        const mq_json* disp_arr = jget(s, "display");
        const int dn = jlen(disp_arr);
        for (int di = 0; di < dn; ++di) {
            const mq_json* disp = jat(disp_arr, di);
            if (!disp) continue;
            skin.displays[(size_t)slot_index].push_back(parse_display(d, disp, scale));
        }
    }
    arm.skins.push_back(std::move(skin));
    return MQ_DB_OK;
}

/* ---------------- 解析：时间轴 ---------------- */

struct FrameClock {
    float fps = 24.0F;
    float start = 0.0F;                 // 已累计的帧数
    float to_seconds(float frames) const { return frames / (fps > 0.0F ? fps : 24.0F); }
};

void parse_vec2_frames(const mq_json* arr, FrameClock clock, std::vector<Vec2Frame>& out) {
    const int n = jlen(arr);
    out.reserve((size_t)n);
    float start = 0.0F;
    for (int i = 0; i < n; ++i) {
        const mq_json* f = jat(arr, i);
        if (!f) continue;
        Vec2Frame fr;
        const float dur = jfloat(f, "duration", 1.0F);
        fr.start = clock.to_seconds(start);
        fr.duration = clock.to_seconds(dur);
        fr.x = jfloat(f, "x", 0.0F);
        fr.y = jfloat(f, "y", 0.0F);
        fr.tween = jtween(f);
        start += dur;
        out.push_back(std::move(fr));
    }
}

void parse_rot_frames(const mq_json* arr, FrameClock clock, std::vector<RotFrame>& out) {
    const int n = jlen(arr);
    out.reserve((size_t)n);
    float start = 0.0F;
    float prev_clockwise = 0.0F;
    float prev_rotation = 0.0F;
    for (int i = 0; i < n; ++i) {
        const mq_json* f = jat(arr, i);
        if (!f) continue;
        RotFrame fr;
        const float dur = jfloat(f, "duration", 1.0F);
        fr.start = clock.to_seconds(start);
        fr.duration = clock.to_seconds(dur);
        /* ⚠️ 动画帧的 `rotate`/`skew` 是**角度制**（`* DEG_RAD`；官方 _parseBoneRotateFrame），
         * 与 setup 的 transform 同一单位。按弧度读会让每根骨骼的旋转差 ~57 倍 —— 实测表现为
         * "关节朝反方向甩 / 动画整体扭曲"（docs/36 §5.3）。 */
        fr.rotate = jfloat(f, "rotate", 0.0F) * kPi / 180.0F;
        fr.skew = jfloat(f, "skew", 0.0F) * kPi / 180.0F;
        fr.tween = jtween(f);
        /* clockwise 语义（官方 _parseBoneRotateFrame）：把跨圈旋转摊平成连续值（弧度域） */
        if (fr.start > 0.0F) {
            if (prev_clockwise == 0.0F) {
                fr.rotate = prev_rotation + normalize_radian(fr.rotate - prev_rotation);
            } else {
                if (prev_clockwise > 0.0F ? fr.rotate >= prev_rotation : fr.rotate <= prev_rotation)
                    prev_clockwise += prev_clockwise > 0.0F ? -1.0F : 1.0F;
                fr.rotate = fr.rotate + 2.0F * kPi * prev_clockwise;
            }
        }
        prev_clockwise = jfloat(f, "clockwise", 0.0F);
        prev_rotation = fr.rotate;
        start += dur;
        out.push_back(std::move(fr));
    }
}

void parse_scale_frames(const mq_json* arr, FrameClock clock, std::vector<ScaleFrame>& out) {
    const int n = jlen(arr);
    out.reserve((size_t)n);
    float start = 0.0F;
    for (int i = 0; i < n; ++i) {
        const mq_json* f = jat(arr, i);
        if (!f) continue;
        ScaleFrame fr;
        const float dur = jfloat(f, "duration", 1.0F);
        fr.start = clock.to_seconds(start);
        fr.duration = clock.to_seconds(dur);
        fr.x = jfloat(f, "x", 1.0F);
        fr.y = jfloat(f, "y", 1.0F);
        fr.tween = jtween(f);
        start += dur;
        out.push_back(std::move(fr));
    }
}

void parse_display_frames(const mq_json* arr, FrameClock clock, std::vector<DisplayFrame>& out) {
    const int n = jlen(arr);
    out.reserve((size_t)n);
    float start = 0.0F;
    for (int i = 0; i < n; ++i) {
        const mq_json* f = jat(arr, i);
        if (!f) continue;
        DisplayFrame fr;
        const float dur = jfloat(f, "duration", 1.0F);
        fr.start = clock.to_seconds(start);
        fr.duration = clock.to_seconds(dur);
        fr.value = jint(f, "value", 0);
        start += dur;
        out.push_back(fr);
    }
}

void parse_color_frames(const mq_json* arr, FrameClock clock, std::vector<ColorFrame>& out) {
    const int n = jlen(arr);
    out.reserve((size_t)n);
    float start = 0.0F;
    for (int i = 0; i < n; ++i) {
        const mq_json* f = jat(arr, i);
        if (!f) continue;
        ColorFrame fr;
        const float dur = jfloat(f, "duration", 1.0F);
        fr.start = clock.to_seconds(start);
        fr.duration = clock.to_seconds(dur);
        fr.value = jcolor(jget(f, "value"));
        fr.tween = jtween(f);
        start += dur;
        out.push_back(std::move(fr));
    }
}

int find_slot(const ArmatureData& arm, const std::string& name) {
    for (size_t i = 0; i < arm.slots.size(); ++i)
        if (arm.slots[i].name == name) return (int)i;
    return -1;
}

int find_bone(const ArmatureData& arm, const std::string& name) {
    for (size_t i = 0; i < arm.bones.size(); ++i)
        if (arm.bones[i].name == name) return (int)i;
    return -1;
}

void parse_animation(ArmatureData& arm, const mq_json* a, float default_fps) {
    AnimationData an;
    an.name = jstr(a, "name");
    an.play_times = jint(a, "playTimes", 1);
    an.duration_frames = jfloat(a, "duration", 1.0F);
    const float fps = arm.frame_rate > 0.0F ? arm.frame_rate : default_fps;
    FrameClock clock{ fps, 0.0F };
    an.duration = clock.to_seconds(an.duration_frames);

    const mq_json* bones = jget(a, "bone");
    for (int i = 0; i < jlen(bones); ++i) {
        const mq_json* b = jat(bones, i);
        if (!b) continue;
        BoneTimeline bt;
        bt.bone = find_bone(arm, jstr(b, "name"));
        if (bt.bone < 0) continue;
        parse_vec2_frames(jget(b, "translateFrame"), clock, bt.translate);
        parse_rot_frames(jget(b, "rotateFrame"), clock, bt.rotate);
        parse_scale_frames(jget(b, "scaleFrame"), clock, bt.scale);
        if (!bt.translate.empty() || !bt.rotate.empty() || !bt.scale.empty())
            an.bones.push_back(std::move(bt));
    }
    const mq_json* slots = jget(a, "slot");
    for (int i = 0; i < jlen(slots); ++i) {
        const mq_json* s = jat(slots, i);
        if (!s) continue;
        SlotTimeline st;
        st.slot = find_slot(arm, jstr(s, "name"));
        if (st.slot < 0) continue;
        parse_display_frames(jget(s, "displayFrame"), clock, st.display);
        parse_color_frames(jget(s, "colorFrame"), clock, st.color);
        if (!st.display.empty() || !st.color.empty()) an.slots.push_back(std::move(st));
    }
    /* FFD（网格顶点变形）：P0 只对**非蒙皮网格**生效 */
    const mq_json* ffd = jget(a, "ffd");
    for (int i = 0; i < jlen(ffd); ++i) {
        const mq_json* t = jat(ffd, i);
        if (!t) continue;
        FfdTimeline ffd_tl;
        ffd_tl.name = jstr(t, "name");
        ffd_tl.slot = find_slot(arm, jstr(t, "slot"));
        const std::string skin_name = jstr(t, "skin");
        if (ffd_tl.slot < 0) continue;
        /* 皮肤：空名 = 默认皮肤（第 0 个） */
        ffd_tl.skin = 0;
        for (size_t k = 0; k < arm.skins.size(); ++k)
            if (arm.skins[k].name == skin_name) { ffd_tl.skin = (int)k; break; }
        /* 显示下标：在该槽的候选显示里按名字找 */
        const SkinData* skin = (ffd_tl.skin >= 0 && ffd_tl.skin < (int)arm.skins.size())
                                   ? &arm.skins[(size_t)ffd_tl.skin] : nullptr;
        if (!skin || (size_t)ffd_tl.slot >= skin->displays.size()) continue;
        const auto& list = skin->displays[(size_t)ffd_tl.slot];
        for (size_t k = 0; k < list.size(); ++k)
            if (list[k].name == ffd_tl.name) { ffd_tl.display = (int)k; break; }
        if (ffd_tl.display < 0) continue;
        const mq_json* frames = jget(t, "frame");
        float start = 0.0F;
        for (int fi = 0; fi < jlen(frames); ++fi) {
            const mq_json* f = jat(frames, fi);
            if (!f) continue;
            FfdFrame fr;
            const float dur = jfloat(f, "duration", 1.0F);
            fr.start = clock.to_seconds(start);
            fr.duration = clock.to_seconds(dur);
            fr.offset = jint(f, "offset", 0);
            const mq_json* vs = jget(f, "vertices");
            fr.deltas.resize((size_t)jlen(vs));
            for (int k = 0; k < jlen(vs); ++k) fr.deltas[(size_t)k] = (float)mq_json_num(jat(vs, k), 0.0);
            fr.tween = jtween(f);
            start += dur;
            ffd_tl.frames.push_back(std::move(fr));
        }
        if (!ffd_tl.frames.empty()) an.ffd.push_back(std::move(ffd_tl));
    }
    arm.animations.push_back(std::move(an));
}

/* 骨骼拓扑序：父在子前（导出器通常已排序，但不能依赖）。 */
void build_bone_order(ArmatureData& arm) {
    const int n = (int)arm.bones.size();
    std::vector<char> done((size_t)n, 0);
    arm.bone_order.clear();
    arm.bone_order.reserve((size_t)n);
    for (int guard = 0; guard < n + 1 && (int)arm.bone_order.size() < n; ++guard) {
        for (int i = 0; i < n; ++i) {
            if (done[(size_t)i]) continue;
            const int p = arm.bones[(size_t)i].parent;
            if (p < 0 || done[(size_t)p]) {
                done[(size_t)i] = 1;
                arm.bone_order.push_back(i);
            }
        }
    }
    for (int i = 0; i < n; ++i)                 // 兜底（父环/坏数据）：保证每个骨骼都参与
        if (!done[(size_t)i]) arm.bone_order.push_back(i);
}

int parse_armature(SkeletonData& d, const mq_json* node) {
    ArmatureData arm;
    arm.name = jstr(node, "name");
    arm.frame_rate = jfloat(node, "frameRate", d.frame_rate);
    const float scale = 1.0F;                  // 数据级 scale（DragonBonesData.scale）P0 未支持

    const mq_json* bones = jget(node, "bone");
    for (int i = 0; i < jlen(bones); ++i) {
        const mq_json* b = jat(bones, i);
        if (!b) continue;
        BoneData bd;
        bd.name = jstr(b, "name");
        bd.setup = jtransform(jget(b, "transform"), scale);
        bd.inherit_translation = jint(b, "inheritTranslation", 1) != 0;
        bd.inherit_rotation = jint(b, "inheritRotation", 1) != 0;
        bd.inherit_scale = jint(b, "inheritScale", 1) != 0;
        bd.inherit_reflection = jint(b, "inheritReflection", 0) != 0;
        if (!bd.inherit_translation || !bd.inherit_rotation || !bd.inherit_scale)
            arm.needs_inherit_fallback = true;
        arm.bones.push_back(std::move(bd));
    }
    for (int i = 0; i < jlen(bones); ++i) {
        const mq_json* b = jat(bones, i);
        if (!b) continue;
        const std::string parent = jstr(b, "parent");
        if (!parent.empty()) arm.bones[(size_t)i].parent = find_bone(arm, parent);
    }
    build_bone_order(arm);

    const mq_json* slots = jget(node, "slot");
    for (int i = 0; i < jlen(slots); ++i) {
        const mq_json* s = jat(slots, i);
        if (!s) continue;
        SlotData sd;
        sd.name = jstr(s, "name");
        sd.bone = find_bone(arm, jstr(s, "parent"));
        sd.display_index = jint(s, "displayIndex", 0);
        const std::string blend = jstr(s, "blendMode");
        if (blend == "add" || blend == "additive") sd.blend = MQ_DB_BLEND_ADD;
        else if (blend == "multiply") sd.blend = MQ_DB_BLEND_MULTIPLY;
        else if (blend == "screen") sd.blend = MQ_DB_BLEND_SCREEN;
        else sd.blend = MQ_DB_BLEND_NORMAL;
        sd.color = jcolor(jget(s, "color"));
        arm.slots.push_back(std::move(sd));
    }

    const mq_json* skins = jget(node, "skin");
    for (int i = 0; i < jlen(skins); ++i) {
        const mq_json* s = jat(skins, i);
        if (s) parse_skin(arm, d, s, scale);
    }
    if (arm.skins.empty()) {                       // 没有皮肤 → 建一个空的（槽全空，不崩）
        SkinData empty_skin;
        empty_skin.displays.resize(arm.slots.size());
        arm.skins.push_back(std::move(empty_skin));
    }

    const mq_json* anims = jget(node, "animation");
    for (int i = 0; i < jlen(anims); ++i) {
        const mq_json* a = jat(anims, i);
        if (a) parse_animation(arm, a, d.frame_rate);
    }

    /* 诊断计数（跨全部皮肤） */
    for (const auto& skin : arm.skins) {
        for (const auto& list : skin.displays) {
            for (const auto& disp : list) {
                if (disp.kind == DisplayData::Mesh) {
                    ++d.mesh_count;
                    if (disp.weighted) ++d.weighted_count;
                } else if (disp.kind == DisplayData::Armature) {
                    ++d.nested_count;
                } else if (disp.kind == DisplayData::Image && disp.texture < 0) {
                    ++d.missing_texture_count;
                }
            }
        }
    }
    d.armatures.push_back(std::move(arm));
    return MQ_DB_OK;
}

int parse_skeleton(SkeletonData& d, const char* text, size_t len) {
    mq_json* root = mq_json_parse(text, len);
    if (!root) return MQ_DB_ERR_FORMAT;
    struct Guard { mq_json* j; ~Guard() { mq_json_free(j); } } guard{ root };

    d.name = jstr(root, "name");
    d.frame_rate = jfloat(root, "frameRate", 24.0F);
    if (d.frame_rate <= 0.0F) d.frame_rate = 24.0F;
    const mq_json* arms = jget(root, "armature");
    if (!arms || jlen(arms) == 0) return MQ_DB_ERR_FORMAT;
    for (int i = 0; i < jlen(arms); ++i) {
        const mq_json* a = jat(arms, i);
        if (a) parse_armature(d, a);
    }
    if (d.armatures.empty()) return MQ_DB_ERR_FORMAT;
    return MQ_DB_OK;
}

/* ---------------- 采样 ---------------- */

float bezier_y(const float c[4], float x) {
    if (x <= 0.0F) return 0.0F;
    if (x >= 1.0F) return 1.0F;
    float lo = 0.0F, hi = 1.0F, t = x;
    for (int i = 0; i < 20; ++i) {
        const float mt = 1.0F - t;
        const float xt = 3.0F * mt * mt * t * c[0] + 3.0F * mt * t * t * c[2] + t * t * t;
        if (xt < x) lo = t; else hi = t;
        t = 0.5F * (lo + hi);
    }
    const float mt = 1.0F - t;
    return 3.0F * mt * mt * t * c[1] + 3.0F * mt * t * t * c[3] + t * t * t;
}

float ease(const Tween& tw, float p) {
    switch (tw.type) {
    case Tween::Line: return p;
    case Tween::QuadIn: { const float v = p * p; return (v - p) * tw.easing + p; }
    case Tween::QuadOut: { const float v = 1.0F - (1.0F - p) * (1.0F - p); return (v - p) * tw.easing + p; }
    case Tween::QuadInOut: { const float v = 0.5F * (1.0F - std::cos(p * kPi)); return (v - p) * tw.easing + p; }
    case Tween::Curve: return bezier_y(tw.curve, p);
    default: return p;
    }
}

/* 当前帧：**最后一个 duration>0 且 start<=t** 的帧（末尾 duration=0 的帧只当"下一帧"用）。 */
template <class F>
int find_frame(const std::vector<F>& frames, float t) {
    int cur = 0;
    for (int i = 0; i < (int)frames.size(); ++i) {
        if (frames[i].duration <= 0.0F) continue;
        if (t + 1e-6F >= frames[i].start) cur = i;
        else break;
    }
    return cur;
}

/* 通用补间采样：interp(a, b, w, wrapped)。wrapped=1 表示 b 是回绕到第 0 帧（rotate 需最短路径）。 */
template <class F, class Interp>
void sample_frames(const std::vector<F>& frames, float t, Interp&& interp) {
    if (frames.empty()) return;
    const int n = (int)frames.size();
    const int cur = find_frame(frames, t);
    const F& a = frames[(size_t)cur];
    const int next = (cur + 1 < n) ? (cur + 1) : 0;
    const bool wrapped = (cur + 1 >= n);
    float w = 0.0F;
    const bool tween = a.tween.type != Tween::None && a.duration > 0.0F && n > 1;
    if (tween) {
        w = (t - a.start) / a.duration;
        if (w < 0.0F) w = 0.0F;
        if (w > 1.0F) w = 1.0F;
        w = ease(a.tween, w);
        interp(a, frames[(size_t)next], w, wrapped);
    } else {
        interp(a, a, 0.0F, false);
    }
}

/* ---------------- 实例 ---------------- */

struct Pose {
    float x = 0.0F, y = 0.0F, rot = 0.0F, skew = 0.0F, sx = 1.0F, sy = 1.0F;
};

struct SlotState {
    int display = 0;
    ColorTransform color;
};

}  // namespace maque::db

struct mq_db_data {
    maque::db::SkeletonData d;
};

struct mq_db_instance {
    mq_db_data* owner = nullptr;
    maque::db::ArmatureData* arm = nullptr;
    int skin = 0;
    int anim = -1;
    float time = 0.0F;
    bool playing = true;
    bool loop = true;
    bool complete = false;
    int play_times = 0;
    float scale = 1.0F;
    bool flip_x = false, flip_y = false;
    float tint[4] = { 1.0F, 1.0F, 1.0F, 1.0F };
    std::vector<maque::db::Pose> bones;
    std::vector<maque::db::Mat> bone_mat;      // 骨骼世界矩阵（armature 空间）
    std::vector<float> bone_rot;               // 骨骼绝对旋转（inheritRotation 语义用）
    std::vector<maque::db::SlotState> slots;
    std::vector<float> deform;                 // FFD 顶点缓冲（x,y 交错）
};

namespace maque::db {

void evaluate(mq_db_instance& in) {
    const ArmatureData& arm = *in.arm;
    const size_t nb = arm.bones.size();
    in.bones.assign(nb, Pose{});
    in.bone_mat.assign(nb, Mat{});
    in.bone_rot.assign(nb, 0.0F);
    in.slots.resize(arm.slots.size());
    for (size_t i = 0; i < arm.slots.size(); ++i) {
        in.slots[i].display = arm.slots[i].display_index;
        in.slots[i].color = arm.slots[i].color;
    }

    if (in.anim >= 0 && in.anim < (int)arm.animations.size()) {
        const AnimationData& a = arm.animations[(size_t)in.anim];
        const float t = in.time;
        for (const BoneTimeline& bt : a.bones) {
            if (bt.bone < 0 || (size_t)bt.bone >= nb) continue;
            Pose& p = in.bones[(size_t)bt.bone];
            sample_frames(bt.translate, t, [&p](const Vec2Frame& x, const Vec2Frame& y, float w, bool) {
                p.x = x.x + (y.x - x.x) * w;
                p.y = x.y + (y.y - x.y) * w;
            });
            sample_frames(bt.rotate, t, [&p](const RotFrame& x, const RotFrame& y, float w, bool wrapped) {
                float dr = y.rotate - x.rotate;
                float ds = y.skew - x.skew;
                if (wrapped) { dr = normalize_radian(dr); ds = normalize_radian(ds); }
                p.rot = x.rotate + dr * w;
                p.skew = x.skew + ds * w;
            });
            sample_frames(bt.scale, t, [&p](const ScaleFrame& x, const ScaleFrame& y, float w, bool) {
                p.sx = x.x + (y.x - x.x) * w;
                p.sy = x.y + (y.y - x.y) * w;
            });
        }
        for (const SlotTimeline& st : a.slots) {
            if (st.slot < 0 || (size_t)st.slot >= in.slots.size()) continue;
            SlotState& s = in.slots[(size_t)st.slot];
            if (!st.display.empty()) {
                const int cur = find_frame(st.display, t);
                s.display = st.display[(size_t)cur].value;
            }
            if (!st.color.empty()) {
                sample_frames(st.color, t, [&s](const ColorFrame& x, const ColorFrame& y, float w, bool) {
                    const ColorTransform& a0 = x.value;
                    const ColorTransform& b0 = y.value;
                    s.color.r_m = a0.r_m + (b0.r_m - a0.r_m) * w;
                    s.color.g_m = a0.g_m + (b0.g_m - a0.g_m) * w;
                    s.color.b_m = a0.b_m + (b0.b_m - a0.b_m) * w;
                    s.color.a_m = a0.a_m + (b0.a_m - a0.a_m) * w;
                });
            }
        }
    }

    /* 骨骼世界矩阵：global = setup + anim（增量），matrix = local * parent。
     * ⚠️ `inheritScale=false` **不是"什么都不继承"**：官方（Bone.cpp 的 else 分支）仍然
     * 继承父的**平移与旋转**，只是不把父的缩放乘进自己的轴向量。踩过：按"只继承平移"近似
     * → 整具骨架的旋转全丢，机甲看起来像躺倒/散架（docs/36 §坑）。 */
    for (int idx : arm.bone_order) {
        const BoneData& bd = arm.bones[(size_t)idx];
        const Pose& p = in.bones[(size_t)idx];
        Transform g;
        g.x = bd.setup.x + p.x;
        g.y = bd.setup.y + p.y;
        g.rotation = bd.setup.rotation + p.rot;
        g.skew = bd.setup.skew + p.skew;
        g.scale_x = bd.setup.scale_x * p.sx;
        g.scale_y = bd.setup.scale_y * p.sy;

        const int parent = bd.parent;
        const bool has_parent = parent >= 0 && (size_t)parent < nb;
        const float parent_abs_rot = has_parent ? in.bone_rot[(size_t)parent] : 0.0F;
        Mat m;
        if (!has_parent) {
            m = to_matrix(g);
        } else if (bd.inherit_scale) {
            if (!bd.inherit_rotation) g.rotation -= parent_abs_rot;
            m = to_matrix(g);
            concat(m, in.bone_mat[(size_t)parent]);
            if (!bd.inherit_translation) { m.tx = g.x; m.ty = g.y; }
        } else {
            const Mat& pm = in.bone_mat[(size_t)parent];
            const float local_x = g.x, local_y = g.y;
            if (bd.inherit_translation) {
                /* 位置：父矩阵 * 局部位置（父的旋转/缩放/平移都作用在**位置**上） */
                g.x = pm.a * local_x + pm.c * local_y + pm.tx;
                g.y = pm.b * local_x + pm.d * local_y + pm.ty;
            }
            if (bd.inherit_rotation) {
                g.rotation += parent_abs_rot;
                /* 父矩阵含反射（负行列式）时按官方修正旋转；inheritReflection 默认 false */
                if (pm.a * pm.d - pm.b * pm.c < 0.0F) {
                    g.rotation -= (g.rotation - parent_abs_rot) * 2.0F;
                    if (bd.inherit_reflection) g.skew += kPi;
                }
            }
            m = to_matrix(g);
            if (!bd.inherit_translation) { m.tx = local_x; m.ty = local_y; }
        }
        in.bone_mat[(size_t)idx] = m;
        in.bone_rot[(size_t)idx] = has_parent ? (bd.inherit_scale ? g.rotation + parent_abs_rot : g.rotation)
                                              : g.rotation;
    }
}

/* 子纹理显示尺寸（未裁剪帧尺寸；旋转时宽高互换）。 */
void display_size(const TextureData& tex, float& w, float& h) {
    if (tex.rotated) { w = tex.h; h = tex.w; }
    else { w = tex.w; h = tex.h; }
}

/* 图像附件的可见矩形（在显示局部空间；见文件头第 7 条）。
 * 官方 SFMLSlot::_updateFrame 的画法：quad = (0,0)..(region.w, region.h)（**只画裁剪后的区域**），
 * 然后在 Slot::_updateTransform 里整块减去 pivot（pivot 由未裁剪帧尺寸算出）→ 局部坐标 = u - pivot。
 * uv 输出顺序与顶点一致：(left,top) (right,top) (left,bottom) (right,bottom)。 */
bool image_quad(const TextureData& tex, const PageData& page, float pivot_x, float pivot_y,
                float out_xy[8], float out_uv[8]) {
    float fw = tex.has_frame ? tex.frame_w : tex.w;
    float fh = tex.has_frame ? tex.frame_h : tex.h;
    float fx = tex.has_frame ? tex.frame_x : 0.0F;
    float fy = tex.has_frame ? tex.frame_y : 0.0F;
    if (tex.rotated && !tex.has_frame) { fw = tex.h; fh = tex.w; }
    const float dw = tex.rotated ? tex.h : tex.w;      // 画出来的始终是裁剪后的区域
    const float dh = tex.rotated ? tex.w : tex.h;
    /* pivot 公式（官方 Slot::_updateDisplayData）：pivot.x * frameW + frameX */
    const float px = pivot_x * fw + fx;
    const float py = pivot_y * fh + fy;
    const float left = -px;
    const float top = -py;
    out_xy[0] = left;      out_xy[1] = top;
    out_xy[2] = left + dw; out_xy[3] = top;
    out_xy[4] = left;      out_xy[5] = top + dh;
    out_xy[6] = left + dw; out_xy[7] = top + dh;

    const float pw = page.width > 0 ? (float)page.width : 1.0F;
    const float ph = page.height > 0 ? (float)page.height : 1.0F;
    auto uv_at = [&](float ux, float uy, float& u, float& v) {   // ux/uy ∈ [0,1] 子纹理内
        if (!tex.rotated) {
            u = (tex.x + ux * tex.w) / pw;
            v = (tex.y + uy * tex.h) / ph;
        } else {
            /* 假定图集把图像顺时针旋转 90° 存放（官方 demo 素材无 rotated，未做逐像素对照） */
            u = (tex.x + (1.0F - uy) * tex.w) / pw;
            v = (tex.y + ux * tex.h) / ph;
        }
    };
    uv_at(0.0F, 0.0F, out_uv[0], out_uv[1]);
    uv_at(1.0F, 0.0F, out_uv[2], out_uv[3]);
    uv_at(0.0F, 1.0F, out_uv[4], out_uv[5]);
    uv_at(1.0F, 1.0F, out_uv[6], out_uv[7]);
    return true;
}

/* 网格 UV：uvs 是**子纹理内**的归一化坐标 → 映射到整页。 */
void mesh_uv(const TextureData& tex, const PageData& page, float ux, float uy, float& u, float& v) {
    const float pw = page.width > 0 ? (float)page.width : 1.0F;
    const float ph = page.height > 0 ? (float)page.height : 1.0F;
    if (!tex.rotated) {
        u = (tex.x + ux * tex.w) / pw;
        v = (tex.y + uy * tex.h) / ph;
    } else {
        u = (tex.x + (1.0F - uy) * tex.w) / pw;
        v = (tex.y + ux * tex.h) / ph;
    }
}

/* FFD：当前动画对该槽该显示的顶点增量（未命中 → 不改）。 */
const FfdTimeline* find_ffd(const ArmatureData& arm, const AnimationData* anim, int skin, int slot) {
    if (!anim) return nullptr;
    for (const FfdTimeline& f : anim->ffd) {
        if (f.slot == slot && (f.skin < 0 || f.skin == skin)) return &f;
    }
    return nullptr;
}

/* FFD：当前帧的顶点增量 + 顶点偏移（offset 以**顶点**为单位）。 */
void ffd_apply(const FfdTimeline& f, int display, float t, std::vector<float>& deltas, int& offset) {
    deltas.clear();
    offset = 0;
    if (f.display != display || f.frames.empty()) return;
    const int cur = find_frame(f.frames, t);
    offset = f.frames[(size_t)cur].offset;
    sample_frames(f.frames, t, [&deltas](const FfdFrame& x, const FfdFrame& y, float w, bool) {
        const size_t n = std::max(x.deltas.size(), y.deltas.size());
        deltas.resize(n);
        for (size_t k = 0; k < n; ++k) {
            const float a = k < x.deltas.size() ? x.deltas[k] : 0.0F;
            const float b = k < y.deltas.size() ? y.deltas[k] : 0.0F;
            deltas[k] = a + (b - a) * w;
        }
    });
}

/* ---------------- 绘制导出 ---------------- */

struct DrawCtx {
    float* verts = nullptr;
    float* uvs = nullptr;
    float* cols = nullptr;
    uint32_t* idx = nullptr;
    mq_db_batch* batches = nullptr;
    int max_v = 0, max_i = 0, max_b = 0;
    int nv = 0, ni = 0, nb = 0;
    bool overflow = false;
    int slot_drawn = 0;
    bool has_bounds = false;
    float min_x = 0.0F, min_y = 0.0F, max_x = 0.0F, max_y = 0.0F;

    void grow_point(float x, float y) {
        if (!has_bounds) { has_bounds = true; min_x = max_x = x; min_y = max_y = y; return; }
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    bool vertex_room() const { return nv < max_v; }
    bool index_room(int n) const { return ni + n <= max_i; }
    void push_vertex(float x, float y, float u, float v, const float c[4]) {
        if (nv < max_v) {
            verts[nv * 2] = x; verts[nv * 2 + 1] = y;
            uvs[nv * 2] = u; uvs[nv * 2 + 1] = v;
            std::memcpy(cols + (size_t)nv * 4, c, sizeof(float) * 4);
        } else {
            overflow = true;
        }
        grow_point(x, y);
        ++nv;
    }
    void push_index(uint32_t i) {
        if (ni < max_i) idx[ni] = i; else overflow = true;
        ++ni;
    }
};

void emit_slot(mq_db_instance& in, int slot_index, DrawCtx& dc) {
    const ArmatureData& arm = *in.arm;
    const SlotData& sd = arm.slots[(size_t)slot_index];
    const SlotState& st = in.slots[(size_t)slot_index];
    if (st.display < 0) return;
    const int skin = in.skin >= 0 && in.skin < (int)arm.skins.size() ? in.skin : 0;
    const SkinData& sk = arm.skins[(size_t)skin];
    if ((size_t)slot_index >= sk.displays.size()) return;
    const auto& list = sk.displays[(size_t)slot_index];
    if ((size_t)st.display >= list.size()) return;
    const DisplayData& disp = list[(size_t)st.display];
    if (disp.kind == DisplayData::Armature || disp.kind == DisplayData::BoundingBox) return;
    if (disp.texture < 0) return;
    const TextureData& tex = in.owner->d.textures[(size_t)disp.texture];
    const PageData& page = in.owner->d.pages[(size_t)tex.page];

    /* 骨骼世界矩阵 × 显示局部变换 */
    Mat m = to_matrix(disp.t);
    if (sd.bone >= 0 && (size_t)sd.bone < in.bone_mat.size())
        concat(m, in.bone_mat[(size_t)sd.bone]);

    /* 顶点颜色 = 槽色（乘子）× 实例 tint；颜色 offset 未支持（见文件头） */
    float c[4] = {
        st.color.r_m * in.tint[0],
        st.color.g_m * in.tint[1],
        st.color.b_m * in.tint[2],
        st.color.a_m * in.tint[3],
    };
    const float sx = in.scale * (in.flip_x ? -1.0F : 1.0F);
    const float sy = in.scale * (in.flip_y ? -1.0F : 1.0F);
    auto xform = [&](float lx, float ly, float& ox, float& oy) {
        float px = 0.0F, py = 0.0F;
        apply(m, lx, ly, px, py);
        ox = px * sx;
        oy = py * sy;
    };

    const int vcount = (disp.kind == DisplayData::Mesh) ? (int)(disp.verts.size() / 2) : 4;
    const int icount = (disp.kind == DisplayData::Mesh) ? (int)disp.tris.size() : 6;
    if (vcount <= 0 || icount <= 0) return;

    const int base_vertex = dc.nv;
    const int first_index = dc.ni;

    if (disp.kind == DisplayData::Image) {
        float quad[8], uv[8];
        image_quad(tex, page, disp.pivot_x, disp.pivot_y, quad, uv);
        for (int i = 0; i < 4; ++i) {
            float x = 0.0F, y = 0.0F;
            xform(quad[i * 2], quad[i * 2 + 1], x, y);
            dc.push_vertex(x, y, uv[i * 2], uv[i * 2 + 1], c);
        }
        dc.push_index((uint32_t)base_vertex + 0);
        dc.push_index((uint32_t)base_vertex + 1);
        dc.push_index((uint32_t)base_vertex + 2);
        dc.push_index((uint32_t)base_vertex + 2);
        dc.push_index((uint32_t)base_vertex + 1);
        dc.push_index((uint32_t)base_vertex + 3);
    } else {
        if (disp.weighted) return;                 // P0 未支持（诊断计数在 data 侧）
        /* FFD 变形（只对非蒙皮网格） */
        const AnimationData* anim = (in.anim >= 0 && in.anim < (int)arm.animations.size())
                                        ? &arm.animations[(size_t)in.anim] : nullptr;
        const FfdTimeline* ffd = find_ffd(arm, anim, skin, slot_index);
        std::vector<float> deformed;
        if (ffd && ffd->display == st.display) {
            int offset = 0;
            ffd_apply(*ffd, st.display, in.time, in.deform, offset);
            deformed.resize(disp.verts.size());
            for (size_t i = 0; i < disp.verts.size(); ++i) {
                const size_t k = i - (size_t)offset * 2;      // offset 以顶点计 → 分量下标 *2
                const bool in_range = i >= (size_t)offset * 2 && k < in.deform.size();
                deformed[i] = disp.verts[i] + (in_range ? in.deform[k] : 0.0F);
            }
        }
        const std::vector<float>& verts = deformed.empty() ? disp.verts : deformed;
        for (int i = 0; i < vcount; ++i) {
            float x = 0.0F, y = 0.0F;
            xform(verts[(size_t)i * 2], verts[(size_t)i * 2 + 1], x, y);
            float u = 0.0F, v = 0.0F;
            if ((size_t)(i * 2 + 1) < disp.uvs.size())
                mesh_uv(tex, page, disp.uvs[(size_t)i * 2], disp.uvs[(size_t)i * 2 + 1], u, v);
            dc.push_vertex(x, y, u, v, c);
        }
        for (uint16_t ti : disp.tris) dc.push_index((uint32_t)base_vertex + ti);
    }

    if (dc.overflow) return;
    /* 批次合并：同页同混合模式的连续槽段 */
    if (dc.nb > 0 && dc.batches[dc.nb - 1].page == tex.page &&
        dc.batches[dc.nb - 1].blend == sd.blend) {
        dc.batches[dc.nb - 1].index_count += (uint32_t)(dc.ni - first_index);
    } else if (dc.nb < dc.max_b) {
        dc.batches[dc.nb].page = tex.page;
        dc.batches[dc.nb].blend = sd.blend;
        dc.batches[dc.nb].first_index = (uint32_t)first_index;
        dc.batches[dc.nb].index_count = (uint32_t)(dc.ni - first_index);
        ++dc.nb;
    } else {
        dc.overflow = true;
        return;
    }
    ++dc.slot_drawn;
}

}  // namespace maque::db

/* ================= C ABI ================= */

using namespace maque::db;

extern "C" {

uint32_t mq_db_abi_version(void) { return MQ_DRAGONBONES_ABI_VERSION; }

mq_db_data* mq_db_data_create(const void* skeleton_json, size_t skeleton_size, const char* texture_json,
                              const char* /*dir_hint*/, mq_db_texture_cb load_texture, void* user) {
    if (!skeleton_json || skeleton_size == 0) return nullptr;
    const char* text = static_cast<const char*>(skeleton_json);
    /* 二进制 .dbbin（以 "DBDT" 开头）P0 不支持 → 明确失败，别当成 JSON 瞎解析 */
    if (skeleton_size >= 4 && std::memcmp(text, "DBDT", 4) == 0) return nullptr;
    auto* data = new (std::nothrow) mq_db_data();
    if (!data) return nullptr;

    if (texture_json && *texture_json) {
        mq_json* troot = mq_json_parse(texture_json, std::strlen(texture_json));
        if (troot) {
            parse_page(data->d, troot, load_texture, user);
            mq_json_free(troot);
        }
    }
    if (parse_skeleton(data->d, text, skeleton_size) != MQ_DB_OK) {
        delete data;
        return nullptr;
    }
    return data;
}

void mq_db_data_destroy(mq_db_data* data) { delete data; }

const char* mq_db_data_name(mq_db_data* data) { return data ? data->d.name.c_str() : ""; }
float mq_db_data_frame_rate(mq_db_data* data) { return data ? data->d.frame_rate : 0.0F; }
int mq_db_data_armature_count(mq_db_data* data) { return data ? (int)data->d.armatures.size() : 0; }

const char* mq_db_data_armature_name(mq_db_data* data, int index) {
    if (!data || index < 0 || index >= (int)data->d.armatures.size()) return "";
    return data->d.armatures[(size_t)index].name.c_str();
}

int mq_db_data_armature_index(mq_db_data* data, const char* name) {
    if (!data || !name) return -1;
    for (size_t i = 0; i < data->d.armatures.size(); ++i)
        if (data->d.armatures[i].name == name) return (int)i;
    return -1;
}

int mq_db_data_animation_count(mq_db_data* data, int armature) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return 0;
    return (int)data->d.armatures[(size_t)armature].animations.size();
}

int mq_db_data_animation(mq_db_data* data, int armature, int index, mq_db_anim* out) {
    if (!out) return MQ_DB_ERR_ARG;
    std::memset(out, 0, sizeof *out);
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return MQ_DB_ERR_ARG;
    const ArmatureData& arm = data->d.armatures[(size_t)armature];
    if (index < 0 || index >= (int)arm.animations.size()) return MQ_DB_ERR_NOTFOUND;
    const AnimationData& a = arm.animations[(size_t)index];
    out->name = a.name.c_str();
    out->duration = a.duration;
    out->play_times = a.play_times;
    int frames = 0;
    for (const auto& b : a.bones) frames += (int)(b.translate.size() + b.rotate.size() + b.scale.size());
    for (const auto& s : a.slots) frames += (int)(s.display.size() + s.color.size());
    out->frame_count = frames;
    return MQ_DB_OK;
}

int mq_db_data_animation_index(mq_db_data* data, int armature, const char* name) {
    if (!data || !name || armature < 0 || armature >= (int)data->d.armatures.size()) return -1;
    const ArmatureData& arm = data->d.armatures[(size_t)armature];
    for (size_t i = 0; i < arm.animations.size(); ++i)
        if (arm.animations[i].name == name) return (int)i;
    return -1;
}

int mq_db_data_skin_count(mq_db_data* data, int armature) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return 0;
    return (int)data->d.armatures[(size_t)armature].skins.size();
}

const char* mq_db_data_skin_name(mq_db_data* data, int armature, int index) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return "";
    const ArmatureData& arm = data->d.armatures[(size_t)armature];
    if (index < 0 || index >= (int)arm.skins.size()) return "";
    return arm.skins[(size_t)index].name.c_str();
}

int mq_db_data_skin_index(mq_db_data* data, int armature, const char* name) {
    if (!data || !name || armature < 0 || armature >= (int)data->d.armatures.size()) return -1;
    const ArmatureData& arm = data->d.armatures[(size_t)armature];
    for (size_t i = 0; i < arm.skins.size(); ++i)
        if (arm.skins[i].name == name) return (int)i;
    return -1;
}

int mq_db_data_bone_count(mq_db_data* data, int armature) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return 0;
    return (int)data->d.armatures[(size_t)armature].bones.size();
}

const char* mq_db_data_bone_name(mq_db_data* data, int armature, int index) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return "";
    const ArmatureData& arm = data->d.armatures[(size_t)armature];
    if (index < 0 || index >= (int)arm.bones.size()) return "";
    return arm.bones[(size_t)index].name.c_str();
}

int mq_db_data_slot_count(mq_db_data* data, int armature) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return 0;
    return (int)data->d.armatures[(size_t)armature].slots.size();
}

const char* mq_db_data_slot_name(mq_db_data* data, int armature, int index) {
    if (!data || armature < 0 || armature >= (int)data->d.armatures.size()) return "";
    const ArmatureData& arm = data->d.armatures[(size_t)armature];
    if (index < 0 || index >= (int)arm.slots.size()) return "";
    return arm.slots[(size_t)index].name.c_str();
}

int mq_db_data_page_count(mq_db_data* data) { return data ? (int)data->d.pages.size() : 0; }

int mq_db_data_page(mq_db_data* data, int index, mq_db_page* out) {
    if (!out) return MQ_DB_ERR_ARG;
    std::memset(out, 0, sizeof *out);
    if (!data || index < 0 || index >= (int)data->d.pages.size()) return MQ_DB_ERR_NOTFOUND;
    const PageData& p = data->d.pages[(size_t)index];
    out->name = p.name.c_str();
    out->width = p.width;
    out->height = p.height;
    out->texture = p.texture;
    return MQ_DB_OK;
}

int mq_db_data_set_page_texture(mq_db_data* data, int index, uint64_t texture, int width, int height) {
    if (!data || index < 0 || index >= (int)data->d.pages.size()) return MQ_DB_ERR_ARG;
    PageData& p = data->d.pages[(size_t)index];
    p.texture = texture;
    if (width > 0) p.width = width;
    if (height > 0) p.height = height;
    return MQ_DB_OK;
}

int mq_db_data_texture_count(mq_db_data* data) { return data ? (int)data->d.textures.size() : 0; }
int mq_db_data_mesh_count(mq_db_data* data) { return data ? data->d.mesh_count : 0; }
int mq_db_data_weighted_count(mq_db_data* data) { return data ? data->d.weighted_count : 0; }
int mq_db_data_nested_count(mq_db_data* data) { return data ? data->d.nested_count : 0; }
int mq_db_data_missing_texture_count(mq_db_data* data) { return data ? data->d.missing_texture_count : 0; }

mq_db_instance* mq_db_instance_create(mq_db_data* data, int armature_index) {
    if (!data) return nullptr;
    if (armature_index < 0) armature_index = 0;
    if (armature_index >= (int)data->d.armatures.size()) return nullptr;
    auto* inst = new (std::nothrow) mq_db_instance();
    if (!inst) return nullptr;
    inst->owner = data;
    inst->arm = &data->d.armatures[(size_t)armature_index];
    inst->skin = 0;
    inst->slots.resize(inst->arm->slots.size());
    evaluate(*inst);
    return inst;
}

void mq_db_instance_destroy(mq_db_instance* inst) { delete inst; }

int mq_db_instance_set_skin(mq_db_instance* inst, const char* name) {
    if (!inst || !name) return MQ_DB_ERR_ARG;
    const ArmatureData& arm = *inst->arm;
    for (size_t i = 0; i < arm.skins.size(); ++i) {
        if (arm.skins[i].name == name) {
            inst->skin = (int)i;
            evaluate(*inst);
            return MQ_DB_OK;
        }
    }
    return MQ_DB_ERR_NOTFOUND;
}

const char* mq_db_instance_skin_name(mq_db_instance* inst) {
    if (!inst) return "";
    const ArmatureData& arm = *inst->arm;
    if (inst->skin < 0 || inst->skin >= (int)arm.skins.size()) return "";
    return arm.skins[(size_t)inst->skin].name.c_str();
}

int mq_db_instance_play(mq_db_instance* inst, const char* animation, int loop) {
    if (!inst || !animation) return MQ_DB_ERR_ARG;
    const ArmatureData& arm = *inst->arm;
    for (size_t i = 0; i < arm.animations.size(); ++i) {
        if (arm.animations[i].name == animation) {
            inst->anim = (int)i;
            inst->time = 0.0F;
            inst->play_times = 0;
            inst->complete = false;
            inst->playing = true;
            inst->loop = loop != 0;
            evaluate(*inst);
            return MQ_DB_OK;
        }
    }
    return MQ_DB_ERR_NOTFOUND;
}

void mq_db_instance_stop(mq_db_instance* inst) {
    if (!inst) return;
    inst->anim = -1;
    inst->time = 0.0F;
    inst->playing = false;
    evaluate(*inst);
}

int mq_db_instance_set_time(mq_db_instance* inst, float time) {
    if (!inst) return MQ_DB_ERR_ARG;
    float dur = 0.0F;
    if (inst->anim >= 0 && inst->anim < (int)inst->arm->animations.size())
        dur = inst->arm->animations[(size_t)inst->anim].duration;
    if (dur > 0.0F) {
        if (time < 0.0F) time = 0.0F;
        if (time >= dur) time = std::fmod(time, dur);
    } else {
        time = 0.0F;
    }
    inst->time = time;
    inst->complete = false;
    evaluate(*inst);
    return MQ_DB_OK;
}

float mq_db_instance_time(mq_db_instance* inst) { return inst ? inst->time : 0.0F; }

float mq_db_instance_duration(mq_db_instance* inst) {
    if (!inst || inst->anim < 0 || inst->anim >= (int)inst->arm->animations.size()) return 0.0F;
    return inst->arm->animations[(size_t)inst->anim].duration;
}

const char* mq_db_instance_animation_name(mq_db_instance* inst) {
    if (!inst || !inst->arm || inst->anim < 0 || inst->anim >= (int)inst->arm->animations.size()) return "";
    return inst->arm->animations[(size_t)inst->anim].name.c_str();
}

int mq_db_instance_play_times(mq_db_instance* inst) { return inst ? inst->play_times : 0; }
int mq_db_instance_complete(mq_db_instance* inst) { return (inst && inst->complete) ? 1 : 0; }
void mq_db_instance_set_playing(mq_db_instance* inst, int playing) { if (inst) inst->playing = playing != 0; }

int mq_db_instance_step(mq_db_instance* inst, float dt) {
    if (!inst) return MQ_DB_ERR_ARG;
    if (inst->anim < 0 || inst->anim >= (int)inst->arm->animations.size()) return MQ_DB_OK;
    const float dur = inst->arm->animations[(size_t)inst->anim].duration;
    if (inst->playing && dt != 0.0F) {
        inst->time += dt;
        if (dur > 0.0F) {
            if (inst->loop) {
                while (inst->time >= dur) { inst->time -= dur; ++inst->play_times; }
                while (inst->time < 0.0F) { inst->time += dur; --inst->play_times; }
            } else if (inst->time >= dur) {
                inst->time = dur;
                inst->complete = true;
                inst->playing = false;
            }
        } else {
            inst->time = 0.0F;
            inst->complete = true;
            inst->playing = false;
        }
    }
    evaluate(*inst);
    return MQ_DB_OK;
}

void mq_db_instance_set_transform(mq_db_instance* inst, float scale, int flip_x, int flip_y) {
    if (!inst) return;
    inst->scale = scale;
    inst->flip_x = flip_x != 0;
    inst->flip_y = flip_y != 0;
}

void mq_db_instance_set_color(mq_db_instance* inst, float r, float g, float b, float a) {
    if (!inst) return;
    inst->tint[0] = r; inst->tint[1] = g; inst->tint[2] = b; inst->tint[3] = a;
}

int mq_db_instance_bone_count(mq_db_instance* inst) { return (inst && inst->arm) ? (int)inst->arm->bones.size() : 0; }

const char* mq_db_instance_bone_name(mq_db_instance* inst, int index) {
    if (!inst || !inst->arm || index < 0 || index >= (int)inst->arm->bones.size()) return "";
    return inst->arm->bones[(size_t)index].name.c_str();
}

int mq_db_instance_bone_find(mq_db_instance* inst, const char* name) {
    if (!inst || !inst->arm || !name) return -1;
    for (size_t i = 0; i < inst->arm->bones.size(); ++i)
        if (inst->arm->bones[i].name == name) return (int)i;
    return -1;
}

int mq_db_instance_bone_world_position(mq_db_instance* inst, int index, float* x, float* y) {
    if (!inst || !x || !y || index < 0 || index >= (int)inst->bone_mat.size()) return MQ_DB_ERR_ARG;
    const Mat& m = inst->bone_mat[(size_t)index];
    *x = m.tx * inst->scale * (inst->flip_x ? -1.0F : 1.0F);
    *y = m.ty * inst->scale * (inst->flip_y ? -1.0F : 1.0F);
    return MQ_DB_OK;
}

int mq_db_instance_slot_count(mq_db_instance* inst) { return (inst && inst->arm) ? (int)inst->arm->slots.size() : 0; }

const char* mq_db_instance_slot_name(mq_db_instance* inst, int index) {
    if (!inst || !inst->arm || index < 0 || index >= (int)inst->arm->slots.size()) return "";
    return inst->arm->slots[(size_t)index].name.c_str();
}

int mq_db_instance_slot_index(mq_db_instance* inst, const char* name) {
    if (!inst || !inst->arm || !name) return -1;
    return find_slot(*inst->arm, name);
}

int mq_db_instance_slot_display(mq_db_instance* inst, int index) {
    if (!inst || index < 0 || index >= (int)inst->slots.size()) return -1;
    return inst->slots[(size_t)index].display;
}

int mq_db_instance_set_slot_display(mq_db_instance* inst, int index, int display) {
    if (!inst || index < 0 || index >= (int)inst->slots.size()) return MQ_DB_ERR_ARG;
    inst->slots[(size_t)index].display = display;
    return MQ_DB_OK;
}

int mq_db_instance_draw(mq_db_instance* inst, float* vertices, float* uvs, float* colors, uint32_t* indices,
                        mq_db_batch* batches, int max_vertices, int max_indices, int max_batches,
                        mq_db_draw* out) {
    if (!inst || !out || !vertices || !uvs || !colors || !indices || !batches) return MQ_DB_ERR_ARG;
    std::memset(out, 0, sizeof *out);
    DrawCtx dc;
    dc.verts = vertices;
    dc.uvs = uvs;
    dc.cols = colors;
    dc.idx = indices;
    dc.batches = batches;
    dc.max_v = max_vertices;
    dc.max_i = max_indices;
    dc.max_b = max_batches;
    const ArmatureData& arm = *inst->arm;
    for (int i = 0; i < (int)arm.slots.size(); ++i) {
        emit_slot(*inst, i, dc);
        if (dc.overflow) break;
    }
    out->vertex_count = dc.nv;
    out->index_count = dc.ni;
    out->batch_count = dc.nb;
    out->slot_count = dc.slot_drawn;
    if (dc.has_bounds) {
        out->bounds_x = dc.min_x;
        out->bounds_y = dc.min_y;
        out->bounds_w = dc.max_x - dc.min_x;
        out->bounds_h = dc.max_y - dc.min_y;
    }
    return dc.overflow ? MQ_DB_ERR_NOMEM : MQ_DB_OK;
}

int mq_db_instance_packed(mq_db_instance* /*inst*/) { return 0; }

}  // extern "C"
