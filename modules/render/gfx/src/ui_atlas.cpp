/* UiAtlas 实现：shelf 打包 + first-fit 空闲表（见 ui_atlas.h 的布局说明）。 */
#include "ui_atlas.h"

#include <algorithm>

namespace maque::gfx {

void UiAtlas::configure(int32_t size, bool enabled) {
    size_ = std::clamp(size, 64, 8192);
    enabled_ = enabled;
    reset();
}

void UiAtlas::reset() {
    free_.clear();
    used_ = 0;
    shelf_y_ = 0;
    shelf_h_ = 0;
    shelf_x_ = 0;
}

AtlasRegion UiAtlas::pack(int32_t w, int32_t h) {
    if (!enabled_ || w <= 0 || h <= 0) return {};
    const int32_t bw = w + 2 * kPadding;
    const int32_t bh = h + 2 * kPadding;
    if (bw > size_ || bh > size_) return {};

    /* ① 已释放的空间：first-fit + 右侧切分（高度整块给，避免纵向碎片） */
    for (size_t i = 0; i < free_.size(); ++i) {
        FreeRect& f = free_[i];
        if (f.w < bw || f.h < bh) continue;
        const int32_t x = f.x, y = f.y;
        if (f.w > bw) {
            f.x += bw;
            f.w -= bw;
        } else {
            free_[i] = free_.back();
            free_.pop_back();
        }
        used_ += w * h;
        return AtlasRegion{ x + kPadding, y + kPadding, w, h };
    }

    /* ② 当前货架（同一行内向右排） */
    if (shelf_h_ > 0 && bh <= shelf_h_ && shelf_x_ + bw <= size_) {
        const int32_t x = shelf_x_;
        shelf_x_ += bw;
        used_ += w * h;
        return AtlasRegion{ x + kPadding, shelf_y_ + kPadding, w, h };
    }

    /* ③ 开新货架 */
    if (shelf_y_ + shelf_h_ + bh > size_) return {};
    shelf_y_ += shelf_h_;
    shelf_h_ = bh;
    shelf_x_ = bw;
    used_ += w * h;
    return AtlasRegion{ kPadding, shelf_y_ + kPadding, w, h };
}

void UiAtlas::release(const AtlasRegion& r) {
    if (!r.valid()) return;
    if (r.w <= size_ && r.h <= size_) {
        used_ -= r.w * r.h;
        if (used_ < 0) used_ = 0;
    }
    /* 收回**含边**占位；不合并相邻块（UI 小图数量有限，碎片可控） */
    free_.push_back(FreeRect{ r.x - kPadding, r.y - kPadding,
                              r.w + 2 * kPadding, r.h + 2 * kPadding });
}

}  // namespace maque::gfx
