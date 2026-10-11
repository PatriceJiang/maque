/* WebGPU 渲染后端：作为插件 .so 实现 DSL 生成的 mq_renderer_v1（maque_ifaces.h）。
 * 仅导出 mq_module_query；运行时经 maque::module(dlopen) 加载。 */
#include "maque_ifaces.h"
#include "core/module/mq_module.h"

#include <webgpu/webgpu.h>

#include <cstdio>
#include <print>
#include <cstring>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <cstdlib>

namespace maque::gfx {

#define SV(x) WGPUStringView { (x), WGPU_STRLEN }

/* 统一 uniform（48B，与 C++ float[12] 逐字段对应；WGSL uniform 布局规则下无隐式填充）：
 *   size(8) view(8) cam(8) zoom rot pan eye(16) pad(8) */
static const char* kSolidWGSL = R"(
struct U {
  size: vec2f, view: vec2f, cam: vec2f,
  zoom: f32, rot: f32, pan: f32, eye: f32, pad: vec2f,
};
@group(0) @binding(0) var<uniform> u: U;
struct Inst { pos: vec2f, radius: f32, rot: f32, color: vec4f };
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f };
@vertex fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
  var c = array<vec2f,6>(vec2f(-1.,-1.), vec2f(1.,-1.), vec2f(-1.,1.),
                         vec2f(-1.,1.), vec2f(1.,-1.), vec2f(1.,1.));
  let inst = insts[ii];
  var corner = c[vi] * inst.radius;
  let cs = cos(inst.rot); let sn = sin(inst.rot);
  corner = vec2f(corner.x * cs - corner.y * sn, corner.x * sn + corner.y * cs);
  var q = (inst.pos + corner - u.cam) * u.zoom;
  let cr = cos(-u.rot); let sr = sin(-u.rot);        // 相机旋转（docs/13 §4）：逆旋转到相机空间
  q = vec2f(q.x * cr - q.y * sr, q.x * sr + q.y * cr);
  let px = q + vec2f(u.size.x * 0.5, u.size.y * 0.5) + vec2f(u.pan, u.eye) + u.view;
  var o: VOut;
  o.p = vec4f(px.x / u.size.x * 2.0 - 1.0, 1.0 - px.y / u.size.y * 2.0, 0., 1.);
  o.uv = c[vi]; o.col = inst.color; return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  if (dot(in.uv, in.uv) > 1.0) { discard; }
  return in.col;                                    // 直通 alpha（混合在管线做）
}
)";

static const char* kTexWGSL = R"(
struct U {
  size: vec2f, view: vec2f, cam: vec2f,
  zoom: f32, rot: f32, pan: f32, eye: f32, pad: vec2f,
};
@group(0) @binding(0) var<uniform> u: U;
struct Inst { pos: vec2f, radius: f32, rot: f32, color: vec4f };
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
@group(0) @binding(2) var t: texture_2d<f32>;
@group(0) @binding(3) var smp: sampler;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f };
@vertex fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
  var c = array<vec2f,6>(vec2f(-1.,-1.), vec2f(1.,-1.), vec2f(-1.,1.),
                         vec2f(-1.,1.), vec2f(1.,-1.), vec2f(1.,1.));
  let inst = insts[ii];
  var corner = c[vi] * inst.radius;
  let cs = cos(inst.rot); let sn = sin(inst.rot);
  corner = vec2f(corner.x * cs - corner.y * sn, corner.x * sn + corner.y * cs);
  var q = (inst.pos + corner - u.cam) * u.zoom;
  let cr = cos(-u.rot); let sr = sin(-u.rot);        // 相机旋转（docs/13 §4）：逆旋转到相机空间
  q = vec2f(q.x * cr - q.y * sr, q.x * sr + q.y * cr);
  let px = q + vec2f(u.size.x * 0.5, u.size.y * 0.5) + vec2f(u.pan, u.eye) + u.view;
  var o: VOut;
  o.p = vec4f(px.x / u.size.x * 2.0 - 1.0, 1.0 - px.y / u.size.y * 2.0, 0., 1.);
  o.uv = c[vi] * 0.5 + 0.5; o.col = inst.color; return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  let c = textureSample(t, smp, in.uv);
  if (c.a < 0.01) { discard; }
  return c * in.col;                                // 直通 alpha（混合在管线做）
}
)";


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
    WGPUSampler sampler = nullptr;
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

    dv->solidShader = make_shader(dv->device, kSolidWGSL);
    {
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
    dv->texShader = make_shader(dv->device, kTexWGSL);
    {
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
    }
    return dv;
}

static void destroy_device(Device* dv) {
    if (!dv) return;
    for (auto* t : dv->textures) {
        if (t->view) wgpuTextureViewRelease(t->view);
        if (t->tex) wgpuTextureRelease(t->tex);
        delete t;
    }
    if (dv->sampler) wgpuSamplerRelease(dv->sampler);
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
struct GpuInst { float pos[2]; float radius; float rot; float color[4]; };

/* 批处理（docs/13 §pass 层）：先按 (layer, z, 原始序) 稳定排序，再按"管线 + 贴图"分批。 */
struct Batch { int texture; uint32_t first, count; };

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
static int32_t r_has_texture(void* self, int32_t id) {
    auto* dv = (Device*)self;
    return (dv && id >= 0 && id < (int)dv->textures.size()) ? 1 : 0;
}

static int32_t r_render_views(void* self, int32_t TW, int32_t TH,
                              const mq_renderer_view* views, uint32_t nviews,
                              const mq_renderer_item* items, uint32_t count,
                              const float* bg, uint8_t* out, uint32_t out_size) {

    Device* dv = (Device*)self;
    if (!dv || !out) return 1;
    if ((uint64_t)TW * TH * 4 > out_size) return 2;
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

    /* 按排序后的相邻关系分批（同层同 z 内相同管线+贴图合并一次 draw）。 */
    std::vector<GpuInst> insts;
    insts.reserve(count);
    std::vector<Batch> batches;
    for (uint32_t k = 0; k < count; ++k) {
        const mq_renderer_item& s = items[order[k]];
        const bool has_tex = s.texture >= 0 && s.texture < (int)dv->textures.size() &&
                             dv->textures[s.texture]->view != nullptr;
        const int tex_id = has_tex ? s.texture : -1;
        GpuInst g{ { s.x, s.y }, s.size, s.rotation, { s.r, s.g, s.b, s.a } };
        if (!batches.empty() && batches.back().texture == tex_id) batches.back().count++;
        else batches.push_back(Batch{ tex_id, (uint32_t)insts.size(), 1 });
        insts.push_back(g);
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

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    { WGPURenderPassEncoder pass = nullptr; begin_pass(enc, view, WGPULoadOp_Clear, bg, &pass);
      wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); }

    for (uint32_t vi = 0; vi < nviews; ++vi) {
        const mq_renderer_view& v = views[vi];
        const float zoom = v.zoom > 0.0F ? v.zoom : 1.0F;
        float uni[12] = { (float)TW, (float)TH, v.vx, v.vy, v.cx, v.cy, zoom, v.rot, v.pan, v.eye, 0.0F, 0.0F };
        WGPUBuffer ubuf = wgpuDeviceCreateBuffer(dv->device, &ubd);
        wgpuQueueWriteBuffer(dv->queue, ubuf, 0, uni, sizeof uni);
        if (ibuf) {
            for (const Batch& b : batches) {
                const bool textured = b.texture >= 0;
                WGPUBindGroupEntry be[4] = {};
                be[0].binding = 0; be[0].buffer = ubuf; be[0].size = sizeof uni;
                be[1].binding = 1; be[1].buffer = ibuf;
                be[1].size = (uint64_t)insts.size() * sizeof(GpuInst);
                WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                WGPUBindGroup bgroup = nullptr;
                if (textured) {
                    be[2].binding = 2; be[2].textureView = dv->textures[b.texture]->view;
                    be[3].binding = 3; be[3].sampler = dv->sampler;
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
                wgpuRenderPassEncoderDraw(pass, 6, b.count, 0, b.first);
                wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
                wgpuBindGroupRelease(bgroup);
            }
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
    if (ibuf) wgpuBufferRelease(ibuf);
    wgpuTextureViewRelease(view); wgpuTextureRelease(tex);
    return st == WGPUMapAsyncStatus_Success ? 0 : 3;
}

} // extern "C"

static const mq_renderer_v1 g_api = {
    { (uint32_t)sizeof(mq_renderer_v1), MQ_ABI_VERSION },
    r_create, r_destroy, r_create_texture, r_has_texture, r_render_views
};

extern "C" MQ_MODULE_EXPORT void* mq_module_query(const char* name, uint32_t min_version) {
    if (min_version > MQ_ABI_VERSION) return nullptr;
    if (name && std::strcmp(name, MQ_RENDERER_API_NAME) == 0) return (void*)&g_api;
    return nullptr;
}
