/* maque 渲染器接口（后端无关，docs/13）。
 * 世界/场景只产出中立的 DrawItem；具体后端（WebGPU）实现本接口。
 * 解耦：Scene ↔ 图形后端 之间只隔这一层。 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace maque::render {

/* 后端无关绘制项：由世界层产出，渲染器消费。texture < 0 表示实心精灵。 */
struct DrawItem {
    float x = 0, y = 0;     // 屏幕像素（y 向下）
    float size = 0;         // 半径/半尺寸
    float r = 1, g = 1, b = 1, a = 1;
    int   texture = -1;     // 后端纹理 id（-1 = 实心）
};

/* 视图：多屏/VR。vx..vh 为目标内视口；pan 平移；eye 眼距/视差。 */
struct View { float vx, vy, vw, vh, pan, eye; };

/* 渲染后端接口。所有调用均在渲染线程。 */
class Renderer {
public:
    virtual ~Renderer() = default;

    /* 上传 RGBA8 纹理，返回纹理 id（供 DrawItem.texture）。失败 <0。 */
    virtual int  create_texture(const uint8_t* rgba8, int w, int h) = 0;
    virtual bool has_texture(int id) const = 0;

    /* 多视图离屏渲染 + readback 到 out_rgba（TW*TH*4, RGBA8）。 */
    virtual void render_views(int TW, int TH, const View* views, size_t nviews,
                              const DrawItem* items, size_t count,
                              const float bg[4],
                              std::vector<uint8_t>& out_rgba) = 0;
};

/* 工厂：创建默认后端（WebGPU；实现在 maque::gfx）。不可用返回 nullptr。 */
Renderer* create_renderer(std::string* out_backend = nullptr);

} // namespace maque::render
