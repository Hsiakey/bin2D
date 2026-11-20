#pragma once
// ============================================================
// bin2d/surface.h —— RGBA8 位图（预乘，行主序，左上原点）
//
// 这是 bin2d 的"画布"：图层、离屏目标、导出、bingpu 上传都走它。
// 格式固定为**预乘 RGBA8**（见 core.h 的理由），所以：
//   · 合成只有一条路径：blendOver(dst, src)；
//   · 导出 PNG 时要反预乘（png.h 里做）；
//   · 传给 bingpu_texture_upload 时要反预乘（gpu.h 里做）。
//
// 保留 stride（行距字节数）而不是硬编码 w*4：将来接外来缓冲（DIB、
// 机位纹理、GPU 回读）时，行距经常是 32 位对齐的，硬编码会把那些
// 路径全部挡在门外。
// ============================================================

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "bin2d/core.h"

namespace bin2d {

namespace surface_detail {
// 实例身份的发号器（0.22.3）。不要求线程安全：Surface 本身就不是线程安全的类型，
// 加锁只会让人以为它能跨线程用。
inline uint64_t NextSurfaceId() {
    static uint64_t next = 0;
    return ++next;
}
} // namespace surface_detail

class Surface {
public:
    // ---- 实例身份（0.22.3）----
    //
    // 为什么需要：GPU 执行体的贴图缓存原来用"像素指针 + 尺寸"当键，而**像素指针会被复用**
    // —— 一张图被销毁后，下一张同样尺寸的图经常拿到同一段内存，于是新图直接命中上一张
    // 已销毁的图的纹理，显示成**旧内容**。字形图集换页时实测就是这个症状（整页文字串格）。
    //
    // 语义：构造时发一个号；**拷贝得到新号**（拷贝是新的像素缓冲，不是同一张图）；
    // 移动把号一起搬走、被移走的那个重新发号（它已不再持有那块像素）。
    // 号只在"是不是同一块内容"这个意义上有用，不暗示任何绘制语义。
    Surface() : id_(surface_detail::NextSurfaceId()) {}
    Surface(int w, int h) : id_(surface_detail::NextSurfaceId()) { resize(w, h); }
    Surface(const Surface& o)
        : id_(surface_detail::NextSurfaceId()), width_(o.width_), height_(o.height_),
          stride_(o.stride_), pixels_(o.pixels_) {}
    Surface& operator=(const Surface& o) {
        if (this == &o) return *this;
        width_ = o.width_;
        height_ = o.height_;
        stride_ = o.stride_;
        pixels_ = o.pixels_;
        id_ = surface_detail::NextSurfaceId();   // 内容换了一份 → 身份也换
        return *this;
    }
    Surface(Surface&& o) noexcept
        : id_(o.id_), width_(o.width_), height_(o.height_), stride_(o.stride_),
          pixels_(std::move(o.pixels_)) {
        o.id_ = surface_detail::NextSurfaceId();
        o.width_ = o.height_ = o.stride_ = 0;
        o.pixels_.clear();
    }
    Surface& operator=(Surface&& o) noexcept {
        if (this == &o) return *this;
        id_ = o.id_;
        width_ = o.width_;
        height_ = o.height_;
        stride_ = o.stride_;
        pixels_ = std::move(o.pixels_);
        o.id_ = surface_detail::NextSurfaceId();
        o.width_ = o.height_ = o.stride_ = 0;
        o.pixels_.clear();
        return *this;
    }

    uint64_t id() const { return id_; }

    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    bool empty() const { return width_ <= 0 || height_ <= 0; }
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    std::vector<uint8_t>& pixels() { return pixels_; }
    Rect bounds() const { return Rect(0.0f, 0.0f, static_cast<float>(width_),
                                      static_cast<float>(height_)); }
    size_t byteSize() const { return static_cast<size_t>(stride_) * static_cast<size_t>(height_); }

    // 重新分配（内容清零）。尺寸非法时变成空画布并返回 false。
    bool resize(int w, int h) {
        if (w <= 0 || h <= 0) {
            width_ = height_ = stride_ = 0;
            pixels_.clear();
            return false;
        }
        width_ = w;
        height_ = h;
        stride_ = w * 4;
        pixels_.assign(static_cast<size_t>(stride_) * static_cast<size_t>(h), 0);
        return true;
    }

    uint8_t* row(int y) { return pixels_.data() + static_cast<size_t>(y) * stride_; }
    const uint8_t* row(int y) const {
        return pixels_.data() + static_cast<size_t>(y) * stride_;
    }

    Color get(int x, int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return Color::trans();
        const uint8_t* p = row(y) + x * 4;
        return Color(p[0], p[1], p[2], p[3]);
    }

    void set(int x, int y, const Color& c) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        uint8_t* p = row(y) + x * 4;
        p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
    }

    // 唯一的合成路径：预乘 src-over
    void blend(int x, int y, const Color& c) {
        if (c.a == 0) return;
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        uint8_t* p = row(y) + x * 4;
        if (c.a == 255) {
            p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
            return;
        }
        const uint32_t inv = 255u - c.a;
        p[0] = static_cast<uint8_t>(c.r + (p[0] * inv + 127u) / 255u);
        p[1] = static_cast<uint8_t>(c.g + (p[1] * inv + 127u) / 255u);
        p[2] = static_cast<uint8_t>(c.b + (p[2] * inv + 127u) / 255u);
        p[3] = static_cast<uint8_t>(c.a + (p[3] * inv + 127u) / 255u);
    }

    void clear(const Color& c = Color::trans()) {
        if (empty()) return;
        if (c.a == 0) {
            std::memset(pixels_.data(), 0, byteSize());
            return;
        }
        for (int y = 0; y < height_; ++y) {
            uint8_t* p = row(y);
            for (int x = 0; x < width_; ++x, p += 4) {
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
            }
        }
    }

    // 把一块矩形**覆盖式**写成给定颜色（0.10.0：脏矩形重画前先把脏区清成透明）。
    // 注意不是 fillRect：fillRect 是 src-over 混合，清透明色在 over 语义下是空操作，
    // 而"擦掉旧内容"必须真的写 0。整数边界向外取整，越界自动裁掉。
    void clearRect(const Rect& r, const Color& c = Color::trans()) {
        if (empty() || r.empty()) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
        const int x1 = std::min(width_, static_cast<int>(std::ceil(r.right())));
        const int y1 = std::min(height_, static_cast<int>(std::ceil(r.bottom())));
        if (x1 <= x0 || y1 <= y0) return;
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = row(y) + static_cast<size_t>(x0) * 4;
            for (int x = x0; x < x1; ++x, p += 4) {
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
            }
        }
    }

    // 整像素矩形填充（轴对齐，字体/条带/调试网格用；带 AA 的形状走 raster）
    void fillRect(const Rect& r, const Color& c) {
        if (empty() || c.a == 0 || r.empty()) return;
        const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
        const int x1 = std::min(width_, static_cast<int>(std::ceil(r.right())));
        const int y1 = std::min(height_, static_cast<int>(std::ceil(r.bottom())));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) blend(x, y, c);
    }

    // 把整张 src 画到 (dx,dy)（预乘 src-over，可整体乘不透明度）
    void blit(const Surface& src, int dx, int dy, float opacity = 1.0f) {
        if (empty() || src.empty() || opacity <= 0.0f) return;
        const int x0 = std::max(0, dx);
        const int y0 = std::max(0, dy);
        const int x1 = std::min(width_, dx + src.width());
        const int y1 = std::min(height_, dy + src.height());
        const bool unit = opacity >= 1.0f;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = src.row(y - dy);
            for (int x = x0; x < x1; ++x) {
                const uint8_t* sp = s + (x - dx) * 4;
                if (sp[3] == 0) continue;
                if (unit) {
                    blend(x, y, Color(sp[0], sp[1], sp[2], sp[3]));
                } else {
                    blend(x, y, Color(static_cast<uint8_t>(sp[0] * opacity + 0.5f),
                                      static_cast<uint8_t>(sp[1] * opacity + 0.5f),
                                      static_cast<uint8_t>(sp[2] * opacity + 0.5f),
                                      static_cast<uint8_t>(sp[3] * opacity + 0.5f)));
                }
            }
        }
    }

    // 整张 src 覆盖式写入（REPLACE 语义：不管底子是什么，按 src 的 alpha 替换）
    void blitReplace(const Surface& src, int dx, int dy) {
        if (empty() || src.empty()) return;
        const int x0 = std::max(0, dx);
        const int y0 = std::max(0, dy);
        const int x1 = std::min(width_, dx + src.width());
        const int y1 = std::min(height_, dy + src.height());
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = src.row(y - dy);
            uint8_t* d = row(y);
            for (int x = x0; x < x1; ++x) {
                const uint8_t* sp = s + (x - dx) * 4;
                uint8_t* dp = d + x * 4;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
            }
        }
    }

private:
    uint64_t id_ = 0;   // 实例身份（见上面的说明）：贴图缓存的键，避免像素指针复用撞车
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    std::vector<uint8_t> pixels_;
};

} // namespace bin2d
