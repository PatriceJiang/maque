# 14 · particle 模块计划

> Owner agent：`agent-particle` ｜ 上游：`docs/00`（根 / A3 确定性 / ADR-0027）、`docs/15 §1/§4/§5`（主参考）、`docs/10`（固定步长 + 引擎 RNG + checksum）、`docs/13 §8`（渲染只读消费）。
> 所在层：**世界层**（阶段 D）。交付顺序：`scene → animation → physics → particle`。
> 硬约束：CPU 粒子**确定且参与回放**；GPU 粒子**仅视觉、排除出模拟确定性**（ADR-0027、docs/15 §4）。

---

## 1. 模块概述

- **职责**：为世界提供**粒子发射与演化**能力，拆两条互不污染的通路：
  1. **CPU 粒子**：运行在**模拟线程、固定步长、引擎 RNG** 上，状态是模拟状态的一部分（可快照、进 checksum、可回放）。
  2. **GPU 粒子（compute）**：由 renderer 的 compute pass 演化，**不回写模拟**，**不参与 checksum/快照**，纯视觉；本模块只负责其**配置、预算与派发描述**。
- **所在层**：**世界层**。权威数据在 **ECS**：`ParticleEmitter` 是反射组件（docs/06、docs/15 §1），可从节点树 **bake** 而来（docs/12 §4 单向 bake）。
- **上游文档**：`docs/15 §4`（粒子两种通路 + 发射器 + 预算 + 池化）、`docs/10 D1/D2/D4/D5/D6`（固定步长、引擎 RNG、id 排序、零初始化、单线程）、`docs/07 §8/§9`（池化、预算、禁地址依赖）、`docs/13 §8`（渲染单向消费）、`docs/12 §4`（高基数走 ECS）。
- **一句话**：**确定演化的 CPU 粒子池 + 只读派发的 GPU 粒子描述**，二者**接口同源、确定性归属严格分离**。

### 1.1 与双视图 / ECS 的关系（docs/12 §4/§5）

- **编辑期**：`ParticleEmitter` 作为节点/组件在节点树中被编排（引用贴图/网格资产 id）。
- **运行期**：bake 为实体，粒子模块在 ECS 上按 emitter 迭代（高基数走 ECS，不占节点树）。
- 粒子**单实例状态**（每个粒子的 pos/vel/age）**不由反射逐粒子暴露**（高基数），只由模块内部的 SoA 承载；对 AI/控制平面暴露的是**聚合统计 + 发射器配置**（A1/A2），实体级快照只存发射器配置与 RNG 游标。

---

## 2. 接口层设计（完备，先冻结）

> 跨模块一律 **C ABI + 版本化 vtable + `struct_size`**（ADR-0013、docs/02）。公共头 `mq_particle.h` 纯 C，不含 `std::` / 指针字段 / vtable 暴露于 POD，**先冻结再实现**。
> 目录：`modules/world/particle/`，导出 interface target `maque::particle`。

### 2.1 公共头骨架与版本

```c
/* modules/world/particle/include/particle/mq_particle.h —— 纯 C，C ABI */
#pragma once
#include <stdint.h>
#include "math/mq_math.h"     /* 仅取布局别名 mq_math_vec3 / mq_math_vec4（docs/01 §2.1） */

#define MQ_PARTICLE_ABI_VERSION 1u

/* ---- 状态码：无异常越界，无 -1 魔法值 ---- */
typedef enum mq_particle_result {
    MQ_PARTICLE_OK               = 0,
    MQ_PARTICLE_ERR_INVALID_ARG  = 1,   /* desc 非法 / 区间 min>max / 容量为 0 */
    MQ_PARTICLE_ERR_INVALID_HANDLE = 2, /* 句柄 generation 失配（悬垂） */
    MQ_PARTICLE_ERR_NOT_RESERVED   = 3, /* 未 reserve 就 sim/extract */
    MQ_PARTICLE_ERR_UNSUPPORTED    = 4, /* backend 组合不支持（如 CPU 请求 GPU 特性） */
    MQ_PARTICLE_ERR_OOM            = 5  /* 冷路径分配失败（reserve/create） */
} mq_particle_result;

typedef enum mq_particle_backend {
    MQ_PARTICLE_CPU = 0,   /* 确定，进模拟，participates in checksum */
    MQ_PARTICLE_GPU = 1    /* 仅视觉，排除出确定性（docs/15 §4） */
} mq_particle_backend;

typedef enum mq_particle_space {
    MQ_PARTICLE_SPACE_LOCAL = 0,   /* 跟随发射器变换 */
    MQ_PARTICLE_SPACE_WORLD = 1    /* 发射后脱离发射器 */
} mq_particle_space;

typedef enum mq_particle_exhaustion {
    MQ_PARTICLE_DROP_NEWEST   = 0, /* 池满丢弃新粒子（默认，确定） */
    MQ_PARTICLE_DROP_OLDEST   = 1, /* 回收最老存活槽（按槽序，确定） */
    MQ_PARTICLE_RECYCLE_OLDEST = 2 /* 同 DROP_OLDEST 但立即重用于新粒子 */
} mq_particle_exhaustion;

typedef enum mq_particle_blend {
    MQ_PARTICLE_BLEND_ALPHA     = 0,
    MQ_PARTICLE_BLEND_ADDITIVE  = 1
} mq_particle_blend;

/* ---- 句柄：index + generation，跨 .so 稳定（docs/02） ---- */
typedef struct mq_particle_handle {
    uint32_t index;
    uint32_t generation;
} mq_particle_handle;

typedef struct mq_particle_range_f32 { float min, max; } mq_particle_range_f32; /* 8B */

/* ---- 冻结的组件 / 运行时配置（POD，反射组件 ParticleEmitter 的内存布局即此） ---- */
typedef struct mq_particle_emitter_desc {
    uint32_t            struct_size;          /* = sizeof，版本化按 docs/02 */
    uint32_t            abi_version;          /* = MQ_PARTICLE_ABI_VERSION */
    mq_particle_backend backend;              /* CPU / GPU，决定确定性归属 */
    mq_particle_space   space;
    mq_particle_exhaustion exhaustion;
    mq_particle_blend   blend;
    uint32_t            flags;                /* bit0=loop bit1=playing bit2=align_to_velocity bit3=gpu_sort */
    uint32_t            max_particles;        /* 硬上限（预算），reserve 依据 */
    float               emission_rate;        /* 粒子/秒（0 = 仅 burst） */
    uint32_t            burst_count;          /* 触发一次爆发发射数 */
    mq_particle_range_f32 lifetime;           /* 秒，[min,max]，min>0 */
    mq_particle_range_f32 speed;              /* 单位/秒 */
    mq_particle_range_f32 size_start;
    mq_particle_range_f32 size_end;
    mq_particle_range_f32 rotation_rate_rad;  /* 弧度/秒 */
    float               cone_half_angle_rad;  /* 发射锥半角 [0,π]，0=单向 */
    mq_math_vec3        direction;            /* 发射母方向（发射器局部空间，未归一化则内部归一化） */
    mq_math_vec3        gravity;              /* 加速度 */
    float               drag;                 /* 线性阻力系数 ≥ 0 */
    uint32_t            color_start_rgba8;
    uint32_t            color_end_rgba8;
    uint64_t            texture_asset_id;     /* 经 VFS 解析（docs/04），0=默认 sprite */
    uint64_t            mesh_asset_id;        /* 0 = billboard */
} mq_particle_emitter_desc;

/* ---- 注入依赖：RNG（引擎实例 RNG 的 C 视图，docs/10 D2） ---- */
typedef struct mq_particle_rng_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void*    instance;
    uint32_t (*next_u32)(void* instance);                 /* 原始 32bit */
    /* f32∈[0,1)：实现必须基于 next_u32，禁止 libm/系统熵 */
    float    (*next_f32)(void* instance);
} mq_particle_rng_v1;

/* ---- 注入依赖：allocator（分配者释放，docs/07 R5） ---- */
typedef struct mq_particle_alloc_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void*    instance;
    void*  (*alloc)(void* instance, uint64_t size, uint64_t align);
    void   (*free)(void* instance, void* ptr);
} mq_particle_alloc_v1;

/* ---- 提取输出（渲染消费，模块持有缓冲，valid until 下次 sim/extract） ---- */
typedef struct mq_particle_extract_out {
    uint32_t        struct_size;
    uint32_t        abi_version;
    uint32_t        count;               /* 可见粒子数 */
    uint32_t        backend;             /* 本次提取来自 CPU / GPU 派发 */
    const mq_math_vec3* position;        /* SoA：count 个 */
    const mq_math_vec3* size_color_pack; /* 复用：size.x=尺寸, 其余见文档 */
    const float*    rotation;            /* count 个 */
    const uint32_t* color_rgba8;         /* count 个 */
    uint64_t        content_hash;        /* 提取内容指纹（视觉，不入模拟 checksum） */
} mq_particle_extract_out;

/* ---- GPU 派发描述（renderer 的 compute pass 消费；本模块不执行） ---- */
typedef struct mq_particle_gpu_dispatch {
    uint32_t struct_size;
    uint32_t abi_version;
    mq_particle_handle emitter;
    uint64_t texture_asset_id;
    uint64_t mesh_asset_id;
    uint32_t max_particles;
    uint32_t dispatch_groups_x;
    uint32_t flags;
    /* 发射参数打包（供 WGSL uniform；本模块只填充，不做 GPU 演化） */
    mq_particle_emitter_desc params;
} mq_particle_gpu_dispatch;

/* ---- 聚合统计（A2 指标，冷读） ---- */
typedef struct mq_particle_stats {
    uint32_t live_particles;     /* CPU 存活 */
    uint32_t capacity;           /* 已 reserve 总容量 */
    uint32_t emitted_total;
    uint32_t dropped_total;      /* 因耗尽丢弃 */
    uint32_t emitter_count;
    uint32_t gpu_emitter_count;
} mq_particle_stats;

/* ---- 版本化 vtable（完整操作面） ---- */
typedef struct mq_particle_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    void*    instance;

    /* 冷路径：实例生命周期与预算 */
    mq_particle_result (*init)(void* instance, const mq_particle_alloc_v1* alloc);
    void               (*shutdown)(void* instance);
    mq_particle_result (*reserve)(void* instance, uint32_t total_capacity);   /* 预留全部池，之后热路径零分配 */

    /* 冷路径：发射器生命周期 */
    mq_particle_result (*create)(void* instance, const mq_particle_emitter_desc* desc, mq_particle_handle* out);
    mq_particle_result (*destroy)(void* instance, mq_particle_handle h);
    mq_particle_result (*set_playing)(void* instance, mq_particle_handle h, int playing);

    /* 每帧由 ECS/场景喂入发射器变换（按 id 序，确定，docs/10 D4） */
    mq_particle_result (*set_transform)(void* instance, mq_particle_handle h,
                                        const mq_math_vec3* position, const mq_math_vec3* orientation_euler);
    mq_particle_result (*trigger_burst)(void* instance, mq_particle_handle h, uint32_t count);

    /* 热路径：CPU 粒子步进（固定 dt；RNG 注入；零分配） */
    mq_particle_result (*sim_step)(void* instance, float dt, mq_particle_rng_v1* rng);

    /* 热路径：提取（CPU 或 GPU 派发描述），零分配 */
    mq_particle_result (*extract)(void* instance, mq_particle_extract_out* out);
    mq_particle_result (*gpu_dispatch)(void* instance, mq_particle_gpu_dispatch* out, uint32_t* count);

    /* 诊断 / 确定性 */
    mq_particle_stats  (*stats)(const void* instance);
    uint64_t           (*cpu_checksum)(const void* instance);   /* 仅 CPU 粒子状态 */

    /* 回放 / 快照支持 */
    mq_particle_result (*cpu_snapshot)(const void* instance, void* dst, uint64_t* inout_size);
    mq_particle_result (*cpu_restore)(void* instance, const void* src, uint64_t size);
} mq_particle_v1;
```

- **命名空间/符号**：`mq_particle_*` C 符号；C++ 侧薄封装 `particle::` 仅在模块内部。
- **组件 `ParticleEmitter`**：其 schema 条目（`namespace: particle`）即 `mq_particle_emitter_desc` 的布局唯一真相；codegen 产出 POD 头 + 反射描述，`static_assert(sizeof(...))` 守门（写回 `schema/`，见 docs/06）。

### 2.2 公开类型清单

| 类型 | 尺寸/形态 | 说明 |
|------|-----------|------|
| `mq_particle_handle` | 8 B | `{index, generation}`，跨 `.so` 稳定句柄 |
| `mq_particle_range_f32` | 8 B | `{min,max}`，采样的确定区间 |
| `mq_particle_emitter_desc` | 固定 POD | 发射器配置；即反射组件布局，含 `struct_size`/`abi_version` |
| `mq_particle_rng_v1` | vtable | 注入的引擎 RNG 视图（`next_u32`/`next_f32`） |
| `mq_particle_alloc_v1` | vtable | 注入分配器（冷路径用） |
| `mq_particle_extract_out` | POD | SoA 提取结果，模块持有 |
| `mq_particle_gpu_dispatch` | POD | GPU 派发描述，renderer 消费 |
| `mq_particle_stats` | POD | 聚合指标 |
| `mq_particle_v1` | vtable | 全操作面 |

> 内部类型（**不对外**）：`particle_slot`（SoA：`pos/vel/age/lifetime/size0/size1/rot/rot_rate/seed`）、`emitter_state`（配置副本 + 发射累加器 + 自由链表 + 槽世代）。均 packed POD，`static_assert` 定尺寸。

### 2.3 操作面语义

**冷路径（可在加载 / bake 时分配）**

| 函数 | 语义 |
|------|------|
| `init` | 绑定注入 allocator；置零所有状态（docs/10 D5）。重复 init 返回 `INVALID_ARG`。 |
| `shutdown` | 释放全部池；之后句柄全失效。 |
| `reserve(total_capacity)` | 一次性预留**全部** CPU 池（按各 emitter `max_particles` 之和 + 分块裕量），此后 `sim_step`/`extract` **零分配**。可在 create 前或后调用；不足时 sim 返回 `NOT_RESERVED`。 |
| `create` | 校验 `desc`（`struct_size`/`abi_version`、区间 `min<=max`、`lifetime.min>0`、`max_particles>0`、CPU 容量和 ≤ reserve 余量）；分配发射器槽 + 粒子池；返回句柄。GPU 发射器只登记配置 + 预算，不分配 CPU 粒子。 |
| `destroy` | 归还发射器槽与池；generation++ 使旧句柄失效。 |
| `set_playing` | 开关发射（不影响已存活粒子演化）。 |
| `set_transform` | 更新发射器变换（LOCAL 空间发射用）；模拟内**按句柄 id 升序**调用。 |
| `trigger_burst` | 立即发射 `count` 个（受 `max_particles` 与 exhaustion 策略约束）。 |

**热路径（模拟帧内，**零分配、无锁、无异常、无 I/O、无日志副作用**）**

| 函数 | 语义 |
|------|------|
| `sim_step(dt, rng)` | 按**句柄 id 升序**遍历 CPU 发射器；先按 `emission` 累加器发射新粒子，再积分（重力/阻力/位置）、老化、回收死亡到自由链。RNG 只经注入的 `rng`；同 `dt` 同 `rng` 序列 → 同结果。GPU 发射器**跳过**。 |
| `extract(out)` | 把 CPU 粒子打包为 SoA 提取结果，并生成 GPU 发射器派发描述（分别经 `extract` 与 `gpu_dispatch`）。**只读**，不回写（docs/13 §8）。 |
| `gpu_dispatch(out, count)` | 生成每个 GPU 发射器的派发描述（容量、workgroup、打包参数、资产 id），供 renderer compute pass 消费。**不读取 GPU 粒子状态**。 |

**诊断 / 确定性**

| 函数 | 语义 |
|------|------|
| `stats` | 冷读聚合计数器（供 docs/09 指标）。 |
| `cpu_checksum` | 计算**仅 CPU 粒子状态**的 64bit 指纹：按 emitter id 升序、槽 index 升序遍历，混入位置/速度/寿命/年龄/尺寸（`bit_cast<uint32>` 后 FNV-1a）。**GPU 粒子与提取缓冲不参与**。 |
| `cpu_snapshot` / `cpu_restore` | 序列化/恢复 CPU 粒子池 + 发射器累加器 + RNG 游标（由调用方传入 RNG 状态，或快照内含 rng 计数器）；用于回滚/回放（docs/10 §4）。GPU 粒子状态**不入快照**。 |

### 2.4 发射 / 演化模型（冻结）

- **发射率**：`accum += emission_rate * dt`；`n = (uint32)floor(accum)`；`accum -= n`。**先确定性决定 n，再逐粒子按槽序发射**。`emission_rate=0` 仅 burst。
- **爆发**：`trigger_burst(count)` 立即请求 `count`，同样受上限/耗尽策略约束。
- **采样顺序（确定且固定）**：每个粒子依次取 `lifetime←lerp(rng.next_f32, lifetime)`、`speed←lerp(...speed)`、`size←lerp(...size_start)`、`rotation_rate←lerp(...)` —— **顺序写死**，任何重排即破坏回放。
- **方向**：以 `direction` 为轴，在 `cone_half_angle_rad` 内按 `rng` 取锥内均匀方向（用两个确定角度量 cosθ、φ）。
- **积分**：`vel += (gravity - drag*vel) * dt`（显式欧拉，固定顺序）；`pos += vel*dt`；`age += dt`；`t = age/lifetime`，`size = lerp(size0,size1,t)`，`color = lerp(color0,color1,t)`。
- **死亡**：`age >= lifetime` 回收；回收顺序 = 遍历顺序（确定）。
- **池复用**：自由链取用顺序**确定**（FIFO，按槽 index 拉链），**绝不按地址**（docs/10 D4）。耗尽按 `exhaustion` 策略，均确定：`DROP_NEWEST` 丢弃第 `n` 个新请求；`DROP_OLDEST` 回收**当前最小 age 槽中 index 最小者**；`RECYCLE_OLDEST` 同上但立即使其重生。

### 2.5 错误模型

- **无异常、无 `std::error_code` 于 ABI 面**：全部返回 `mq_particle_result`；`noexcept` 语义。
- **前置条件非法**（`desc` 越界、`min>max`、未 reserve、悬垂句柄）：返回对应错误码，**不改状态**（strong guarantee）；Debug 下额外 `MQ_ASSERT` 便于 A2 采集。
- **无 UB**：不合法输入绝不越界写；`extract` 在未 reserve 时返回空结果 + `NOT_RESERVED`，不崩。
- **分配失败**只发生在 `reserve`/`create` 冷路径 → `OOM`；热路径**永不分配**故无 OOM。

### 2.6 版本化

- `mq_particle_emitter_desc` / 所有输出结构首两字段为 `struct_size` + `abi_version`；消费方按 `struct_size` 判前缀兼容（前向扩展：新增字段追加在尾，老消费方按旧 size 读取）。
- vtable `mq_particle_v1` 同样带 `struct_size`/`abi_version`；模块注册时（docs/03）宿主校验 `MQ_PARTICLE_ABI_VERSION`。
- **任何** `desc` 字段增删/重排、内部 `particle_slot` 布局变化、采样顺序变化、checksum 算法变化 → **bump** `MQ_PARTICLE_ABI_VERSION`。
- `ParticleEmitter` 组件 schema 版本与 ABI 版本**同步发布**（docs/06）。

### 2.7 内存所有权

- **分配者释放**：粒子池由注入 `mq_particle_alloc_v1` 分配，只由本模块 `shutdown` 释放；调用方不持有内部指针。
- **提取缓冲归模块**：`extract_out` 中的 SoA 指针指向模块自有复用缓冲，**valid until 下次 sim/extract**；renderer 只读、拷贝进自己的 GPU 缓冲（docs/13 §5）。
- `desc` 按值传参，模块内部拷贝，调用方栈上构造即可。
- 对外**永不暴露裸内部指针**（docs/00 §5）；一切寻址经句柄 / SoA 输出。

### 2.8 线程 / 确定性

- **模拟线程单线程**（docs/10 D6）：`sim_step`、`set_transform`、`trigger_burst`、`create/destroy` 均在模拟线程串行调用。
- **回归确定性硬规则**：
  1. 只消费注入 RNG，禁系统熵 / `std::random`（docs/10 D2）。
  2. 只用 sim time，禁墙钟（docs/10 D3）。
  3. 一律按 **emitter id 升序 + 槽 index 升序**遍历/哈希，禁地址依赖（docs/10 D4）。
  4. 池状态零初始化，复用不引入未初始化值（docs/10 D5）。
  5. 浮点：关 `-ffast-math`、`-ffp-contract=off`；`lerp/floor` 等只用正确舍入项（docs/10 D8、docs/01 §2.8）。
- **GPU 粒子隔离**：其演化在 renderer compute pass，**本模块不读回**；`cpu_checksum`/`cpu_snapshot` **显式排除** GPU 粒子。GPU 的 seed 可为独立流，**不承诺回放一致**（视觉可接受抖动）。
- **观测不改行为**：`stats` 为冷读计数，不影响遍历顺序与 RNG 消耗（docs/09 §6）。

---

## 3. 依赖与边界

- **可依赖**：
  - **math**（`maque::math`）：仅用其 C 布局别名 `mq_math_vec3` 与 `lerp/clamp` 等确定性纯函数。
  - **core/data**：句柄规则、`MQ_ASSERT`、反射描述符（组件注册）。
  - **注入依赖**（经 vtable 传入，非链接依赖）：引擎 RNG（docs/10）、allocator（docs/07）。
  - C 标准库：`<stdint.h>`、`<stddef.h>`；C++ 实现内部 `<bit>`、`<type_traits>`（不进 ABI 面）。
- **禁止（硬红线）**：
  - 不得依赖 **renderer / scene / physics 的内部实现**；只暴露描述与 SoA，供其消费。
  - 不得持有**全局可变状态**（ADR-0014）：全部状态在实例 context。
  - 不得在 ABI 面出现 `std::` / 裸指针字段 / vtable 字段于 POD / 平台相关类型（docs/07 §4）。
  - 热路径（`sim_step`/`extract`/`gpu_dispatch`）**不得分配、加锁、抛异常、I/O、日志**，**不得经虚函数**（docs/00 §6③）。
  - 不得读 GPU 粒子状态回模拟（会破坏确定性）。
  - 不得使用系统熵 / 墙钟 / 指针序。
- **接缝**：
  - **scene（11）**：bake 时由节点树组件生成 `mq_particle_emitter_desc` 并 `create`；模拟帧按 id 序 `set_transform`。
  - **renderer（15）**：消费 `extract_out`（CPU）与 `gpu_dispatch`（GPU），在 compute pass 演化 GPU 粒子并绘制；**不回写**。
  - **physics（13）**：可选——粒子可采样碰撞结果，但粒子不反向施加力（单向）。
  - **observability（09）**：`stats` 作为指标源。
  - **persistence（08）**：`cpu_snapshot` 复用其 codec/arena。

---

## 4. 测试计划（从接口推导，先行）

> 框架：**doctest**（单头、无依赖）。每个测试目标**只链接 `maque::particle` + math**，不链接 scene/renderer；上游一律 **fake**。
> 先写（红）后实现（绿）。

### 4.1 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| T-01 | 组件布局 | `sizeof`/`offsetof(mq_particle_emitter_desc)` | 与 schema 一致；`static_assert` 通过 | 正常 |
| T-02 | 版本兼容 | `abi_version` 匹配 / 不匹配 | 匹配 OK；不匹配 `INVALID_ARG` | 版本 |
| T-03 | `init` 零初始化 | 新建实例 | 全状态为 0；`stats` 全 0 | 正常 |
| T-04 | 重复 `init` | 二次 init | `INVALID_ARG`，状态不变 | 错误 |
| T-05 | `reserve` 预算 | capacity=N | 后续 create 成功；`stats.capacity>=N` | 正常 |
| T-06 | 未 reserve 即 sim | 无 reserve | `NOT_RESERVED`，不崩 | 错误 |
| T-07 | `create` 参数校验 | `min>max` / `lifetime.min=0` / `max_particles=0` | 各自 `INVALID_ARG` | 错误 |
| T-08 | `create` 容量超预算 | CPU 上限和 > reserve | `OOM`/`INVALID_ARG`，无半初始化 | 边界 |
| T-09 | 悬垂句柄 | destroy 后用旧句柄 | `INVALID_HANDLE` | 错误 |
| T-10 | 句柄 generation | destroy→create 复用 index | 新句柄 generation 递增；旧句柄失效 | 边界 |
| T-11 | 发射率整数步 | rate=60, dt=1/60, 1 步 | 恰发射 1 | 正常 |
| T-12 | 发射率小数累加 | rate=10, dt=0.25（×4 步） | 累计恰 10（无丢/无多） | 边界 |
| T-13 | 发射率 0 | rate=0 | 永不自动发射；burst 仍可 | 边界 |
| T-14 | 发射率极大 | rate=1e6, dt=1/60 | 受 `max_particles` 约束，不越界 | 边界 |
| T-15 | 寿命采样边界 | fake RNG 返回 0.0 / 1.0 | 分别取 `lifetime.min` / `.max` | 边界 |
| T-16 | 速度采样边界 | 同上 | `speed.min` / `.max` | 边界 |
| T-17 | 锥角 0 | cone=0 | 全部沿 `direction`（单位化后） | 边界 |
| T-18 | 锥角 π | cone=π | 分布覆盖全向（抽样统计确定） | 边界 |
| T-19 | 方向未归一化 | direction=(0,0,5) | 内部归一化；速度模长 = 采样 speed | 边界 |
| T-20 | `trigger_burst` | count=k | 立即发射 k（受上限约束） | 正常 |
| T-21 | 积分-重力 | vel0=0, gravity=(0,-9.8,0), dt | v=gravity*dt；pos 按步进 | 正常 |
| T-22 | 积分-阻力 | drag>0 | 速度按 `(g-drag*v)*dt` 演化 | 正常 |
| T-23 | 尺寸插值 | size0≠size1, t=0.5 | `size==lerp(size0,size1,0.5)` | 正常 |
| T-24 | 颜色插值 | color0/color1 | 分量按 t 线性混合 | 正常 |
| T-25 | 寿命到期回收 | age≥lifetime | 槽归还自由链，`live_particles--` | 正常 |
| T-26 | 池耗尽 DROP_NEWEST | 容量满再发射 | 新粒子丢弃；`dropped_total++`；存活不变 | 边界 |
| T-27 | 池耗尽 DROP_OLDEST | 容量满再发射 | 最小 age 槽被回收并重用 | 边界 |
| T-28 | 池复用确定性 | 死亡→新发射 | 复用的槽 index 序列与自由链顺序一致 | 边界 |
| T-29 | 自由链 FIFO 顺序 | 多个槽同时释放 | 复用严格按释放顺序 | 边界 |
| T-30 | LOCAL vs WORLD | 移动发射器 | LOCAL 存活粒子随动；WORLD 不回写位置 | 正常 |
| T-31 | `extract` 内容 | 有 N 存活 | `count==live_particles`；SoA 与内部状态一致 | 正常 |
| T-32 | `extract` 只读 | 连续两次 extract | CPU 状态与 checksum 不变 | 正常 |
| T-33 | 未 reserve extract | 未 reserve | 返回空 + `NOT_RESERVED` | 错误 |
| T-34 | GPU 发射器不进 CPU 池 | create GPU emitter | `live_particles` 不计入；`sim_step` 跳过 | 正常 |
| T-35 | `gpu_dispatch` 内容 | CPU+GPU 混合 | 只产出 GPU 发射器派发；参数=desc | 正常 |
| T-36 | `stats` 计数器 | 发射/丢弃/存活 | 各计数与操作序列一致 | 正常 |
| T-37 | `shutdown` 幂等/释放 | init→shutdown→shutdown | 第二次安全；分配计数归零 | 内存所有权 |
| T-38 | 零分配热路径 | 替换全局 `operator new` 计数 | sim/extract/gpu_dispatch 期间分配为 0 | 内存所有权 |
| T-39 | 快照往返 | `cpu_snapshot→cpu_restore` | 恢复后 `cpu_checksum` 逐位一致 | 内存所有权 |
| T-40 | 快照尺寸协商 | 传 NULL dst 取 size | 返回所需尺寸，不改状态 | 边界 |
| T-41 | 无异常/无越界 | 全错误输入扫描 | 无崩溃、无 ASan 报错 | 错误 |
| T-42 | `set_playing=0` | 关闭后 sim | 不再新发射；存活仍演化 | 正常 |

### 4.2 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S-01 | **爆炸特效（一次性）** | burst=200，CPU 后端，单帧触发；跑 2s | 峰值 ≤ 200；寿命到齐后归零；全程 `live ≤ max_particles` |
| S-02 | **持续尾迹（跟随移动）** | rate=100/s，LOCAL 空间，发射器 60 步匀速移动 | 形成连续尾迹；停止后 1s 内消散 |
| S-03 | **碰撞火花（世界空间）** | WORLD 空间，命中点 burst | 火花钉在世界坐标，不随发射器漂移 |
| S-04 | **GPU 火光（仅视觉）** | GPU 后端爆炸 | `gpu_dispatch` 正确产出；CPU checksum **不含**其任何状态；sim 不因它漂移 |
| S-05 | **大量并发发射器** | 50 个 emitter 各 200 上限 | 帧耗时在预算内；按 id 序确定；stats 聚合正确 |
| S-06 | **过载降级** | rate 设到远超容量，DROP_OLDEST | 稳定维持上限粒子；无溢出、无崩溃 |
| S-07 | **暂停/恢复发射** | `set_playing(0/1)` | 停止期无新粒子；已存活照常演化 |
| S-08 | **快照回滚** | 跑 120 帧→快照→再跑→restore→重跑 | 与原始后续帧 checksum 一致（回滚可见） |
| S-09 | **热重载迁移** | 快照→shutdown→init→restore | 状态完整恢复，句柄重建正确 |
| S-10 | **空发射器边界** | max_particles=1, rate 高 | 恰维持 1 粒子，无越界 |

### 4.3 确定性用例（A3，核心）

| # | 用例 | 方法 | 期望 |
|---|------|------|------|
| D-01 | 同 seed 同输出 | fake RNG 固定种子，跑 N 帧，记录每帧 `cpu_checksum` | 两次运行 checksum 序列**逐位一致** |
| D-02 | 迭代顺序稳定 | 以不同（但逻辑等价）create 顺序建 emitter | 因按 id 排序，checksum 不变（id 重映射需归一化，见实现约定） |
| D-03 | 禁地址依赖 | 同一逻辑、不同池基址对齐 | checksum 完全一致 |
| D-04 | 池复用不影响序列 | 触发大量死亡/重生 | 复用槽 index 与 RNG 消耗顺序确定；checksum 可复现 |
| D-05 | **GPU 粒子不参与 checksum** | CPU+GPU 混合，GPU 端注入不同随机 | `cpu_checksum` 完全一致；仅 `extract.content_hash` 可能不同 |
| D-06 | 快照往返确定 | `snapshot→restore→snapshot` | 两次快照逐位一致（docs/10 §8） |
| D-07 | 关闭观测不变 | 开关 stats 读取 | 行为与 checksum 不变（docs/09 §6） |
| D-08 | 跨优化级别 | `-O0` vs `-O2`（关 fast-math/contract） | checksum 一致（L1，docs/10 §6） |

### 4.4 测试替身（fake / stub）

- **`fake_rng_v1`**：可注入固定种子或脚本化序列（`next_u32` 返回预设序列），实现 `next_f32 = next_u32 >> 8 / 2^24`；用于 T-15/16、D-01/04。
- **`counting_alloc_v1`**：包装全局 `operator new/delete` 计数，验证 T-38 热路径零分配、T-37 释放归零。
- **`scripted_rng`**：按调用序号返回预设值，精确断言采样顺序（T-15/16/17）。
- **`fake_transform_source`**：桩，按 id 序喂 `set_transform`（替代 scene）。
- **`fake_render_sink`**：桩，接收 `extract_out`/`gpu_dispatch` 并做只读快照（替代 renderer），验证"不回写"。
- **`assert_sink`**：重定向 `MQ_ASSERT` 为可捕获记录器，使错误用例可断言且不中断进程。

### 4.5 独立单测保障

- 单测 `target_link_libraries` **仅含** `maque::particle` + `maque::math`；CI 脚本检查 `#include` 白名单（禁 `scene/`、`renderer/` 头）。
- RNG / allocator / transform / render 全部走注入替身，**无上游实现依赖**。
- 布局断言就地验证（不依赖 codegen 运行时）。
- 确定性与零分配用例作为**必过门禁**（CI fail-fast）。

---

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：落地 `mq_particle.h` 全部类型/函数/错误码/版本；把 `particle` 的 `ParticleEmitter` 回写 `schema/`；codegen 产出 POD + 反射描述；`static_assert` 全过；`MQ_PARTICLE_ABI_VERSION=1`。**接口不完备不得进入阶段 1**。
2. **阶段 1 · 测试骨架（红）**：实现 §4.1/4.2/4.3 全部用例（实现为空壳 → 全红）；建立 `fake_rng`/`counting_alloc`/`fake_transform_source`/`fake_render_sink`/`assert_sink`。
3. **阶段 2 · 最小实现到绿**：按序实现 —— ① 实例/句柄/池（reserve/create/destroy）→ ② 发射累加器与采样（确定顺序）→ ③ 积分/老化/回收 → ④ extract/gpu_dispatch → ⑤ checksum/snapshot。每块转绿。
4. **阶段 3 · 补边界/错误/确定性**：打磨耗尽策略、悬垂句柄、未 reserve、快照往返；打通 D-01..D-08；重构去重、确保热路径零分配（T-38）与无虚函数。
5. **阶段 4 · 上游 fake 集成 + 预算**：以 fake scene/renderer 驱动 S-01..S-10；跑 CI 优化级别一致性矩阵（D-08）；测帧耗时/内存水位，确认预算达标、无全局状态。

---

## 6. 验收标准（DoD）

- **接口冻结**：`mq_particle.h` 与 §2 一致，`struct_size`/`abi_version` 就位，`static_assert` 全过；被 fake scene/renderer 成功依赖并编译。
- **单测覆盖率 ≥ 85%**（含分支）；§4.1 全绿。
- **确定性用例通过**：D-01..D-08 全绿；**GPU 粒子经证明不参与 `cpu_checksum` 与快照**（D-05/D-06）。
- **预算达标**：
  - 热路径（`sim_step`/`extract`/`gpu_dispatch`）**零堆分配**（T-38，计数=0）；
  - `live_particles ≤ Σ max_particles` **恒成立**（S-01/S-10）；
  - 单发射器 `max_particles` 级帧耗时在预算内（随场景基准，预算记入 docs/09 指标）。
- **红线**：无跨模块实现依赖；零全局可变状态；无异常/锁/热路径分配；ABI 面不含 `std::`/裸指针/vtable 字段。
- **可观测**：`stats` 提供存活/容量/发射/丢弃/发射器数，接入 docs/09。

## 7. 风险

| 风险 | 处理 |
|------|------|
| **GPU 粒子破坏确定性**（读取/回写模拟） | 硬隔离：`cpu_checksum`/`cpu_snapshot` 显式排除 GPU 粒子；本模块**永不读取** GPU 粒子状态；D-05/D-06 门禁 |
| **预算超支**（高 rate / 多发射器） | `max_particles` 硬上限 + `reserve` 预算校验 + 确定耗尽策略；S-06 过载测试；嵌入式裁剪开关（docs/07 §8） |
| **池复用 / 自由链引入不确定性** | FIFO 按 index 拉链，禁地址序；D-03/D-04 守护；池零初始化（docs/10 D5） |
| **浮点漂移（跨优化/跨平台）** | 关 fast-math/contract；只用正确舍入项；D-08 守护；跨平台仅承诺 L1（docs/10 §6） |
| **RNG 采样顺序被重排** | 采样顺序写死并单测（T-15/16/17、D-01）；RNG 经注入，禁系统熵 |
| **热路径意外分配 / 虚函数** | `reserve` 预分配全部池；T-38 计数门禁；vtable 只在模块边界，热循环内联具体实现 |
| **快照含 padding / 未初始化字节** | 按字段序列化序列化 + 零初始化（docs/10 D5）；D-06 逐位往返 |
| **观测影响行为** | stats 冷读、不改遍历与 RNG；D-07 守护（docs/09 §6） |
| **bake 的 desc 与组件布局漂移** | `ParticleEmitter` schema 为唯一真相；codegen + `static_assert` 守门；禁手改生成物 |

## 8. Owner agent

- **角色标识**：`agent-particle`
- **职责边界**：仅负责 `particle` 模块的**接口（`mq_particle.h`）、测试与实现**；不修改其它模块。GPU 粒子的 compute 演化由 `agent-renderer` 消费本模块派发描述实现，不在本模块范围内。
