#pragma once
// ============================================================
// bin2d/transform.h —— 2×3 仿射矩阵
//
// 约定刻意对齐 **SVG / CSS** 的 matrix(a b c d e f)：
//
//     | a  c  e |        x' = a*x + c*y + e
//     | b  d  f |        y' = b*x + d*y + f
//     | 0  0  1 |
//
// 所以 `matrix(...)`、`transform: rotate()` 这类文本能被逐字喂进来；而
// bingpu.h 的 `xf[6]` 是**列优先的两列 + 平移**，顺序是 {a, c, e, b, d, f}
// —— toBingpu() 负责这个顺序差，别在两处各记一遍。
//
// 为什么是 float 不是 double：光栅化最终落在像素上，float 的 24 位尾数在
// 1e6 像素尺度上误差 < 0.1 像素；double 只会让矩阵乘法慢一倍。
// ============================================================

#include <cmath>

#include "bin2d/core.h"

namespace bin2d {

struct Transform {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

    constexpr Transform() = default;
    constexpr Transform(float ma, float mb, float mc, float md, float me, float mf)
        : a(ma), b(mb), c(mc), d(md), e(me), f(mf) {}

    static constexpr Transform identity() { return Transform(); }
    static constexpr Transform translate(float tx, float ty) { return Transform(1, 0, 0, 1, tx, ty); }
    static constexpr Transform scale(float sx, float sy) { return Transform(sx, 0, 0, sy, 0, 0); }
    static constexpr Transform skew(float kx, float ky) { return Transform(1, ky, kx, 1, 0, 0); }
    static Transform rotate(float radians) {
        const float cs = std::cos(radians), sn = std::sin(radians);
        return Transform(cs, sn, -sn, cs, 0, 0);
    }
    static Transform rotateDeg(float degrees) { return rotate(degrees * 3.14159265358979323846f / 180.0f); }

    // this ∘ rhs：先应用 rhs，再应用 this（与 SVG/CSS 的书写顺序相反，但这是
    // "父 ∘ 子"的自然复合顺序，binUI 的祖先链级联就是这个方向）
    Transform operator*(const Transform& rhs) const {
        return Transform(a * rhs.a + c * rhs.b,
                         b * rhs.a + d * rhs.b,
                         a * rhs.c + c * rhs.d,
                         b * rhs.c + d * rhs.d,
                         a * rhs.e + c * rhs.f + e,
                         b * rhs.e + d * rhs.f + f);
    }

    Point map(float x, float y) const { return Point{a * x + c * y + e, b * x + d * y + f}; }
    Point map(const Point& p) const { return map(p.x, p.y); }

    // 行列式；接近 0 = 退化（整条线被压没），光栅化前要判一下
    float det() const { return a * d - b * c; }
    bool degenerate() const { return std::fabs(det()) < 1e-9f; }

    // 逆矩阵：非退化时返回 true。命中测试 / 反算局部坐标用。
    bool invert(Transform& out) const {
        const float dt = det();
        if (std::fabs(dt) < 1e-9f) return false;
        const float inv = 1.0f / dt;
        out.a = d * inv;
        out.b = -b * inv;
        out.c = -c * inv;
        out.d = a * inv;
        out.e = (c * f - d * e) * inv;
        out.f = (b * e - a * f) * inv;
        return true;
    }

    // 无旋转/斜切时，把包围盒映射成轴对齐矩形；否则取四角包围盒
    Rect mapBounds(const Rect& r) const {
        const Point p0 = map(r.x, r.y);
        const Point p1 = map(r.right(), r.y);
        const Point p2 = map(r.x, r.bottom());
        const Point p3 = map(r.right(), r.bottom());
        const float minx = std::fmin(std::fmin(p0.x, p1.x), std::fmin(p2.x, p3.x));
        const float miny = std::fmin(std::fmin(p0.y, p1.y), std::fmin(p2.y, p3.y));
        const float maxx = std::fmax(std::fmax(p0.x, p1.x), std::fmax(p2.x, p3.x));
        const float maxy = std::fmax(std::fmax(p0.y, p1.y), std::fmax(p2.y, p3.y));
        return Rect(minx, miny, maxx - minx, maxy - miny);
    }

    bool isIdentity() const {
        return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && e == 0.0f && f == 0.0f;
    }
    bool isTranslationOnly() const { return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f; }

    // 交给 bingpu_cmd_set_affine 的顺序：{a, c, e, b, d, f}
    void toBingpu(float out[6]) const {
        out[0] = a; out[1] = c; out[2] = e;
        out[3] = b; out[4] = d; out[5] = f;
    }
    static Transform fromBingpu(const float in[6]) {
        return Transform(in[0], in[3], in[1], in[4], in[2], in[5]);
    }
};

} // namespace bin2d
