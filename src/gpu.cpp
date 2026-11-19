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
#include <cstdio>
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

// ============================================================
// 合成器：混合映射 / 预乘→直通 / 纹理池 / 合批
// ============================================================

BgpuBlend ToBgpuBlend(BlendMode mode) {
    // 语义一一对应，**枚举值不通用**：bin2d 的 Erase=2/Keep=3，bingpu 是 KEEP=2/ERASE=3。
    // 这里刻意逐个列出（不 switch 默认穿透）：将来任何一边加模式，编译器会报缺分支。
    switch (mode) {
    case BlendMode::SrcOver:  return BGPU_BLEND_PREMULTIPLIED;
    case BlendMode::Replace:  return BGPU_BLEND_REPLACE;
    case BlendMode::Erase:    return BGPU_BLEND_ERASE;
    case BlendMode::Keep:     return BGPU_BLEND_KEEP;
    case BlendMode::Additive: return BGPU_BLEND_ADDITIVE;
    }
    return BGPU_BLEND_PREMULTIPLIED;
}

namespace {

// 预乘 → 直通（bingpu 的纹理是直通的，上传时后端会立刻再预乘存起来）。
// 与 Color::toArgbStraight 同一口径：sr = (r*255 + a/2)/a，四舍五入后夹到 255。
void StraightFromPremul(const uint8_t* src, uint8_t* dst, size_t count) {
    for (size_t i = 0; i < count; ++i, src += 4, dst += 4) {
        const uint32_t a = src[3];
        if (a == 0) {
            dst[0] = dst[1] = dst[2] = dst[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = (static_cast<uint32_t>(src[c]) * 255u + a / 2u) / a;
            dst[c] = static_cast<uint8_t>(v > 255u ? 255u : v);
        }
        dst[3] = static_cast<uint8_t>(a);
    }
}

} // namespace

void GpuCompositor::fail(std::string& error, const char* what, BgpuResult result) {
    error = what;
    error += ": ";
    error += ResultName(result);
    const char* detail = bingpu_last_error();
    if (detail && detail[0]) {
        error += " (";
        error += detail;
        error += ")";
    }
}

bool GpuCompositor::create(int width, int height, BgpuBackendKind backend) {
    return createInternal(width, height, backend, nullptr, false);
}

bool GpuCompositor::createWindow(void* nativeWindow, int width, int height,
                                 BgpuBackendKind backend) {
    if (!nativeWindow) { error_ = "createWindow: nativeWindow 为空"; return false; }
    return createInternal(width, height, backend, nativeWindow, true);
}

bool GpuCompositor::createInternal(int width, int height, BgpuBackendKind backend,
                                   void* nativeWindow, bool windowed) {
    destroy();
    if (width <= 0 || height <= 0) return false;
    width_ = width;
    height_ = height;
    backend_ = backend;
    windowed_ = windowed;

    BgpuDeviceDesc dd{};
    dd.struct_size = sizeof(dd);
    dd.backend = backend;
    dd.native_window = nativeWindow;
    dd.width = static_cast<uint32_t>(width);
    dd.height = static_cast<uint32_t>(height);
    // 窗口版不能带 HEADLESS（headless 后端没有呈现路径），其余校验照旧
    dd.flags = BGPU_DEVICE_VALIDATE |
               (windowed ? 0u : static_cast<uint32_t>(BGPU_DEVICE_HEADLESS));
    dd.msaa_samples = 1;
    BgpuResult r = bingpu_device_create(&dd, &device_);
    if (r != BGPU_OK) { fail("bingpu_device_create", r); destroy(); return false; }

    BgpuSwapchainDesc sd{};
    sd.struct_size = sizeof(sd);
    sd.native_window = nativeWindow;   // nullptr = 无窗口：frame_read 出图
    sd.width = static_cast<uint32_t>(width);
    sd.height = static_cast<uint32_t>(height);
    sd.format = BGPU_FORMAT_RGBA8;
    r = bingpu_swapchain_create(device_, &sd, &swapchain_);
    if (r != BGPU_OK) { fail("bingpu_swapchain_create", r); destroy(); return false; }

    // 5 种混合模式各一条管线：合成器每帧只切管线/纹理，不新建任何对象
    for (int i = 0; i < 5; ++i) {
        BgpuPipelineDesc pd{};
        pd.struct_size = sizeof(pd);
        pd.vertex_layout = BGPU_VERTEX_2D;
        pd.blend = static_cast<BgpuBlend>(i);
        pd.flags = BGPU_PIPE_TEXTURED;
        r = bingpu_pipeline_create(device_, &pd, &pipelines_[i]);
        if (r != BGPU_OK) {
            char what[64];
            std::snprintf(what, sizeof(what), "bingpu_pipeline_create(blend=%d)", i);
            fail(what, r);
            destroy();
            return false;
        }
    }

    BgpuSamplerDesc samd{};
    samd.struct_size = sizeof(samd);
    samd.min_filter = BGPU_FILTER_LINEAR;
    samd.mag_filter = BGPU_FILTER_LINEAR;
    samd.wrap_u = BGPU_WRAP_CLAMP;
    samd.wrap_v = BGPU_WRAP_CLAMP;
    r = bingpu_sampler_create(device_, &samd, &samplerLinear_);
    if (r != BGPU_OK) { fail("bingpu_sampler_create(linear)", r); destroy(); return false; }
    samd.min_filter = BGPU_FILTER_NEAREST;
    samd.mag_filter = BGPU_FILTER_NEAREST;
    r = bingpu_sampler_create(device_, &samd, &samplerNearest_);
    if (r != BGPU_OK) { fail("bingpu_sampler_create(nearest)", r); destroy(); return false; }

    r = bingpu_cmd_create(device_, &cmd_);
    if (r != BGPU_OK) { fail("bingpu_cmd_create", r); destroy(); return false; }

    BgpuDeviceInfo info{};
    info.struct_size = sizeof(info);
    if (bingpu_device_info(device_, &info) == BGPU_OK) backend_ = info.backend;
    return true;
}

void GpuCompositor::destroy() {
    if (device_) {
        // 池里的块（空闲 + 借出未还）先销毁：句柄是我们的，设备销毁虽然兜底，
        // 但"谁借出谁回收"这条一旦破例，将来换成真正的池化后端就会漏。
        for (const PoolEntry& e : free_)
            if (e.texture) bingpu_texture_destroy(device_, e.texture);
        for (const PoolEntry& e : live_)
            if (e.texture) bingpu_texture_destroy(device_, e.texture);
        free_.clear();
        live_.clear();
        if (cmd_) { bingpu_cmd_destroy(device_, cmd_); cmd_ = 0; }
        if (samplerNearest_) { bingpu_sampler_destroy(device_, samplerNearest_); samplerNearest_ = 0; }
        if (samplerLinear_) { bingpu_sampler_destroy(device_, samplerLinear_); samplerLinear_ = 0; }
        for (int i = 0; i < 5; ++i) {
            if (pipelines_[i]) { bingpu_pipeline_destroy(device_, pipelines_[i]); pipelines_[i] = 0; }
        }
        if (swapchain_) { bingpu_swapchain_destroy(device_, swapchain_); swapchain_ = 0; }
        bingpu_device_destroy(device_);
        device_ = 0;
    }
    width_ = height_ = 0;
    windowed_ = false;
    target_ = 0;
    stats_ = Stats{};
}

BgpuPipeline GpuCompositor::pipelineFor(BlendMode mode) const {
    // 管线表按 **BgpuBlend** 建（下标 = BgpuBlend 的值），所以这里必须先映射再取 ——
    // 直接 pipelines_[int(mode)] 会让 Erase 拿到 KEEP 的管线（两者枚举值刚好互换），
    // 表现是"擦除之后画面一点没变"（踩过，靠五模式合成对齐测试抓到的）。
    const int i = static_cast<int>(ToBgpuBlend(mode));
    return (i >= 0 && i < 5) ? pipelines_[i] : 0;
}

bool GpuCompositor::findPooled(int w, int h, uint32_t flags, PoolEntry* out) {
    for (size_t i = 0; i < free_.size(); ++i) {
        if (free_[i].w == w && free_[i].h == h && free_[i].flags == flags) {
            *out = free_[i];
            free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

void GpuCompositor::dropLive(BgpuTexture texture) {
    for (size_t i = 0; i < live_.size(); ++i) {
        if (live_[i].texture == texture) {
            live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

BgpuTexture GpuCompositor::acquireInternal(int w, int h, uint32_t flags) {
    if (!device_ || w <= 0 || h <= 0) return 0;
    PoolEntry e;
    if (!findPooled(w, h, flags, &e)) {
        BgpuTextureDesc td{};
        td.struct_size = sizeof(td);
        td.width = static_cast<uint32_t>(w);
        td.height = static_cast<uint32_t>(h);
        td.format = BGPU_FORMAT_RGBA8;
        td.flags = flags;
        const BgpuResult tr = bingpu_texture_create(device_, &td, &e.texture);
        if (tr != BGPU_OK) { fail("bingpu_texture_create", tr); return 0; }
        e.w = w;
        e.h = h;
        e.flags = flags;
    }
    live_.push_back(e);
    return e.texture;
}

// 图层纹理：内容每帧可能被 upload 覆盖；合成只画不读，所以不开 CPU_READ
BgpuTexture GpuCompositor::acquire(int w, int h) {
    return acquireInternal(w, h, BGPU_TEXTURE_DYNAMIC);
}

// 渲染目标：可当渲染目标、可 texture_read（回读时要求两者之一，这里两个都给）
BgpuTexture GpuCompositor::acquireTarget(int w, int h) {
    return acquireInternal(w, h, BGPU_TEXTURE_RENDER_TARGET | BGPU_TEXTURE_CPU_READ);
}

bool GpuCompositor::upload(BgpuTexture texture, const Surface& surface) {
    if (!device_ || !texture || surface.empty()) return false;
    return upload(texture, surface, surface.bounds());
}

bool GpuCompositor::upload(BgpuTexture texture, const Surface& surface, const Rect& box) {
    if (!device_ || !texture || surface.empty()) return false;
    // 裁到表面范围（也裁到负数）——脏区经常带着光栅化时的向外取整，越界是常态
    const int x0 = std::max(0, static_cast<int>(std::floor(box.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(box.y)));
    const int x1 = std::min(surface.width(), static_cast<int>(std::ceil(box.right())));
    const int y1 = std::min(surface.height(), static_cast<int>(std::ceil(box.bottom())));
    if (x1 <= x0 || y1 <= y0) return true;   // 空脏区：没什么可传的，不算失败

    const int bw = x1 - x0, bh = y1 - y0;
    const bool whole = (bw == surface.width() && bh == surface.height());
    std::vector<uint8_t> straight(static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4u);
    for (int y = 0; y < bh; ++y) {
        StraightFromPremul(surface.row(y0 + y) + x0 * 4,
                           straight.data() + static_cast<size_t>(y) * static_cast<size_t>(bw) * 4u,
                           static_cast<size_t>(bw));
    }
    BgpuBox b{};
    b.x = static_cast<uint32_t>(x0);
    b.y = static_cast<uint32_t>(y0);
    b.w = static_cast<uint32_t>(bw);
    b.h = static_cast<uint32_t>(bh);
    const BgpuResult r = bingpu_texture_upload(
        texture, whole ? nullptr : &b, straight.data(), static_cast<uint32_t>(bw) * 4u);
    if (r != BGPU_OK) { fail("bingpu_texture_upload", r); return false; }
    return true;
}

void GpuCompositor::recycle(BgpuTexture texture) {
    if (!device_ || !texture) return;
    for (size_t i = 0; i < live_.size(); ++i) {
        if (live_[i].texture == texture) {
            free_.push_back(live_[i]);
            live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
    // 不是本池借出的句柄：直接销毁（调用方误还也不会漏一块显存）
    bingpu_texture_destroy(device_, texture);
}

void GpuCompositor::purgePool() {
    if (!device_) return;
    for (const PoolEntry& e : free_)
        if (e.texture) bingpu_texture_destroy(device_, e.texture);
    free_.clear();
}

size_t GpuCompositor::pooledBytes() const {
    size_t total = 0;
    for (const PoolEntry& e : free_)
        total += static_cast<size_t>(e.w) * static_cast<size_t>(e.h) * 4u;
    for (const PoolEntry& e : live_)
        total += static_cast<size_t>(e.w) * static_cast<size_t>(e.h) * 4u;
    return total;
}

int GpuCompositor::idleOfSize(int w, int h) const {
    int n = 0;
    for (const PoolEntry& e : free_)
        if (e.w == w && e.h == h) ++n;
    return n;
}

bool GpuCompositor::readBack(Surface& out) {
    const int w = width_, h = height_;
    out.resize(w, h);
    std::vector<uint8_t> straight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    uint32_t stride = 0;
    const BgpuResult r = bingpu_frame_read(swapchain_, straight.data(), straight.size(), &stride);
    if (r != BGPU_OK) { fail("bingpu_frame_read", r); return false; }
    if (stride == 0) stride = static_cast<uint32_t>(w) * 4u;
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
    return true;
}

BgpuResult GpuCompositor::recordLayers(const Layer* layers, size_t count) {
    std::vector<BgpuVertex> batch;
    batch.reserve(4u * 16u);
    bool bound = false;
    BgpuTexture boundTex = 0;
    BgpuSampler boundSampler = 0;
    BgpuPipeline boundPipe = 0;

    auto flush = [&]() -> BgpuResult {
        if (batch.empty()) return BGPU_OK;
        const BgpuResult fr = bingpu_cmd_draw_quads(
            cmd_, batch.data(), static_cast<uint32_t>(batch.size() / 4u));
        ++stats_.draws;
        stats_.quads += static_cast<int>(batch.size() / 4u);
        batch.clear();
        return fr;
    };

    BgpuResult last = BGPU_OK;
    for (size_t i = 0; i < count; ++i) {
        const Layer& L = layers[i];
        if (!L.texture || L.dst.empty() || !L.tint.visible()) continue;
        const BgpuPipeline pipe = pipelineFor(L.blend);
        const BgpuSampler sampler =
            (L.filter == ImageFilter::Nearest) ? samplerNearest_ : samplerLinear_;
        // 状态变了就先结批：同一批里的所有四边形共享"管线 + 纹理 + 采样器"
        if (bound && (pipe != boundPipe || L.texture != boundTex || sampler != boundSampler)) {
            last = flush();
            if (last != BGPU_OK) break;
        }
        if (pipe != boundPipe) {
            last = bingpu_cmd_set_pipeline(cmd_, pipe);
            if (last != BGPU_OK) break;
            boundPipe = pipe;
        }
        if (L.texture != boundTex || sampler != boundSampler) {
            last = bingpu_cmd_set_texture(cmd_, L.texture, sampler);
            if (last != BGPU_OK) break;
            boundTex = L.texture;
            boundSampler = sampler;
            ++stats_.textureBinds;
        }
        bound = true;

        const float x0 = L.dst.x, y0 = L.dst.y;
        const float x1 = L.dst.right(), y1 = L.dst.bottom();
        const float u0 = L.uv.x, v0 = L.uv.y;
        const float u1 = L.uv.right(), v1 = L.uv.bottom();
        const uint32_t argb = L.tint.toArgbStraight();
        // TL, TR, BL, BR（bingpu 的 quad 约定，双线性 UV 因此不会翻面）
        batch.push_back(BgpuVertex{x0, y0, 0.0f, u0, v0, argb});
        batch.push_back(BgpuVertex{x1, y0, 0.0f, u1, v0, argb});
        batch.push_back(BgpuVertex{x0, y1, 0.0f, u0, v1, argb});
        batch.push_back(BgpuVertex{x1, y1, 0.0f, u1, v1, argb});
        ++stats_.layers;
    }
    if (last == BGPU_OK) last = flush();
    if (last != BGPU_OK) fail("bingpu_cmd_draw_quads", last);
    return last;
}

bool GpuCompositor::compose(const Layer* layers, size_t count, Surface& out) {
    return composeInternal(layers, count, &out);
}

bool GpuCompositor::composeInternal(const Layer* layers, size_t count, Surface* out) {
    if (!ready() || (!layers && count)) return false;
    resetStats();

    BgpuResult r = bingpu_frame_begin(swapchain_, cmd_, &target_);
    if (r != BGPU_OK) { fail("bingpu_frame_begin", r); return false; }

    BgpuRenderBeginDesc rb{};
    rb.struct_size = sizeof(rb);
    rb.target = target_;
    rb.flags = BGPU_RENDER_CLEAR_COLOR;
    rb.clear_color = 0x00000000u;      // 清成全透明：与"空图层"的 CPU 初值一致
    r = bingpu_cmd_begin_render(cmd_, &rb);
    if (r != BGPU_OK) { fail("bingpu_cmd_begin_render", r); return false; }

    r = bingpu_cmd_set_viewport(cmd_, 0.0f, 0.0f, static_cast<float>(width_),
                                static_cast<float>(height_));
    if (r != BGPU_OK) fail("bingpu_cmd_set_viewport", r);
    (void)bingpu_cmd_set_scissor(cmd_, 0, 0, width_, height_);
    (void)bingpu_cmd_set_affine(cmd_, nullptr);   // 单位阵：顶点位置就是目标像素

    r = recordLayers(layers, count);

    const BgpuResult rEnd = bingpu_cmd_end_render(cmd_);
    if (rEnd != BGPU_OK) { fail("bingpu_cmd_end_render", rEnd); return false; }
    r = bingpu_frame_end(swapchain_, cmd_);
    if (r != BGPU_OK) { fail("bingpu_frame_end", r); return false; }
    target_ = 0;
    if (!out) return true;    // 窗口呈现：像素已经 present 出去，不付回读的钱
    return readBack(*out);
}

bool GpuCompositor::composeTo(const Layer* layers, size_t count, BgpuTexture target) {
    if (!ready() || !target || (!layers && count)) return false;
    resetStats();

    BgpuRenderBeginDesc rb{};
    rb.struct_size = sizeof(rb);
    rb.target = target;
    rb.flags = BGPU_RENDER_CLEAR_COLOR;
    rb.clear_color = 0x00000000u;
    BgpuResult r = bingpu_cmd_begin_render(cmd_, &rb);
    if (r != BGPU_OK) { fail("bingpu_cmd_begin_render(rt)", r); return false; }

    // 离屏目标的尺寸由纹理自己决定，不是合成器的 width_/height_
    BgpuTextureInfo info{};
    info.struct_size = sizeof(info);
    int tw = width_, th = height_;
    if (bingpu_texture_info(target, &info) == BGPU_OK) {
        tw = static_cast<int>(info.width);
        th = static_cast<int>(info.height);
    }
    r = bingpu_cmd_set_viewport(cmd_, 0.0f, 0.0f, static_cast<float>(tw), static_cast<float>(th));
    if (r != BGPU_OK) fail("bingpu_cmd_set_viewport(rt)", r);
    (void)bingpu_cmd_set_scissor(cmd_, 0, 0, tw, th);
    (void)bingpu_cmd_set_affine(cmd_, nullptr);

    r = recordLayers(layers, count);
    const BgpuResult rEnd = bingpu_cmd_end_render(cmd_);
    if (rEnd != BGPU_OK) { fail("bingpu_cmd_end_render(rt)", rEnd); return false; }
    return r == BGPU_OK;
}

bool GpuCompositor::waitIdle() {
    if (!device_) return false;
    const BgpuResult r = bingpu_device_wait_idle(device_);
    if (r != BGPU_OK) { fail("bingpu_device_wait_idle", r); return false; }
    return true;
}

bool GpuCompositor::readTarget(BgpuTexture target, Surface& out) {
    if (!device_ || !target) return false;
    BgpuTextureInfo info{};
    info.struct_size = sizeof(info);
    if (bingpu_texture_info(target, &info) != BGPU_OK) { fail("bingpu_texture_info", BGPU_ERR_INVALID_ARGUMENT); return false; }
    const int w = static_cast<int>(info.width), h = static_cast<int>(info.height);
    out.resize(w, h);
    std::vector<uint8_t> straight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    uint32_t stride = 0;
    const BgpuResult r = bingpu_texture_read(target, straight.data(), straight.size(), &stride);
    if (r != BGPU_OK) { fail("bingpu_texture_read", r); return false; }
    if (stride == 0) stride = static_cast<uint32_t>(w) * 4u;
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
    return true;
}

bool GpuCompositor::composeOne(const Surface& src, Surface& out) {
    if (!ready() || src.empty()) return false;
    BgpuTexture tex = acquire(src.width(), src.height());
    if (!tex) return false;
    if (!upload(tex, src)) { recycle(tex); return false; }
    Layer layer;
    layer.texture = tex;
    layer.dst = Rect(0.0f, 0.0f, static_cast<float>(src.width()),
                     static_cast<float>(src.height()));
    const bool ok = compose(&layer, 1, out);
    recycle(tex);
    return ok;
}

// 窗口呈现：整张 Surface 铺满交换链 → present。不回读（省 92 万像素的往返）。
bool GpuCompositor::presentSurface(const Surface& src) {
    if (!windowed_) { error_ = "presentSurface: 这个合成器不是窗口版（用 createWindow）"; return false; }
    if (!ready() || src.empty()) return false;
    BgpuTexture tex = acquire(src.width(), src.height());
    if (!tex) return false;
    if (!upload(tex, src)) { recycle(tex); return false; }
    Layer layer;
    layer.texture = tex;
    layer.dst = Rect(0.0f, 0.0f, static_cast<float>(src.width()),
                     static_cast<float>(src.height()));
    const bool ok = composeInternal(&layer, 1, nullptr);
    recycle(tex);
    return ok;
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
