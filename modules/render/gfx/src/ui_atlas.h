/* UI 动态合图（dynamic atlas）：把 UI 图片（ui.Image 的贴图）打进一张**预分配**的
 * 1024×1024 纹理，让小图共享同一次 drawcall。
 *
 * 与**字体图集**是两个独立系统（用户明确要求分开）：
 *   · 字体图集：text 模块按 (字体, 字号) 动态增长的字形位图（512×H），由 text 模块自己管理；
 *   · UI 图集：本类，只收"UI 图片"（runtime 经 create_texture2 创建的 2D 贴图）。
 * 分类依据在 gfx 侧：`Texture::ui_atlas_src` 只有 create_texture2 的 2D 路径会置位；
 * 字体图集走旧的 create_texture → 永远不会被 UI 图集吸走。
 *
 * 本文件是**纯 CPU 的空间分配器**（不依赖 wgpu，可单测）：gfx 负责建纹理/拷像素/重映射 UV。
 *
 * 布局：shelf（货架）打包 —— 每个区域按 `w+2·pad × h+2·pad` 占位，内容落在 (x, y)。
 * 四周留 1px 外扩边：线性采样在贴图边缘本来走 clamp-to-edge，图集里必须把边像素复制出去
 * （gfx 用 8 条 1px 的 GPU copy 完成），否则边缘会混到相邻小图。
 */
#pragma once

#include <cstdint>
#include <vector>

namespace maque::gfx {

/* 图集里的内容矩形（像素）：x/y = 内容左上（**不含**外扩边）。 */
struct AtlasRegion {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    bool valid() const { return w > 0 && h > 0; }
};

class UiAtlas {
public:
    static constexpr int32_t kPadding = 1;   /* 每边 1px 外扩（线性采样 clamp 语义） */

    /* 预分配 size×size；enabled=false 时 pack() 一律失败（MAQUE_UI_ATLAS=0）。 */
    void configure(int32_t size, bool enabled);
    bool enabled() const { return enabled_; }
    int32_t size() const { return size_; }
    int32_t used_pixels() const { return used_; }
    int32_t capacity_pixels() const { return size_ * size_; }

    /* 申请 w×h 的内容区域（失败 → 无效区域，调用方退回原贴图）。 */
    AtlasRegion pack(int32_t w, int32_t h);
    /* 归还（贴图销毁/图集重建）；相同尺寸可被后续 pack 复用。 */
    void release(const AtlasRegion& r);
    void reset();

private:
    struct FreeRect {
        int32_t x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<FreeRect> free_;   /* 已释放的**含边**矩形（first-fit + 右切分） */
    int32_t size_ = 1024;
    bool enabled_ = true;
    int32_t used_ = 0;             /* 已占用的**内容**像素（不含边） */
    int32_t shelf_y_ = 0;          /* 当前货架顶部（含边的坐标） */
    int32_t shelf_h_ = 0;          /* 当前货架高度（含边） */
    int32_t shelf_x_ = 0;          /* 当前货架游标（含边） */
};

}  // namespace maque::gfx
