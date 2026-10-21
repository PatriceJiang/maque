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

/* 内建 shader 来自**资产** assets/builtin/shaders/<name>/（不是内联字面量）：
 * 配置期由 cmake/shader_pack.cmake + tools/shader_pack.js 打包成压缩源码（zstd），
 * 这里按名字取（未编入的 shader → NULL，见 MAQUE_SHADER_<NAME>=OFF 的剔除开关）。
 *   solid    —— 无贴图图元（矩形/圆/方块）
 *   textured —— 贴图图元（图集子矩形 + 采样器），uniform 布局与 solid 一致（48B）
 *   mesh     —— 网格图元（逐顶点 pos/uv/color + 索引），uniform 布局同上（48B） */
struct Texture { WGPUTexture tex = nullptr; WGPUTextureView view = nullptr; };

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
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;                // 直通 alpha 合成
    blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.color.operation = WGPUBlendOperation_Add;
    blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.alpha.operation = WGPUBlendOperation_Add;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_RGBA8Unorm; target.writeMask = WGPUColorWriteMask_All;
    target.blend = &blend;

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
        WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT; frag.module = dv->solidShader; frag.entryPoint = SV("fs"); frag.targetCount = 1; frag.targets = &target;
        WGPUVertexState vert = WGPU_VERTEX_STATE_INIT; vert.module = dv->solidShader; vert.entryPoint = SV("vs");
        WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rp.layout = dv->solidPl; rp.vertex = vert; rp.fragment = &frag; rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        dv->solidPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
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
        WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT; frag.module = dv->texShader; frag.entryPoint = SV("fs"); frag.targetCount = 1; frag.targets = &target;
        WGPUVertexState vert = WGPU_VERTEX_STATE_INIT; vert.module = dv->texShader; vert.entryPoint = SV("vs");
        WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rp.layout = dv->texPl; rp.vertex = vert; rp.fragment = &frag; rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        dv->texPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
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
        /* 顶点布局：交错 stride 48B（pos.xy | uv.xy | color.rgba | darkColor.rgba），与 CPU 侧 MeshVertex 一一对应。
         * dark 一路 = 两色着色（TintBlack）：公式与出处见 assets/builtin/shaders/mesh/shader.wgsl 顶部。 */
        WGPUVertexAttribute mattrs[4] = {};
        mattrs[0].format = WGPUVertexFormat_Float32x2; mattrs[0].offset = 0;  mattrs[0].shaderLocation = 0;
        mattrs[1].format = WGPUVertexFormat_Float32x2; mattrs[1].offset = 8;  mattrs[1].shaderLocation = 1;
        mattrs[2].format = WGPUVertexFormat_Float32x4; mattrs[2].offset = 16; mattrs[2].shaderLocation = 2;
        mattrs[3].format = WGPUVertexFormat_Float32x4; mattrs[3].offset = 32; mattrs[3].shaderLocation = 3;
        WGPUVertexBufferLayout mvbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        mvbl.stepMode = WGPUVertexStepMode_Vertex; mvbl.arrayStride = 48;
        mvbl.attributeCount = 4; mvbl.attributes = mattrs;
        for (int blend = 0; blend < 4; ++blend) {
            for (int pma = 0; pma < 2; ++pma) {
                WGPUBlendState mb = WGPU_BLEND_STATE_INIT;
                if (blend == 1) {                       /* additive */
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
                mt.format = WGPUTextureFormat_RGBA8Unorm; mt.writeMask = WGPUColorWriteMask_All;
                mt.blend = &mb;
                WGPUFragmentState mfrag = WGPU_FRAGMENT_STATE_INIT;
                mfrag.module = dv->meshShader; mfrag.entryPoint = SV("fs"); mfrag.targetCount = 1; mfrag.targets = &mt;
                WGPUVertexState mvert = WGPU_VERTEX_STATE_INIT;
                mvert.module = dv->meshShader; mvert.entryPoint = SV("vs");
                mvert.bufferCount = 1; mvert.buffers = &mvbl;
                WGPURenderPipelineDescriptor mrp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
                mrp.layout = dv->meshPl; mrp.vertex = mvert; mrp.fragment = &mfrag;
                mrp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
                dv->meshPipeline[blend][pma] = wgpuDeviceCreateRenderPipeline(dv->device, &mrp);
            }
        }
    }
    return dv;
}

static void destroy_device(Device* dv) {
    if (!dv) return;
    for (auto* t : dv->textures) {
        if (!t) continue;                  /* 已由 destroy_texture 释放（槽位保留但指针为空） */
        if (t->view) wgpuTextureViewRelease(t->view);
        if (t->tex) wgpuTextureRelease(t->tex);
        delete t;
    }
    if (dv->sampler) wgpuSamplerRelease(dv->sampler);
    if (dv->samplerNearest) wgpuSamplerRelease(dv->samplerNearest);
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
    return (dv && id >= 0 && id < (int)dv->textures.size()) ? 1 : 0;
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

/* 成帧主路径：图元 + 网格 → 同一渲染目标（render_views 是其 meshes == nullptr 的特例）。 */
static int32_t render_impl(void* self, int32_t TW, int32_t TH,
                           const mq_renderer_view* views, uint32_t nviews,
                           const mq_renderer_item* items, uint32_t count,
                           const mq_renderer_mesh* meshes, uint32_t mesh_count,
                           const float* bg, uint8_t* out, uint32_t out_size) {

    Device* dv = (Device*)self;
    if (!dv || !out) return 1;
    if ((uint64_t)TW * TH * 4 > out_size) return 2;
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

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ (uint32_t)TW, (uint32_t)TH, 1 };
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(dv->device, &td);
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
    { WGPURenderPassEncoder pass = nullptr; begin_pass(enc, view, WGPULoadOp_Clear, bg, &pass);
      wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); }

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
                WGPUBindGroupEntry be[3] = {};
                be[0].binding = 0; be[0].buffer = ubuf; be[0].size = sizeof uni;
                be[1].binding = 1; be[1].textureView = dv->textures[c.texture]->view;
                be[2].binding = 2; be[2].sampler = dv->sampler;
                WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                bgd.layout = dv->meshBgl; bgd.entryCount = 3; bgd.entries = be;
                WGPUBindGroup bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);
                WGPURenderPassEncoder pass = nullptr;
                begin_pass(enc, view, WGPULoadOp_Load, bg, &pass);
                /* 裁剪矩形：`clip_w/h > 0` 时用网格自带的（ImGui 的 scissor），否则用整个视图。
                 * 两者取**交集**并夹到视图内，避免越界（WebGPU 要求 scissor 在附件范围内）。 */
                int sx = v.vx, sy = v.vy, sw = v.vw, sh = v.vh;
                if (m.clip_w > 0.0F && m.clip_h > 0.0F) {
                    const int cx = (int)(m.clip_x + 0.5F), cy = (int)(m.clip_y + 0.5F);
                    const int cw = (int)(m.clip_w + 0.5F), ch = (int)(m.clip_h + 0.5F);
                    const int x0 = std::max(sx, cx), y0 = std::max(sy, cy);
                    const int x1 = std::min(sx + sw, cx + cw), y1 = std::min(sy + sh, cy + ch);
                    sx = x0; sy = y0; sw = std::max(0, x1 - x0); sh = std::max(0, y1 - y0);
                    if (sw <= 0 || sh <= 0) { wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); wgpuBindGroupRelease(bgroup); continue; }
                }
                wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)sx, (uint32_t)sy, (uint32_t)sw, (uint32_t)sh);
                wgpuRenderPassEncoderSetPipeline(pass, pipe);
                wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
                wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mvbuf, 0, (uint64_t)mverts.size() * sizeof(MeshVertex));
                wgpuRenderPassEncoderSetIndexBuffer(pass, mibuf, WGPUIndexFormat_Uint32, 0,
                                                    (uint64_t)midx.size() * sizeof(uint32_t));
                wgpuRenderPassEncoderDrawIndexed(pass, m.index_count, 1, ibase[c.first],
                                                 (int32_t)(vbase[c.first] + m.base_vertex), 0);
                wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
                wgpuBindGroupRelease(bgroup);
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
            WGPUBindGroupEntry be[4] = {};
            be[0].binding = 0; be[0].buffer = ubuf; be[0].size = sizeof uni;
            be[1].binding = 1; be[1].buffer = ibuf;
            be[1].size = (uint64_t)insts.size() * sizeof(GpuInst);
            WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            WGPUBindGroup bgroup = nullptr;
            if (textured) {
                be[2].binding = 2; be[2].textureView = dv->textures[c.texture]->view;
                be[3].binding = 3;
                be[3].sampler = c.nearest && dv->samplerNearest ? dv->samplerNearest : dv->sampler;
                bgd.layout = dv->texBgl; bgd.entryCount = 4; bgd.entries = be;
            } else {
                bgd.layout = dv->solidBgl; bgd.entryCount = 2; bgd.entries = be;
            }
            bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);
            WGPURenderPassEncoder pass = nullptr;
            begin_pass(enc, view, WGPULoadOp_Load, bg, &pass);
            wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)v.vx, (uint32_t)v.vy, (uint32_t)v.vw, (uint32_t)v.vh);
            wgpuRenderPassEncoderSetPipeline(pass, textured ? dv->texPipeline : dv->solidPipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 6, c.count, 0, c.first);
            wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
            wgpuBindGroupRelease(bgroup);
        }
        wgpuBufferRelease(ubuf);
    }

    const uint32_t bytes_per_row = TW * 4;
    const uint32_t padded = (bytes_per_row + 255u) & ~255u;
    WGPUBufferDescriptor rbd = WGPU_BUFFER_DESCRIPTOR_INIT;
    rbd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    rbd.size = (uint64_t)padded * TH;
    WGPUBuffer rbuf = wgpuDeviceCreateBuffer(dv->device, &rbd);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; src.texture = tex; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT; dst.buffer = rbuf; dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = TH;
    WGPUExtent3D extent = WGPUExtent3D{ (uint32_t)TW, (uint32_t)TH, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd); wgpuCommandEncoderRelease(enc);

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
    if (mibuf) wgpuBufferRelease(mibuf);
    if (mvbuf) wgpuBufferRelease(mvbuf);
    if (ibuf) wgpuBufferRelease(ibuf);
    wgpuTextureViewRelease(view); wgpuTextureRelease(tex);
    return st == WGPUMapAsyncStatus_Success ? 0 : 3;
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
    r_create, r_destroy, r_destroy_texture, r_create_texture, r_has_texture, r_render_views, r_render_mesh_views
};

extern "C" MQ_MODULE_EXPORT void* mq_module_query(const char* name, uint32_t min_version) {
    if (min_version > MQ_ABI_VERSION) return nullptr;
    if (name && std::strcmp(name, MQ_RENDERER_API_NAME) == 0) return (void*)&g_api;
    return nullptr;
}
