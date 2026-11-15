#pragma once
// ============================================================
// bin2d/paint.h —— "用什么画"（纯色 / 线性渐变 / 图片）+ 圆角 + 投影
//
// 为什么 Paint 和 Path 分开：
//   形状（Path）与颜料（Paint）是**正交**的两件事 —— 同一个圆环既要能用纯色
//   填充，也要能吃渐变、吃位图；反过来同一份渐变要给矩形、路径、描边共用。
//   合成到一个 struct 里会让"只换颜色"这种最常见的需求复制整条路径几何。
//
// 渐变为什么只做**线性**（不做径向 / 锥形）：
//   径向渐变在 UI 里出现的场合（头像高光、球形按钮）几乎都能用"线性 + 一组 stops"
//   近似，而径向需要额外的椭圆求交、spread 模式与逐像素开方，成本是线性的 3~4 倍。
//   真正需要时再加 `PaintKind::Radial`，接口不预留空洞（现在加会带来没人用的分支）。
//
// 渐变坐标是**局部坐标**（与 Path 同一套）：进光栅化时按"像素中心 → 局部坐标"
// 反算，所以渐变跟着形状一起被变换，放大缩小都正确（不是"先画渐变再拉伸"）。
// ============================================================

#include <cstdint>
#include <vector>

#include "bin2d/core.h"

namespace bin2d {

class Image;   // image.h；这里只存指针，避免 paint.h ↔ image.h 互相 include

// ------------------------------------------------------------
// 渐变端点上的色档
// ------------------------------------------------------------
struct GradientStop {
    float t = 0.0f;      // 0..1（沿渐变轴归一化后的位置）
    Color color;         // 预乘

    GradientStop() = default;
    GradientStop(float tt, const Color& c) : t(tt), color(c) {}

    // 直通色便捷构造：GradientStop(0.0f, 0xFF3E8BFF)
    GradientStop(float tt, uint32_t argb) : t(tt), color(Color::argb(argb)) {}
};

// ------------------------------------------------------------
// 投影（给 Scene 的 Shape/Group 用；一次是通过"偏移 + 盒式模糊 + 组合成"）
// ------------------------------------------------------------
struct Shadow {
    float dx = 0.0f;
    float dy = 0.0f;
    float blur = 0.0f;      // 盒式模糊半径（像素）；0 = 硬边
    // 默认**透明** = 不画投影。默认给个半透明黑会让"每个形状都自带阴影"，
    // 而那是只在局部代码里看得出来的行为（踩过：Scene 与 Canvas 逐像素对比时
    // 差了 3 个色阶，找了一圈才发现是默认阴影）
    Color color = Color::trans();

    bool visible() const { return color.a != 0 && blur >= 0.0f; }
};

// ------------------------------------------------------------
// Paint
// ------------------------------------------------------------
enum class PaintKind : uint8_t {
    Solid = 0,
    Linear = 1,
    Image = 2,
};

class Paint {
public:
    PaintKind kind = PaintKind::Solid;

    // 形状级圆角（半径，局部单位）。fillPath 不看它（路径自带几何）；
    // fillRoundedRect / Scene::Shape 用它 —— 放在这里是因为"圆角"经常和
    // "这张卡片是什么颜色"一起被传下去，单开一个 style 结构体在调用点更啰嗦。
    float radius = 0.0f;

    // ---- Solid ----
    Color color = Color::white();

    // ---- Linear ----
    // 渐变轴：局部坐标里的 p0 → p1。p0 == p1 时退化成 p0 处的色（按末档取）。
    Point g0;
    Point g1;
    std::vector<GradientStop> stops;

    // ---- Image ----
    const Image* image = nullptr;
    Rect srcUv = Rect(0.0f, 0.0f, 1.0f, 1.0f);   // UV 子矩形（0..1，左上原点）
    Color tint = Color::white();                 // 与纹理相乘（预乘）
    bool nearest = false;                        // true = 最近邻采样
    Rect imageDst;                               // 图片在局部坐标里铺到的矩形

    Paint() = default;

    // ---- 工厂：纯色 ----
    static Paint solid(const Color& c) {
        Paint p;
        p.kind = PaintKind::Solid;
        p.color = c;
        return p;
    }
    static Paint solid(uint32_t argb) { return solid(Color::argb(argb)); }

    // ---- 工厂：两点线性渐变 ----
    static Paint linear(const Point& a, const Point& b,
                        std::vector<GradientStop> st) {
        Paint p;
        p.kind = PaintKind::Linear;
        p.g0 = a;
        p.g1 = b;
        p.stops = std::move(st);
        return p;
    }
    // 两色简写（UI 里 90% 就是这个形状）
    static Paint linear(const Point& a, const Point& b, uint32_t c0, uint32_t c1) {
        return linear(a, b, {GradientStop(0.0f, Color::argb(c0)),
                             GradientStop(1.0f, Color::argb(c1))});
    }

    // ---- 工厂：角度线性渐变（CSS 语义：0° = 从下往上，90° = 从左往右）----
    // 端点按 Rect 的"投影长度最大"取（与 CSS linear-gradient 在盒子上的效果一致）。
    static Paint linearAngle(const Rect& box, float degrees,
                             std::vector<GradientStop> st) {
        constexpr float kPi = 3.14159265358979323846f;
        const float a = degrees * kPi / 180.0f;
        // CSS 0° 向上、顺时针增：方向向量 = (sin a, -cos a)
        const float dx = std::sin(a), dy = -std::cos(a);
        const float halfLen = (std::fabs(dx) * box.w + std::fabs(dy) * box.h) * 0.5f;
        const Point c{box.cx(), box.cy()};
        Paint p = linear(Point{c.x - dx * halfLen, c.y - dy * halfLen},
                         Point{c.x + dx * halfLen, c.y + dy * halfLen}, std::move(st));
        return p;
    }
    static Paint linearAngle(const Rect& box, float degrees, uint32_t c0, uint32_t c1) {
        return linearAngle(box, degrees, {GradientStop(0.0f, Color::argb(c0)),
                                          GradientStop(1.0f, Color::argb(c1))});
    }
    // 两色简写（Color 版：调用点经常已经算好了预乘色）
    static Paint linearAngle(const Rect& box, float degrees, const Color& c0,
                             const Color& c1) {
        return linearAngle(box, degrees,
                           {GradientStop(0.0f, c0), GradientStop(1.0f, c1)});
    }

    // ---- 工厂：图片 ----
    static Paint withImage(const Image& img, const Rect& dst, const Rect& uv = Rect(0, 0, 1, 1)) {
        Paint p;
        p.kind = PaintKind::Image;
        p.image = &img;
        p.imageDst = dst;
        p.srcUv = uv;
        return p;
    }

    bool isSolid() const { return kind == PaintKind::Solid; }
    bool isLinear() const { return kind == PaintKind::Linear; }
    bool isImage() const { return kind == PaintKind::Image && image != nullptr; }

    // 一个"代表色"：纯色就是自己；渐变取中档；图片取 tint。
    // 用于投影/退化（比如零长度渐变轴）这类只需要一个颜色的场合。
    Color representative() const {
        if (kind == PaintKind::Solid) return color;
        if (kind == PaintKind::Image) return tint;
        if (stops.empty()) return Color::trans();
        // 中间的色档（不是 0.5 处插值 —— 调用方要的是一个"像这团颜色的色"）
        return stops[stops.size() / 2].color;
    }

    // 渐变在某点（局部坐标）的颜色。轴退化成一点时返回末档。
    Color sampleLinear(float x, float y) const {
        if (stops.empty()) return Color::trans();
        const float ax = g1.x - g0.x, ay = g1.y - g0.y;
        const float len2 = ax * ax + ay * ay;
        if (len2 < 1e-12f) return stops.back().color;
        float t = ((x - g0.x) * ax + (y - g0.y) * ay) / len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return SampleStops(stops, t);
    }

    // stops 的归一化插值：色档要求按 t 升序（调用方保证；乱序由 GradientStopList 排好）
    static Color SampleStops(const std::vector<GradientStop>& st, float t) {
        if (st.empty()) return Color::trans();
        if (t <= st.front().t) return st.front().color;
        if (t >= st.back().t) return st.back().color;
        for (size_t i = 1; i < st.size(); ++i) {
            if (t > st[i].t) continue;
            const float span = st[i].t - st[i - 1].t;
            if (span <= 1e-6f) return st[i].color;
            return lerp(st[i - 1].color, st[i].color, (t - st[i - 1].t) / span);
        }
        return st.back().color;
    }
};

// stops 排序 + 夹紧（t 单调不回退）。返回整理后的列表。
inline std::vector<GradientStop> NormalizeStops(std::vector<GradientStop> st) {
    for (GradientStop& s : st) s.t = s.t < 0.0f ? 0.0f : (s.t > 1.0f ? 1.0f : s.t);
    // 稳定排序：t 相同的保持书写顺序（CSS 用后者覆盖前者）
    for (size_t i = 1; i < st.size(); ++i) {
        size_t j = i;
        while (j > 0 && st[j].t < st[j - 1].t) {
            const GradientStop tmp = st[j];
            st[j] = st[j - 1];
            st[j - 1] = tmp;
            --j;
        }
    }
    return st;
}

// 不透明度折进颜料：纯色直接乘；渐变每档乘；图片折进 tint。
// 单独一个形状（没有重叠）时用它比走离屏图层便宜得多 —— 组透明度才必须离屏。
inline Paint FadePaint(const Paint& src, float k) {
    if (k >= 1.0f) return src;
    Paint p = src;
    if (k <= 0.0f) {
        p.color = Color::trans();
        for (GradientStop& s : p.stops) s.color = Color::trans();
        p.tint = Color::trans();
        return p;
    }
    p.color = p.color * k;
    for (GradientStop& s : p.stops) s.color = s.color * k;
    p.tint = p.tint * k;
    return p;
}
} // namespace bin2d
