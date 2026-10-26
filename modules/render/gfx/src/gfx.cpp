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
static_assert(sizeof(mq_renderer_pbr_material) == 92, "mq_renderer_pbr_material 布局漂移（5 贴图槽）");
static_assert(sizeof(mq_renderer_environment) == 48, "mq_renderer_environment 布局漂移");
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
 * 规范顶点布局 layout=0：pos.xyz / normal.xyz / tangent.xyzw / uv.xy（stride 48B）。 */
struct MeshGpu {
    WGPUBuffer vbo = nullptr;
    WGPUBuffer ibo = nullptr;
    uint32_t index_count = 0;
    int32_t vertex_layout = 0;
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
    WGPUTextureFormat p3dFormat = WGPUTextureFormat_Undefined;   /* unlit3d 管线当前目标格式 */
    WGPURenderPipeline p3dPipeline = nullptr;
    WGPUBindGroupLayout p3dCamBgl = nullptr;
    WGPUBindGroupLayout p3dDrawBgl = nullptr;
    WGPUPipelineLayout p3dPl = nullptr;
    WGPUShaderModule p3dShader = nullptr;
    /* PBR 贴图缺省：1×1 白纹理（材质没给 albedo 贴图时绑定它，基色乘 1） */
    WGPUTexture whiteTex = nullptr;
    WGPUTextureView whiteTexView = nullptr;
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
static void ensure_3d_pipelines(Device* dv, WGPUTextureFormat format) {
    if (!dv) return;
    if (dv->p3dFormat == format && dv->p3dPipeline) return;
    if (dv->p3dPipeline) { wgpuRenderPipelineRelease(dv->p3dPipeline); dv->p3dPipeline = nullptr; }
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
            WGPUBindGroupLayoutEntry e{};
            e.binding = 0; e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e.buffer.type = WGPUBufferBindingType_Uniform; e.buffer.minBindingSize = 128;
            WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 1; bd.entries = &e;
            dv->p3dCamBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        }
        {
            WGPUBindGroupLayoutEntry e[5] = {};
            e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 112;
            e[1].binding = 1; e[1].visibility = WGPUShaderStage_Fragment;
            e[1].texture.sampleType = WGPUTextureSampleType_Float;
            e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[2].binding = 2; e[2].visibility = WGPUShaderStage_Fragment;
            e[2].sampler.type = WGPUSamplerBindingType_Filtering;
            e[3].binding = 3; e[3].visibility = WGPUShaderStage_Fragment;   /* metallic-roughness */
            e[3].texture.sampleType = WGPUTextureSampleType_Float;
            e[3].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[4].binding = 4; e[4].visibility = WGPUShaderStage_Fragment;   /* occlusion */
            e[4].texture.sampleType = WGPUTextureSampleType_Float;
            e[4].texture.viewDimension = WGPUTextureViewDimension_2D;
            WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 5; bd.entries = e;
            dv->p3dDrawBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        }
        WGPUBindGroupLayout bl[2] = { dv->p3dCamBgl, dv->p3dDrawBgl };
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl.bindGroupLayoutCount = 2; pl.bindGroupLayouts = bl;
        dv->p3dPl = wgpuDeviceCreatePipelineLayout(dv->device, &pl);
    }
    if (!dv->p3dShader || !dv->p3dPl) return;
    WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
    ct.format = format; ct.writeMask = WGPUColorWriteMask_All;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.depthCompare = WGPUCompareFunction_Less;
    WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
    frag.module = dv->p3dShader; frag.entryPoint = SV("fs"); frag.targetCount = 1; frag.targets = &ct;
    WGPUVertexAttribute attrs[3] = {};
    attrs[0].format = WGPUVertexFormat_Float32x3; attrs[0].offset = 0;  attrs[0].shaderLocation = 0;
    attrs[1].format = WGPUVertexFormat_Float32x3; attrs[1].offset = 12; attrs[1].shaderLocation = 1;
    attrs[2].format = WGPUVertexFormat_Float32x2; attrs[2].offset = 40; attrs[2].shaderLocation = 2;
    WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    vbl.stepMode = WGPUVertexStepMode_Vertex; vbl.arrayStride = 48;
    vbl.attributeCount = 3; vbl.attributes = attrs;
    WGPUVertexState vs = WGPU_VERTEX_STATE_INIT;
    vs.module = dv->p3dShader; vs.entryPoint = SV("vs");
    vs.bufferCount = 1; vs.buffers = &vbl;
    WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rp.layout = dv->p3dPl; rp.vertex = vs; rp.fragment = &frag;
    rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rp.primitive.cullMode = WGPUCullMode_None;
    rp.depthStencil = &ds;
    dv->p3dPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
    dv->p3dFormat = dv->p3dPipeline ? format : WGPUTextureFormat_Undefined;
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

/* PBR 缺省 albedo：1×1 白纹理（材质没给 albedo 贴图时绑定它，基色乘 1）。 */
static void create_white_texture(Device* dv) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ 1, 1, 1 };
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    dv->whiteTex = wgpuDeviceCreateTexture(dv->device, &td);
    if (!dv->whiteTex) return;
    dv->whiteTexView = wgpuTextureCreateView(dv->whiteTex, nullptr);
    const uint8_t px[4] = { 255, 255, 255, 255 };
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = dv->whiteTex; dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = 4; layout.rowsPerImage = 1;
    WGPUExtent3D sz = WGPUExtent3D{ 1, 1, 1 };
    wgpuQueueWriteTexture(dv->queue, &dst, px, sizeof px, &layout, &sz);
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
    /* M2 3D 资源 */
    if (dv->depthView) wgpuTextureViewRelease(dv->depthView);
    if (dv->depthTex) wgpuTextureRelease(dv->depthTex);
    if (dv->p3dPipeline) wgpuRenderPipelineRelease(dv->p3dPipeline);
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
    out->max_lights = 64;      /* MVP：1 方向光 + 点/聚光数组上限（M4 起按设备调） */
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
    sd.maxAnisotropy = 1;
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

/* 3D 网格上传（GPU 常驻）：layout 0 = pos3/normal3/tangent4/uv2（stride 48B）。 */
static int32_t r_create_mesh(void* self, const mq_renderer_mesh_gpu* data, int32_t* out_id) {
    auto* dv = (Device*)self;
    if (!dv || !data || !out_id) return -1;
    if (!data->vertices || data->vertex_bytes == 0) return -2;
    if (!data->indices || data->index_count == 0) return -3;
    if (data->vertex_layout != 0 || data->vertex_stride != 48) return -4;   /* 仅 PBR 规范布局（M2） */

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

/* M2：3D pass（depth + unlit3d）渲染进 target；2D 覆盖层由调用方随后
 * 用 render_impl(target, clear_first=false) 叠加（保持 2D 像素路径完全不变）。 */
static int32_t render_3d_only(Device* dv, WGPUTexture target, WGPUTextureFormat target_format,
                              const mq_renderer_frame3d* f) {
    if (!dv || !target || !f) return 1;
    if (f->view_count == 0 || !f->views) return 0;
    ensure_3d_pipelines(dv, target_format);
    if (!dv->p3dPipeline) return 0;                  /* shader 剔除：3D 跳过（原因已打印） */
    if (!ensure_depth(dv, f->width, f->height)) return 2;
    WGPUTextureView view = wgpuTextureCreateView(target, nullptr);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    /* 方向光/环境光：取第一盏 Directional（M4 再加多光源/阴影）。 */
    float ldir[3] = { 0.0F, -1.0F, 0.0F };
    float lcol[3] = { 1.0F, 1.0F, 1.0F };
    for (uint32_t i = 0; i < f->light_count; ++i) {
        const mq_renderer_light& L = f->lights[i];
        if (L.type != 0) continue;
        ldir[0] = L.dir[0]; ldir[1] = L.dir[1]; ldir[2] = L.dir[2];
        lcol[0] = L.color[0] * L.intensity;
        lcol[1] = L.color[1] * L.intensity;
        lcol[2] = L.color[2] * L.intensity;
        break;
    }
    float ll = std::sqrt(ldir[0] * ldir[0] + ldir[1] * ldir[1] + ldir[2] * ldir[2]);
    if (ll < 1e-6F) { ldir[0] = 0.0F; ldir[1] = -1.0F; ldir[2] = 0.0F; ll = 1.0F; }
    ldir[0] /= ll; ldir[1] /= ll; ldir[2] /= ll;

    for (uint32_t vi = 0; vi < f->view_count; ++vi) {
        const mq_renderer_view3d& v = f->views[vi];
        float vp[16];
        mat4_mul(v.proj, v.view, vp);                /* vp = proj * view */
        float cam[32] = {};
        std::memcpy(cam, vp, sizeof vp);
        cam[16] = v.eye[0]; cam[17] = v.eye[1]; cam[18] = v.eye[2]; cam[19] = v.exposure;
        cam[20] = ldir[0]; cam[21] = ldir[1]; cam[22] = ldir[2]; cam[23] = 0.0F;
        cam[24] = lcol[0]; cam[25] = lcol[1]; cam[26] = lcol[2]; cam[27] = 0.0F;
        cam[28] = f->env.env_color[0] * f->env.intensity;
        cam[29] = f->env.env_color[1] * f->env.intensity;
        cam[30] = f->env.env_color[2] * f->env.intensity;
        cam[31] = 0.0F;
        WGPUBufferDescriptor ubd = WGPU_BUFFER_DESCRIPTOR_INIT;
        ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        ubd.size = sizeof cam;
        WGPUBuffer cbuf = wgpuDeviceCreateBuffer(dv->device, &ubd);
        wgpuQueueWriteBuffer(dv->queue, cbuf, 0, cam, sizeof cam);
        WGPUBindGroupEntry cbe{};
        cbe.binding = 0; cbe.buffer = cbuf; cbe.size = sizeof cam;
        WGPUBindGroupDescriptor cbgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        cbgd.layout = dv->p3dCamBgl; cbgd.entryCount = 1; cbgd.entries = &cbe;
        WGPUBindGroup camBg = wgpuDeviceCreateBindGroup(dv->device, &cbgd);

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
        wgpuRenderPassEncoderSetPipeline(pass, dv->p3dPipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, camBg, 0, nullptr);
        for (uint32_t di = 0; di < f->draw_count; ++di) {
            const mq_renderer_draw3d& d = f->draws[di];
            if (d.mesh < 0 || d.mesh >= (int)dv->meshes.size()) continue;
            MeshGpu* m = dv->meshes[(size_t)d.mesh];
            if (!m || !m->vbo || !m->ibo || m->index_count == 0) continue;
            float du[28] = {};
            std::memcpy(du, d.model, sizeof d.model);              /* 0..15：模型矩阵 */
            const mq_renderer_pbr_material* mat =
                (f->materials && d.material >= 0 && (uint32_t)d.material < f->material_count)
                    ? &f->materials[d.material] : nullptr;
            if (mat) {
                du[16] = mat->base_color[0]; du[17] = mat->base_color[1];
                du[18] = mat->base_color[2]; du[19] = mat->base_color[3];
                du[20] = mat->emissive[0]; du[21] = mat->emissive[1]; du[22] = mat->emissive[2];
                du[24] = mat->metallic;
                du[25] = mat->roughness;
                du[26] = mat->normal_scale;
                du[27] = mat->occlusion_strength;
            } else {
                du[16] = du[17] = du[18] = du[19] = 1.0F;
                du[25] = 0.6F; du[26] = 1.0F; du[27] = 1.0F;
            }
            WGPUBufferDescriptor dbd = WGPU_BUFFER_DESCRIPTOR_INIT;
            dbd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            dbd.size = sizeof du;
            WGPUBuffer dbuf = wgpuDeviceCreateBuffer(dv->device, &dbd);
            wgpuQueueWriteBuffer(dv->queue, dbuf, 0, du, sizeof du);
            WGPUTextureView albedoView = dv->whiteTexView;
            WGPUTextureView mrView = dv->whiteTexView;
            WGPUTextureView aoView = dv->whiteTexView;
            if (mat) {
                if (mat->textures[0] >= 0 && (size_t)mat->textures[0] < dv->textures.size()) {
                    Texture* at = dv->textures[(size_t)mat->textures[0]];
                    if (at && at->view) albedoView = at->view;
                }
                if (mat->textures[1] >= 0 && (size_t)mat->textures[1] < dv->textures.size()) {
                    Texture* at = dv->textures[(size_t)mat->textures[1]];
                    if (at && at->view) mrView = at->view;
                }
                if (mat->textures[2] >= 0 && (size_t)mat->textures[2] < dv->textures.size()) {
                    Texture* at = dv->textures[(size_t)mat->textures[2]];
                    if (at && at->view) aoView = at->view;
                }
            }
            WGPUBindGroupEntry dbe[5] = {};
            dbe[0].binding = 0; dbe[0].buffer = dbuf; dbe[0].size = sizeof du;
            dbe[1].binding = 1; dbe[1].textureView = albedoView;
            dbe[2].binding = 2; dbe[2].sampler = dv->sampler;
            dbe[3].binding = 3; dbe[3].textureView = mrView;
            dbe[4].binding = 4; dbe[4].textureView = aoView;
            WGPUBindGroupDescriptor dbgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            dbgd.layout = dv->p3dDrawBgl; dbgd.entryCount = 5; dbgd.entries = dbe;
            WGPUBindGroup drawBg = wgpuDeviceCreateBindGroup(dv->device, &dbgd);
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
