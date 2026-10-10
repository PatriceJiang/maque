# 13 · physics 模块计划

> Owner agent：`agent-physics` ｜ 上游：`docs/00`（根，ADR-0027/0028）、`docs/15`（主参考 §3）、`docs/10`（确定性/快照/回放）、`docs/02`（跨 `.so` ABI 与多实例）、`docs/06`（反射组件）、`docs/07`（内存/句柄）、`docs/12`（世界/双视图/相位）。
> 交付顺序：**阶段 D 第三块**（`scene → animation → physics → particle`）。

## 1. 模块概述

- **职责**：为世界层提供**确定性的刚体/碰撞模拟**。消费 ECS 上 bake 后的 `RigidBody` / `Collider` 组件，输出只读变换与查询结果；是回放最易漂移的环节，规则最严（docs/10 §6）。
- **所在层**：**世界层**（依赖基础层 math/core，不依赖 renderer/script）。
- **上游文档**：`docs/15 §3`（实现选型 = Rapier + C ABI FFI；固定步长/迭代/稳定排序；`physics_process(dt)` 独立相位；前两帧只读插值）、`docs/10`（D1/D4/D5/D6、L1 平台范围、checksum/回放）、`docs/02`（R1–R8 强制规则）、`docs/00 ADR-0028`（集成 Rapier，确定性需实测，不达标退回自研）。
- **一句话**：**以 Rapier（Rust）为可替换后端的确定性物理服务**——对外一张 C ABI vtable + 世代句柄，对内通过稳定排序、固定步长与逐帧 checksum 保证 L1 可复现，Rapier 不达标时同 vtable 下切换自研求解器。

### 关键架构决策（本模块内固化）

1. **两段边界，一个稳定 vtable**：
   - **对外**（scene/host ← physics）：`mq_physics.h` 的 `mq_physics_v1` C vtable + 句柄，跨 `.so` 严格遵循 docs/02。
   - **对内**（physics ← Rapier）：`mq_physics_sys.h` 是 **C++ 与 Rust 之间的 C ABI**，只传 POD + 句柄；**不共享任何 C++ 类型**。
   - **后端可插拔**：`.so` 内部实现 `IPhysicsBackend`（模块**内部** C++ 抽象，不跨边界），有两个实现：
     - `RapierBackend`：经 `mq_physics_sys.h` 调 Rust/Rapier（**默认**）。
     - `NativeSolverBackend`：自研确定性求解器（**降级/回退**，也是单测的 fake 语义基线）。
   - **公共 vtable 与后端解耦**：fake solver 因此不是"假模块"，而是与 Rapier 同契约的真实后端，可独立单测（满足计划铁律 3）。
2. **确定性策略**：固定步长（累加器吸收渲染帧波动）+ 固定迭代 + **按 `body.index` 稳定排序**的接触/岛求解 + 引擎 seed 注入 Rapier（仅用于其内部需要的确定性 RNG）。**禁**非确定线程、禁指针序、禁 `-ffast-math`（docs/10 D1/D4/D8）。
3. **快照/回滚友好**：物理状态可 `serialize_state` / `restore_state`（Rapier 内部状态 + 显式快照），使 A5 回滚覆盖物理（docs/10 §4）。
4. **只读插值**：模块持有每体 `prev`/`curr` 两份变换，渲染插值只读、**绝不回写**模拟（docs/15 §3、docs/13 §8）。

### 模块目录（建议）

```
modules/world/physics/
  include/physics/mq_physics.h        # 对外纯 C ABI（跨 .so）
  include/physics/mq_physics_sys.h    # 对 Rust 的纯 C ABI（跨语言）
  src/physics_module.cpp              # mq_module_query / vtable 装配
  src/backend.h                       # 模块内部 IPhysicsBackend
  src/backend_rapier.cpp              # 调 mqrp_* C ABI
  src/backend_native.cpp              # 自研降级求解器（亦作 fake）
  src/stable_sort.h                   # id 稳定排序 / checksum（确定性核心）
  rust/maque_physics_sys/             # Rappier 封装 crate（staticlib）
  tests/                              # doctest：接口 / 确定性 / 场景 / 插值
  CMakeLists.txt
```

---

## 2. 接口层设计（完备，先冻结）

> 跨 `.so` 只暴露 C ABI + 世代句柄（docs/02 R1–R8）；所有入口 `noexcept`；异常/panic **不得越界**。接口不完备不得进入实现。

### 2.1 公共头 `mq_physics.h`（纯 C）

```c
/* modules/world/physics/include/physics/mq_physics.h —— 纯 C，无 std */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "math/mq_math_c.h"   /* 复用 math 的 C 布局别名：mq_math_vec3 / mq_math_quat / mq_math_transform */

#define MQ_PHYSICS_ABI_VERSION 1u
#define MQ_PHYSICS_IFACE_NAME  "maque.physics.v1"

/* ---------- 不透明世界（模块分配/释放；宿主只持有指针，不解引用） ---------- */
typedef struct mq_physics_world mq_physics_world;

/* ---------- 世代句柄（防悬垂；generation 复用检测，docs/02 R4） ---------- */
typedef struct mq_physics_body     { uint32_t index; uint32_t generation; } mq_physics_body;
typedef struct mq_physics_collider { uint32_t index; uint32_t generation; } mq_physics_collider;
typedef struct mq_physics_joint    { uint32_t index; uint32_t generation; } mq_physics_joint;
#define MQ_PHYSICS_HANDLE_INVALID  ((uint32_t)0u)   /* generation==0 恒为无效 */

/* ---------- 错误码（无异常越界；0 = OK） ---------- */
typedef enum mq_physics_error {
    MQ_PHYSICS_OK                = 0,
    MQ_PHYSICS_ERR_INVALID_ARG   = 1,   /* 空指针 / 非法区间 / 非法枚举 */
    MQ_PHYSICS_ERR_INVALID_HANDLE= 2,   /* 句柄 slot 空或 generation 过期 */
    MQ_PHYSICS_ERR_OUT_OF_MEMORY = 3,   /* 经传入 allocator 分配失败 */
    MQ_PHYSICS_ERR_VERSION_MISMATCH = 4,
    MQ_PHYSICS_ERR_BACKEND_UNAVAILABLE = 5, /* Rapier 未链接/初始化失败 → 已回退或拒绝 */
    MQ_PHYSICS_ERR_NOT_SUPPORTED = 6,
    MQ_PHYSICS_ERR_PANIC        = 7,    /* Rust 侧 catch_unwind 捕获，绝不 unwind 越界 */
    MQ_PHYSICS_ERR_INTERNAL     = 8
} mq_physics_error;

/* ---------- POD 描述/状态（固定布局；struct_size 前向兼容） ---------- */

typedef enum mq_physics_body_type {
    MQ_PHYSICS_BODY_STATIC    = 0,
    MQ_PHYSICS_BODY_KINEMATIC = 1,   /* 由外部 set_transform 驱动，参与碰撞 */
    MQ_PHYSICS_BODY_DYNAMIC   = 2
} mq_physics_body_type;

typedef enum mq_physics_shape_type {
    MQ_PHYSICS_SHAPE_BALL       = 0,
    MQ_PHYSICS_SHAPE_CUBOID     = 1,
    MQ_PHYSICS_SHAPE_CAPSULE    = 2,
    MQ_PHYSICS_SHAPE_CYLINDER   = 3,
    MQ_PHYSICS_SHAPE_CONVEX_HULL= 4,   /* 见 hull_points */
    MQ_PHYSICS_SHAPE_TRIMESH    = 5    /* 静态；预算受限，见 §7 */
} mq_physics_shape_type;

/* body flags（位或） */
#define MQ_PHYSICS_BF_LOCK_ROT_X    (1u<<0)
#define MQ_PHYSICS_BF_LOCK_ROT_Y    (1u<<1)
#define MQ_PHYSICS_BF_LOCK_ROT_Z    (1u<<2)
#define MQ_PHYSICS_BF_LOCK_TRANS_X  (1u<<3)
#define MQ_PHYSICS_BF_LOCK_TRANS_Y  (1u<<4)
#define MQ_PHYSICS_BF_LOCK_TRANS_Z  (1u<<5)
#define MQ_PHYSICS_BF_CCD           (1u<<6)   /* 连续碰撞检测（子弹） */
#define MQ_PHYSICS_BF_DISABLE_GRAVITY (1u<<7)
#define MQ_PHYSICS_BF_CAN_SLEEP     (1u<<8)

/* collider flags */
#define MQ_PHYSICS_CF_SENSOR        (1u<<0)   /* 仅检测不求解 */
#define MQ_PHYSICS_CF_GROUP_MASK    (0xFFFFu<<4)  /* 简单碰撞分组，具体位段见下 */
#define MQ_PHYSICS_CF_GROUP_SHIFT   4
#define MQ_PHYSICS_CF_MASK_SHIFT    20

typedef struct mq_physics_config {
    uint32_t       struct_size;
    mq_math_vec3   gravity;           /* 世界重力，默认 (0,-9.81,0) */
    float          fixed_dt;          /* 固定步长秒，默认 1/60；>0 */
    uint32_t       max_substeps;      /* 每 step 最多子步，默认 8（防螺旋死亡） */
    uint32_t       solver_iterations; /* 固定迭代次数，默认 4；不得随负载变化 */
    uint32_t       backend;           /* 0=auto(Rapier→native) 1=rapier 2=native */
    uint32_t       flags;             /* 预留 */
    uint64_t       seed;              /* 引擎 seed（docs/10 D2），决定 Rapier 内部确定性 RNG */
} mq_physics_config;

typedef struct mq_physics_body_desc {
    uint32_t             struct_size;
    uint32_t             type;        /* mq_physics_body_type */
    uint32_t             flags;       /* MQ_PHYSICS_BF_* */
    mq_math_vec3         position;
    mq_math_quat         rotation;
    mq_math_vec3         linvel;      /* 初速度 */
    mq_math_vec3         angvel;
    float                lin_damping; /* 默认 0 */
    float                ang_damping;
    float                gravity_scale; /* 默认 1 */
} mq_physics_body_desc;

typedef struct mq_physics_shape_desc {
    uint32_t         struct_size;
    uint32_t         shape_type;      /* mq_physics_shape_type */
    float            radius;          /* BALL/CAPSULE/CYLINDER */
    float            half_height;     /* CAPSULE/CYLINDER 沿 Y */
    mq_math_vec3     half_extents;    /* CUBOID */
    float            friction;        /* 默认 0.5，>=0 */
    float            restitution;     /* 默认 0，[0,1] 建议 */
    float            density;         /* 默认 1，>0（质量由形状×密度推） */
    uint32_t         flags;           /* MQ_PHYSICS_CF_* */
    const float*     hull_points;     /* CONVEX_HULL：仅调用期借入，模块立即拷贝；不做所有权转移 */
    uint32_t         hull_point_count;
} mq_physics_shape_desc;

typedef struct mq_physics_body_state {
    mq_math_vec3  position;
    mq_math_quat  rotation;
    mq_math_vec3  linvel;
    mq_math_vec3  angvel;
    uint32_t      flags;      /* 位0: sleeping，位1: dynamic */
    uint32_t      _pad0;      /* 显式零填充：序列化按字段，禁 padding 泄漏（docs/10 D5） */
    uint64_t      user_id;    /* 与 ECS entity 的稳定关联，仅回传不解释 */
} mq_physics_body_state;

typedef struct mq_physics_ray {
    mq_math_vec3 origin;
    mq_math_vec3 dir;         /* 须归一化（调用方保证，否则 INVALID_ARG） */
    float        max_toi;     /* >0 */
} mq_physics_ray;

typedef struct mq_physics_ray_hit {
    uint32_t            has_hit;   /* 0/1 */
    uint32_t            _pad0;
    mq_physics_collider collider;
    mq_physics_body     body;
    float               toi;       /* 命中距离（沿 dir） */
    mq_math_vec3        normal;
    mq_math_vec3        point;
} mq_physics_ray_hit;

typedef struct mq_physics_contact {
    mq_physics_body a;
    mq_physics_body b;
    mq_math_vec3    normal;      /* a→b 方向 */
    float           depth;
    uint32_t        sensor;      /* 0/1 */
    uint32_t        _pad0;
} mq_physics_contact;

typedef struct mq_physics_stats {
    uint64_t step_count;
    uint32_t body_count;
    uint32_t collider_count;
    uint32_t awake_body_count;
    uint32_t contact_pair_count;
    uint32_t island_count;
    uint64_t last_step_micros;
} mq_physics_stats;

/* ---------- 插件入口：C++ 侧实现，跨 .so 消费 ---------- */
typedef struct mq_physics_v1 {
    uint32_t struct_size;        /* sizeof(mq_physics_v1)，只增不改 */
    uint32_t abi_version;        /* 主版本不匹配即拒绝 */

    /* 生命周期 */
    mq_physics_error (*create)(void* host, const mq_allocator* alloc,
                               const mq_physics_config* cfg,
                               mq_physics_world** out_world);
    void             (*destroy)(mq_physics_world* w);

    /* 世界参数 */
    mq_physics_error (*set_gravity)(mq_physics_world*, mq_math_vec3);
    mq_physics_error (*get_config)(const mq_physics_world*, mq_physics_config* out);

    /* 刚体 */
    mq_physics_error (*add_body)(mq_physics_world*, const mq_physics_body_desc*,
                                 mq_physics_body* out_body);
    mq_physics_error (*remove_body)(mq_physics_world*, mq_physics_body);
    mq_physics_error (*set_body_transform)(mq_physics_world*, mq_physics_body,
                                           const mq_math_transform*);
    mq_physics_error (*get_body_transform)(const mq_physics_world*, mq_physics_body,
                                           mq_math_transform* out);
    mq_physics_error (*set_body_linvel)(mq_physics_world*, mq_physics_body, mq_math_vec3);
    mq_physics_error (*get_body_linvel)(const mq_physics_world*, mq_physics_body,
                                        mq_math_vec3* out);
    mq_physics_error (*set_body_angvel)(mq_physics_world*, mq_physics_body, mq_math_vec3);
    mq_physics_error (*set_body_type)(mq_physics_world*, mq_physics_body, uint32_t type);
    mq_physics_error (*apply_force)(mq_physics_world*, mq_physics_body,
                                    mq_math_vec3 force, mq_math_vec3 point);
    mq_physics_error (*apply_impulse)(mq_physics_world*, mq_physics_body,
                                      mq_math_vec3 impulse, mq_math_vec3 point);
    mq_physics_error (*wake_body)(mq_physics_world*, mq_physics_body);
    mq_physics_error (*is_body_sleeping)(const mq_physics_world*, mq_physics_body,
                                         uint32_t* out_sleeping);

    /* 碰撞体 */
    mq_physics_error (*add_collider)(mq_physics_world*, mq_physics_body,
                                     const mq_physics_shape_desc*,
                                     mq_physics_collider* out_collider);
    mq_physics_error (*remove_collider)(mq_physics_world*, mq_physics_collider);
    mq_physics_error (*set_collider_material)(mq_physics_world*, mq_physics_collider,
                                              float friction, float restitution);

    /* 约束（v1 最小集；更多关节类型走 v2 表尾追加） */
    mq_physics_error (*add_joint)(mq_physics_world*, mq_physics_body a, mq_physics_body b,
                                  uint32_t joint_type, const void* joint_desc,
                                  mq_physics_joint* out_joint);
    mq_physics_error (*remove_joint)(mq_physics_world*, mq_physics_joint);

    /* 推进：内部按 fixed_dt 分解子步；更新 prev/curr 快照 */
    mq_physics_error (*step)(mq_physics_world*, float dt);

    /* 查询（只读） */
    mq_physics_error (*raycast)(const mq_physics_world*, const mq_physics_ray*,
                                uint32_t group, uint32_t mask, mq_physics_ray_hit* out);
    mq_physics_error (*overlap_sphere)(const mq_physics_world*, mq_math_vec3 center,
                                       float radius, uint32_t group, uint32_t mask,
                                       mq_physics_collider* out, uint32_t cap,
                                       uint32_t* out_count);
    /* 接触对迭代器：稳定按 (a.index,b.index) 升序输出（确定性） */
    mq_physics_error (*contacts_begin)(const mq_physics_world*, void** out_iter);
    mq_physics_error (*contacts_next)(void* iter, mq_physics_contact* out, uint32_t* has_next);
    void             (*contacts_end)(void* iter);

    /* 读回 / 只读插值（渲染用；绝不回写模拟） */
    mq_physics_error (*get_body_state)(const mq_physics_world*, mq_physics_body,
                                       mq_physics_body_state* out);              /* curr */
    mq_physics_error (*get_body_state_prev)(const mq_physics_world*, mq_physics_body,
                                            mq_physics_body_state* out);         /* prev */
    mq_physics_error (*interpolate_transform)(const mq_physics_world*, mq_physics_body,
                                              float alpha, mq_math_transform* out);

    /* 确定性 / 观测 */
    mq_physics_error (*checksum)(const mq_physics_world*, uint64_t* out);
    mq_physics_error (*frame_count)(const mq_physics_world*, uint64_t* out);
    mq_physics_error (*stats)(const mq_physics_world*, mq_physics_stats* out);

    /* 快照 / 回滚：经传入 allocator 分配，调用方同 allocator 释放 */
    mq_physics_error (*serialize_state)(const mq_physics_world*, const mq_allocator*,
                                        void** out_blob, size_t* out_len);
    mq_physics_error (*restore_state)(mq_physics_world*, const void* blob, size_t len);
} mq_physics_v1;

/* 错误码可读化（A2；返回静态字符串，调用方不释放） */
const char* mq_physics_error_str(mq_physics_error e);

/* 唯一导出入口（docs/02 §3） */
void* mq_module_query(const char* interface_name, uint32_t min_version);
```

### 2.2 类型 / 约定（冻结）

- **复用 math 布局**：`mq_math_vec3`（12B）/`mq_math_quat`（16B）/`mq_math_transform`（48B）由 `math/mq_math_c.h` 提供；physics **不重复定义**，在 C++ 侧 `static_assert(sizeof/alignof/offsetof)` 对拍（详见 §2.6）。列主序、右手系、深度 `[0,1]`、Euler ZYX 一律遵守 math（docs/00 §2.4）。
- **枚举**：`body_type` / `shape_type` / `error` 均为 `uint32_t` 语义的稳定整数，**只增不改语义**。
- **位掩码**：flags 使用固定位段，文档化并冻结；新增走未用高位。
- **`_pad0`**：显式零填充字段，保证 `serialize` 按字段而非裸 `memcmp`（docs/10 D5）。
- **质量**：由形状 × `density` 推导，避免另一套质量参数面。

### 2.3 操作面语义要点

| 操作 | 语义 / 前置 | 返回 / 错误 |
|------|-------------|-------------|
| `create` | `alloc` 非空（模块经它分配世界）；`cfg.fixed_dt>0`；backend 选择 Rapier 失败时 → 若 `backend=auto` 回退 native 并置 `MQ_PHYSICS_ERR_BACKEND_UNAVAILABLE` 于内部日志（不失败），若显式 `rapier` 则返回该错 | `OK` + `out_world` |
| `destroy` | 幂等；释放全部内部 slot 与快照 | 无返回 |
| `add_body` | `desc.struct_size` 不小于已知最小；产出 `{index,generation}`，`generation` 从 1 起，slot 复用则 `generation++` | `OK`/`INVALID_ARG`/`OOM` |
| `remove_body` | 连带移除其 colliders/joints；句柄作废（generation 失效） | `OK`/`INVALID_HANDLE` |
| `set_body_transform` | STATIC/KINEMATIC 立即生效；DYNAMIC 亦写（teleport，清相关缓存） | `OK` |
| `apply_force/impulse` | 累积到下一 `step` 后清零；DYNAMIC 有效 | `OK`/`INVALID_HANDLE` |
| `add_collider` | 同一 body 可多 collider；`hull_points` 仅调用期借入并**立即深拷** | `OK`/`INVALID_ARG`/`INVALID_HANDLE` |
| `step` | `dt` 累加进内部 accumulator，按 `fixed_dt` 切子步，上限 `max_substeps`；步内：更新 curr→prev、求接触（稳定排序）、按迭代数求解、更新 curr；`step_count++` | `OK`/`INVALID_ARG(dt<0)` |
| `raycast` | `dir` 归一化、`max_toi>0`；命中返回最近者；sensor 也命中 | `OK`+`has_hit` |
| `contacts_*` | 迭代器按 `(a.index,b.index)` 升序；调用期间世界应只读 | `OK` |
| `get_body_state[_prev]` | 只读返回 prev/curr；不改变任何模拟状态 | `OK` |
| `interpolate_transform` | `alpha∈[0,1]`：位置线性、旋转 slerp（math），**只读** | `OK` |
| `checksum` | 对**按 `body.index` 升序**的全部状态（位置/旋转/线角速度/标志）做 FNV-1a；**不掺入指针/迭代序** | `OK`+u64 |
| `serialize_state` | 输出世界完整物理状态（含 Rapier 内部状态）的字节 blob，经传入 allocator 分配 | `OK`+blob |
| `restore_state` | 就地恢复到 blob 状态；`prev=curr`（回滚后无插值残留） | `OK`/`INVALID_ARG` |

### 2.4 错误模型（无异常越界）

- 所有函数 `noexcept`，出错返回 `mq_physics_error`；`create` 经 `out_world` 返回并返回码。
- **C++ 侧**：内部用 `std::expected`，在边界**翻译**为错误码，绝不抛过 `.so`。
- **Rust 侧**：每个 `extern "C"` 入口包 `std::panic::catch_unwind`，捕获 → 返回 `MQ_PHYSICS_ERR_PANIC`，**panic 绝不 unwind 进 C++**（UB）。Rust 侧**禁** `panic=abort`（会杀进程），用 `panic=unwind` + catch。
- 错误码经 `mq_physics_error_str` 可读化，喂 A2 日志。

### 2.5 版本化

- **对外**：`mq_physics_v1.struct_size` + `abi_version`；`mq_module_query("maque.physics.v1", min_ver)` 校验主版本，不匹配返回 `NULL` + 可读错误（docs/02 §3）。新能力一律**表尾追加**，旧调用方按自身 `struct_size` 截断。
- **对内**：`mq_physics_sys.h` 亦导出 `mqrp_abi_version()`；C++ 侧 `create` 时校验，不一致即拒绝（防 Rust `.a` 与 C++ `.so` 版本漂移）。
- 任一行为约定变更（枚举语义、flags 位段、校验和算法、fixed_dt 默认）→ **bump 主版本**。

### 2.6 Rust FFI 边界约定（`mq_physics_sys.h`）

> 这是 **C++ ↔ Rust 的 C ABI**，与对外 vtable 是**两条独立边界**。Rust 只经此文件暴露符号，**按 `#[no_mangle] extern "C"]` 生成**。不出现任何 C++ 类型、STL、异常。

```c
/* modules/world/physics/include/physics/mq_physics_sys.h —— 纯 C */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MQRP_ABI_VERSION 1u

typedef struct mqrp_world mqrp_world;   /* Rust 分配/释放的不透明对象 */

/* 与 mq_physics_* 对齐的 POD（同一 struct_size 版本化策略） */
typedef struct mqrp_config {
    uint32_t     struct_size;
    mq_math_vec3 gravity;
    float        fixed_dt;
    uint32_t     max_substeps;
    uint32_t     solver_iterations;
    uint64_t     seed;
} mqrp_config;

typedef struct mqrp_body_desc { /* 字段与 mq_physics_body_desc 同构（POD 拷贝） */ uint32_t struct_size; /* ... */ } mqrp_body_desc;
typedef struct mqrp_shape_desc { uint32_t struct_size; /* ... 含 hull */ } mqrp_shape_desc;
typedef struct mqrp_body_state { /* 与 mq_physics_body_state 同构 */ } mqrp_body_state;

uint32_t mqrp_abi_version(void);

/* 返回值统一为 int32_t 错误码（0=OK），与 mq_physics_error 数值对齐 */
int32_t     mqrp_world_create(const mqrp_config* cfg, mqrp_world** out);
void        mqrp_world_destroy(mqrp_world*);
int32_t     mqrp_set_gravity(mqrp_world*, float x, float y, float z);

int32_t     mqrp_add_body(mqrp_world*, const mqrp_body_desc*, uint32_t* out_index, uint32_t* out_gen);
int32_t     mqrp_remove_body(mqrp_world*, uint32_t index, uint32_t gen);
int32_t     mqrp_set_transform(mqrp_world*, uint32_t index, uint32_t gen, const mq_math_transform*);
int32_t     mqrp_get_state(const mqrp_world*, uint32_t index, uint32_t gen, mqrp_body_state* out);
int32_t     mqrp_set_linvel(mqrp_world*, uint32_t index, uint32_t gen, float x, float y, float z);
int32_t     mqrp_apply_force(mqrp_world*, uint32_t index, uint32_t gen,
                             float fx, float fy, float fz, float px, float py, float pz);
int32_t     mqrp_apply_impulse(mqrp_world*, uint32_t index, uint32_t gen,
                               float ix, float iy, float iz, float px, float py, float pz);
int32_t     mqrp_set_body_type(mqrp_world*, uint32_t index, uint32_t gen, uint32_t type);
int32_t     mqrp_wake_body(mqrp_world*, uint32_t index, uint32_t gen);
int32_t     mqrp_is_sleeping(const mqrp_world*, uint32_t index, uint32_t gen, uint32_t* out);

int32_t     mqrp_add_collider(mqrp_world*, uint32_t index, uint32_t gen,
                              const mqrp_shape_desc*, uint32_t* out_col_index, uint32_t* out_col_gen);
int32_t     mqrp_remove_collider(mqrp_world*, uint32_t col_index, uint32_t col_gen);
int32_t     mqrp_set_collider_material(mqrp_world*, uint32_t col_index, uint32_t col_gen,
                                       float friction, float restitution);

int32_t     mqrp_step(mqrp_world*, float dt);

int32_t     mqrp_raycast(const mqrp_world*, float ox, float oy, float oz,
                         float dx, float dy, float dz, float max_toi,
                         uint32_t group, uint32_t mask,
                         uint32_t* has_hit, uint32_t* col_index, uint32_t* col_gen,
                         float* toi, float* nx, float* ny, float* nz,
                         float* px, float* py, float* pz);

/* 接触对：按 (a,b) 稳定升序，返回条数；buffer 由调用方提供 */
int32_t     mqrp_contacts(const mqrp_world*, mqrp_contact* out, uint32_t cap, uint32_t* out_count);

uint64_t    mqrp_checksum(const mqrp_world*);

int32_t     mqrp_serialize(const mqrp_world*, const mq_allocator* a, void** out_blob, size_t* out_len);
int32_t     mqrp_restore(mqrp_world*, const void* blob, size_t len);

/* 释放 mqrp_serialize 产出的 blob（必须经同一 allocator；见 R5） */
void        mqrp_free_blob(const mq_allocator* a, void* blob);
```

**边界铁律（Rust↔C++）**：

| # | 规则 | 落实 |
|---|------|------|
| F1 | **只 C ABI** | Rust 符号 `extern "C"` + `#[no_mangle]`；不导出 Rust 泛型/`impl Trait`/`Result` |
| F2 | **不共享 C++ 类型** | 参数/返回全为 C POD（`repr(C)`：`mqrp_*` 结构对齐 `mq_physics_*`），**无 STL/引用/`std::string`** |
| F3 | **句柄传递** | 世界为 `mqrp_world*`（Rust 拥有）；刚体/碰撞体以 `(index,gen)` 整数对传递，Rust 侧维护同构 slot 表 |
| F4 | **内存所有权** | 世界由 **Rust 分配、Rust 释放**；`serialize` blob 由 Rust 经**传入 allocator** 分配、调用方经 `mqrp_free_blob`（同 allocator）释放；`hull_points` **仅调用期借入**，Rust 立即拷贝 |
| F5 | **错误码** | 统一 `int32_t`，数值与 `mq_physics_error` 对齐；无异常越界 |
| F6 | **panic 隔离** | 每入口 `catch_unwind` → `MQ_PHYSICS_ERR_PANIC`；Rust 编译 `panic=unwind` |
| F7 | **确定性** | Rust 侧遍历一律按整数 id 升序；**禁** `HashMap` 迭代序出接口（用 `IndexMap`/排序 Vec）；FP 禁 FMA 收缩 |
| F8 | **无全局状态** | 所有状态在 `mqrp_world` 实例内（ADR-0014）；禁 Rust `static mut`/`lazy_static` 可变全局 |

### 2.7 内存所有权（对外）

- 世界：`create` 分配、`destroy` 释放，**同一模块**，符合 R5。
- 句柄表/快照：世界内部 arena，随 `destroy` 释放。
- 传入缓冲（`overlap_sphere` 的 `out`、`contacts` 的 `out`）：**调用方分配、调用方拥有**；模块只写。
- `serialize_state`：经**传入 allocator** 分配，调用方**用同一 allocator** 释放（`alloc->free`）。
- 借入指针（`hull_points`）：**仅调用期有效**，模块立即深拷，调用后调用方可释放。

### 2.8 线程 / 确定性

- **世界非线程安全**：所有调用必须来自**模拟线程**（docs/10 D6、ADR-0018）；一个世界一个线程。多实例 = 多世界，彼此隔离，句柄不跨世界（`generation` + 世界内 slot）。
- **固定步长**：`step(dt)` 用 accumulator 切片，子步恒为 `fixed_dt`；渲染帧率波动不影响模拟步序列（docs/10 D1）。
- **稳定排序**：接触/岛求解按 `body.index` 升序；迭代器输出按 `(a.index,b.index)` 升序（docs/10 D4）。
- **FP**：`-ffp-contract=off`、禁 `-ffast-math`、x86 用 SSE2（与 math 一致）。WASM 天然偏 L2/L3。
- **checksum**：只依赖字段值与 id 序，可跨运行比对，用于回放失步检测（docs/10 §5）。

---

## 3. 依赖与边界

- **可依赖**：
  - `math`（POD 布局与向量/四元数运算）、`core`（句柄/反射描述符/`MQ_ASSERT`）。
  - docs/02 的 `mq_allocator`（跨边界分配器契约）。
  - Rust：`rapier2d`/`rapier3d`（本模块选 **rapier3d**）、`nalgebra`、`bincode`（序列化）、`serde`。版本**锁定**并存 `Cargo.lock`（确定性依赖树）。
- **禁止**（硬红线）：
  - **不得**依赖 renderer/script/particle/scene 的**实现**；与 scene 只经「bake 出的 ECS 组件快照 + 只读变换回吐」接缝。
  - **不得**跨边界暴露 C++ 类型/STL/异常/panic/RTTI（docs/02 §9 反模式）。
  - **不得**持有全局/静态可变状态（ADR-0014）。
  - **不得**在模拟路径引入非确定来源：墙钟、系统熵、线程、`HashMap` 迭代序、指针序、`-ffast-math`。
  - **不得**让渲染插值回写模拟状态（docs/15 §3）。
  - 热路径（逐体迭代/求解）**不得**经 vtable 穿越 `.so`；`.so` 边界只在 `step`/`add_*`/查询等粗粒度操作（docs/02 §7）。
- **与其它模块的接缝**：
  - **scene（11）**：`RigidBody`/`Collider` 为其 ECS 反射组件（docs/06/12）；scene 每帧把组件状态灌入 physics（descriptor），physics 回吐只读变换；`physics_process(dt)` 为独立相位（docs/12 §6）。
  - **animation（12）**：若骨骼/角色需物理驱动，经同一 ECS 组件接口，不直连。
  - **particle（14）**：粒子可查询物理世界（raycast）但**不写回**物理。
  - **renderer（15）**：只读 `get_body_state[_prev]` 与 `interpolate_transform`（docs/13 §8）。
  - **persistence（07）/ host（03）**：经 `serialize_state`/`restore_state` 纳入世界快照/回滚（docs/10 §4）。
  - **observability（09）**：`stats` 上报步耗时/体数/接触对数（docs/15 §6）。

---

## 4. 测试计划（从接口推导，先行）

> 测试框架：**doctest**（与 math 一致）。单测目标**只链接 physics 模块**（`maque::physics`）与公共头，**不链接真 Rapier**；对 Rapier 的依赖经 fake solver 后端隔离。真 Rapier 的确定性放在**独立回归目标**（阶段 3 起）。

### 4.0 测试替身

| 替身 | 作用 | 说明 |
|------|------|------|
| `FakeSolverBackend`（= `NativeSolverBackend` 的确定子集） | 独立单测被测实现 | 极简确定性：AABB/球积分 + 位置修正 + 简单冲量；**不用 Rapier**，实现同一 `mq_physics_v1` 语义 |
| `counting_allocator` | 所有权/泄漏验证 | 计数 `alloc/free`，断言世界销毁后归零、blob 正确释放 |
| `panic_backend`（测试专用） | FFI panic 隔离验证 | Rust 侧注入 `panic!`，断言返回 `MQ_PHYSICS_ERR_PANIC` 且进程存活 |
| `recording_scene`（fake 上游） | 场景集成 | 生成刚体/碰撞体描述序列，灌入 physics 并比对回吐，**不链接 scene 实现** |
| `mq_test_assert.h` | 断言捕获 | 重定向 `MQ_ASSERT`，错误路径可断言而不中断 |

### 4.1 单元测试用例清单（正常 / 边界 / 错误 / 版本 / 所有权）

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| T-01 | 创建/销毁 | 合法 cfg，counting_allocator | `OK`；销毁后 alloc 计数归零 | 正常/所有权 |
| T-02 | `create` 空 alloc | `alloc=NULL` | `INVALID_ARG` | 错误 |
| T-03 | `create` 非法 dt | `fixed_dt=0`/负 | `INVALID_ARG` | 边界 |
| T-04 | 添加/移除刚体 | 加 N 个，逐个移除 | 句柄有效→失效，计数正确 | 正常 |
| T-05 | 句柄 generation | 移除后再加，复用 slot | 新句柄 generation+1；旧句柄 `INVALID_HANDLE` | 边界 |
| T-06 | 无效句柄 | spec/默认 `{0,0}` 或过期 gen | `INVALID_HANDLE` | 错误 |
| T-07 | 向已移除 body 加 collider | 过期 body | `INVALID_HANDLE` | 错误 |
| T-08 | 一 body 多 collider | 加 2 个 | 各自独立句柄，均可移除 | 正常 |
| T-09 | 空 hull / 计数 0 | CONVEX_HULL，`count=0` | `INVALID_ARG` | 边界/错误 |
| T-10 | hull 借入即拷贝 | 传入后立即释放源数组 | 后续 step 不读悬垂（无崩溃/结果不变） | 所有权 |
| T-11 | 设置/读取标量变换 | STATIC body | 往返一致（bit） | 正常 |
| T-12 | 速度/力/冲量 | DYNAMIC body | 经 step 后速度/位置按预期变化 | 正常 |
| T-13 | 对 STATIC 施力 | STATIC body | 无效果（或返回 `OK` 但状态不变） | 边界 |
| T-14 | `step` dt 累加 | 固定 dt 多次调用 `step(0.01)` | 每满 `fixed_dt` 才产生一个模拟子步；`step_count` 正确 | 边界 |
| T-15 | `step` 大 dt 限子步 | `dt=10*fixed_dt*max_substeps` | 子步数 ≤ `max_substeps`（无螺旋死亡） | 边界 |
| T-16 | `step` 负 dt | `dt<0` | `INVALID_ARG`，世界不变 | 错误 |
| T-17 | `raycast` 命中 | 射线指向球 | `has_hit=1`，`toi`/`point`/`normal` 正确 | 正常 |
| T-18 | `raycast` 未命中/超程 | 背向或超 `max_toi` | `has_hit=0` | 边界 |
| T-19 | `raycast` 非法 dir | 未归一化/零向量 | `INVALID_ARG` | 错误 |
| T-20 | `overlap_sphere` | 球重叠范围内多体 | 返回全部命中，`out_count` 正确 | 正常 |
| T-21 | `overlap_sphere` cap 截断 | 命中 > cap | `out_count=cap`（不越界写） | 边界/内存 |
| T-22 | `contacts` 升序 | 多对接触 | 输出按 `(a.index,b.index)` 严格升序 | 确定性 |
| T-23 | sensor 接触 | 标志 SENSOR | 出现在 contacts，`sensor=1`，不产生求解力 | 正常 |
| T-24 | `get_body_state[_prev]` | step 前后 | prev==step 前，curr==step 后 | 正常 |
| T-25 | `interpolate_transform` 端点 | `alpha=0/1/0.5` | 分别等于 prev/curr/中点（旋转 slerp） | 正常/边界 |
| T-26 | 版本不匹配 | `mq_module_query` 传大版本 | 返回 NULL | 版本 |
| T-27 | `struct_size` 截断 | 旧 `config.struct_size` | 接受未知尾部为零，不越界读 | 版本/边界 |
| T-28 | backend 回退 | `backend=auto`，模拟 Rapier 不可用 | 回退 native，仍 `OK`，`stats` 可用 | 错误/降级 |
| T-29 | `serialize→restore` 往返 | 跑若干步后序列化，改状态，恢复 | 恢复后 checksum 与原一致（逐位） | 确定性/所有权 |
| T-30 | `serialize` blob 释放 | 经传入 allocator | `free` 后无泄漏（计数归零） | 所有权 |
| T-31 | `restore` 非法 blob | 截断/魔数错 | `INVALID_ARG`，世界保持原状（强一致） | 错误 |
| T-32 | `destroy` 幂等 | 连续两次 | 第二次安全（内部置空），无 double-free | 边界/所有权 |
| T-33 | 多世界隔离 | 两个 world 同句柄值 | 各自独立，互不影响；句柄不跨世界 | 多实例 |
| T-34 | 零全局状态 | 两实例交替操作 | 结果与各自单独运行一致 | 多实例 |
| T-35 | 错误码可读 | `mq_physics_error_str` | 每个码返回非空静态串 | 正常 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S-01 | **玩家移动碰撞** | 胶囊 DYNAMIC 在静态地面上给水平速度 60 帧 | 不穿地（y≈地面+半高），水平位移≈`v*T`，无抖动 |
| S-02 | **撞墙停走** | 玩家朝静态墙前进 | 被墙阻挡，位置夹在墙前，速度沿法线归零 |
| S-03 | **子弹命中检测** | 快速小体开 CCD，射向靶体；同时用 `raycast` 预测 | CCD 不穿透；`raycast` 命中 `toi` 与体碰撞一致 |
| S-04 | **箱体落地堆叠** | 5 个体重力下落堆叠 1000 帧 | 稳定不炸开，最终 sleeping，高度有序 |
| S-05 | **移动平台** | KINEMATIC 平台带 DYNAMIC 玩家 | 玩家随平台移动（摩擦带动），无相对滑移漂移 |
| S-06 | **斜坡滑落** | 球放斜坡 | 沿坡加速滚落，法向无穿透 |
| S-07 | **传感器触发区** | 玩家进入 SENSOR 区域 | `contacts` 报告对，玩家不被推挤 |
| S-08 | **推箱子** | 玩家施力推 DYNAMIC 箱 | 箱位移方向与力一致，摩擦合理 |
| S-09 | **暂停/恢复** | step 若干后 serialize，销毁重建 restore，继续 step | 与原连续运行 checksum 序列一致 |

### 4.3 确定性用例（A3 / docs/10）

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| D-01 | fake 同输入同输出 | FakeSolver 跑固定场景 N 帧，逐帧 `checksum` | 每帧 checksum 完全一致（bit） |
| D-02 | **真 Rapier L1 回归** | 同构建同平台，同 seed/同输入跑 60/600 帧 | 逐帧 checksum 一致；与记录基线一致 |
| D-03 | 插入顺序无关 | 同一场景，改变 `add_body` 调用顺序 | 最终 checksum 一致（id 排序生效） |
| D-04 | 回放一致 | 记录 S-04 输入日志两遍 | 每帧 checksum 序列一致；不一致时报首个分歧帧 |
| D-05 | `serialize→restore→checksum` | 中间点快照恢复后继续 | 与原轨迹 checksum 逐帧一致 |
| D-06 | sleeping 稳定 | 让堆叠进入睡眠，再跑 300 帧 | 睡眠体 checksum 不变；无自发唤醒 |
| D-07 | 编译选项守护 | `-O0` vs `-O2`，`-ffp-contract=off` | checksum 一致（L2 目标，非硬承诺，超差即记录） |
| D-08 | 跨平台（CI 矩阵） | 原生 x86_64/aarch64/WASM 跑同场景 | 报告一致性；WASM 期望最稳（L2/L3） |
| D-09 | 关日志不变 | 开/关 stats 采集 | checksum 一致（P0） |

> **决策门（Gate）**：若 D-02 在目标平台**逐帧一致**，Rapier 保留为默认真后端；若**不稳定**，则保持 vtable 不变，默认切 `NativeSolverBackend`（ADR-0028 的"不达标退回自研"），并把 Rapier 限制为可选的视觉/非权威后端。Gate 结果写入 `docs/15` 附录。

### 4.4 边界 / 稳定性用例

| # | 用例 | 输入 | 期望 |
|---|------|------|------|
| B-01 | 静止接触 | 球停在地面 | 速度幅值 < 阈值，无 jitter（位置漂移 < ε） |
| B-02 | 大堆叠 | 20 箱堆叠 2000 帧 | 不爆、不穿、最终 sleeping |
| B-03 | 零重力 | `gravity=0` | 自由体匀速直线 |
| B-04 | 退化形状 | 球 `radius=0` / 立方体零半长 | `INVALID_ARG` |
| B-05 | 极端密度 | `density=1e9` | 不产生 NaN/Inf（`is_finite` 检查） |
| B-06 | 巨大速度 | 未开 CCD 的高速体 | 允许穿透（文档化需要 CCD）；开 CCD 后不穿透 |
| B-07 | 睡眠阈值边界 | 慢速体 | 进入睡眠；外部 `wake_body` 可唤醒 |
| B-08 | max_substeps 饱和 | 连续超大 dt | 子步封顶，模拟时间不追平（防死循环），`stats` 可见丢弃 |

### 4.5 渲染插值只读用例（关键）

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| I-01 | 插值不污染模拟 | step 后取 `checksum0`，调用 `interpolate_transform` 100 次，再取 `checksum1` | `checksum0==checksum1` |
| I-02 | 读回不污染 | 反复 `get_body_state[_prev]`/`stats`/`raycast` 后 checksum 不变 | 相等 |
| I-03 | 帧率无关 | 同一物理序列用 alpha=0.3/0.7 采样 | 位置在两帧间线性、旋转在 slerp 弧上 |
| I-04 | restore 清插值 | restore 后立即 `interpolate_transform` | 等于 curr（prev=curr） |

### 4.6 独立单测保障

- 单测目标 `target_link_libraries(... maque::physics)`，**不含** Rapier、scene、renderer。
- `#include` 白名单：仅 `mq_physics.h` + `<cstdint>` 等（CI 检查）。
- 真 Rapier 测试放在**独立可执行**（`physics_rapier_tests`），默认 CI 跑，但**不阻塞** fake 单测。
- 场景用例用 `recording_scene` fake 驱动，验证接口自洽而非 scene 实现。

---

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：写定 `mq_physics.h` + `mq_physics_sys.h`；确定枚举/flags/错误码/`struct_size` 规则；`mq_module_query` 与 vtable 装配；与 math 的 `static_assert` 对拍；`MQ_PHYSICS_ABI_VERSION=1`。**接口不完备不得进入阶段 1**。
2. **阶段 1 · 测试骨架（红）**：落地 §4.1 全部接口用例 + `FakeSolverBackend` 桩 + `counting_allocator` + `panic_backend`。此时实现为空壳 → 全红。
3. **阶段 2 · fake/native 后端到绿**：实现 `IPhysicsBackend` 与 `NativeSolverBackend`（球/AABB 积分 + 位置修正 + 简单冲量 + 固定迭代 + id 稳定排序 + prev/curr 快照 + checksum + serialize/restore）。跑通 T-01..T-35、S-01..S-09（在 native 下）、D-01/D-03/D-05/D-06、B-01..B-08、I-01..I-04。**此阶段不引入 Rapier**，证明接口与确定性逻辑自洽。
4. **阶段 3 · 接 Rapier（真后端）**：建 Rust crate `maque_physics_sys`，`repr(C)` 结构对齐（`static_assert`/`#[repr(C)]` + Rust 侧 size 断言），每入口 `catch_unwind`；实现 `RapierBackend` 经 `mq_physics_sys.h` 调用；跑 T/S 全量子集 + **D-02 L1 回归**；打通 `panic` 隔离（F6）与 ownership（F4）。
5. **阶段 4 · 确定性 Gate + 性能/体积**：在目标平台跑 D-02/D-04/D-08；**执行决策门**（保留 Rapier 或默认 native）；测 `stats.last_step_micros`、内存、`.so`/WASM 体积，对照预算；补 B-06（CCD）。
6. **阶段 5 · 上游 fake 集成 + 验收**：用 `recording_scene` 做端到端（descriptor 灌入 → step → 只读回吐插值）；对接 host 的 `mq_module_query` 加载；确认无跨模块实现依赖、零全局状态、异常/panic 不越界。

---

## 6. 验收标准（DoD）

- **接口冻结**：`mq_physics.h` 与 `mq_physics_sys.h` 签名与 §2 一致；被 `recording_scene` fake 与 host 加载成功；`abi_version`/`struct_size` 规则自洽。
- **单测覆盖率 ≥ 85%**（接口密集 + 后端双实现，行/分支覆盖达标）。
- **确定性用例通过**：D-01/D-03/D-05/D-06/D-09 全绿（fake/native）；**D-02（真 Rapier L1）逐帧 checksum 一致**，否则按 §4.3 决策门切换到 native 并留存证据。
- **场景用例通过**：S-01..S-09 与 B-01..B-08 通过；I-01..I-04 证明插值**只读**。
- **性能/内存预算**：
  - 单步（如 200 DYNAMIC 体 + 堆叠）`last_step_micros` ≤ 预算（目标：稳态 < 2ms@60Hz，内嵌可放宽）；
  - 世界内存与快照 blob 有明确上界；`serialize` 不产生额外持久分配（走传入 allocator）；
  - `.so` + Rapier 静态链接体积在校准预算内；WASM 构建可产出并跑 D-08。
- **红线**：无跨模块实现依赖；**零全局可变状态**（多世界隔离通过）；**无异常/panic 跨边界**（F2/F6 测试通过）；ABI 面不含 C++ 类型/STL/指针语义。
- **可观测**：`stats` 上报步耗时/体数/接触对/岛数；`checksum` 可喂回放失步二分（docs/10 §7）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **Rapier 确定性不达标**（求解顺序/SIMD/浮点） | 固定步长 + 固定迭代 + id 稳定排序 + 注入 seed；**D-02 L1 回归为 Gate**；不达标切 `NativeSolverBackend`（同 vtable），Rapier 降为可选（ADR-0028） |
| **Rust↔C++ FFI 边界**（panic/布局/所有权） | 只 C ABI + POD（F1/F2）；句柄整数传递（F3）；Rust 分配 Rust 释放、blob 走传入 allocator（F4）；`catch_unwind` + `panic=unwind`（F6）；`repr(C)` + 双侧 size 断言 |
| **体积过大**（Rapier + nalgebra + serde） | 锁 `Cargo.lock`；开启 `opt-level`/LTO/`strip`；关闭未用 feature；嵌入式评估按需裁剪；必要时 native 后端为小体积构建 |
| **跨平台逐位不一致**（浮点/libm） | 明确 **L1 为默认承诺**，向 L2 努力；关 FMA 收缩/fast-math；WASM 优先；D-08 只报告不硬断 |
| **后端版本漂移**（Rust `.a` vs C++ `.so`） | `mqrp_abi_version()` 在 `create` 校验，不一致拒绝（§2.5） |
| **快照不完整破坏回滚** | `serialize` 须含 Rapier 内部状态（pipeline/broadphase/islands）；D-05 守门 |
| **迭代顺序不稳定** | 全部按整数 id 升序；Rust 侧禁 `HashMap` 迭代序出接口（F7） |
| **热路径经 `.so`/vtable 拖慢** | 边界粗粒度（step/add/query）；逐体迭代在模块内部（docs/02 §7） |
| **TRIMESH/CCD 预算失控** | 明确预算与限制（docs/07 §8）；B-06 覆盖；文档化"需 CCD"语义 |
| **上游把渲染插值写回模拟** | `interpolate_transform` 只读 + I-01 断言 checksum 不变；接口注释强约束 |

## 8. Owner agent

- **角色标识**：`agent-physics`
- **职责边界**：仅负责 `physics` 模块的**接口冻结、测试与实现**（`modules/world/physics/`：`mq_physics.h`、`mq_physics_sys.h`、C++ vtable 装配、`NativeSolverBackend`、`Rust crate maque_physics_sys`、按 §2.6 与 math 的 `static_assert` 对拍、确定性 Gate）。**不修改**其它模块实现；Rapier 确定性 Gate 结论以证据形式提交，接口/降级决定暴露给 host 与 renderer 的 owner。
