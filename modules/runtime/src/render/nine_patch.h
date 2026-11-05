/* 9-patch（九宫格）几何：一个矩形 → 最多 9 个普通图元。
 *
 * 纯函数、不依赖 Engine/wgpu：源边距按**源图像素**解释，目标尺寸按设计单位解释。
 * 角块保持源尺寸（目标过小时按比例压缩）；边块单轴拉伸；中心块双轴拉伸。
 * 相邻块共享同一浮点边界 → 不会出现缝。返回的四元组中心以**容器中心**为原点，
 * y 向下（与 mq_renderer_item 的屏幕坐标一致）；旋转由调用方做。
 *
 * 为什么展开成 9 个 item 而不是改 shader：
 *   · 9 块共享同一贴图/采样/裁剪，UiBatcher 会把它们合成**一次 drawcall**；
 *   · 不改图元 ABI、不改内建 shader、不破坏 UI 动态合图（UV 逐块重映射）；
 *   · 每块 UV 独立 → 图集/翻转/region 都自然正确。 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace maque::rt {

struct NinePatchQuad {
    float cx = 0.0F, cy = 0.0F;      /* 中心（相对容器中心，y 向下） */
    float hw = 0.0F, hh = 0.0F;      /* 半尺寸 */
    float u0 = 0.0F, v0 = 0.0F;      /* 归一化 UV：左上 */
    float u1 = 0.0F, v1 = 0.0F;      /* 右下 */
};

struct NinePatchParams {
    float dst_w = 0.0F, dst_h = 0.0F;          /* 目标矩形（设计单位） */
    float src_x = 0.0F, src_y = 0.0F;          /* 源区域左上（纹理像素） */
    float src_w = 0.0F, src_h = 0.0F;          /* 源区域尺寸（纹理像素） */
    float tex_w = 0.0F, tex_h = 0.0F;          /* 整张纹理尺寸（像素；UV 归一化） */
    float margin_l = 0.0F, margin_t = 0.0F;    /* 源边距（纹理像素） */
    float margin_r = 0.0F, margin_b = 0.0F;
    bool flip_h = false, flip_v = false;
};

/* 生成九宫格四元组。任何退化输入（无目标/无源/无纹理尺寸）返回空。 */
inline std::vector<NinePatchQuad> nine_patch_quads(const NinePatchParams& p) {
    std::vector<NinePatchQuad> out;
    out.reserve(9);
    if (!(p.dst_w > 0.0F) || !(p.dst_h > 0.0F)) return out;
    if (!(p.src_w > 0.0F) || !(p.src_h > 0.0F)) return out;
    if (!(p.tex_w > 0.0F) || !(p.tex_h > 0.0F)) return out;

    /* 源边距：先夹到非负，若和超过源尺寸则等比压缩（避免中间块负宽）。 */
    float sl = std::max(0.0F, p.margin_l);
    float st = std::max(0.0F, p.margin_t);
    float sr = std::max(0.0F, p.margin_r);
    float sb = std::max(0.0F, p.margin_b);
    if (sl + sr > p.src_w) { const float k = p.src_w / (sl + sr); sl *= k; sr *= k; }
    if (st + sb > p.src_h) { const float k = p.src_h / (st + sb); st *= k; sb *= k; }

    /* 目标边距：源边距为目标尺寸；目标过小时再压缩，保证 9 块不重叠。 */
    float dl = sl, dr = sr, dt = st, db = sb;
    if (dl + dr > p.dst_w) { const float k = p.dst_w / (dl + dr); dl *= k; dr *= k; }
    if (dt + db > p.dst_h) { const float k = p.dst_h / (dt + db); dt *= k; db *= k; }

    const float src_cols[3] = { sl, p.src_w - sl - sr, sr };
    const float src_rows[3] = { st, p.src_h - st - sb, sb };
    const float dst_cols[3] = { dl, p.dst_w - dl - dr, dr };
    const float dst_rows[3] = { dt, p.dst_h - dt - db, db };

    float sy = p.src_y;
    float dy = 0.0F;
    for (int row = 0; row < 3; ++row) {
        if (!(src_rows[row] > 0.0F) || !(dst_rows[row] > 0.0F)) { sy += src_rows[row]; dy += dst_rows[row]; continue; }
        float sx = p.src_x;
        float dx = 0.0F;
        for (int col = 0; col < 3; ++col) {
            if (!(src_cols[col] > 0.0F) || !(dst_cols[col] > 0.0F)) { sx += src_cols[col]; dx += dst_cols[col]; continue; }
            NinePatchQuad q{};
            q.cx = -p.dst_w * 0.5F + dx + dst_cols[col] * 0.5F;
            q.cy = -p.dst_h * 0.5F + dy + dst_rows[row] * 0.5F;
            q.hw = dst_cols[col] * 0.5F;
            q.hh = dst_rows[row] * 0.5F;
            q.u0 = sx / p.tex_w;
            q.v0 = sy / p.tex_h;
            q.u1 = (sx + src_cols[col]) / p.tex_w;
            q.v1 = (sy + src_rows[row]) / p.tex_h;
            if (p.flip_h) { q.cx = -q.cx; std::swap(q.u0, q.u1); }
            if (p.flip_v) { q.cy = -q.cy; std::swap(q.v0, q.v1); }
            out.push_back(q);
            sx += src_cols[col];
            dx += dst_cols[col];
        }
        sy += src_rows[row];
        dy += dst_rows[row];
    }
    return out;
}

}  // namespace maque::rt
