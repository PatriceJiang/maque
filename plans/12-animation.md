# 12 · animation（属性轨道 + 骨骼动画：确定性采样 / 状态机 / 渲染插值）

> 主参考：`docs/15-动画物理粒子.md`（§2 动画、共同约束、§5 双视图）；
> 上游：`docs/10-确定性快照与回放.md`（D1/D2/D3/D4/D6/D7、checksum）、`docs/06-反射与代码生成.md`（任意反射字段关键帧、type_id）、
> `docs/12-场景与世界.md`（Animator 组件、bake 到 ECS）、`docs/04-虚拟文件系统与多协议.md`（资源预加载）、
> `docs/00`（ADR-0027 世界层、ADR-0005 确定性、ADR-0013 C ABI、ADR-0014 零全局可变状态）。

## 1. 模块概述

- **职责**：在**模拟步内、固定 dt** 下对动画求值并写回组件，作为**权威结果**；渲染侧只对**只读快照**做插值。
  - **属性轨道（property tracks）**：对**任意反射字段**打关键帧（`f32` / `vec3` / `quat` / `color` / 自定义数值），
    目标形如 `{ entity, field_path }`（稳定、可序列化，docs/00 §5）。
  - **骨骼动画（skeletal）**：骨骼**层级变换**（bind local TRS）→ 蒙皮矩阵（skin matrices）。
  - **混合与状态**：layer 权重混合、1D blend tree、状态机（states + transitions），**求值确定**：权重归一化、
    过渡按声明序评估、**无时间依赖的随机混合**（不用 wall clock、不用系统熵）。
  - **求值写回**：`simulate(animator, target, dt)` 按 dt 采样，把属性轨道结果经 `mq_anim_target` 写回组件（确定）。
  - **渲染插值**：`snapshot` 产出 `sim_frame/sim_time + local TRS`；`interpolate(prev, cur, alpha)` 纯函数、**不写模拟**（docs/13 §8）。
  - **资源**：关键帧/骨骼以**文本**（YAML）入库、可 diff，经 **VFS 预加载**（帧内无异步，D7）。
- **所在层**：**世界层**（权威数据在 ECS，docs/15 §5）。
- **上游文档**：docs/15（主）、docs/10、docs/06、docs/12、docs/04、docs/00。
- **一句话**：`clip(文本) → 采样(固定 dt) → 混合/状态机 → 写回组件`；渲染只读快照插值，模拟与呈现解耦且可回放。

## 2. 接口层设计（完备，先冻结）

> 跨模块接口一律 **C ABI + 版本化 vtable**（ADR-0013）。公开类型全是 POD / 句柄 / 函数指针；**禁**跨边界异常 / RTTI / STL。
> 前置依赖（只读）：`core` 的 `mq_type_desc` / `mq_field_desc` / `mq_type_id` / `mq_handle`（docs/06 §4）；`math` 的 `mq_vec3` / `mq_quat` / `mq_mat4` / `mq_color`。

### 2.1 公共头（`include/mq_anim.h`）

```c
/* mq_anim.h —— 纯 C，可跨 .so / WASM */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mq_reflect.h"   /* core: mq_type_desc / mq_field_desc / mq_type_id / mq_handle */
#include "mq_math.h"      /* math: mq_vec3 / mq_quat / mq_mat4 / mq_color  */

#define MQ_ANIM_ABI_VERSION   1u
#define MQ_ANIM_MAX_BONES     256u   /* 快照 POD 容量上限（编译期可裁剪） */
#define MQ_ANIM_MAX_TRACKS    64u
#define MQ_ANIM_MAX_LAYERS    8u

/* ---------- 错误模型：std::expected 风格的 C 映射，无异常越界 ---------- */
typedef enum mq_anim_result {
    MQ_ANIM_OK                = 0,
    MQ_ANIM_E_IO              = 1,   /* VFS 读失败 */
    MQ_ANIM_E_CODEC           = 2,   /* 文本 clip/skeleton 语法非法 */
    MQ_ANIM_E_VERSION         = 3,   /* format_version / abi_version 不兼容 */
    MQ_ANIM_E_ABI             = 4,   /* struct_size/abi_version 校验失败 */
    MQ_ANIM_E_NOT_FOUND       = 5,   /* clip / skeleton / state / param 不存在 */
    MQ_ANIM_E_BAD_BIND        = 6,   /* {entity, field_path} 无法解析到字段/类型 */
    MQ_ANIM_E_TYPE            = 7,   /* 轨道 kind 与字段类型不符（如 vec3 写 f32） */
    MQ_ANIM_E_RANGE           = 8,   /* 数值越界 / 权重非法 / 骨骼数超上限 */
    MQ_ANIM_E_BAD_STATE       = 9,   /* 状态机描述非法（重复名/悬空引用） */
    MQ_ANIM_E_BAD_TRANSITION  = 10,  /* 过渡条件引用未知参数 / 自环非法 */
    MQ_ANIM_E_CYCLE           = 11,  /* 骨骼层级成环 */
    MQ_ANIM_E_STALE           = 12,  /* 句柄 generation 失配（已释放） */
    MQ_ANIM_E_SM_ACTIVE       = 13,  /* 运行中改动状态机结构被拒 */
    MQ_ANIM_E_OOM             = 14,  /* 分配失败 */
    MQ_ANIM_E_UNSUPPORTED     = 15,  /* 未知 interp / 未实现类型种类 */
} mq_anim_result;

typedef struct mq_anim_error {
    mq_anim_result code;
    const char*    msg;     /* 静态或所属 host 生命周期内有效，不转移所有权 */
    uint32_t       line;    /* 文本资源：出错行，0=未知 */
    uint32_t       column;  /* 文本资源：出错列，0=未知 */
} mq_anim_error;

/* ---------- 分配器（与 core/persistence 一致；分配者释放） ---------- */
typedef struct mq_allocator_v1 {
    uint32_t struct_size; uint32_t abi_version;
    void* (*alloc)(void* ud, size_t n, size_t align);
    void* (*realloc)(void* ud, void* p, size_t old_n, size_t n, size_t align);
    void  (*free)(void* ud, void* p, size_t n, size_t align);
    void* ud;
} mq_allocator_v1;

/* ---------- 句柄（{index, generation}，docs/00 §5；跨 .so 稳定） ---------- */
typedef mq_handle mq_anim_clip;      /* 关键帧集（属性轨道 + 可选骨骼轨道） */
typedef mq_handle mq_anim_skeleton;  /* 骨骼层级 + bind pose */
typedef mq_handle mq_anim_animator;  /* 组件运行期实例 */
typedef mq_handle mq_anim_sm;        /* 状态机定义 */

/* ---------- 轨道与播放 ---------- */
typedef enum mq_anim_track_kind {          /* 与字段类型匹配，写回时校验 */
    MQ_ANIM_TRACK_F32   = 0,
    MQ_ANIM_TRACK_VEC3  = 1,
    MQ_ANIM_TRACK_QUAT  = 2,
    MQ_ANIM_TRACK_COLOR = 3,
} mq_anim_track_kind;

typedef enum mq_anim_interp {
    MQ_ANIM_INTERP_STEP   = 0,   /* 阶梯 */
    MQ_ANIM_INTERP_LINEAR = 1,   /* 线性；quat 用 slerp（最短路径，符号确定） */
    MQ_ANIM_INTERP_CUBIC  = 2,   /* Hermite/Catmull-Rom；切线由相邻键确定 */
} mq_anim_interp;

typedef enum mq_anim_loop {
    MQ_ANIM_LOOP_ONCE     = 0,   /* 一次性：到 duration 停在末帧并置 finished */
    MQ_ANIM_LOOP_REPEAT   = 1,   /* 循环：t 对 duration 取模（确定性） */
    MQ_ANIM_LOOP_PINGPONG = 2,   /* 往返 */
} mq_anim_loop;

/* 采样目标：把属性轨道结果写回组件（模拟线程内调用，禁止异步/IO） */
typedef struct mq_anim_target_v1 {
    uint32_t struct_size; uint32_t abi_version;
    void* self;                                 /* scene 句柄 / fake */
    mq_anim_result (*write_f32)  (void* self, mq_handle e, const char* field_path, float v, mq_anim_error*);
    mq_anim_result (*write_vec3) (void* self, mq_handle e, const char* field_path, mq_vec3 v, mq_anim_error*);
    mq_anim_result (*write_quat) (void* self, mq_handle e, const char* field_path, mq_quat q, mq_anim_error*);
    mq_anim_result (*write_color)(void* self, mq_handle e, const char* field_path, mq_color v, mq_anim_error*);
} mq_anim_target_v1;

typedef struct mq_anim_bind {
    mq_handle          entity;               /* 目标实体（属性轨道） */
    const char*        field_path;           /* 反射字段路径，如 "Transform.position" */
    mq_anim_track_kind kind;
} mq_anim_bind;

/* 播放参数（一次性动画用 ONCE，攻击动画靠 finished 判定） */
typedef struct mq_anim_play_opts {
    uint32_t     struct_size; uint32_t abi_version;
    mq_anim_loop loop;       /* 默认 ONCE */
    float        speed;      /* 默认 1.0；负值倒放；0 冻结 */
    float        start_time; /* 默认 0 */
    float        fade_in;    /* 交叉淡入秒数；0=瞬切 */
    uint32_t     layer;      /* 目标层，默认 0 */
} mq_anim_play_opts;

typedef struct mq_anim_load_opts {
    uint32_t struct_size; uint32_t abi_version;
    uint32_t format_version;      /* 0=使用资源内置版本 */
    const mq_allocator_v1* alloc; /* NULL=host 默认 */
} mq_anim_load_opts;

/* 层混合（mask 可选：按骨骼名白/黑名单） */
typedef struct mq_anim_mask {
    const char* const* names;   /* 骨骼名数组 */
    uint32_t           count;
    int                invert;  /* 1=黑名单 */
} mq_anim_mask;

/* ---------- 状态机描述（文本可等价表达；此处为编程构造面） ---------- */
typedef enum mq_anim_cmp {
    MQ_ANIM_CMP_GT = 0, MQ_ANIM_CMP_GE, MQ_ANIM_CMP_LT, MQ_ANIM_CMP_LE, MQ_ANIM_CMP_EQ
} mq_anim_cmp;
typedef enum mq_anim_ptype {
    MQ_ANIM_P_F32 = 0, MQ_ANIM_P_BOOL, MQ_ANIM_P_TRIGGER
} mq_anim_ptype;

typedef struct mq_anim_cond {
    const char*  param;       /* 参数名 */
    mq_anim_ptype type;
    mq_anim_cmp  cmp;         /* F32 用；BOOL/TRIGGER 用 EQ */
    float        threshold;
    int          value;       /* BOOL/TRIGGER 期望值 */
} mq_anim_cond;

typedef struct mq_anim_state_desc {
    const char*         name;
    mq_anim_clip        clip;      /* 该状态播放的 clip（可 0 = 仅混合子节点） */
    mq_anim_loop        loop;
    float               speed;
    const mq_anim_bind* binds; uint32_t bind_count; /* 属性轨道写回目标 */
    const mq_anim_mask* mask;      /* 可选 */
} mq_anim_state_desc;

typedef struct mq_anim_transition_desc {
    const char*         from;      /* NULL / "*" = Any State */
    const char*         to;
    const mq_anim_cond* conds; uint32_t cond_count; /* 全部满足才触发（AND） */
    float               duration;  /* 交叉淡入秒数 */
    int                 has_exit_time; float exit_time; /* 一次性动画结束才转 */
} mq_anim_transition_desc;

typedef struct mq_anim_state_machine_desc {
    uint32_t                       struct_size; uint32_t abi_version;
    const char*                    entry;          /* 初始状态名 */
    const mq_anim_state_desc*      states; uint32_t state_count;
    const mq_anim_transition_desc* transitions; uint32_t transition_count;
} mq_anim_state_machine_desc;

/* ---------- 快照（POD，模拟只读导出；渲染插值输入） ---------- */
typedef struct mq_anim_prop_slot { mq_handle entity; uint32_t field_hash; float v[4]; } mq_anim_prop_slot;

typedef struct mq_anim_snapshot {
    uint32_t  struct_size; uint32_t abi_version;
    mq_handle skeleton;       /* 校验 prev/cur 同源 */
    uint64_t  sim_frame;
    float     sim_time;
    uint32_t  bone_count;
    mq_transform local_bones[MQ_ANIM_MAX_BONES];   /* local TRS（已求值） */
    uint32_t  prop_count;
    mq_anim_prop_slot props[MQ_ANIM_MAX_TRACKS];
} mq_anim_snapshot;

typedef struct mq_anim_pose_out {
    uint32_t     struct_size; uint32_t abi_version;
    uint32_t     bone_count;
    mq_transform local_bones[MQ_ANIM_MAX_BONES];    /* 插值后 local TRS */
    mq_mat4      world_matrices[MQ_ANIM_MAX_BONES]; /* 层级传播后，供蒙皮/渲染 */
} mq_anim_pose_out;

/* ---------- 只读信息（自省 A1） ---------- */
typedef struct mq_anim_clip_info {
    uint32_t struct_size; uint32_t abi_version;
    const char* name; float duration; mq_anim_loop default_loop;
    uint32_t track_count; uint32_t bone_track_count;
} mq_anim_clip_info;

typedef struct mq_anim_skeleton_info {
    uint32_t struct_size; uint32_t abi_version;
    const char* name; uint32_t bone_count;
    /* 解析期已保证：parent[i] < i（拓扑序），无环 */
} mq_anim_skeleton_info;

typedef struct mq_anim_animator_info {
    uint32_t   struct_size; uint32_t abi_version;
    mq_anim_sm sm; const char* current_state;
    float      state_time; int finished;   /* 一次性动画结束标志（场景用） */
    uint32_t   layer_count;
    uint64_t   sim_frame;
} mq_anim_animator_info;

typedef struct mq_anim_opts {
    uint32_t               struct_size; uint32_t abi_version;
    const char*            vfs_uri_prefix;  /* 资源根，默认 "res://" */
    uint32_t               format_version;  /* 写入/接受的最大格式版本；0=默认 */
    const mq_allocator_v1* alloc;
} mq_anim_opts;

/* ---------- 顶层门面（一次性获取；函数指针即完整操作面） ---------- */
typedef struct mq_anim_v1 {
    uint32_t struct_size; uint32_t abi_version;

    /* lifecycle：每实例一个 host，零全局可变状态（ADR-0014） */
    mq_anim_result (*create)(const mq_allocator_v1* alloc, const mq_anim_opts* opts, void** out_host, mq_anim_error*);
    void           (*destroy)(void* host);

    /* asset 预加载（经 VFS；禁止在模拟帧内调用，D7） */
    mq_anim_result (*load_clip)(void* host, const char* uri, const mq_anim_load_opts*, mq_anim_clip* out, mq_anim_error*);
    mq_anim_result (*load_skeleton)(void* host, const char* uri, const mq_anim_load_opts*, mq_anim_skeleton* out, mq_anim_error*);
    void           (*release_clip)(void* host, mq_anim_clip);
    void           (*release_skeleton)(void* host, mq_anim_skeleton);
    const mq_anim_clip_info*     (*clip_info)(void* host, mq_anim_clip);
    const mq_anim_skeleton_info* (*skeleton_info)(void* host, mq_anim_skeleton);

    /* animator 实例（Animator 组件的运行期状态；句柄存在 host 内） */
    mq_anim_result (*animator_create)(void* host, mq_anim_skeleton skel, mq_anim_animator* out, mq_anim_error*);
    void           (*animator_destroy)(void* host, mq_anim_animator);

    /* 播放控制（属性 + 骨骼统一入口） */
    mq_anim_result (*play)(void* host, mq_anim_animator, mq_anim_clip, const mq_anim_play_opts*, const mq_anim_bind* binds, uint32_t bind_count, mq_anim_error*);
    mq_anim_result (*stop)(void* host, mq_anim_animator, mq_anim_error*);
    mq_anim_result (*set_speed)(void* host, mq_anim_animator, float speed, mq_anim_error*);
    mq_anim_result (*seek)(void* host, mq_anim_animator, float t, mq_anim_error*);
    mq_anim_result (*cross_fade)(void* host, mq_anim_animator, mq_anim_clip, float duration, const mq_anim_bind* binds, uint32_t bind_count, mq_anim_error*);
    mq_anim_result (*set_layer_weight)(void* host, mq_anim_animator, uint32_t layer, float weight, const mq_anim_mask* mask, mq_anim_error*);

    /* 状态机（结构冻结后运行；运行中改动结构 → MQ_ANIM_E_SM_ACTIVE） */
    mq_anim_result (*sm_create)(void* host, const mq_anim_state_machine_desc*, mq_anim_sm* out, mq_anim_error*);
    void           (*sm_destroy)(void* host, mq_anim_sm);
    mq_anim_result (*animator_set_sm)(void* host, mq_anim_animator, mq_anim_sm, mq_anim_error*);
    mq_anim_result (*set_param_f32)(void* host, mq_anim_animator, const char* name, float v, mq_anim_error*);
    mq_anim_result (*set_param_bool)(void* host, mq_anim_animator, const char* name, int v, mq_anim_error*);
    mq_anim_result (*trigger)(void* host, mq_anim_animator, const char* name, mq_anim_error*);   /* 自动复位 */

    /* 模拟采样（固定 dt，唯一会推进时间并写回的入口） */
    mq_anim_result (*simulate)(void* host, mq_anim_animator, const mq_anim_target_v1* target, float dt, mq_anim_error*);

    /* 只读快照 + 渲染插值（纯函数；不改任何模拟状态） */
    mq_anim_result (*snapshot)(void* host, mq_anim_animator, mq_anim_snapshot* out, mq_anim_error*);
    mq_anim_result (*interpolate)(void* host, const mq_anim_snapshot* prev, const mq_anim_snapshot* cur, float alpha, mq_anim_pose_out* out, mq_anim_error*);
    mq_anim_result (*skin_matrices)(void* host, const mq_anim_pose_out*, const mq_anim_skeleton_info*, mq_mat4* out, uint32_t* inout_count, mq_anim_error*);

    /* 自省 */
    mq_anim_result (*info)(void* host, mq_anim_animator, mq_anim_animator_info* out, mq_anim_error*);
} mq_anim_v1;
```

### 2.2 类型清单

| 类型 | 形态 | 说明 |
|------|------|------|
| `mq_anim_result` / `mq_anim_error` | POD | 错误码 + 静态消息 + 行列 |
| `mq_allocator_v1` | POD vtable | 分配者释放契约 |
| `mq_anim_clip` / `mq_anim_skeleton` / `mq_anim_animator` / `mq_anim_sm` | 句柄 | `mq_handle{index,generation}`，失配→`MQ_ANIM_E_STALE` |
| `mq_anim_bind` | POD | `{entity, field_path, kind}`，属性轨道写回目标 |
| `mq_anim_track_kind` / `mq_anim_interp` / `mq_anim_loop` | enum | 轨道类型 / 插值 / 循环 |
| `mq_anim_target_v1` | POD vtable | 写回组件唯一接缝（场景 facade / fake） |
| `mq_anim_play_opts` / `mq_anim_load_opts` / `mq_anim_mask` | POD | 播放 / 加载 / 层遮罩 |
| `mq_anim_*_desc` | POD | 状态机编程构造面 |
| `mq_anim_snapshot` / `mq_anim_pose_out` | POD（定长） | 只读快照 / 插值输出，容量 `MAX_BONES`/`MAX_TRACKS` |
| `mq_anim_*_info` | POD | 自省信息（A1） |
| `mq_anim_v1` | POD vtable | 顶层门面 |

### 2.3 操作面（全部函数语义）

- `create` / `destroy`：建立/销毁实例 host（承载 clip 表、animator 池）；**零全局可变状态**。
- `load_clip` / `load_skeleton`：经 VFS 读文本并解析为运行时结构（预加载，D7）；同一 `uri` 重复加载返回同一句柄（引用计数）。
- `release_*`：减少引用计数，归零释放；句柄 generation++。
- `animator_create` / `destroy`：为某 skeleton 建立求值实例（各层当前 clip、参数表、状态机绑定）。
- `play`：设置目标层当前 clip、loop、speed、fade_in；`binds` 为属性轨道写回目标（不传则只算骨骼）。
- `cross_fade`：在 `duration` 秒内把目标层从当前 clip 线性淡入新 clip（权重按 sim_time 推进，**确定**）。
- `set_layer_weight`：设置层权重与遮罩；最终权重 = `layer_weight × fade_weight`，所有层**归一到和=1**（含空层兜底）。
- `sm_create` / `sm_destroy`：校验状态机描述（重复名、悬空引用、自环 → `MQ_ANIM_E_BAD_STATE`）；成功后可被多个 animator 引用。
- `animator_set_sm`：绑定状态机；初始状态取 `entry`。
- `set_param_f32` / `set_param_bool` / `trigger`：设置参数；`trigger` 在被某次过渡消费后**自动复位**。参数表按名稳定排序（D4）。
- `simulate`：**核心**。`sim_time += dt`（仅模拟时间，D3）；① 处理状态机过渡（按 `transitions` 声明序，全部条件 AND）；② 对每层采样 clip（键查找、插值、循环取模）；③ 混合权重归一化、逐骨/逐属性加权（quat 用加权 slerp，符号确定）；④ 属性轨道经 `target` 写回；⑤ `sim_frame++`。`dt<=0` 视为无推进（仍求值一次，用于初始帧）。
- `snapshot`：把当前 local TRS 与属性值拷入 POD（**只读**，不推进时间、不改状态）。
- `interpolate`：在 `prev`/`cur` 两快照间按 `alpha` 插值（两快照骨架须同源，否则 `MQ_ANIM_E_TYPE`）；quat 走最短路径 slerp；随后层级传播得 `world_matrices`；**纯函数**。
- `skin_matrices`：`world_matrix[b] × inverse_bind[b]` → 蒙皮矩阵（bind pose 来自 skeleton）。
- `info`：返回当前状态名、`state_time`、`finished`、层数、`sim_frame`（供控制平面/脚本/测试读取）。

### 2.4 文本资源规范（开发期唯一真相，ADR-0021）

`.skel`（骨骼）与 `.anim`（clip）均为 YAML、可 diff、AI 可读；二进制为烘焙派生。

```yaml
# hero.skel
skeleton: Hero
format_version: 1
bones:                       # parent 必须 < 自身索引（拓扑序，禁止成环）
  - { name: root, parent: -1, bind: { p:[0,0,0], r:[0,0,0,1], s:[1,1,1] } }
  - { name: Hips, parent: 0,  bind: { p:[0,1,0], r:[0,0,0,1], s:[1,1,1] } }
  - { name: ArmL, parent: 1,  bind: { p:[0.3,0,0], r:[0,0,0,1], s:[1,1,1] } }
```

```yaml
# hero_run.anim
clip: Run
format_version: 1
duration: 1.0
loop: repeat
tracks:                                   # 属性轨道：任意反射字段
  - target: { entity: "0x0001_0002", field: "Transform.position" }
    kind: vec3
    interp: linear
    keys: [ { t: 0.0, v: [0,0,0] }, { t: 0.5, v: [0,0,0.2] }, { t: 1.0, v: [0,0,0] } ]
bone_tracks:                              # 骨骼局部 TRS
  - { bone: Hips, interp: linear, keys: [ { t: 0.0, r: [0,0,0,1] }, { t: 1.0, r: [0,0.2,0,1] } ] }
```

- 关键帧 `t` 必须**升序、去重、落在 `[0, duration]`**；越界/乱序 → `MQ_ANIM_E_CODEC`。
- 属性**只写被显式声明的轨道**；未声明字段保持原值（不做隐式归零）。
- 文本内**禁止**时间戳 / 随机种子 / 绝对路径（确定性 + 可 diff）。

### 2.5 版本化规则（前向兼容）

1. **ABI 版本化**：所有 vtable 首部 `struct_size + abi_version`。调用方填编译期 `struct_size`；被调方校验 `>=` 最小需求，不足 → `MQ_ANIM_E_ABI`；新增回调只允许**追加到尾部**并抬升 `abi_version`。
2. **资源格式版本化**：`.skel`/`.anim` 的 `format_version` 超过 host `opts.format_version` → `MQ_ANIM_E_VERSION`；新增键类型走追加式。
3. **骨骼兼容**：clip 引用的骨骼名在 skeleton 中缺失 → `MQ_ANIM_E_BAD_BIND`（加载期即报，不留到运行期）。

### 2.6 内存所有权

- **分配者释放**：`load_*` 内部结构用 host allocator 分配，`release_*` 释放；host 用 `opts.alloc`（NULL 用默认）建立。
- `snapshot` / `pose_out` / `info` 均为**调用方栈/预分配 POD**，模块只写不分配。
- 跨 `.so` 仅传 POD/句柄，**禁**跨边界 `new`/`delete`/STL。

### 2.7 线程 / 确定性

- **仅模拟线程**调用 `simulate` / `set_param` / `play`（D6）；`load_*` 在加载阶段（帧外）执行（D7）。
- 确定性：不读墙钟（D3）、不用系统熵（D2）；参数表与层/过渡按**声明序/名序**处理（D4）；浮点关 `-ffast-math`（D8）；quat 插值走**确定的最短路径**（先按 dot 符号翻转，再做加权）。
- `interpolate` / `skin_matrices` 为纯函数，可跨线程对**只读快照**调用。

## 3. 依赖与边界

- **可依赖**：`core` 反射（解析 `field_path` → 描述符/偏移/类型；`mq_handle`）、`math`（`vec3/quat/mat4/color` 与确定性插值）、`VFS`（`res://` 预加载文本）、`persistence`（可选：文本 YAML 解析复用其 codec visitor）。
- **禁止**：
  - 不得依赖上层（`renderer` / `script` / `control plane`）——渲染插值只消费本模块导出的**只读快照**；
  - `simulate` 热路径**不得**过反射读写字段（反射仅供绑定解析，写回走 `mq_anim_target` 直接偏移，docs/06 §5）；
  - **不得**持有全局可变状态；句柄表挂在 host 实例（ADR-0014）；
  - **不得**在 `simulate` 内做 IO/阻塞（D7）；不得读墙钟/系统熵（D2/D3）；
  - 不得暴露裸指针（对外全是句柄 + POD）。
- **接缝**：
  - 上游 `scene`：提供 `mq_anim_target_v1`（按 `{entity, field_path}` 写字段）与实体句柄；`Animator` 组件本身是反射组件（docs/12）。
  - 下游 `renderer`：读 `snapshot`（模拟线程产出）+ `interpolate`（渲染线程按 alpha）→ `world_matrices` 供 GPU 蒙皮（docs/13 §8）。
  - 上游 `asset pipeline`：把文本 `.anim`/`.skel` bake 成二进制（可复用 persistence codec），本模块只消费。

## 4. 测试计划（从接口推导，先行）

> 单测只依赖本模块 + `math` + `core` **fake 描述符**，用 **fake clock**（传入 dt 序列）与 **fake storage**（内存 VFS）驱动；**不**链接 scene/renderer/asset。

### 4.1 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| T01 | 加载最小 clip | 内存 `.anim`（1 条 f32 轨道，2 键） | `MQ_ANIM_OK`，`clip_info.duration` 正确 | 正常 |
| T02 | 加载骨骼拓扑 | `.skel` root→Hips→ArmL | `bone_count=3`，拓扑序 `parent[i]<i` | 正常 |
| T03 | 键查找 t=0 | 键 `{0:A,1:B}`，t=0 | 值 = A（精确） | 边界 |
| T04 | 键查找 t=duration | 同上，t=1.0 | 值 = B（精确） | 边界 |
| T05 | 线性插值 t=0.5 | `{0:0,1:10}` | 值 = 5（±eps） | 正常 |
| T06 | 越界 t<0 | `seek(-0.5)`（非循环） | clamp 到 t=0，取首键 | 边界 |
| T07 | 越界 t>duration | `seek(2.0)`（非循环） | clamp 到 t=duration，取末键 | 边界 |
| T08 | 循环取模 | loop=REPEAT，t=1.25，dur=1.0 | 等价 t=0.25 | 正常 |
| T09 | pingpong 往返 | loop=PINGPONG，dur=1，t=1.25 | 等价 t=0.75；符号确定 | 正常 |
| T10 | step 插值 | interp=STEP，t=0.9 | 保持左键值 | 正常 |
| T11 | cubic 插值 | 3 键 Catmull-Rom，t=0.5 | 落于两端之间且对称（确定值） | 正常 |
| T12 | quat 最短路径 | 两键 quat（dot<0） | 沿短弧 slerp，无翻转跳变 | 边界 |
| T13 | 一次性播放结束 | loop=ONCE，dt 推过 duration | 停末帧，`info.finished==1` | 正常 |
| T14 | 一次性不循环 | loop=ONCE，继续推 dt | `state_time` 不再增，值不变 | 边界 |
| T15 | 倒放 | speed=-1 | 从 duration 向 0 推进 | 正常 |
| T16 | speed=0 冻结 | speed=0，推 dt | 值不变，`sim_time` 不增 | 边界 |
| T17 | 双轨混合权重=1 | 层权重 `{0.3,0.7}` | 结果 = `0.3·A + 0.7·B`（±eps） | 正常 |
| T18 | 权重和≠1 归一 | 层权重 `{2,2}` | 归一到 `{0.5,0.5}` | 边界 |
| T19 | 权重和=0 兜底 | 全 0 | 取首层结果（确定的兜底策略） | 边界 |
| T20 | cross_fade | t=0 全 A，duration=0.4 | t=0.2 各 0.5；t≥0.4 全 B | 正常 |
| T21 | 属性写回 | 轨道 `Transform.position`(vec3) | `target.write_vec3` 收到正确 entity/path/值 | 正常 |
| T22 | 类型不符 | 轨道 kind=f32 但字段为 vec3 | `MQ_ANIM_E_TYPE` | 错误 |
| T23 | 绑定不存在 | `field_path="No.Such"` | `MQ_ANIM_E_BAD_BIND` | 错误 |
| T24 | 未声明字段保持原值 | 只声明 pos，不声明 rot | 写回后 rot 不变 | 正常 |
| T25 | 状态机 entry | sm 有 Idle/Run，entry=Idle | 绑定后 `current_state=="Idle"` | 正常 |
| T26 | 过渡条件触发 | `speed>0.5` → Run | `set_param_f32(speed,0.8)` 后迁移 Run | 正常 |
| T27 | 过渡按声明序 | 两条均满足的过渡 | 取声明在前的目标 | 确定性 |
| T28 | trigger 消费复位 | trigger 触发一次过渡后再 `simulate` | 同 trigger 不重复触发 | 正常 |
| T29 | Any State 过渡 | `from="*"` + 条件 | 任一状态都满足时触发 | 正常 |
| T30 | exit_time 过渡 | 一次性攻击动画，`has_exit_time=1` | 仅在 `finished` 后转向下一状态 | 正常 |
| T31 | 骨骼层级变换 | Hips 父 root：root 平移 (0,1,0)、Hips 局部旋转 | `world_matrices` = 父×子（±eps） | 正常 |
| T32 | 蒙皮矩阵 | `pose_out` + bind pose | `world×inverse_bind` 正确 | 正常 |
| T33 | 快照只读性 | 调 `snapshot` 后 `info.sim_frame` | 不变（未推进时间） | 边界 |
| T34 | 插值 alpha=0/1 | `interpolate(prev,cur,0/1)` | 分别等于 prev/cur（±eps） | 边界 |
| T35 | 快照跨骨架 | prev/cur 不同 skeleton | `MQ_ANIM_E_TYPE` | 错误 |
| T36 | 骨骼成环 | `.skel` parent 指向自身/后向 | `MQ_ANIM_E_CYCLE` | 错误 |
| T37 | 骨骼超上限 | bone_count>MAX_BONES | `MQ_ANIM_E_RANGE` | 错误 |
| T38 | 关键帧乱序 | keys t=[0,0.5,0.3] | `MQ_ANIM_E_CODEC` | 错误 |
| T39 | 未知 interp | `interp: cubic2` | `MQ_ANIM_E_UNSUPPORTED` | 错误 |
| T40 | 版本过高 | `format_version: 999` | `MQ_ANIM_E_VERSION` | 版本兼容 |
| T41 | 空 clip | 无轨道、duration=0 | `MQ_ANIM_OK`，采样恒为空操作 | 边界 |
| T42 | 分配平衡 | 计数 allocator 跑 load/release 循环 | alloc==free，无泄漏 | 内存所有权 |
| T43 | 句柄失配 | 释放后使用旧 clip 句柄 | `MQ_ANIM_E_STALE` | 错误 |
| T44 | 未知参数 | `set_param_f32("nope",1)` | `MQ_ANIM_E_NOT_FOUND` | 错误 |

### 4.2 游戏场景用例

- **S1 角色 idle→run→jump 状态迁移**：加载 `hero.skel` + `Idle`/`Run`/`Jump` clip，
  构建状态机：`Any→Run`（`speed>0.1`）、`Run→Idle`（`speed<=0.1`）、`Any→Jump`（trigger `jump`）、
  `Jump→Idle`（`has_exit_time=1`）。
  1. 初始 `current_state=="Idle"`；
  2. `set_param_f32("speed",0.8)` → `simulate(dt=1/60)` → 迁移 Run，`info.current_state=="Run"`；
  3. `trigger("jump")` → `simulate` → 迁移 Jump（一次性 clip，`loop=ONCE`）；
  4. 持续 `simulate` 至 Jump 结束（约 duration 秒）→ `finished==1` → 迁移回 Idle；
  5. 全程断言 `sim_frame` 单调 +1、属性写回经 fake target 正确。
- **S2 一次性攻击动画**：`Attack` clip（`loop=ONCE`），在 Idle 状态 `play(Attack)`；
  断言：攻击期间 `finished==0`；到末帧 `finished==1`；`state_time` 被 clamp 到 duration；末帧值 = 末键值。
- **S3 渲染帧率 ≠ 模拟帧率**：模拟固定 `dt=1/60` 跑 3 帧得到 `s0→s1→s2` 快照；
  渲染侧 `interpolate(s1,s2,alpha=0.5)`，断言输出为两帧中值；**并断言再次 `snapshot` 与 `s2` 完全一致**（回写未被污染）。
- **S4 属性轨道驱动 HUD**：对自定义反射字段 `Hud.alpha`(f32) 与 `Transform.color`(color) 打关键帧，
  断言经 fake target 写回、类型匹配、越界 clamp 正确。
- **S5 混合待机/行走**：layer0=Idle、layer1=Run，`set_layer_weight(1,0.5)`，
  断言骨姿态 = 两者 50/50（±eps），权重和=1。

### 4.3 确定性用例

- **D1 同 dt 序列同输出**：构造相同初始状态 + 相同 `dt` 序列（如 120 帧 `1/60`），
  两次独立运行 `simulate` + `snapshot`，断言每帧快照 `memcmp` 逐位一致。
- **D2 回放一致（checksum）**：对每帧快照计算 checksum（对 `local_bones`/`props` 逐字段哈希，非含 padding 裸 memcmp），
  两次运行 checksum 序列完全相同；输出**首个分歧帧**索引（应为 -1）。
- **D3 变量 dt 不影响确定**：若调用方喂相同 dt 序列，结果确定；**禁止**用变化 dt 之外的随机源（无 wall clock）。
- **D4 过渡确定性**：多条过渡同时满足时，结果与声明序绑定（T27），重复 100 次结果一致。
- **D5 quat 符号确定**：对 `dot(q0,q1)<0` 的相邻键，插值路径与平台无关（先翻转后 slerp），checksum 稳定。

### 4.4 测试替身

- **fake clock**：不引入真实时间；测试显式传入 `dt` 序列（`1/60` 等），验证固定步长语义。
- **fake storage / VFS**：`mem://` 内存字典，键为 `uri`、值为 `.anim`/`.skel` 文本，供 `load_*` 独立单测。
- **fake target**：实现 `mq_anim_target_v1`，把写回值存入 `map<{entity,field_hash}, float[4]>`，供断言；不链接 scene。
- **fake 描述符**：手工构造 `mq_type_desc`（`Transform.position`(vec3)、`Hud.alpha`(f32)、`Transform.color`(color)），不依赖 codegen 产物。

### 4.5 独立单测保障

- 用例只 `#include "mq_anim.h"` + `"mq_math.h"` + `"mq_reflect.h"`（fake 描述符）；
- 资源全部走 fake storage；写回全部走 fake target；**不**链接 scene/renderer/asset/persistence；
- CI 中断言链接符号不含其它模块实现；确定性用例在 Debug/Release 两种构建下都跑。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：落地 `mq_anim.h`（本节全部类型/vtable/错误码）+ `MQ_ANIM_ABI_VERSION=1`；冻结文本 `.anim`/`.skel` 规范与快照 POD 布局。评审通过才进入实现。
2. **阶段 1 · 测试骨架（红）**：先写 T01–T44、S1–S5、D1–D5 的断言（fake storage + fake target + fake clock），全部失败。
3. **阶段 2 · 最小实现到绿**：实现文本解析（`.anim`/`.skel`）、键查找与线性/step 插值、单层骨骼采样、`simulate` 写回、`snapshot`；打通 T01–T05、T13、T21、T24、T31、T41。
4. **阶段 3 · 混合与边界**：实现多层级混合 + 归一化、`cross_fade`、quat slerp、循环/pingpong、越界 clamp、cubic；打通 T06–T12、T15–T20、T33–T35。
5. **阶段 4 · 状态机与错误面**：实现状态机（entry/条件/Any State/exit_time/trigger 复位）、全部错误码路径（T22/T23/T36–T40/T43/T44）与内存所有权（T42）。
6. **阶段 5 · 确定性与场景**：接通确定性 checksum 用例（D1–D5）与游戏场景 S1–S5；引入层级传播 + 蒙皮矩阵（T31/T32）；重构去重。
7. **阶段 6 · 集成与预算**：对接 `scene` 的 fake target 与真实 VFS 文本加载；接控制平面 `info` 只读面；跑性能/内存/泄漏预算。

## 6. 验收标准（DoD）

- 接口 `mq_anim.h` 冻结且被 fake storage / fake target / 上游 fake 依赖通过；
- T01–T44、S1–S5、D1–D5 全绿，单测覆盖率 ≥ 80%；
- **确定性成立**：同 dt 序列同输出、checksum 序列一致、失步可定位到首个分歧帧；quat 符号路径确定；
- **只读插值成立**：`snapshot`/`interpolate`/`skin_matrices` 不改任何模拟状态（S3/T33 守护）；
- 内存所有权无泄漏（计数 allocator 平衡）；无跨模块实现依赖；无全局可变状态；
- 性能预算：单 animator 单层采样 + 256 骨层级传播 < 0.1 ms/帧（Release，桌面）；快照导出为**纯 POD 拷贝**、无分配。

## 7. 风险

| 风险 | 处理 |
|------|------|
| 浮点混合逐位漂移 | 关 `-ffast-math`（D8）；固定加权顺序；D1/D2 checksum 常驻 CI |
| quat 插值符号跳变 | 先按 `dot` 符号翻转再 slerp，符号确定（T12/D5） |
| 循环/一次性边界错误 | t clamp 与取模显式用例（T03/T04/T06–T09/T13/T14） |
| 混合权重不归一 | 每帧强制归一 + 全零兜底（T17–T19/S5） |
| 状态机过渡顺序不定 | 按声明序评估、trigger 消费即复位（T27/T28/D4） |
| 渲染插值污染模拟 | 插值只读快照为纯函数；S3/T33 防回归 |
| 骨骼层级成环/超限 | 解析期校验 `parent[i]<i` + 上限（T36/T37） |
| 文本资源漂移/不可 diff | 文本入库、禁时间戳/随机/绝对路径；格式版本化（T38–T40） |
| 模拟帧内加载卡帧 | 帧内无异步（D7）；VFS 预加载，`load_*` 仅帧外调用 |
| 快照过大/分配抖动 | 定长 POD、纯拷贝、容量编译期裁剪（T41 空 clip 边界） |

## 8. Owner agent

- **角色标识**：`agent-animation`
- **职责边界**：仅负责 `animation` 模块的接口（`mq_anim.h`）、测试与实现（文本 clip/skeleton 解析、采样、混合、状态机、蒙皮矩阵、只读快照插值）；
  不修改 `core`/`math`/`VFS`/`scene`/`renderer` 等其它模块；与上游通过冻结的 C ABI（`mq_anim_target_v1`、反射描述符、VFS `res://`）交互。
