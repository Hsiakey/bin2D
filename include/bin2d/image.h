#pragma once
// ============================================================
// bin2d/image.h —— 位图资源（预乘 RGBA8）+ 采样 + 九宫格参数
//
// 为什么和 Surface 分开而不是直接用 Surface：
//   Surface 是"画布"（要能当渲染目标、要能 resize、要能按行访问），
//   Image 是"素材"（只读、可共享、要的只有采样）。
//   混成一个类会让"这张图画到哪"和"这张图本身"绑死 —— 同一个图标画在
//   十个位置，就要十份 Surface 的错觉。Image 内部持有一份 Surface 拷贝，
//   拷贝语义清楚（值类型），共享由调用方用 const Image* / 引用表达。
//
// 采样约定：UV 为 0..1、左上原点、**钳制**（CLAMP_TO_EDGE）—— 九宫格把 1px
// 边拉长时不会出现"边缘变暗"，因为边缘像素被重复而不是插值到透明。
// 双线性在**预乘**空间插值：透明像素的 RGB 本来就是 0，插值不会把黑色渗进来
// （直通空间插值会把透明黑当"颜色黑"，边缘出现黑边 —— 这是最常见的贴图瑕疵）。
// ============================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bin2d/core.h"
#include "bin2d/surface.h"

namespace bin2d {

enum class ImageFilter : uint8_t {
    Linear = 0,
    Nearest = 1,
};

class Image {
public:
    Image() = default;
    Image(int w, int h) { surface_.resize(w, h); }
    explicit Image(const Surface& s) : surface_(s) {}

    bool empty() const { return surface_.empty(); }
    int width() const { return surface_.width(); }
    int height() const { return surface_.height(); }
    Rect bounds() const { return surface_.bounds(); }

    const Surface& surface() const { return surface_; }
    // 非 const 取用 = "我要改像素" → 预缩小副本作废（它们是从旧像素算出来的）
    Surface& surface() {
        mips_.clear();
        return surface_;
    }

    // 缩小时的**预滤波**（mip 的轻量版，0.4.0）：
    // texelsPerPixel > 1 时返回按 2 的幂盒式缩小过的副本，取样仍是双线性。
    // 为什么必须有它：8 倍缩小一张细纹图时双线性只看 4 个纹素，其余高频全折叠成
    // 摩尔纹（实测与理想盒式平均差 31.8 级）；预缩小是"先平均再取样"，
    // 把每一级的高频在缩小前就消掉（0.4.0 实测降到 3 级以内）。
    // 只按 2 的幂分级、不做各向异性：斜看大图会比硬件 mip 再糊一点，但那是"糊"
    // 而不是"摩尔纹"，且省掉一整套 LOD/各向异性状态。
    const Surface& sampleSurface(float texelsPerPixel) const;
    // 已生成几级预缩小副本（0 = 还没生成；诊断/测试用）
    int mipLevelCount() const { return static_cast<int>(mips_.size()); }

    // 预缩小副本占的字节数（诊断 / 显存预算用）
    size_t mipBytes() const {
        size_t total = 0;
        for (const Surface& m : mips_) total += m.byteSize();
        return total;
    }

    // ---- mip 的显存上限（0.8.0）----
    // 两个口径可以并用，哪个先到就停在哪个：
    //   · levelCap：最多生成几级（0 = 不限制；1 = 只到 1/2）；
    //   · byteBudget：预缩小副本总字节上限（0 = 不限制）。
    // 为什么需要：一张 4096×4096 的图满链是 ~22 MB 的额外拷贝，而 9 宫格/图标这类
    // 素材根本用不到第 8 级。调用方按"这张图在界面上最大能放大到多少"设上限即可。
    // 上限定小了的代价是"极小缩放时退回更糙的一级"（会糊一点，不会摩尔纹更重）。
    void setMipLevelCap(int levels) {
        mipLevelCap_ = levels < 0 ? 0 : levels;
    }
    void setMipByteBudget(size_t bytes) { mipByteBudget_ = bytes; }
    int mipLevelCap() const { return mipLevelCap_; }
    size_t mipByteBudget() const { return mipByteBudget_; }
    void clearMips() { mips_.clear(); }

    // 直接按像素坐标取（越界钳制）；实现在 src/image.cpp
    Color at(int x, int y) const;

    // 按 UV 取样（0..1）。双线性用 4 邻域加权，权重按预乘分量算；实现在 src/image.cpp
    Color sample(float u, float v, ImageFilter filter = ImageFilter::Linear) const;

    // 按 UV 子矩形取样：uv 是该子矩形内的 0..1（九宫格/精灵图用）
    Color sampleUv(const Rect& sub, float u, float v,
                   ImageFilter filter = ImageFilter::Linear) const {
        return sample(sub.x + sub.w * u, sub.y + sub.h * v, filter);
    }

    // 用一个回调生成图像（演示/测试造素材用；不引入文件依赖）
    template <class Fn>
    static Image generate(int w, int h, Fn&& fn) {
        Image img(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                img.surface_.set(x, y, fn(x, y));
        return img;
    }

private:
    Surface surface_;
    // 预缩小副本：mips_[i] = 原图的 1/2^(i+1)。惰性生成（只算用到的那几级），
    // 拷贝 Image 会连副本一起拷 —— 素材本来就是"小图"（图标/九宫格），不值得为省这份
    // 内存引入 shared_ptr 与生命周期规则。
    mutable std::vector<Surface> mips_;
    int mipLevelCap_ = 0;          // 0 = 不限制
    size_t mipByteBudget_ = 0;     // 0 = 不限制
};

// ------------------------------------------------------------
// 九宫格的自动边界（0.8.0）
//
// 为什么放在 bin2d：九宫格的 (l,t,r,b) 是**素材的属性**（哪几行/列可以拉伸），
// 不是绘制参数。让每个调用点各自写死 12/16/12/16，换个皮肤就错一个角。
//
// 判别式（刻意简单，且只认"能拉伸"这一件事）：
//   相邻两列（或两行）在容差内**完全相同**，说明这两列之间没有信息，可以被拉伸。
//   取最长的一段连续"可拉伸列"，它的左右边界就是 l 与 r；行同理。
// 适用：圆角卡片、气泡、按钮、滚动条 —— 它们的直边段一定是列/行完全重复的。
// 不适用：整张图都是渐变的"没有边"的图（那种图本来也不需要九宫格），检测会返回
//   detected=false，调用方应当退回普通拉伸（而不是拿 0 当边界去画）。
//
// tolerance 是逐通道最大绝对差（8 位）。默认 1：容忍 AA/压缩噪声，又不至于把
// "相邻两列差 2 级"的细节当可拉伸区。
// ------------------------------------------------------------
struct NineSliceInsets {
    float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
    bool detected = false;   // false = 没找到可靠的拉伸带（l/t/r/b 全 0）
};

NineSliceInsets DetectNineSliceInsets(const Image& image, int tolerance = 1);

} // namespace bin2d
