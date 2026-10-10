#include "render/gfx/mq_gfx.h"

#include <webgpu/webgpu.h>

#include <cstdio>
#include <print>
#include <cstring>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace maque::gfx {

#define SV(x) WGPUStringView { (x), WGPU_STRLEN }

/* 实心圆：颜色 * 圆内 */
static const char* kSolidWGSL = R"(
struct Globals { size: vec2f, pan: f32, eye: f32 };
@group(0) @binding(0) var<uniform> g: Globals;
struct Inst { pos: vec2f, radius: f32, _pad: f32, color: vec4f };
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f };
@vertex fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
  var c = array<vec2f,6>(vec2f(-1.,-1.), vec2f(1.,-1.), vec2f(-1.,1.),
                         vec2f(-1.,1.), vec2f(1.,-1.), vec2f(1.,1.));
  let corner = c[vi];
  let inst = insts[ii];
  let px = inst.pos + corner * inst.radius;
  let sx = px.x + g.pan + g.eye;
  let x = sx / g.size.x * 2.0 - 1.0;
  let y = 1.0 - px.y / g.size.y * 2.0;
  var o: VOut; o.p = vec4f(x, y, 0., 1.); o.uv = corner; o.col = inst.color; return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  if (dot(in.uv, in.uv) > 1.0) { discard; }
  return in.col;
}
)";

/* 纹理精灵：采样纹理 * 颜色 */
static const char* kTexWGSL = R"(
struct Globals { size: vec2f, pan: f32, eye: f32 };
@group(0) @binding(0) var<uniform> g: Globals;
struct Inst { pos: vec2f, radius: f32, _pad: f32, color: vec4f };
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
@group(0) @binding(2) var t: texture_2d<f32>;
@group(0) @binding(3) var s: sampler;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f };
@vertex fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
  var c = array<vec2f,6>(vec2f(-1.,-1.), vec2f(1.,-1.), vec2f(-1.,1.),
                         vec2f(-1.,1.), vec2f(1.,-1.), vec2f(1.,1.));
  let corner = c[vi];
  let inst = insts[ii];
  let px = inst.pos + corner * inst.radius;      /* radius = 半尺寸 */
  let sx = px.x + g.pan + g.eye;
  let x = sx / g.size.x * 2.0 - 1.0;
  let y = 1.0 - px.y / g.size.y * 2.0;
  var o: VOut; o.p = vec4f(x, y, 0., 1.); o.uv = corner * 0.5 + 0.5; o.col = inst.color; return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  let c = textureSample(t, s, in.uv);
  if (c.a < 0.01) { discard; }
  return c * in.col;
}
)";

struct Texture {
    WGPUTexture     tex = nullptr;
    WGPUTextureView view = nullptr;
};

struct Device {
    WGPUInstance      instance = nullptr;
    WGPUAdapter       adapter  = nullptr;
    WGPUDevice        device   = nullptr;
    WGPUQueue         queue    = nullptr;
    // 实心
    WGPURenderPipeline solidPipeline = nullptr;
    WGPUBindGroupLayout solidBgl = nullptr;
    WGPUPipelineLayout  solidPl  = nullptr;
    WGPUShaderModule    solidShader = nullptr;
    // 纹理
    WGPURenderPipeline texPipeline = nullptr;
    WGPUBindGroupLayout texBgl = nullptr;
    WGPUPipelineLayout  texPl  = nullptr;
    WGPUShaderModule    texShader = nullptr;
    WGPUSampler         sampler = nullptr;
};

template <class P>
static bool poll(WGPUInstance inst, P ready, int timeout_ms = 20000) {
    auto start = std::chrono::steady_clock::now();
    while (!ready()) {
        wgpuInstanceProcessEvents(inst);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > timeout_ms)
            return false;
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
static void map_cb(WGPUMapAsyncStatus s, WGPUStringView, void* u1, void*) {
    *(WGPUMapAsyncStatus*)u1 = s;
}

static WGPUShaderModule make_shader(WGPUDevice dev, const char* wgsl) {
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = SV(wgsl);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &src.chain;
    return wgpuDeviceCreateShaderModule(dev, &smd);
}

Device* create_device(std::string* out_backend) {
    Device* dv = new Device();

    WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    dv->instance = wgpuCreateInstance(&idesc);
    if (!dv->instance) { delete dv; return nullptr; }

    WGPURequestAdapterOptions aopts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_AllowProcessEvents;
    aci.callback = req_adapter_cb;
    aci.userdata1 = &dv->adapter;
    wgpuInstanceRequestAdapter(dv->instance, &aopts, aci);
    if (!poll(dv->instance, [&]{ return dv->adapter != nullptr; })) { delete dv; return nullptr; }

    if (out_backend) {
        WGPUAdapterInfo inf = WGPU_ADAPTER_INFO_INIT;
        wgpuAdapterGetInfo(dv->adapter, &inf);
        *out_backend = std::string(inf.device.data ? inf.device.data : "", inf.device.length);
        wgpuAdapterInfoFreeMembers(inf);
    }

    WGPUDeviceDescriptor ddesc = WGPU_DEVICE_DESCRIPTOR_INIT;
    ddesc.uncapturedErrorCallbackInfo.callback = on_uncaptured;
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_AllowProcessEvents;
    dci.callback = req_device_cb;
    dci.userdata1 = &dv->device;
    wgpuAdapterRequestDevice(dv->adapter, &ddesc, dci);
    if (!poll(dv->instance, [&]{ return dv->device != nullptr; })) { delete dv; return nullptr; }

    dv->queue = wgpuDeviceGetQueue(dv->device);

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_RGBA8Unorm;
    target.writeMask = WGPUColorWriteMask_All;

    // 实心管线
    dv->solidShader = make_shader(dv->device, kSolidWGSL);
    {
        WGPUBindGroupLayoutEntry e[2] = {};
        e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex; e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 16;
        e[1].binding = 1; e[1].visibility = WGPUShaderStage_Vertex; e[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT; bd.entryCount = 2; bd.entries = e;
        dv->solidBgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bd);
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT; pl.bindGroupLayoutCount = 1; pl.bindGroupLayouts = &dv->solidBgl;
        dv->solidPl = wgpuDeviceCreatePipelineLayout(dv->device, &pl);
        WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT; frag.module = dv->solidShader; frag.entryPoint = SV("fs"); frag.targetCount = 1; frag.targets = &target;
        WGPUVertexState vert = WGPU_VERTEX_STATE_INIT; vert.module = dv->solidShader; vert.entryPoint = SV("vs");
        WGPURenderPipelineDescriptor rp = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        rp.layout = dv->solidPl; rp.vertex = vert; rp.fragment = &frag;
        rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        dv->solidPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
    }

    // 纹理管线
    dv->texShader = make_shader(dv->device, kTexWGSL);
    {
        WGPUBindGroupLayoutEntry e[4] = {};
        e[0].binding = 0; e[0].visibility = WGPUShaderStage_Vertex; e[0].buffer.type = WGPUBufferBindingType_Uniform; e[0].buffer.minBindingSize = 16;
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
        rp.layout = dv->texPl; rp.vertex = vert; rp.fragment = &frag;
        rp.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        dv->texPipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rp);
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.magFilter = WGPUFilterMode_Linear; sd.minFilter = WGPUFilterMode_Linear; sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
        sd.addressModeU = WGPUAddressMode_ClampToEdge; sd.addressModeV = WGPUAddressMode_ClampToEdge; sd.addressModeW = WGPUAddressMode_ClampToEdge;
        dv->sampler = wgpuDeviceCreateSampler(dv->device, &sd);
    }

    return dv;
}

void destroy_device(Device* dv) {
    if (!dv) return;
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

Texture* create_texture(Device* dv, int w, int h, const uint8_t* rgba8) {
    if (!dv || w <= 0 || h <= 0 || !rgba8) return nullptr;
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
    wgpuQueueWriteTexture(dv->queue, &dst, rgba8, (size_t)w * h * 4, &layout, &sz);
    return t;
}

void destroy_texture(Texture* t) {
    if (!t) return;
    if (t->view) wgpuTextureViewRelease(t->view);
    if (t->tex) wgpuTextureRelease(t->tex);
    delete t;
}

struct GpuInst { float pos[2]; float radius; float pad; float color[4]; }; // 32B

static WGPUBuffer make_ibuf(Device* dv, const std::vector<GpuInst>& v) {
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    bd.size = (uint64_t)(v.empty() ? 1 : v.size()) * sizeof(GpuInst);
    WGPUBuffer b = wgpuDeviceCreateBuffer(dv->device, &bd);
    if (!v.empty()) wgpuQueueWriteBuffer(dv->queue, b, 0, v.data(), v.size() * sizeof(GpuInst));
    return b;
}

static void begin_pass(WGPUCommandEncoder enc, WGPUTextureView view, WGPULoadOp load, const float bg[4],
                       WGPURenderPassEncoder* out) {
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = view; ca.loadOp = load; ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = WGPUColor{ bg[0], bg[1], bg[2], bg[3] };
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    *out = wgpuCommandEncoderBeginRenderPass(enc, &rp);
}

/* 多视图渲染（VR/多屏）：同一场景按 views 渲染到目标的不同视口，一次读回。 */
void render_views(Device* dv, int TW, int TH, const View* views, size_t nviews,
                  const SpriteDraw* sprites, size_t count,
                  const float bg[4], std::vector<uint8_t>& out_rgba) {
    if (!dv || !dv->device) return;
    static const View kDefault{ 0, 0, 0, 0, 0, 0 };
    if (nviews == 0 || !views) {
        static View def;
        def = View{ 0, 0, (float)TW, (float)TH, 0, 0 };
        views = &def; nviews = 1;
    }
    (void)kDefault;

    std::vector<GpuInst> solid;
    std::unordered_map<const Texture*, std::vector<GpuInst>> groups;
    for (size_t i = 0; i < count; ++i) {
        const SpriteDraw& s = sprites[i];
        GpuInst g{ { s.x, s.y }, s.radius, 0.f, { s.r, s.g, s.b, s.a } };
        if (s.tex && s.tex->view) groups[s.tex].push_back(g);
        else solid.push_back(g);
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
    ubd.size = 16;

    WGPUBuffer solidBuf = solid.empty() ? nullptr : make_ibuf(dv, solid);
    std::unordered_map<const Texture*, WGPUBuffer> groupBufs;
    for (auto& kv : groups) groupBufs[kv.first] = make_ibuf(dv, kv.second);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);

    // 清屏（整目标一次）
    {
        WGPURenderPassEncoder pass = nullptr;
        begin_pass(enc, view, WGPULoadOp_Clear, bg, &pass);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    for (size_t vi = 0; vi < nviews; ++vi) {
        const View& v = views[vi];
        float uni[4] = { (float)TW, (float)TH, v.pan, v.eye };
        WGPUBuffer ubuf = wgpuDeviceCreateBuffer(dv->device, &ubd);
        wgpuQueueWriteBuffer(dv->queue, ubuf, 0, uni, 16);

        // 实心
        if (solidBuf) {
            WGPUBindGroupEntry be[2] = {};
            be[0].binding = 0; be[0].buffer = ubuf; be[0].size = 16;
            be[1].binding = 1; be[1].buffer = solidBuf; be[1].size = (uint64_t)solid.size() * sizeof(GpuInst);
            WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT; bgd.layout = dv->solidBgl; bgd.entryCount = 2; bgd.entries = be;
            WGPUBindGroup bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);
            WGPURenderPassEncoder pass = nullptr;
            begin_pass(enc, view, WGPULoadOp_Load, bg, &pass);
            wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)v.vx, (uint32_t)v.vy, (uint32_t)v.vw, (uint32_t)v.vh);
            wgpuRenderPassEncoderSetPipeline(pass, dv->solidPipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)solid.size(), 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            wgpuBindGroupRelease(bgroup);
        }

        // 纹理组
        for (auto& kv : groups) {
            const Texture* texh = kv.first;
            WGPUBuffer gb = groupBufs[texh];
            WGPUBindGroupEntry be[4] = {};
            be[0].binding = 0; be[0].buffer = ubuf; be[0].size = 16;
            be[1].binding = 1; be[1].buffer = gb; be[1].size = (uint64_t)kv.second.size() * sizeof(GpuInst);
            be[2].binding = 2; be[2].textureView = texh->view;
            be[3].binding = 3; be[3].sampler = dv->sampler;
            WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT; bgd.layout = dv->texBgl; bgd.entryCount = 4; bgd.entries = be;
            WGPUBindGroup bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);
            WGPURenderPassEncoder pass = nullptr;
            begin_pass(enc, view, WGPULoadOp_Load, bg, &pass);
            wgpuRenderPassEncoderSetScissorRect(pass, (uint32_t)v.vx, (uint32_t)v.vy, (uint32_t)v.vw, (uint32_t)v.vh);
            wgpuRenderPassEncoderSetPipeline(pass, dv->texPipeline);
            wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)kv.second.size(), 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            wgpuBindGroupRelease(bgroup);
        }
        wgpuBufferRelease(ubuf);
    }

    // readback（TW x TH）
    const uint32_t bytes_per_row = TW * 4;
    const uint32_t padded = (bytes_per_row + 255u) & ~255u;
    WGPUBufferDescriptor rbd = WGPU_BUFFER_DESCRIPTOR_INIT;
    rbd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    rbd.size = (uint64_t)padded * TH;
    WGPUBuffer rbuf = wgpuDeviceCreateBuffer(dv->device, &rbd);

    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = rbuf; dst.layout.offset = 0; dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = TH;
    WGPUExtent3D extent = WGPUExtent3D{ (uint32_t)TW, (uint32_t)TH, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    WGPUMapAsyncStatus st = (WGPUMapAsyncStatus)0;
    WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = map_cb; mci.userdata1 = &st;
    wgpuBufferMapAsync(rbuf, WGPUMapMode_Read, 0, rbd.size, mci);
    poll(dv->instance, [&]{ return (uint32_t)st != 0; });

    out_rgba.assign((size_t)TW * TH * 4, 0);
    if (st == WGPUMapAsyncStatus_Success) {
        const uint8_t* mapped = (const uint8_t*)wgpuBufferGetConstMappedRange(rbuf, 0, rbd.size);
        for (int y = 0; y < TH; ++y)
            std::memcpy(&out_rgba[(size_t)y * TW * 4], mapped + (size_t)y * padded, bytes_per_row);
        wgpuBufferUnmap(rbuf);
    }

    wgpuBufferRelease(rbuf);
    if (solidBuf) wgpuBufferRelease(solidBuf);
    for (auto& kv : groupBufs) wgpuBufferRelease(kv.second);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(tex);
}

void render_offscreen(Device* dv, int w, int h,
                      const SpriteDraw* sprites, size_t count,
                      const float bg[4], std::vector<uint8_t>& out_rgba) {
    View v{ 0, 0, (float)w, (float)h, 0, 0 };
    render_views(dv, w, h, &v, 1, sprites, count, bg, out_rgba);
}

} // namespace maque::gfx
