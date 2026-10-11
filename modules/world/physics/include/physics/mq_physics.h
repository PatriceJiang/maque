/* maque physics —— 确定性 2D 刚体/碰撞 C ABI（docs/15 §3、docs/10、docs/02）。
 * 纯 C 头（C11 干净，无 std/C++ 类型/STL）：跨 .so 只暴露本 vtable + 世代句柄。
 * 确定性：固定步长、固定迭代、按 body.index 稳定排序；禁墙钟/线程/std::random。
 * 坐标系：像素单位、y 轴向下；重力默认 (0, 900) 像素/秒²。
 * 内存所有权：world 由 mq_physics_create 分配、mq_physics_destroy 释放（同一模块）；
 * save_state 的缓冲由调用方提供。 */
#ifndef MAQUE_PHYSICS_H
#define MAQUE_PHYSICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQ_PHYSICS_ABI_VERSION 1u
#define MQ_PHYSICS_IFACE_NAME "maque.physics.v1"

/* ---------- 不透明世界（模块分配/释放；宿主只持有指针，不解引用） ---------- */
typedef struct mq_physics_world mq_physics_world;

/* ---------- 世代句柄（index 0 恒为无效；移除即 generation++ 防悬垂） ---------- */
typedef struct mq_physics_body {
    uint32_t index;
    uint32_t generation;
} mq_physics_body;

#define MQ_PHYSICS_BODY_INVALID_INDEX 0xFFFFFFFFu

/* ---------- 错误码（无异常越界；0 = OK） ---------- */
enum {
    MQ_PHYSICS_OK = 0,
    MQ_PHYSICS_ERR_INVALID_ARG = 1,     /* 空指针 / 非法数值 / 越界参数 */
    MQ_PHYSICS_ERR_INVALID_HANDLE = 2,  /* slot 空或 generation 过期 */
    MQ_PHYSICS_ERR_OUT_OF_MEMORY = 3,   /* 容量分配失败（不抛异常越界） */
    MQ_PHYSICS_ERR_CAPACITY = 4,        /* 已达 max_bodies，宿主需扩容/复用 */
    MQ_PHYSICS_ERR_NOT_SUPPORTED = 5    /* 形状/语义不在 v1 范围 */
};

enum { MQ_PHYSICS_BODY_STATIC = 0, MQ_PHYSICS_BODY_KINEMATIC = 1, MQ_PHYSICS_BODY_DYNAMIC = 2 };
enum { MQ_PHYSICS_SHAPE_BALL = 0, MQ_PHYSICS_SHAPE_CUBOID = 1 };

/* body flags：bit0 锁定旋转（2D 无角自由度，no-op，保留位语义）；bit1 关闭重力。 */
#define MQ_PHYSICS_BF_LOCK_ROTATION (1u << 0)
#define MQ_PHYSICS_BF_DISABLE_GRAVITY (1u << 1)

/* ---------- 世界配置（struct_size 版本化，只增不改） ---------- */
typedef struct mq_physics_config {
    uint32_t struct_size; /* = sizeof(mq_physics_config)；0 → 全部字段取默认 */
    float gravity_x;      /* 默认 0 */
    float gravity_y;      /* 默认 900（屏幕 y 向下，像素/秒²） */
    float fixed_dt;       /* 默认 1/60；必须 > 0 */
    uint32_t solver_iterations; /* 默认 4；每步恒跑该次数（不随负载变化） */
    uint32_t max_bodies;        /* 容量，默认 256；0 = 默认 */
    uint64_t seed;              /* 引擎 seed（docs/10 D2）；v1 求解器无 RNG，仅记录 */
} mq_physics_config;

/* ---------- 刚体描述（struct_size 版本化） ---------- */
typedef struct mq_physics_body_desc {
    uint32_t struct_size;
    uint32_t type;      /* MQ_PHYSICS_BODY_* */
    uint32_t shape;     /* MQ_PHYSICS_SHAPE_* */
    float x, y;         /* 位置（像素，y 向下） */
    float vx, vy;       /* 速度（像素/秒） */
    float radius;       /* BALL：> 0 */
    float half_w, half_h; /* CUBOID：均 > 0 */
    float mass;         /* <= 0 → 由密度推导（默认质量 1） */
    float restitution;  /* 0..1，默认 0.5 */
    float friction;     /* 0..1，默认 0.2 */
    uint32_t flags;     /* 位0 锁旋转（no-op）；位1 关闭重力 */
} mq_physics_body_desc;

/* ---------- 生命周期 ---------- */
/* cfg 为 NULL → 全默认。失败返回 NULL（无异常越界）。 */
mq_physics_world* mq_physics_create(const mq_physics_config* cfg);
void mq_physics_destroy(mq_physics_world* w);

/* ---------- 刚体 ---------- */
int mq_physics_add_body(mq_physics_world* w, const mq_physics_body_desc* d, mq_physics_body* out);
int mq_physics_remove_body(mq_physics_world* w, mq_physics_body b);
int mq_physics_body_count(mq_physics_world* w);   /* -1: 世界无效 */
int mq_physics_body_valid(mq_physics_world* w, mq_physics_body b); /* 1/0；-1: 世界无效 */
int mq_physics_get_transform(mq_physics_world* w, mq_physics_body b, float* x, float* y);
int mq_physics_set_transform(mq_physics_world* w, mq_physics_body b, float x, float y);
int mq_physics_get_velocity(mq_physics_world* w, mq_physics_body b, float* vx, float* vy);
int mq_physics_set_velocity(mq_physics_world* w, mq_physics_body b, float vx, float vy);

/* prev/curr 线性插值：只读，绝不回写模拟状态（docs/15 §3、plans/13 §8）。
 * alpha 不裁剪；alpha=0 → prev，alpha=1 → curr。 */
int mq_physics_interpolate(mq_physics_world* w, mq_physics_body b, float alpha, float* x, float* y);

/* ---------- 静态平面墙（世界边界；参与 DYNAMIC 体碰撞与接触报告） ---------- */
int mq_physics_set_bounds(mq_physics_world* w, float min_x, float min_y, float max_x, float max_y);

/* ---------- 推进（固定 dt 取自 cfg；确定；按 body.index 升序） ---------- */
int mq_physics_step(mq_physics_world* w);
uint32_t mq_physics_step_count(mq_physics_world* w);

/* ---------- 确定性校验和：FNV-1a，按 body.index 升序覆盖全部体状态（docs/10 §5） ---------- */
uint64_t mq_physics_checksum(mq_physics_world* w);

/* ---------- 本帧接触（稳定按 (a_index,b_index) 升序；平面接触 a_index = 体、b_index = PLANE） ---------- */
#define MQ_PHYSICS_PLANE_INDEX 0xFFFFFFFFu
typedef struct mq_physics_contact {
    uint32_t a_index, b_index; /* 恒有 a_index < b_index（平面接触 b_index = PLANE） */
    float nx, ny;              /* 单位法线：a → b 方向 */
    float depth;               /* 穿透深度 >= 0 */
} mq_physics_contact;
uint32_t mq_physics_contact_count(mq_physics_world* w);
int mq_physics_contact_at(mq_physics_world* w, uint32_t i, mq_physics_contact* out);

/* ---------- 快照 / 回滚（docs/10 §4；POD 字节，index 升序，往返逐位一致） ---------- */
size_t mq_physics_state_size(mq_physics_world* w);
/* 写入至多 cap 字节；返回实际需要的字节数（> cap 表示未写入，需扩容）。 */
size_t mq_physics_save_state(mq_physics_world* w, void* out, size_t cap);
int mq_physics_load_state(mq_physics_world* w, const void* in, size_t size);

/* ---------- 插件 vtable（docs/02 §3）：跨 .so 的唯一函数表 ---------- */
typedef struct mq_physics_v1 {
    uint32_t abi_version;
    const char* name;
    mq_physics_world* (*create)(const mq_physics_config*);
    void (*destroy)(mq_physics_world*);
    int (*add_body)(mq_physics_world*, const mq_physics_body_desc*, mq_physics_body*);
    int (*remove_body)(mq_physics_world*, mq_physics_body);
    int (*step)(mq_physics_world*);
    int (*get_transform)(mq_physics_world*, mq_physics_body, float*, float*);
    int (*set_transform)(mq_physics_world*, mq_physics_body, float, float);
    int (*set_velocity)(mq_physics_world*, mq_physics_body, float, float);
    uint64_t (*checksum)(mq_physics_world*);
} mq_physics_v1;

/* 主版本匹配返回静态 const vtable，否则 NULL。 */
const mq_physics_v1* mq_physics_query(uint32_t abi_version);

#ifdef __cplusplus
}
#endif

#endif /* MAQUE_PHYSICS_H */
