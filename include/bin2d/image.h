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
    Surface& surface() { return surface_; }

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
};

} // namespace bin2d
