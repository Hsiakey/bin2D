// ============================================================
// src/image.cpp —— 素材采样（把插值从 header 里挪出来）
//
// 为什么采样不写成 inline 头：这段是"每像素都跑"的热路径，写成 inline 会
// 在每个包含 image.h 的 TU 里各生成一份，代码体积上去了、i-cache 也吃亏；
// 而 at/sample 的语义又足够稳定，没有必须内联的理由。
// 九宫格在 raster.cpp（它要落像素，属于光栅层），这里只管"取一个颜色"。
// ============================================================

#include "bin2d/image.h"

#include <algorithm>
#include <cmath>

namespace bin2d {
namespace {
int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

Color Image::at(int x, int y) const {
    if (surface_.empty()) return Color::trans();
    return surface_.get(ClampInt(x, 0, surface_.width() - 1),
                        ClampInt(y, 0, surface_.height() - 1));
}

Color Image::sample(float u, float v, ImageFilter filter) const {
    if (surface_.empty()) return Color::trans();
    const float fx = ClampF(u, 0.0f, 1.0f) * static_cast<float>(surface_.width() - 1);
    const float fy = ClampF(v, 0.0f, 1.0f) * static_cast<float>(surface_.height() - 1);
    if (filter == ImageFilter::Nearest)
        return at(static_cast<int>(fx + 0.5f), static_cast<int>(fy + 0.5f));

    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    // 预乘空间里插值是安全的：透明像素的 RGB 本来就是 0，不会渗出黑边
    const Color top = lerp(at(x0, y0), at(x0 + 1, y0), tx);
    const Color bot = lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx);
    return lerp(top, bot, ty);
}

const Surface& Image::sampleSurface(float texelsPerPixel) const {
    if (surface_.empty() || !(texelsPerPixel > 1.0f)) return surface_;
    const int longest = std::max(surface_.width(), surface_.height());
    // 最多缩到 1 像素：log2(longest) 级
    const int maxLevels = std::max(1, static_cast<int>(std::floor(std::log2(
                                          static_cast<double>(longest)))));
    int level = static_cast<int>(std::floor(std::log2(static_cast<double>(texelsPerPixel))));
    if (level <= 0) return surface_;
    level = std::min(level, maxLevels);

    while (static_cast<int>(mips_.size()) < level) {
        const Surface& src = mips_.empty() ? surface_ : mips_.back();
        if (src.width() <= 1 && src.height() <= 1) break;
        const int w = std::max(1, src.width() / 2);
        const int h = std::max(1, src.height() / 2);
        Surface dst(w, h);
        // 2×2 盒式平均。**在预乘空间平均**是正确做法：透明像素的 RGB 本来就是 0，
        // 边缘不会渗黑（直通空间平均才是错的）。
        for (int y = 0; y < h; ++y) {
            const uint8_t* r0 = src.row(std::min(2 * y, src.height() - 1));
            const uint8_t* r1 = src.row(std::min(2 * y + 1, src.height() - 1));
            uint8_t* out = dst.row(y);
            for (int x = 0; x < w; ++x) {
                const int x0 = std::min(2 * x, src.width() - 1) * 4;
                const int x1 = std::min(2 * x + 1, src.width() - 1) * 4;
                for (int c = 0; c < 4; ++c) {
                    const unsigned sum = static_cast<unsigned>(r0[x0 + c]) + r0[x1 + c] +
                                         r1[x0 + c] + r1[x1 + c];
                    out[x * 4 + c] = static_cast<uint8_t>((sum + 2u) / 4u);
                }
            }
        }
        mips_.push_back(std::move(dst));
    }
    if (mips_.empty()) return surface_;
    const size_t idx = std::min(static_cast<size_t>(level), mips_.size()) - 1;
    return mips_[idx];
}

} // namespace bin2d
