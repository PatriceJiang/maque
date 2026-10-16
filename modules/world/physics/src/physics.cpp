/* maque physics 模块实现（确定性 2D 求解器，docs/15 §3、docs/10 D1–D8、plans/13）。
 *
 * 求解语义（v1，冻结）：
 *   1) 积分（半隐式 Euler）：DYNAMIC 体（未置 DISABLE_GRAVITY）v += g*dt; p += v*dt；
 *      KINEMATIC 体仅按自身速度平移；STATIC 体不动。逐体按 body.index 升序。
 *   2) 窄相：存活体按 index 升序稳定配对 i<j（球-球 / 球-AABB 盒 / AABB-AABB），
 *      再按升序追加世界边界平面接触；接触表恒按 (a_index,b_index) 升序。
 *   3) 位置修正：先按穿透量沿法线一次性完全分离（只移动 DYNAMIC 体；法线 a → b）。
 *   4) 速度求解：恒定 solver_iterations 次迭代；法向目标分离速度 = -e*approach
 *      （approach 慢于阈值时取 0，避免静息反复弹跳），合并恢复系数 e = max(ea, eb)
 *      （显式选择：取大者，使"高弹地面"可被单个物件抬高）；
 *      切向按合并摩擦 f = max(fa, fb) 对相对切向速度做指数阻尼 (1 - f)：只衰减不反向。
 *   5) 静息钳制：接触面上法向速度小于 (0.5 + 1.5*|g|*dt) 像素/秒时清零 —— 确定性、
 *      与负载无关，消除落定后的持续抖动。
 * 边界：世界边界为静态平面（深度取各平面重叠量），DYNAMIC 体取自身恢复系数/摩擦。
 * 坐标约定（权威，见 docs/15 §3.1）：物理世界**就是**世界坐标，**y 向上**。
 *   · 重力是矢量：`gravity_y < 0` = 屏幕向下（默认 -900）；`> 0` = 向上加速。
 *   · 边界平面：`min_y` 是 y 小的一侧（世界 y 向上 → 屏幕下沿），`max_y` 是 y 大的一侧（屏幕上沿）；
 *     四条平面都向内推体（min_y 平面法线 (0,-1)，max_y 平面 (0,+1)）。体被挡在
 *     `[min + 外接半径, max - 外接半径]` 内（BALL 取 radius，CUBOID 取 half_w/half_h）。
 *   · 接触法线恒为 a → b（平面接触 = 内向法线、b_index = PLANE）。
 * 确定性：无墙钟 / 无线程 / 无 std::random / 无指针序；浮点仅普通 float 运算；
 *          体表、存活索引表与接触缓冲均在 create 时一次性预留，step 内零分配。
 * 每实例状态：全部在 mq_physics_world 内（docs/02 §5 零全局可变状态）。 */

#include "physics/mq_physics.h"

#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace {

/* 默认重力 y 分量：世界坐标 y 向上 → 屏幕向下 = -y。 */
constexpr float kDefaultGravityY = -900.0F;
constexpr float kDefaultFixedDt = 1.0F / 60.0F;
constexpr uint32_t kDefaultSolverIterations = 4;
constexpr uint32_t kDefaultMaxBodies = 256;

/* 静息速度阈值：低于此速度且接触法向非分离时清零（像素/秒，确定性、与负载无关）。 */
constexpr float kRestVelocityEpsilon = 0.5F;
/* 恢复系数生效的最小接近速度：更慢的接近只抵消法向速度，不产生弹跳（消抖）。 */
constexpr float kRestitutionVelocityThreshold = 1.0F;
/* 接触判定容差：位置修正到恰好相切（穿透 = 0）后接触仍保留，
 * 使静息体每步都被求解（否则会先自由下落一帧、再被弹起 → 持续抖动）。 */
constexpr float kContactSlop = 0.25F;
/* 法线退化阈值（中心重合 / 零向量）。 */
constexpr float kNormalEpsilon = 1.0e-6F;

constexpr uint32_t kSnapshotMagic = 0x5053514Du; /* "MQSP" (LE) */
constexpr uint32_t kSnapshotVersion = 1u;

/* 快照记录：POD 字节、按字段序列化（不 dump 含 padding 的裸结构，docs/10 D5）。 */
struct MqSnapshotHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t body_count;
    uint32_t max_bodies;
    uint64_t step_count;
};

struct MqSnapshotBody {
    uint32_t index;
    uint32_t generation;
    uint32_t type;
    uint32_t shape;
    uint32_t flags;
    float x, y;
    float px, py;
    float vx, vy;
    float radius;
    float half_w, half_h;
    float inv_mass_dynamic; /* type == DYNAMIC ? 1/mass : 0 */
    float restitution;
    float friction;
};

/* 快照记录必须无 padding（按字段序列化，docs/10 D5） */
static_assert(sizeof(MqSnapshotHeader) == 24u, "snapshot header must be padding-free");
static_assert(sizeof(MqSnapshotBody) == 68u, "snapshot body record must be padding-free");

struct PhysicsBody {
    uint32_t generation = 0;
    uint32_t type = MQ_PHYSICS_BODY_STATIC;
    uint32_t shape = MQ_PHYSICS_SHAPE_BALL;
    uint32_t flags = 0;
    float x = 0.0F, y = 0.0F;
    float prev_x = 0.0F, prev_y = 0.0F;
    float vx = 0.0F, vy = 0.0F;
    float radius = 0.0F;
    float half_w = 0.0F, half_h = 0.0F;
    float inv_mass_dynamic = 0.0F;
    float restitution = 0.5F;
    float friction = 0.2F;
};

struct BodySlot {
    PhysicsBody body{};
    uint32_t generation = 0; /* 0 = 空槽；分配后从 1 起，释放即自增 */
    bool live = false;
};

struct Bounds {
    bool enabled = false;
    float min_x = 0.0F, min_y = 0.0F, max_x = 0.0F, max_y = 0.0F;
};

struct Contact {
    uint32_t a_index = 0;
    uint32_t b_index = 0;
    float nx = 0.0F, ny = 0.0F;
    float depth = 0.0F;
    bool plane = false; /* b_index == PLANE：a 对静态平面 */
};

/* 穿透 → 接触（允许 depth 略负：静息相切体保持接触）。 */
bool contact_from_depth(float nx, float ny, float depth, float& out_nx, float& out_ny,
                        float& out_depth) {
    if (depth < -kContactSlop)
        return false;
    out_nx = nx;
    out_ny = ny;
    out_depth = (depth > 0.0F) ? depth : 0.0F;
    return true;
}

} // namespace

struct mq_physics_world {
    std::vector<BodySlot> slots;
    std::vector<uint32_t> live; /* 存活 index，恒升序（稳定遍历序，docs/10 D4） */
    uint32_t max_bodies = kDefaultMaxBodies;
    uint32_t generation_base = 0;

    float gravity_x = 0.0F;
    float gravity_y = kDefaultGravityY;
    float fixed_dt = kDefaultFixedDt;
    uint32_t solver_iterations = kDefaultSolverIterations;
    uint64_t seed = 0;
    uint64_t step_count = 0;

    Bounds bounds{};
    std::vector<Contact> contacts;
    uint32_t contact_cap = 0; /* contacts 预分配上限：step 内零分配（确定性） */
};

namespace {

bool is_movable(const PhysicsBody& b) {
    return b.type != MQ_PHYSICS_BODY_STATIC;
}

/* 接触缓冲上限：按体数折算并封顶（step 内不再分配；超出则丢弃尾部接触）。 */
constexpr uint32_t kMaxContacts = 8192u;

uint32_t contact_capacity(uint32_t max_bodies) {
    const uint64_t n = max_bodies;
    const uint64_t pairs = n * (n - 1) / 2 + 4 * n + 8;
    return static_cast<uint32_t>(pairs < kMaxContacts ? pairs : kMaxContacts);
}

void live_insert(std::vector<uint32_t>& live, uint32_t index) {
    size_t i = live.size();
    live.push_back(index);
    while (i > 0 && live[i - 1] > index) {
        live[i] = live[i - 1];
        --i;
    }
    live[i] = index;
}

void live_erase(std::vector<uint32_t>& live, uint32_t index) {
    for (size_t i = 0; i < live.size(); ++i) {
        if (live[i] == index) {
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

uint32_t fnv1a_bytes(uint32_t hash, const void* data, size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint32_t>(p[i]);
        hash *= 16777619u;
    }
    return hash;
}

template <typename T> void hash_scalar(uint32_t& hash, const T& value) {
    hash = fnv1a_bytes(hash, &value, sizeof(T));
}

PhysicsBody* lookup(mq_physics_world* w, mq_physics_body handle) {
    if (w == nullptr || handle.index == 0u || handle.index >= w->slots.size())
        return nullptr;
    BodySlot& slot = w->slots[handle.index];
    if (!slot.live || slot.generation != handle.generation)
        return nullptr;
    return &slot.body;
}

/* 接触法线：a → b（文档化方向；调用方据 a_index 取值）。 */
bool ball_ball_contact(const PhysicsBody& a, const PhysicsBody& b, float& nx, float& ny,
                       float& depth) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float rsum = a.radius + b.radius;
    const float dist = (dx == 0.0F && dy == 0.0F) ? 0.0F : std::sqrt(dx * dx + dy * dy);
    if (dist > kNormalEpsilon) {
        nx = dx / dist;
        ny = dy / dist;
    } else {
        nx = 0.0F;
        ny = -1.0F; /* 中心重合：确定性退化法线 */
    }
    return contact_from_depth(nx, ny, rsum - dist, nx, ny, depth);
}

bool box_box_contact(const PhysicsBody& a, const PhysicsBody& b, float& nx, float& ny,
                     float& depth) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float ox = (a.half_w + b.half_w) - (dx < 0.0F ? -dx : dx);
    const float oy = (a.half_h + b.half_h) - (dy < 0.0F ? -dy : dy);
    if (ox < -kContactSlop || oy < -kContactSlop)
        return false;
    if (ox < oy) {
        nx = (dx >= 0.0F) ? 1.0F : -1.0F;
        ny = 0.0F;
        depth = ox;
    } else {
        nx = 0.0F;
        ny = (dy >= 0.0F) ? 1.0F : -1.0F;
        depth = oy;
    }
    return contact_from_depth(nx, ny, depth, nx, ny, depth);
}

/* 输出法线 ball → box。 */
bool ball_box_contact(const PhysicsBody& ball, const PhysicsBody& box, float& nx, float& ny,
                      float& depth) {
    const float cx = (ball.x < box.x - box.half_w)   ? box.x - box.half_w
                     : (ball.x > box.x + box.half_w) ? box.x + box.half_w
                                                     : ball.x;
    const float cy = (ball.y < box.y - box.half_h)   ? box.y - box.half_h
                     : (ball.y > box.y + box.half_h) ? box.y + box.half_h
                                                     : ball.y;
    const float dx = cx - ball.x; /* 球心 → 盒上最近点 */
    const float dy = cy - ball.y;
    const float dist_sq = dx * dx + dy * dy;
    if (dist_sq > kNormalEpsilon * kNormalEpsilon) {
        const float dist = std::sqrt(dist_sq);
        nx = dx / dist;
        ny = dy / dist;
        return contact_from_depth(nx, ny, ball.radius - dist, nx, ny, depth);
    }
    /* 球心落在盒内（最近点即球心）：取最小穿透轴（确定性）。 */
    const float ox = ball.x - box.x;
    const float oy = ball.y - box.y;
    const float pen_x = (ball.radius + box.half_w) - std::fabs(ox);
    const float pen_y = (ball.radius + box.half_h) - std::fabs(oy);
    if (pen_x < pen_y) {
        nx = (ox >= 0.0F) ? -1.0F : 1.0F; /* 法线指向盒（即把球推离盒的方向的反向） */
        ny = 0.0F;
        depth = pen_x;
    } else {
        nx = 0.0F;
        ny = (oy >= 0.0F) ? -1.0F : 1.0F;
        depth = pen_y;
    }
    return contact_from_depth(nx, ny, depth, nx, ny, depth);
}

/* 输出法线恒为 a → b。 */
bool body_pair_contact(const PhysicsBody& a, const PhysicsBody& b, float& nx, float& ny,
                       float& depth) {
    if (a.shape == MQ_PHYSICS_SHAPE_BALL && b.shape == MQ_PHYSICS_SHAPE_BALL)
        return ball_ball_contact(a, b, nx, ny, depth);
    if (a.shape == MQ_PHYSICS_SHAPE_CUBOID && b.shape == MQ_PHYSICS_SHAPE_CUBOID)
        return box_box_contact(a, b, nx, ny, depth);
    if (a.shape == MQ_PHYSICS_SHAPE_BALL)
        return ball_box_contact(a, b, nx, ny, depth);
    if (ball_box_contact(b, a, nx, ny, depth)) {
        nx = -nx; /* ball_box_contact 给 ball → box，翻转为 a → b */
        ny = -ny;
        return true;
    }
    return false;
}

/* 是否参与求解：STATIC 与 STATIC/KINEMATIC 之间不生成接触。 */
bool pair_active(const PhysicsBody& a, const PhysicsBody& b) {
    if (a.type != MQ_PHYSICS_BODY_STATIC && b.type != MQ_PHYSICS_BODY_STATIC)
        return true;
    return a.type == MQ_PHYSICS_BODY_DYNAMIC || b.type == MQ_PHYSICS_BODY_DYNAMIC;
}

/* ---------- 位置修正（先于速度求解，一次性分离；不写速度） ---------- */
void correct_positions(PhysicsBody& a, PhysicsBody& b, float nx, float ny, float depth) {
    if (depth <= 0.0F)
        return;
    const float wa = is_movable(a) ? a.inv_mass_dynamic : 0.0F;
    const float wb = is_movable(b) ? b.inv_mass_dynamic : 0.0F;
    const float wsum = wa + wb;
    if (wsum <= 0.0F)
        return;
    a.x -= depth * (wa / wsum) * nx;
    a.y -= depth * (wa / wsum) * ny;
    b.x += depth * (wb / wsum) * nx;
    b.y += depth * (wb / wsum) * ny;
}

void correct_position_plane(PhysicsBody& body, float nx, float ny, float depth) {
    if (depth <= 0.0F || !is_movable(body) || body.inv_mass_dynamic <= 0.0F)
        return;
    body.x -= depth * nx;
    body.y -= depth * ny;
}

/* ---------- 速度求解（不写位置） ----------
 * 法向：目标分离速度 = -e * approach（接近速度小于阈值时取 0，避免静息弹跳），
 *       取到达目标的冲量（非累积冲量迭代 → 多次迭代不叠加弹跳）。
 * 切向：指数阻尼（friction=1 → 消除相对切向速度；friction=0 → 不衰减）。 */
void impulse_contacts(PhysicsBody& a, PhysicsBody* b, float nx, float ny, float restitution,
                      float friction) {
    const float wa = is_movable(a) ? a.inv_mass_dynamic : 0.0F;
    const float wb = (b != nullptr && is_movable(*b)) ? b->inv_mass_dynamic : 0.0F;
    const float wsum = wa + wb;
    if (wsum <= 0.0F)
        return;

    const float rvx = (b != nullptr ? b->vx : 0.0F) - a.vx;
    const float rvy = (b != nullptr ? b->vy : 0.0F) - a.vy;
    const float approach = rvx * nx + rvy * ny; /* > 0：沿法线分离 */

    float bias = 0.0F;
    if (approach < -kRestitutionVelocityThreshold)
        bias = -restitution * approach;
    else if (approach < 0.0F)
        bias = -approach;
    float j = (bias - approach) / wsum;
    if (j < 0.0F)
        j = 0.0F;
    a.vx -= j * wa * nx;
    a.vy -= j * wa * ny;
    if (b != nullptr) {
        b->vx += j * wb * nx;
        b->vy += j * wb * ny;
    }

    const float rvx2 = (b != nullptr ? b->vx : 0.0F) - a.vx;
    const float rvy2 = (b != nullptr ? b->vy : 0.0F) - a.vy;
    const float vn = rvx2 * nx + rvy2 * ny;
    const float scale = 1.0F - friction;
    const float jt_x = (-(rvx2 - vn * nx) * scale) / wsum;
    const float jt_y = (-(rvy2 - vn * ny) * scale) / wsum;
    a.vx -= jt_x * wa;
    a.vy -= jt_y * wa;
    if (b != nullptr) {
        b->vx += jt_x * wb;
        b->vy += jt_y * wb;
    }
}

/* 按 (a_index,b_index) 稳定升序插入；达到上限即丢弃（确定性：与分配器状态无关）。 */
void contact_sort_insert(std::vector<Contact>& contacts, uint32_t cap, const Contact& c) {
    if (contacts.size() >= cap)
        return;
    size_t i = contacts.size();
    contacts.push_back(c);
    while (i > 0) {
        const Contact& prev = contacts[i - 1];
        const bool ordered = (prev.a_index < c.a_index) ||
                             (prev.a_index == c.a_index && prev.b_index <= c.b_index);
        if (ordered)
            break;
        contacts[i] = prev;
        --i;
    }
    contacts[i] = c;
}

/* 静息钳制（单侧）：接触面上法向速度量级不超过阈值时清零该法向分量，
 * 残余微量速度一并归零。仅作用于 DYNAMIC（KINEMATIC 由外部驱动，不被接触改写）。 */
void rest_clamp(PhysicsBody& body, float nx, float ny, float threshold) {
    if (body.type != MQ_PHYSICS_BODY_DYNAMIC)
        return;
    const float vn = body.vx * nx + body.vy * ny;
    if (vn > -threshold && vn < threshold) {
        body.vx -= vn * nx;
        body.vy -= vn * ny;
    }
    if (body.vx * body.vx + body.vy * body.vy <
        kRestVelocityEpsilon * kRestVelocityEpsilon) {
        body.vx = 0.0F;
        body.vy = 0.0F;
    }
}

void reset_body_to_defaults(PhysicsBody& body) {
    body.generation = 0;
    body.type = MQ_PHYSICS_BODY_STATIC;
    body.shape = MQ_PHYSICS_SHAPE_BALL;
    body.flags = 0;
    body.x = body.y = 0.0F;
    body.prev_x = body.prev_y = 0.0F;
    body.vx = body.vy = 0.0F;
    body.radius = 0.0F;
    body.half_w = body.half_h = 0.0F;
    body.inv_mass_dynamic = 0.0F;
    body.restitution = 0.5F;
    body.friction = 0.2F;
}

} // namespace

extern "C" {

mq_physics_world* mq_physics_create(const mq_physics_config* cfg) {
    mq_physics_config defaults{};
    defaults.struct_size = static_cast<uint32_t>(sizeof(mq_physics_config));
    defaults.gravity_x = 0.0F;
    defaults.gravity_y = kDefaultGravityY;
    defaults.fixed_dt = kDefaultFixedDt;
    defaults.solver_iterations = kDefaultSolverIterations;
    defaults.max_bodies = kDefaultMaxBodies;
    defaults.seed = 0;

    mq_physics_config resolved = defaults;
    if (cfg != nullptr) {
        if (cfg->struct_size != 0u && cfg->struct_size < sizeof(mq_physics_config))
            return nullptr; /* v1 要求完整结构（截断配置无法安全解释） */
        if (cfg->struct_size != 0u) {
            resolved.gravity_x = cfg->gravity_x;
            resolved.gravity_y = cfg->gravity_y;
            resolved.fixed_dt = cfg->fixed_dt;
            resolved.solver_iterations = cfg->solver_iterations;
            resolved.max_bodies = cfg->max_bodies;
            resolved.seed = cfg->seed;
        }
    }

    if (!(resolved.fixed_dt > 0.0F))
        return nullptr;
    uint32_t cap = resolved.max_bodies;
    if (cap == 0u)
        cap = kDefaultMaxBodies;
    if (cap > 65535u)
        return nullptr; /* 上限：槽表为 O(n) 预留，接触缓冲另有 8192 上限 */

    mq_physics_world* w = nullptr;
    try {
        w = new mq_physics_world();
        w->slots.resize(cap);
        w->contact_cap = contact_capacity(cap);
        w->contacts.reserve(w->contact_cap);
    } catch (...) {
        delete w;
        return nullptr; /* bad_alloc 绝不越界 */
    }
    w->max_bodies = cap;
    w->gravity_x = resolved.gravity_x;
    w->gravity_y = resolved.gravity_y;
    w->fixed_dt = resolved.fixed_dt;
    w->solver_iterations = (resolved.solver_iterations == 0u) ? 1u : resolved.solver_iterations;
    w->seed = resolved.seed;
    return w;
}

void mq_physics_destroy(mq_physics_world* w) {
    delete w;
}

int mq_physics_add_body(mq_physics_world* w, const mq_physics_body_desc* d, mq_physics_body* out) {
    if (w == nullptr || d == nullptr || out == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (d->struct_size != 0u && d->struct_size < sizeof(mq_physics_body_desc))
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (d->type > MQ_PHYSICS_BODY_DYNAMIC)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (d->shape > MQ_PHYSICS_SHAPE_CUBOID)
        return MQ_PHYSICS_ERR_NOT_SUPPORTED;
    if (d->shape == MQ_PHYSICS_SHAPE_BALL) {
        if (!(d->radius > 0.0F))
            return MQ_PHYSICS_ERR_INVALID_ARG;
    } else if (!(d->half_w > 0.0F) || !(d->half_h > 0.0F)) {
        return MQ_PHYSICS_ERR_INVALID_ARG;
    }
    if (d->restitution < 0.0F || d->restitution > 1.0F)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (d->friction < 0.0F || d->friction > 1.0F)
        return MQ_PHYSICS_ERR_INVALID_ARG;

    uint32_t index = UINT32_MAX;
    for (uint32_t i = 1u; i < w->max_bodies; ++i) {
        if (!w->slots[i].live) {
            index = i;
            break;
        }
    }
    if (index == UINT32_MAX)
        return MQ_PHYSICS_ERR_CAPACITY;
    if (w->generation_base == UINT32_MAX)
        return MQ_PHYSICS_ERR_CAPACITY; /* 世代耗尽：拒绝而非回绕 */

    BodySlot& slot = w->slots[index];
    reset_body_to_defaults(slot.body);
    uint32_t generation = slot.generation;
    if (generation == 0u) {
        ++w->generation_base;
        generation = w->generation_base;
    }
    slot.generation = generation;
    slot.live = true;
    live_insert(w->live, index);

    PhysicsBody& body = slot.body;
    body.generation = generation;
    body.type = d->type;
    body.shape = d->shape;
    body.flags = d->flags;
    body.x = d->x;
    body.y = d->y;
    body.prev_x = d->x;
    body.prev_y = d->y;
    body.vx = d->vx;
    body.vy = d->vy;
    body.radius = d->radius;
    body.half_w = d->half_w;
    body.half_h = d->half_h;
    const float mass = (d->mass > 0.0F) ? d->mass : 1.0F;
    body.inv_mass_dynamic = (d->type == MQ_PHYSICS_BODY_DYNAMIC) ? (1.0F / mass) : 0.0F;
    body.restitution = d->restitution;
    body.friction = d->friction;

    out->index = index;
    out->generation = generation;
    return MQ_PHYSICS_OK;
}

int mq_physics_remove_body(mq_physics_world* w, mq_physics_body b) {
    if (w == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (b.index == 0u || b.index >= w->max_bodies)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    BodySlot& slot = w->slots[b.index];
    if (!slot.live || slot.generation != b.generation)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    slot.live = false;
    live_erase(w->live, b.index);
    uint32_t next = slot.generation + 1u;
    if (next == 0u)
        next = 1u;
    slot.generation = next;
    reset_body_to_defaults(slot.body);
    return MQ_PHYSICS_OK;
}

int mq_physics_body_count(mq_physics_world* w) {
    if (w == nullptr)
        return -1;
    return static_cast<int>(w->live.size());
}

int mq_physics_body_valid(mq_physics_world* w, mq_physics_body b) {
    if (w == nullptr)
        return -1;
    return lookup(w, b) != nullptr ? 1 : 0;
}

int mq_physics_get_transform(mq_physics_world* w, mq_physics_body b, float* x, float* y) {
    if (w == nullptr || x == nullptr || y == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    const PhysicsBody* body = lookup(w, b);
    if (body == nullptr)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    *x = body->x;
    *y = body->y;
    return MQ_PHYSICS_OK;
}

int mq_physics_set_transform(mq_physics_world* w, mq_physics_body b, float x, float y) {
    if (w == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    PhysicsBody* body = lookup(w, b);
    if (body == nullptr)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    body->x = x;
    body->y = y;
    body->prev_x = x; /* 传送：prev 同步，避免跨传送插值 */
    body->prev_y = y;
    return MQ_PHYSICS_OK;
}

int mq_physics_get_velocity(mq_physics_world* w, mq_physics_body b, float* vx, float* vy) {
    if (w == nullptr || vx == nullptr || vy == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    const PhysicsBody* body = lookup(w, b);
    if (body == nullptr)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    *vx = body->vx;
    *vy = body->vy;
    return MQ_PHYSICS_OK;
}

int mq_physics_set_velocity(mq_physics_world* w, mq_physics_body b, float vx, float vy) {
    if (w == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    PhysicsBody* body = lookup(w, b);
    if (body == nullptr)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    body->vx = vx;
    body->vy = vy;
    return MQ_PHYSICS_OK;
}

int mq_physics_interpolate(mq_physics_world* w, mq_physics_body b, float alpha, float* x,
                           float* y) {
    if (w == nullptr || x == nullptr || y == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    const PhysicsBody* body = lookup(w, b);
    if (body == nullptr)
        return MQ_PHYSICS_ERR_INVALID_HANDLE;
    *x = body->prev_x + (body->x - body->prev_x) * alpha;
    *y = body->prev_y + (body->y - body->prev_y) * alpha;
    return MQ_PHYSICS_OK;
}

int mq_physics_set_bounds(mq_physics_world* w, float min_x, float min_y, float max_x,
                          float max_y) {
    if (w == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (!(min_x <= max_x) || !(min_y <= max_y))
        return MQ_PHYSICS_ERR_INVALID_ARG;
    w->bounds.enabled = true;
    w->bounds.min_x = min_x;
    w->bounds.min_y = min_y;
    w->bounds.max_x = max_x;
    w->bounds.max_y = max_y;
    return MQ_PHYSICS_OK;
}

int mq_physics_step(mq_physics_world* w) {
    if (w == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;

    for (const uint32_t i : w->live) {
        BodySlot& slot = w->slots[i];
        slot.body.prev_x = slot.body.x;
        slot.body.prev_y = slot.body.y;
    }

    w->contacts.clear();

    /* 1) 积分（升序；半隐式 Euler）。 */
    for (const uint32_t i : w->live) {
        BodySlot& slot = w->slots[i];
        PhysicsBody& body = slot.body;
        if (body.type == MQ_PHYSICS_BODY_DYNAMIC) {
            if ((body.flags & MQ_PHYSICS_BF_DISABLE_GRAVITY) == 0u) {
                body.vx += w->gravity_x * w->fixed_dt;
                body.vy += w->gravity_y * w->fixed_dt;
            }
        } else if (body.type == MQ_PHYSICS_BODY_STATIC) {
            continue;
        }
        body.x += body.vx * w->fixed_dt;
        body.y += body.vy * w->fixed_dt;
    }

    /* 2) 窄相：i<j 稳定配对（升序生成 + 稳定插入排序）。 */
    for (size_t ia = 0; ia < w->live.size(); ++ia) {
        const uint32_t i = w->live[ia];
        for (size_t ib = ia + 1; ib < w->live.size(); ++ib) {
            const uint32_t j = w->live[ib];
            const PhysicsBody& a = w->slots[i].body;
            const PhysicsBody& b = w->slots[j].body;
            if (!pair_active(a, b))
                continue;
            float nx = 0.0F, ny = 0.0F, depth = 0.0F;
            if (!body_pair_contact(a, b, nx, ny, depth))
                continue; /* 法线已由窄相统一为 a → b */
            Contact c;
            c.a_index = i;
            c.b_index = j;
            c.nx = nx;
            c.ny = ny;
            c.depth = depth;
            c.plane = false;
            contact_sort_insert(w->contacts, w->contact_cap, c);
        }
    }

    /* 3) 世界边界平面接触（升序追加；平面索引恒大于体索引，故仍保序）。 */
    if (w->bounds.enabled) {
        for (const uint32_t i : w->live) {
            const PhysicsBody& body = w->slots[i].body;
            if (!is_movable(body) || body.inv_mass_dynamic <= 0.0F)
                continue;
            const float ext_x = (body.shape == MQ_PHYSICS_SHAPE_BALL) ? body.radius : body.half_w;
            const float ext_y = (body.shape == MQ_PHYSICS_SHAPE_BALL) ? body.radius : body.half_h;

            const float pen_lo_x = (w->bounds.min_x + ext_x) - body.x;
            const float pen_hi_x = body.x - (w->bounds.max_x - ext_x);
            const float pen_lo_y = (w->bounds.min_y + ext_y) - body.y;
            const float pen_hi_y = body.y - (w->bounds.max_y - ext_y);

            float best_depth = -kContactSlop;
            float nx = 0.0F, ny = 0.0F;
            bool hit = false;
            if (pen_lo_x > best_depth) {
                best_depth = pen_lo_x;
                nx = -1.0F;
                ny = 0.0F;
                hit = true;
            }
            if (pen_hi_x > best_depth) {
                best_depth = pen_hi_x;
                nx = 1.0F;
                ny = 0.0F;
                hit = true;
            }
            if (pen_lo_y > best_depth) {
                best_depth = pen_lo_y;
                nx = 0.0F;
                ny = -1.0F;
                hit = true;
            }
            if (pen_hi_y > best_depth) {
                best_depth = pen_hi_y;
                nx = 0.0F;
                ny = 1.0F;
                hit = true;
            }
            if (hit) {
                Contact c;
                c.a_index = i;
                c.b_index = MQ_PHYSICS_PLANE_INDEX;
                c.nx = nx;
                c.ny = ny;
                c.depth = (best_depth > 0.0F) ? best_depth : 0.0F;
                c.plane = true;
                /* 同体仅一条；live 升序 → a_index 升序，天然稳定 */
                if (w->contacts.size() < w->contact_cap)
                    w->contacts.push_back(c);
            }
        }
    }

    /* 4) 位置修正：先一次性分离（避免位置漂移污染速度求解）。 */
    for (const Contact& c : w->contacts) {
        if (c.depth <= 0.0F)
            continue;
        BodySlot& slot_a = w->slots[c.a_index];
        if (!slot_a.live)
            continue;
        if (c.plane) {
            correct_position_plane(slot_a.body, c.nx, c.ny, c.depth);
            continue;
        }
        if (c.b_index >= w->max_bodies)
            continue;
        BodySlot& slot_b = w->slots[c.b_index];
        if (!slot_b.live)
            continue;
        correct_positions(slot_a.body, slot_b.body, c.nx, c.ny, c.depth);
    }

    /* 5) 速度求解：恒定 solver_iterations 次（不随负载变化）。 */
    for (uint32_t iter = 0; iter < w->solver_iterations; ++iter) {
        for (const Contact& c : w->contacts) {
            BodySlot& slot_a = w->slots[c.a_index];
            if (!slot_a.live)
                continue;
            PhysicsBody& a = slot_a.body;
            if (c.plane) {
                impulse_contacts(a, nullptr, c.nx, c.ny, a.restitution, a.friction);
                continue;
            }
            if (c.b_index >= w->max_bodies || !w->slots[c.b_index].live)
                continue;
            PhysicsBody& b = w->slots[c.b_index].body;
            const float restitution = (a.restitution > b.restitution) ? a.restitution : b.restitution;
            const float friction = (a.friction > b.friction) ? a.friction : b.friction;
            impulse_contacts(a, &b, c.nx, c.ny, restitution, friction);
        }
    }

    /* 6) 静息钳制（确定性）：接触两侧分别清零微小法向速度 —— 消除落定后的持续抖动。
     * 阈值 = 静息 eps + 一帧重力增量；真实碰撞/弹跳速度远大于该阈值，不受影响。 */
    const float rest_threshold = kRestVelocityEpsilon + 1.5F * (std::fabs(w->gravity_x) +
                                                                std::fabs(w->gravity_y)) *
                                                           w->fixed_dt;
    for (const Contact& c : w->contacts) {
        BodySlot& slot_a = w->slots[c.a_index];
        if (slot_a.live)
            rest_clamp(slot_a.body, c.nx, c.ny, rest_threshold);
        if (c.plane || c.b_index >= w->max_bodies)
            continue;
        BodySlot& slot_b = w->slots[c.b_index];
        if (slot_b.live)
            rest_clamp(slot_b.body, c.nx, c.ny, rest_threshold);
    }

    ++w->step_count;
    return MQ_PHYSICS_OK;
}

uint32_t mq_physics_step_count(mq_physics_world* w) {
    if (w == nullptr)
        return 0u;
    return static_cast<uint32_t>(w->step_count);
}

uint64_t mq_physics_checksum(mq_physics_world* w) {
    uint32_t hash = 2166136261u;
    if (w == nullptr)
        return static_cast<uint64_t>(hash);
    const uint32_t cap = w->max_bodies;
    hash_scalar(hash, cap);
    const uint32_t live_count = static_cast<uint32_t>(w->live.size());
    hash_scalar(hash, live_count);
    const uint64_t steps = w->step_count;
    hash_scalar(hash, steps);
    for (const uint32_t i : w->live) {
        const BodySlot& slot = w->slots[i];
        const PhysicsBody& b = slot.body;
        hash_scalar(hash, i);
        hash_scalar(hash, b.generation);
        hash_scalar(hash, b.type);
        hash_scalar(hash, b.shape);
        hash_scalar(hash, b.flags);
        hash_scalar(hash, b.x);
        hash_scalar(hash, b.y);
        hash_scalar(hash, b.prev_x);
        hash_scalar(hash, b.prev_y);
        hash_scalar(hash, b.vx);
        hash_scalar(hash, b.vy);
        hash_scalar(hash, b.radius);
        hash_scalar(hash, b.half_w);
        hash_scalar(hash, b.half_h);
        hash_scalar(hash, b.inv_mass_dynamic);
        hash_scalar(hash, b.restitution);
        hash_scalar(hash, b.friction);
    }
    return static_cast<uint64_t>(hash);
}

uint32_t mq_physics_contact_count(mq_physics_world* w) {
    if (w == nullptr)
        return 0u;
    return static_cast<uint32_t>(w->contacts.size());
}

int mq_physics_contact_at(mq_physics_world* w, uint32_t i, mq_physics_contact* out) {
    if (w == nullptr || out == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (i >= w->contacts.size())
        return MQ_PHYSICS_ERR_INVALID_ARG;
    const Contact& c = w->contacts[i];
    out->a_index = c.a_index;
    out->b_index = c.b_index;
    out->nx = c.nx;
    out->ny = c.ny;
    out->depth = c.depth;
    return MQ_PHYSICS_OK;
}

size_t mq_physics_state_size(mq_physics_world* w) {
    if (w == nullptr)
        return 0u;
    return sizeof(MqSnapshotHeader) + w->live.size() * sizeof(MqSnapshotBody);
}

size_t mq_physics_save_state(mq_physics_world* w, void* out, size_t cap) {
    if (w == nullptr)
        return 0u;
    const size_t needed = mq_physics_state_size(w);
    if (out == nullptr || cap < needed)
        return needed;

    unsigned char* p = static_cast<unsigned char*>(out);
    MqSnapshotHeader header{};
    header.magic = kSnapshotMagic;
    header.version = kSnapshotVersion;
    header.body_count = static_cast<uint32_t>(w->live.size());
    header.max_bodies = w->max_bodies;
    header.step_count = w->step_count;
    std::memcpy(p, &header, sizeof(header));
    size_t offset = sizeof(header);

    for (const uint32_t i : w->live) {
        const BodySlot& slot = w->slots[i];
        const PhysicsBody& b = slot.body;
        MqSnapshotBody rec{};
        rec.index = i;
        rec.generation = b.generation;
        rec.type = b.type;
        rec.shape = b.shape;
        rec.flags = b.flags;
        rec.x = b.x;
        rec.y = b.y;
        rec.px = b.prev_x;
        rec.py = b.prev_y;
        rec.vx = b.vx;
        rec.vy = b.vy;
        rec.radius = b.radius;
        rec.half_w = b.half_w;
        rec.half_h = b.half_h;
        rec.inv_mass_dynamic = b.inv_mass_dynamic;
        rec.restitution = b.restitution;
        rec.friction = b.friction;
        std::memcpy(p + offset, &rec, sizeof(rec));
        offset += sizeof(rec);
    }
    return needed;
}

int mq_physics_load_state(mq_physics_world* w, const void* in, size_t size) {
    if (w == nullptr || in == nullptr)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (size < sizeof(MqSnapshotHeader))
        return MQ_PHYSICS_ERR_INVALID_ARG;

    MqSnapshotHeader header{};
    std::memcpy(&header, in, sizeof(header));
    if (header.magic != kSnapshotMagic || header.version != kSnapshotVersion)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    if (header.body_count > w->max_bodies || header.max_bodies > w->max_bodies)
        return MQ_PHYSICS_ERR_INVALID_ARG;
    const size_t needed =
        sizeof(MqSnapshotHeader) + static_cast<size_t>(header.body_count) * sizeof(MqSnapshotBody);
    if (size < needed)
        return MQ_PHYSICS_ERR_INVALID_ARG;

    /* 第一遍：只校验（不改世界）→ 失败时世界保持原状（强一致，且不分配）。 */
    const unsigned char* p = static_cast<const unsigned char*>(in);
    uint32_t prev_index = 0u;
    for (uint32_t n = 0u; n < header.body_count; ++n) {
        MqSnapshotBody rec{};
        std::memcpy(&rec, p + sizeof(MqSnapshotHeader) + n * sizeof(MqSnapshotBody), sizeof(rec));
        if (rec.index == 0u || rec.index >= w->max_bodies || rec.index <= prev_index ||
            rec.generation == 0u || rec.type > MQ_PHYSICS_BODY_DYNAMIC ||
            rec.shape > MQ_PHYSICS_SHAPE_CUBOID) {
            return MQ_PHYSICS_ERR_INVALID_ARG; /* 含索引必须严格升序（本模块快照的规范形式） */
        }
        prev_index = rec.index;
    }

    /* 第二遍：应用（已校验，不会失败）。 */
    for (const uint32_t i : w->live) {
        BodySlot& slot = w->slots[i];
        slot.live = false;
        reset_body_to_defaults(slot.body);
    }
    w->live.clear();
    for (uint32_t n = 0u; n < header.body_count; ++n) {
        MqSnapshotBody rec{};
        std::memcpy(&rec, p + sizeof(MqSnapshotHeader) + n * sizeof(MqSnapshotBody), sizeof(rec));
        BodySlot& slot = w->slots[rec.index];
        slot.live = true;
        slot.generation = rec.generation;
        live_insert(w->live, rec.index);
        PhysicsBody& b = slot.body;
        reset_body_to_defaults(b);
        b.generation = rec.generation;
        b.type = rec.type;
        b.shape = rec.shape;
        b.flags = rec.flags;
        b.x = rec.x;
        b.y = rec.y;
        b.prev_x = rec.px;
        b.prev_y = rec.py;
        b.vx = rec.vx;
        b.vy = rec.vy;
        b.radius = rec.radius;
        b.half_w = rec.half_w;
        b.half_h = rec.half_h;
        b.inv_mass_dynamic = rec.inv_mass_dynamic;
        b.restitution = rec.restitution;
        b.friction = rec.friction;
    }
    w->step_count = header.step_count;
    return MQ_PHYSICS_OK;
}

} // extern "C"

namespace {

const mq_physics_v1 kPhysicsV1 = {
    MQ_PHYSICS_ABI_VERSION, MQ_PHYSICS_IFACE_NAME,
    &mq_physics_create,     &mq_physics_destroy,
    &mq_physics_add_body,   &mq_physics_remove_body,
    &mq_physics_step,       &mq_physics_get_transform,
    &mq_physics_set_transform, &mq_physics_set_velocity,
    &mq_physics_checksum,
};

} // namespace

extern "C" const mq_physics_v1* mq_physics_query(uint32_t abi_version) {
    if (abi_version != MQ_PHYSICS_ABI_VERSION)
        return nullptr;
    return &kPhysicsV1;
}
