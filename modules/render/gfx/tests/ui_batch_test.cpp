/* UiBatcher 纯逻辑单测（不依赖 wgpu / 窗口）：合批决策 = drawcall 数 + 提交顺序。
 *
 * 覆盖：
 *   ① 画家序模式：相邻同键合并，键一变就新建批次；
 *   ② UI 分组：solid / 字体图集交叉出现 → 只要顺序不可见就并成每键一批；
 *   ③ 顺序安全：面板后画在文字上（会与分组顺序冲突）→ 切段，面板仍在文字之后；
 *   ④ 同键图元即使重叠也保持画家序（组内顺序 = 入组顺序）；
 *   ⑤ 网格图元是屏障（前/后图元不得并成同一批）；
 *   ⑥ 退化图元不影响批次；⑦ 越界图元 → 退回画家序；
 *   ⑧ 旋转矩形的 AABB 参与重叠判定；⑨ 逐字节确定性。 */
#include "ui_batch.h"
#include "ui_atlas.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace maque::gfx;

namespace {

int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

BatchInstance make_rect(float x, float y, float hw, float hh, float rot = 0.0F) {
    BatchInstance i{};
    i.pos[0] = x;
    i.pos[1] = y;
    i.radius = hw > hh ? hw : hh;
    i.rot = rot;
    i.color[0] = i.color[1] = i.color[2] = i.color[3] = 1.0F;
    i.half[0] = hw;
    i.half[1] = hh;
    i.shape = 1.0F;
    return i;
}

const BatchKey kSolid{ -1, false };
const BatchKey kAtlas0{ 0, true };
const BatchKey kAtlas1{ 1, true };

/* ① 画家序：同键相邻合并；换键新建。 */
void painter_order_merges_adjacent() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 200.0F, 200.0F);
    b.begin_layer(0, false);
    b.add_item(make_rect(10, 10, 5, 5), kSolid);
    b.add_item(make_rect(30, 10, 5, 5), kSolid);
    b.add_item(make_rect(50, 10, 5, 5), kAtlas0);
    b.add_item(make_rect(70, 10, 5, 5), kAtlas0);
    b.add_item(make_rect(90, 10, 5, 5), kSolid);
    b.end_layer();
    b.end();
    CHECK(b.draws().size() == 3);
    CHECK(b.instances().size() == 5);
    CHECK(b.draws()[0].count == 2);
    CHECK(b.draws()[1].count == 2);
    CHECK(b.draws()[2].count == 1);
    CHECK(b.stats().merges == 2);
    CHECK(b.stats().segments == 0);
}

/* ② UI 分组：按钮矩形 + 其上的字形（不越到后面的按钮）→ 每键一批。 */
void ui_grouping_merges_by_state() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 640.0F, 360.0F);
    b.begin_layer(1, true);
    b.add_item(make_rect(100, 100, 60, 24), kSolid);   /* 按钮 1 */
    b.add_item(make_rect(100, 100, 8, 10), kAtlas0);   /* 压在按钮 1 上的字 */
    b.add_item(make_rect(300, 100, 60, 24), kSolid);   /* 按钮 2（与上面的字不相交） */
    b.add_item(make_rect(300, 100, 8, 10), kAtlas0);   /* 压在按钮 2 上的字 */
    b.end_layer();
    b.end();
    CHECK(b.draws().size() == 2);
    CHECK(b.stats().segments == 0);
    CHECK(b.draws()[0].key == kSolid && b.draws()[0].count == 2);
    CHECK(b.draws()[1].key == kAtlas0 && b.draws()[1].count == 2);
    /* 组内顺序 = 入组顺序（画家序），不因分组改变同键图元相对次序 */
    CHECK(b.instances()[0].pos[0] == 100.0F);
    CHECK(b.instances()[1].pos[0] == 300.0F);
    CHECK(b.instances()[2].pos[0] == 100.0F);
    CHECK(b.instances()[3].pos[0] == 300.0F);
}

/* ③ 顺序反转：后画的实心面板压在此前文字上 → 必须切段，面板仍在文字之后。 */
void ui_grouping_splits_on_order_inversion() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 640.0F, 360.0F);
    b.begin_layer(1, true);
    b.add_item(make_rect(100, 100, 60, 24), kSolid);
    b.add_item(make_rect(100, 100, 8, 10), kAtlas0);   /* 文字在按钮之上 */
    b.add_item(make_rect(100, 100, 60, 24), kSolid);   /* 后画面板盖住该文字 */
    b.end_layer();
    b.end();
    CHECK(b.stats().segments == 1);
    CHECK(b.draws().size() == 3);                      /* 段 1：solid + atlas；段 2：solid */
    CHECK(b.draws()[0].key == kSolid);
    CHECK(b.draws()[1].key == kAtlas0);
    CHECK(b.draws()[2].key == kSolid);                 /* 面板没有被挪到文字下面 */
}

/* ④ 同一键的图元即使互相重叠也不切段（组内顺序 = 画家序，结果不变）。 */
void same_key_overlap_keeps_paint_order() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 200.0F, 200.0F);
    b.begin_layer(1, true);
    b.add_item(make_rect(50, 50, 20, 20), kAtlas0);
    b.add_item(make_rect(55, 55, 20, 20), kAtlas0);
    b.end_layer();
    b.end();
    CHECK(b.draws().size() == 1);
    CHECK(b.draws()[0].count == 2);
    CHECK(b.instances()[0].pos[0] == 50.0F);
    CHECK(b.instances()[1].pos[0] == 55.0F);
}

/* ⑤ 网格是屏障：其前后的同键图元不得并成同一批。 */
void mesh_is_a_barrier() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 200.0F, 200.0F);
    b.begin_layer(0, true);
    b.add_item(make_rect(10, 10, 5, 5), kSolid);
    b.add_mesh(7, 3);
    b.add_item(make_rect(30, 10, 5, 5), kSolid);
    b.end_layer();
    b.end();
    CHECK(b.draws().size() == 3);
    CHECK(b.draws()[0].kind == BatchDraw::Kind::Instanced);
    CHECK(b.draws()[1].kind == BatchDraw::Kind::Mesh);
    CHECK(b.draws()[1].mesh == 7);
    CHECK(b.draws()[1].key.texture == 3);
    CHECK(b.draws()[2].kind == BatchDraw::Kind::Instanced);
    CHECK(b.stats().meshes == 1);
}

/* ⑥ 退化图元（零面积）不产生可见覆盖，也不应该把批次拆开。 */
void degenerate_items_are_harmless() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 200.0F, 200.0F);
    b.begin_layer(1, true);
    b.add_item(make_rect(10, 10, 5, 5), kSolid);
    b.add_item(make_rect(50, 50, 0, 0), kAtlas0);      /* 零面积 */
    b.add_item(make_rect(90, 10, 5, 5), kSolid);
    b.end_layer();
    b.end();
    /* kAtlas0 只有退化图元 → 单独一批（没有像素），其余 solid 并成一批 */
    CHECK(b.instances().size() == 3);
    CHECK(b.draws().size() == 2);
    CHECK(b.draws()[0].key == kSolid && b.draws()[0].count == 2);
}

/* ⑦ 有面积但落在 begin() 的 bbox 外 → 放弃分组、退回画家序（正确性优先）。 */
void out_of_bounds_falls_back_to_painter() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 10.0F, 10.0F);
    b.begin_layer(1, true);
    b.add_item(make_rect(5, 5, 2, 2), kSolid);
    b.add_item(make_rect(1000, 1000, 5, 5), kAtlas0);  /* 越界 */
    b.add_item(make_rect(5, 5, 2, 2), kSolid);         /* 与第一个同键且重叠 */
    b.end_layer();
    b.end();
    CHECK(b.instances().size() == 3);
    CHECK(b.draws().size() == 3);                      /* 退回画家序：不合并（中间隔了 atlas） */
}

/* ⑧ 旋转矩形的包围盒：90° 旋转后 AABB 由 10×40 变成 40×10，重叠必须按旋转后的盒判定。
 *    后画的实心块落在"旋转后才覆盖"的区域 → 必须切段；按未旋转盒（x≤110）会漏判。 */
void rotated_bounds_are_used() {
    BatchInstance a = make_rect(100, 100, 10, 40, 1.5707963267948966F);   /* 90°：半宽 40 */
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    CHECK(UiBatcher::item_bounds(a, x0, y0, x1, y1, 0.0F));
    CHECK(std::fabs(x0 - 60.0F) < 1e-3F && std::fabs(x1 - 140.0F) < 1e-3F);
    CHECK(std::fabs(y0 - 90.0F) < 1e-3F && std::fabs(y1 - 110.0F) < 1e-3F);

    UiBatcher b;
    b.begin(0.0F, 0.0F, 200.0F, 200.0F);
    b.begin_layer(1, true);
    b.add_item(make_rect(100, 100, 5, 5), kSolid);                        /* 先画的小方块 */
    b.add_item(make_rect(100, 100, 10, 40, 1.5707963267948966F), kAtlas0); /* 横躺，覆盖右侧 */
    b.add_item(make_rect(130, 100, 2, 2), kSolid);                        /* 只在旋转后的盒内 */
    b.end_layer();
    b.end();
    CHECK(b.draws().size() == 3);
    CHECK(b.stats().segments == 1);
    CHECK(b.draws()[2].key == kSolid);   /* 后画的方块仍在最后（不能被并到段 1 的 solid 里） */
}

/* ⑨ 相同输入 → 逐字节相同的命令/实例序列（确定性）。 */
void deterministic_output() {
    auto build = [](std::vector<BatchDraw>& draws, std::vector<BatchInstance>& insts) {
        UiBatcher b;
        b.begin(0.0F, 0.0F, 320.0F, 240.0F);
        b.begin_layer(1, true);
        for (int i = 0; i < 24; ++i) {
            const float x = 20.0F + (float)(i % 6) * 48.0F;
            const float y = 20.0F + (float)(i / 6) * 48.0F;
            b.add_item(make_rect(x, y, 20.0F, 12.0F), kSolid);
            b.add_item(make_rect(x, y, 4.0F, 6.0F), kAtlas0);
            b.add_item(make_rect(x, y + 6.0F, 3.0F, 5.0F), kAtlas1);
        }
        b.end_layer();
        b.end();
        draws = b.draws();
        insts = b.instances();
    };
    std::vector<BatchDraw> d1, d2;
    std::vector<BatchInstance> i1, i2;
    build(d1, i1);
    build(d2, i2);
    CHECK(d1.size() == d2.size());
    CHECK(std::memcmp(d1.data(), d2.data(), d1.size() * sizeof(BatchDraw)) == 0);
    CHECK(i1.size() == i2.size());
    CHECK(std::memcmp(i1.data(), i2.data(), i1.size() * sizeof(BatchInstance)) == 0);
    CHECK(d1.size() == 3);                 /* solid / atlas0 / atlas1 各一批 */
    CHECK(i1.size() == 72);
}

/* 空输入 / 退化 bbox 不崩。 */
void empty_and_degenerate() {
    UiBatcher b;
    b.begin(5.0F, 5.0F, 5.0F, 5.0F);       /* min == max */
    b.begin_layer(1, true);
    b.end_layer();
    b.end();
    CHECK(b.draws().empty());
    CHECK(b.instances().empty());
    CHECK(b.stats().draws == 0);
}


/* ⑩ UI 图集空间分配：shelf 打包 + 1px 外扩边 + 不重叠 + 释放复用。
 *    与字体图集无关（这里只测纯 CPU 分配器；GPU 拷贝在 gfx，e2e 验证）。 */
void ui_atlas_packs_without_overlap() {
    UiAtlas a;
    a.configure(1024, true);
    CHECK(a.enabled());
    CHECK(a.size() == 1024);
    struct Item { AtlasRegion r; int w, h; };
    std::vector<Item> items;
    const int sizes[][2] = { {24, 24}, {24, 24}, {24, 24}, {48, 24}, {200, 60}, {300, 90}, {96, 48} };
    for (auto& s : sizes) {
        const AtlasRegion r = a.pack(s[0], s[1]);
        CHECK(r.valid());
        CHECK(r.x >= UiAtlas::kPadding && r.y >= UiAtlas::kPadding);
        CHECK(r.x + r.w + UiAtlas::kPadding <= a.size());
        CHECK(r.y + r.h + UiAtlas::kPadding <= a.size());
        items.push_back(Item{ r, s[0], s[1] });
    }
    /* 含 1px 外扩边的占位矩形两两不重叠 */
    auto pad_rect = [](const AtlasRegion& r) {
        return std::array<int, 4>{ r.x - UiAtlas::kPadding, r.y - UiAtlas::kPadding,
                                   r.w + 2 * UiAtlas::kPadding, r.h + 2 * UiAtlas::kPadding };
    };
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) {
            const auto A = pad_rect(items[i].r), B = pad_rect(items[j].r);
            const bool sep = A[0] + A[2] <= B[0] || B[0] + B[2] <= A[0] ||
                             A[1] + A[3] <= B[1] || B[1] + B[3] <= A[1];
            CHECK(sep);
        }
    }
    CHECK(a.used_pixels() == 24 * 24 * 3 + 48 * 24 + 200 * 60 + 300 * 90 + 96 * 48);
}

/* ⑪ 释放后同尺寸可复用；关闭/超尺寸/塞满 → 无效区域。 */
void ui_atlas_release_and_limits() {
    UiAtlas a;
    a.configure(256, true);
    const AtlasRegion r = a.pack(100, 100);
    CHECK(r.valid());
    a.release(r);
    const AtlasRegion r2 = a.pack(100, 100);
    CHECK(r2.valid());
    CHECK(r2.x == r.x && r2.y == r.y);          /* first-fit 复用同一块 */

    CHECK(!a.pack(300, 10).valid());             /* 超过图集边长 */
    CHECK(!a.pack(0, 10).valid());
    CHECK(!a.pack(10, 0).valid());

    a.configure(128, false);                     /* MAQUE_UI_ATLAS=0 */
    CHECK(!a.enabled());
    CHECK(!a.pack(8, 8).valid());

    a.configure(64, true);
    int packed = 0;
    while (a.pack(8, 8).valid()) ++packed;
    CHECK(packed > 0 && packed < 64);            /* 塞满后明确失败（不越界、不死循环） */
}

/* ⑫ 裁剪区互不相交的两个图元即使完整 AABB 重叠，也可以安全分组（reverse 条带）。 */
void disjoint_clip_bands_merge() {
    UiBatcher b;
    b.begin(0.0F, 0.0F, 100.0F, 100.0F);
    b.begin_layer(1, true);
    BatchKey left{ -1, false };
    left.clip[0] = 0.0F; left.clip[1] = 0.0F; left.clip[2] = 50.0F; left.clip[3] = 100.0F;
    BatchKey right{ -1, false };
    right.clip[0] = 50.0F; right.clip[1] = 0.0F; right.clip[2] = 50.0F; right.clip[3] = 100.0F;
    b.add_item(make_rect(50, 50, 10, 10), left);
    b.add_item(make_rect(50, 50, 10, 10), right);   /* 完整 AABB 重叠，但可见区不相交 */
    b.end_layer();
    b.end();
    CHECK(b.draws().size() == 2);                  /* 两个裁剪状态各一批 */
    CHECK(b.stats().segments == 0);                /* 不应因粗网格边界切段 */
}

/* ⑫ 相同序列 → 相同布局（确定性）；reset 回到初始状态。 */
void ui_atlas_deterministic() {
    auto build = [] {
        UiAtlas a;
        a.configure(512, true);
        std::vector<AtlasRegion> out;
        for (int i = 0; i < 12; ++i) out.push_back(a.pack(20 + i * 3, 12 + i));
        return out;
    };
    const auto a1 = build();
    const auto a2 = build();
    CHECK(a1.size() == a2.size());
    for (size_t i = 0; i < a1.size(); ++i) {
        CHECK(a1[i].x == a2[i].x && a1[i].y == a2[i].y);
        CHECK(a1[i].w == a2[i].w && a1[i].h == a2[i].h);
    }
    UiAtlas a;
    a.configure(512, true);
    (void)a.pack(64, 64);
    a.reset();
    CHECK(a.used_pixels() == 0);
    const AtlasRegion r = a.pack(16, 16);
    CHECK(r.x == UiAtlas::kPadding && r.y == UiAtlas::kPadding);
}

}  // namespace

int main() {
    painter_order_merges_adjacent();
    ui_grouping_merges_by_state();
    ui_grouping_splits_on_order_inversion();
    same_key_overlap_keeps_paint_order();
    mesh_is_a_barrier();
    degenerate_items_are_harmless();
    out_of_bounds_falls_back_to_painter();
    rotated_bounds_are_used();
    deterministic_output();
    empty_and_degenerate();
    ui_atlas_packs_without_overlap();
    ui_atlas_release_and_limits();
    ui_atlas_deterministic();
    disjoint_clip_bands_merge();
    if (g_fail) {
        std::printf("ui_batch: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ui_batch: ok\n");
    return 0;
}
