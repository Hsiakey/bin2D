#pragma once
// ============================================================
// bin2d/path.h —— 矢量路径（命令级存储 + 按需展平）
//
// 为什么存"命令"而不是像上层库的路径段那样直接存展平后的折线：
//   bin2d 里同一条路径会被**反复**用在不同变换下（缩放动画、缩放后导出、
//   命中测试），展平结果与缩放比绑定 —— 存折线的库一放大就出多边形棱角，
//   或者得按每次变换重新细分，反而更贵。这里存贝塞尔控制点，展平时按
//   **目标精度**细分：变换放大了，精度自动跟着提（容差是按设备像素给的）。
//
// 展平精度约定：`flatten(tolerance)` 的 tolerance 是**最大弦高误差**（用户单位，
// 调用方负责把"设备像素"折算成用户单位）。递归细分的终止条件是"控制点到弦的
// 距离 ≤ tolerance"，二次/三次贝塞尔的控制点到弦距离是实际曲线偏离弦的**上界**，
// 所以真实误差只会更小（通常小 3~4 倍）。
//
// bounds() 是**精确**包围盒（二次/三次曲线取导数零点），不是控制点的外包盒 ——
// 后者在强弯曲的曲线上会大出 30% 以上，图层分配和裁剪都跟着浪费。
// ============================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "bin2d/core.h"
#include "bin2d/transform.h"

namespace bin2d {

enum class FillRule : uint8_t {
    NonZero = 0,
    EvenOdd = 1,
};

// 展平后的折线（一条子路径）
struct Polyline {
    std::vector<Point> pts;
    bool closed = false;
};

class Path {
public:
    enum class Verb : uint8_t {
        Move = 0,   // 起一个新子路径
        Line = 1,   // 直线到 p2
        Quad = 2,   // 二次贝塞尔：控制点 p0，终点 p1
        Cubic = 3,  // 三次贝塞尔：控制点 p0/p1，终点 p2
        Close = 4,  // 闭合当前子路径
    };

    struct Command {
        Verb verb = Verb::Move;
        Point p0, p1, p2;   // 按 verb 解释（未用的点是 {0,0}，读代码时别被误导）
    };

    Path() = default;

    // ------------------------------------------------------------
    // 构建
    // ------------------------------------------------------------
    Path& moveTo(float x, float y) {
        Command n;
        n.verb = Verb::Move;
        n.p2 = Point{x, y};
        cmds_.push_back(n);
        return *this;
    }
    Path& moveTo(const Point& p) { return moveTo(p.x, p.y); }

    Path& lineTo(float x, float y) {
        if (cmds_.empty()) return moveTo(x, y);   // SVG 语义：首个 lineTo 当 moveTo
        Command n;
        n.verb = Verb::Line;
        n.p2 = Point{x, y};
        cmds_.push_back(n);
        return *this;
    }
    Path& lineTo(const Point& p) { return lineTo(p.x, p.y); }

    Path& quadTo(float cx, float cy, float x, float y) {
        if (cmds_.empty()) return moveTo(cx, cy);
        Command n;
        n.verb = Verb::Quad;
        n.p0 = Point{cx, cy};
        n.p1 = Point{x, y};
        cmds_.push_back(n);
        return *this;
    }
    Path& quadTo(const Point& c, const Point& p) { return quadTo(c.x, c.y, p.x, p.y); }

    Path& cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        if (cmds_.empty()) return moveTo(c1x, c1y);
        Command n;
        n.verb = Verb::Cubic;
        n.p0 = Point{c1x, c1y};
        n.p1 = Point{c2x, c2y};
        n.p2 = Point{x, y};
        cmds_.push_back(n);
        return *this;
    }
    Path& cubicTo(const Point& c1, const Point& c2, const Point& p) {
        return cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
    }

    // 闭合当前子路径（回到该子路径的起点）
    Path& close() {
        if (!cmds_.empty()) {
            Command n;
            n.verb = Verb::Close;
            cmds_.push_back(n);
        }
        return *this;
    }

    // ---- 常用形状 ----
    void addRect(const Rect& r) {
        moveTo(r.x, r.y)
            .lineTo(r.right(), r.y)
            .lineTo(r.right(), r.bottom())
            .lineTo(r.x, r.bottom())
            .close();
    }

    // 圆角矩形：半径先夹到 min(w,h)/2（超了就是胶囊/圆），四角用 kappa 三次贝塞尔。
    // 与上层库的圆角填充用同一套弧近似（kappa = 0.5522847），三端差异 ≤ 0.05px。
    void addRoundedRect(const Rect& r, float radius) {
        if (r.w <= 0.0f || r.h <= 0.0f) return;
        float rad = radius;
        const float lim = std::min(r.w, r.h) * 0.5f;
        if (rad > lim) rad = lim;
        if (rad <= 0.0f) {
            addRect(r);
            return;
        }
        const float k = 0.5522847498f;
        const float x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
        const float kr = rad * k;
        moveTo(x0 + rad, y0);
        lineTo(x1 - rad, y0);
        cubicTo(x1 - rad + kr, y0, x1, y0 + rad - kr, x1, y0 + rad);
        lineTo(x1, y1 - rad);
        cubicTo(x1, y1 - rad + kr, x1 - rad + kr, y1, x1 - rad, y1);
        lineTo(x0 + rad, y1);
        cubicTo(x0 + rad - kr, y1, x0, y1 - rad + kr, x0, y1 - rad);
        lineTo(x0, y0 + rad);
        cubicTo(x0, y0 + rad - kr, x0 + rad - kr, y0, x0 + rad, y0);
        close();
    }

    // 椭圆（四段三次贝塞尔）
    void addEllipse(float cx, float cy, float rx, float ry) {
        if (rx <= 0.0f || ry <= 0.0f) return;
        const float k = 0.5522847498f;
        const float ex = rx * k, ey = ry * k;
        moveTo(cx + rx, cy);
        cubicTo(cx + rx, cy + ey, cx + ex, cy + ry, cx, cy + ry);
        cubicTo(cx - ex, cy + ry, cx - rx, cy + ey, cx - rx, cy);
        cubicTo(cx - rx, cy - ey, cx - ex, cy - ry, cx, cy - ry);
        cubicTo(cx + ex, cy - ry, cx + rx, cy - ey, cx + rx, cy);
        close();
    }
    void addCircle(float cx, float cy, float r) { addEllipse(cx, cy, r, r); }

    // 正多边形 / 星形（演示与图表用；也比"手算一堆 lineTo"更不容易写歪）
    void addRegularPolygon(float cx, float cy, float r, int sides, float startDeg = -90.0f) {
        if (sides < 3 || r <= 0.0f) return;
        constexpr float kPi = 3.14159265358979323846f;
        for (int i = 0; i < sides; ++i) {
            const float a = (startDeg + 360.0f * static_cast<float>(i) /
                             static_cast<float>(sides)) * kPi / 180.0f;
            const Point p{cx + std::cos(a) * r, cy + std::sin(a) * r};
            if (i == 0) moveTo(p); else lineTo(p);
        }
        close();
    }
    // 星形：内外半径交替（star 的经典写法）
    void addStar(float cx, float cy, float rOuter, float rInner, int points,
                 float startDeg = -90.0f) {
        if (points < 3 || rOuter <= 0.0f || rInner <= 0.0f) return;
        constexpr float kPi = 3.14159265358979323846f;
        const int n = points * 2;
        for (int i = 0; i < n; ++i) {
            const float r = (i % 2 == 0) ? rOuter : rInner;
            const float a = (startDeg + 180.0f * static_cast<float>(i) /
                             static_cast<float>(points)) * kPi / 180.0f;
            const Point p{cx + std::cos(a) * r, cy + std::sin(a) * r};
            if (i == 0) moveTo(p); else lineTo(p);
        }
        close();
    }

    // 追加另一条路径（不移动子路径起点）
    void append(const Path& other) {
        cmds_.insert(cmds_.end(), other.cmds_.begin(), other.cmds_.end());
    }

    // ------------------------------------------------------------
    // 变换（逐控制点，曲线类型不变 —— 仿射变换下贝塞尔仍然是同阶贝塞尔）
    // ------------------------------------------------------------
    void transform(const Transform& xf) {
        for (Command& n : cmds_) {
            n.p0 = xf.map(n.p0);
            n.p1 = xf.map(n.p1);
            n.p2 = xf.map(n.p2);
        }
    }
    Path transformed(const Transform& xf) const {
        Path out = *this;
        out.transform(xf);
        return out;
    }

    // ------------------------------------------------------------
    // 精确包围盒
    // ------------------------------------------------------------
    Rect bounds() const {
        if (cmds_.empty()) return Rect();
        float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;
        bool first = true;
        auto admit = [&](float x, float y) {
            if (first) {
                minx = maxx = x;
                miny = maxy = y;
                first = false;
                return;
            }
            minx = std::min(minx, x); maxx = std::max(maxx, x);
            miny = std::min(miny, y); maxy = std::max(maxy, y);
        };

        Point cur{0.0f, 0.0f}, start{0.0f, 0.0f};
        for (const Command& n : cmds_) {
            switch (n.verb) {
            case Verb::Move:
                cur = n.p2;
                start = cur;
                admit(cur.x, cur.y);
                break;
            case Verb::Line:
                admit(n.p2.x, n.p2.y);
                cur = n.p2;
                break;
            case Verb::Quad: {
                admit(n.p1.x, n.p1.y);
                // 导数零点：(1-t)²·P0 + 2t(1-t)·C + t²·P1
                for (int axis = 0; axis < 2; ++axis) {
                    const float p0 = axis ? cur.y : cur.x;
                    const float c = axis ? n.p0.y : n.p0.x;
                    const float p1 = axis ? n.p1.y : n.p1.x;
                    const float den = p0 - 2.0f * c + p1;
                    if (std::fabs(den) < 1e-9f) continue;
                    const float t = (p0 - c) / den;
                    if (t <= 0.0f || t >= 1.0f) continue;
                    const float mt = 1.0f - t;
                    const float v = mt * mt * p0 + 2.0f * mt * t * c + t * t * p1;
                    if (axis == 0) admit(v, miny);   // 沿用当前 y 范围（另一个轴由极值决定，见下）
                    else admit(minx, v);
                }
                cur = n.p1;
                break;
            }
            case Verb::Cubic: {
                admit(n.p2.x, n.p2.y);
                for (int axis = 0; axis < 2; ++axis) {
                    const float p0 = axis ? cur.y : cur.x;
                    const float c1 = axis ? n.p0.y : n.p0.x;
                    const float c2 = axis ? n.p1.y : n.p1.x;
                    const float p1 = axis ? n.p2.y : n.p2.x;
                    // B'(t) = 3[(1-t)²(c1-p0) + 2(1-t)t(c2-c1) + t²(p1-c2)]
                    const float a2 = -p0 + 3.0f * c1 - 3.0f * c2 + p1;
                    const float b2 = 2.0f * (p0 - 2.0f * c1 + c2);
                    const float c3 = c1 - p0;
                    float ts[2];
                    int cnt = 0;
                    if (std::fabs(a2) < 1e-9f) {
                        if (std::fabs(b2) > 1e-9f) { ts[cnt++] = -c3 / b2; }
                    } else {
                        const float disc = b2 * b2 - 4.0f * a2 * c3;
                        if (disc >= 0.0f) {
                            const float sq = std::sqrt(disc);
                            ts[cnt++] = (-b2 + sq) / (2.0f * a2);
                            ts[cnt++] = (-b2 - sq) / (2.0f * a2);
                        }
                    }
                    for (int k = 0; k < cnt; ++k) {
                        const float t = ts[k];
                        if (t <= 0.0f || t >= 1.0f) continue;
                        const float mt = 1.0f - t;
                        const float v = mt * mt * mt * p0 + 3.0f * mt * mt * t * c1 +
                                        3.0f * mt * t * t * c2 + t * t * t * p1;
                        if (axis == 0) admit(v, miny);
                        else admit(minx, v);
                    }
                }
                cur = n.p2;
                break;
            }
            case Verb::Close:
                cur = start;
                break;
            }
        }
        return Rect(minx, miny, maxx - minx, maxy - miny);
    }

    // ------------------------------------------------------------
    // 展平（自适应细分）
    // ------------------------------------------------------------
    // tolerance: 最大弦高误差（用户单位）。<=0 时按 0.25 处理（免得退化成无限递归）。
    // maxPoints: 点数上限（防御性：极端控制点 + 极小容差会把点数炸到几十万）
    std::vector<Polyline> flatten(float tolerance = 0.25f,
                                  size_t maxPoints = 200000) const {
        if (tolerance <= 0.0f) tolerance = 0.25f;
        std::vector<Polyline> out;
        Polyline cur;
        Point p{0.0f, 0.0f};
        Point start{0.0f, 0.0f};
        bool open = false;
        size_t total = 0;

        auto flush = [&]() {
            if (open && cur.pts.size() >= 2) out.push_back(cur);
            cur = Polyline();
            open = false;
        };
        auto addPoint = [&](const Point& q) {
            if (!open) { cur.pts.clear(); cur.pts.push_back(p); open = true; }
            cur.pts.push_back(q);
            ++total;
        };

        for (const Command& n : cmds_) {
            if (total >= maxPoints) break;
            switch (n.verb) {
            case Verb::Move:
                flush();
                p = n.p2;
                start = p;
                cur.pts.push_back(p);   // 单点先占位；真有线段时才会成为子路径
                open = false;
                break;
            case Verb::Line:
                addPoint(n.p2);
                p = n.p2;
                break;
            case Verb::Quad:
                FlattenQuad(p, n.p0, n.p1, tolerance, addPoint, 0);
                p = n.p1;
                break;
            case Verb::Cubic:
                FlattenCubic(p, n.p0, n.p1, n.p2, tolerance, addPoint, 0);
                p = n.p2;
                break;
            case Verb::Close:
                if (open) {
                    cur.closed = true;
                    // 显式补上起点：光栅化时"闭合"是按 true 处理，但描边要有回程边
                    if (cur.pts.size() >= 2) cur.pts.push_back(start);
                    out.push_back(cur);
                    cur = Polyline();
                    open = false;
                }
                p = start;
                break;
            }
        }
        flush();
        return out;
    }

    // ------------------------------------------------------------
    // 统计 / 清理
    // ------------------------------------------------------------
    bool empty() const { return cmds_.empty(); }
    size_t commandCount() const { return cmds_.size(); }
    const std::vector<Command>& commands() const { return cmds_; }
    void clear() { cmds_.clear(); }

    // 点是否在路径内（命中测试；把"设备坐标 → 局部"的逆变换交给调用方）
    bool contains(float x, float y, FillRule rule = FillRule::NonZero,
                  float tolerance = 0.2f) const {
        const std::vector<Polyline> polys = flatten(tolerance);
        int winding = 0;
        for (const Polyline& pl : polys) {
            const size_t n = pl.pts.size();
            if (n < 2) continue;
            const size_t segs = pl.closed ? n : n - 1;
            for (size_t i = 0; i < segs; ++i) {
                const Point& a = pl.pts[i];
                const Point& b = pl.pts[(i + 1) % n];
                if (a.y == b.y) continue;
                if (a.y <= y && b.y > y) {
                    const float t = (y - a.y) / (b.y - a.y);
                    if (a.x + (b.x - a.x) * t > x) ++winding;
                } else if (b.y <= y && a.y > y) {
                    const float t = (y - a.y) / (b.y - a.y);
                    if (a.x + (b.x - a.x) * t > x) --winding;
                }
            }
        }
        return rule == FillRule::NonZero ? winding != 0 : (winding & 1) != 0;
    }

private:
    // 控制点到弦的距离（二次/三次贝塞尔的最大偏离上界）
    static float DistToLine(const Point& p, const Point& a, const Point& b) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-9f) {
            const float ex = p.x - a.x, ey = p.y - a.y;
            return std::sqrt(ex * ex + ey * ey);
        }
        return std::fabs((p.x - a.x) * dy - (p.y - a.y) * dx) / len;
    }

    template <class AddPoint>
    static void FlattenQuad(const Point& p0, const Point& c, const Point& p1,
                            float tol, AddPoint& add, int depth) {
        if (depth >= 18 || DistToLine(c, p0, p1) <= tol) {
            add(p1);
            return;
        }
        const Point c0{(p0.x + c.x) * 0.5f, (p0.y + c.y) * 0.5f};
        const Point c1{(c.x + p1.x) * 0.5f, (c.y + p1.y) * 0.5f};
        const Point m{(c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f};
        FlattenQuad(p0, c0, m, tol, add, depth + 1);
        FlattenQuad(m, c1, p1, tol, add, depth + 1);
    }

    template <class AddPoint>
    static void FlattenCubic(const Point& p0, const Point& c1, const Point& c2,
                             const Point& p1, float tol, AddPoint& add, int depth) {
        const float d = std::max(DistToLine(c1, p0, p1), DistToLine(c2, p0, p1));
        if (depth >= 18 || d <= tol) {
            add(p1);
            return;
        }
        const Point a{(p0.x + c1.x) * 0.5f, (p0.y + c1.y) * 0.5f};
        const Point b{(c1.x + c2.x) * 0.5f, (c1.y + c2.y) * 0.5f};
        const Point c{(c2.x + p1.x) * 0.5f, (c2.y + p1.y) * 0.5f};
        const Point m0{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const Point m1{(b.x + c.x) * 0.5f, (b.y + c.y) * 0.5f};
        const Point mid{(m0.x + m1.x) * 0.5f, (m0.y + m1.y) * 0.5f};
        FlattenCubic(p0, a, m0, mid, tol, add, depth + 1);
        FlattenCubic(mid, m1, c, p1, tol, add, depth + 1);
    }

    std::vector<Command> cmds_;
};

} // namespace bin2d
