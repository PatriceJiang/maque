#include "render/gfx/mq_gfx.h"

#include <webgpu/webgpu.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>

namespace maque::gfx {

#define SV(x) WGPUStringView { (x), WGPU_STRLEN }

static const char* kWGSL = R"(
struct Globals { size: vec2f };
@group(0) @binding(0) var<uniform> g: Globals;
struct Inst { pos: vec2f, radius: f32, _pad: f32, color: vec4f };
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
struct VOut { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) col: vec4f };
@vertex fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
  var c = array<vec2f,6>(vec2f(-1.,-1.), vec2f(1.,-1.), vec2f(-1.,1.),
                         vec2f(-1.,1.), vec2f(1.,-1.), vec2f(1.,1.));
  let corner = c[vi];
  let inst = insts[ii];
  let px = inst.pos + corner * inst.radius;      /* 像素坐标，y 向下 */
  let x = px.x / g.size.x * 2.0 - 1.0;
  let y = 1.0 - px.y / g.size.y * 2.0;
  var o: VOut; o.p = vec4f(x, y, 0., 1.); o.uv = corner; o.col = inst.color; return o;
}
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  if (dot(in.uv, in.uv) > 1.0) { discard; }
  return in.col;
}
)";

struct Device {
    WGPUInstance      instance = nullptr;
    WGPUAdapter       adapter  = nullptr;
    WGPUDevice        device   = nullptr;
    WGPUQueue         queue    = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    WGPUBindGroupLayout bgl = nullptr;
    WGPUPipelineLayout  pl  = nullptr;
    WGPUShaderModule    shader = nullptr;
};

/* ---- ProcessEvents 轮询（本 wgpu-native 构建的 WaitAny 未实现） ---- */
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
    std::fprintf(stderr, "[wgpu] error %d: %.*s\n", (int)type, (int)msg.length, msg.data ? msg.data : "");
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

    // shader
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = SV(kWGSL);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &wgsl.chain;
    dv->shader = wgpuDeviceCreateShaderModule(dv->device, &smd);

    // bind group layout
    WGPUBindGroupLayoutEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex;
    entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    entries[0].buffer.minBindingSize = 16;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Vertex;
    entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    WGPUBindGroupLayoutDescriptor bgl = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bgl.entryCount = 2; bgl.entries = entries;
    dv->bgl = wgpuDeviceCreateBindGroupLayout(dv->device, &bgl);

    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 1; pld.bindGroupLayouts = &dv->bgl;
    dv->pl = wgpuDeviceCreatePipelineLayout(dv->device, &pld);

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = WGPUTextureFormat_RGBA8Unorm;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
    frag.module = dv->shader; frag.entryPoint = SV("fs");
    frag.targetCount = 1; frag.targets = &target;
    WGPUVertexState vert = WGPU_VERTEX_STATE_INIT;
    vert.module = dv->shader; vert.entryPoint = SV("vs");
    WGPURenderPipelineDescriptor rpd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    rpd.layout = dv->pl; rpd.vertex = vert; rpd.fragment = &frag;
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    dv->pipeline = wgpuDeviceCreateRenderPipeline(dv->device, &rpd);

    return dv;
}

void destroy_device(Device* dv) {
    if (!dv) return;
    if (dv->pipeline) wgpuRenderPipelineRelease(dv->pipeline);
    if (dv->pl) wgpuPipelineLayoutRelease(dv->pl);
    if (dv->bgl) wgpuBindGroupLayoutRelease(dv->bgl);
    if (dv->shader) wgpuShaderModuleRelease(dv->shader);
    if (dv->queue) wgpuQueueRelease(dv->queue);
    if (dv->device) wgpuDeviceRelease(dv->device);
    if (dv->adapter) wgpuAdapterRelease(dv->adapter);
    if (dv->instance) wgpuInstanceRelease(dv->instance);
    delete dv;
}

void render_offscreen(Device* dv, int w, int h,
                      const SpriteDraw* sprites, size_t count,
                      const float bg[4],
                      std::vector<uint8_t>& out_rgba) {
    if (!dv || !dv->device) return;

    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    td.dimension = WGPUTextureDimension_2D;
    td.size = WGPUExtent3D{ (uint32_t)w, (uint32_t)h, 1 };
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1; td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(dv->device, &td);
    WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

    // uniform { w, h, 0, 0 }
    float uni[4] = { (float)w, (float)h, 0.f, 0.f };
    WGPUBufferDescriptor ubd = WGPU_BUFFER_DESCRIPTOR_INIT;
    ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    ubd.size = 16;
    WGPUBuffer ubuf = wgpuDeviceCreateBuffer(dv->device, &ubd);
    wgpuQueueWriteBuffer(dv->queue, ubuf, 0, uni, 16);

    // instances
    struct GpuInst { float pos[2]; float radius; float pad; float color[4]; }; // 32B
    std::vector<GpuInst> inst(count);
    for (size_t i = 0; i < count; ++i) {
        inst[i] = GpuInst{ { sprites[i].x, sprites[i].y }, sprites[i].radius, 0.f,
                           { sprites[i].r, sprites[i].g, sprites[i].b, sprites[i].a } };
    }
    WGPUBufferDescriptor ibd = WGPU_BUFFER_DESCRIPTOR_INIT;
    ibd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    ibd.size = (uint64_t)(count ? count : 1) * sizeof(GpuInst);
    WGPUBuffer ibuf = wgpuDeviceCreateBuffer(dv->device, &ibd);
    if (count) wgpuQueueWriteBuffer(dv->queue, ibuf, 0, inst.data(), count * sizeof(GpuInst));

    WGPUBindGroupEntry bge[2] = {};
    bge[0].binding = 0; bge[0].buffer = ubuf; bge[0].offset = 0; bge[0].size = 16;
    bge[1].binding = 1; bge[1].buffer = ibuf; bge[1].offset = 0; bge[1].size = ibd.size;
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = dv->bgl; bgd.entryCount = 2; bgd.entries = bge;
    WGPUBindGroup bgroup = wgpuDeviceCreateBindGroup(dv->device, &bgd);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(dv->device, nullptr);
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = view; ca.loadOp = WGPULoadOp_Clear; ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = WGPUColor{ bg[0], bg[1], bg[2], bg[3] };
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(pass, dv->pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, bgroup, 0, nullptr);
    if (count) wgpuRenderPassEncoderDraw(pass, 6, (uint32_t)count, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    // readback（bytesPerRow 需 256 对齐）
    const uint32_t bytes_per_row = w * 4;
    const uint32_t padded = (bytes_per_row + 255u) & ~255u;
    WGPUBufferDescriptor rbd = WGPU_BUFFER_DESCRIPTOR_INIT;
    rbd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    rbd.size = (uint64_t)padded * h;
    WGPUBuffer rbuf = wgpuDeviceCreateBuffer(dv->device, &rbd);

    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex; src.mipLevel = 0; src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = rbuf; dst.layout.offset = 0; dst.layout.bytesPerRow = padded; dst.layout.rowsPerImage = h;
    WGPUExtent3D extent = WGPUExtent3D{ (uint32_t)w, (uint32_t)h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(dv->queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

    // map + read
    WGPUMapAsyncStatus st = (WGPUMapAsyncStatus)0;
    WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mci.mode = WGPUCallbackMode_AllowProcessEvents; mci.callback = map_cb; mci.userdata1 = &st;
    wgpuBufferMapAsync(rbuf, WGPUMapMode_Read, 0, rbd.size, mci);
    poll(dv->instance, [&]{ return (uint32_t)st != 0; });

    out_rgba.assign((size_t)w * h * 4, 0);
    if (st == WGPUMapAsyncStatus_Success) {
        const uint8_t* mapped = (const uint8_t*)wgpuBufferGetConstMappedRange(rbuf, 0, rbd.size);
        for (int y = 0; y < h; ++y)
            std::memcpy(&out_rgba[(size_t)y * w * 4], mapped + (size_t)y * padded, bytes_per_row);
        wgpuBufferUnmap(rbuf);
    }

    wgpuBufferRelease(rbuf);
    wgpuBindGroupRelease(bgroup);
    wgpuBufferRelease(ibuf);
    wgpuBufferRelease(ubuf);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(tex);
}

} // namespace maque::gfx
