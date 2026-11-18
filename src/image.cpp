// ============================================================
// src/image.cpp —— 素材采样（把插值从 header 里挪出来）+ mip 上限 + 九宫格边界探测
//
// 为什么采样不写成 inline 头：这段是"每像素都跑"的热路径，写成 inline 会
// 在每个包含 image.h 的 TU 里各生成一份，代码体积上去了、i-cache 也吃亏；
// 而 at/sample 的语义又足够稳定，没有必须内联的理由。
// 九宫格在 raster.cpp（它要落像素，属于光栅层），这里只管"取一个颜色"。
// ============================================================

#include "bin2d/image.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bin2d {
namespace {
int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 相邻两列/两行在容差内是否逐像素相同（含 alpha；预乘空间里 alpha 与颜色同等重要）
bool ColumnMatchesColumn(const Surface& s, int a, int b, int tol) {
    for (int y = 0; y < s.height(); ++y) {
        const uint8_t* p = s.row(y) + static_cast<size_t>(a) * 4;
        const uint8_t* q = s.row(y) + static_cast<size_t>(b) * 4;
        for (int c = 0; c < 4; ++c) {
            if (std::abs(static_cast<int>(p[c]) - static_cast<int>(q[c])) > tol) return false;
        }
    }
    return true;
}

bool RowMatchesRow(const Surface& s, int a, int b, int tol) {
    const uint8_t* p = s.row(a);
    const uint8_t* q = s.row(b);
    for (int i = 0; i < s.width() * 4; ++i) {
        if (std::abs(static_cast<int>(p[i]) - static_cast<int>(q[i])) > tol) return false;
    }
    return true;
}

// 最长的一段连续"相邻相等"。返回 [start, end] 覆盖的**相邻对**下标；
// 也就是 start..end+1 这些列（行）彼此相等。没有长度 ≥2 的段时返回 len = 0。
struct EqualRun {
    int start = 0;
    int len = 0;
};

template <class Fn>
EqualRun LongestEqualRun(int count, Fn equalAdjacent) {
    EqualRun best;
    int run = 0;
    for (int i = 0; i + 1 < count; ++i) {
        if (equalAdjacent(i)) {
            ++run;
            if (run > best.len) {
                best.len = run;
                best.start = i - run + 1;
            }
        } else {
            run = 0;
        }
    }
    return best;
}
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
    int maxLevels = std::max(1, static_cast<int>(std::floor(std::log2(
                                    static_cast<double>(longest)))));
    // 显存上限：级数先夹一次（省掉"生成了又不用"的拷贝）
    if (mipLevelCap_ > 0) maxLevels = std::min(maxLevels, mipLevelCap_);
    int level = static_cast<int>(std::floor(std::log2(static_cast<double>(texelsPerPixel))));
    if (level <= 0) return surface_;
    level = std::min(level, maxLevels);

    while (static_cast<int>(mips_.size()) < level) {
        const Surface& src = mips_.empty() ? surface_ : mips_.back();
        if (src.width() <= 1 && src.height() <= 1) break;
        const int w = std::max(1, src.width() / 2);
        const int h = std::max(1, src.height() / 2);
        // 字节预算：这一级会超就不生成，直接用手上最深的一级
        // （宁可糊一点，也不能让一张大图悄悄吃掉几十 MB）
        if (mipByteBudget_ > 0) {
            const size_t add = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
            if (mipBytes() + add > mipByteBudget_) break;
        }
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

// ------------------------------------------------------------
// 九宫格边界探测
// ------------------------------------------------------------
NineSliceInsets DetectNineSliceInsets(const Image& image, int tolerance) {
    NineSliceInsets out;
    const Surface& s = image.surface();
    if (s.empty() || tolerance < 0) return out;
    const int w = s.width(), h = s.height();
    // 至少要有 2 列 + 2 行才谈得上九宫格（1px 的图没有"边"）
    if (w < 4 || h < 4) return out;

    const EqualRun cols = LongestEqualRun(
        w, [&](int x) { return ColumnMatchesColumn(s, x, x + 1, tolerance); });
    const EqualRun rows = LongestEqualRun(
        h, [&](int y) { return RowMatchesRow(s, y, y + 1, tolerance); });

    // 可拉伸带 = 相等列 [start .. start+len]（len 个相邻对 → len+1 列）。
    // 两侧各至少要留 1 列（否则就是"整张图都能拉伸"，等于没有边界）。
    if (cols.len < 2 || rows.len < 2) return out;
    const int l = cols.start;
    const int r = w - (cols.start + cols.len + 1);
    const int t = rows.start;
    const int b = h - (rows.start + rows.len + 1);
    if (l < 1 || t < 1 || r < 1 || b < 1) return out;

    out.l = static_cast<float>(l);
    out.t = static_cast<float>(t);
    out.r = static_cast<float>(r);
    out.b = static_cast<float>(b);
    out.detected = true;
    return out;
}

} // namespace bin2d
