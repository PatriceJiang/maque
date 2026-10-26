/* WebGPU 渲染后端：作为插件 .so 实现 DSL 生成的 mq_renderer_v1（maque_ifaces.h）。
 * 仅导出 mq_module_query；运行时经 maque::module(dlopen) 加载。 */
#include "maque_ifaces.h"
#include "core/module/mq_module.h"
#include "builtin_shaders.h"   // 内建 shader 注册表（配置期由流水线生成）

#include <webgpu/webgpu.h>

#include <cstdio>
#include <print>
#include <algorithm>   // std::stable_sort（缺它时 clang 会连带把排序 lambda 报成捕获错误，误导性极强）
#include <cstring>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <cstdlib>

namespace maque::gfx {

#define SV(x) WGPUStringView { (x), WGPU_STRLEN }

/* ---- 3D/PBR 记录布局守卫（plans/18-pbr-3d.md §2.2）----
 * 这些记录**不含指针**，尺寸与平台/字长无关 → 可直接 static_assert；
 * C 定义与 WGSL uniform 布局逐字段对齐（M2/M3 的 PBR shader 按此打包）。 */
static_assert(sizeof(mq_renderer_caps) == 32, "mq_renderer_caps 应为 8×int32");
static_assert(sizeof(mq_renderer_view3d) == 184, "mq_renderer_view3d 布局漂移（view/proj 各 64B + 后段）");
static_assert(sizeof(mq_renderer_light) == 64, "mq_renderer_light 布局漂移");
static_assert(sizeof(mq_renderer_pbr_material) == 276, "mq_renderer_pbr_material 布局漂移（5 贴图槽 + clearcoat + 多材质模型 + 贴图 UV 变换 + uv1_mask + textures3[7]）");
static_assert(sizeof(mq_renderer_environment) == 88, "mq_renderer_environment 布局漂移（+analytic studio IBL 参数）");
static_assert(sizeof(mq_renderer_post) == 8, "mq_renderer_post 布局漂移");
static_assert(sizeof(mq_renderer_draw3d) == 96, "mq_renderer_draw3d 布局漂移（model 64B + sort_key u64）");
static_assert(offsetof(mq_renderer_draw3d, model) == 8, "draw3d.model 必须在 offset 8");
static_assert(offsetof(mq_renderer_draw3d, sort_key) == 88, "draw3d.sort_key 必须在 offset 88");

/* 内建 shader 来自**资产** assets/builtin/shaders/<name>/（不是内联字面量）：
 * 配置期由 cmake/shader_pack.cmake + tools/shader_pack.js 打包成压缩源码（zstd），
 * 这里按名字取（未编入的 shader → NULL，见 MAQUE_SHADER_<NAME>=OFF 的剔除开关）。
 *   solid    —— 无贴图图元（矩形/圆/方块）
 *   textured —— 贴图图元（图集子矩形 + 采样器），uniform 布局与 solid 一致（48B）
 *   mesh     —— 网格图元（逐顶点 pos/uv/color + 索引），uniform 布局同上（48B） */
struct Texture {
    WGPUTexture tex = nullptr;
    WGPUTextureView view = nullptr;
    WGPUSampler sampler = nullptr;                        /* create_texture2 的 filter/wrap（M3 PBR 采样用） */
    WGPUTextureFormat format = WGPUTextureFormat_RGBA8Unorm;
};

/* 3D 网格（plans/18-pbr-3d.md）：create_mesh 一次性上传，GPU 常驻。
 * 顶点布局（plans/20 §顶点布局 v1，CMake 侧与 runtime/import 的 stride 一致）：
 *   layout 0（旧，stride 48B）：pos@0 / normal@12 / tangent@24(未用) / uv0@40
 *   layout 1（新，stride 56B）：pos@0 / normal@12 / color@24(RGBA) / uv0@40 / uv1@48
 * 管线按 layout 缓存（shader entry：layout0=vs / layout1=vs1）。 */
struct MeshGpu {
    WGPUBuffer vbo = nullptr;
    WGPUBuffer ibo = nullptr;
    uint32_t index_count = 0;
    int32_t vertex_layout = 0;
    uint32_t vertex_stride = 48;                          /* 48=layout0 / 56=layout1（管线内烘焙，绘制时不传） */
    float bounds_min[3] = { 0, 0, 0 };
    float bounds_max[3] = { 0, 0, 0 };
};

struct Device {
    WGPUInstance instance = nullptr;
    WGPUAdapter  adapter = nullptr;
    WGPUDevice   device = nullptr;
    WGPUQueue    queue = nullptr;
    WGPURenderPipeline solidPipeline = nullptr;
    WGPUBindGroupLayout solidBgl = nullptr; WGPUPipelineLayout solidPl = nullptr; WGPUShaderModule solidShader = nullptr;
    WGPURenderPipeline texPipeline = nullptr;
    WGPUBindGroupLayout texBgl = nullptr; WGPUPipelineLayout texPl = nullptr; WGPUShaderModule texShader = nullptr;
    /* 网格管线：[混合模式 0..3][pma 0/1]。预乘 alpha 与直通 alpha 的混合方程不同
     * （spine 图集默认 pma: true），所以同一混合模式要两份。 */
    WGPURenderPipeline meshPipeline[4][2] = {};
    WGPUBindGroupLayout meshBgl = nullptr; WGPUPipelineLayout meshPl = nullptr; WGPUShaderModule meshShader = nullptr;
    WGPUSampler sampler = nullptr;
    WGPUSampler samplerNearest = nullptr;      /* 字形/像素画：nearest */
    std::string backend;
    std::vector<Texture*> textures;
    /* ---- 直接呈现（plans/18-pbr-3d.md §4.1）：attach_window 成功后非空（M1）---- */
    WGPUSurface surface = nullptr;
    WGPUTextureFormat surfaceFormat = WGPUTextureFormat_RGBA8Unorm;
    int32_t surfaceWidth = 0, surfaceHeight = 0;
    WGPUTexture surfaceTex = nullptr;              /* 本帧从 swapchain acquire 的纹理 */
    WGPUTextureView surfaceView = nullptr;
    bool surfaceCopySrc = false;                   /* surface caps 允许 CopySrc → 可抓直呈帧（CI 像素断言） */
    int  captureCount = 0;
    WGPUTextureFormat pipelineFormat = WGPUTextureFormat_RGBA8Unorm;  /* 2D 管线当前目标格式 */
    /* ---- M2 3D 基础：depth + unlit3d 管线（plans/18-pbr-3d.md §4.2）---- */
    WGPUTexture depthTex = nullptr;
    WGPUTextureView depthView = nullptr;
    int32_t depthW = 0, depthH = 0;
    WGPUTextureFormat p3dFormat = WGPUTextureFormat_Undefined;   /* 3D 材质管线当前目标格式 */
    WGPUBindGroupLayout p3dCamBgl = nullptr;
    WGPUBindGroupLayout p3dDrawBgl = nullptr;
    WGPUPipelineLayout p3dPl = nullptr;
    WGPUShaderModule p3dShader = nullptr;
    /* 多材质模型管线：[shading_model 0..9][blend 0/1][cull 0..2][mirrored 0/1][layout 0/1]，按需创建（plans/19/20）。
     * mirrored = model 3×3 行列式 < 0（镜像缩放的 glTF 节点）：镜像把三角形绕序翻成 CW，
     * 而 WebGPU 的 frontFace 默认是 CCW，不做补偿时 side=Front 的材质会把正面剔掉/显示背面。
     * layout = 网格顶点布局（0=48B 旧 / 1=56B COLOR_0+TEXCOORD_1），对应 shader entry vs / vs1。 */
    WGPURenderPipeline p3dMatPipeline[10][2][3][2][2] = {};
    bool p3dMatWarned[10] = {};
    bool p3dMatLayoutWarned[2] = {};      /* layout 级（vs/vs1 缺失）只告警一次 */
    /* M5c 方向光阴影：1024² depth map + comparison sampler + 1×1 深度占位（避免同 pass 同纹理读写） */
    WGPUTexture shadowTex = nullptr;
    WGPUTextureView shadowView = nullptr;
    WGPUTexture dummyDepth = nullptr;
    WGPUTextureView dummyDepthView = nullptr;
    WGPUSampler shadowSampler = nullptr;
    /* 阴影管线按顶点布局两套（pos@0+uv@40 相同、stride 不同：48 / 56）：深度 pass 只读 pos+uv。 */
    WGPURenderPipeline shadowPipeline[2] = {};
    WGPUBuffer shadowCamBuf = nullptr;
    WGPUBindGroup shadowCamBg = nullptr;
    /* PBR 贴图缺省：1×1 白纹理（材质没给 albedo 贴图时绑定它，基色乘 1） */
    WGPUTexture whiteTex = nullptr;
    WGPUTextureView whiteTexView = nullptr;
    /* 法线贴图缺省：1×1 中性法线 (0.5,0.5,1) → 解码 (0,0,1)，不扰动几何法线 */
    WGPUTexture neutralNormalTex = nullptr;
    WGPUTextureView neutralNormalView = nullptr;
    /* ---- 3D GPU 资源（plans/18-pbr-3d.md §4.3）---- */
    std::vector<MeshGpu*> meshes;
};


template <class P>
static bool poll(WGPUInstance inst, P ready, int timeout_ms = 20000) {
    auto start = std::chrono::steady_clock::now();
    while (!ready()) {
        wgpuInstanceProcessEvents(inst);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}
static void on_uncaptured(WGPUDevice const*, WGPUErrorType type, WGPUStringView msg, void*, void*) {
    std::print(stderr, "[wgpu] error {}: {:.{}}\n", (int)type, msg.data ? msg.data : "", (int)msg.length);
}
static void req_adapter_cb(WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView, void* u1, void*) {
    if (s == WGPURequestAdapterStatus_Success) *(WGPUAdapter*)u1 = a;
}
static void req_device_cb(WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView, void* u1, void*) {
    if (s == WGPURequestDeviceStatus_Success) *(WGPUDevice*)u1 = d;
}
static void map_cb(WGPUMapAsyncStatus s, WGPUStringView, void* u1, void*) { *(WGPUMapAsyncStatus*)u1 = s; }

static WGPUShaderModule make_shader(WGPUDevice dev, const char* wgsl) {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = SV(wgsl);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &src.chain;
    /* 注意：该 wgpu-native 构建未实现 wgpuShaderModuleGetCompilationInfo（会 panic），
     * 故不主动取编译信息；着色器错误经 uncapturedErrorCallback 报告。 */
    return wgpuDeviceCreateShaderModule(dev, &smd);
}

/* ---- 2D 管线：按**目标格式**创建/重建（plans/18-pbr-3d.md §4.1）------------
 * 直接呈现时目标 = swapchain 纹理（格式可能是 BGRA8/SRGB），离屏/截图 = RGBA8Unorm。
 * 管线状态（blend/topology）与格式无关，随格式变化的只有 ColorTargetState.format；
 * 切换目标格式时释放重建（最多两套，只在 attach/首次离屏发生，不在热循环里）。 */
static void release_2d_pipelines(Device* dv) {
    if (dv->solidPipeline) { wgpuRenderPipelineRelease(dv->solidPipeline); dv->solidPipeline = nullptr; }
    if (dv->texPipeline) { wgpuRenderPipelineRelease(dv->texPipeline); dv->texPipeline = nullptr; }
    for (int b = 0; b < 4; ++b)
        for (int p = 0; p < 2; ++p)
            if (dv->meshPipeline[b][p]) { wgpuRenderPipelineRelease(dv->meshPipeline[b][p]); dv->meshPipeline[b][p] = nullptr; }
}

static void create_2d_pipelines(Device* dv, WGPUTextureFormat format) {
    release_2d_pipelines(dv);
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;                // 直通 alpha 合成
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.writeMask = WGPUColorWriteMask_All;
    target.blend = &blend;

    if (dv->solidShader && dv->solidPl) {
        WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT; frag.module = dv->solidShader; frag.entryPoint = SV("fs"); frag.targetCount = 1; frag.targets = &target;
        WGPUVertexState vert = WGPU_VERTEX_STATE_INIT; vert.module = dv->solidShader; vert.entryPoint = SV("vs");
        WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rp.layout = dv->solidPl; rp.vertex = vert; rp.fragment = &frag; rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        dv->solidPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
    }
    if (dv->texShader && dv->texPl) {
        WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT; frag.module = dv->texShader; frag.entryPoint = SV("fs"); frag.targetCount = 1; frag.targets = &target;
        WGPUVertexState vert = WGPU_VERTEX_STATE_INIT; vert.module = dv->texShader; vert.entryPoint = SV("vs");
        WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rp.layout = dv->texPl; rp.vertex = vert; rp.fragment = &frag; rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        dv->texPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
    }
    if (dv->meshShader && dv->meshPl) {
        /* 顶点布局：交错 stride 48B（pos.xy | uv.xy | color.rgba | darkColor.rgba），与 CPU 侧 MeshVertex 一一对应。 */
        WGPUVertexAttribute mattrs[4] = {};
        mattrs[0].format = WGPUVertexFormat_Float32x2; mattrs[0].offset = 0;  mattrs[0].shaderLocation = 0;
        mattrs[1].format = WGPUVertexFormat_Float32x2; mattrs[1].offset = 8;  mattrs[1].shaderLocation = 1;
        mattrs[2].format = WGPUVertexFormat_Float32x4; mattrs[2].offset = 16; mattrs[2].shaderLocation = 2;
        mattrs[3].format = WGPUVertexFormat_Float32x4; mattrs[3].offset = 32; mattrs[3].shaderLocation = 3;
        WGPUVertexBufferLayout mvbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        mvbl.stepMode = WGPUVertexStepMode_Vertex; mvbl.arrayStride = 48;
        mvbl.attributeCount = 4; mvbl.attributes = mattrs;
        for (int blendIdx = 0; blendIdx < 4; ++blendIdx) {
            for (int pma = 0; pma < 2; ++pma) {
                WGPUBlendState mb = WGPU_BLEND_STATE_INIT;
                if (blendIdx == 1) {                        /* additive */
                    mb.color.srcFactor = WGPUBlendFactor_One; mb.color.dstFactor = WGPUBlendFactor_One;
                } else {                                    /* normal（含 multiply/screen 的回退，见 render_impl） */
                    /* pma=1：纹理已预乘 → 源因子必须 ONE，否则 alpha 被乘两次（边缘发暗） */
                    mb.color.srcFactor = pma ? WGPUBlendFactor_One : WGPUBlendFactor_SrcAlpha;
                    mb.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
                }
                mb.color.operation = WGPUBlendOperation_Add;
                mb.alpha.srcFactor = WGPUBlendFactor_One; mb.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
                mb.alpha.operation = WGPUBlendOperation_Add;
                WGPUColorTargetState mt = WGPU_COLOR_TARGET_STATE_INIT;
                mt.format = format; mt.writeMask = WGPUColorWriteMask_All;
                mt.blend = &mb;
                WGPUFragmentState mfrag = WGPU_FRAGMENT_STATE_INIT;
                mfrag.module = dv->meshShader; mfrag.entryPoint = SV("fs"); mfrag.targetCount = 1; mfrag.targets = &mt;
                WGPUVertexState mvert = WGPU_VERTEX_STATE_INIT;
                mvert.module = dv->meshShader; mvert.entryPoint = SV("vs");
                mvert.bufferCount = 1; mvert.buffers = &mvbl;
                WGPURenderPipelineDescriptor mrp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
                mrp.layout = dv->meshPl; mrp.vertex = mvert; mrp.fragment = &mfrag;
                mrp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
                dv->meshPipeline[blendIdx][pma] = wgpuDeviceCreateRenderPipeline(dv->device, &mrp);
            }
        }
    }
    dv->pipelineFormat = format;
}

/* 保证当前目标格式的 2D 管线已建好（离屏 RGBA8 ↔ 窗口 swapchain 格式切换时重建）。 */
static void ensure_2d_pipelines(Device* dv, WGPUTextureFormat format) {
    if (dv->pipelineFormat == format) return;
    create_2d_pipelines(dv, format);
}

/* ---- M2 3D 基础（plans/18-pbr-3d.md §4.2）：depth 纹理 / unlit3d 管线 / 离屏回读 ---- */
static bool ensure_depth(Device* dv, int32_t w, int32_t h) {
    if (!dv || w <= 0 || h <= 0) return false;
    if (dv->depthTex && dv->depthW == w && dv->depthH == h) return true;
    if (dv->depthView) { wgpuTextureViewRelease(dv->depthView); dv->depthView = nullptr; }
    if (dv->depthTex) { wgpuTextureRelease(dv->depthTex); dv->depthTex = nullptr; }
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_RenderAttachment;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ (uint32_t)w, (uint32_t)h, 1 };
    td.format = WGPUTextureFormat_Depth32Float;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    dv->depthTex = wgpuDeviceCreateTexture(dv->device, &td);
    if (!dv->depthTex) return false;
    dv->depthView = wgpuTextureCreateView(dv->depthTex, nullptr);
    dv->depthW = w; dv->depthH = h;
    return dv->depthView != nullptr;
}

/* pbr 管线：按**目标颜色格式**创建（swapchain/离屏各一），depth32 + Cook-Torrance。 */
/* 材质模型 → fragment entry point / 诊断名（与 assets/builtin/shaders/pbr/ 的入口名一致） */
static const char* const kMatFsEntry[10] = {
    "fs_standard", "fs_physical", "fs_basic", "fs_lambert", "fs_phong",
    "fs_toon", "fs_normal", "fs_matcap", "fs_depth", "fs_shadow",
};
static const char* const kMatName[10] = {
    "Standard", "Physical", "Basic", "Lambert", "Phong", "Toon", "Normal", "Matcap", "Depth", "Shadow",
};
/* 顶点布局 → vertex entry（plans/20）：layout 0 = 旧 48B（vs），layout 1 = 56B COLOR_0/TEXCOORD_1（vs1）。 */
static const char* const kMatVsEntry[2] = { "vs", "vs1" };

static void release_material_pipelines(Device* dv) {
    for (int m = 0; m < 10; ++m)
        for (int b = 0; b < 2; ++b)
            for (int c = 0; c < 3; ++c)
                for (int mir = 0; mir < 2; ++mir)
                    for (int lay = 0; lay < 2; ++lay)
                        if (dv->p3dMatPipeline[m][b][c][mir][lay]) {
                            wgpuRenderPipelineRelease(dv->p3dMatPipeline[m][b][c][mir][lay]);
                            dv->p3dMatPipeline[m][b][c][mir][lay] = nullptr;
                        }
}

static void ensure_3d_pipelines(Device* dv, WGPUTextureFormat format) {
    if (!dv) return;
    const bool have_res = dv->p3dShader && dv->p3dPl && dv->p3dCamBgl && dv->p3dDrawBgl;
    if (have_res && dv->p3dFormat == format) return;
    if (!dv->p3dShader) {
        const char* wgsl = mq_builtin_shader_text("pbr");
        if (!wgsl) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::print(stderr, "gfx: 内建 shader 'pbr' 不可用 → 3D 网格不渲染（MAQUE_SHADER_PBR=ON 可编回）\n");
            }
            return;
        }
        dv->p3dShader = make_shader(dv->device, wgsl);
        if (!dv->p3dShader) return;
        {
            WGPUBindGroupLayoutEntry e[3] = {};
            e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 816;
            e[1].binding = 1; e[1].visibility = WGPUShaderStage_Fragment;
            e[1].texture.sampleType = WGPUTextureSampleType_Depth;
            e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment;
            e[2].sampler.type = WGPUSamplerBindingType_Comparison;
            WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 3; bd.entries = e;
            dv->p3dCamBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        }
        /* ---- 阴影资源（plans/18 §4.2 / R2/R19 修复）----
         * shadowTex 缺失时仍必须建好 dummyDepth 与比较采样器：
         *  · dummyDepth 给"主 pass 采样阴影图"的同时避免同 pass 读写的 1×1 深度占位（既有的正确设计）；
         *  · comparison sampler 是 group0 binding2 的唯一合法类型（Filtering sampler 绑上去会 bind-group 校验失败）。
         * 因此三者分开建；任一下降为 null 时 render_3d_only 会自动关闭 shadow（shadow_params.z=0），而不是绑错类型。 */
        if (!dv->shadowTex) {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
            td.dimension = WGPUTextureDimension_2D;
            td.size = WGPUExtent3D{ 1024, 1024, 1 };
            td.format = WGPUTextureFormat_Depth32Float;
            td.mipLevelCount = 1; td.sampleCount = 1;
            dv->shadowTex = wgpuDeviceCreateTexture(dv->device, &td);
            if (dv->shadowTex) dv->shadowView = wgpuTextureCreateView(dv->shadowTex, nullptr);
        }
        if (!dv->dummyDepth) {
            WGPUTextureDescriptor dtd = WGPU_TEXTURE_DESCRIPTOR_INIT;
            dtd.usage = WGPUTextureUsage_TextureBinding;
            dtd.dimension = WGPUTextureDimension_2D;
            dtd.size = WGPUExtent3D{ 1, 1, 1 };
            dtd.format = WGPUTextureFormat_Depth32Float;
            dtd.mipLevelCount = 1; dtd.sampleCount = 1;
            dv->dummyDepth = wgpuDeviceCreateTexture(dv->device, &dtd);
            if (dv->dummyDepth) dv->dummyDepthView = wgpuTextureCreateView(dv->dummyDepth, nullptr);
        }
        if (!dv->shadowSampler) {
            WGPUSamplerDescriptor smp = WGPU_SAMPLER_DESCRIPTOR_INIT;
            smp.addressModeU = WGPUAddressMode_ClampToEdge;
            smp.addressModeV = WGPUAddressMode_ClampToEdge;
            smp.addressModeW = WGPUAddressMode_ClampToEdge;
            smp.magFilter = WGPUFilterMode_Linear;
            smp.minFilter = WGPUFilterMode_Linear;
            smp.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            smp.compare = WGPUCompareFunction_LessEqual;
            smp.maxAnisotropy = 1;
            dv->shadowSampler = wgpuDeviceCreateSampler(dv->device, &smp);
        }
        {
            /* group1：材质 uniform（288B = 72 floats）+ **15 张 sampled 贴图** + 1 sampler（plans/19/20）。
             * 绑定布局：0=uniform，1=albedo，2=sampler，3=mr，4=ao，5=normal，6=emissive，7=matcap，8=gradient，
             * 9=clearcoat_normal（旧 8 槽），10..16 = KHR 扩展 7 槽（textures3[0..6]）。
             * 采样器上限：group1 的 15 张 + group0 的 1 张 shadow depth = 16，
             * 正好等于 WebGPU 默认 maxSampledTexturesPerShaderStage（16）；再加就要裁剪绑定。 */
            WGPUBindGroupLayoutEntry e[17] = {};
            e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 288;
            for (int ti = 1; ti <= 16; ++ti) {
                if (ti == 2) continue;                       /* binding2 = sampler */
                e[ti].binding = (uint32_t)ti;
                e[ti].visibility = WGPUShaderStage_Fragment;
                e[ti].texture.sampleType = WGPUTextureSampleType_Float;
                e[ti].texture.viewDimension = WGPUTextureViewDimension_2D;
            }
            e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment;
            e[2].sampler.type = WGPUSamplerBindingType_Filtering;
            WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 17; bd.entries = e;
            dv->p3dDrawBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        }
        WGPUBindGroupLayout bl[2] = { dv->p3dCamBgl, dv->p3dDrawBgl };
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl.bindGroupLayoutCount = 2; pl.bindGroupLayouts = bl;
        dv->p3dPl = wgpuDeviceCreatePipelineLayout(dv->device, &pl);
    }
    if (!dv->p3dShader || !dv->p3dPl) return;
    if (dv->p3dFormat != format) {
        release_material_pipelines(dv);
        dv->p3dFormat = format;
    }
    if (dv->shadowTex && dv->dummyDepthView && dv->shadowSampler) {
        WGPUDepthStencilState sds = WGPU_DEPTH_STENCIL_STATE_INIT;
        sds.format = WGPUTextureFormat_Depth32Float;
        sds.depthWriteEnabled = WGPUOptionalBool_True;
        sds.depthCompare = WGPUCompareFunction_Less;
        /* 顶点布局两套（layout0 48B / layout1 56B）：阴影只读 pos@0 + uv0@40，两个布局下偏移相同、stride 不同。
         * 阴影 fragment 要用 uv 采样 albedo 做 MASK alpha test（见 fs_shadow_alpha）。 */
        for (int lay = 0; lay < 2; ++lay) {
            if (dv->shadowPipeline[lay]) continue;
            WGPUVertexAttribute sattrs[4] = {};
            sattrs[0].format = WGPUVertexFormat_Float32x3; sattrs[0].offset = 0;  sattrs[0].shaderLocation = 0;   /* pos */
            uint32_t sattr_count = 2;
            const char* s_vs = "vs_shadow";
            if (lay == 0) {
                sattrs[1].format = WGPUVertexFormat_Float32x2; sattrs[1].offset = 40; sattrs[1].shaderLocation = 2;  /* uv0 */
            } else {
                /* 布局 v1：阴影 alpha mask 也需要顶点色 alpha 与 TEXCOORD_1（baseColor 贴图可能用 uv1） */
                sattrs[1].format = WGPUVertexFormat_Float32x4; sattrs[1].offset = 24; sattrs[1].shaderLocation = 1;  /* color */
                sattrs[2].format = WGPUVertexFormat_Float32x2; sattrs[2].offset = 40; sattrs[2].shaderLocation = 2;  /* uv0 */
                sattrs[3].format = WGPUVertexFormat_Float32x2; sattrs[3].offset = 48; sattrs[3].shaderLocation = 3;  /* uv1 */
                sattr_count = 4;
                s_vs = "vs_shadow1";
            }
            WGPUVertexBufferLayout svbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            svbl.stepMode = WGPUVertexStepMode_Vertex;
            svbl.arrayStride = (lay == 1) ? 56u : 48u;
            svbl.attributeCount = sattr_count; svbl.attributes = sattrs;
            WGPUVertexState svs = WGPU_VERTEX_STATE_INIT;
            svs.module = dv->p3dShader; svs.entryPoint = SV(s_vs);
            svs.bufferCount = 1; svs.buffers = &svbl;
            /* 阴影 pass 只有深度附件（colorAttachmentCount=0）。WebGPU 允许 fragment 阶段 targetCount=0
             * （无颜色输出、仅用于 discard）：glTF alphaMode=MASK 的镂空几何要靠它执行 alpha test，
             * 否则树叶/栅栏/发卡的投影会是整块实心（R3）。若该后端拒绝零 target，回退 depth-only 并提示。 */
            WGPUFragmentState sfrag = WGPU_FRAGMENT_STATE_INIT;
            sfrag.module = dv->p3dShader; sfrag.entryPoint = SV("fs_shadow_alpha");
            sfrag.targetCount = 0; sfrag.targets = nullptr;
            WGPURenderPipelineDescriptor srp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            srp.layout = dv->p3dPl;
            srp.vertex = svs;
            srp.fragment = &sfrag;                 /* 首试：depth-only + MASK alpha discard */
            srp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
            srp.primitive.frontFace = WGPUFrontFace_CCW;    /* 阴影 pass 不剔面，显式写清 WebGPU 默认 CCW */
            srp.primitive.cullMode = WGPUCullMode_None;
            srp.depthStencil = &sds;
            dv->shadowPipeline[lay] = wgpuDeviceCreateRenderPipeline(dv->device, &srp);
            if (!dv->shadowPipeline[lay]) {
                static bool warned_shadow_alpha = false;
                if (!warned_shadow_alpha) {
                    warned_shadow_alpha = true;
                    std::print(stderr, "gfx: 阴影 alpha 管线创建失败（fragment 'fs_shadow_alpha' 或 targetCount=0 不受支持）"
                                       "→ 回退 depth-only 阴影（MASK 镂空材质的投影会变成实心）\n");
                }
                srp.fragment = nullptr;
                dv->shadowPipeline[lay] = wgpuDeviceCreateRenderPipeline(dv->device, &srp);
                if (!dv->shadowPipeline[lay]) {
                    static bool warned_shadow_off = false;
                    if (!warned_shadow_off) {
                        warned_shadow_off = true;
                        std::print(stderr, "gfx: depth-only 阴影管线也创建失败（布局 {}，vs_shadow 输出了 varying？）"
                                           "→ 该顶点布局不投阴影\n", lay);
                    }
                }
            }
        }
    }
}

/* 按材质模型 / 透明混合 / 面朝向 / 镜像（负行列式）/ 顶点布局懒创建 3D 管线（plans/19/20）。
 * shading_model 直接选择 fragment entry point；cull 由 side 决定（Front 剔背面 / Back 剔正面 / Double 不剔）。
 * mirrored = model 3×3 det<0：镜像变换把三角形绕序从 CCW 翻成 CW → 管线必须显式 frontFace=CW，
 * 否则 WebGPU 默认 CCW 会把正反面判反（side=Front 时正面被剔掉）。
 * layout = 网格顶点布局：0=48B 旧（vs） / 1=56B COLOR_0+TEXCOORD_1（vs1）。 */
static WGPURenderPipeline get_material_pipeline(Device* dv, int model, bool blend, int side, bool mirrored,
                                                int layout) {
    if (!dv || !dv->p3dShader || !dv->p3dPl) return nullptr;
    model = (model < 0) ? 0 : (model > 9 ? 9 : model);
    side = (side < 0) ? 2 : (side > 2 ? 2 : side);
    layout = (layout == 1) ? 1 : 0;
    const int b = blend ? 1 : 0;
    const int mir = mirrored ? 1 : 0;
    if (WGPURenderPipeline& cached = dv->p3dMatPipeline[model][b][side][mir][layout]; cached) return cached;
    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = dv->p3dFormat; ct.writeMask = WGPUColorWriteMask_All;
    WGPUBlendState blend_state = WGPU_BLEND_STATE_INIT;
    blend_state.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend_state.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend_state.color.operation = WGPUBlendOperation_Add;
    blend_state.alpha.srcFactor = WGPUBlendFactor_One;
    blend_state.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend_state.alpha.operation = WGPUBlendOperation_Add;
    if (blend) ct.blend = &blend_state;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = blend ? WGPUOptionalBool_False : WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Less;
    WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
    frag.module = dv->p3dShader; frag.entryPoint = SV(kMatFsEntry[model]);
    frag.targetCount = 1; frag.targets = &ct;
    /* 顶点布局：layout0 = pos@0/normal@12/uv0@40（stride 48，tangent@24 未用）；
     *           layout1 = 再加 color@24(RGBA)/uv1@48（stride 56，对应 vs1 的 @location(3)/(4)）。 */
    WGPUVertexAttribute attrs[5] = {};
    attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;   /* pos */
    attrs[1].format = WGPUVertexFormat_Float32x3; attrs[1].offset = 12; attrs[1].shaderLocation = 1;   /* normal */
    attrs[2].format = WGPUVertexFormat_Float32x2; attrs[2].offset = 40; attrs[2].shaderLocation = 2;   /* uv0 */
    attrs[3].format = WGPUVertexFormat_Float32x4; attrs[3].offset = 24; attrs[3].shaderLocation = 3;   /* color */
    attrs[4].format = WGPUVertexFormat_Float32x2; attrs[4].offset = 48; attrs[4].shaderLocation = 4;   /* uv1 */
    WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbl.stepMode = WGPUVertexStepMode_Vertex;
    vbl.arrayStride = (layout == 1) ? 56u : 48u;
    vbl.attributeCount = (layout == 1) ? 5u : 3u;
    vbl.attributes = attrs;
    WGPUVertexState vs = WGPU_VERTEX_STATE_INIT;
    vs.module = dv->p3dShader; vs.entryPoint = SV(kMatVsEntry[layout]);
    vs.bufferCount = 1; vs.buffers = &vbl;
    WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rp.layout = dv->p3dPl; rp.vertex = vs; rp.fragment = &frag;
    rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    /* WebGPU 约定：frontFace 默认 CCW（glTF 正面的绕序也是 CCW）；负行列式 model 翻转绕序 →
     * 显式改成 CW，让 cullMode 的"正面/背面"语义仍然成立。 */
    rp.primitive.frontFace = mirrored ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    rp.primitive.cullMode = (side == 0) ? WGPUCullMode_Back
                          : (side == 1) ? WGPUCullMode_Front : WGPUCullMode_None;
    rp.depthStencil = &ds;
    WGPURenderPipeline pipe = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
    dv->p3dMatPipeline[model][b][side][mir][layout] = pipe;
    if (!pipe) {
        if (layout == 1) {
            if (!dv->p3dMatLayoutWarned[1]) {
                dv->p3dMatLayoutWarned[1] = true;
                std::print(stderr, "gfx: 顶点布局 v1 管线创建失败（shader entry 'vs1' 缺失/校验失败）"
                                   "→ vertex_layout=1 的网格不渲染（布局 0 不受影响）\n");
            }
        } else if (!dv->p3dMatWarned[model]) {
            dv->p3dMatWarned[model] = true;
            std::print(stderr, "gfx: 材质模型 '{}' 管线创建失败（shader entry '{}' 缺失/校验失败）→ 该模型不渲染\n",
                       kMatName[model], kMatFsEntry[model]);
        }
    }
    return pipe;
}

/* 离屏 RGBA8 目标 → 像素缓冲（3D + 2D 合成后回读；--shot/CI 用）。 */
static int32_t readback_rgba8(Device* dv, WGPUTexture tex, int32_t W, int32_t H,
                              uint8_t* out, uint32_t out_size) {
    if (!dv || !tex || !out) return 1;
    if ((uint64_t)W * H * 4 > out_size) return 2;
    const uint32_t row = (uint32_t)W * 4u;
    const uint32_t padded = (row + 255u) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    bd.size = (uint64_t)padded * (uint64_t)H;
    WGPUBuffer rb = wgpuDeviceCreateBuffer(dv->device, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = rb; dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = (uint32_t)H;
    WGPUExtent3D ext = WGPUExtent3D{ (uint32_t)W, (uint32_t)H, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cb);
    wgpuCommandBufferRelease(cb); wgpuCommandEncoderRelease(enc);
    WGPUMapAsyncStatus st = (WGPUMapAsyncStatus)0;
    WGPUBufferMapCallbackInfo ci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    ci.mode = WGPUCallbackMode_AllowProcessEvents; ci.callback = map_cb; ci.userdata1 = &st;
    wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, bd.size, ci);
    poll(dv->instance, [&]{ return (uint32_t)st != 0; });
    if (st == WGPUMapAsyncStatus_Success) {
        const uint8_t* m = (const uint8_t*)wgpuBufferGetConstMappedRange(rb, 0, bd.size);
        for (int32_t y = 0; y < H; ++y) std::memcpy(out + (size_t)y * row, m + (size_t)y * padded, row);
        wgpuBufferUnmap(rb);
    }
    wgpuBufferRelease(rb);
    return st == WGPUMapAsyncStatus_Success ? 0 : 3;
}

/* 1×1 RGBA8 缺省纹理（白 / 中性法线等），CopyDst 写入后用于绑定。 */
static void make_1x1_texture(Device* dv, WGPUTexture* out_tex, WGPUTextureView* out_view,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ 1, 1, 1 };
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(dv->device, &td);
    if (!tex) return;
    *out_tex = tex;
    *out_view = wgpuTextureCreateView(tex, nullptr);
    const uint8_t px[4] = { r, g, b, a };
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = 4; layout.rowsPerImage = 1;
    WGPUExtent3D sz = WGPUExtent3D{ 1, 1, 1 };
    wgpuQueueWriteTexture(dv->queue, &dst, px, sizeof px, &layout, &sz);
}

/* PBR 缺省 albedo/法线：1×1 白 + 1×1 中性法线。 */
static void create_white_texture(Device* dv) {
    make_1x1_texture(dv, &dv->whiteTex, &dv->whiteTexView, 255, 255, 255, 255);
    make_1x1_texture(dv, &dv->neutralNormalTex, &dv->neutralNormalView, 128, 128, 255, 255);
}

static Device* create_device(std::string* out_backend) {
    Device* dv = new Device();
    WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    dv->instance = wgpuCreateInstance(&idesc);
    if (!dv->instance) { delete dv; return nullptr; }

    WGPURequestAdapterOptions aopts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_AllowProcessEvents; aci.callback = req_adapter_cb; aci.userdata1 = &dv->adapter;
    aopts.backendType = WGPUBackendType_Undefined;
    wgpuInstanceRequestAdapter(dv->instance, &aopts, aci);
    if (!poll(dv->instance, [&]{ return dv->adapter != nullptr; })) { delete dv; return nullptr; }

    {
        WGPUAdapterInfo inf = WGPU_ADAPTER_INFO_INIT;
        wgpuAdapterGetInfo(dv->adapter, &inf);
        dv->backend = std::string(inf.device.data ? inf.device.data : "", inf.device.length);
        wgpuAdapterInfoFreeMembers(inf);
        if (out_backend) *out_backend = dv->backend;
    }

    WGPUDeviceDescriptor ddesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    ddesc.uncapturedErrorCallbackInfo.callback = on_uncaptured;
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_AllowProcessEvents; dci.callback = req_device_cb; dci.userdata1 = &dv->device;
    wgpuAdapterRequestDevice(dv->adapter, &ddesc, dci);
    if (!poll(dv->instance, [&]{ return dv->device != nullptr; })) { delete dv; return nullptr; }

    dv->queue = wgpuDeviceGetQueue(dv->device);

    /* 内建 shader（按名字查注册表）：某个被剔除（-DMAQUE_SHADER_<NAME>=OFF）或解压失败时
     * 打印一行说明、跳过该管线 —— 渲染循环对空管线会跳过对应批次（不崩、不静默）。 */
    const char* solid_wgsl = mq_builtin_shader_text("solid");
    const char* tex_wgsl = mq_builtin_shader_text("textured");
    if (!solid_wgsl) std::print(stderr, "gfx: 内建 shader 'solid' 不可用 → 无贴图图元不渲染"
                                       "（剔除开关 MAQUE_SHADER_SOLID=ON 可编回）\n");
    if (!tex_wgsl) std::print(stderr, "gfx: 内建 shader 'textured' 不可用 → 贴图图元不渲染"
                                     "（剔除开关 MAQUE_SHADER_TEXTURED=ON 可编回）\n");

    if (solid_wgsl) dv->solidShader = make_shader(dv->device, solid_wgsl);
    if (dv->solidShader) {
        WGPUBindGroupLayoutEntry e[2] = {};
        e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex; e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 48;
        e[1].binding = 1; e[1].visibility = WGPUShaderStage_Vertex; e[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 2; bd.entries = e;
        dv->solidBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; pl.bindGroupLayoutCount = 1; pl.bindGroupLayouts = &dv->solidBgl;
        dv->solidPl = wgpuDeviceCreatePipelineLayout(dv->device, &pl);
    }
    if (tex_wgsl) dv->texShader = make_shader(dv->device, tex_wgsl);
    if (dv->texShader) {
        WGPUBindGroupLayoutEntry e[4] = {};
        e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex; e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 48;
        e[1].binding = 1; e[1].visibility = WGPUShaderStage_Vertex; e[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment; e[2].texture.sampleType = WGPUTextureSampleType_Float; e[2].texture.viewDimension = WGPUTextureViewDimension_2D;
        e[3].binding = 3; e[3].visibility = WGPUShaderStage_Fragment; e[3].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 4; bd.entries = e;
        dv->texBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; pl.bindGroupLayoutCount = 1; pl.bindGroupLayouts = &dv->texBgl;
        dv->texPl = wgpuDeviceCreatePipelineLayout(dv->device, &pl);
    }
    /* 全局采样器与 2D 贴图管线无关地创建：PBR 材质 bind group 在"材质无任何贴图"时回退到 dv->sampler，
     * 若只在 textured shader 存在时才建，`MAQUE_SHADER_TEXTURED=OFF` 的构建里 3D 材质 bind group 会拿到
     * null sampler（创建失败）；且 material_sampler() 的兜底也依赖它。 */
    {
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.magFilter = WGPUFilterMode_Linear; sd.minFilter = WGPUFilterMode_Linear; sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
        sd.addressModeU = WGPUAddressMode_ClampToEdge; sd.addressModeV = WGPUAddressMode_ClampToEdge; sd.addressModeW = WGPUAddressMode_ClampToEdge;
        dv->sampler = wgpuDeviceCreateSampler(dv->device, &sd);
        /* 最近邻采样器：字形/像素画 1:1 呈现（线性过滤会糊掉像素网格） */
        WGPUSamplerDescriptor nd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        nd.magFilter = WGPUFilterMode_Nearest; nd.minFilter = WGPUFilterMode_Nearest;
        nd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
        nd.addressModeU = WGPUAddressMode_ClampToEdge; nd.addressModeV = WGPUAddressMode_ClampToEdge;
        nd.addressModeW = WGPUAddressMode_ClampToEdge;
        dv->samplerNearest = wgpuDeviceCreateSampler(dv->device, &nd);
    }

    /* ---- 网格管线（mesh）：顶点缓冲 + 索引 + 页纹理 + 逐顶点颜色（骨骼动画/任意三角网格）---- */
    const char* mesh_wgsl = mq_builtin_shader_text("mesh");
    if (!mesh_wgsl) std::print(stderr, "gfx: 内建 shader 'mesh' 不可用 → 网格图元（spine 等）不渲染"
                                       "（剔除开关 MAQUE_SHADER_MESH=ON 可编回）\n");
    if (mesh_wgsl) dv->meshShader = make_shader(dv->device, mesh_wgsl);
    if (dv->meshShader) {
        WGPUBindGroupLayoutEntry me[3] = {};
        me[0].binding = 0; me[0].visibility = WGPUShaderStage_Vertex;   me[0].buffer.type = WGPUBufferBindingType_Uniform; me[0].buffer.minBindingSize = 48;
        me[1].binding = 1; me[1].visibility = WGPUShaderStage_Fragment; me[1].texture.sampleType = WGPUTextureSampleType_Float; me[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        me[2].binding = 2; me[2].visibility = WGPUShaderStage_Fragment; me[2].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor mbd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; mbd.entryCount = 3; mbd.entries = me;
        dv->meshBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &mbd);
        WGPUPipelineLayoutDescriptor mpl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; mpl.bindGroupLayoutCount = 1; mpl.bindGroupLayouts = &dv->meshBgl;
        dv->meshPl = wgpuDeviceCreatePipelineLayout(dv->device, &mpl);
    }
    /* 初始 2D 管线 = **离屏 RGBA8** 目标；窗口直接呈现在 attach_window 时按 swapchain 格式重建。 */
    create_white_texture(dv);
    create_2d_pipelines(dv, WGPUTextureFormat_RGBA8Unorm);
    return dv;
}

static void destroy_device(Device* dv) {
    if (!dv) return;
    for (auto* m : dv->meshes) {
        if (!m) continue;
        if (m->vbo) wgpuBufferRelease(m->vbo);
        if (m->ibo) wgpuBufferRelease(m->ibo);
        delete m;
    }
    for (auto* t : dv->textures) {
        if (!t) continue;                  /* 已由 destroy_texture 释放（槽位保留但指针为空） */
        if (t->sampler) wgpuSamplerRelease(t->sampler);
        if (t->view) wgpuTextureViewRelease(t->view);
        if (t->tex) wgpuTextureRelease(t->tex);
        delete t;
    }
    if (dv->surfaceView) wgpuTextureViewRelease(dv->surfaceView);
    if (dv->surfaceTex) wgpuTextureRelease(dv->surfaceTex);
    if (dv->surface) wgpuSurfaceRelease(dv->surface);
    if (dv->sampler) wgpuSamplerRelease(dv->sampler);
    if (dv->samplerNearest) wgpuSamplerRelease(dv->samplerNearest);
    if (dv->whiteTexView) wgpuTextureViewRelease(dv->whiteTexView);
    if (dv->whiteTex) wgpuTextureRelease(dv->whiteTex);
    if (dv->neutralNormalView) wgpuTextureViewRelease(dv->neutralNormalView);
    if (dv->neutralNormalTex) wgpuTextureRelease(dv->neutralNormalTex);
    /* M2 3D 资源 */
    if (dv->depthView) wgpuTextureViewRelease(dv->depthView);
    if (dv->depthTex) wgpuTextureRelease(dv->depthTex);
    release_material_pipelines(dv);
    if (dv->shadowPipeline[0]) wgpuRenderPipelineRelease(dv->shadowPipeline[0]);
    if (dv->shadowPipeline[1]) wgpuRenderPipelineRelease(dv->shadowPipeline[1]);
    if (dv->shadowSampler) wgpuSamplerRelease(dv->shadowSampler);
    if (dv->dummyDepthView) wgpuTextureViewRelease(dv->dummyDepthView);
    if (dv->dummyDepth) wgpuTextureRelease(dv->dummyDepth);
    if (dv->shadowView) wgpuTextureViewRelease(dv->shadowView);
    if (dv->shadowTex) wgpuTextureRelease(dv->shadowTex);
    if (dv->p3dPl) wgpuPipelineLayoutRelease(dv->p3dPl);
    if (dv->p3dCamBgl) wgpuBindGroupLayoutRelease(dv->p3dCamBgl);
    if (dv->p3dDrawBgl) wgpuBindGroupLayoutRelease(dv->p3dDrawBgl);
    if (dv->p3dShader) wgpuShaderModuleRelease(dv->p3dShader);
    for (int b = 0; b < 4; ++b)
        for (int p = 0; p < 2; ++p)
            if (dv->meshPipeline[b][p]) wgpuRenderPipelineRelease(dv->meshPipeline[b][p]);
    if (dv->meshPl) wgpuPipelineLayoutRelease(dv->meshPl);
    if (dv->meshBgl) wgpuBindGroupLayoutRelease(dv->meshBgl);
    if (dv->meshShader) wgpuShaderModuleRelease(dv->meshShader);
    if (dv->solidPipeline) wgpuRenderPipelineRelease(dv->solidPipeline);
    if (dv->solidPl) wgpuPipelineLayoutRelease(dv->solidPl);
    if (dv->solidBgl) wgpuBindGroupLayoutRelease(dv->solidBgl);
    if (dv->solidShader) wgpuShaderModuleRelease(dv->solidShader);
    if (dv->texPipeline) wgpuRenderPipelineRelease(dv->texPipeline);
    if (dv->texPl) wgpuPipelineLayoutRelease(dv->texPl);
    if (dv->texBgl) wgpuBindGroupLayoutRelease(dv->texBgl);
    if (dv->texShader) wgpuShaderModuleRelease(dv->texShader);
    if (dv->queue) wgpuQueueRelease(dv->queue);
    if (dv->device) wgpuDeviceRelease(dv->device);
    if (dv->adapter) wgpuAdapterRelease(dv->adapter);
    if (dv->instance) wgpuInstanceRelease(dv->instance);
    delete dv;
}

/* 实例数据：pos(8) + radius(4) + rot(4) + color(16) = 32B（16 字节对齐，WGSL 数组步长合法）。 */
/* 与 WGSL `struct Inst` 严格对齐（vec2f/f32/f32/vec4f/vec4f/vec2f/f32/f32，数组 stride=64） */
struct GpuInst {
    float pos[2];      /* 0  */
    float radius;      /* 8  */
    float rot;         /* 12 */
    float color[4];    /* 16 */
    float uv[4];       /* 32 */
    float half[2];     /* 48（<=0 → 用 radius） */
    float shape;       /* 56（0=圆形，1=矩形） */
    float snap;        /* 60（1=左上角吸附整数像素） */
};
static_assert(sizeof(GpuInst) == 64, "GpuInst 必须与 WGSL Inst 布局一致（stride 64）");

/* 批处理（docs/13 §pass 层）：先按 (layer, z, 原始序) 稳定排序，再按"管线 + 贴图"分批。 */
struct Batch { int texture; bool nearest; uint32_t first, count; };

static WGPUBuffer make_ibuf(Device* dv, const std::vector<GpuInst>& v) {
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    bd.size = (uint64_t)(v.empty() ? 1 : v.size()) * sizeof(GpuInst);
    WGPUBuffer b = wgpuDeviceCreateBuffer(dv->device, &bd);
    if (!v.empty()) wgpuQueueWriteBuffer(dv->queue, b, 0, v.data(), v.size() * sizeof(GpuInst));
    return b;
}
static void begin_pass(WGPUCommandEncoder enc, WGPUTextureView view, WGPULoadOp load, const float bg[4], WGPURenderPassEncoder* out) {
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = view; ca.loadOp = load; ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = WGPUColor{ bg[0], bg[1], bg[2], bg[3] };
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    *out = wgpuCommandEncoderBeginRenderPass(enc, &rp);
}

} // namespace maque::gfx

using namespace maque::gfx;

extern "C" {

static void* r_create(const char** out_backend) {
    std::string b;
    Device* d = create_device(&b);
    if (out_backend) *out_backend = d ? d->backend.c_str() : nullptr;
    return d;
}
static void r_destroy(void* self) { destroy_device((Device*)self); }

static int32_t r_create_texture(void* self, const uint8_t* rgba, int32_t w, int32_t h) {
    auto* dv = (Device*)self;
    if (!dv || w <= 0 || h <= 0 || !rgba) return -1;
    auto* t = new Texture();
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ (uint32_t)w, (uint32_t)h, 1 };
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1; td.sampleCount = 1;
    t->tex = wgpuDeviceCreateTexture(dv->device, &td);
    t->view = wgpuTextureCreateView(t->tex, nullptr);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = t->tex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = (uint32_t)w * 4; layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D sz = WGPUExtent3D{ (uint32_t)w, (uint32_t)h, 1 };
    wgpuQueueWriteTexture(dv->queue, &dst, rgba, (size_t)w * h * 4, &layout, &sz);
    dv->textures.push_back(t);
    return (int32_t)dv->textures.size() - 1;
}
static void r_destroy_texture(void* self, int32_t id) {
    auto* dv = (Device*)self;
    if (!dv || id < 0 || id >= (int)dv->textures.size()) return;
    Texture* t = dv->textures[(size_t)id];
    if (t) {
        /* per-texture sampler（create_texture2 每张贴图一个）也必须释放，否则 destroy 一次漏一个 GPU 对象。 */
        if (t->sampler) wgpuSamplerRelease(t->sampler);
        if (t->view) wgpuTextureViewRelease(t->view);
        if (t->tex) wgpuTextureRelease(t->tex);
        delete t;
    }
    dv->textures[(size_t)id] = nullptr;      /* 槽位保留（id 稳定，避免悬空引用） */
}

static int32_t r_has_texture(void* self, int32_t id) {
    auto* dv = (Device*)self;
    if (!dv || id < 0 || id >= (int)dv->textures.size()) return 0;
    return dv->textures[(size_t)id] != nullptr ? 1 : 0;   /* 已 destroy 的槽位 → 0（调用方据此丢弃悬空句柄） */
}

/* ============================================================================
 * 3D / PBR 资源层（plans/18-pbr-3d.md §2.2/§4）
 *   M0（本次）：纹理 v2（sRGB/mip/wrap/filter）、网格上传、caps —— 只加资源，不改 2D 路径。
 *   M1+：attach_window（wgpu surface 直呈）、3D pass、PBR 管线。
 * 未实现的方法**明确返回非 0**（不静默降级），调用方（runtime）在帧边界按返回值回退。
 * ========================================================================== */
static WGPUTextureFormat format_from_code(int32_t code) {
    switch (code) {
    case 1: return WGPUTextureFormat_RGBA8UnormSrgb;
    case 2: return WGPUTextureFormat_RGBA16Float;
    default: return WGPUTextureFormat_RGBA8Unorm;
    }
}
static int32_t format_code(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_BGRA8Unorm: return 1;
    case WGPUTextureFormat_RGBA8UnormSrgb: return 2;
    case WGPUTextureFormat_BGRA8UnormSrgb: return 3;
    case WGPUTextureFormat_RGBA16Float: return 4;
    default: return 0;
    }
}
static WGPUAddressMode wrap_from_code(int32_t c) {
    switch (c) {
    case 1: return WGPUAddressMode_ClampToEdge;
    case 2: return WGPUAddressMode_MirrorRepeat;
    default: return WGPUAddressMode_Repeat;
    }
}

/* 适配器与目标能力：提取/资产 cook 据此选择（不做静默降级）。 */
static int32_t r_caps(void* self, mq_renderer_caps* out) {
    auto* dv = (Device*)self;
    if (!dv || !out) return -1;
    std::memset(out, 0, sizeof *out);
    WGPULimits lim = WGPU_LIMITS_INIT;
    if (wgpuAdapterGetLimits(dv->adapter, &lim) == WGPUStatus_Success)
        out->max_texture_size = (int32_t)lim.maxTextureDimension2D;
    if (out->max_texture_size <= 0) out->max_texture_size = 2048;
    out->max_lights = 8;       /* 与 shader 的 lights: array<GpuLight,8> 一致（超出部分 runtime 侧应截断并告警） */
    out->max_samples = 1;      /* M5 MSAA 后再按设备能力填 */
    out->target_format = dv->surface ? format_code(dv->surfaceFormat) : 0;
    out->depth_format = 0;     /* Depth32Float（M2） */
    out->direct_present = dv->surface ? 1 : 0;
    out->hdr = 0;              /* M3 用 RGBA16F 之前按设备特性探测 */
    return 0;
}

/* 纹理 v2：RGBA8 / RGBA8_SRGB + mip 链 + per-texture sampler（PBR 用）。
 * data 里各 mip 紧排（mip0..mipN）；data 为空 = 只建资源（M3 里给 runtime 上传用）。 */
static int32_t r_create_texture2(void* self, const mq_renderer_texture_desc* d, int32_t* out_id) {
    auto* dv = (Device*)self;
    if (!dv || !d || !out_id) return -1;
    if (d->width <= 0 || d->height <= 0) return -2;
    if (d->kind != 0) return -3;                                  /* cube/equirect：M4 */
    if (d->pixel_format != 0 && d->pixel_format != 1) return -4;  /* M0 只 RGBA8 / RGBA8_SRGB */
    int32_t max_mips = 1;
    for (uint32_t s = (uint32_t)std::max(d->width, d->height); s > 1; s >>= 1) ++max_mips;
    int32_t mips = d->mip_levels <= 0 ? 1 : d->mip_levels;
    if (mips > max_mips) mips = max_mips;

    auto* t = new Texture();
    t->format = format_from_code(d->pixel_format);
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ (uint32_t)d->width, (uint32_t)d->height, 1 };
    td.format = t->format;
    td.mipLevelCount = (uint32_t)mips;
    td.sampleCount = 1;
    t->tex = wgpuDeviceCreateTexture(dv->device, &td);
    if (!t->tex) { delete t; return -5; }
    t->view = wgpuTextureCreateView(t->tex, nullptr);

    const bool nearest = d->filter == 0;
    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.addressModeU = wrap_from_code(d->wrap_u);
    sd.addressModeV = wrap_from_code(d->wrap_v);
    sd.addressModeW = sd.addressModeU;
    sd.magFilter = nearest ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    sd.minFilter = sd.magFilter;
    sd.mipmapFilter = nearest ? WGPUMipmapFilterMode_Nearest : WGPUMipmapFilterMode_Linear;
    /* 各向异性 4：PBR 地面/车漆等斜视纹理靠它保锐度。WebGPU 采样器校验要求
     * maxAnisotropy>1 时三个 filter 都是 linear —— nearest（像素画/数据图）保持 1。
     * 实际生效取决于该贴图绑定到材质 bind group 时用的是这个 per-texture sampler（见 material_sampler）。 */
    sd.maxAnisotropy = nearest ? 1 : 4;
    t->sampler = wgpuDeviceCreateSampler(dv->device, &sd);

    if (d->data && d->data_size) {
        const uint8_t* p = d->data;
        size_t remain = (size_t)d->data_size;
        uint32_t w = (uint32_t)d->width, h = (uint32_t)d->height;
        for (int32_t lvl = 0; lvl < mips; ++lvl) {
            const size_t bytes = (size_t)w * h * 4;
            if (remain < bytes) break;                    /* 数据不足：后续 mip 留空（不越界） */
            WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            dst.texture = t->tex;
            dst.mipLevel = (uint32_t)lvl;
            dst.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = w * 4;
            layout.rowsPerImage = h;
            WGPUExtent3D sz = WGPUExtent3D{ w, h, 1 };
            wgpuQueueWriteTexture(dv->queue, &dst, p, bytes, &layout, &sz);
            p += bytes;
            remain -= bytes;
            w = std::max(1u, w >> 1);
            h = std::max(1u, h >> 1);
        }
    }
    dv->textures.push_back(t);
    *out_id = (int32_t)dv->textures.size() - 1;
    return 0;
}

/* 3D 网格上传（GPU 常驻）：layout 0 = pos3/normal3/tangent4/uv2（stride 48B）；
 * layout 1 = pos3/normal3/color4/uv0 2/uv1 2（stride 56B，plans/20 COLOR_0+TEXCOORD_1）。 */
static int32_t r_create_mesh(void* self, const mq_renderer_mesh_gpu* data, int32_t* out_id) {
    auto* dv = (Device*)self;
    if (!dv || !data || !out_id) return -1;
    if (!data->vertices || data->vertex_bytes == 0) return -2;
    if (!data->indices || data->index_count == 0) return -3;
    if (data->vertex_layout != 0 && data->vertex_layout != 1) return -4;   /* 仅布局 0/1 */
    const uint32_t want_stride = (data->vertex_layout == 1) ? 56u : 48u;
    if (data->vertex_stride != want_stride) return -4;                     /* stride 必须与布局匹配 */

    auto* m = new MeshGpu();
    {
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        bd.size = data->vertex_bytes;
        m->vbo = wgpuDeviceCreateBuffer(dv->device, &bd);
        if (!m->vbo) { delete m; return -5; }
        wgpuQueueWriteBuffer(dv->queue, m->vbo, 0, data->vertices, (size_t)data->vertex_bytes);
    }
    {
        const uint64_t bytes = (uint64_t)data->index_count * sizeof(uint32_t);
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        bd.size = bytes;
        m->ibo = wgpuDeviceCreateBuffer(dv->device, &bd);
        if (!m->ibo) { wgpuBufferRelease(m->vbo); delete m; return -6; }
        wgpuQueueWriteBuffer(dv->queue, m->ibo, 0, data->indices, (size_t)bytes);
    }
    m->index_count = data->index_count;
    m->vertex_layout = data->vertex_layout;
    m->vertex_stride = want_stride;
    std::memcpy(m->bounds_min, data->bounds_min, sizeof m->bounds_min);
    std::memcpy(m->bounds_max, data->bounds_max, sizeof m->bounds_max);
    dv->meshes.push_back(m);
    *out_id = (int32_t)dv->meshes.size() - 1;
    return 0;
}
static void r_destroy_mesh(void* self, int32_t id) {
    auto* dv = (Device*)self;
    if (!dv || id < 0 || id >= (int)dv->meshes.size()) return;
    MeshGpu* m = dv->meshes[(size_t)id];
    if (m) {
        if (m->vbo) wgpuBufferRelease(m->vbo);
        if (m->ibo) wgpuBufferRelease(m->ibo);
        delete m;
    }
    dv->meshes[(size_t)id] = nullptr;   /* 槽位保留（id 稳定） */
}

/* 前向声明：成帧主路径定义在下方（图元 + 网格 + 可选外部目标纹理）。 */
static int32_t render_impl(void* self, int32_t TW, int32_t TH,
                           const mq_renderer_view* views, uint32_t nviews,
                           const mq_renderer_item* items, uint32_t count,
                           const mq_renderer_mesh* meshes, uint32_t mesh_count,
                           const float* bg, uint8_t* out, uint32_t out_size,
                           WGPUTexture target = nullptr,
                           WGPUTextureFormat target_format = WGPUTextureFormat_RGBA8Unorm,
                           bool clear_first = true);

/* 前向声明：3D pass（depth + unlit3d），定义在 begin_pass 之后。 */
static int32_t render_3d_only(Device* dv, WGPUTexture target, WGPUTextureFormat target_format,
                              const mq_renderer_frame3d* f);

/* ---- M1 直接呈现：wgpu surface + swapchain（plans/18-pbr-3d.md §4.1）----
 * 窗口由 platform 提供原生句柄（Win32 HWND / X11 Window / Wayland wl_surface）；
 * swapchain 格式由 surface caps 决定（优先非 sRGB 的 RGBA8/BGRA8，保持既有 2D 输出观感），
 * 2D 管线按该格式重建；离屏/截图仍是 RGBA8（切换时 ensure_2d_pipelines 处理）。 */
static bool configure_surface(Device* dv, int32_t w, int32_t h) {
    if (!dv->surface || w <= 0 || h <= 0) return false;
    WGPUSurfaceConfiguration cfg = WGPU_SURFACE_CONFIGURATION_INIT;
    cfg.device = dv->device;
    cfg.format = dv->surfaceFormat;
    cfg.usage = WGPUTextureUsage_RenderAttachment |
                (dv->surfaceCopySrc ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_None);
    cfg.width = (uint32_t)w;
    cfg.height = (uint32_t)h;
    cfg.presentMode = WGPUPresentMode_Fifo;
    cfg.alphaMode = WGPUCompositeAlphaMode_Opaque;
    wgpuSurfaceConfigure(dv->surface, &cfg);
    dv->surfaceWidth = w;
    dv->surfaceHeight = h;
    return true;
}

static int32_t r_attach_window(void* self, const mq_renderer_window* win) {
    auto* dv = (Device*)self;
    if (!dv || !win) return -1;
    if (win->width <= 0 || win->height <= 0) return -2;
    if (dv->surface) {
        wgpuSurfaceUnconfigure(dv->surface);
        wgpuSurfaceRelease(dv->surface);
        dv->surface = nullptr;
    }
    WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    if (win->kind == 1) {                       /* Win32 */
        WGPUSurfaceSourceWindowsHWND src = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
        src.hinstance = win->display;
        src.hwnd = win->handle;
        desc.nextInChain = &src.chain;
        dv->surface = wgpuInstanceCreateSurface(dv->instance, &desc);
    } else if (win->kind == 2) {                /* X11 */
        WGPUSurfaceSourceXlibWindow src = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
        src.display = win->display;
        src.window = (uint64_t)(uintptr_t)win->handle;
        desc.nextInChain = &src.chain;
        dv->surface = wgpuInstanceCreateSurface(dv->instance, &desc);
    } else if (win->kind == 3) {                /* Wayland */
        WGPUSurfaceSourceWaylandSurface src = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
        src.display = win->display;
        src.surface = win->handle;
        desc.nextInChain = &src.chain;
        dv->surface = wgpuInstanceCreateSurface(dv->instance, &desc);
    } else {
        return -3;                              /* 不支持的窗口系统 */
    }
    if (!dv->surface) return -4;

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(dv->surface, dv->adapter, &caps) != WGPUStatus_Success || caps.formatCount == 0) {
        wgpuSurfaceCapabilitiesFreeMembers(caps);
        wgpuSurfaceRelease(dv->surface);
        dv->surface = nullptr;
        return -5;
    }
    WGPUTextureFormat chosen = caps.formats[0];
    for (size_t i = 0; i < caps.formatCount; ++i) {
        const WGPUTextureFormat f = caps.formats[i];
        if (f == WGPUTextureFormat_RGBA8Unorm || f == WGPUTextureFormat_BGRA8Unorm) { chosen = f; break; }
    }
    dv->surfaceFormat = chosen;
    dv->surfaceCopySrc = (caps.usages & WGPUTextureUsage_CopySrc) != 0;
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    if (!configure_surface(dv, win->width, win->height)) return -6;
    ensure_2d_pipelines(dv, dv->surfaceFormat);
    return 0;
}

/* 抓直呈帧（CI 像素断言用）：MAQUE_CAPTURE_WGPU=<前缀> 前 N 帧从 swapchain 纹理回读 PPM。
 * 需要 surface 支持 CopySrc；不支持时打印一次原因（不静默）。 */
static void capture_surface(Device* dv, int32_t w, int32_t h) {
    const char* prefix = std::getenv("MAQUE_CAPTURE_WGPU");
    if (!prefix || !*prefix || !dv->surfaceTex) return;
    int cap_max = 3;
    if (const char* v = std::getenv("MAQUE_CAPTURE_FRAMES")) cap_max = std::atoi(v);
    if (dv->captureCount >= cap_max) return;
    if (!dv->surfaceCopySrc) {
        static bool warned = false;
        if (!warned) { warned = true; std::print(stderr, "gfx: surface 不支持 CopySrc → 无法抓直呈帧\n"); }
        return;
    }
    const uint32_t row = (uint32_t)w * 4u;
    const uint32_t padded = (row + 255u) & ~255u;
    WGPUBufferDescriptor rbd = WGPU_BUFFER_DESCRIPTOR_INIT;
    rbd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    rbd.size = (uint64_t)padded * (uint64_t)h;
    WGPUBuffer rb = wgpuDeviceCreateBuffer(dv->device, &rbd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = dv->surfaceTex; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = rb; dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext = WGPUExtent3D{ (uint32_t)w, (uint32_t)h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);

    WGPUMapAsyncStatus st = (WGPUMapAsyncStatus)0;
    WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = map_cb; mci.userdata1 = &st;
    wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, rbd.size, mci);
    poll(dv->instance, [&]{ return (uint32_t)st != 0; });
    if (st == WGPUMapAsyncStatus_Success) {
        const uint8_t* m = (const uint8_t*)wgpuBufferGetConstMappedRange(rb, 0, rbd.size);
        char path[512];
        std::snprintf(path, sizeof path, "%s_%d.ppm", prefix, dv->captureCount);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            const bool bgra = dv->surfaceFormat == WGPUTextureFormat_BGRA8Unorm ||
                              dv->surfaceFormat == WGPUTextureFormat_BGRA8UnormSrgb;
            for (int y = 0; y < h; ++y) {
                const uint8_t* p = m + (size_t)y * padded;
                for (int x = 0; x < w; ++x) {
                    const uint8_t* q = p + (size_t)x * 4;
                    const uint8_t rgb[3] = { bgra ? q[2] : q[0], q[1], bgra ? q[0] : q[2] };
                    std::fwrite(rgb, 1, 3, f);
                }
            }
            std::fclose(f);
            std::print("gfx: capture(wgpu surface) '{}' {}x{} frame#{}\n", path, w, h, dv->captureCount);
        }
        wgpuBufferUnmap(rb);
        ++dv->captureCount;
    }
    wgpuBufferRelease(rb);
}

static int32_t r_resize_surface(void* self, int32_t w, int32_t h) {
    auto* dv = (Device*)self;
    if (!dv || !dv->surface) return -1;
    if (w <= 0 || h <= 0) return -2;
    if (w == dv->surfaceWidth && h == dv->surfaceHeight) return 0;
    return configure_surface(dv, w, h) ? 0 : -3;
}

/* 一帧：M1 仅 2D 覆盖层（3D pass 在 M2/M3）。out 非空 → 离屏回读（--shot/CI）；空 → 直接呈现。 */
static int32_t r_render_frame3d(void* self, const mq_renderer_frame3d* f) {
    auto* dv = (Device*)self;
    if (!dv || !f) return -1;
    if (f->width <= 0 || f->height <= 0) return -2;
    float bg[4] = { 0.0F, 0.0F, 0.0F, 1.0F };
    if (f->bg[0] != 0.0F || f->bg[1] != 0.0F || f->bg[2] != 0.0F || f->bg[3] != 0.0F)
        std::memcpy(bg, f->bg, sizeof bg);
    /* ---- M2 3D：先跑 depth + unlit3d pass，再用 render_impl(clear_first=false) 叠加 2D 覆盖层 ---- */
    const bool has3d = f->draw_count > 0 && f->view_count > 0 && f->views != nullptr;
    if (has3d) {
        if (f->out) {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
            td.dimension = WGPUTextureDimension_2D;
            td.size = WGPUExtent3D{ (uint32_t)f->width, (uint32_t)f->height, 1 };
            td.format = WGPUTextureFormat_RGBA8Unorm;
            td.mipLevelCount = 1; td.sampleCount = 1;
            WGPUTexture ctex = wgpuDeviceCreateTexture(dv->device, &td);
            if (!ctex) return -5;
            const int32_t rc3 = render_3d_only(dv, ctex, WGPUTextureFormat_RGBA8Unorm, f);
            const int32_t rc2 = render_impl(dv, f->width, f->height, f->views2d, f->view2d_count,
                                            f->items, f->item_count, f->meshes, f->mesh_count,
                                            bg, nullptr, 0, ctex, WGPUTextureFormat_RGBA8Unorm, false);
            const int32_t rcr = (rc3 == 0 && rc2 == 0)
                ? readback_rgba8(dv, ctex, f->width, f->height, f->out, f->out_size) : -6;
            wgpuTextureRelease(ctex);
            return rcr;
        }
        if (!dv->surface) return -3;
        if (dv->surfaceWidth != f->width || dv->surfaceHeight != f->height)
            r_resize_surface(dv, f->width, f->height);
        WGPUSurfaceTexture st3 = WGPU_SURFACE_TEXTURE_INIT;
        wgpuSurfaceGetCurrentTexture(dv->surface, &st3);
        if (st3.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            st3.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            if (st3.texture) wgpuTextureRelease(st3.texture);
            if (st3.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
                st3.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
                configure_surface(dv, dv->surfaceWidth, dv->surfaceHeight);
            return -4;
        }
        dv->surfaceTex = st3.texture;
        dv->surfaceView = wgpuTextureCreateView(st3.texture, nullptr);
        const int32_t rc3 = render_3d_only(dv, dv->surfaceTex, dv->surfaceFormat, f);
        const int32_t rc2 = render_impl(dv, f->width, f->height, f->views2d, f->view2d_count,
                                        f->items, f->item_count, f->meshes, f->mesh_count,
                                        bg, nullptr, 0, dv->surfaceTex, dv->surfaceFormat, false);
        if (rc3 != 0 || rc2 != 0) {
            if (dv->surfaceView) { wgpuTextureViewRelease(dv->surfaceView); dv->surfaceView = nullptr; }
            if (dv->surfaceTex) { wgpuTextureRelease(dv->surfaceTex); dv->surfaceTex = nullptr; }
            return -6;
        }
        capture_surface(dv, f->width, f->height);
        return 0;
    }
    if (f->out) {
        return render_impl(dv, f->width, f->height, f->views2d, f->view2d_count,
                           f->items, f->item_count, f->meshes, f->mesh_count,
                           bg, f->out, f->out_size);
    }
    /* 直接呈现：acquire → 渲染 → present（present 由 ABI 的 present 完成） */
    if (!dv->surface) return -3;
    if (dv->surfaceWidth != f->width || dv->surfaceHeight != f->height)
        r_resize_surface(dv, f->width, f->height);
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(dv->surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (st.texture) wgpuTextureRelease(st.texture);
        if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated ||
            st.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
            configure_surface(dv, dv->surfaceWidth, dv->surfaceHeight);
        return -4;
    }
    dv->surfaceTex = st.texture;
    dv->surfaceView = wgpuTextureCreateView(st.texture, nullptr);
    const int32_t rc = render_impl(dv, f->width, f->height, f->views2d, f->view2d_count,
                                   f->items, f->item_count, f->meshes, f->mesh_count,
                                   bg, nullptr, 0, dv->surfaceTex, dv->surfaceFormat);
    if (rc != 0) {
        if (dv->surfaceView) { wgpuTextureViewRelease(dv->surfaceView); dv->surfaceView = nullptr; }
        if (dv->surfaceTex) { wgpuTextureRelease(dv->surfaceTex); dv->surfaceTex = nullptr; }
        return rc;
    }
    capture_surface(dv, f->width, f->height);   /* MAQUE_CAPTURE_WGPU：CI 抓直呈帧（present 前） */
    return 0;
}
static int32_t r_present(void* self) {
    auto* dv = (Device*)self;
    if (!dv) return -1;
    if (!dv->surface) return 0;               /* 离屏路径：空操作 */
    if (dv->surfaceView) { wgpuTextureViewRelease(dv->surfaceView); dv->surfaceView = nullptr; }
    if (dv->surfaceTex) { wgpuTextureRelease(dv->surfaceTex); dv->surfaceTex = nullptr; }
    return wgpuSurfacePresent(dv->surface) == WGPUStatus_Success ? 0 : -2;
}

/* 网格顶点（与 assets/builtin/shaders/mesh/shader.wgsl 的顶点布局严格一致：stride 48B）
 * col  = light 色（骨骼 × 槽位 × 附件）；col2 = dark 色（两色着色 TintBlack）。
 * dark 缺省 = (0,0,0,1)：此时着色公式**精确退化**为旧的 `tex * light`，未启用该特性的素材不回归。 */
struct MeshVertex { float x, y, u, v, r, g, b, a, d0, d1, d2, d3; };
static_assert(sizeof(MeshVertex) == 48, "MeshVertex 必须与 WGSL 顶点布局一致（stride 48）");
static_assert(offsetof(MeshVertex, r) == 16, "col 必须在 offset 16（location 2）");
static_assert(offsetof(MeshVertex, d0) == 32, "dark 必须在 offset 32（location 3）");

/* 一条绘制命令：**图元批次**（实例化 6 顶点）或**网格批次**（索引绘制）。
 * 两者在同一序列里按 (layer, z, 原始序) 排序 → 网格与精灵可以正确互相遮挡。 */
struct Cmd {
    int layer = 0, z = 0;
    uint32_t seq = 0;         /* 原始下标（同键时的稳定次序） */
    int kind = 0;             /* 0 = 图元，1 = 网格 */
    uint32_t first = 0;       /* 图元：insts 起点；网格：meshes 下标 */
    uint32_t count = 0;       /* 图元：实例数；网格：恒 1 */
    int texture = -1;
    bool nearest = false;
};

/* 列主序 4×4 乘法：o = a * b（与 WGSL/WebGPU 的列主序约定一致）。 */
static void mat4_mul(const float* a, const float* b, float* o) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            o[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                           a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
}

static void mat4_look_at(const float eye[3], const float ctr[3], const float up[3], float* m) {
    float f[3] = { ctr[0] - eye[0], ctr[1] - eye[1], ctr[2] - eye[2] };
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6F) { f[0] = 0.0F; f[1] = 0.0F; f[2] = -1.0F; fl = 1.0F; }
    f[0] /= fl; f[1] /= fl; f[2] /= fl;
    float sx = f[1] * up[2] - f[2] * up[1];
    float sy = f[2] * up[0] - f[0] * up[2];
    float sz = f[0] * up[1] - f[1] * up[0];
    float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-6F) { sx = 1.0F; sy = 0.0F; sz = 0.0F; sl = 1.0F; }
    sx /= sl; sy /= sl; sz /= sl;
    const float ux = sy * f[2] - sz * f[1], uy = sz * f[0] - sx * f[2], uz = sx * f[1] - sy * f[0];
    m[0] = sx; m[4] = sy; m[8] = sz; m[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    m[1] = ux; m[5] = uy; m[9] = uz; m[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2]; m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[3] = 0.0F; m[7] = 0.0F; m[11] = 0.0F; m[15] = 1.0F;
}
static void mat4_ortho(float zn, float zf, float half, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0F / half; m[5] = 1.0F / half;
    m[10] = 1.0F / (zf - zn); m[14] = -zn / (zf - zn); m[15] = 1.0F;
}
/* 任意正交盒（视空间 x∈[l,r]、y∈[b,t]、z∈[zn,zf]，zn > zf 且都为负=相机前方）。
 * 与 mat4_ortho 的 WebGPU NDC 约定完全一致（z∈[0,1]：near→0、far→1），只是 x/y 各自缩放+平移。
 * 用于按场景 mesh bounds 拟合方向光阴影视锥（审计 R2：旧实现固定原点+半径启发式、忽略 bounds）。 */
static void mat4_ortho_box(float l, float r, float b, float t, float zn, float zf, float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0F / (r - l);
    m[5] = 2.0F / (t - b);
    m[10] = 1.0F / (zf - zn);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -zn / (zf - zn);
    m[15] = 1.0F;
}
/* model 矩阵左上 3×3 的行列式（列主序）：<0 = 镜像缩放 → 三角形绕序翻转，材质管线需 frontFace=CW。 */
static float mat3_det(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) -
           m[4] * (m[1] * m[10] - m[2] * m[9]) +
           m[8] * (m[1] * m[6] - m[2] * m[5]);
}
/* 材质 uniform 打包（plans/19）：72 floats / 288B，与 pbr shader 的 Draw 结构逐项对应。
 * 阴影 pass 与主 pass 共用同一函数，避免两处字段漂移。 */
static constexpr int kMatUniformFloats = 72;
static void pack_draw_uniform(float* du, const float model[16], const mq_renderer_pbr_material* mat) {
    std::memset(du, 0, sizeof(float) * (size_t)kMatUniformFloats);
    std::memcpy(du, model, sizeof(float) * 16);                    /* 0..15：模型矩阵 */
    if (!mat) {
        /* 无材质：沿用历史默认灰（基色白/粗糙度 0.6/无 clearcoat） */
        du[16] = du[17] = du[18] = du[19] = 1.0F;
        du[23] = 1.0F;                                             /* emissive_intensity */
        du[25] = 0.6F;                                             /* roughness */
        du[26] = 1.0F;                                             /* normal_scale */
        du[27] = 1.0F;                                             /* occlusion */
        du[28] = 0.5F;                                             /* alpha_cutoff */
        du[32] = 0.0F; du[33] = 0.03F;                             /* clearcoat */
        du[36] = 0.0F; du[37] = 2.0F; du[38] = 0.0F; du[39] = 1.0F;/* model/side/flags2/opacity */
        du[40] = 1.0F; du[41] = 1.5F; du[42] = 30.0F; du[43] = 1.0F;
        du[44] = 0.0F; du[45] = 0.5F; du[46] = 0.0F; du[47] = 1.3F;
        du[48] = 100.0F; du[49] = 400.0F; du[50] = 0.0F; du[51] = 0.0F;
        du[52] = 0.0F; du[53] = 1.0F; du[54] = 1.0F; du[55] = 3.0F;
        du[56] = du[57] = du[58] = 1.0F;                           /* specular */
        du[59] = 0.0F; du[63] = 0.0F;                              /* uv_rotation / normal_uv_rotation */
        du[64] = du[65] = 1.0F; du[66] = du[67] = 0.0F;             /* uv scale/offset */
        du[68] = du[69] = 1.0F; du[70] = du[71] = 0.0F;             /* normal uv scale/offset */
        return;
    }
    du[16] = mat->base_color[0]; du[17] = mat->base_color[1];
    du[18] = mat->base_color[2]; du[19] = mat->base_color[3];
    du[20] = mat->emissive[0]; du[21] = mat->emissive[1]; du[22] = mat->emissive[2];
    du[23] = mat->emissive_intensity;
    du[24] = mat->metallic; du[25] = mat->roughness;
    du[26] = mat->normal_scale; du[27] = mat->occlusion_strength;
    du[28] = mat->alpha_cutoff; du[29] = (float)mat->alpha_mode; du[30] = (float)mat->flags;
    du[32] = mat->clearcoat; du[33] = mat->clearcoat_roughness;
    du[36] = (float)mat->shading_model; du[37] = (float)mat->side;
    du[38] = (float)mat->flags2; du[39] = mat->opacity;
    du[40] = mat->specular_intensity; du[41] = mat->ior;
    du[42] = mat->shininess; du[43] = mat->env_intensity;
    du[44] = mat->sheen; du[45] = mat->sheen_roughness;
    du[46] = mat->iridescence; du[47] = mat->iridescence_ior;
    du[48] = mat->iridescence_thickness_min; du[49] = mat->iridescence_thickness_max;
    du[50] = mat->anisotropy; du[51] = mat->anisotropy_rotation;
    du[52] = (float)mat->depth_packing; du[53] = mat->normal_scale_y;
    du[54] = mat->clearcoat_normal_scale; du[55] = (float)mat->toon_steps;
    du[56] = mat->specular[0]; du[57] = mat->specular[1]; du[58] = mat->specular[2];
    du[59] = mat->uv_rotation;                                   /* specular.w = 基础贴图 UV 旋转 */
    du[60] = mat->sheen_color[0]; du[61] = mat->sheen_color[1]; du[62] = mat->sheen_color[2];
    du[63] = mat->normal_uv_rotation;                            /* sheen_color.w = 法线贴图 UV 旋转 */
    du[64] = mat->uv_scale[0]; du[65] = mat->uv_scale[1];
    du[66] = mat->uv_offset[0]; du[67] = mat->uv_offset[1];
    du[68] = mat->normal_uv_scale[0]; du[69] = mat->normal_uv_scale[1];
    du[70] = mat->normal_uv_offset[0]; du[71] = mat->normal_uv_offset[1];
}

/* 材质贴图句柄 → 纹理 view（非法/未给 → fallback 缺省纹理） */
static WGPUTextureView material_tex_view(Device* dv, int32_t handle, WGPUTextureView fallback) {
    if (dv && handle >= 0 && (size_t)handle < dv->textures.size()) {
        Texture* t = dv->textures[(size_t)handle];
        if (t && t->view) return t->view;
    }
    return fallback;
}

/* 材质贴图采样器：优先用该材质**第一张有效贴图**的 per-texture sampler。
 * 为什么：create_texture2 按 desc.wrap_u/wrap_v/filter 建了独立 sampler（runtime 传 wrap=0=Repeat、
 * filter=1=linear），glTF 的 REPEAT/MIRRORED_REPEAT/NEAREST 就存在这里；旧实现把所有贴图统一绑
 * 全局 ClampToEdge+linear 采样器 → UV>1（平铺、KHR_texture_transform）被 clamp 成边缘拉伸、
 * NEAREST 被线性糊掉（审计 R1）。同一材质的各贴图通常共用同一批 sampler 语义，取第一张有效贴图的
 * sampler 即可覆盖绝大多数资产；找不到（材质无贴图）才回退全局 sampler。 */
static WGPUSampler material_sampler(Device* dv, const mq_renderer_pbr_material* mat) {
    if (dv && mat) {
        for (int i = 0; i < 5; ++i) {                       /* albedo→mr→ao→normal→emissive */
            const int32_t h = mat->textures[i];
            if (h < 0 || (size_t)h >= dv->textures.size()) continue;
            Texture* t = dv->textures[(size_t)h];
            if (t && t->sampler) return t->sampler;
        }
        for (int i = 0; i < 3; ++i) {                       /* matcap / gradient / clearcoat-normal */
            const int32_t h = mat->textures2[i];
            if (h < 0 || (size_t)h >= dv->textures.size()) continue;
            Texture* t = dv->textures[(size_t)h];
            if (t && t->sampler) return t->sampler;
        }
        for (int i = 0; i < 7; ++i) {                       /* KHR 扩展 7 槽（textures3） */
            const int32_t h = mat->textures3[i];
            if (h < 0 || (size_t)h >= dv->textures.size()) continue;
            Texture* t = dv->textures[(size_t)h];
            if (t && t->sampler) return t->sampler;
        }
    }
    return dv ? dv->sampler : nullptr;
}

static void fill_material_bind_entries(Device* dv, WGPUBindGroupEntry (&e)[17], WGPUBuffer buf, size_t buf_size,
                                       const mq_renderer_pbr_material* mat) {
    const WGPUTextureView white = dv->whiteTexView;
    const WGPUTextureView neutral = dv->neutralNormalView ? dv->neutralNormalView : dv->whiteTexView;
    std::memset(e, 0, sizeof(e));
    e[0].binding = 0; e[0].buffer = buf; e[0].size = buf_size;
    e[1].binding = 1; e[1].textureView = material_tex_view(dv, mat ? mat->textures[0] : -1, white);
    e[2].binding = 2; e[2].sampler = material_sampler(dv, mat);   /* R1：per-texture wrap/filter 恢复 */
    e[3].binding = 3; e[3].textureView = material_tex_view(dv, mat ? mat->textures[1] : -1, white);
    e[4].binding = 4; e[4].textureView = material_tex_view(dv, mat ? mat->textures[2] : -1, white);
    e[5].binding = 5; e[5].textureView = material_tex_view(dv, mat ? mat->textures[3] : -1, neutral);
    e[6].binding = 6; e[6].textureView = material_tex_view(dv, mat ? mat->textures[4] : -1, white);
    e[7].binding = 7; e[7].textureView = material_tex_view(dv, mat ? mat->textures2[0] : -1, white);
    e[8].binding = 8; e[8].textureView = material_tex_view(dv, mat ? mat->textures2[1] : -1, white);
    e[9].binding = 9; e[9].textureView = material_tex_view(dv, mat ? mat->textures2[2] : -1, neutral);
    /* KHR 扩展 7 槽（plans/20）：绑定 10..16 对应 textures3[0..6]。
     * 缺失槽绑 1×1 白 —— shader 侧用 flags2 presence bit12..18 决定是否采样/参与计算，
     * 所以白色不会污染旧材质（presence=0 时根本不读数）。
     * 注：iridescence_thickness 缺失绑 white → 采样 G=1，shader 按 bit17 决定是否 mix。 */
    for (int i = 0; i < 7; ++i) {
        WGPUBindGroupEntry& en = e[10 + i];
        en.binding = (uint32_t)(10 + i);
        en.textureView = material_tex_view(dv, mat ? mat->textures3[i] : -1, white);
    }
}

/* M2：3D pass（depth + unlit3d）渲染进 target；2D 覆盖层由调用方随后
 * 用 render_impl(target, clear_first=false) 叠加（保持 2D 像素路径完全不变）。 */
static int32_t render_3d_only(Device* dv, WGPUTexture target, WGPUTextureFormat target_format,
                              const mq_renderer_frame3d* f) {
    if (!dv || !target || !f) return 1;
    static const bool shadow_on = [] {
        const char* e = std::getenv("MAQUE_SHADOW");
        return !e || (std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0);
    }();
    if (f->view_count == 0 || !f->views) return 0;
    if (f->draw_count > 0 && !f->draws) return 1;     /* 防御：非法帧描述（不做静默降级） */
    ensure_3d_pipelines(dv, target_format);
    if (!dv->p3dShader || !dv->p3dPl) return 0;      /* shader 剔除：3D 跳过（原因已打印） */
    if (!ensure_depth(dv, f->width, f->height)) return 2;
    /* group0 的 binding1/2 分别是 depth 纹理与 comparison sampler：主 pass 无论如何都要能绑一组合法资源
     * （未启用阴影时绑 1×1 dummy depth + comparison sampler，z=0 让 shader 提前返回）。
     * 任一缺失只可能来自 GPU 资源创建失败 → 明确跳过 3D 并打印一次（R19：绝不把 filtering sampler 绑到
     * comparison 槽，那会让整个 bind group 校验失败）。 */
    if (!dv->dummyDepthView || !dv->shadowSampler) {
        static bool warned_shadow_res = false;
        if (!warned_shadow_res) {
            warned_shadow_res = true;
            std::print(stderr, "gfx: 阴影占位深度纹理/比较采样器不可用 → 3D 跳过本帧\n");
        }
        return 3;
    }
    WGPUTextureView view = wgpuTextureCreateView(target, nullptr);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    /* ---- 光源顺序（R4）：方向光在前、其余在后，各自保持原相对序 ----
     * shader 的阴影固定乘在 lights[0]（30_lighting.wgsl: `if (li == 0)`），而 runtime 的灯序 = 实体 id 序，
     * 不保证方向光最先（例如先创建点光）。这里做一次确定性重排，保证 0 号就是方向光；超过 8 盏按
     * "方向光优先"截断并打印一次告警（shader 的数组上限是 8）。 */
    uint32_t light_order[8] = {};
    uint32_t light_count = 0;
    {
        auto push_lights = [&](bool want_dir) {
            if (!f->lights) return;                        /* 防御：light_count>0 但指针为空 */
            for (uint32_t i = 0; i < f->light_count && light_count < 8; ++i)
                if ((f->lights[i].type == 0) == want_dir) light_order[light_count++] = i;
        };
        push_lights(true);
        push_lights(false);
        if (f->light_count > 8) {
            static bool warned_many_lights = false;
            if (!warned_many_lights) {
                warned_many_lights = true;
                std::print(stderr, "gfx: 光源 {} 盏 > shader 上限 8 → 按「方向光优先」截断（其余忽略）\n",
                           f->light_count);
            }
        }
    }
    /* 光源 0 = 方向光（若无方向光则 has_shadow_light=false → 关闭阴影：light_vp 与 li==0 的约定不成立）。 */
    float ldir[3] = { 0.0F, -1.0F, 0.0F };
    float lcol[3] = { 1.0F, 1.0F, 1.0F };
    bool has_shadow_light = false;
    if (light_count > 0) {
        const mq_renderer_light& L0 = f->lights[light_order[0]];
        if (L0.type == 0) {
            has_shadow_light = true;
            ldir[0] = L0.dir[0]; ldir[1] = L0.dir[1]; ldir[2] = L0.dir[2];
            lcol[0] = L0.color[0] * L0.intensity;
            lcol[1] = L0.color[1] * L0.intensity;
            lcol[2] = L0.color[2] * L0.intensity;
        }
    }
    float ll = std::sqrt(ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2]);
    if (ll < 1e-6F) { ldir[0] = 0.0F; ldir[1] = -1.0F; ldir[2] = 0.0F; ll = 1.0F; }
    ldir[0] /= ll; ldir[1] /= ll; ldir[2] /= ll;

    /* ---- 方向光阴影视锥拟合（R2）----
     * 旧实现：中心固定世界原点、半径 = max(4, |model 平移|₁ + 2.5) 封顶 12，mesh bounds 上传后从未使用
     * → 小尺度资产（如 ToyCar，scale=1e-4）阴影只剩几像素且 bias 大于物体；大/偏移资产阴影被裁掉。
     * 现在：把每个可绘制 draw 的 mesh 局部 bounds 8 角点 × model 变换到世界，再变换到光源视空间求 AABB，
     * 用 AABB 的 x/y 当作正交盒、z 当作 near/far。视锥随场景走，与模型尺度无关。
     * 无任何有效 mesh（bounds 非法/网格缺失）时回退旧启发式，保证老场景仍有合理阴影。 */
    const bool shadow_ready = shadow_on && has_shadow_light && dv->shadowView &&
                              (dv->shadowPipeline[0] || dv->shadowPipeline[1]);
    float shadow_vp[16] = {};
    shadow_vp[0] = shadow_vp[5] = shadow_vp[10] = shadow_vp[15] = 1.0F;   /* 未启用：单位矩阵（shader 不会用） */
    float shadow_bias = 0.0012F;
    if (shadow_ready) {
        std::vector<float> corners;                    /* 每个有效 draw 8 个世界点（xyz 紧排） */
        corners.reserve((size_t)f->draw_count * 24u);
        for (uint32_t di = 0; di < f->draw_count; ++di) {
            const mq_renderer_draw3d& d = f->draws[di];
            if (d.mesh < 0 || d.mesh >= (int)dv->meshes.size()) continue;
            MeshGpu* m = dv->meshes[(size_t)d.mesh];
            if (!m || !m->vbo || !m->ibo || m->index_count == 0) continue;
            for (int ci = 0; ci < 8; ++ci) {
                const float p[3] = { (ci & 1) ? m->bounds_max[0] : m->bounds_min[0],
                                     (ci & 2) ? m->bounds_max[1] : m->bounds_min[1],
                                     (ci & 4) ? m->bounds_max[2] : m->bounds_min[2] };
                corners.push_back(d.model[0] * p[0] + d.model[4] * p[1] + d.model[8] * p[2] + d.model[12]);
                corners.push_back(d.model[1] * p[0] + d.model[5] * p[1] + d.model[9] * p[2] + d.model[13]);
                corners.push_back(d.model[2] * p[0] + d.model[6] * p[1] + d.model[10] * p[2] + d.model[14]);
            }
        }
        /* look_at 的 up：光方向接近 ±Y 时换 X 轴，避免退化 */
        const float up[3] = { (std::fabs(ldir[1]) > 0.99F) ? 1.0F : 0.0F,
                              (std::fabs(ldir[1]) > 0.99F) ? 0.0F : 1.0F, 0.0F };
        float lview[16];
        if (!corners.empty()) {
            float max_r2 = 0.0F;
            for (size_t i = 0; i + 2 < corners.size(); i += 3) {
                const float x = corners[i], y = corners[i + 1], z = corners[i + 2];
                max_r2 = std::max(max_r2, x * x + y * y + z * z);
            }
            /* 视点放在包围球外侧（方向光无位置，只需要一个不产生除零的 look_at 参考点） */
            const float dist = std::max(1.0F, std::sqrt(max_r2) + 1.0F);
            const float eye[3] = { -ldir[0] * dist, -ldir[1] * dist, -ldir[2] * dist };
            const float ctr[3] = { 0.0F, 0.0F, 0.0F };
            mat4_look_at(eye, ctr, up, lview);
            float xmin = 1e30F, xmax = -1e30F, ymin = 1e30F, ymax = -1e30F;
            float znear = -1e30F, zfar = 1e30F;        /* 视空间 z：相机前方为负 → 近 = 较大值，远 = 较小值 */
            for (size_t i = 0; i + 2 < corners.size(); i += 3) {
                const float x = corners[i], y = corners[i + 1], z = corners[i + 2];
                const float vx = lview[0] * x + lview[4] * y + lview[8] * z + lview[12];
                const float vy = lview[1] * x + lview[5] * y + lview[9] * z + lview[13];
                const float vz = lview[2] * x + lview[6] * y + lview[10] * z + lview[14];
                xmin = std::min(xmin, vx); xmax = std::max(xmax, vx);
                ymin = std::min(ymin, vy); ymax = std::max(ymax, vy);
                znear = std::max(znear, vz); zfar = std::min(zfar, vz);
            }
            /* padding：xy 取 2% 尺寸（保证平面/边界片元不会被裁）、z 取范围 2% */
            const float extent = std::max(xmax - xmin, ymax - ymin);
            const float pad_xy = std::max(extent * 0.02F, 1e-4F);
            const float pad_z = std::max((znear - zfar) * 0.02F, 1e-4F);
            const float l = xmin - pad_xy, r = xmax + pad_xy;
            const float b = ymin - pad_xy, t = ymax + pad_xy;
            float zn = znear + pad_z, zf = zfar - pad_z;
            if (zn - zf < 1e-3F) zf = zn - 1e-3F;      /* 退化保护：极扁平场景仍给出稳定的深度范围 */
            float lproj[16];
            mat4_ortho_box(l, r, b, t, zn, zf, lproj);
            mat4_mul(lproj, lview, shadow_vp);
            /* NDC z 常数偏置按"纹素在世界空间的大小 / 深度范围"派生：
             * 1024² 贴图覆盖 max(宽,高) → texel_world = max(r-l, t-b)/1024；乘 1.5 texel 安全边际。
             * 旧值 0.0012 是固定 NDC（= 世界 0.0012*(zf-zn)，随场景尺度失真）→ R2/R15。 */
            const float depth_range = zn - zf;
            const float texel_world = std::max(r - l, t - b) / 1024.0F;
            shadow_bias = std::clamp(texel_world * 1.5F / depth_range, 1e-6F, 0.01F);
        } else {
            /* fallback：旧半径启发式（无 mesh bounds 可用时保持历史行为） */
            float radius = 4.0F;
            for (uint32_t di = 0; di < f->draw_count; ++di) {
                const float* mm = f->draws[di].model;
                const float reach = std::fabs(mm[12]) + std::fabs(mm[13]) + std::fabs(mm[14]) + 2.5F;
                if (reach > radius) radius = reach;
            }
            if (radius > 12.0F) radius = 12.0F;
            const float dist = radius * 2.0F;
            const float ctr[3] = { 0.0F, 0.0F, 0.0F };
            const float eye[3] = { -ldir[0] * dist, -ldir[1] * dist, -ldir[2] * dist };
            float lview2[16], lproj2[16];
            mat4_look_at(eye, ctr, up, lview2);
            mat4_ortho(-dist + radius * 1.5F, -dist - radius * 1.5F, radius * 1.2F, lproj2);
            mat4_mul(lproj2, lview2, shadow_vp);
            shadow_bias = 0.0012F;
        }
    }

    for (uint32_t vi = 0; vi < f->view_count; ++vi) {
        const mq_renderer_view3d& v = f->views[vi];
        float vp[16];
        mat4_mul(v.proj, v.view, vp);                /* vp = proj * view */
        float cam[204] = {};
        std::memcpy(cam, vp, sizeof vp);
        cam[16] = v.eye[0]; cam[17] = v.eye[1]; cam[18] = v.eye[2]; cam[19] = v.exposure;
        cam[20] = ldir[0]; cam[21] = ldir[1]; cam[22] = ldir[2]; cam[23] = 0.0F;
        cam[24] = lcol[0]; cam[25] = lcol[1]; cam[26] = lcol[2]; cam[27] = 0.0F;
        cam[28] = f->env.env_color[0] * f->env.intensity;
        cam[29] = f->env.env_color[1] * f->env.intensity;
        cam[30] = f->env.env_color[2] * f->env.intensity;
        cam[31] = 0.0F;
        /* 阴影：light_vp 是按场景 mesh bounds 在光源视空间拟合的正交盒（R2，见循环外），
         * 对每个视图相同（方向光与相机无关）；未启用时为单位阵 + z=0（shader 直接返回 1）。 */
        std::memcpy(cam + 160, shadow_vp, sizeof shadow_vp);
        cam[176] = shadow_bias;
        cam[177] = 1.0F / 1024.0F;          /* 斜率项用的纹素尺寸（UV 单位；shader 现有公式保留） */
        cam[178] = shadow_ready ? 1.0F : 0.0F; cam[179] = 0.0F;
        /* 视矩阵（matcap 用 view-space 法线；plans/19）：camera uniform 尾部 16 floats */
        std::memcpy(cam + 180, v.view, sizeof(float) * 16);
        /* analytic studio IBL（plans/19）：天空/地面色（已乘环境强度）+ 软光箱强度 + 旋转（弧度） */
        cam[196] = f->env.env_sky_color[0] * f->env.intensity;
        cam[197] = f->env.env_sky_color[1] * f->env.intensity;
        cam[198] = f->env.env_sky_color[2] * f->env.intensity;
        cam[199] = f->env.env_highlight;
        cam[200] = f->env.env_ground_color[0] * f->env.intensity;
        cam[201] = f->env.env_ground_color[1] * f->env.intensity;
        cam[202] = f->env.env_ground_color[2] * f->env.intensity;
        cam[203] = f->env.env_rotation;
        /* 光照数组（8 盏）：按 light_order 重排后的顺序（方向光在前，保证 0 号是方向光/阴影灯）。
         * 每盏 16 float = pos(4, w=类型) + dir(4) + color(4) + params(4)。 */
        for (uint32_t li = 0; li < 8; ++li) {
            float* dst = cam + 32 + li * 16;
            if (li < light_count) {
                const mq_renderer_light& L = f->lights[light_order[li]];
                dst[0] = L.pos[0]; dst[1] = L.pos[1]; dst[2] = L.pos[2];
                dst[3] = (L.type == 0) ? 1.0F : ((L.type == 1) ? 2.0F : 3.0F);
                dst[4] = L.dir[0]; dst[5] = L.dir[1]; dst[6] = L.dir[2]; dst[7] = 1.0F;
                dst[8] = L.color[0] * L.intensity;
                dst[9] = L.color[1] * L.intensity;
                dst[10] = L.color[2] * L.intensity;
                dst[11] = 1.0F;
                dst[12] = L.range; dst[13] = L.inner_cos; dst[14] = L.outer_cos; dst[15] = 0.0F;
            } else {
                dst[3] = 0.0F;                                   /* w=0 → 未启用 */
            }
        }
        WGPUBufferDescriptor ubd = WGPU_BUFFER_DESCRIPTOR_INIT;
        ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        ubd.size = sizeof cam;
        WGPUBuffer cbuf = wgpuDeviceCreateBuffer(dv->device, &ubd);
        wgpuQueueWriteBuffer(dv->queue, cbuf, 0, cam, sizeof cam);
        WGPUBindGroupEntry cbe[3] = {};
        cbe[0].binding = 0; cbe[0].buffer = cbuf; cbe[0].size = sizeof cam;
        /* 主 pass 采样阴影图；阴影未启用时绑 1×1 dummy depth（z=0 → shader 不采样）。
         * binding2 必须是 Comparison sampler（BGL 如此声明），绝不拿 filtering sampler 兜底（R19）。 */
        cbe[1].binding = 1; cbe[1].textureView = shadow_ready ? dv->shadowView : dv->dummyDepthView;
        cbe[2].binding = 2; cbe[2].sampler = dv->shadowSampler;
        WGPUBindGroupDescriptor cbgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        cbgd.layout = dv->p3dCamBgl; cbgd.entryCount = 3; cbgd.entries = cbe;
        WGPUBindGroup camBg = wgpuDeviceCreateBindGroup(dv->device, &cbgd);
        /* 阴影深度 pass（仅首视图）：用同一个 816B 相机块（light_vp 在 float 160 起）；group0 绑 dummy depth
         * 避免与本 pass 写入的 shadowView 同纹理读写。 */
        if (shadow_ready && vi == 0) {
            WGPUBindGroupEntry sbe[3] = {};
            sbe[0].binding = 0; sbe[0].buffer = cbuf; sbe[0].size = sizeof cam;
            sbe[1].binding = 1; sbe[1].textureView = dv->dummyDepthView;
            sbe[2].binding = 2; sbe[2].sampler = dv->shadowSampler;
            WGPUBindGroupDescriptor sbgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            sbgd.layout = dv->p3dCamBgl; sbgd.entryCount = 3; sbgd.entries = sbe;
            WGPUBindGroup sbg = wgpuDeviceCreateBindGroup(dv->device, &sbgd);
            WGPUCommandEncoder senc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
            WGPURenderPassDepthStencilAttachment sda = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            sda.view = dv->shadowView;
            sda.depthLoadOp = WGPULoadOp_Clear;
            sda.depthStoreOp = WGPUStoreOp_Store;
            sda.depthClearValue = 1.0F;
            WGPURenderPassDescriptor srpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            srpd.depthStencilAttachment = &sda;
            WGPURenderPassEncoder spass = wgpuCommandEncoderBeginRenderPass(senc, &srpd);
            /* pipeline 按 draw 的顶点布局选择（stride 烘焙在管线里），所以不在这里统一 SetPipeline。 */
            wgpuRenderPassEncoderSetBindGroup(spass, 0, sbg, 0, nullptr);
            for (uint32_t di = 0; di < f->draw_count; ++di) {
                const mq_renderer_draw3d& d = f->draws[di];
                const mq_renderer_pbr_material* smat =
                    (f->materials && d.material >= 0 && (uint32_t)d.material < f->material_count)
                        ? &f->materials[d.material] : nullptr;
                if (smat && smat->shading_model == 9) continue;   /* ShadowMaterial 只接收阴影，不投射 */
                /* alpha_mode==2（BLEND）不投影：透明玻璃/薄片投实心影在多数资产上明显错误；
                 * MASK(1) 仍然投影——阴影 fragment 用 albedo alpha 做镂空。 */
                if (smat && smat->alpha_mode == 2) continue;
                if (d.mesh < 0 || d.mesh >= (int)dv->meshes.size()) continue;
                MeshGpu* m = dv->meshes[(size_t)d.mesh];
                if (!m || !m->vbo || !m->ibo || m->index_count == 0) continue;
                const int lay = (m->vertex_layout == 1) ? 1 : 0;
                WGPURenderPipeline spipe = dv->shadowPipeline[lay];
                if (!spipe) continue;                    /* 该布局阴影管线不可用（vs1 缺失/创建失败） */
                wgpuRenderPassEncoderSetPipeline(spass, spipe);
                float du[kMatUniformFloats] = {};
                /* 传真实材质：阴影 fragment（fs_shadow_alpha）要用 dr.alpha（cutoff/mode）、
                 * dr.base_color.a、dr.model_params.w(opacity) 与 albedo 贴图做 MASK alpha test（R3）。 */
                pack_draw_uniform(du, d.model, smat);
                WGPUBufferDescriptor dbd = WGPU_BUFFER_DESCRIPTOR_INIT;
                dbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
                dbd.size = sizeof du;
                WGPUBuffer dbuf = wgpuDeviceCreateBuffer(dv->device, &dbd);
                wgpuQueueWriteBuffer(dv->queue, dbuf, 0, du, sizeof du);
                WGPUBindGroupEntry dbe[17] = {};
                fill_material_bind_entries(dv, dbe, dbuf, sizeof du, smat);
                WGPUBindGroupDescriptor dbgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                dbgd.layout = dv->p3dDrawBgl; dbgd.entryCount = 17; dbgd.entries = dbe;
                WGPUBindGroup dbg = wgpuDeviceCreateBindGroup(dv->device, &dbgd);
                wgpuRenderPassEncoderSetBindGroup(spass, 1, dbg, 0, nullptr);
                wgpuRenderPassEncoderSetVertexBuffer(spass, 0, m->vbo, 0, WGPU_WHOLE_SIZE);
                wgpuRenderPassEncoderSetIndexBuffer(spass, m->ibo, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
                wgpuRenderPassEncoderDrawIndexed(spass, m->index_count, 1, 0, 0, 0);
                wgpuBindGroupRelease(dbg);
                wgpuBufferRelease(dbuf);
            }
            wgpuRenderPassEncoderEnd(spass);
            wgpuRenderPassEncoderRelease(spass);
            WGPUCommandBuffer scb = wgpuCommandEncoderFinish(senc, nullptr);
            wgpuQueueSubmit(dv->queue, 1, &scb);
            wgpuCommandBufferRelease(scb);
            wgpuCommandEncoderRelease(senc);
            wgpuBindGroupRelease(sbg);
        }

        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = view;
        ca.loadOp = vi == 0 ? WGPULoadOp_Clear : WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        ca.clearValue = WGPUColor{ v.clear_color[0], v.clear_color[1], v.clear_color[2], v.clear_color[3] };
        WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        da.view = dv->depthView;
        da.depthLoadOp = WGPULoadOp_Clear;
        da.depthStoreOp = WGPUStoreOp_Store;
        da.depthClearValue = 1.0F;
        WGPURenderPassDescriptor rpd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rpd.colorAttachmentCount = 1; rpd.colorAttachments = &ca;
        rpd.depthStencilAttachment = &da;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rpd);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, camBg, 0, nullptr);
        /* 不透明先画；透明按"相机→物体"距离从远到近（不写深度、混合管线） */
        std::vector<const mq_renderer_draw3d*> opaque_list, blend_list;
        opaque_list.reserve(f->draw_count);
        blend_list.reserve(f->draw_count);
        for (uint32_t di = 0; di < f->draw_count; ++di) {
            const mq_renderer_draw3d& d = f->draws[di];
            const mq_renderer_pbr_material* mm =
                (f->materials && d.material >= 0 && (uint32_t)d.material < f->material_count)
                    ? &f->materials[d.material] : nullptr;
            if (mm && mm->alpha_mode == 2) blend_list.push_back(&d);
            else opaque_list.push_back(&d);
        }
        {
            const float ex = v.eye[0], ey = v.eye[1], ez = v.eye[2];
            auto dist2 = [&](const mq_renderer_draw3d* d) {
                const float dx = d->model[12] - ex, dy = d->model[13] - ey, dz = d->model[14] - ez;
                return dx * dx + dy * dy + dz * dz;
            };
            std::stable_sort(blend_list.begin(), blend_list.end(),
                             [&](const mq_renderer_draw3d* a, const mq_renderer_draw3d* b) {
                                 return dist2(a) > dist2(b);
                             });
        }
        std::vector<const mq_renderer_draw3d*> draw_order = opaque_list;
        draw_order.insert(draw_order.end(), blend_list.begin(), blend_list.end());
        for (const mq_renderer_draw3d* dpp : draw_order) {
            const mq_renderer_draw3d& d = *dpp;
            if (d.mesh < 0 || d.mesh >= (int)dv->meshes.size()) continue;
            MeshGpu* m = dv->meshes[(size_t)d.mesh];
            if (!m || !m->vbo || !m->ibo || m->index_count == 0) continue;
            const mq_renderer_pbr_material* mat =
                (f->materials && d.material >= 0 && (uint32_t)d.material < f->material_count)
                    ? &f->materials[d.material] : nullptr;
            float du[kMatUniformFloats] = {};
            pack_draw_uniform(du, d.model, mat);
            WGPUBufferDescriptor dbd = WGPU_BUFFER_DESCRIPTOR_INIT;
            dbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            dbd.size = sizeof du;
            WGPUBuffer dbuf = wgpuDeviceCreateBuffer(dv->device, &dbd);
            wgpuQueueWriteBuffer(dv->queue, dbuf, 0, du, sizeof du);
            WGPUBindGroupEntry dbe[17] = {};
            fill_material_bind_entries(dv, dbe, dbuf, sizeof du, mat);
            WGPUBindGroupDescriptor dbgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            dbgd.layout = dv->p3dDrawBgl; dbgd.entryCount = 17; dbgd.entries = dbe;
            WGPUBindGroup drawBg = wgpuDeviceCreateBindGroup(dv->device, &dbgd);
            const bool is_blend = mat && mat->alpha_mode == 2;
            const int model = mat ? mat->shading_model : 0;
            const int side = mat ? mat->side : 2;
            /* 负行列式（镜像缩放）：NDC 绕序翻转，管线用 frontFace=CW 补偿（R9）。
             * 顶点布局由网格决定（0=48B 旧 / 1=56B COLOR_0+TEXCOORD_1），stride 烘焙在管线里。 */
            const bool mirrored = mat3_det(d.model) < 0.0F;
            const int layout = (m->vertex_layout == 1) ? 1 : 0;
            WGPURenderPipeline pipe =
                get_material_pipeline(dv, model, is_blend, side, mirrored, layout);
            if (!pipe) {
                wgpuBindGroupRelease(drawBg);
                wgpuBufferRelease(dbuf);
                continue;
            }
            wgpuRenderPassEncoderSetPipeline(pass, pipe);
            wgpuRenderPassEncoderSetBindGroup(pass, 1, drawBg, 0, nullptr);
            wgpuRenderPassEncoderSetVertexBuffer(pass, 0, m->vbo, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderSetIndexBuffer(pass, m->ibo, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
            wgpuRenderPassEncoderDrawIndexed(pass, m->index_count, 1, 0, 0, 0);
            wgpuBindGroupRelease(drawBg);
            wgpuBufferRelease(dbuf);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        wgpuBindGroupRelease(camBg);
        wgpuBufferRelease(cbuf);
    }
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    wgpuTextureViewRelease(view);
    return 0;
}

/* 成帧主路径：图元 + 网格 → 同一渲染目标（render_views 是其 meshes == nullptr 的特例）。 */
static int32_t render_impl(void* self, int32_t TW, int32_t TH,
                           const mq_renderer_view* views, uint32_t nviews,
                           const mq_renderer_item* items, uint32_t count,
                           const mq_renderer_mesh* meshes, uint32_t mesh_count,
                           const float* bg, uint8_t* out, uint32_t out_size,
                           WGPUTexture target,
                           WGPUTextureFormat target_format,
                           bool clear_first) {

    Device* dv = (Device*)self;
    if (!dv) return 1;
    if (!target && !out) return 1;                        /* 离屏必须给回读缓冲；直呈用 target */
    if (!target && (uint64_t)TW * TH * 4 > out_size) return 2;
    if (target && out) return 1;                          /* 二者互斥：直呈不回读（截图走离屏） */
    /* 目标格式变了（窗口 swapchain vs 离屏 RGBA8）→ 按需重建 2D 管线。 */
    ensure_2d_pipelines(dv, target ? target_format : WGPUTextureFormat_RGBA8Unorm);
    if (count && !items) return 1;
    if (mesh_count && !meshes) return 1;
    if (nviews == 0 || !views) {
        static mq_renderer_view def{ 0, 0, 0, 0, 0, 0 };
        def.vw = (float)TW; def.vh = (float)TH;
        views = &def; nviews = 1;
    }

    /* 稳定排序：layer → z → 原始下标（禁地址/哈希序，docs/10 D4、docs/13 §分层）。 */
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const mq_renderer_item& ia = items[a];
        const mq_renderer_item& ib = items[b];
        if (ia.layer != ib.layer) return ia.layer < ib.layer;
        if (ia.z != ib.z) return ia.z < ib.z;
        return a < b;
    });
    std::vector<uint32_t> morder(mesh_count);
    for (uint32_t i = 0; i < mesh_count; ++i) morder[i] = i;
    std::stable_sort(morder.begin(), morder.end(), [&](uint32_t a, uint32_t b) {
        const mq_renderer_mesh& ia = meshes[a];
        const mq_renderer_mesh& ib = meshes[b];
        if (ia.layer != ib.layer) return ia.layer < ib.layer;
        if (ia.z != ib.z) return ia.z < ib.z;
        return a < b;
    });

    /* 合并两条有序序列 → 一条绘制命令序列。相邻的"同贴图 + 同采样"图元仍合并成一次实例化
     * 绘制（保留原批处理收益），网格则插在它该在的位置上。 */
    std::vector<GpuInst> insts;
    insts.reserve(count);
    std::vector<Cmd> cmds;
    cmds.reserve((size_t)count + (size_t)mesh_count);
    size_t ii = 0, mi = 0;
    while (ii < count || mi < mesh_count) {
        bool take_item;
        if (ii >= count) take_item = false;
        else if (mi >= mesh_count) take_item = true;
        else {
            const mq_renderer_item& a = items[order[ii]];
            const mq_renderer_mesh& b = meshes[morder[mi]];
            if (a.layer != b.layer) take_item = a.layer < b.layer;
            else if (a.z != b.z) take_item = a.z < b.z;
            else take_item = order[ii] <= morder[mi];       /* 同键：图元在前（稳定、可复现） */
        }
        if (take_item) {
            const mq_renderer_item& s = items[order[ii]];
            const bool has_tex = s.texture >= 0 && s.texture < (int)dv->textures.size() &&
                                 dv->textures[s.texture] != nullptr &&          /* 槽位可能已释放（destroy_texture） */
                                 dv->textures[s.texture]->view != nullptr;
            const int tex_id = has_tex ? s.texture : -1;
            const bool rect = s.half_w > 0.0F && s.half_h > 0.0F;
            const bool nearest = s.filter == 1;                 /* 字形/像素画：1 纹素 = 1 像素 */
            GpuInst g{ { s.x, s.y }, s.size, s.rotation, { s.r, s.g, s.b, s.a },
                       { s.uv0, s.uv1, s.uv2, s.uv3 },
                       { rect ? s.half_w : 0.0F, rect ? s.half_h : 0.0F },
                       rect ? 1.0F : 0.0F, s.snap ? 1.0F : 0.0F };
            if (!cmds.empty() && cmds.back().kind == 0 && cmds.back().texture == tex_id &&
                cmds.back().nearest == nearest) {
                cmds.back().count++;
            } else {
                Cmd c; c.layer = s.layer; c.z = s.z; c.seq = order[ii]; c.kind = 0;
                c.first = (uint32_t)insts.size(); c.count = 1; c.texture = tex_id; c.nearest = nearest;
                cmds.push_back(c);
            }
            insts.push_back(g);
            ++ii;
        } else {
            const mq_renderer_mesh& m = meshes[morder[mi]];
            Cmd c; c.layer = m.layer; c.z = m.z; c.seq = morder[mi]; c.kind = 1;
            c.first = morder[mi]; c.count = 1; c.texture = m.texture;
            cmds.push_back(c);
            ++mi;
        }
    }

    /* ---- 网格几何：所有网格的顶点**交错**打进一个顶点缓冲 + 一个索引缓冲，
     *      每个批次用 (vbase, ibase, base_vertex) 定位 —— 避免逐批创建 GPU 缓冲。 ---- */
    std::vector<MeshVertex> mverts;
    std::vector<uint32_t> midx;
    std::vector<uint32_t> vbase(mesh_count, 0), ibase(mesh_count, 0);
    std::vector<uint8_t> mvalid(mesh_count, 0);
    for (uint32_t k = 0; k < mesh_count; ++k) {
        const mq_renderer_mesh& m = meshes[k];
        if (!m.vertices || !m.indices || m.vertex_count == 0 || m.index_count == 0) continue;
        vbase[k] = (uint32_t)mverts.size();
        ibase[k] = (uint32_t)midx.size();
        for (uint32_t i = 0; i < m.vertex_count; ++i) {
            MeshVertex mv{};
            mv.x = m.vertices[(size_t)i * 2]; mv.y = m.vertices[(size_t)i * 2 + 1];
            mv.u = m.uvs ? m.uvs[(size_t)i * 2] : 0.0F;
            mv.v = m.uvs ? m.uvs[(size_t)i * 2 + 1] : 0.0F;
            if (m.colors) {
                mv.r = m.colors[(size_t)i * 4]; mv.g = m.colors[(size_t)i * 4 + 1];
                mv.b = m.colors[(size_t)i * 4 + 2]; mv.a = m.colors[(size_t)i * 4 + 3];
            } else { mv.r = mv.g = mv.b = mv.a = 1.0F; }
            /* 两色着色（TintBlack）：dark 一路。
             * 语义与默认值取自官方参考实现 third_party/spine-cpp/src/spine/SkeletonRenderer.cpp:217
             * （`uint32_t darkColor = 0xff000000;` 即 rgba(0,0,0,1) —— "暗色=黑"）。
             * 调用方不给 dark_colors（例如纯色/非 spine 的网格）时用该默认值，
             * 着色公式随即退化为 `tex * light`，与加两色着色之前逐字节一致。 */
            if (m.dark_colors) {
                mv.d0 = m.dark_colors[(size_t)i * 4]; mv.d1 = m.dark_colors[(size_t)i * 4 + 1];
                mv.d2 = m.dark_colors[(size_t)i * 4 + 2]; mv.d3 = m.dark_colors[(size_t)i * 4 + 3];
            } else { mv.d0 = 0.0F; mv.d1 = 0.0F; mv.d2 = 0.0F; mv.d3 = 1.0F; }
            mverts.push_back(mv);
        }
        for (uint32_t t = 0; t < m.index_count; ++t) midx.push_back(m.indices[(size_t)m.first_index + t]);
        mvalid[k] = 1;
    }

    /* 目标纹理：离屏路径自建 RGBA8（+CopySrc 供回读）；直接呈现用 swapchain 纹理（不拥有）。 */
    WGPUTexture tex = target;
    if (!tex) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
        td.dimension = WGPUTextureDimension_2D;
        td.size = WGPUExtent3D{ (uint32_t)TW, (uint32_t)TH, 1 };
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.mipLevelCount = 1; td.sampleCount = 1;
        tex = wgpuDeviceCreateTexture(dv->device, &td);
        if (!tex) return 3;
    }
    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

    WGPUBufferDescriptor ubd = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubd.size = 48;
    WGPUBuffer ibuf = insts.empty() ? nullptr : make_ibuf(dv, insts);

    WGPUBuffer mvbuf = nullptr, mibuf = nullptr;
    if (!mverts.empty() && !midx.empty()) {
        WGPUBufferDescriptor vbd = WGPU_BUFFER_DESCRIPTOR_INIT;
        vbd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
        vbd.size = (uint64_t)mverts.size() * sizeof(MeshVertex);
        mvbuf = wgpuDeviceCreateBuffer(dv->device, &vbd);
        wgpuQueueWriteBuffer(dv->queue, mvbuf, 0, mverts.data(), vbd.size);
        WGPUBufferDescriptor ibd = WGPU_BUFFER_DESCRIPTOR_INIT;
        ibd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
        ibd.size = (uint64_t)midx.size() * sizeof(uint32_t);
        mibuf = wgpuDeviceCreateBuffer(dv->device, &ibd);
        wgpuQueueWriteBuffer(dv->queue, mibuf, 0, midx.data(), ibd.size);
    }

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    /* **一个视图一个 render pass**：所有绘制命令（图元 + 网格）都在同一 pass 内完成。
     * 旧实现是"每条命令一个 pass"（all.scn 有 331 条命令 → 331 次 Begin/EndRenderPass
     * + 331 个 bind group），wgpu 的 pass 建立/校验开销让 render 多花约 6-8ms（实测）。
     * 同一 pass 内本来就允许改 pipeline / bind group / scissor / vertex buffer，
     * 命令顺序不变 → 绘制结果与旧实现完全一致；bind group 按"同纹理+同采样"缓存复用。 */
    bool cleared = false;
    for (uint32_t vi = 0; vi < nviews; ++vi) {
        const mq_renderer_view& v = views[vi];
        const float zoom = v.zoom > 0.0F ? v.zoom : 1.0F;
        /* u.size = **单视图**尺寸，不是整个渲染目标尺寸。
         * 依据（实测，非推测）：多视图下传入总宽时，view0 的 px 计算会得到 TW/2 而实际画面把内容
         * 画在 TW/2+SW/2 —— 即 shader 侧的分母/居中项用的是"每个视图自己的宽高"。
         * 几何自洽：sprite 的 px = (x-cam)*zoom + size/2 + pan + view，而 view 是该视图在目标里的
         * 像素原点 → 传入单视图尺寸 + 各视图 view 偏移，多视图与单视图才等价。 */
        float uni[12] = { (float)TW, (float)TH, v.vx, v.vy, v.cx, v.cy, zoom, v.rot, v.pan, v.eye, 0.0F, 0.0F };
        WGPUBuffer ubuf = wgpuDeviceCreateBuffer(dv->device, &ubd);
        wgpuQueueWriteBuffer(dv->queue, ubuf, 0, uni, sizeof uni);

        WGPURenderPassEncoder pass = nullptr;
        begin_pass(enc, view, cleared ? WGPULoadOp_Load : (clear_first ? WGPULoadOp_Clear : WGPULoadOp_Load), bg, &pass);
        cleared = true;

        /* 每视图 bind group 缓存（uniform buffer 每视图一个，所以不能跨视图复用）。
         * key: 0 = solid；1<<32 | tex<<1 | nearest = textured；2<<32 | tex = mesh。 */
        struct BgEntry { uint64_t key; WGPUBindGroup bg; };
        std::vector<BgEntry> bg_cache;
        auto find_bg = [&](uint64_t key) -> WGPUBindGroup {
            for (const BgEntry& e : bg_cache)
                if (e.key == key) return e.bg;
            return nullptr;
        };

        for (const Cmd& c : cmds) {
            /* ---- 网格批次 ---- */
            if (c.kind == 1) {
                if (!mvbuf || !mibuf || !mvalid[c.first]) continue;
                const mq_renderer_mesh& m = meshes[c.first];
                int blend = (m.blend >= 0 && m.blend < 4) ? m.blend : 0;
                if (blend >= 2) {
                    /* multiply / screen 需要 dst-color 类 blend factor，本 wgpu-native 构建的
                     * WGPUBlendFactor 里没有（只有 zero/one/src/dst/*alpha）→ 回退 normal 并提示一次。
                     * 不静默假装支持（docs/27 已知限制同样记录这一点）。 */
                    static bool warned_mul = false, warned_screen = false;
                    bool& warned = (blend == 2) ? warned_mul : warned_screen;
                    if (!warned) {
                        warned = true;
                        std::print(stderr, "gfx: 网格混合模式 {}（multiply/screen）无对应 blend factor → 回退 normal\n",
                                   (int)m.blend);
                    }
                    blend = 0;
                }
                const bool has_tex = c.texture >= 0 && c.texture < (int)dv->textures.size() &&
                                     dv->textures[c.texture] != nullptr && dv->textures[c.texture]->view != nullptr;
                const int pma = m.pma ? 1 : 0;
                WGPURenderPipeline pipe = dv->meshPipeline[blend][pma];
                if (!pipe) {
                    static bool warned_mesh_pipe = false;
                    if (!warned_mesh_pipe) {
                        warned_mesh_pipe = true;
                        std::print(stderr, "gfx: 跳过网格批次（内建 shader 'mesh' 未编入；见 MAQUE_SHADER_* 开关）\n");
                    }
                    continue;
                }
                if (!has_tex) continue;          /* 无页纹理 → 无法采样（不静默画错颜色） */
                const uint64_t bkey = (uint64_t{ 2 } << 32) | (uint32_t)c.texture;
                WGPUBindGroup bgroup = find_bg(bkey);
                if (!bgroup) {
                    WGPUBindGroupEntry be[3] = {};
                    be[0].binding = 0; be[0].buffer = ubuf; be[0].size = sizeof uni;
                    be[1].binding = 1; be[1].textureView = dv->textures[c.texture]->view;
                    be[2].binding = 2; be[2].sampler = dv->sampler;
                    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                    bgd.layout = dv->meshBgl; bgd.entryCount = 3; bgd.entries = be;
                    bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);
                    bg_cache.push_back({ bkey, bgroup });
                }
                /* 裁剪矩形：`clip_w/h > 0` 时用网格自带的（ImGui 的 scissor），否则用整个视图。
                 * 两者取**交集**并夹到视图内，避免越界（WebGPU 要求 scissor 在附件范围内）。 */
                int sx = v.vx, sy = v.vy, sw = v.vw, sh = v.vh;
                if (m.clip_w > 0.0F && m.clip_h > 0.0F) {
                    const int cx = (int)(m.clip_x + 0.5F), cy = (int)(m.clip_y + 0.5F);
                    const int cw = (int)(m.clip_w + 0.5F), ch = (int)(m.clip_h + 0.5F);
                    const int x0 = std::max(sx, cx), y0 = std::max(sy, cy);
                    const int x1 = std::min(sx + sw, cx + cw), y1 = std::min(sy + sh, cy + ch);
                    sx = x0; sy = y0; sw = std::max(0, x1 - x0); sh = std::max(0, y1 - y0);
                    if (sw <= 0 || sh <= 0) continue;
                }
                wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, (uint32_t)sh);
                wgpuRenderPassEncoderSetPipeline(pass, pipe);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
                wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mvbuf, 0, (uint64_t)mverts.size() * sizeof(MeshVertex));
                wgpuRenderPassEncoderSetIndexBuffer(pass, mibuf, WGPUIndexFormat_Uint32, 0,
                                                    (uint64_t)midx.size() * sizeof(uint32_t));
                wgpuRenderPassEncoderDrawIndexed(pass, m.index_count, 1, ibase[c.first],
                                                 (int32_t)(vbase[c.first] + m.base_vertex), 0);
                continue;
            }
            /* ---- 图元批次（实例化矩形/圆） ---- */
            if (!ibuf) continue;
            const bool textured = c.texture >= 0;
            /* 该批次的管线不可用（对应内建 shader 被剔除 / 解压失败）→ 跳过该批次。
             * 只提示一次，避免逐帧刷屏；不崩、不静默（初始化时已打印原因）。 */
            if (!(textured ? dv->texPipeline : dv->solidPipeline)) {
                static bool warned_solid = false, warned_tex = false;
                bool& warned = textured ? warned_tex : warned_solid;
                if (!warned) {
                    warned = true;
                    std::print(stderr, "gfx: 跳过批次（内建 shader '{}' 未编入；见 MAQUE_SHADER_* 开关）\n",
                               textured ? "textured" : "solid");
                }
                continue;
            }
            const uint64_t bkey = textured
                ? ((uint64_t{ 1 } << 32) | ((uint64_t)(uint32_t)c.texture << 1) | (c.nearest ? 1ull : 0ull))
                : 0ull;
            WGPUBindGroup bgroup = find_bg(bkey);
            if (!bgroup) {
                WGPUBindGroupEntry be[4] = {};
                be[0].binding = 0; be[0].buffer = ubuf; be[0].size = sizeof uni;
                be[1].binding = 1; be[1].buffer = ibuf;
                be[1].size = (uint64_t)insts.size() * sizeof(GpuInst);
                WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                if (textured) {
                    be[2].binding = 2; be[2].textureView = dv->textures[c.texture]->view;
                    be[3].binding = 3;
                    be[3].sampler = c.nearest && dv->samplerNearest ? dv->samplerNearest : dv->sampler;
                    bgd.layout = dv->texBgl; bgd.entryCount = 4; bgd.entries = be;
                } else {
                    bgd.layout = dv->solidBgl; bgd.entryCount = 2; bgd.entries = be;
                }
                bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);
                bg_cache.push_back({ bkey, bgroup });
            }
            wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)v.vx, (uint32_t)v.vy, (uint32_t)v.vw, (uint32_t)v.vh);
            wgpuRenderPassEncoderSetPipeline(pass, textured ? dv->texPipeline : dv->solidPipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 6, c.count, 0, c.first);
        }
        wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
        for (const BgEntry& e : bg_cache)
            if (e.bg) wgpuBindGroupRelease(e.bg);
        wgpuBufferRelease(ubuf);
    }

    /* 离屏路径：编码回读拷贝（必须在 finish 之前）；直呈路径跳过（present 由 ABI 的 present 完成）。 */
    const uint32_t bytes_per_row = TW * 4;
    const uint32_t padded = (bytes_per_row + 255u) & ~255u;
    WGPUBuffer rbuf = nullptr;
    WGPUBufferDescriptor rbd = WGPU_BUFFER_DESCRIPTOR_INIT;
    if (!target) {
        rbd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        rbd.size = (uint64_t)padded * TH;
        rbuf = wgpuDeviceCreateBuffer(dv->device, &rbd);
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; src.texture = tex; src.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT; dst.buffer = rbuf; dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = TH;
        WGPUExtent3D extent = WGPUExtent3D{ (uint32_t)TW, (uint32_t)TH, 1 };
        wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
    }
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);

    int32_t rc = 0;
    if (!target) {
        WGPUMapAsyncStatus st = (WGPUMapAsyncStatus)0;
        WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = map_cb; mci.userdata1 = &st;
        wgpuBufferMapAsync(rbuf, WGPUMapMode_Read, 0, rbd.size, mci);
        poll(dv->instance, [&]{ return (uint32_t)st != 0; });
        if (st == WGPUMapAsyncStatus_Success) {
            const uint8_t* mapped = (const uint8_t*)wgpuBufferGetConstMappedRange(rbuf, 0, rbd.size);
            for (int y = 0; y < TH; ++y)
                std::memcpy(out + (size_t)y * TW * 4, mapped + (size_t)y * padded, bytes_per_row);
            wgpuBufferUnmap(rbuf);
        }
        wgpuBufferRelease(rbuf);
        rc = st == WGPUMapAsyncStatus_Success ? 0 : 3;
    }
    if (mibuf) wgpuBufferRelease(mibuf);
    if (mvbuf) wgpuBufferRelease(mvbuf);
    if (ibuf) wgpuBufferRelease(ibuf);
    wgpuTextureViewRelease(view);
    if (!target) wgpuTextureRelease(tex);   /* 外部 target（swapchain 纹理）归调用方所有 */
    return rc;
}

/* 兼容入口：只有图元（render_views 语义不变）。 */
static int32_t r_render_views(void* self, int32_t TW, int32_t TH,
                              const mq_renderer_view* views, uint32_t nviews,
                              const mq_renderer_item* items, uint32_t count,
                              const float* bg, uint8_t* out, uint32_t out_size) {
    return render_impl(self, TW, TH, views, nviews, items, count, nullptr, 0, bg, out, out_size);
}

/* 图元 + 网格一次成帧（骨骼动画等任意三角网格走这里）。 */
static int32_t r_render_mesh_views(void* self, int32_t TW, int32_t TH,
                                   const mq_renderer_view* views, uint32_t nviews,
                                   const mq_renderer_item* items, uint32_t count,
                                   const mq_renderer_mesh* meshes, uint32_t mesh_count,
                                   const float* bg, uint8_t* out, uint32_t out_size) {
    return render_impl(self, TW, TH, views, nviews, items, count, meshes, mesh_count, bg, out, out_size);
}

} // extern "C"

static const mq_renderer_v1 g_api = {
    { (uint32_t)sizeof(mq_renderer_v1), MQ_ABI_VERSION },
    r_create, r_destroy, r_destroy_texture, r_create_texture, r_has_texture, r_render_views, r_render_mesh_views,
    /* ---- 3D / 直接呈现（追加；plans/18-pbr-3d.md）---- */
    r_attach_window, r_resize_surface, r_caps, r_create_texture2, r_create_mesh, r_destroy_mesh,
    r_render_frame3d, r_present
};

extern "C" MQ_MODULE_EXPORT void* mq_module_query(const char* name, uint32_t min_version) {
    if (min_version > MQ_ABI_VERSION) return nullptr;
    if (name && std::strcmp(name, MQ_RENDERER_API_NAME) == 0) return (void*)&g_api;
    return nullptr;
}
