/* 导航实现（2D 网格）：A* + 视线平滑 + 确定性追踪。
 * 确定性要点：① 节点按格子索引升序的稳定优先队列；② 浮点只用普通运算（无 fast-math）；
 * ③ 不读墙钟/不用 std::random；④ 所有状态在实例内。 */
#include "nav/mq_nav.h"

#include <cmath>
#include <queue>
#include <vector>
#include <cstring>

namespace {

constexpr float kSqrt2 = 1.41421356237309504880F;

struct Node {
    float g = 0.0F;         // 起点到本格代价
    float f = 0.0F;         // g + h
    uint32_t parent = 0;    // 父格索引
    uint8_t  state = 0;     // 0 未访问 1 开启 2 关闭
    uint8_t  walkable = 1;
};

} // namespace

struct mq_nav_world {
    uint32_t width = 0, height = 0;
    float cell = 1.0F;
    bool diagonal = true;
    std::vector<Node> nodes;
    /* 稳定优先队列：先比 f，再比格子索引（禁指针序，docs/10 D4） */
    struct QItem { float f; uint32_t index; };
    struct Cmp { bool operator()(const QItem& a, const QItem& b) const {
        if (a.f != b.f) return a.f > b.f;
        return a.index > b.index;
    } };
    std::priority_queue<QItem, std::vector<QItem>, Cmp> open;
    uint32_t last_expanded = 0;

    uint32_t idx(uint32_t x, uint32_t y) const { return y * width + x; }
    bool in_range(uint32_t x, uint32_t y) const { return x < width && y < height; }
    bool walkable(uint32_t x, uint32_t y) const {
        if (!in_range(x, y)) return false;
        return nodes[idx(x, y)].walkable != 0;
    }
    float wx(uint32_t x) const { return ((float)x + 0.5F) * cell; }
    float wy(uint32_t y) const { return ((float)y + 0.5F) * cell; }
};

extern "C" {

mq_nav_world* mq_nav_create(const mq_nav_config* cfg) {
    if (!cfg) return nullptr;
    mq_nav_world* w = new (std::nothrow) mq_nav_world();
    if (!w) return nullptr;
    const uint32_t width = cfg->struct_size == 0 ? 32u : cfg->width;
    const uint32_t height = cfg->struct_size == 0 ? 32u : cfg->height;
    const float cell = (cfg->struct_size == 0 || cfg->cell_size <= 0.0F) ? 1.0F : cfg->cell_size;
    if (width == 0 || height == 0 || width > 4096u || height > 4096u) { delete w; return nullptr; }
    w->width = width; w->height = height; w->cell = cell;
    w->diagonal = cfg->struct_size == 0 ? true : (cfg->allow_diagonal != 0);
    w->nodes.assign((size_t)width * height, Node{});
    return w;
}

void mq_nav_destroy(mq_nav_world* w) { delete w; }

int mq_nav_set_blocked(mq_nav_world* w, uint32_t x, uint32_t y, int blocked) {
    if (!w) return MQ_NAV_ERR_ARG;
    if (!w->in_range(x, y)) return MQ_NAV_ERR_RANGE;
    w->nodes[w->idx(x, y)].walkable = blocked ? 0 : 1;
    return MQ_NAV_OK;
}

int mq_nav_is_blocked(mq_nav_world* w, uint32_t x, uint32_t y) {
    if (!w || !w->in_range(x, y)) return 1;
    return w->nodes[w->idx(x, y)].walkable ? 0 : 1;
}

int mq_nav_set_rect(mq_nav_world* w, uint32_t x, uint32_t y, uint32_t width, uint32_t height, int blocked) {
    if (!w) return MQ_NAV_ERR_ARG;
    for (uint32_t j = y; j < y + height; ++j)
        for (uint32_t i = x; i < x + width; ++i)
            if (w->in_range(i, j)) w->nodes[w->idx(i, j)].walkable = blocked ? 0 : 1;
    return MQ_NAV_OK;
}

uint32_t mq_nav_walkable_count(mq_nav_world* w) {
    if (!w) return 0;
    uint32_t n = 0;
    for (auto& node : w->nodes) if (node.walkable) ++n;
    return n;
}

int mq_nav_cell_at(mq_nav_world* w, float wx, float wy, uint32_t* out_x, uint32_t* out_y) {
    if (!w) return MQ_NAV_ERR_ARG;
    if (wx < 0.0F || wy < 0.0F) return MQ_NAV_ERR_RANGE;
    const uint32_t x = (uint32_t)(wx / w->cell);
    const uint32_t y = (uint32_t)(wy / w->cell);
    if (!w->in_range(x, y)) return MQ_NAV_ERR_RANGE;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    return MQ_NAV_OK;
}

void mq_nav_cell_center(mq_nav_world* w, uint32_t x, uint32_t y, float* out_wx, float* out_wy) {
    if (!w) return;
    if (out_wx) *out_wx = w->wx(x);
    if (out_wy) *out_wy = w->wy(y);
}

int mq_nav_is_walkable_at(mq_nav_world* w, float wx, float wy) {
    uint32_t x = 0, y = 0;
    if (mq_nav_cell_at(w, wx, wy, &x, &y) != MQ_NAV_OK) return 0;
    return w->walkable(x, y) ? 1 : 0;
}

/* A*：稳定优先队列 + 升序索引；给出容器（path_xy 无容器时仅返回点数）。 */
int mq_nav_find_path(mq_nav_world* w, float sx, float sy, float tx, float ty,
                     float* out_xy, uint32_t out_cap, uint32_t* out_count, uint32_t* out_needed) {
    if (out_count) *out_count = 0;
    if (out_needed) *out_needed = 0;
    if (!w) return MQ_NAV_ERR_ARG;
    uint32_t sx_c = 0, sy_c = 0, tx_c = 0, ty_c = 0;
    if (mq_nav_cell_at(w, sx, sy, &sx_c, &sy_c) != MQ_NAV_OK) return MQ_NAV_ERR_RANGE;
    if (mq_nav_cell_at(w, tx, ty, &tx_c, &ty_c) != MQ_NAV_OK) return MQ_NAV_ERR_RANGE;
    if (!w->walkable(sx_c, sy_c) || !w->walkable(tx_c, ty_c)) return MQ_NAV_ERR_BLOCKED;

    const uint32_t start = w->idx(sx_c, sy_c);
    const uint32_t goal = w->idx(tx_c, ty_c);

    for (auto& n : w->nodes) { n.g = 0.0F; n.f = 0.0F; n.parent = 0; n.state = 0; }
    w->last_expanded = 0;

    auto heuristic = [&](uint32_t x, uint32_t y) {
        const float dx = (float)((int)x - (int)tx_c);
        const float dy = (float)((int)y - (int)ty_c);
        if (w->diagonal) {
            const float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            const float mn = ax < ay ? ax : ay;
            return (ax + ay - mn) + kSqrt2 * mn;   // 八向距离（admissible）
        }
        return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    };

    while (!w->open.empty()) w->open.pop();
    w->nodes[start].g = 0.0F;
    w->nodes[start].f = heuristic(sx_c, sy_c);
    w->nodes[start].state = 1;
    w->open.push({ w->nodes[start].f, start });

    bool found = false;
    while (!w->open.empty()) {
        const mq_nav_world::QItem top = w->open.top();
        w->open.pop();
        Node& cur = w->nodes[top.index];
        if (cur.state == 2) continue;                    // 过期项
        cur.state = 2;
        ++w->last_expanded;
        if (top.index == goal) { found = true; break; }

        const uint32_t cx = top.index % w->width;
        const uint32_t cy = top.index / w->width;
        static const int dirs8[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
        const int ndirs = w->diagonal ? 8 : 4;
        for (int d = 0; d < ndirs; ++d) {
            const int nx = (int)cx + dirs8[d][0];
            const int ny = (int)cy + dirs8[d][1];
            if (nx < 0 || ny < 0 || nx >= (int)w->width || ny >= (int)w->height) continue;
            const uint32_t nidx = w->idx((uint32_t)nx, (uint32_t)ny);
            if (!w->nodes[nidx].walkable) continue;
            const bool diag = dirs8[d][0] != 0 && dirs8[d][1] != 0;
            if (diag) {                                  // 不切角：两侧正交格必须可行走
                if (!w->walkable((uint32_t)nx, cy) || !w->walkable(cx, (uint32_t)ny)) continue;
            }
            const float step = diag ? kSqrt2 : 1.0F;
            const float ng = cur.g + step;
            Node& nb = w->nodes[nidx];
            if (nb.state != 0 && ng >= nb.g) continue;
            nb.g = ng;
            nb.f = ng + heuristic((uint32_t)nx, (uint32_t)ny);
            nb.parent = top.index;
            nb.state = 1;
            w->open.push({ nb.f, nidx });
        }
    }
    if (!found) return MQ_NAV_ERR_NOPATH;

    /* 回溯 → 反向转正 */
    std::vector<uint32_t> cells;
    for (uint32_t c = goal;; c = w->nodes[c].parent) {
        cells.push_back(c);
        if (c == start) break;
    }
    const uint32_t count = (uint32_t)cells.size();
    if (out_needed) *out_needed = count * 2;             // 所需浮点数（= 点数 × 2）
    if (!out_xy || out_cap < count * 2) {
        if (out_count) *out_count = count;                 // 报告点数（所需浮点数见 out_needed）
        return count ? MQ_NAV_ERR_CAPACITY : MQ_NAV_OK;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = cells[count - 1 - i];
        out_xy[i * 2 + 0] = w->wx(c % w->width);
        out_xy[i * 2 + 1] = w->wy(c / w->width);
    }
    if (out_count) *out_count = count;
    return MQ_NAV_OK;
}

/* 视线：Bresenham 超覆盖（每步检查相邻两格），保证"穿墙不可见"。 */
int mq_nav_line_of_sight(mq_nav_world* w, float ax, float ay, float bx, float by) {
    if (!w) return 0;
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (mq_nav_cell_at(w, ax, ay, &x0, &y0) != MQ_NAV_OK) return 0;
    if (mq_nav_cell_at(w, bx, by, &x1, &y1) != MQ_NAV_OK) return 0;
    int ix0 = (int)x0, iy0 = (int)y0;
    const int ix1 = (int)x1, iy1 = (int)y1;
    const int dx = std::abs(ix1 - ix0), dy = std::abs(iy1 - iy0);
    const int sx = ix0 < ix1 ? 1 : -1, sy = iy0 < iy1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        if (!w->walkable((uint32_t)ix0, (uint32_t)iy0)) return 0;
        if (ix0 == ix1 && iy0 == iy1) return 1;
        const int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; ix0 += sx; }
        if (e2 < dx)  { err += dx; iy0 += sy; }
    }
}

uint32_t mq_nav_smooth(mq_nav_world* w, float* xy, uint32_t count) {
    if (!w || !xy || count < 3) return count;
    /* 压缩：保留起点，随后只保留"视线不可达"处的前一个点，最后必保终点。 */
    uint32_t out = 1;
    for (uint32_t i = 2; i < count; ++i) {
        const uint32_t anchor = out - 1;
        if (!mq_nav_line_of_sight(w, xy[anchor * 2], xy[anchor * 2 + 1], xy[i * 2], xy[i * 2 + 1])) {
            xy[out * 2] = xy[(i - 1) * 2];
            xy[out * 2 + 1] = xy[(i - 1) * 2 + 1];
            ++out;
        }
    }
    const uint32_t last = count - 1;                     // 终点
    if (out == 0 || xy[(out - 1) * 2] != xy[last * 2] || xy[(out - 1) * 2 + 1] != xy[last * 2 + 1]) {
        xy[out * 2] = xy[last * 2];
        xy[out * 2 + 1] = xy[last * 2 + 1];
        ++out;
    }
    return out;
}

int mq_nav_step_towards(mq_nav_world* w, float* px, float* py, const float* path_xy,
                        uint32_t count, uint32_t* index, float speed, float dt,
                        float arrive_dist) {
    if (!w || !px || !py || !path_xy || !index) return -1;
    if (*index >= count) return 1;                        // 已到终点
    const float tx = path_xy[*index * 2];
    const float ty = path_xy[*index * 2 + 1];
    float dx = tx - *px, dy = ty - *py;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist <= arrive_dist) {
        *px = tx; *py = ty;
        ++(*index);
        return *index >= count ? 1 : 0;
    }
    const float step = speed * dt;
    if (step >= dist) { *px = tx; *py = ty; ++(*index); return *index >= count ? 1 : 0; }
    *px += dx / dist * step;
    *py += dy / dist * step;
    return 0;
}

uint32_t mq_nav_last_expanded(mq_nav_world* w) { return w ? w->last_expanded : 0u; }

} // extern "C"
