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
#include <vector>

#include "bingpu.h"
#include "bin2d/core.h"
#include "bin2d/raster.h"
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

// ============================================================
// 多图层合成器（GpuCompositor）
//
// GpuBridge 证明的是"一帧过一遍 GPU 还是它"；合成器解决的是另一件事：
// **一帧里有 N 个图层**。区别不是"多画几个四边形"，而是三条实现边界：
//
//   1. 一个回合，不是 N 个回合。N 层共用一次 frame_begin/frame_end 和一次
//      frame_read —— 每层各走一回合的话，光回读就要 N 倍带宽。
//   2. 合批。相邻的两个图层只要纹理/混合模式/采样器都一样，就并进**同一次**
//      draw_quads（binGPU 的 quad 数组本来就是给批量准备的）。`Stats` 把
//      "几层 / 几次绘制 / 几次绑纹理" 记下来，合批有没有生效是看得见的数字。
//   3. 纹理池。每帧为一个 1280×720 的图层新建纹理、用完销毁，等于每帧让驱动
//      做一次显存分配；池按 (宽,高) 复用，配合 upload(box) 只传脏矩形，
//      静态图层可以一直躺在显存里不动。
//
// 合成语义（与 CPU 逐像素对齐的唯一定义）：
//   对图层目标矩形内的**每个**像素：
//       src = 纹理采样值（预乘）× tint（预乘）
//       dst = BlendPixel(dst, src, blend)
//   即"整块矩形都参与混合"，不像 Surface::blit 那样跳过 alpha==0 的源像素 ——
//   Replace/Erase/Keep 三种模式下"跳过透明像素"和"写透明像素"是不等价的。
//
// 颜色的两个边界（与 core.h 一致）：
//   · Surface 是预乘；bingpu 纹理是直通 → upload 时反预乘，回读时再预乘。
//   · 顶点色（tint）走 0xAARRGGBB 直通。tint = Color::white() * k（整数不透明度）
//     与不透明彩色 tint 能逐像素精确；**带 alpha 的彩色 tint** 反预乘成 8 位
//     直通会掉精度（≤2），要精确就把 tint 折进纹理。
// ============================================================

// bin2d 的 BlendMode → BgpuBlend 的**唯一**映射处。
// 语义一一对应，但枚举值顺序不同（bin2d 的 Erase=2/Keep=3，bingpu 是 KEEP=2/ERASE=3）——
// 直接强转会让"擦除"和"保留"互换，测试里逐模式验像素就是为了钉死这一点。
BgpuBlend ToBgpuBlend(BlendMode mode);

class GpuCompositor {
public:
    // 一个图层 = 一张纹理 + 目标矩形 + 采样窗口 + 染色 + 混合模式
    struct Layer {
        BgpuTexture texture = 0;         // 必填：池里拿的或自己传的
        Rect dst;                        // 目标空间像素矩形（整数对齐才逐像素精确）
        Rect uv = Rect(0.0f, 0.0f, 1.0f, 1.0f);   // 归一化采样窗口
        Color tint = Color::white();     // 预乘；alpha 就是这一层的组不透明度
        BlendMode blend = BlendMode::SrcOver;
        ImageFilter filter = ImageFilter::Linear;
    };

    // 上一帧的合批证据：N 层如果被合成 1 次绘制，draws 就是 1
    struct Stats {
        int layers = 0;        // 真正提交的层数（空纹理/空矩形/全透明的跳过）
        int draws = 0;         // bingpu_cmd_draw_quads 调用次数
        int quads = 0;         // 四边形总数
        int textureBinds = 0;  // bingpu_cmd_set_texture 次数
    };

    GpuCompositor() = default;
    ~GpuCompositor() { destroy(); }
    GpuCompositor(const GpuCompositor&) = delete;
    GpuCompositor& operator=(const GpuCompositor&) = delete;

    // 建设备 + 交换链 + 5 个混合管线 + 2 个采样器 + cmd。
    // 后端不可用时返回 false（error() 里有原因），调用方据此跳过而不是当失败。
    bool create(int width, int height, BgpuBackendKind backend = BGPU_BACKEND_CPU);
    void destroy();
    bool ready() const { return device_ != 0 && swapchain_ != 0 && cmd_ != 0; }

    int width() const { return width_; }
    int height() const { return height_; }
    BgpuBackendKind backend() const { return backend_; }
    const char* backendName() const { return bingpu_backend_name(backend_); }
    const std::string& lastError() const { return error_; }

    // ---- 纹理池 ----
    // 取一块 w×h 的纹理：池里有同尺寸的空闲块就直接复用（不新建、不分配显存）。
    // 返回的句柄归调用方持有，用完 recycle() 归还（归还**不**销毁）。
    BgpuTexture acquire(int w, int h);
    // 整张覆盖写入（预乘 Surface → 直通 → upload）
    bool upload(BgpuTexture texture, const Surface& surface);
    // 只写脏矩形（box 会被裁到表面与纹理尺寸之内）。box 为空 = 整张。
    bool upload(BgpuTexture texture, const Surface& surface, const Rect& box);
    // 归还：进了空闲表，下一次同尺寸 acquire 会拿到它
    void recycle(BgpuTexture texture);
    // 真正销毁空闲块（显存紧张时调；正在用的不受影响）
    void purgePool();
    int pooledCount() const { return static_cast<int>(free_.size()); }
    size_t pooledBytes() const;
    // 该尺寸的空闲块数（测试用：验证"池真的复用了"）
    int idleOfSize(int w, int h) const;

    // ---- 一帧合成 N 层 ----
    // 顺序即合成顺序（先提交的在下）。out 是预乘 RGBA8，尺寸 = width()×height()。
    // 空纹理 / 空矩形 / tint.a==0 的层被跳过，不计入 Stats::layers。
    bool compose(const Layer* layers, size_t count, Surface& out);
    // 单层满幅（= 老 GpuBridge::render 的等价物，走池子免得每帧新建纹理）
    bool composeOne(const Surface& src, Surface& out);

    // ---- 离屏渲染目标：合到显存里，不回读 ----
    // 回读（frame_read/texture_read）是 Surface→Surface 这条接口的固有成本：
    // 1280×720 一帧要反预乘 92 万像素再预乘一遍。图层套图层、或者"中间结果不需要
    // CPU 看见"的场景，用渲染目标纹理直接接下一层，一步都不落 CPU 内存。
    // acquireTarget 也走池子（同尺寸同用途才复用），用完 recycle。
    BgpuTexture acquireTarget(int w, int h);
    // 把 N 层合到目标纹理上（清成全透明后按顺序画）。不触碰 CPU 内存。
    bool composeTo(const Layer* layers, size_t count, BgpuTexture target);
    // 把目标纹理读回成预乘 Surface（只在真的要 CPU 侧像素时调）
    bool readTarget(BgpuTexture target, Surface& out);
    // 等 GPU 把已提交的工作做完。正常渲染**不要**调（那是在等流水线）；
    // 基准测试和截图前必须调 —— 否则量到的是"命令录制时间"，不是"画完的时间"。
    bool waitIdle();

    Stats stats() const { return stats_; }

private:
    struct PoolEntry {
        BgpuTexture texture = 0;
        int w = 0;
        int h = 0;
        uint32_t flags = 0;   // 池按 (w,h,用途) 分桶：图层纹理和渲染目标不混用
    };
    static void fail(std::string& error, const char* what, BgpuResult result);
    void fail(const char* what, BgpuResult result) { fail(error_, what, result); }
    BgpuPipeline pipelineFor(BlendMode mode) const;
    bool findPooled(int w, int h, uint32_t flags, PoolEntry* out);
    BgpuTexture acquireInternal(int w, int h, uint32_t flags);
    void dropLive(BgpuTexture texture);
    // 记一帧的图层（调用方已经 begin_render 好目标）；内部按状态合批
    BgpuResult recordLayers(const Layer* layers, size_t count);
    void resetStats() { stats_ = Stats{}; }
    bool readBack(Surface& out);

    BgpuDevice device_ = 0;
    BgpuSwapchain swapchain_ = 0;
    BgpuCmd cmd_ = 0;
    BgpuPipeline pipelines_[5] = {0, 0, 0, 0, 0};   // 下标 = BlendMode 的枚举值
    BgpuSampler samplerLinear_ = 0;
    BgpuSampler samplerNearest_ = 0;
    BgpuTexture target_ = 0;   // 帧内借用，不持有
    std::vector<PoolEntry> live_;   // 已借出（调用方持有）
    std::vector<PoolEntry> free_;   // 池里的空闲块
    int width_ = 0;
    int height_ = 0;
    BgpuBackendKind backend_ = BGPU_BACKEND_AUTO;
    Stats stats_{};
    std::string error_;
};

} // namespace bin2d
