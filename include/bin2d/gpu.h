#pragma once
// ============================================================
// bin2d/gpu.h —— 与 binGPU 的桥（只依赖 bingpu.h 的 C 接口）
//
// 为什么 bin2d 需要这条路：
//   软件光栅是"任何地方都能跑"的底线（无驱动、可复现、离屏出图），
//   但它撑不住大画布的重复合成。GPU 一帧合成几十个图层是家常便饭 ——
//   所以 bin2d 的图层合成将来可以整块交给 binGPU，而**上层代码一行不改**：
//   传进去的仍然是一张 Surface，出来仍然是一张 Surface。
//
// 为什么只 include bingpu.h、不 include binGPU 的源码：
//   binGPU 是 C API 的动态库；bin2d 只该依赖它的头 + 一个 .a/.dll。
//   这样 bin2d 的构建在没有 binGPU 的环境里仍然成立（GPU 桥不参与默认目标）。
//
// 预乘 vs 直通：Surface 是预乘，bingpu 的纹理是直通 RGBA8，所以 upload/read
// 两侧都要转一次。转换用 core.h 的 toArgbStraight / Color::argb，只有一处口径。
// ============================================================

#include <cstdint>
#include <string>

#include "bingpu.h"
#include "bin2d/core.h"
#include "bin2d/surface.h"

namespace bin2d {

// 一块"离屏四次方"的桥：设备 + 交换链 + 管线 + 命令缓冲 + 采样器。
// 用法：
//   GpuBridge gpu;
//   if (gpu.create(w, h)) { Surface out; gpu.render(src, out); }
class GpuBridge {
public:
    GpuBridge() = default;
    ~GpuBridge() { destroy(); }
    GpuBridge(const GpuBridge&) = delete;
    GpuBridge& operator=(const GpuBridge&) = delete;

    // HEADLESS + CPU 后端：不需要窗口/驱动，纯粹用来验证"这条路上真的跑过一遍"
    bool create(int width, int height, bool preferCpu = true);
    // 同一个回合，但**点名**后端（BGPU_BACKEND_CPU / _VULKAN / _D3D11 / _GL / _AUTO）。
    // 用途：三端一致性验收 —— 同一份 Surface 过每个真实后端，读回来必须还是它。
    // 这个后端不可用时返回 false（error() 里有原因），调用方据此跳过而不是当成失败。
    bool create(int width, int height, BgpuBackendKind backend);
    void destroy();
    bool ready() const { return device_ != 0 && swapchain_ != 0; }

    int width() const { return width_; }
    int height() const { return height_; }

    // 上传一张预乘 Surface 成直通纹理；失败返回 0（调用方负责 destroy）
    BgpuTexture upload(const Surface& surface);
    void releaseTexture(BgpuTexture texture);

    // 把纹理画到 (0,0,w,h) 整块（REPLACE 混合：不跟底色混，便于逐像素对照）
    bool drawQuad(BgpuTexture texture, const Rect& dst, const Rect& srcUv,
                  uint32_t tintArgb = 0xFFFFFFFFu);

    // 一帧完整回合：src → 纹理 → 四边形 → frame_read → out（预乘）
    bool render(const Surface& src, Surface& out);

    const std::string& lastError() const { return error_; }
    // 实际拿到的后端（AUTO 解析之后的那个），以及它的名字（"vulkan" / "d3d11" / "gl" / "cpu"）
    BgpuBackendKind backend() const { return backend_; }
    const char* backendName() const { return bingpu_backend_name(backend_); }

private:
    void fail(const char* what, BgpuResult result);

    BgpuDevice device_ = 0;
    BgpuSwapchain swapchain_ = 0;
    BgpuCmd cmd_ = 0;
    BgpuPipeline pipeline_ = 0;
    BgpuSampler sampler_ = 0;
    int width_ = 0;
    int height_ = 0;
    BgpuBackendKind backend_ = BGPU_BACKEND_AUTO;
    std::string error_;
};

// ---- 低层自由函数（与上面的类等价；给"已经有一台设备/一个 cmd"的调用方）----
// 上传一张预乘 Surface 成直通 RGBA8 纹理。失败返回 0。
BgpuTexture uploadSurface(BgpuDevice device, const Surface& surface);
// 把纹理按 dst 画成四边形（REPLACE 混合）。**调用前**由调用方绑定好
// "VERTEX_2D + TEXTURED + BGPU_BLEND_REPLACE" 的管线与采样器。
bool drawSurfaceQuad(BgpuCmd cmd, BgpuTexture texture, const Rect& dst,
                     uint32_t tintArgb = 0xFFFFFFFFu);

// 便捷函数：一次性把一张 Surface 过一遍 GPU 再读回（内部建/销毁设备）
bool RenderViaGpu(const Surface& src, Surface& out, std::string* error = nullptr);

} // namespace bin2d
