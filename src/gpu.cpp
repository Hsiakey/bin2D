// ============================================================
// src/gpu.cpp —— binGPU 桥的实现（HEADLESS CPU 设备）
//
// 这里刻意**不**做批量上传、纹理缓存、多帧流水线：bin2d 的 GPU 路径当前只有
// 一个用途 —— 证明"软件画出来的这一帧，过 GPU 一遍之后还是它"。等真的接了
// 实时合成（每帧几十个图层）再谈资源池，那时接口不变、实现换掉。
//
// 顶点顺序 TL,TR,BL,BR 与 bingpu_cmd_draw_quads 的约定一致；tint 走顶点色的
// 0xAARRGGBB 直通口径（bingpu 内部会按预乘处理）。
// ============================================================

#include "bin2d/gpu.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace bin2d {

// 结果码的名字本地化：binGPU 的 bingpu_result_name 不是每个后端构建都会导出
// （CPU 后端这份就没有），而错误文案对调试很重要 —— 自己映射一遍，少一个链接依赖。
static const char* ResultName(BgpuResult r) {
    switch (r) {
    case BGPU_OK: return "BGPU_OK";
    case BGPU_ERR_INVALID_ARGUMENT: return "BGPU_ERR_INVALID_ARGUMENT";
    case BGPU_ERR_UNSUPPORTED: return "BGPU_ERR_UNSUPPORTED";
    case BGPU_ERR_OUT_OF_MEMORY: return "BGPU_ERR_OUT_OF_MEMORY";
    case BGPU_ERR_DEVICE_LOST: return "BGPU_ERR_DEVICE_LOST";
    case BGPU_ERR_INVALID_HANDLE: return "BGPU_ERR_INVALID_HANDLE";
    case BGPU_ERR_STATE: return "BGPU_ERR_STATE";
    case BGPU_ERR_BACKEND_FAILED: return "BGPU_ERR_BACKEND_FAILED";
    case BGPU_ERR_NOT_READY: return "BGPU_ERR_NOT_READY";
    case BGPU_ERR_LIMIT: return "BGPU_ERR_LIMIT";
    default: return "BGPU_ERR_?";
    }
}

void GpuBridge::fail(const char* what, BgpuResult result) {
    error_ = what;
    error_ += ": ";
    error_ += ResultName(result);
    const char* detail = bingpu_last_error();
    if (detail && detail[0]) {
        error_ += " (";
        error_ += detail;
        error_ += ")";
    }
}

bool GpuBridge::create(int width, int height, bool preferCpu) {
    return create(width, height, preferCpu ? BGPU_BACKEND_CPU : BGPU_BACKEND_AUTO);
}

bool GpuBridge::create(int width, int height, BgpuBackendKind backend) {
    destroy();
    if (width <= 0 || height <= 0) return false;
    width_ = width;
    height_ = height;
    backend_ = backend;

    BgpuDeviceDesc dd{};
    dd.struct_size = sizeof(dd);
    dd.backend = backend;
    dd.native_window = nullptr;
    dd.width = static_cast<uint32_t>(width);
    dd.height = static_cast<uint32_t>(height);
    dd.flags = BGPU_DEVICE_HEADLESS | BGPU_DEVICE_VALIDATE;
    dd.msaa_samples = 1;
    BgpuResult r = bingpu_device_create(&dd, &device_);
    if (r != BGPU_OK) { fail("bingpu_device_create", r); destroy(); return false; }

    BgpuSwapchainDesc sd{};
    sd.struct_size = sizeof(sd);
    sd.native_window = nullptr;     // 无窗口：frame_read 出图
    sd.width = static_cast<uint32_t>(width);
    sd.height = static_cast<uint32_t>(height);
    sd.format = BGPU_FORMAT_RGBA8;
    r = bingpu_swapchain_create(device_, &sd, &swapchain_);
    if (r != BGPU_OK) { fail("bingpu_swapchain_create", r); destroy(); return false; }

    BgpuPipelineDesc pd{};
    pd.struct_size = sizeof(pd);
    pd.vertex_layout = BGPU_VERTEX_2D;
    pd.blend = BGPU_BLEND_REPLACE;  // 覆盖：不做预乘混合，逐像素对照更干净
    pd.flags = BGPU_PIPE_TEXTURED;
    r = bingpu_pipeline_create(device_, &pd, &pipeline_);
    if (r != BGPU_OK) { fail("bingpu_pipeline_create", r); destroy(); return false; }

    BgpuSamplerDesc samd{};
    samd.struct_size = sizeof(samd);
    samd.min_filter = BGPU_FILTER_LINEAR;
    samd.mag_filter = BGPU_FILTER_LINEAR;
    samd.wrap_u = BGPU_WRAP_CLAMP;
    samd.wrap_v = BGPU_WRAP_CLAMP;
    r = bingpu_sampler_create(device_, &samd, &sampler_);
    if (r != BGPU_OK) { fail("bingpu_sampler_create", r); destroy(); return false; }

    r = bingpu_cmd_create(device_, &cmd_);
    if (r != BGPU_OK) { fail("bingpu_cmd_create", r); destroy(); return false; }

    /* AUTO 解析之后再问一次：上层要如实知道"这一帧到底跑在哪个后端上"，
     * 三端一致性验收就是按后端名逐个打印的。 */
    BgpuDeviceInfo info{};
    info.struct_size = sizeof(info);
    if (bingpu_device_info(device_, &info) == BGPU_OK) backend_ = info.backend;
    return true;
}

void GpuBridge::destroy() {
    if (device_) {
        // 先子后父（C 没有析构；设备销毁虽会兜底，但正确顺序仍然要写对）
        if (cmd_) { bingpu_cmd_destroy(device_, cmd_); cmd_ = 0; }
        if (sampler_) { bingpu_sampler_destroy(device_, sampler_); sampler_ = 0; }
        if (pipeline_) { bingpu_pipeline_destroy(device_, pipeline_); pipeline_ = 0; }
        if (swapchain_) { bingpu_swapchain_destroy(device_, swapchain_); swapchain_ = 0; }
        bingpu_device_destroy(device_);
        device_ = 0;
    }
    width_ = height_ = 0;
}

BgpuTexture GpuBridge::upload(const Surface& surface) {
    if (!device_ || surface.empty()) return 0;
    const int w = surface.width(), h = surface.height();
    // 预乘 → 直通（bingpu 的纹理是直通 RGBA8）
    std::vector<uint8_t> straight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = surface.row(y);
        uint8_t* q = straight.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
        for (int x = 0; x < w; ++x, p += 4, q += 4) {
            const uint32_t v = Color(p[0], p[1], p[2], p[3]).toArgbStraight();
            q[0] = static_cast<uint8_t>((v >> 16) & 0xFFu);
            q[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
            q[2] = static_cast<uint8_t>(v & 0xFFu);
            q[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        }
    }

    BgpuTextureDesc td{};
    td.struct_size = sizeof(td);
    td.width = static_cast<uint32_t>(w);
    td.height = static_cast<uint32_t>(h);
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_DYNAMIC;
    BgpuTexture tex = 0;
    BgpuResult r = bingpu_texture_create(device_, &td, &tex);
    if (r != BGPU_OK) { fail("bingpu_texture_create", r); return 0; }
    r = bingpu_texture_upload(tex, nullptr, straight.data(),
                              static_cast<uint32_t>(w) * 4u);
    if (r != BGPU_OK) {
        fail("bingpu_texture_upload", r);
        bingpu_texture_destroy(device_, tex);
        return 0;
    }
    return tex;
}

void GpuBridge::releaseTexture(BgpuTexture texture) {
    if (device_ && texture) bingpu_texture_destroy(device_, texture);
}

bool GpuBridge::drawQuad(BgpuTexture texture, const Rect& dst, const Rect& srcUv,
                         uint32_t tintArgb) {
    if (!device_ || !cmd_ || !pipeline_) return false;
    const float x0 = dst.x, y0 = dst.y, x1 = dst.right(), y1 = dst.bottom();
    const float u0 = srcUv.x, v0 = srcUv.y, u1 = srcUv.right(), v1 = srcUv.bottom();
    BgpuVertex verts[4];
    // TL, TR, BL, BR（bingpu 的 quad 约定）
    verts[0] = BgpuVertex{x0, y0, 0.0f, u0, v0, tintArgb};
    verts[1] = BgpuVertex{x1, y0, 0.0f, u1, v0, tintArgb};
    verts[2] = BgpuVertex{x0, y1, 0.0f, u0, v1, tintArgb};
    verts[3] = BgpuVertex{x1, y1, 0.0f, u1, v1, tintArgb};
    BgpuResult r = bingpu_cmd_set_pipeline(cmd_, pipeline_);
    if (r != BGPU_OK) { fail("bingpu_cmd_set_pipeline", r); return false; }
    r = bingpu_cmd_set_texture(cmd_, texture, sampler_);
    if (r != BGPU_OK) { fail("bingpu_cmd_set_texture", r); return false; }
    r = bingpu_cmd_set_affine(cmd_, nullptr);   // 单位阵
    if (r != BGPU_OK) { fail("bingpu_cmd_set_affine", r); return false; }
    r = bingpu_cmd_draw_quads(cmd_, verts, 1);
    if (r != BGPU_OK) { fail("bingpu_cmd_draw_quads", r); return false; }
    return true;
}

bool GpuBridge::render(const Surface& src, Surface& out) {
    if (!ready() || src.empty()) return false;
    const int w = src.width(), h = src.height();
    out.resize(w, h);

    BgpuTexture tex = upload(src);
    if (!tex) return false;

    BgpuTexture target = 0;
    BgpuResult r = bingpu_frame_begin(swapchain_, cmd_, &target);
    if (r != BGPU_OK) { fail("bingpu_frame_begin", r); releaseTexture(tex); return false; }

    BgpuRenderBeginDesc rb{};
    rb.struct_size = sizeof(rb);
    rb.target = target;
    rb.flags = BGPU_RENDER_CLEAR_COLOR;
    rb.clear_color = 0x00000000u;      // 清成全透明，与软件路径的初始状态一致
    r = bingpu_cmd_begin_render(cmd_, &rb);
    if (r != BGPU_OK) { fail("bingpu_cmd_begin_render", r); releaseTexture(tex); return false; }

    r = bingpu_cmd_set_viewport(cmd_, 0.0f, 0.0f, static_cast<float>(w),
                                static_cast<float>(h));
    if (r != BGPU_OK) { fail("bingpu_cmd_set_viewport", r); }
    (void)bingpu_cmd_set_scissor(cmd_, 0, 0, w, h);
    const bool drew = drawQuad(tex, Rect(0, 0, static_cast<float>(w), static_cast<float>(h)),
                               Rect(0, 0, 1, 1));
    r = bingpu_cmd_end_render(cmd_);
    if (r != BGPU_OK) fail("bingpu_cmd_end_render", r);
    r = bingpu_frame_end(swapchain_, cmd_);
    if (r != BGPU_OK) fail("bingpu_frame_end", r);

    std::vector<uint8_t> straight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    uint32_t stride = 0;
    r = bingpu_frame_read(swapchain_, straight.data(), straight.size(), &stride);
    releaseTexture(tex);
    if (r != BGPU_OK) { fail("bingpu_frame_read", r); return false; }
    if (stride == 0) stride = static_cast<uint32_t>(w) * 4u;

    // 直通 → 预乘
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = straight.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x, s += 4) {
            const uint32_t argb = (static_cast<uint32_t>(s[3]) << 24) |
                                  (static_cast<uint32_t>(s[0]) << 16) |
                                  (static_cast<uint32_t>(s[1]) << 8) |
                                  static_cast<uint32_t>(s[2]);
            out.set(x, y, Color::argb(argb));
        }
    }
    return drew;
}

// ---- 自由函数实现 ----
BgpuTexture uploadSurface(BgpuDevice device, const Surface& surface) {
    if (!device || surface.empty()) return 0;
    const int w = surface.width(), h = surface.height();
    std::vector<uint8_t> straight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = surface.row(y);
        uint8_t* q = straight.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u;
        for (int x = 0; x < w; ++x, p += 4, q += 4) {
            const uint32_t v = Color(p[0], p[1], p[2], p[3]).toArgbStraight();
            q[0] = static_cast<uint8_t>((v >> 16) & 0xFFu);
            q[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
            q[2] = static_cast<uint8_t>(v & 0xFFu);
            q[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
        }
    }
    BgpuTextureDesc td{};
    td.struct_size = sizeof(td);
    td.width = static_cast<uint32_t>(w);
    td.height = static_cast<uint32_t>(h);
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = BGPU_TEXTURE_CPU_READ | BGPU_TEXTURE_DYNAMIC;
    BgpuTexture tex = 0;
    if (bingpu_texture_create(device, &td, &tex) != BGPU_OK) return 0;
    if (bingpu_texture_upload(tex, nullptr, straight.data(),
                              static_cast<uint32_t>(w) * 4u) != BGPU_OK) {
        bingpu_texture_destroy(device, tex);
        return 0;
    }
    return tex;
}

bool drawSurfaceQuad(BgpuCmd cmd, BgpuTexture texture, const Rect& dst, uint32_t tintArgb) {
    if (!cmd || !texture) return false;
    const float x0 = dst.x, y0 = dst.y, x1 = dst.right(), y1 = dst.bottom();
    BgpuVertex verts[4] = {
        {x0, y0, 0.0f, 0.0f, 0.0f, tintArgb},   // TL
        {x1, y0, 0.0f, 1.0f, 0.0f, tintArgb},   // TR
        {x0, y1, 0.0f, 0.0f, 1.0f, tintArgb},   // BL
        {x1, y1, 0.0f, 1.0f, 1.0f, tintArgb},   // BR
    };
    if (bingpu_cmd_set_texture(cmd, texture, 0) != BGPU_OK) return false;
    return bingpu_cmd_draw_quads(cmd, verts, 1) == BGPU_OK;
}

bool RenderViaGpu(const Surface& src, Surface& out, std::string* error) {
    GpuBridge bridge;
    if (!bridge.create(src.width(), src.height())) {
        if (error) *error = bridge.lastError();
        return false;
    }
    const bool ok = bridge.render(src, out);
    if (!ok && error) *error = bridge.lastError();
    return ok;
}

} // namespace bin2d
