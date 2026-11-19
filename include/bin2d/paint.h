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

    // ---- 渐变的抖动（0.4.0）----
    // 8 位**预乘**存储下，低 alpha 的彩色渐变只剩个位数色阶（实测 alpha=7.8% 时
    // 256px 上只有 9 级），叠在亮底上就是可见的色带。抖动把量化误差摊到相邻像素
    // （Bayer 4×4，±0.5 级），眼睛看到的是"级数与级数之间的平均值"，色带消失。
    // 默认开：这是画质收益，代价只是每像素一次查表；要逐像素复现旧结果（对拍/回归）
    // 关掉即可，此时像素与 0.3.0 逐位相同。
    bool dither = true;

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
    // 这是"每次渐变填充每像素都跑"的热路径，所以保持**直接**实现（不落中间数组、
    // 不做多余的夹紧）：值域天然在 0..255，`(uint8_t)(v+0.5f)` 就是四舍五入。
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

    // 抖动版：量化前加 ±0.5 级的有序抖动（Bayer 4×4）。px/py 是**设备像素**坐标 ——
    // 抖动图样必须钉在屏幕上，否则画面一动就"爬行"（放大/旋转时尤其明显）。
    static Color SampleStopsDithered(const std::vector<GradientStop>& st, float t, int px, int py) {
        float f[4];
        if (!StopsAt(st, t, f)) return Color::trans();
        const float d = Bayer8(px, py);
        for (int i = 0; i < 4; ++i) f[i] += d;
        return FromFloats(f);
    }

    // 8×8 Bayer 阈值，返回 (-0.5, 0.5) 之间的偏移（单位：8 位色阶）
    //
    // 为什么是 8×8 而不是 4×4：抖动能"凭空"造出的中间调数量 = 图样格数。
    // 低 alpha 的彩色渐变本身只有 ~9 级（预乘 8 位），4×4 能把它摊成 17 个半级，
    // 8×8 摊成 33 个（实测）；代价是同一张 64 字节表和一个更绕的取模。
    // 为什么钉在设备像素上：抖动图样的周期必须跟着屏幕走，否则画面一动（滚动、
    // 动画）抖点会"爬行"，那比色带更难看。
    static float Bayer8(int x, int y) {
        static const unsigned char kTable[64] = {
            0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
            12, 44, 4,  36, 14, 46, 6,  38, 60, 28, 52, 20, 62, 30, 54, 22,
            3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25,
            15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};
        const int i = ((y & 7) << 3) | (x & 7);
        return (static_cast<float>(kTable[i]) + 0.5f) / 64.0f - 0.5f;
    }
    static float Bayer4(int x, int y) {
        static const int kTable[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        const int i = ((y & 3) << 2) | (x & 3);
        return (static_cast<float>(kTable[i]) + 0.5f) / 16.0f - 0.5f;
    }

private:
    // 段查找 + **浮点**插值（量化留给调用方：不抖动的走 FromFloats，抖动的先加偏移）
    static bool StopsAt(const std::vector<GradientStop>& st, float t, float out[4]) {
        if (st.empty()) {
            out[0] = out[1] = out[2] = out[3] = 0.0f;
            return false;
        }
        if (t <= st.front().t) { LoadF(st.front().color, out); return true; }
        if (t >= st.back().t) { LoadF(st.back().color, out); return true; }
        for (size_t i = 1; i < st.size(); ++i) {
            if (t > st[i].t) continue;
            const float span = st[i].t - st[i - 1].t;
            if (span <= 1e-6f) { LoadF(st[i].color, out); return true; }
            const float u = (t - st[i - 1].t) / span;
            const Color& a = st[i - 1].color;
            const Color& b = st[i].color;
            out[0] = static_cast<float>(a.r) + (static_cast<float>(b.r) - static_cast<float>(a.r)) * u;
            out[1] = static_cast<float>(a.g) + (static_cast<float>(b.g) - static_cast<float>(a.g)) * u;
            out[2] = static_cast<float>(a.b) + (static_cast<float>(b.b) - static_cast<float>(a.b)) * u;
            out[3] = static_cast<float>(a.a) + (static_cast<float>(b.a) - static_cast<float>(a.a)) * u;
            return true;
        }
        LoadF(st.back().color, out);
        return true;
    }
    static void LoadF(const Color& c, float out[4]) {
        out[0] = c.r;
        out[1] = c.g;
        out[2] = c.b;
        out[3] = c.a;
    }
    // 与 core.h 的 lerp() 同一取整口径（+0.5 截断）。值域：色档 0..255 之间插值，
    // 抖动再加 ±0.5，所以结果必然落在 [0, 255.99]，不需要夹紧。
    static Color FromFloats(const float f[4]) {
        auto q = [](float v) { return static_cast<uint8_t>(v + 0.5f); };
        return Color(q(f[0]), q(f[1]), q(f[2]), q(f[3]));
    }

public:
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

// ------------------------------------------------------------
// 线性渐变的**四角颜色**（0.17.0）：给"用四边形画渐变"的执行体用
//
// 为什么这是一个精确解而不是近似：
//   屏幕空间的线性渐变色是坐标的**仿射函数**，而三角形上的线性插值对仿射函数
//   是精确的。所以"一个四角各取一次色的四边形"和"逐像素求值"在数学上完全相等
//   （差的只有 8 位舍入）。GPU 与 CPU 两条路因此可以共用同一条语义。
//
// 与 `Paint::linearAngle` 的轴完全同一口径（同一个 CSS 角度约定、同一个
// "投影长度最大"的端点取法），否则"同一个渐变用两条执行体画"就会整体偏移。
//
// 输出顺序：左上、右上、左下、右下（与顶点 TL,TR,BL,BR 同序）。
// **只对不透明两端精确**：两端 alpha 不同时，正确做法是在预乘空间插值
// （CSS Color 4），而那要 CPU 侧也改成预乘插值 —— 见 CHANGELOG 的说明。
// ------------------------------------------------------------
inline void LinearGradientCorners(const Rect& box, float degrees, const Color& from,
                                  const Color& to, Color out[4]) {
    constexpr float kPi = 3.14159265358979323846f;
    const float a = degrees * kPi / 180.0f;
    const float dx = std::sin(a), dy = -std::cos(a);
    const float len = std::fabs(dx) * box.w + std::fabs(dy) * box.h;
    const float sx = box.cx() - dx * len * 0.5f;
    const float sy = box.cy() - dy * len * 0.5f;
    const float xs[4] = {box.x, box.right(), box.x, box.right()};
    const float ys[4] = {box.y, box.y, box.bottom(), box.bottom()};
    for (int i = 0; i < 4; ++i) {
        float t = len > 1e-6f ? (((xs[i] - sx) * dx + (ys[i] - sy) * dy) / len) : 0.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        out[i] = Color::straight(static_cast<uint8_t>(from.r + (to.r - from.r) * t + 0.5f),
                                 static_cast<uint8_t>(from.g + (to.g - from.g) * t + 0.5f),
                                 static_cast<uint8_t>(from.b + (to.b - from.b) * t + 0.5f),
                                 static_cast<uint8_t>(from.a + (to.a - from.a) * t + 0.5f));
    }
}
} // namespace bin2d
