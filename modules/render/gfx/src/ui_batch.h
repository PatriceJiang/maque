/* UI / 2D 图元的 drawcall 合批（docs/13 §分层、AGENTS「两类图元」）。
 *
 * 背景：`mq_renderer_item` 是"一个矩形/圆 = 一个实例"，gfx 早先只把**相邻且同贴图 + 同采样**
 * 的图元并成一次实例化绘制。UI（layer=1）由"控件矩形（solid）+ 字形（字体图集）"交替组成，
 * 状态一切换就断批 → 一个 HUD 有多少控件就有多少次 drawcall。
 *
 * 本类把批次决策从 gfx.cpp 里抽出来，单独成类（纯 CPU、不依赖 wgpu / ABI 头，便于单测）：
 *
 *   · **画家序模式**（其它图层，默认）：与旧实现逐字节等价 —— 相邻同键合并，网格图元作屏障。
 *   · **UI 分组模式**（layer=1）：按渲染状态（solid / 贴图 + 采样）分组，每组最多一次实例化绘制，
 *     把 O(控件数) 的 drawcall 降到 O(状态数)。
 *
 * 分组会改变图元的绘制顺序，所以必须证明"顺序不可见"。规则：
 *   段内每个状态组按**首次出现顺序**排位；处理图元 X 时，若存在**更早绘制**、**排位更靠后**的
 *   图元覆盖了 X 的包围盒 → 分组会把 X 画到它前面 → 视觉可能不同 → 就地**切段**
 *   （之前已收集的图元按组落盘，X 从新段开始）。段与段之间保持画家序，网格图元也切段。
 *   于是任何被重排的图元对都是"包围盒不相交"的，像素输出与逐图元顺序逐字节一致。
 *   重叠检测用 8 设计单位的粗网格（格子更粗 = 更保守 = 只少合批、不改变结果）；
 *   AABB 额外外扩 `bleed`（覆盖 snap 像素吸附与光栅化边缘）。
 *
 * 坐标：全部在**设计坐标**（与 `mq_renderer_item.x/y` 同空间）。视图变换是可逆仿射，
 * 不改变"两个四边形是否相交"，所以不需要知道 TW/TH / 相机。
 */
#pragma once

#include <cstdint>
#include <vector>

namespace maque::gfx {

/* 一个 2D 实例：与 assets/builtin/shaders/{solid,textured}/shader.wgsl 的 `Inst` 逐字段一致
 *（16 × 32 位 = 64B stride）。由 gfx 从 mq_renderer_item 转换后交给 UiBatcher。 */
struct BatchInstance {
    float pos[2];      /* 0  中心（设计坐标，y 向下） */
    float radius;      /* 8  圆/方半径（half 退化时用） */
    float rot;         /* 12 自转（弧度） */
    float color[4];    /* 16 */
    float uv[4];       /* 32 图集子矩形（左上/右下）；退化 = 整张贴图 */
    float half[2];     /* 48 矩形半尺寸（两者都 > 0 才算矩形） */
    float shape;       /* 56 0=圆 1=矩形 */
    float snap;        /* 60 1=左上角吸附整数目标像素 */
    int32_t mask;      /* 64 程序化 alpha mask 链头；0=无 */
    int32_t pad[3];    /* 68 对齐到 80B，与 WGSL mask/pad 一致 */
};
static_assert(sizeof(BatchInstance) == 80, "BatchInstance 必须与 WGSL Inst 布局一致（stride 80）");

/* 批次状态键：只有它相同的图元才能共享一次 drawcall。
 * texture < 0 = solid（无贴图）；nearest 选择采样器（字形/像素画用 nearest）。 */
struct BatchKey {
    int32_t texture = -1;
    bool nearest = false;
    /* 裁剪矩形（设计坐标，屏幕 y 向下；w/h <= 0 = 不裁剪）。
     * 不同裁剪区不能共享一次 scissor → 必须作为状态键的一部分。reverse Mask 会给出
     * 多个互不重叠的矩形，每个矩形各成一批；同一矩形内的 9-patch 块仍合成一次 drawcall。 */
    float clip[4] = { 0.0F, 0.0F, 0.0F, 0.0F };
    friend bool operator==(const BatchKey&, const BatchKey&) = default;
};

/* 一条绘制命令（顺序 = 提交顺序）。 */
struct BatchDraw {
    enum class Kind : uint8_t { Instanced, Mesh };
    Kind kind = Kind::Instanced;
    BatchKey key{};        /* Instanced：批次状态；Mesh：key.texture = 网格页纹理（-1 = 无） */
    uint32_t first = 0;    /* Instanced：instances() 起点；Mesh：= mesh */
    uint32_t count = 0;    /* Instanced：实例数；Mesh：恒 1 */
    uint32_t mesh = 0;     /* Mesh：meshes 下标 */
};

struct UiBatchStats {
    uint32_t items = 0;      /* 输入图元数 */
    uint32_t meshes = 0;     /* 输入网格数 */
    uint32_t draws = 0;      /* 输出 drawcall 数（实例批次 + 网格） */
    uint32_t merges = 0;     /* 并入已有批次的图元数（items - batches_created） */
    uint32_t segments = 0;   /* 因顺序不安全而切段的次数 */
    uint32_t layers = 0;     /* 处理的图层数 */
};

/* 2D 合批器：每帧 `begin()` → 逐层 `begin_layer()` + `add_item/add_mesh` + `end_layer()`。
 * 实例与命令都按提交顺序输出，调用方只读。 */
class UiBatcher {
public:
    /* 预处理：图元的包围盒（设计坐标）。退化图元（无面积）返回 false，不参与重叠判定。
     * `bleed` = 外扩量（覆盖 snap 像素吸附/光栅化边缘）；供 gfx 预扫描整帧 bbox、也供单测使用。
     * `clip` = 可选的裁剪矩形（x,y,w,h；w/h<=0 = 不裁剪）：包围盒先与它求交。
     * 顺序安全必须按**可见**包围盒判定，否则不同裁剪带的图元会被当成重叠而切碎合批。 */
    static bool item_bounds(const BatchInstance& inst, float& x0, float& y0, float& x1, float& y1,
                            float bleed = 1.0F, const float* clip = nullptr);

    void begin(float min_x, float min_y, float max_x, float max_y, float bleed = 1.0f);
    void begin_layer(int32_t layer, bool group);
    void add_item(const BatchInstance& inst, const BatchKey& key);
    void add_mesh(uint32_t mesh_index, int32_t texture = -1);
    void end_layer();
    void end();

    const std::vector<BatchDraw>& draws() const { return draws_; }
    const std::vector<BatchInstance>& instances() const { return insts_; }
    const UiBatchStats& stats() const { return stats_; }

private:
    struct Bucket {
        BatchKey key;
        std::vector<BatchInstance> insts;
    };

    bool tiles_for(const BatchInstance& inst, const float* clip,
                   int& tx0, int& ty0, int& tx1, int& ty1) const;
    /* 候选图元覆盖的格子里是否有"排位更靠后"的更早图元（→ 分组会翻转二者顺序）。 */
    bool order_unsafe(int tx0, int ty0, int tx1, int ty1, uint32_t rank, const float* clip) const;
    void stamp(int tx0, int ty0, int tx1, int ty1, uint32_t rank);
    int rank_of(const BatchKey& key) const;
    void flush_segment();
    void add_painter(const BatchInstance& inst, const BatchKey& key);
    void reset_grid();

    std::vector<BatchDraw> draws_;
    std::vector<BatchInstance> insts_;
    std::vector<Bucket> seg_;
    UiBatchStats stats_;

    bool grouping_ = false;     /* 当前图层是否按状态分组 */
    bool open_ = false;         /* 画家序模式：最后一条命令仍是可续写的实例批次 */
    size_t last_draw_ = 0;      /* open_ 时指向 draws_.back() */

    /* 重叠网格（本次 begin 的 bbox 覆盖）。cell = (gen << 16) | rank；gen 隔离各分段。 */
    std::vector<uint32_t> cells_;
    int cols_ = 1, rows_ = 1;
    float origin_x_ = 0.0F, origin_y_ = 0.0F, tile_ = 8.0F, bleed_ = 1.0F;
    uint32_t gen_ = 1;
};

}  // namespace maque::gfx
