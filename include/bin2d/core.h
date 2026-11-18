#pragma once
// ============================================================
// bin2d/core.h —— 基础类型（颜色 / 矩形 / 版本）
//
// 颜色约定（和 bingpu.h、binUI 一致的一条线）：
//   · 对外接口收 0xAARRGGBB **直通**（binUI 的格式，方便上层对接）；
//   · bin2d **内部一律预乘**（Color 存的就是预乘后的分量）——
//     因为组透明度、图层合成、滤镜只有在预乘空间里才是"乘法"，直通空间里
//     每次都要先除 alpha（alpha=0 的像素会炸）。
//   · PNG 是有 alpha 的直通格式，编码前反预乘（见 png.h）。
//
// 为什么不用 uint32_t 当颜色：预乘 + 4 个分量做乘加时，逐通道算术比
// 位运算更贵一点点，但换来的是"合成代码只有一种写法"。实测这条路径
// 不是瓶颈（瓶颈是内存带宽），所以选可读性。
// ============================================================

#include <cmath>
#include <cstdint>

namespace bin2d {

// ------------------------------------------------------------
// 版本
// ------------------------------------------------------------
#define BIN2D_VERSION_MAJOR 0
#define BIN2D_VERSION_MINOR 9
#define BIN2D_VERSION_PATCH 1
#define BIN2D_VERSION_STRING "0.9.1"

// ------------------------------------------------------------
// 基本几何
// ------------------------------------------------------------
struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Size {
    float w = 0.0f;
    float h = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    constexpr Rect() = default;
    constexpr Rect(float ax, float ay, float aw, float ah) : x(ax), y(ay), w(aw), h(ah) {}

    constexpr bool empty() const { return w <= 0.0f || h <= 0.0f; }
    constexpr float right() const { return x + w; }
    constexpr float bottom() const { return y + h; }
    constexpr float cx() const { return x + w * 0.5f; }
    constexpr float cy() const { return y + h * 0.5f; }
    constexpr bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    // 求交；无交集返回空矩形（w/h <= 0）
    static Rect intersect(const Rect& a, const Rect& b) {
        const float nx = a.x > b.x ? a.x : b.x;
        const float ny = a.y > b.y ? a.y : b.y;
        const float nr = a.right() < b.right() ? a.right() : b.right();
        const float nb = a.bottom() < b.bottom() ? a.bottom() : b.bottom();
        if (nr <= nx || nb <= ny) return Rect(nx, ny, 0.0f, 0.0f);
        return Rect(nx, ny, nr - nx, nb - ny);
    }
    // 向外取整到整像素（光栅化前用，避免半像素缝）
    Rect snappedOut() const {
        const float nx = std::floor(x);
        const float ny = std::floor(y);
        return Rect(nx, ny, std::ceil(right()) - nx, std::ceil(bottom()) - ny);
    }
    Rect inset(float d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
};

// ------------------------------------------------------------
// 颜色（预乘 RGBA8）
// ------------------------------------------------------------
struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 0;   // 全部是**预乘**后的分量：r <= a

    constexpr Color() = default;
    constexpr Color(uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa)
        : r(pr), g(pg), b(pb), a(pa) {}

    static constexpr Color trans() { return Color(0, 0, 0, 0); }
    static constexpr Color black() { return Color(0, 0, 0, 255); }
    static constexpr Color white() { return Color(255, 255, 255, 255); }

    // 直通构造（传进来的 RGB 是"没乘 alpha 的原色"，alpha 是 0..255）
    static constexpr Color straight(uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa = 255) {
        return Color(static_cast<uint8_t>((sr * sa + 127) / 255),
                     static_cast<uint8_t>((sg * sa + 127) / 255),
                     static_cast<uint8_t>((sb * sa + 127) / 255), sa);
    }
    static constexpr Color premul(uint8_t pr, uint8_t pg, uint8_t pb, uint8_t pa) {
        return Color(pr, pg, pb, pa);
    }
    // binUI / bingpu 的 0xAARRGGBB
    static constexpr Color argb(uint32_t v) {
        return straight(static_cast<uint8_t>((v >> 16) & 0xFFu),
                        static_cast<uint8_t>((v >> 8) & 0xFFu),
                        static_cast<uint8_t>(v & 0xFFu),
                        static_cast<uint8_t>((v >> 24) & 0xFFu));
    }

    // 反预乘回直通（PNG / bingpu 上传用）
    uint32_t toArgbStraight() const {
        if (a == 0) return 0;
        const uint32_t sr = static_cast<uint32_t>((r * 255u + a / 2u) / a);
        const uint32_t sg = static_cast<uint32_t>((g * 255u + a / 2u) / a);
        const uint32_t sb = static_cast<uint32_t>((b * 255u + a / 2u) / a);
        const uint32_t cr = sr > 255u ? 255u : sr;
        const uint32_t cg = sg > 255u ? 255u : sg;
        const uint32_t cb = sb > 255u ? 255u : sb;
        return (static_cast<uint32_t>(a) << 24) | (cr << 16) | (cg << 8) | cb;
    }

    constexpr bool visible() const { return a != 0; }
    constexpr bool opaque() const { return a == 255; }
};

// 逐通道乘一个不透明度（预乘空间中就是纯乘法）
inline Color operator*(const Color& c, float k) {
    if (k <= 0.0f) return Color(0, 0, 0, 0);
    if (k >= 1.0f) return c;
    auto s = [k](uint8_t v) {
        const float f = static_cast<float>(v) * k + 0.5f;
        return static_cast<uint8_t>(f > 255.0f ? 255.0f : f);
    };
    return Color(s(c.r), s(c.g), s(c.b), s(c.a));
}

// 两色线性插值（预乘空间里插值是安全的：透明色的预乘分量本来就是 0）
inline Color lerp(const Color& A, const Color& B, float t) {
    if (t <= 0.0f) return A;
    if (t >= 1.0f) return B;
    auto m = [t](uint8_t a, uint8_t b) {
        const float f = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
        return static_cast<uint8_t>(f + 0.5f);
    };
    return Color(m(A.r, B.r), m(A.g, B.g), m(A.b, B.b), m(A.a, B.a));
}

// 预乘 src-over：dst = src + dst * (1 - srcA)。**所有**合成路径都归到这一条。
inline void blendOver(Color& dst, const Color& src) {
    if (src.a == 0) return;
    if (src.a == 255) { dst = src; return; }
    const uint32_t inv = 255u - src.a;          // 1 - srcA，以 255 为 1
    dst.r = static_cast<uint8_t>(src.r + (dst.r * inv + 127u) / 255u);
    dst.g = static_cast<uint8_t>(src.g + (dst.g * inv + 127u) / 255u);
    dst.b = static_cast<uint8_t>(src.b + (dst.b * inv + 127u) / 255u);
    dst.a = static_cast<uint8_t>(src.a + (dst.a * inv + 127u) / 255u);
}

} // namespace bin2d

