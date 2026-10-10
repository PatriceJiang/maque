# 15 · renderer（WebGPU / T1 pass 层）

> 复制自 `_template.md`。接口与测试章节先于实现冻结。
> 上游主参考：`docs/13-渲染（WebGPU）.md`；分层与混合来源：`docs/12-场景与世界.md §5`；
> `Renderer` domain 与 `capture_frame`：`docs/11-控制平面与AI接口.md §3/§6`；
> 边界规则：`docs/02`；总纲：`docs/00`（ADR-0006/0025/0029/0013/0014/0005/0018）。

## 1. 模块概述

- **职责**：实现 maque 的**唯一渲染后端 = WebGPU**（ADR-0006）。`renderer/gfx` 把引擎概念（mesh / 材质 / 变换 / layer / 相机）映射为 WebGPU 资源与管线；它**只消费可序列化的绘制描述 DrawList**，**不认识** scene 的内部存储（docs/13 §1、docs/12 §8）。对外提供：设备/交换链初始化、GPU 资源创建（纹理/网格/着色器/管线，经 VFS 加载、WGSL 文本入库）、**T1 轻量声明式 pass 层**（pass 列表 + 声明读写依赖，不重排、不别名、支持多目标/离屏）、**离屏渲染 + readback** 以支撑 `Renderer.capture_frame`（AI 预览 / 图像回归）。
- **所在层**：呈现层（presentation）。是数据流的**终端消费者**：节点树/ECS →提取(cull/batch)→ DrawList → pass 层 → WebGPU。**渲染绝不回写模拟**（单向数据流，ADR-0005）。
- **上游文档**：`docs/13`（主）、`docs/12 §5`（2D 图层/3D 世界/UI 分层）、`docs/11`（Renderer domain / capture_frame）、`docs/00`（ADR-0006 WebGPU、ADR-0025 T1 pass 层、ADR-0029 不降级、ADR-0013 C ABI、ADR-0014 无全局可变状态、ADR-0005 确定性、ADR-0018 单线程）。
- **一句话**：把"可序列化的绘制描述"经一层轻量 pass 声明，翻译成 WebGPU 命令，并能在无 GUI 下离屏截帧供 AI 与 CI 检视——且对模拟只读。

## 2. 接口层设计（完备，先冻结）

> 一律 **C ABI + 版本化 vtable**（docs/02 R1–R8）。接口冻结前不得进入实现。
> 命名空间（稳定字符串 ID）：`"maque.render.v1"`（公共操作面）、`"maque.gpu_backend.v1"`（**内部后端 seam**，真实实现走 WebGPU/wgpu，测试走 fake）。

### 2.1 公共头 `mq_render.h`（纯 C，示意但可编译的完整面）

```c
/* mq_render.h —— 纯 C，extern "C"；边界上无 C++ / 无异常 / 无 STL / 无裸指针内部结构 */
#include <stddef.h>
#include <stdint.h>

#define MQ_RENDER_ABI_VERSION    1u
#define MQ_DRAWLIST_VERSION      1u
#define MQ_GPU_BACKEND_ABI_VERSION 1u

/* ---- 结果码：所有入口 noexcept，用返回码表达（docs/02 R2） ---- */
typedef enum mq_render_result {
    MQ_RENDER_OK               =   0,
    MQ_RENDER_ERR_NO_ADAPTER   =  -1,   /* 无可用 Vulkan/Metal/D3D12 后端（ADR-0029，不降级） */
    MQ_RENDER_ERR_DEVICE_LOST  =  -2,
    MQ_RENDER_ERR_OUT_OF_MEMORY=  -3,   /* 显存/内存不足 */
    MQ_RENDER_ERR_INVALID_ARG  =  -4,
    MQ_RENDER_ERR_NOT_FOUND    =  -5,   /* 资源句柄过期 / 不存在 */
    MQ_RENDER_ERR_ABI          =  -6,   /* abi_version 主版本不匹配 */
    MQ_RENDER_ERR_UNSUPPORTED  =  -7,   /* 设备/后端不支持该能力 */
    MQ_RENDER_ERR_PASS_ORDER   =  -8,   /* pass 依赖违反（读未声明 / 乱序） */
    MQ_RENDER_ERR_PASS_CYCLE   =  -9,   /* pass 读写依赖成环 */
    MQ_RENDER_ERR_SHADER       = -10,   /* WGSL 编译/校验失败 */
    MQ_RENDER_ERR_SURFACE_LOST = -11,   /* 交换链表面失效，需重建 */
    MQ_RENDER_ERR_READBACK     = -12,   /* 回读失败 */
    MQ_RENDER_ERR_VFS          = -13,   /* 资产经 VFS 加载失败（docs/04/08） */
    MQ_RENDER_ERR_NO_SPACE     = -14,   /* 输出缓冲区不足，out_size 给出所需 */
    MQ_RENDER_ERR_PENDING      = -15,   /* 非错误：异步未完成（轮询语义） */
} mq_render_result;

/* ---- 句柄：不透明 + 世代，禁止跨实例（docs/02 R4） ---- */
typedef struct mq_render      mq_render;       /* 渲染实例上下文（零全局状态，ADR-0014） */
typedef struct mq_swapchain   mq_swapchain;    /* 交换链 */
typedef struct mq_frame       mq_frame;        /* 一帧录制上下文 */
typedef struct mq_pass        mq_pass;         /* 已声明的 pass（T1 声明式） */
typedef struct mq_gpu_task    mq_gpu_task;     /* 异步 GPU 任务（加载/回读） */

/* GPU 资源：可序列化 ID（DrawList 跨边界引用资源必需），非指针 */
typedef struct mq_resource_id { uint32_t index; uint32_t generation; } mq_resource_id;
#define MQ_RESOURCE_NULL ((mq_resource_id){0u, 0u})

typedef enum mq_resource_kind {
    MQ_RES_BUFFER=1, MQ_RES_TEXTURE=2, MQ_RES_SAMPLER=3,
    MQ_RES_SHADER=4, MQ_RES_PIPELINE=5, MQ_RES_BIND_GROUP=6,
} mq_resource_kind;

/* 管线状态哈希：同一 desc → 同一 key，用于缓存与 DrawList 引用（docs/13 §5） */
typedef uint64_t mq_pipeline_key;

/* ---- 分配器：宿主注入，分配者释放（docs/02 R5） ---- */
typedef struct mq_allocator {
    uint32_t struct_size;
    void* (*alloc)(void* self, size_t size, size_t align);
    void  (*free) (void* self, void* ptr);
    void* self;
} mq_allocator;

/* =========================================================
 *  DrawList —— 可序列化的绘制描述（docs/13 §2；跨边界只传它）
 *  固定布局 POD；static_assert(sizeof==expected)
 * ========================================================= */
typedef enum mq_draw_flags {
    MQ_DRAW_NONE       = 0,
    MQ_DRAW_INDEXED    = 1u << 0,   /* 用 index_data */
    MQ_DRAW_INSTANCED  = 1u << 1,   /* instance_count > 1 */
    MQ_DRAW_WIREFRAME  = 1u << 2,
    MQ_DRAW_ALPHA      = 1u << 3,   /* 透明批，参与排序 */
} mq_draw_flags;

typedef struct mq_draw_item {
    mq_pipeline_key pipeline;     /* 状态哈希，指向已缓存管线 */
    mq_resource_id  mesh;         /* 网格/顶点缓冲资源；可为 NULL（用 vertex_data） */
    mq_resource_id  material;     /* 材质绑定组（纹理/采样器/uniform） */
    float           transform[16];/* 列主序 mat4（docs/06，与 math 一致） */
    uint64_t        sort_key;     /* 稳定排序键（层内确定性排序） */
    uint32_t        layer;        /* 2D/3D/UI 分层（docs/12 §5） */
    uint32_t        viewport;     /* 相机/视口索引 */
    uint32_t        flags;        /* mq_draw_flags */
    uint32_t        index_count;
    uint32_t        instance_count;
    uint32_t        first_index;  /* index_data 内偏移 */
    int32_t         vertex_offset;
    uint32_t        _pad;
} mq_draw_item;                   /* 固定大小：static_assert(sizeof(mq_draw_item)==96) */

typedef struct mq_drawlist {
    uint32_t          struct_size;    /* = sizeof(mq_drawlist) */
    uint32_t          version;        /* = MQ_DRAWLIST_VERSION */
    uint32_t          layer;          /* 整表所属层（批处理分组） */
    uint32_t          item_count;
    mq_draw_item*     items;          /* 由 drawlist 分配器拥有 */
    /* 可选内联几何（粒子/UI 批等不在 VFS 中的动态数据） */
    const void*       vertex_data;    uint64_t vertex_bytes;
    const void*       index_data;     uint64_t index_bytes;
} mq_drawlist;

/* DrawList 构建 / 序列化（提取阶段产出，pack 后即可跨进程/dump，A1） */
typedef struct mq_drawlist_builder mq_drawlist_builder;
typedef struct mq_drawlist_pack_header {
    uint32_t magic;        /* 'MQDL' */
    uint32_t version;      /* MQ_DRAWLIST_VERSION */
    uint32_t layer;
    uint32_t item_count;
    uint64_t vertex_bytes;
    uint64_t index_bytes;
    uint32_t payload_crc32; /* 完整 payload 校验（可选，0=跳过） */
    uint32_t _pad;
} mq_drawlist_pack_header;

/* ---- 交换链 / 表面 ---- */
typedef struct mq_native_window { void* handle; uintptr_t display; } mq_native_window;
typedef struct mq_swapchain_desc {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t format;          /* mq_texture_format */
    uint32_t present_mode;    /* mq_present_mode */
    uint32_t sample_count;    /* 1 / 4 / 8 */
    uint32_t _pad;
} mq_swapchain_desc;

typedef enum mq_texture_format {
    MQ_FMT_BGRA8_UNORM=1, MQ_FMT_RGBA8_UNORM=2, MQ_FMT_RGBA16_FLOAT=3, MQ_FMT_DEPTH32_FLOAT=4,
} mq_texture_format;
typedef enum mq_present_mode { MQ_PRESENT_FIFO=0, MQ_PRESENT_MAILBOX=1, MQ_PRESENT_IMMEDIATE=2 } mq_present_mode;

/* ---- T1 pass 层：声明式，不重排 / 不别名（docs/13 §3） ---- */
typedef enum mq_load_op  { MQ_LOAD_CLEAR=0, MQ_LOAD_LOAD=1, MQ_LOAD_DONT_CARE=2 } mq_load_op;
typedef enum mq_store_op { MQ_STORE_STORE=0, MQ_STORE_DISCARD=1 } mq_store_op;
typedef enum mq_access   { MQ_ACCESS_READ=1, MQ_ACCESS_WRITE=2, MQ_ACCESS_READWRITE=3 } mq_access;

typedef struct mq_color_target {
    mq_resource_id texture;      /* NULL = 用交换链当前帧颜色目标 */
    mq_load_op     load;
    mq_store_op    store;
    float          clear[4];
} mq_color_target;

typedef struct mq_pass_dep {     /* 声明读写依赖，供校验/列举/未来 T2 */
    mq_resource_id resource;
    uint32_t       access;       /* mq_access */
    uint32_t       _pad;
} mq_pass_dep;

typedef struct mq_pass_desc {
    uint32_t          struct_size;
    const char*       name;        /* 可读名，控制平面列举（A1，docs/11 §3） */
    uint32_t          pass_id;     /* 帧内显式顺序（T1 不重排） */
    mq_color_target   color;
    mq_resource_id    depth;       /* 可 NULL */
    mq_load_op        depth_load;
    mq_store_op       depth_store;
    float             depth_clear;
    const mq_pass_dep* reads;      uint32_t read_count;
    const mq_pass_dep* writes;     uint32_t write_count;
    uint32_t          depends_on;  /* 前驱 pass_id（可 =0xFFFFFFFF 无前驱） */
    uint32_t          flags;       /* MQ_PASS_OFFSCREEN / MQ_PASS_MAIN_LAYER / ... */
} mq_pass_desc;

typedef enum mq_pass_flags {
    MQ_PASS_NONE       = 0,
    MQ_PASS_OFFSCREEN  = 1u << 0,   /* 渲染到离屏目标（capture/preview） */
    MQ_PASS_MAIN_LAYER = 1u << 1,   /* 主层（可直接交换） */
    MQ_PASS_DISABLED   = 1u << 2,   /* 控制平面可启停（A1） */
} mq_pass_flags;

/* ---- 离屏目标 / 截帧 / readback（docs/13 §6，docs/11 §6） ---- */
typedef struct mq_image {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t format;      /* mq_texture_format */
    uint32_t stride;      /* 行字节数（含 padding） */
    uint32_t _pad;
    uint8_t* pixels;      /* renderer 分配；调用 mq_render_image_free 释放 */
} mq_image;
typedef void (*mq_readback_cb)(void* ud, mq_render_result r, const mq_image* img);

/* ---- 观测（docs/13 §7，docs/09） ---- */
typedef struct mq_frame_stats {
    uint32_t struct_size;
    uint32_t draw_calls;
    uint32_t triangles;
    uint32_t passes;
    uint64_t gpu_bytes;
    uint64_t frame_index;
} mq_frame_stats;

/* =========================================================
 *  公共操作面：mq_render_v1
 * ========================================================= */
typedef struct mq_render_v1 {
    uint32_t struct_size;
    uint32_t abi_version;

    /* 生命周期：注入 allocator + vfs（资产加载）+ 可选 executor（异步回读） */
    mq_render_result (*create) (void* host, mq_allocator* a, void* vfs, void* executor,
                                const mq_swapchain_desc* opt, mq_render** out);
    void             (*destroy)(mq_render* r);
    mq_render_result (*wait_idle)(mq_render* r);   /* 帧边界冷路径；模拟线程禁调 */

    /* 适配器自省（A1；ADR-0029：无 Vulkan/Metal/D3D12 → ERR_NO_ADAPTER） */
    mq_render_result (*adapter_info)(mq_render* r, char* out_json, size_t cap, size_t* out_n);

    /* 交换链 */
    mq_render_result (*create_swapchain) (mq_render* r, const mq_native_window* win,
                                          const mq_swapchain_desc* d, mq_swapchain** out);
    void             (*destroy_swapchain)(mq_render* r, mq_swapchain* sc);
    mq_render_result (*resize_swapchain) (mq_render* r, mq_swapchain* sc, uint32_t w, uint32_t h);
    mq_render_result (*acquire_frame)    (mq_render* r, mq_swapchain* sc, mq_frame** out);
    mq_render_result (*present)          (mq_render* r, mq_swapchain* sc);

    /* 资源创建（纹理/着色器可经 VFS path；load_* 支持异步） */
    mq_render_result (*create_buffer) (mq_render* r, const void* data, uint64_t bytes,
                                       uint32_t usage, mq_resource_id* out);
    mq_render_result (*create_texture)(mq_render* r, uint32_t w, uint32_t h, uint32_t fmt,
                                       uint32_t usage, const void* pixels, mq_resource_id* out);
    mq_render_result (*load_texture)(mq_render* r, const char* vfs_path,
                                     mq_gpu_task** out_task);   /* 完成得 resource */
    mq_render_result (*create_sampler)(mq_render* r, uint32_t filter, uint32_t wrap, mq_resource_id* out);
    mq_render_result (*create_shader) (mq_render* r, const char* wgsl_text, const char* entry,
                                       uint32_t stage, mq_resource_id* out);   /* 编译期校验 */
    mq_render_result (*create_pipeline)(mq_render* r, mq_resource_id shader,
                                        const void* vertex_layout,
                                        const void* state_desc,
                                        mq_resource_id* out_pipeline,
                                        mq_pipeline_key* out_key);   /* 状态哈希缓存 */
    mq_render_result (*create_bind_group)(mq_render* r, const void* binding_desc,
                                          const mq_resource_id* resources, uint32_t count,
                                          mq_resource_id* out);
    void             (*destroy_resource)(mq_render* r, mq_resource_id id);

    /* 异步任务查询（帧边界驱动完成检查，同 VFS 约定） */
    mq_render_result (*task_state) (mq_render* r, mq_gpu_task* t, uint32_t* out_state);
    mq_render_result (*task_result)(mq_render* r, mq_gpu_task* t, mq_resource_id* out_res);
    void             (*task_release)(mq_render* r, mq_gpu_task* t);

    /* ---- T1 pass 层：声明 → 绘制 → 提交 ---- */
    mq_render_result (*frame_begin) (mq_render* r, mq_swapchain* sc, mq_frame** out); /* 或 offscreen */
    mq_render_result (*frame_begin_offscreen)(mq_render* r, mq_resource_id target,
                                              uint32_t w, uint32_t h, mq_frame** out);
    mq_render_result (*declare_pass)(mq_render* r, mq_frame* f, const mq_pass_desc* d, mq_pass** out);
    mq_render_result (*pass_begin)  (mq_render* r, mq_frame* f, mq_pass* p);
    mq_render_result (*pass_draw)   (mq_render* r, mq_frame* f, const mq_drawlist* dl);
    mq_render_result (*pass_end)    (mq_render* r, mq_frame* f, mq_pass* p);
    mq_render_result (*frame_submit)(mq_render* r, mq_frame* f);   /* 结束录制并提交 */
    mq_render_result (*frame_end)   (mq_render* r, mq_frame* f);

    /* ---- 离屏 + readback：capture_frame（docs/11 §6） ---- */
    mq_render_result (*readback_async)(mq_render* r, mq_resource_id target,
                                       uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                       uint32_t fmt, mq_readback_cb cb, void* ud,
                                       mq_gpu_task** out_task);
    mq_render_result (*readback_sync) (mq_render* r, mq_resource_id target,
                                       uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                       uint32_t fmt, mq_image* out);   /* 冷路径 */
    void             (*image_free)    (mq_render* r, mq_image* img);

    /* ---- 观测 / pass 控制（A1/A2） ---- */
    mq_render_result (*frame_stats) (mq_render* r, mq_frame_stats* out);
    mq_render_result (*dump_passes) (mq_render* r, char* out_json, size_t cap, size_t* out_n);
    mq_render_result (*set_pass_enabled)(mq_render* r, const char* pass_name, uint32_t enabled);

    /* DrawList 序列化（独立于设备，A1 可 dump / 跨进程传递） */
    mq_render_result (*drawlist_pack)  (const mq_drawlist* dl, void* out_buf, size_t cap, size_t* out_n);
    mq_render_result (*drawlist_unpack)(const void* buf, size_t n, mq_allocator* a, mq_drawlist* out);
    void             (*drawlist_free)  (mq_drawlist* dl, mq_allocator* a);
} mq_render_v1;

/* =========================================================
 *  内部后端 seam：真实=WebGPU(wgpu/Dawn)，测试=fake（记录命令）
 *  renderer/gfx 只经此 vtable 触达 GPU；CI 无 GPU 也能跑单测。
 * ========================================================= */
typedef struct mq_gpu_backend_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    mq_render_result (*init_device) (void* host, mq_allocator* a, char* out_adapter_json, size_t cap, void** out_dev);
    void             (*shutdown)    (void* dev);
    mq_render_result (*create_resource)(void* dev, uint32_t kind, const void* desc, mq_resource_id* out);
    void             (*destroy_resource)(void* dev, mq_resource_id id);
    mq_render_result (*begin_pass)(void* dev, void* frame, const mq_pass_desc* d);
    mq_render_result (*draw)      (void* dev, void* frame, const mq_draw_item* item,
                                   const void* verts, uint64_t vbytes);
    mq_render_result (*end_pass)  (void* dev, void* frame);
    mq_render_result (*submit)    (void* dev, void* frame);
    mq_render_result (*readback)  (void* dev, mq_resource_id target, void* out_pixels, uint64_t bytes);
} mq_gpu_backend_v1;
```

### 2.2 类型清单

| 类型 | 性质 | 说明 |
|------|------|------|
| `mq_render_result` | POD 枚举 | 本模块统一错误码，全入口 noexcept |
| `mq_render` / `mq_swapchain` / `mq_frame` / `mq_pass` / `mq_gpu_task` | 不透明句柄 | 绝不暴露内部结构 |
| `mq_resource_id` | POD `{index,generation}` | GPU 资源可序列化 ID；DrawList 跨边界引用 |
| `mq_pipeline_key` | POD u64 | 管线状态哈希，缓存键 / DrawList 引用 |
| `mq_draw_item` / `mq_drawlist` | 固定布局 POD | 可序列化绘制描述，`static_assert` 校验 |
| `mq_drawlist_pack_header` | POD | 打包头（magic/版本/计数/CRC） |
| `mq_pass_desc` / `mq_pass_dep` / `mq_color_target` | POD | T1 pass 声明（读写依赖/目标/顺序） |
| `mq_image` | POD + 归属已定义 | readback 结果；renderer 分配，`image_free` 释放 |
| `mq_frame_stats` | POD | draw call / 三角形 / pass / 显存水位 |
| `mq_allocator` | POD vtable | 宿主注入，分配者释放 |
| `mq_render_v1` | C vtable | 公共操作面 |
| `mq_gpu_backend_v1` | C vtable | **内部**后端 seam；WebGPU 实现或 fake |
| `mq_readback_cb` | 函数指针 | 异步回读完成回调 |

### 2.3 操作面语义（逐函数契约）

- `create`：建立独立实例上下文（allocator、vfs、executor、资源表、管线缓存全在 `mq_render`）。无可用适配器（无 Vulkan/Metal/D3D12）返回 `MQ_RENDER_ERR_NO_ADAPTER`（ADR-0029，**不降级**），`*out` 不变。
- `adapter_info` / `dump_passes` / `frame_stats`：纯查询，供控制平面 `Renderer` domain 自省（A1/A2）；不产生 GPU 副作用。
- `create_shader`：**WGSL 文本入库**（docs/13 §5）；编译/校验失败返回 `MQ_RENDER_ERR_SHADER` 并给出诊断到 `out_json`（复用 adapter_info 式缓冲）。
- `create_pipeline`：对 `(shader, vertex_layout, state_desc)` 求状态哈希做**管线缓存**；命中直接返回既有 `mq_resource_id` 与相同 `mq_pipeline_key`，不重编译。
- `load_texture`：经 **VFS** 异步加载（docs/04/08），完成得资源；帧间驱动 `task_state`。
- `frame_begin` / `frame_begin_offscreen`：开始一帧录制；后者绑定离屏 `target`（capture/preview）。
- `declare_pass`：注册 pass 声明并**校验依赖**：①所有 `reads` 的资源若在帧内被写，须由 `depends_on` 之前的 pass 写；②`depends_on` 必须已声明；③读写依赖不成环。违反返回 `MQ_RENDER_ERR_PASS_ORDER` / `MQ_RENDER_ERR_PASS_CYCLE`。T1 **不重排**，`pass_id` 即执行顺序。
- `pass_draw`：把一个 DrawList 提交到当前 pass；层内按 `sort_key` 稳定排序后批处理（同 `pipeline` + 同 `material` 合并实例化）。
- `frame_submit`：结束录制、按声明顺序翻译为后端命令并提交；递增 `frame_index`。
- `readback_async` / `readback_sync`：离屏目标 → 像素。`async` 立即返回（`MQ_RENDER_ERR_PENDING` 表进行中），完成经 `cb`；`sync` 为冷路径封装（模拟线程调用返回 `MQ_RENDER_ERR_UNSUPPORTED`，防破坏确定性）。
- `drawlist_pack` / `drawlist_unpack`：DrawList 与字节流互转，**独立于设备**、可在设备创建前用于 dump/检视（A1）；`unpack` 校验 magic/version，版本不符 `MQ_RENDER_ERR_ABI`，cap 不足 `MQ_RENDER_ERR_NO_SPACE` 且 `*out_n` 给所需大小。

### 2.4 数据流与 pass 分层（docs/13 §2/§3/§4）

```
节点树/ECS ──bake──▶ Renderable ──提取(cull/batch)──▶ DrawList(层=2D|3D|UI)
   (docs/12)                                          │  (serializable, A1)
                                                      ▼
   pass 列表(T1 声明式, 显式顺序 + 声明读写依赖) ──▶ mq_gpu_backend_v1 ──▶ WebGPU
        ├─ 3D 世界 pass（网格 + 实例化）
        ├─ 2D 图层 pass（精灵批）
        ├─ UI pass
        └─ 离屏 pass → readback → mq_image（capture_frame / 图像回归）
```

- **合成顺序显式**：UI 在 3D/2D 之上，由 `pass_id` + `depends_on` + `sort_key` 决定，不靠隐式排序。
- **不重排、不别名**：T1 只做"声明 + 顺序校验 + 列举/启停"，把 T2 的重排/裁剪/瞬态别名留作接口演进（OD-11）。

### 2.5 错误模型

- 全模块统一 `mq_render_result`（C ABI 禁异常，docs/02 R2）；内部实现用 `std::expected<T, mq_render_result>`，边界只传返回码（ADR-0012）。
- 约定：负值为错误，`OK=0`；`MQ_RENDER_ERR_PENDING` 是"进行中"状态而非失败，调用方须区分。
- `DEVICE_LOST` / `SURFACE_LOST` 为可恢复错误：调用方重建上下文/交换链后重试。

### 2.6 版本化

- 三个 vtable（`mq_render_v1` / `mq_gpu_backend_v1` / `mq_drawlist`）均以 `struct_size` + `abi_version` 打头，**`struct_size` 只增不改**：新版本在表尾追加，旧调用方按自身 `struct_size` 截断使用。
- `mq_drawlist` 携带独立 `version`（`MQ_DRAWLIST_VERSION`），与 pack 头版本一致；跨进程 dump 按此校验。
- 接口名（`"maque.render.v1"` / `"maque.gpu_backend.v1"`）与主版本绑定；主版本不同 → `MQ_RENDER_ERR_ABI`。

### 2.7 内存所有权

- **分配者释放**：`mq_render`、GPU 资源、`mq_image.pixels`、`mq_drawlist.items`、任务句柄均由 renderer 经注入的 `mq_allocator` 分配；调用方用对应 `destroy_resource` / `image_free` / `drawlist_free` / `task_release` 归还，不 `free` 非自己分配之物。
- **`char*` 输入**（`vfs_path` / WGSL 文本 / `pass name`）：调用方拥有、UTF-8、调用返回后可失效；renderer 若需留存即复制进自身 arena。
- **DrawList 输入**：`pass_draw` 期间有效即可；renderer 不取得其所有权（除非显式 `drawlist_unpack`）。
- **`callback`**：`readback_cb` 的 `img` 仅回调用内有效，调用方须复制以留存。
- **输出缓冲**（`adapter_info` / `dump_passes` / `drawlist_pack`）：写入调用方缓冲，cap 不足返回 `MQ_RENDER_ERR_NO_SPACE` 且 `*out_n` 给所需字节。

### 2.8 线程 / 确定性

- **单向数据流**：renderer 是模拟的**只读消费方**，**绝不回写**模拟状态（ADR-0005，docs/13 §8）。提取只读世界，产出新的 DrawList。
- **默认单线程**（ADR-0018）：录制与提交在模拟线程；异步加载/回读经 executor 在帧边界完成，模拟线程永不阻塞。
- **帧内禁阻塞回读**：模拟帧内不得调 `readback_sync` / `wait_idle`（返回 `UNSUPPORTED` 或断言）。
- 所有状态在 `mq_render` 上下文，**零全局可变状态 / 无单例**（ADR-0014），支持多实例隔离。
- **确定性**：给定同一 DrawList 序列 + 同 seed → 后端命令流与 pack 字节一致（fake 后端可断言）；渲染不得影响模拟 checksum。

## 3. 依赖与边界

- **可依赖**：
  - `core/data`：POD 类型、`mq_resource_id` 句柄、`std::expected`（仅模块内部）。
  - `math`：mat4 / 视锥（cull）——仅在提取阶段（冷/热路径的 CPU 侧），不跨边界。
  - `VFS`：纹理/网格/着色器资产加载（docs/04）。
  - `platform`：原生窗口/表面句柄、WebGPU surface 创建（薄抽象，docs/05）。
  - `executor`：异步资产加载与回读完成（唯一异步出口）。
  - `module host`：发现 GPU 后端 `.so`（`maque.gpu_backend.v1`）。
  - allocator 注入：宿主 `mq_allocator`。
- **禁止**：
  - ❌ 依赖 scene / animation / particle 等**世界层内部存储**；只消费 DrawList 与 Renderable 的中性描述（docs/13 §1）。
  - ❌ **回写模拟**（任何读回结果不得进入模拟状态）。
  - ❌ 自建 WebGPU 之上的第二层图形抽象；不自造 frame graph 引擎（T1 止步于声明+校验，OD-11）。
  - ❌ 跨边界传 `std::string/vector/shared_ptr/异常/RTTI`（docs/02 §9）。
  - ❌ 持有全局可变状态 / 单例（破坏多实例，ADR-0014）。
  - ❌ 暴露裸 GPU 指针；热路径（每 draw）不得反复过重虚函数（后端 seam 按批调用，非逐顶点）。
  - ❌ 在模拟帧内阻塞回读或等待 GPU。
- **接缝**：
  - **向上**：scene/animation/particle 的提取阶段产出 `mq_drawlist`，经 `pass_draw` 提交；控制平面经 `Renderer` domain 调 `capture_frame`（= `frame_begin_offscreen` + `readback_*`）、`dump_passes`、`set_pass_enabled`（docs/11 §3）。
  - **向下**：`mq_gpu_backend_v1` 由 WebGPU 实现（原生 wgpu/Dawn、WASM 原生 WebGPU）；测试由 **fake 后端**实现。
  - **观测**：`frame_stats` / `dump_passes` / timestamp 查询对接 observability（docs/09，A2）。

## 4. 测试计划（从接口推导，先行）

> 先写测试（红），再实现（绿）。单测**独立**：仅依赖 `renderer` + `mq_render.h`，GPU 交互全部走 **fake 后端**（记录命令），CI 无 GPU 也能跑。真实 GPU 用例进独立 CI 分支。

### 4.1 测试替身（fixtures）

| 替身 | 作用 |
|------|------|
| `fake_gpu_backend` | 实现 `mq_gpu_backend_v1`，把每条 `begin_pass/draw/end_pass/submit` **顺序记录为命令流**，供断言；可注入 `DEVICE_LOST` / `OUT_OF_MEMORY` / `SHADER` 失败 |
| `fake_vfs` | 返回预置纹理/网格/着色器字节；可注入失败（`MQ_RENDER_ERR_VFS`） |
| `fake_executor` | 手动脉冲，精确控制异步加载/回读完成时机 |
| `fake_allocator` | 统计分配/释放，断言"分配者释放"与无泄漏 |
| `headless_device` | 无窗口的 `mq_render`（`create(opt=NULL)`），供纯离屏用例 |
| `golden_images` | 离线基准 PNG 集，供有 GPU 分支的图像回归比对 |

### 4.2 单元测试用例清单

| # | 用例 | 输入 | 期望 | 类型 |
|---|------|------|------|------|
| 1 | 创建/销毁 | `create(fake)`→`destroy` | `OK`，`fake_allocator` 净分配 0 | 正常/所有权 |
| 2 | 无适配器 | fake 报无 Vulkan/Metal/D3D12 | `ERR_NO_ADAPTER`，`*out` 不变 | 错误/ADR-0029 |
| 3 | 适配器自省 | `adapter_info` | JSON 含后端名/特性/上限，可读（A1） | 正常 |
| 4 | ABI 不匹配 | `abi_version=99` | `ERR_ABI` | 版本 |
| 5 | vtable 前向兼容 | 构造更小 `struct_size` 的旧表 | 正常调用旧面，不越界读 | 版本 |
| 6 | 创建资源 | buffer/texture/sampler/shader | `OK` 且返回有效 `mq_resource_id` | 正常 |
| 7 | 过期句柄 | destroy 后复用 `resource_id` | `ERR_NOT_FOUND`（世代校验） | 边界 |
| 8 | WGSL 编译失败 | 非法 WGSL 文本 | `ERR_SHADER` + 诊断输出 | 错误 |
| 9 | 管线缓存命中 | 同 `(shader,layout,state)` 两次 | 同 `mq_pipeline_key`，back-end 编译计数=1 | 核心 |
| 10 | 管线状态差异 | 仅 blend 不同 | 不同 key，各编译一次 | 核心 |
| 11 | 绑定组创建 | 资源数组 + 描述 | `OK`，back-end 记录绑定 | 正常 |
| 12 | 异步纹理加载 | `load_texture(fake_vfs)`+脉冲 | `task_state DONE`，得资源 | 异步 |
| 13 | 加载失败 | fake_vfs 注入 `VFS` 错 | `ERR_VFS`，无泄漏 | 错误 |
| 14 | DrawList 构建 | builder push N 项 | `item_count==N`，布局正确 | 正常 |
| 15 | DrawList pack 往返 | pack→unpack | 逐字段相等 | 核心 |
| 16 | pack 版本不符 | 改头 version | `ERR_ABI` | 版本 |
| 17 | pack 缓冲不足 | cap 过小 | `ERR_NO_SPACE` 且 `out_n` 报所需 | 边界 |
| 18 | pack 确定性 | 同 DrawList 两次 pack | 字节完全一致（含 CRC） | 确定性 |
| 19 | pass 声明+顺序 | 两 pass 依赖正确 | `OK`，命令流 pass 顺序 = `pass_id` | 核心 |
| 20 | **依赖违反** | pass 读某资源但写它的前驱未声明 | `ERR_PASS_ORDER` | 错误 |
| 21 | **依赖成环** | A 依赖 B、B 依赖 A | `ERR_PASS_CYCLE` | 错误 |
| 22 | load/store 映射 | CLEAR/LOAD/STORE/DISCARD 组合 | 后端记录对应操作 | 正常 |
| 23 | 离屏目标 clear | `frame_begin_offscreen` + clear | 后端记录离屏目标+clear 色 | 正常 |
| 24 | 异步回读 | `readback_async`+脉冲 | 回调收到 `OK` + 尺寸正确 `mq_image` | 异步 |
| 25 | 同步回读（冷路径） | 非模拟线程 `readback_sync` | 返回像素；模拟线程调用 → `UNSUPPORTED` | 安全 |
| 26 | 回读区域裁剪 | 子矩形 x/y/w/h | `image` 尺寸/stride 正确 | 边界 |
| 27 | 交换链 resize | `resize_swapchain` | 重建目标，旧帧失效 | 正常 |
| 28 | 表面丢失 | fake 注入 `SURFACE_LOST` | 可恢复错误，重建后成功 | 错误 |
| 29 | 设备丢失 | fake 注入 `DEVICE_LOST` | `ERR_DEVICE_LOST`，上下文可重建 | 错误 |
| 30 | 帧统计 | 提交含 3 pass/若干 draw | `draw_calls/triangles/passes` 正确 | 自省 |
| 31 | pass 列举/启停 | `dump_passes` / `set_pass_enabled` | JSON 完整；禁用 pass 不出现在命令流 | 自省/A1 |
| 32 | 时间戳 | 后端返回 pass 耗时 | 关联到对应 pass 名（A2） | 观测 |
| 33 | **批处理合并** | N 精灵同 pipeline/material | 后端记录 **1** draw + `instance_count=N` | 核心 |
| 34 | 排序稳定 | 同 `sort_key` 多项 | 顺序与输入序一致（稳定） | 确定性 |
| 35 | 剔除 | 视锥外项 | 不出现在命令流 | 核心 |
| 36 | 多实例隔离 | 两 `mq_render` 同后端 | 资源/命令互不干扰，句柄不跨实例 | 多实例 |
| 37 | 内存所有权 | 全路径走 `fake_allocator` | 退出净分配 0，无二次释放 | 所有权 |
| 38 | 帧内禁阻塞 | 模拟线程 `wait_idle`/`readback_sync` | `UNSUPPORTED`/断言 | 安全 |

### 4.3 游戏场景用例（真实玩法驱动）

| # | 场景 | 步骤 | 期望 |
|---|------|------|------|
| S1 | **一帧含精灵+网格+UI** | 构建 3D 网格 pass + 2D 精灵 pass + UI pass，各填 DrawList，`submit` | 命令流 pass 顺序为 3D→2D→UI；draw/三角形计数正确 |
| S2 | **离屏截帧（AI 预览）** | `frame_begin_offscreen` 渲染 S1 → `readback_async` | 得 `mq_image`，尺寸/格式正确，等价 `Renderer.capture_frame`（docs/11 §6） |
| S3 | **图像回归（有 GPU CI 分支）** | 相同场景渲染 → 与 `golden_images` 比对 | 像素差在阈值内（无 GPU 时跳过，fake 断言命令流一致） |
| S4 | **相机移动** | 改变视锥/变换后重渲截帧 | 可见集变化，剔除生效，截帧不同 |
| S5 | **着色器热改** | 替换 WGSL 文本重建管线 | 新 key 编译一次，旧管线释放；画面按新着色器 |
| S6 | **粒子批** | 数千同 pipeline 粒子项 | 合并为少数 draw，`instance_count` 正确 |
| S7 | **窗口 resize** | 运行中 `resize_swapchain` | 比例正确，UI 锚点不随拉伸漂移（层独立） |
| S8 | **控制平面预览** | 经 `Renderer` domain：截帧 + `frame_stats` + `dump_passes` | 图像 + 指标 + pass 列表三者一致（A1/A2/A4） |

### 4.4 确定性用例

| # | 用例 | 期望 |
|---|------|------|
| D1 | **渲染不回写模拟** | 渲染整帧后，模拟世界 checksum 与渲染前**完全一致**（只读消费，ADR-0005） |
| D2 | DrawList 字节确定 | 相同提取输入 → `drawlist_pack` 字节一致（含稳定排序） |
| D3 | 命令流确定 | fake 后端：相同 DrawList 序列 → 相同命令流（同 seed）；回放一致 |
| D4 | 回读确定 | 相同场景离屏截帧 → 相同像素（有 GPU 时逐字节/阈值比对） |
| D5 | 完成顺序无影响 | 以不同顺序脉冲异步加载完成，最终命令流/帧结果不变 |

### 4.5 测试替身与独立单测保障

- 单测仅链接 `renderer` + `mq_render.h`，**fake 后端**实现 `mq_gpu_backend_v1`，**零真实 GPU/窗口**依赖（CI 可在无 GPU 机器跑）。
- 资产交互全部经 `fake_vfs`；异步用 `fake_executor` 手动脉冲，避免线程/时序依赖。
- `fake_allocator` 断言所有权与无泄漏。
- **有 GPU 的 CI 分支**：用真实 WebGPU 后端跑 S3/D4 图像回归；无 GPU 时这些用例标记跳过，其余全量在 fake 后端通过。

## 5. 开发计划（分阶段 TDD）

1. **阶段 0 · 冻结接口**：定稿 `mq_render.h`（`mq_render_v1` + `mq_gpu_backend_v1` + `mq_drawlist`/`mq_pass_desc`）、结果码、版本化、所有权；写 `static_assert(sizeof(mq_draw_item)==96)` 等布局断言与 ABI 校验测试。**冻结后签名只增不改。**
2. **阶段 1 · 测试骨架（红）**：实现 `fake_gpu_backend`（命令记录）/`fake_vfs`/`fake_executor`/`fake_allocator`；写 4.2 全表 + 4.4 D1/D2 用例（先失败）；实现 DrawList 构建/`pack`/`unpack`（用例 14–18 转绿）。
3. **阶段 2 · 最小实现（绿）**：设备/资源生命周期 + 管线状态哈希缓存；T1 `frame_begin/declare_pass/pass_begin/pass_draw/pass_end/frame_submit` + 依赖校验（用例 6–13、19–23、31、33–35 转绿）。
4. **阶段 3 · 离屏与回读**：`frame_begin_offscreen` + `readback_async`/`readback_sync` + `image_free`；落地 `capture_frame` 语义（用例 24–26、S2、D4/D5）。补边界/错误/多实例/所有权（27–30、32、36–38）。
5. **阶段 4 · 真实 WebGPU 后端**：接入 wgpu/Dawn，实现 `mq_gpu_backend_v1`；窗口/交换链、WGSL 编译、readback；对接 `fake_vfs`→真实 VFS；跑 S1–S8 与有 GPU 的 S3/D4。
6. **阶段 5 · 观测与重构**：`frame_stats`/`dump_passes`/timestamp 对接 observability（A2）；抽批处理与排序公共路径；整理文档与遗留项。

## 6. 验收标准（DoD）

- `mq_render.h` 接口**冻结**，被上游（scene 提取、控制平面 `Renderer` domain）fake 依赖编译通过；
- 4.2 全部用例 + 4.3 场景（S1–S8，其中 S3/D4 在有 GPU 分支）+ 4.4 确定性用例通过；
- 单测覆盖率 ≥ 80%（pass 依赖校验 / 管线缓存 / DrawList 序列化 / 批处理 ≥ 90%）；
- **渲染不回写模拟**（D1）通过；fake 后端命令流确定性（D3）通过；
- 无全局可变状态；两实例隔离（#36）通过；
- 无 GPU 环境下全部 fake 后端用例通过；有 GPU CI 分支图像回归通过；
- 性能预算：管线缓存命中不重编译；批处理使同管线同材质 N 项合并为 1 draw；单帧 CPU 侧提取+录制不超预算（目标 < 2ms/帧，含 5k draw 项）；
- 内存：`fake_allocator` 净分配为 0，无泄漏/二次释放；
- 不依赖任何上层模块实现（只消费 DrawList）。

## 7. 风险

| 风险 | 处理 |
|------|------|
| 过度设计 frame graph（WebGPU 收益低） | 默认 **T1**：声明+顺序校验+列举/启停；T2 仅按硬指标触发（OD-11） |
| 嵌入式 WebGPU 驱动缺失 | **不降级**：无 Vulkan/Metal/D3D12 目标不支持，`ERR_NO_ADAPTER`（ADR-0029，用例 #2） |
| 离屏 readback 开销 | 仅按需捕获；`readback_async` 异步回读；帧内禁同步阻塞（D1/用例 #38） |
| 管线编译卡顿 | 状态哈希缓存 + 预热；缓存命中不重编译（用例 #9/#10） |
| **渲染回写模拟（破坏确定性）** | 单向数据流：模拟→提取→渲染；只读消费；D1 强制校验 |
| 着色器维护 | WGSL 文本入库 + `create_shader` 编译期校验（docs/08，用例 #8） |
| DrawList 跨版本/跨进程不兼容 | 独立 `MQ_DRAWLIST_VERSION` + pack 头 magic/CRC；版本不符 `ERR_ABI`（用例 #15–18） |
| 分层合成顺序错乱 | `pass_id`+`depends_on`+稳定 `sort_key` 显式声明；依赖校验（用例 #19–21、S1） |
| 后端 seam 泄漏为"第二层图形抽象" | `mq_gpu_backend_v1` 仅作**测试 seam**，只暴露命令级接口；真实实现直连 WebGPU，不做抽象化重排 |
| 多实例相互污染 | 零全局可变状态，状态全在 `mq_render`；隔离用例（#36，ADR-0014） |

## 8. Owner agent

- **角色标识**：`agent-renderer`
- **职责边界**：仅负责 renderer 模块的接口（`mq_render.h`）、测试（fake GPU 后端单测、DrawList 打包/解包、pass 顺序与依赖、离屏截帧、分层合成、游戏场景与确定性用例）与实现（gfx 前端、T1 pass 层、管线缓存、WebGPU 后端 seam 与真实后端）。不修改 scene/animation/particle 等其它模块；提取阶段的 Renderable→DrawList 由世界层模块产出，本模块只消费。任何跨模块接口变更需先修订本计划与 `docs/13`。
