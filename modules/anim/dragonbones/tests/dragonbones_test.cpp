/* DragonBones 模块单测（docs/36）：解析 + 动画求值 + 绘制数据导出的**数据面**断言。
 *
 * 用真实官方素材（tests/data/dragonbones/，MIT）验证：
 *   · 骨架/皮肤/槽/骨骼/动画清单与尺寸（与 JSON 原文对拍）
 *   · 绘制数据：顶点/索引/批次/UV 范围/包围盒
 *   · 动画推进/循环计数/非循环完成/时间跳转/确定性
 *   · 边界与错误路径（二进制拒绝、坏名字、缓冲不足 → ERR_NOMEM 后再扩容）
 * 另用**内联合成素材**验证皮肤切换（真实 demo 素材都是单皮肤，无法覆盖换皮肤路径）。
 */
#include "anim/mq_dragonbones.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <vector>

namespace {

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) {
        std::print(stderr, "FAIL: {}\n", what);
        ++failures;
    }
}

struct TexCb {
    std::vector<std::string> pages;
    int calls = 0;
};

int tex_cb(void* user, const char* page, uint64_t* texture, int* w, int* h) {
    auto* cb = static_cast<TexCb*>(user);
    ++cb->calls;
    cb->pages.emplace_back(page ? page : "");
    if (texture) *texture = 0x1000u + (uint64_t)cb->calls;
    if (w) *w = 0;             // 0 = 让模块用图集 json（缺失时用子纹理边界兜底）
    if (h) *h = 0;
    return 0;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Drawn {
    std::vector<float> verts, uvs, cols;
    std::vector<uint32_t> idx;
    std::vector<mq_db_batch> batches;
    mq_db_draw info{};
};

bool draw(mq_db_instance* inst, Drawn& out, int vcap = 16384, int icap = 32768, int bcap = 512) {
    out.verts.assign((size_t)vcap * 2, 0.0F);
    out.uvs.assign((size_t)vcap * 2, 0.0F);
    out.cols.assign((size_t)vcap * 4, 0.0F);
    out.idx.assign((size_t)icap, 0u);
    out.batches.assign((size_t)bcap, mq_db_batch{});
    return mq_db_instance_draw(inst, out.verts.data(), out.uvs.data(), out.cols.data(), out.idx.data(),
                               out.batches.data(), vcap, icap, bcap, &out.info) == MQ_DB_OK;
}

void check_uv_range(const Drawn& d, const char* what) {
    for (int i = 0; i < d.info.vertex_count; ++i) {
        const float u = d.uvs[(size_t)i * 2], v = d.uvs[(size_t)i * 2 + 1];
        if (!(u >= -0.001F && u <= 1.001F && v >= -0.001F && v <= 1.001F)) {
            std::print(stderr, "FAIL: {} UV 越界 [{}] u={} v={}\n", what, i, u, v);
            ++failures;
            return;
        }
    }
    check(true, what);
}

bool nearly(float a, float b, float eps = 1e-3F) { return std::fabs(a - b) <= eps; }

void test_real_mecha(const std::string& dir) {
    const std::string ske = read_file(dir + "/mecha_1004d_ske.json");
    const std::string tex = read_file(dir + "/mecha_1004d_tex.json");
    check(!ske.empty() && !tex.empty(), "mecha_1004d 素材读取");
    if (ske.empty() || tex.empty()) return;

    TexCb cb;
    mq_db_data* data = mq_db_data_create(ske.data(), ske.size(), tex.c_str(), "", tex_cb, &cb);
    check(data != nullptr, "mecha_1004d 解析成功");
    if (!data) return;

    check(mq_db_abi_version() == MQ_DRAGONBONES_ABI_VERSION, "ABI 版本");
    check(mq_db_data_frame_rate(data) == 25.0F, "frameRate=25");
    check(mq_db_data_armature_count(data) == 4, "armature 数=4");
    check(std::string(mq_db_data_armature_name(data, 0)) == "mecha_1004d", "armature[0] 名字");
    check(mq_db_data_armature_index(data, "weapon_replace") == 3, "armature 名字 → 下标");
    check(mq_db_data_animation_count(data, 0) == 10, "armature[0] 动画数=10");
    check(mq_db_data_animation_index(data, 0, "walk") == 1, "walk 下标");
    check(mq_db_data_bone_count(data, 0) == 20, "骨骼数=20");
    check(mq_db_data_slot_count(data, 0) == 18, "槽数=18");
    check(mq_db_data_skin_count(data, 0) == 1, "皮肤数=1");
    check(mq_db_data_texture_count(data) == 46, "子纹理数=46");
    check(mq_db_data_mesh_count(data) == 0, "mecha 无 mesh");
    check(mq_db_data_weighted_count(data) == 0, "mecha 无蒙皮网格");

    mq_db_anim anim{};
    check(mq_db_data_animation(data, 0, 0, &anim) == MQ_DB_OK, "取动画 0");
    check(std::string(anim.name) == "idle", "动画 0 = idle");
    check(nearly(anim.duration, 60.0F / 25.0F), "idle 时长 = 60/25 秒");
    check(anim.play_times == 0, "idle playTimes=0（循环）");
    check(anim.frame_count > 50, "idle 关键帧总数 > 50");

    check(mq_db_data_page_count(data) == 1, "图集页数=1");
    mq_db_page page{};
    check(mq_db_data_page(data, 0, &page) == MQ_DB_OK, "取页 0");
    check(page.width == 1024 && page.height == 1024, "页尺寸 1024x1024");
    check(page.texture == 0x1001u, "页纹理句柄已回填");
    check(cb.calls == 1 && cb.pages.size() == 1 && cb.pages[0] == "mecha_1004d_tex.png", "纹理回调页名");

    mq_db_instance* inst = mq_db_instance_create(data, 0);
    check(inst != nullptr, "实例创建");
    if (!inst) { mq_db_data_destroy(data); return; }
    check(mq_db_instance_play_times(inst) == 0, "初始播放轮次 0");
    check(mq_db_instance_play(inst, "idle", 1) == MQ_DB_OK, "播放 idle");
    check(std::string(mq_db_instance_animation_name(inst)) == "idle", "当前动画名");
    check(nearly(mq_db_instance_duration(inst), 2.4F), "实例时长 2.4s");

    Drawn a;
    check(draw(inst, a), "绘制 idle@0");
    /* 18 个槽：effect_l/effect_r 的 setup displayIndex=-1（隐藏）；weapon_hand_l/r 是
     * 嵌套子骨架（armature 类型，P0 未支持）→ 实际画 14 个 image 四边形 = 56 顶点 */
    check(a.info.vertex_count >= 56, "idle 顶点数 >= 56（14 个可见 image 槽）");
    check(a.info.index_count == a.info.vertex_count / 4 * 6, "索引数 = 四边形数*6（mecha 全 image）");
    check(a.info.batch_count >= 1, "批次数 >= 1");
    check(a.info.slot_count >= 13, "画出几何的槽数 >= 13");
    check(a.info.bounds_w > 1.0F && a.info.bounds_h > 1.0F, "包围盒非退化");
    check_uv_range(a, "UV 范围");

    /* 动画真的在变：t=0 与 t=1.2（idle 中段）的顶点必须不同 */
    Drawn b;
    check(mq_db_instance_set_time(inst, 1.2F) == MQ_DB_OK, "set_time(1.2)");
    check(draw(inst, b), "绘制 idle@1.2");
    check(std::memcmp(a.verts.data(), b.verts.data(), (size_t)a.info.vertex_count * 2 * sizeof(float)) != 0,
          "不同播放头的顶点不同（动画求值生效）");

    /* 确定性：两个实例同 dt 推进 → 逐字节一致 */
    mq_db_instance* inst2 = mq_db_instance_create(data, 0);
    check(inst2 != nullptr, "第二实例");
    if (inst2) {
        mq_db_instance_play(inst, "walk", 1);
        mq_db_instance_play(inst2, "walk", 1);
        for (int i = 0; i < 10; ++i) { mq_db_instance_step(inst, 0.1F); mq_db_instance_step(inst2, 0.1F); }
        Drawn c, d;
        check(draw(inst, c) && draw(inst2, d), "两实例绘制");
        check(c.info.vertex_count == d.info.vertex_count && c.info.index_count == d.info.index_count, "两实例规模一致");
        check(std::memcmp(c.verts.data(), d.verts.data(), (size_t)c.info.vertex_count * 2 * sizeof(float)) == 0,
              "同输入 → 顶点逐字节一致（确定性）");
        check(nearly(mq_db_instance_time(inst), 1.0F) && nearly(mq_db_instance_time(inst2), 1.0F), "step 推进时间");
        mq_db_instance_destroy(inst2);
    }

    /* 循环：播 3 轮多点 → play_times>=3 且 time 回绕到 [0,duration) */
    mq_db_instance_play(inst, "idle", 1);
    mq_db_instance_step(inst, 2.4F * 3.0F + 0.05F);
    check(mq_db_instance_play_times(inst) >= 3, "循环轮次 >= 3");
    check(mq_db_instance_time(inst) >= 0.0F && mq_db_instance_time(inst) < 2.4F, "循环后播放头回绕");
    check(mq_db_instance_complete(inst) == 0, "循环动画 complete=0");

    /* 非循环：death 播完 → complete=1、播放头停在时长上 */
    const float death_dur = 66.0F / 25.0F;
    check(mq_db_instance_play(inst, "death", 0) == MQ_DB_OK, "播放 death（非循环）");
    mq_db_instance_step(inst, death_dur * 2.0F);
    check(mq_db_instance_complete(inst) == 1, "非循环 complete=1");
    check(nearly(mq_db_instance_time(inst), death_dur), "非循环播放头停在时长");

    /* 骨骼查询 */
    check(mq_db_instance_bone_count(inst) == 20, "实例骨骼数");
    check(std::string(mq_db_instance_bone_name(inst, 0)) == "root1", "骨骼 0 = root1（声明序）");
    check(mq_db_instance_bone_find(inst, "pelvis") >= 0, "pelvis 骨骼存在");
    const int root_index = mq_db_instance_bone_find(inst, "root1");
    check(root_index == 0, "root1 下标=0");
    float bx = 0.0F, by = 0.0F;
    check(mq_db_instance_bone_world_position(inst, root_index, &bx, &by) == MQ_DB_OK, "骨骼世界位置");
    check(nearly(bx, 0.0F) && nearly(by, 0.0F), "root1 在原点");

    /* 错误路径 */
    check(mq_db_instance_play(inst, "no-such-anim", 1) == MQ_DB_ERR_NOTFOUND, "坏动画名 → NOTFOUND");
    check(mq_db_data_animation_index(data, 0, "no-such-anim") == -1, "坏动画名 → -1");
    check(mq_db_instance_set_skin(inst, "no-such-skin") == MQ_DB_ERR_NOTFOUND, "坏皮肤名 → NOTFOUND");

    /* 缓冲不足：返回 NOMEM，但 out 里给出实际规模（调用方据此扩容重试） */
    {
        std::vector<float> v(2, 0.0F), uv(2, 0.0F), c(4, 0.0F);
        std::vector<uint32_t> ix(3, 0u);
        std::vector<mq_db_batch> bt(1);
        mq_db_draw info{};
        mq_db_instance_play(inst, "idle", 1);
        const int rc = mq_db_instance_draw(inst, v.data(), uv.data(), c.data(), ix.data(), bt.data(), 1, 3, 1, &info);
        check(rc == MQ_DB_ERR_NOMEM, "缓冲不足 → ERR_NOMEM");
        check(info.vertex_count > 1, "NOMEM 时仍报告实际顶点数");
        Drawn e;
        check(draw(inst, e), "扩容后重试成功");
    }

    mq_db_instance_destroy(inst);
    mq_db_data_destroy(data);
}

void test_mesh(const std::string& dir) {
    const std::string ske = read_file(dir + "/weapon_1004_show_ske.json");
    const std::string tex = read_file(dir + "/weapon_1004_show_tex.json");
    if (ske.empty() || tex.empty()) { check(false, "weapon_1004_show 素材读取"); return; }
    TexCb cb;
    mq_db_data* data = mq_db_data_create(ske.data(), ske.size(), tex.c_str(), "", tex_cb, &cb);
    check(data != nullptr, "weapon_1004_show 解析");
    if (!data) return;
    check(mq_db_data_armature_count(data) == 2, "weapon_1004_show armature 数=2");
    check(mq_db_data_mesh_count(data) >= 1, "含 mesh 附件");
    check(mq_db_data_weighted_count(data) == 0, "无蒙皮网格");

    mq_db_instance* inst = mq_db_instance_create(data, 0);
    check(inst != nullptr, "mesh 实例");
    if (inst) {
        check(mq_db_instance_play(inst, "newAnimation", 1) == MQ_DB_OK, "播放 newAnimation");
        /* 该槽 5 个候选显示，下标 3 是网格（weapon_1004d_r）；动画默认显示的可能是 image →
         * 显式选网格，验证网格路径（顶点/索引都来自 triangles，不是 4/6） */
        check(mq_db_instance_set_slot_display(inst, 0, 3) == MQ_DB_OK, "切到网格显示");
        Drawn d;
        check(draw(inst, d), "mesh 绘制");
        check(d.info.vertex_count == 22, "mesh 顶点数 = 22（附件 vertices/2）");
        check(d.info.index_count == 60, "mesh 索引数 = 60（20 个三角形）");
        check_uv_range(d, "mesh UV 范围");
        check(mq_db_instance_slot_count(inst) == 1, "槽数=1");
        check(mq_db_instance_slot_display(inst, 0) == 3, "槽显示下标读回");
        mq_db_instance_destroy(inst);
    }
    mq_db_data_destroy(data);
}

/* 合成素材：**动画帧的 rotate 是角度制**（官方 _parseBoneRotateFrame 乘 DEG_RAD）。
 * 回归：按弧度读会让旋转差 ~57 倍（实测表现为"关节朝反方向甩/动画扭曲"）。
 *   root  setup 无旋转；idle 的 rotateFrame 第一帧 rotate=90（**度**）
 *   child (100,0) → 世界位置应为 R(90°)·(100,0) = (0,100)；
 *   若误当弧度（90 rad ≈ 2.8° 取模后完全不同的角度）位置会明显不同。 */
void test_synthetic_deg_rotate() {
    const char* atlas =
        R"({"name":"deg_tex","imagePath":"deg_tex.png","width":16,"height":16,)"
        R"("SubTexture":[{"name":"px","x":0,"y":0,"width":4,"height":4}]})";
    const char* skeleton =
        R"({"name":"deg","version":"5.5","frameRate":24,"armature":[{"name":"deg","type":"Armature",)"
        R"("bone":[{"name":"root"},{"name":"child","parent":"root","transform":{"x":100,"y":0}}],)"
        R"("slot":[{"name":"s0","parent":"child"}],)"
        R"("skin":[{"name":"","slot":[{"name":"s0","display":[{"name":"px","type":"image"}]}]}],)"
        R"("animation":[{"name":"turn","duration":1,"playTimes":0,)"
        R"("bone":[{"name":"root","rotateFrame":[{"duration":1,"tweenEasing":0,"rotate":90}]}]}]}]})";
    TexCb cb;
    mq_db_data* data = mq_db_data_create(skeleton, std::strlen(skeleton), atlas, "", tex_cb, &cb);
    check(data != nullptr, "角度制素材解析");
    if (!data) return;
    mq_db_instance* inst = mq_db_instance_create(data, 0);
    check(inst != nullptr, "角度制实例");
    if (inst) {
        check(mq_db_instance_play(inst, "turn", 1) == MQ_DB_OK, "播放 turn");
        check(mq_db_instance_set_time(inst, 0.0F) == MQ_DB_OK, "turn @0");
        float bx = 0.0F, by = 0.0F;
        const int child = mq_db_instance_bone_find(inst, "child");
        check(child >= 0 && mq_db_instance_bone_world_position(inst, child, &bx, &by) == MQ_DB_OK,
              "child 骨骼查询");
        check(nearly(bx, 0.0F, 0.05F) && nearly(by, 100.0F, 0.05F),
              "rotateFrame.rotate=90 被当**度**（child → (0,100)）");
        mq_db_instance_destroy(inst);
    }
    mq_db_data_destroy(data);
}

/* 合成素材：`inheritScale=false` 的语义回归（**踩过的坑**：这不是"什么都不继承"）。
 * 官方 Bone.cpp 的 else 分支：不乘父缩放，但**平移与旋转照常继承**。
 *   root  (0,0)  skY=90°  inheritScale=false
 *   child (100,0) skY=0°  inheritScale=false
 *   → child 世界位置 = R(90°)·(100,0) = (0,100)，世界旋转 = 90°
 *   → 20×20 图（pivot 0.5）的包围盒 x∈[-10,10]、y∈[90,110]
 * 旧实现"只继承平移"会得到 x∈[90,110]、y∈[-10,10]（整具骨架旋转丢失 → 机甲躺倒）。 */
void test_synthetic_inherit() {
    const char* atlas =
        R"({"name":"inh_tex","imagePath":"inh_tex.png","width":32,"height":32,)"
        R"("SubTexture":[{"name":"quad","x":0,"y":0,"width":20,"height":20}]})";
    const char* skeleton =
        R"({"name":"inh","version":"5.5","frameRate":24,"armature":[{"name":"inh","type":"Armature",)"
        R"("bone":[{"name":"root","inheritScale":false,"transform":{"x":0,"y":0,"skX":90,"skY":90}},)"
        R"({"name":"child","parent":"root","inheritScale":false,"transform":{"x":100,"y":0}}],)"
        R"("slot":[{"name":"s0","parent":"child"}],)"
        R"("skin":[{"name":"","slot":[{"name":"s0","display":[{"name":"quad","type":"image"}]}]}],)"
        R"("animation":[{"name":"idle","duration":1}]}]})";
    TexCb cb;
    mq_db_data* data = mq_db_data_create(skeleton, std::strlen(skeleton), atlas, "", tex_cb, &cb);
    check(data != nullptr, "inheritScale 素材解析");
    if (!data) return;
    mq_db_instance* inst = mq_db_instance_create(data, 0);
    check(inst != nullptr, "inheritScale 实例");
    if (inst) {
        Drawn d;
        check(draw(inst, d), "inheritScale 绘制");
        check(d.info.vertex_count == 4, "inheritScale：4 顶点");
        check(nearly(d.info.bounds_x, -10.0F) && nearly(d.info.bounds_w, 20.0F),
              "inheritScale=false 仍继承父**旋转**（x∈[-10,10]）");
        check(nearly(d.info.bounds_y, 90.0F) && nearly(d.info.bounds_h, 20.0F),
              "inheritScale=false 仍继承父**平移**（y∈[90,110]）");
        float bx = 0.0F, by = 0.0F;
        const int child = mq_db_instance_bone_find(inst, "child");
        check(child >= 0 && mq_db_instance_bone_world_position(inst, child, &bx, &by) == MQ_DB_OK,
              "inheritScale：child 骨骼查询");
        check(nearly(bx, 0.0F) && nearly(by, 100.0F), "child 世界位置 = R(90°)·(100,0)");
        mq_db_instance_destroy(inst);
    }
    mq_db_data_destroy(data);
}

/* 合成素材：裁剪（frameX/frameY）+ pivot 数学（官方 SFMLSlot 画法：quad=裁剪区，整块减 pivot） */
void test_synthetic_trim() {
    const char* atlas =
        R"({"name":"trim_tex","imagePath":"trim_tex.png","width":64,"height":64,)"
        R"("SubTexture":[{"name":"trim","x":0,"y":0,"width":24,"height":20,)"
        R"("frameX":-4,"frameY":-2,"frameWidth":32,"frameHeight":24}]})";
    const char* skeleton =
        R"({"name":"trim","version":"5.5","frameRate":24,"armature":[{"name":"trim","type":"Armature",)"
        R"("bone":[{"name":"root"}],"slot":[{"name":"s0","parent":"root"}],)"
        R"("skin":[{"name":"","slot":[{"name":"s0","display":[{"name":"trim","type":"image"}]}]}],)"
        R"("animation":[{"name":"idle","duration":1}]}]})";
    TexCb cb;
    mq_db_data* data = mq_db_data_create(skeleton, std::strlen(skeleton), atlas, "", tex_cb, &cb);
    check(data != nullptr, "裁剪素材解析");
    if (!data) return;
    mq_db_instance* inst = mq_db_instance_create(data, 0);
    check(inst != nullptr, "裁剪实例");
    if (inst) {
        Drawn d;
        check(draw(inst, d), "裁剪素材绘制");
        /* pivotX = 0.5*32 + (-4) = 12 → left=-12；宽度 = 裁剪区 24 → [-12,12]
         * pivotY = 0.5*24 + (-2) = 10 → top=-10；高度 = 20 → [-10,10] */
        check(d.info.vertex_count == 4, "裁剪素材 4 顶点");
        check(nearly(d.info.bounds_x, -12.0F) && nearly(d.info.bounds_w, 24.0F),
              "裁剪：x ∈ [-12,12]（pivot 用未裁剪帧尺寸算）");
        check(nearly(d.info.bounds_y, -10.0F) && nearly(d.info.bounds_h, 20.0F),
              "裁剪：y ∈ [-10,10]");
        check(nearly(d.uvs[0], 0.0F) && nearly(d.uvs[1], 0.0F), "裁剪 UV 左上 = 区域左上");
        check(nearly(d.uvs[2], 24.0F / 64.0F) && nearly(d.uvs[3], 0.0F), "裁剪 UV 右上");
        check(nearly(d.uvs[5], 20.0F / 64.0F), "裁剪 UV 下边 = 区域高度/页高");
        mq_db_instance_destroy(inst);
    }
    mq_db_data_destroy(data);
}

/* 合成素材：3 个皮肤（a/b/空），验证皮肤切换 + 四边形 pivot 数学 + 空皮肤不画 */
void test_synthetic_skins() {
    const char* atlas =
        R"({"name":"syn_tex","imagePath":"syn_tex.png","width":64,"height":64,)"
        R"("SubTexture":[{"name":"tex_a","x":0,"y":0,"width":32,"height":32},)"
        R"({"name":"tex_b","x":32,"y":0,"width":32,"height":32}]})";
    const char* skeleton =
        R"({"name":"syn","version":"5.5","frameRate":24,"armature":[{"name":"syn","type":"Armature",)"
        R"("bone":[{"name":"root"}],)"
        R"("slot":[{"name":"s0","parent":"root"}],)"
        R"("skin":[{"name":"a","slot":[{"name":"s0","display":[{"name":"tex_a","type":"image"}]}]},)"
        R"({"name":"b","slot":[{"name":"s0","display":[{"name":"tex_b","type":"image"}]}]},)"
        R"({"name":"empty","slot":[]}],)"
        R"("animation":[{"name":"idle","duration":1,"playTimes":0}]}]})";

    TexCb cb;
    mq_db_data* data = mq_db_data_create(skeleton, std::strlen(skeleton), atlas, "", tex_cb, &cb);
    check(data != nullptr, "合成素材解析");
    if (!data) return;
    check(mq_db_data_skin_count(data, 0) == 3, "合成素材 3 个皮肤");
    check(std::string(mq_db_data_skin_name(data, 0, 0)) == "a", "皮肤 0 = a");
    check(mq_db_data_skin_index(data, 0, "b") == 1, "皮肤 b 下标");

    mq_db_instance* inst = mq_db_instance_create(data, 0);
    check(inst != nullptr, "合成实例");
    if (inst) {
        check(mq_db_instance_set_skin(inst, "a") == MQ_DB_OK, "set_skin(a)");
        check(std::string(mq_db_instance_skin_name(inst)) == "a", "读回皮肤 a");
        Drawn a;
        check(draw(inst, a), "皮肤 a 绘制");
        check(a.info.vertex_count == 4 && a.info.index_count == 6, "皮肤 a = 一个四边形");
        check(nearly(a.info.bounds_x, -16.0F) && nearly(a.info.bounds_w, 32.0F), "pivot=0.5 → 包围盒 [-16,16]");
        check(nearly(a.uvs[0], 0.0F) && nearly(a.uvs[1], 0.0F) && nearly(a.uvs[2], 0.5F), "tex_a 的 UV 落在图集左上 32px");

        check(mq_db_instance_set_skin(inst, "b") == MQ_DB_OK, "set_skin(b)");
        Drawn b;
        check(draw(inst, b), "皮肤 b 绘制");
        check(nearly(b.uvs[0], 0.5F) && nearly(b.uvs[1], 0.0F), "tex_b 的 UV 落在图集右上");

        check(mq_db_instance_set_skin(inst, "empty") == MQ_DB_OK, "set_skin(empty)");
        Drawn e;
        check(draw(inst, e), "空皮肤绘制");
        check(e.info.vertex_count == 0 && e.info.index_count == 0, "空皮肤 → 零顶点（不画）");

        /* 实例 tint 写进顶点色 */
        mq_db_instance_set_skin(inst, "a");
        mq_db_instance_set_color(inst, 0.5F, 0.25F, 0.75F, 0.5F);
        Drawn t;
        check(draw(inst, t), "tint 绘制");
        check(nearly(t.cols[0], 0.5F) && nearly(t.cols[1], 0.25F) && nearly(t.cols[2], 0.75F) &&
                  nearly(t.cols[3], 0.5F), "实例 tint → 顶点色");
        mq_db_instance_destroy(inst);
    }
    mq_db_data_destroy(data);
}

void test_errors() {
    const char bin[8] = { 'D', 'B', 'D', 'T', 0, 0, 0, 0 };
    check(mq_db_data_create(bin, sizeof bin, nullptr, "", nullptr, nullptr) == nullptr, "二进制 .dbbin 明确拒绝");
    check(mq_db_data_create(nullptr, 0, nullptr, "", nullptr, nullptr) == nullptr, "空输入 → nullptr");
    check(mq_db_data_create("not json", 8, nullptr, "", nullptr, nullptr) == nullptr, "坏 JSON → nullptr");
    check(mq_db_data_armature_count(nullptr) == 0, "空句柄安全");
    check(mq_db_instance_draw(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr) == MQ_DB_ERR_ARG,
          "空参数 → ERR_ARG");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::print(stderr, "usage: maque_dragonbones_test <tests/data/dragonbones>\n");
        return 2;
    }
    const std::string dir = argv[1];
    test_real_mecha(dir);
    test_mesh(dir);
    test_synthetic_deg_rotate();
    test_synthetic_inherit();
    test_synthetic_trim();
    test_synthetic_skins();
    test_errors();
    if (failures != 0) {
        std::print(stderr, "dragonbones_test: {} failure(s)\n", failures);
        return 1;
    }
    std::print("dragonbones_test: ok\n");
    return 0;
}
