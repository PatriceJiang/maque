/* UiBatcher 实现：见 ui_batch.h 顶部的设计说明。 */
#include "ui_batch.h"

#include <algorithm>
#include <cmath>

namespace maque::gfx {
namespace {

constexpr float kTile = 8.0F;      /* 设计单位 / 格；更粗 = 更保守（只少合批，不改结果） */
constexpr int kMaxSide = 512;      /* 最多 512×512 格（1MB uint32） */
constexpr uint32_t kRankMask = 0xFFFFu;
constexpr uint32_t kGenMask = 0xFFFF0000u;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* 两个裁剪矩形是否相交（w/h<=0 = 不裁剪 = 与任何区域相关）。 */
bool clip_rects_overlap(const float a[4], const float b[4]) {
    if (!(a[2] > 0.0F) || !(a[3] > 0.0F) || !(b[2] > 0.0F) || !(b[3] > 0.0F)) return true;
    return a[0] < b[0] + b[2] && a[0] + a[2] > b[0] &&
           a[1] < b[1] + b[3] && a[1] + a[3] > b[1];
}

}  // namespace

bool UiBatcher::item_bounds(const BatchInstance& inst, float& x0, float& y0, float& x1, float& y1,
                            float bleed, const float* clip) {
    /* 与 shader 的取尺寸规则一致：half 两轴都 > 0 才是矩形，否则用 radius（圆/方）。 */
    const bool rect = inst.half[0] > 0.0F && inst.half[1] > 0.0F;
    const float hw = rect ? inst.half[0] : inst.radius;
    const float hh = rect ? inst.half[1] : inst.radius;
    if (!(hw > 0.0F) || !(hh > 0.0F)) return false;          /* 退化图元：画不出像素 */
    if (!std::isfinite(inst.pos[0]) || !std::isfinite(inst.pos[1]) ||
        !std::isfinite(inst.rot) || !std::isfinite(hw) || !std::isfinite(hh))
        return false;
    const float c = std::abs(std::cos(inst.rot));
    const float s = std::abs(std::sin(inst.rot));
    const float ex = hw * c + hh * s + bleed;
    const float ey = hw * s + hh * c + bleed;
    x0 = inst.pos[0] - ex; x1 = inst.pos[0] + ex;
    y0 = inst.pos[1] - ey; y1 = inst.pos[1] + ey;
    if (clip && clip[2] > 0.0F && clip[3] > 0.0F) {
        const float cx0 = clip[0], cy0 = clip[1];
        const float cx1 = clip[0] + clip[2], cy1 = clip[1] + clip[3];
        x0 = std::max(x0, cx0); y0 = std::max(y0, cy0);
        x1 = std::min(x1, cx1); y1 = std::min(y1, cy1);
        if (!(x1 > x0) || !(y1 > y0)) return false;
    }
    return true;
}

void UiBatcher::begin(float min_x, float min_y, float max_x, float max_y, float bleed) {
    draws_.clear();
    insts_.clear();
    seg_.clear();
    stats_ = {};
    grouping_ = false;
    open_ = false;
    last_draw_ = 0;
    bleed_ = (bleed > 0.0F && std::isfinite(bleed)) ? bleed : 1.0F;
    if (!std::isfinite(min_x) || !std::isfinite(min_y) || !std::isfinite(max_x) || !std::isfinite(max_y)) {
        min_x = min_y = 0.0F; max_x = max_y = 1.0F;
    }
    if (!(max_x > min_x)) max_x = min_x + 1.0F;
    if (!(max_y > min_y)) max_y = min_y + 1.0F;
    /* 格数上限 512²：跨度过大时自动放粗格子（保守方向），保证内存有界。 */
    const float span = std::max(max_x - min_x, max_y - min_y);
    tile_ = std::max(kTile, span / (float)kMaxSide);
    cols_ = clampi((int)std::ceil((max_x - min_x) / tile_) + 1, 1, kMaxSide);
    rows_ = clampi((int)std::ceil((max_y - min_y) / tile_) + 1, 1, kMaxSide);
    origin_x_ = min_x;
    origin_y_ = min_y;
    const size_t n = (size_t)cols_ * (size_t)rows_;
    if (cells_.size() != n) cells_.assign(n, 0u);
    else std::fill(cells_.begin(), cells_.end(), 0u);
    gen_ = 1;
}

void UiBatcher::begin_layer(int32_t layer, bool group) {
    end_layer();
    (void)layer;                       /* 顺序由调用方保证（已按 layer → z → seq 排序） */
    grouping_ = group;
    ++stats_.layers;
}

void UiBatcher::end_layer() {
    if (grouping_) flush_segment();
    grouping_ = false;
    /* 画家序模式**保留** open_：与旧实现一致，相邻且同键的图元可跨图层合并。 */
}

void UiBatcher::end() {
    end_layer();
    stats_.draws = (uint32_t)draws_.size();
}

void UiBatcher::add_item(const BatchInstance& inst, const BatchKey& key) {
    ++stats_.items;
    if (!grouping_) { add_painter(inst, key); return; }

    int rank = rank_of(key);
    if (rank < 0) {
        rank = (int)seg_.size();
        seg_.push_back(Bucket{ key, {} });
    }
    int tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
    if (!tiles_for(inst, key.clip, tx0, ty0, tx1, ty1)) {
        float x0, y0, x1, y1;
        if (!item_bounds(inst, x0, y0, x1, y1, bleed_, key.clip)) {
            /* 退化图元：不覆盖像素，对顺序没有影响 → 直接入组。 */
            if (!seg_[rank].insts.empty()) ++stats_.merges;
            seg_[rank].insts.push_back(inst);
            return;
        }
        /* 有面积却落在 begin() 的预扫描 bbox 外 → 无法做重叠判定。放弃本帧分组，
         * 退回画家序（正确性优先；调用方应传入整帧图元的 bbox）。 */
        flush_segment();
        grouping_ = false;
        add_painter(inst, key);
        return;
    }
    if (order_unsafe(tx0, ty0, tx1, ty1, (uint32_t)rank, key.clip)) {
        /* 分组会把当前图元画到某个更早图元之前，而二者包围盒相交 → 切段。 */
        flush_segment();
        ++stats_.segments;
        rank = (int)seg_.size();
        seg_.push_back(Bucket{ key, {} });
    }
    const bool merge = !seg_[(size_t)rank].insts.empty();
    stamp(tx0, ty0, tx1, ty1, (uint32_t)rank);
    seg_[(size_t)rank].insts.push_back(inst);
    if (merge) ++stats_.merges;
}

void UiBatcher::add_mesh(uint32_t mesh_index, int32_t texture) {
    if (grouping_) flush_segment();    /* 网格是屏障：之后的图元不得跨过它合批 */
    open_ = false;
    BatchDraw d;
    d.kind = BatchDraw::Kind::Mesh;
    d.key.texture = texture;
    d.mesh = mesh_index;
    d.first = mesh_index;
    d.count = 1;
    draws_.push_back(d);
    ++stats_.meshes;
}

void UiBatcher::flush_segment() {
    for (Bucket& b : seg_) {
        if (b.insts.empty()) continue;
        BatchDraw d;
        d.kind = BatchDraw::Kind::Instanced;
        d.key = b.key;
        d.first = (uint32_t)insts_.size();
        d.count = (uint32_t)b.insts.size();
        insts_.insert(insts_.end(), b.insts.begin(), b.insts.end());
        draws_.push_back(d);
    }
    seg_.clear();
    reset_grid();
    open_ = false;
}

void UiBatcher::add_painter(const BatchInstance& inst, const BatchKey& key) {
    if (open_ && last_draw_ < draws_.size()) {
        BatchDraw& d = draws_[last_draw_];
        if (d.kind == BatchDraw::Kind::Instanced && d.key == key) {
            ++d.count;
            insts_.push_back(inst);
            ++stats_.merges;
            return;
        }
    }
    BatchDraw d;
    d.kind = BatchDraw::Kind::Instanced;
    d.key = key;
    d.first = (uint32_t)insts_.size();
    d.count = 1;
    insts_.push_back(inst);
    draws_.push_back(d);
    last_draw_ = draws_.size() - 1;
    open_ = true;
}

int UiBatcher::rank_of(const BatchKey& key) const {
    for (size_t i = 0; i < seg_.size(); ++i)
        if (seg_[i].key == key) return (int)i;
    return -1;
}

bool UiBatcher::tiles_for(const BatchInstance& inst, const float* clip,
                          int& tx0, int& ty0, int& tx1, int& ty1) const {
    float x0, y0, x1, y1;
    if (!item_bounds(inst, x0, y0, x1, y1, bleed_, clip)) return false;
    const float gx0 = (x0 - origin_x_) / tile_;
    const float gy0 = (y0 - origin_y_) / tile_;
    const float gx1 = (x1 - origin_x_) / tile_;
    const float gy1 = (y1 - origin_y_) / tile_;
    if (gx1 < 0.0F || gy1 < 0.0F || gx0 > (float)cols_ || gy0 > (float)rows_) return false;
    /* 半开区间 [x0,x1)：恰好共享边界的两个图元不算重叠（9-patch 相邻块 / reverse 的
     * 上下左右条带都靠这一点）；否则边界格会把它们判成相交，分组被无谓切段。 */
    tx0 = clampi((int)std::floor(gx0), 0, cols_ - 1);
    ty0 = clampi((int)std::floor(gy0), 0, rows_ - 1);
    tx1 = clampi((int)std::ceil(gx1) - 1, 0, cols_ - 1);
    ty1 = clampi((int)std::ceil(gy1) - 1, 0, rows_ - 1);
    return true;
}

bool UiBatcher::order_unsafe(int tx0, int ty0, int tx1, int ty1, uint32_t rank,
                             const float* clip) const {
    const uint32_t g = gen_ << 16;
    for (int y = ty0; y <= ty1; ++y) {
        const uint32_t* row = cells_.data() + (size_t)y * (size_t)cols_;
        for (int x = tx0; x <= tx1; ++x) {
            const uint32_t cell = row[x];
            if ((cell & kGenMask) != g) continue;
            const uint32_t other = cell & kRankMask;
            if (other <= rank) continue;
            /* 两个图元落在**互不相交的裁剪区**时，谁先画都看不到对方（scissor 保证），
             * 粗网格在边界上的“重叠”不应阻止分组。reverse 的上/下/左/右条带正是这种情形。 */
            if (other < seg_.size() && !clip_rects_overlap(clip, seg_[other].key.clip)) continue;
            return true;
        }
    }
    return false;
}

void UiBatcher::stamp(int tx0, int ty0, int tx1, int ty1, uint32_t rank) {
    const uint32_t g = gen_ << 16;
    for (int y = ty0; y <= ty1; ++y) {
        uint32_t* row = cells_.data() + (size_t)y * (size_t)cols_;
        for (int x = tx0; x <= tx1; ++x) {
            uint32_t& cell = row[x];
            if ((cell & kGenMask) != g) cell = g | rank;
            else if ((cell & kRankMask) < rank) cell = g | rank;
        }
    }
}

void UiBatcher::reset_grid() {
    /* 分段用 generation 隔离：旧段的标记自动失效，无需清整张网格。 */
    ++gen_;
    if (gen_ >= 0xFFFFu) {
        std::fill(cells_.begin(), cells_.end(), 0u);
        gen_ = 1;
    }
}

}  // namespace maque::gfx
