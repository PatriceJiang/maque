/* maque 导航（docs/15 §持续：导航；2D 网格切片）：**确定性**寻路 + 路径平滑。
 *
 * 与设计文档的对应：
 *   · 模拟层模块：固定步长驱动、单线程、无墙钟、禁 std::random（docs/10 D1/D2/D6）；
 *   · 迭代顺序按**格子索引升序**与**稳定优先队列**（同 f 按插入序）→ 逐位可复现（docs/10 D4）；
 *   · per-instance：全部状态在 mq_nav_world 内，无全局可变状态（ADR-0014）；
 *   · 3D/NavMesh 版（nav.NavAgent 的 Vec3 + NavMeshData）为后续扩展：
 *     本切片先把"网格可行走性 + A* + 视线平滑"这套确定性内核跑通。
 *
 * 坐标语义：格子 (x, y) 覆盖 [x*cell, (x+1)*cell) × [y*cell, (y+1)*cell)；
 * 世界坐标 y 向下（与 runtime/physics 一致）。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_nav_world mq_nav_world;

enum {
    MQ_NAV_OK            = 0,
    MQ_NAV_ERR_ARG       = 1,
    MQ_NAV_ERR_RANGE     = 2,   /* 越界/尺寸不合法 */
    MQ_NAV_ERR_BLOCKED   = 3,   /* 起点或终点不可行走 */
    MQ_NAV_ERR_NOPATH    = 4,   /* 无可行路径 */
    MQ_NAV_ERR_NOMEM     = 5,
    MQ_NAV_ERR_CAPACITY  = 6    /* 路径缓冲不足 */
};

typedef struct mq_nav_config {
    uint32_t struct_size;
    uint32_t width;        /* 网格列数（>0） */
    uint32_t height;       /* 网格行数（>0） */
    float    cell_size;    /* 每格世界尺寸（像素/米），>0 */
    uint8_t  allow_diagonal; /* 1 = 允许八向移动（对角移动不切角） */
} mq_nav_config;

mq_nav_world* mq_nav_create(const mq_nav_config* cfg);
void          mq_nav_destroy(mq_nav_world* w);

/* 可行走性：默认全部可行走；用 set_blocked/set_walkable 逐格设置（编辑器/AI 可改）。 */
int mq_nav_set_blocked(mq_nav_world* w, uint32_t x, uint32_t y, int blocked);
int mq_nav_is_blocked(mq_nav_world* w, uint32_t x, uint32_t y);
int mq_nav_set_rect(mq_nav_world* w, uint32_t x, uint32_t y, uint32_t width, uint32_t height, int blocked);
uint32_t mq_nav_walkable_count(mq_nav_world* w);

/* 世界坐标 ↔ 格子。 */
int mq_nav_cell_at(mq_nav_world* w, float wx, float wy, uint32_t* out_x, uint32_t* out_y);
void mq_nav_cell_center(mq_nav_world* w, uint32_t x, uint32_t y, float* out_wx, float* out_wy);
/* 世界坐标是否可行走（越界视为不可行走）。 */
int mq_nav_is_walkable_at(mq_nav_world* w, float wx, float wy);

/* 寻路：结果写入 out_xy（交错 [x0,y0,x1,y1,...]，世界坐标），返回**点数**。
 * 缓冲不足返回 MQ_NAV_ERR_CAPACITY（并写出所需点数到 *out_needed）。 */
int mq_nav_find_path(mq_nav_world* w, float sx, float sy, float tx, float ty,
                     float* out_xy, uint32_t out_cap, uint32_t* out_count, uint32_t* out_needed);

/* 视线可达（Bresenham 超覆盖；用于路径平滑与 AI 判断）。 */
int mq_nav_line_of_sight(mq_nav_world* w, float ax, float ay, float bx, float by);

/* 平滑：在保证视线可达的前提下删除冗余点（原地压缩；返回新点数）。 */
uint32_t mq_nav_smooth(mq_nav_world* w, float* xy, uint32_t count);

/* 一步追踪（确定性）：朝当前路径点前进，返回 0 正常；1 = 已到终点；<0 错误。
 * speed 单位：世界单位/秒；dt 固定步长。 */
int mq_nav_step_towards(mq_nav_world* w, float* px, float* py, const float* path_xy,
                        uint32_t count, uint32_t* index, float speed, float dt,
                        float arrive_dist);

/* 诊断/自省：最后一次寻路的扩展节点数与耗时无关统计（确定性，可用于预算）。 */
uint32_t mq_nav_last_expanded(mq_nav_world* w);

#ifdef __cplusplus
}
#endif
