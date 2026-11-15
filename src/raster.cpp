// ============================================================
// src/raster.cpp —— 扫描线光栅化 + 覆盖率 AA + 合成
//
// 文件内部分三层，读代码时按这个顺序看：
//   1. Cover（覆盖率缓冲）与它的构造：把一批**设备空间折线**栅成覆盖率；
//   2. 合成：拿覆盖率 + 颜料逐像素混合（唯一的"落像素"出口）；
//   3. 对外 API：填充 / 描边 / 圆角矩形 / 位图 / 九宫格。
//
// 覆盖率缓冲只覆盖"这批几何的包围盒 ∩ 裁剪"，不按整画布分配 —— 小图形
// 不付整屏的 memset；大图形也只付自己那块。
// ============================================================

#include "bin2d/raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bin2d {
namespace {

// ------------------------------------------------------------
// 覆盖率缓冲
// ------------------------------------------------------------
struct Cover {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<float> v;

    void reset(const Rect& bbox, const Rect& clip) {
        const Rect r = Rect::intersect(bbox, clip);
        if (r.empty()) { w = h = 0; v.clear(); return; }
        x0 = static_cast<int>(std::floor(r.x));
        y0 = static_cast<int>(std::floor(r.y));
        const int x1 = static_cast<int>(std::ceil(r.right()));
        const int y1 = static_cast<int>(std::ceil(r.bottom()));
        w = x1 - x0;
        h = y1 - y0;
        if (w <= 0 || h <= 0) { w = h = 0; v.clear(); return; }
        v.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
    }

    float* row(int y) { return v.data() + static_cast<size_t>(y) * static_cast<size_t>(w); }
    const float* row(int y) const {
        return v.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
    }
    float at(int x, int y) const { return row(y)[x - x0]; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// 一批设备空间折线的包围盒（先取所有点，再按需外扩）
Rect PolysBounds(const std::vector<Polyline>& polys) {
    bool first = true;
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    for (const Polyline& pl : polys) {
        for (const Point& p : pl.pts) {
            if (first) { minx = maxx = p.x; miny = maxy = p.y; first = false; continue; }
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
        }
    }
    if (first) return Rect();
    return Rect(minx, miny, maxx - minx, maxy - miny);
}

struct Crossing {
    float x = 0.0f;
    int dir = 0;
};

// 把一批设备空间折线栅进 cover（union=true 时取 max，用于描边的多块并集）
void RasterPolys(const std::vector<Polyline>& polys, FillRule rule, bool unionMode,
                 Cover& cover) {
    if (cover.empty()) return;
    const int h = cover.h;
    const int w = cover.w;
    const float subStep = 1.0f / static_cast<float>(kSubSamples);
    const float subWeight = 1.0f / static_cast<float>(kSubSamples);

    std::vector<Crossing> xs;
    std::vector<float> rowAcc(static_cast<size_t>(w), 0.0f);

    for (int yy = 0; yy < h; ++yy) {
        const int py = cover.y0 + yy;
        float* dstRow = cover.row(yy);
        bool any = false;

        for (int sub = 0; sub < kSubSamples; ++sub) {
            const float sy = static_cast<float>(py) +
                             (static_cast<float>(sub) + 0.5f) * subStep;
            xs.clear();
            for (const Polyline& pl : polys) {
                const size_t n = pl.pts.size();
                if (n < 2) continue;
                const size_t segs = pl.closed ? n : n - 1;
                for (size_t i = 0; i < segs; ++i) {
                    const Point& a = pl.pts[i];
                    const Point& b = pl.pts[(i + 1) % n];
                    if (a.y == b.y) continue;
                    // 半开区间规则：只统计"下闭上开"方向的穿越，顶点处不会重复计数
                    const bool down = (a.y <= sy) && (b.y > sy);
                    const bool up = (b.y <= sy) && (a.y > sy);
                    if (!down && !up) continue;
                    const float t = (sy - a.y) / (b.y - a.y);
                    xs.push_back(Crossing{a.x + (b.x - a.x) * t, down ? 1 : -1});
                }
            }
            if (xs.empty()) continue;
            std::sort(xs.begin(), xs.end(), [](const Crossing& a, const Crossing& b) {
                if (a.x != b.x) return a.x < b.x;
                return a.dir < b.dir;
            });

            // 把 [xa,xb] 区间按像素小数部分累进本行覆盖率
            auto addSpan = [&](float xa, float xb) {
                if (xb <= xa) return;
                float lo = xa, hi = xb;
                if (lo < static_cast<float>(cover.x0)) lo = static_cast<float>(cover.x0);
                const float maxX = static_cast<float>(cover.x0 + w);
                if (hi > maxX) hi = maxX;
                if (hi <= lo) return;
                int ix = static_cast<int>(std::floor(lo));
                while (static_cast<float>(ix) < hi) {
                    const float l = std::max(lo, static_cast<float>(ix));
                    const float r = std::min(hi, static_cast<float>(ix + 1));
                    if (r > l) {
                        const int xi = ix - cover.x0;
                        if (xi >= 0 && xi < w) {
                            rowAcc[static_cast<size_t>(xi)] += (r - l) * subWeight;
                            any = true;
                        }
                    }
                    ++ix;
                }
            };

            if (rule == FillRule::EvenOdd) {
                for (size_t i = 0; i + 1 < xs.size(); i += 2) addSpan(xs[i].x, xs[i + 1].x);
            } else {
                int winding = 0;
                for (size_t i = 0; i + 1 < xs.size(); ++i) {
                    winding += xs[i].dir;
                    if (winding != 0) addSpan(xs[i].x, xs[i + 1].x);
                }
            }
        }

        if (!any) continue;
        for (int xi = 0; xi < w; ++xi) {
            float c = rowAcc[static_cast<size_t>(xi)];
            rowAcc[static_cast<size_t>(xi)] = 0.0f;
            if (c <= 0.0f) continue;
            if (c > 1.0f) c = 1.0f;
            if (unionMode) dstRow[xi] = std::max(dstRow[xi], c);
            else dstRow[xi] += c;   // 单次栅格化理论上不会 >1；相加只是防御
        }
        if (!unionMode) {
            for (int xi = 0; xi < w; ++xi)
                if (dstRow[xi] > 1.0f) dstRow[xi] = 1.0f;
        }
    }
}

// 局部 → 设备；同时给出展平容差（按变换的平均缩放折成用户单位）
std::vector<Polyline> FlattenToDevice(const Path& path, const Transform& xf,
                                      float tolerancePx) {
    const float scale = std::sqrt(std::fabs(xf.det()));
    const float tol = (scale > 1e-6f) ? tolerancePx / scale : tolerancePx;
    std::vector<Polyline> polys = path.flatten(tol);
    for (Polyline& pl : polys)
        for (Point& p : pl.pts) p = xf.map(p);
    return polys;
}

// 颜料在**局部坐标**某点的颜色
Color SamplePaint(const Paint& paint, float lx, float ly) {
    switch (paint.kind) {
    case PaintKind::Solid:
        return paint.color;
    case PaintKind::Linear:
        return paint.sampleLinear(lx, ly);
    case PaintKind::Image: {
        if (!paint.image || paint.image->empty()) return Color::trans();
        const Rect& d = paint.imageDst;
        if (d.w <= 1e-6f || d.h <= 1e-6f) return Color::trans();
        const float u = paint.srcUv.x + ((lx - d.x) / d.w) * paint.srcUv.w;
        const float v = paint.srcUv.y + ((ly - d.y) / d.h) * paint.srcUv.h;
        const ImageFilter f = paint.nearest ? ImageFilter::Nearest : ImageFilter::Linear;
        Color c = paint.image->sample(u, v, f);
        // tint 是预乘色，逐通道相乘
        auto mul = [](uint8_t a, uint8_t b) {
            return static_cast<uint8_t>((static_cast<uint32_t>(a) * b + 127u) / 255u);
        };
        c = Color(mul(c.r, paint.tint.r), mul(c.g, paint.tint.g),
                  mul(c.b, paint.tint.b), mul(c.a, paint.tint.a));
        return c;
    }
    }
    return Color::trans();
}

// 覆盖率 + 颜料 → 目标像素（唯一的落像素出口）
void Composite(const RasterTarget& t, const Cover& cover, const Paint& paint,
               const Transform& invXf, float alphaScale) {
    if (cover.empty() || !t.valid()) return;
    Surface& dst = *t.surface;
    const int dw = dst.width(), dh = dst.height();
    for (int yy = 0; yy < cover.h; ++yy) {
        const int py = cover.y0 + yy;
        if (py < 0 || py >= dh) continue;
        const float* row = cover.row(yy);
        for (int xi = 0; xi < cover.w; ++xi) {
            float c = row[xi];
            if (c <= 0.0f) continue;
            const int px = cover.x0 + xi;
            if (px < 0 || px >= dw) continue;
            if (t.mask) c *= t.mask[static_cast<size_t>(py) * static_cast<size_t>(dw) +
                                     static_cast<size_t>(px)];
            if (c <= 0.0f) continue;
            if (alphaScale != 1.0f) c *= alphaScale;
            if (c <= 0.0f) continue;
            const Point lp = invXf.map(static_cast<float>(px) + 0.5f,
                                       static_cast<float>(py) + 0.5f);
            Color src = SamplePaint(paint, lp.x, lp.y);
            if (src.a == 0) continue;
            src = src * (c > 1.0f ? 1.0f : c);
            const Color out = BlendPixel(dst.get(px, py), src, t.blend);
            dst.set(px, py, out);
        }
    }
}

// 直接在 Surface 上按 UV 采样（Image 版本走同一个实现，避免两套插值）
Color SampleSurface(const Surface& s, float u, float v, bool nearest) {
    if (s.empty()) return Color::trans();
    const float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    const float cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    const float fx = cu * static_cast<float>(s.width() - 1);
    const float fy = cv * static_cast<float>(s.height() - 1);
    auto at = [&s](int x, int y) {
        const int cx = x < 0 ? 0 : (x >= s.width() ? s.width() - 1 : x);
        const int cy = y < 0 ? 0 : (y >= s.height() ? s.height() - 1 : y);
        return s.get(cx, cy);
    };
    if (nearest) return at(static_cast<int>(fx + 0.5f), static_cast<int>(fy + 0.5f));
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    return lerp(lerp(at(x0, y0), at(x0 + 1, y0), tx),
                lerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), tx), ty);
}

// 像素 (px,py) 与轴对齐矩形 r 的重叠面积（0..1）—— 轴对齐图形的解析覆盖率
float RectCoverage(const Rect& r, int px, int py) {
    const float l = std::max(r.x, static_cast<float>(px));
    const float t = std::max(r.y, static_cast<float>(py));
    const float rr = std::min(r.right(), static_cast<float>(px + 1));
    const float bb = std::min(r.bottom(), static_cast<float>(py + 1));
    if (rr <= l || bb <= t) return 0.0f;
    return (rr - l) * (bb - t);
}

// ------------------------------------------------------------
// 描边几何：四边形 + 连接 + 端帽（全部在**局部空间**构造）
// ------------------------------------------------------------
void PushQuad(std::vector<std::vector<Point>>& out, const Point& a, const Point& b,
              const Point& c, const Point& d) {
    out.push_back({a, b, c, d});
}
void PushTri(std::vector<std::vector<Point>>& out, const Point& a, const Point& b,
             const Point& c) {
    out.push_back({a, b, c});
}
// 半径为 r 的圆多边形（连接/端帽用）；segments 越大越圆
void PushCircle(std::vector<std::vector<Point>>& out, const Point& c, float r, int segments) {
    std::vector<Point> ring;
    ring.reserve(static_cast<size_t>(segments));
    for (int i = 0; i < segments; ++i) {
        const float a = 6.28318530717958647692f * static_cast<float>(i) /
                        static_cast<float>(segments);
        ring.push_back(Point{c.x + std::cos(a) * r, c.y + std::sin(a) * r});
    }
    out.push_back(std::move(ring));
}

struct Dir {
    float x = 1.0f, y = 0.0f;
};

Dir Unit(const Point& a, const Point& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-9f) return Dir{1.0f, 0.0f};
    return Dir{dx / len, dy / len};
}

// 一条子路径的描边块：每段一个四边形，每个拐点一个连接块，开放两端加端帽。
// 全部叠进同一张覆盖率（union）后再混合一次，所以接缝不会重复混。
void BuildStrokePolys(const std::vector<Polyline>& polys, const StrokeStyle& st,
                      std::vector<std::vector<Point>>& out) {
    const float half = std::max(0.05f, st.width) * 0.5f;
    const int circleSegs = 24;

    for (const Polyline& pl : polys) {
        std::vector<Point> pts = pl.pts;
        // 展平器给闭合子路径补了回到起点的重复点；这里去掉，闭合单独标记
        if (pl.closed && pts.size() >= 2) {
            const Point& f = pts.front();
            const Point& b = pts.back();
            if (std::fabs(f.x - b.x) < 1e-6f && std::fabs(f.y - b.y) < 1e-6f)
                pts.pop_back();
        }
        if (pts.size() < 2) continue;
        const size_t n = pts.size();
        const size_t segs = pl.closed ? n : n - 1;

        // 每段：法向偏移出四边形
        for (size_t i = 0; i < segs; ++i) {
            const Point& a = pts[i];
            const Point& b = pts[(i + 1) % n];
            const Dir d = Unit(a, b);
            const Point nrm{-d.y * half, d.x * half};
            Point a0{a.x + nrm.x, a.y + nrm.y};
            Point b0{b.x + nrm.x, b.y + nrm.y};
            Point a1{a.x - nrm.x, a.y - nrm.y};
            Point b1{b.x - nrm.x, b.y - nrm.y};
            // 方端帽：把开放子路径的首/末段沿切线外延半个线宽
            if (!pl.closed && st.squareCap) {
                if (i == 0) {
                    a0 = Point{a0.x - d.x * half, a0.y - d.y * half};
                    a1 = Point{a1.x - d.x * half, a1.y - d.y * half};
                }
                if (i + 1 == segs) {
                    b0 = Point{b0.x + d.x * half, b0.y + d.y * half};
                    b1 = Point{b1.x + d.x * half, b1.y + d.y * half};
                }
            }
            PushQuad(out, a0, b0, b1, a1);
        }

        // 拐点连接
        const size_t jointCount = pl.closed ? n : n - 2;
        for (size_t i = 0; i < jointCount; ++i) {
            const size_t vi = (i + 1) % n;
            const Point& prev = pts[i];
            const Point& v = pts[vi];
            const Point& next = pts[(vi + 1) % n];
            const Dir d0 = Unit(prev, v);
            const Dir d1 = Unit(v, next);
            const float cross = d0.x * d1.y - d0.y * d1.x;
            const float dot = d0.x * d1.x + d0.y * d1.y;
            if (std::fabs(cross) < 1e-5f && dot > 0.0f) continue;   // 共线直行
            if (st.roundJoin) {
                PushCircle(out, v, half, circleSegs);
            } else {
                // 外侧法向：拐向的对面
                const float sgn = (cross > 0.0f) ? -1.0f : 1.0f;
                const Point n0{-d0.y * half * sgn, d0.x * half * sgn};
                const Point n1{-d1.y * half * sgn, d1.x * half * sgn};
                const Point outerA{v.x + n0.x, v.y + n0.y};
                const Point outerB{v.x + n1.x, v.y + n1.y};
                // 内外两个三角把拐点两侧都填满（否则内侧会缺一个楔形缝）
                PushTri(out, v, outerA, outerB);
                const Point innerA{v.x - n0.x, v.y - n0.y};
                const Point innerB{v.x - n1.x, v.y - n1.y};
                PushTri(out, v, innerB, innerA);
                // miter：沿角平分线伸出，长度受 miterLimit 约束
                const float cosHalf = std::sqrt(std::max(0.0f, (1.0f + dot) * 0.5f));
                if (cosHalf > 1e-4f) {
                    const float ratio = 1.0f / cosHalf;
                    if (ratio <= std::max(1.0f, st.miterLimit)) {
                        const float bisX = (n0.x + n1.x), bisY = (n0.y + n1.y);
                        const float bl = std::sqrt(bisX * bisX + bisY * bisY);
                        if (bl > 1e-6f) {
                            const float scale = half * ratio / (bl * 0.5f) * 0.5f;
                            const Point tip{v.x + bisX * scale, v.y + bisY * scale};
                            PushQuad(out, outerA, tip, tip, outerB);
                        }
                    }
                }
            }
        }

        // 开放子路径的端帽
        if (!pl.closed) {
            if (st.roundCap && !st.squareCap) {
                PushCircle(out, pts.front(), half, circleSegs);
                PushCircle(out, pts.back(), half, circleSegs);
            }
        }
    }
}

} // namespace

// ============================================================
// 对外 API
// ============================================================
void FillPath(const RasterTarget& t, const Path& path, FillRule rule,
              const Paint& paint, const Transform& xf, float tolerancePx) {
    if (!t.valid() || path.empty()) return;
    Transform inv;
    if (xf.degenerate() || !xf.invert(inv)) return;
    const std::vector<Polyline> polys = FlattenToDevice(path, xf, tolerancePx);
    if (polys.empty()) return;
    const Rect bbox = PolysBounds(polys);
    if (bbox.empty()) return;
    Cover cover;
    cover.reset(bbox, Rect::intersect(t.clip, t.surface->bounds()));
    if (cover.empty()) return;
    RasterPolys(polys, rule, false, cover);
    Composite(t, cover, paint, inv, 1.0f);
}

void StrokePath(const RasterTarget& t, const Path& path, const Paint& paint,
                const StrokeStyle& style, const Transform& xf, float tolerancePx) {
    if (!t.valid() || path.empty() || style.width <= 0.0f) return;
    Transform inv;
    if (xf.degenerate() || !xf.invert(inv)) return;
    const std::vector<Polyline> polys = path.flatten(
        tolerancePx / std::sqrt(std::max(1e-6f, std::fabs(xf.det()))));
    if (polys.empty()) return;

    std::vector<std::vector<Point>> pieces;
    BuildStrokePolys(polys, style, pieces);
    if (pieces.empty()) return;

    // 设备空间包围盒（外扩半个线宽，圆角连接可能探出去）
    std::vector<Polyline> devPieces;
    devPieces.reserve(pieces.size());
    const float pad = style.width * std::sqrt(std::fabs(xf.det())) * 0.5f + 1.0f;
    Rect bbox;
    bool first = true;
    for (const std::vector<Point>& piece : pieces) {
        Polyline pl;
        pl.closed = true;
        pl.pts.reserve(piece.size());
        for (const Point& p : piece) pl.pts.push_back(xf.map(p));
        const Rect pb = PolysBounds({pl});
        if (first) { bbox = pb; first = false; }
        else {
            const float minx = std::min(bbox.x, pb.x), miny = std::min(bbox.y, pb.y);
            const float maxx = std::max(bbox.right(), pb.right());
            const float maxy = std::max(bbox.bottom(), pb.bottom());
            bbox = Rect(minx, miny, maxx - minx, maxy - miny);
        }
        devPieces.push_back(std::move(pl));
    }
    bbox = Rect(bbox.x - pad, bbox.y - pad, bbox.w + pad * 2.0f, bbox.h + pad * 2.0f);

    Cover cover;
    cover.reset(bbox, Rect::intersect(t.clip, t.surface->bounds()));
    if (cover.empty()) return;
    // union：多块叠在同一像素取 max（不是相加），接缝不会变深
    for (const Polyline& piece : devPieces) RasterPolys({piece}, FillRule::NonZero, true, cover);
    Composite(t, cover, paint, inv, 1.0f);
}

void FillRoundedRect(const RasterTarget& t, const Rect& r, float radius,
                     const Paint& paint, const Transform& xf) {
    if (r.empty()) return;
    Path p;
    p.addRoundedRect(r, radius);
    FillPath(t, p, FillRule::NonZero, paint, xf);
}

void FillRect(const RasterTarget& t, const Rect& r, const Paint& paint,
              const Transform& xf) {
    if (r.empty()) return;
    if (paint.kind == PaintKind::Solid && xf.isTranslationOnly()) {
        // 轴对齐纯色：解析覆盖率，整像素处精确为 1（不经过扫描线，速度与精度双赢）
        Transform inv;
        if (!xf.invert(inv)) return;
        const Rect dev = Rect(r.x + xf.e, r.y + xf.f, r.w, r.h);
        const Rect area = Rect::intersect(Rect::intersect(dev, t.clip),
                                          t.surface->bounds());
        if (area.empty()) return;
        Surface& dst = *t.surface;
        const int x0 = static_cast<int>(std::floor(area.x));
        const int y0 = static_cast<int>(std::floor(area.y));
        const int x1 = static_cast<int>(std::ceil(area.right()));
        const int y1 = static_cast<int>(std::ceil(area.bottom()));
        const int dw = dst.width();
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                float c = RectCoverage(dev, px, py);
                if (c <= 0.0f) continue;
                if (t.mask) c *= t.mask[static_cast<size_t>(py) * static_cast<size_t>(dw) +
                                         static_cast<size_t>(px)];
                if (c <= 0.0f) continue;
                Color src = paint.color * (c > 1.0f ? 1.0f : c);
                dst.set(px, py, BlendPixel(dst.get(px, py), src, t.blend));
            }
        }
        return;
    }
    Path p;
    p.addRect(r);
    FillPath(t, p, FillRule::NonZero, paint, xf);
}

void FillLinearGradient(const RasterTarget& t, const Rect& r, const Paint& gradient,
                        const Transform& xf) {
    FillRect(t, r, gradient, xf);
}

void DrawSurface(const RasterTarget& t, const Surface& image, const Rect& dstLocal,
                 const Rect& srcUv, const Color& tint, bool nearest,
                 const Transform& xf) {
    if (!t.valid() || image.empty() || dstLocal.empty()) return;
    Transform inv;
    if (xf.degenerate() || !xf.invert(inv)) return;
    Surface& dst = *t.surface;

    const bool axisAligned = std::fabs(xf.b) < 1e-6f && std::fabs(xf.c) < 1e-6f;
    const Rect devRect = axisAligned
        ? Rect(dstLocal.x * xf.a + xf.e, dstLocal.y * xf.d + xf.f,
               dstLocal.w * std::fabs(xf.a), dstLocal.h * std::fabs(xf.d))
        : xf.mapBounds(dstLocal);
    if (devRect.empty()) return;
    const Rect area = Rect::intersect(Rect::intersect(devRect, t.clip), dst.bounds());
    if (area.empty()) return;

    const int dw = dst.width();
    const int x0 = static_cast<int>(std::floor(area.x));
    const int y0 = static_cast<int>(std::floor(area.y));
    const int x1 = static_cast<int>(std::ceil(area.right()));
    const int y1 = static_cast<int>(std::ceil(area.bottom()));
    auto mul8 = [](uint8_t a, uint8_t b) {
        return static_cast<uint8_t>((static_cast<uint32_t>(a) * b + 127u) / 255u);
    };

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            float cov;
            float u = 0.0f, v = 0.0f;
            if (axisAligned) {
                cov = RectCoverage(devRect, px, py);
                if (cov <= 0.0f) continue;
                // 反算局部坐标（轴对齐时直接除缩放）
                const float lx = (static_cast<float>(px) + 0.5f - xf.e) / xf.a;
                const float ly = (static_cast<float>(py) + 0.5f - xf.f) / xf.d;
                u = srcUv.x + ((lx - dstLocal.x) / dstLocal.w) * srcUv.w;
                v = srcUv.y + ((ly - dstLocal.y) / dstLocal.h) * srcUv.h;
            } else {
                // 旋转/斜切：4×4 子采样判内（覆盖率），UV 取中心点
                int hits = 0;
                float cu = 0.0f, cv = 0.0f;
                for (int sy = 0; sy < 4; ++sy) {
                    for (int sx = 0; sx < 4; ++sx) {
                        const float dx = static_cast<float>(px) +
                                         (static_cast<float>(sx) + 0.5f) * 0.25f;
                        const float dy = static_cast<float>(py) +
                                         (static_cast<float>(sy) + 0.5f) * 0.25f;
                        const Point lp = inv.map(dx, dy);
                        if (lp.x < dstLocal.x || lp.x >= dstLocal.right() ||
                            lp.y < dstLocal.y || lp.y >= dstLocal.bottom()) continue;
                        ++hits;
                        cu += srcUv.x + ((lp.x - dstLocal.x) / dstLocal.w) * srcUv.w;
                        cv += srcUv.y + ((lp.y - dstLocal.y) / dstLocal.h) * srcUv.h;
                    }
                }
                if (hits == 0) continue;
                cov = static_cast<float>(hits) / 16.0f;
                u = cu / static_cast<float>(hits);
                v = cv / static_cast<float>(hits);
            }
            if (t.mask) cov *= t.mask[static_cast<size_t>(py) * static_cast<size_t>(dw) +
                                      static_cast<size_t>(px)];
            if (cov <= 0.0f) continue;

            Color c = SampleSurface(image, u, v, nearest);
            c = Color(mul8(c.r, tint.r), mul8(c.g, tint.g), mul8(c.b, tint.b),
                      mul8(c.a, tint.a));
            if (c.a == 0) continue;
            c = c * (cov > 1.0f ? 1.0f : cov);
            dst.set(px, py, BlendPixel(dst.get(px, py), c, t.blend));
        }
    }
}

void DrawNineSlice(const RasterTarget& t, const Image& image, const Rect& dstLocal,
                   float insetL, float insetT, float insetR, float insetB,
                   const Color& tint, bool nearest, const Transform& xf) {
    if (image.empty() || dstLocal.empty()) return;
    const float iw = static_cast<float>(image.width());
    const float ih = static_cast<float>(image.height());
    const float l = std::max(0.0f, std::min(insetL, iw));
    const float r = std::max(0.0f, std::min(insetR, iw - l));
    const float tp = std::max(0.0f, std::min(insetT, ih));
    const float bt = std::max(0.0f, std::min(insetB, ih - tp));

    // 目标里的边界（夹到 dst 尺寸，避免负宽/负高）
    const float dl = std::min(dstLocal.w, l);
    const float dr = std::min(dstLocal.w - dl, r);
    const float dt = std::min(dstLocal.h, tp);
    const float db = std::min(dstLocal.h - dt, bt);
    const float dcx = dstLocal.w - dl - dr;
    const float dcy = dstLocal.h - dt - db;
    const float scx = iw - l - r;
    const float scy = ih - tp - bt;

    // 3×3 的源/目标切分；零宽块跳过（图小或目标小都会出现）
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float sx = (col == 0) ? 0.0f : (col == 1 ? l : l + scx);
            const float sw = (col == 0) ? l : (col == 1 ? scx : r);
            const float sy = (row == 0) ? 0.0f : (row == 1 ? tp : tp + scy);
            const float sh = (row == 0) ? tp : (row == 1 ? scy : bt);
            const float dx = (col == 0) ? 0.0f : (col == 1 ? dl : dl + dcx);
            const float dwq = (col == 0) ? dl : (col == 1 ? dcx : dr);
            const float dy = (row == 0) ? 0.0f : (row == 1 ? dt : dt + dcy);
            const float dhq = (row == 0) ? dt : (row == 1 ? dcy : db);
            if (sw <= 0.0f || sh <= 0.0f || dwq <= 0.0f || dhq <= 0.0f) continue;
            const Rect srcUv(sx / iw, sy / ih, sw / iw, sh / ih);
            const Rect dstSub(dstLocal.x + dx, dstLocal.y + dy, dwq, dhq);
            DrawImage(t, image, dstSub, srcUv, tint, nearest, xf);
        }
    }
}

void DrawImage(const RasterTarget& t, const Image& image, const Rect& dstLocal,
               const Rect& srcUv, const Color& tint, bool nearest,
               const Transform& xf) {
    // Image 只是"素材"包装：采样直接落到它的 Surface 上，两处不会各写一套插值
    DrawSurface(t, image.surface(), dstLocal, srcUv, tint, nearest, xf);
}

// ------------------------------------------------------------
// 模糊：两次可分离的盒式模糊（横一遍、竖一遍）
//
// 为什么用盒式而不是高斯：一次窗口长度 w 的盒式 = 方差 w²/12 的近似高斯，
// 两次串起来（横+竖）已经看不出方向性块感；而真正的高斯核要 O(w) 乘加/像素，
// 盒式用滑动窗 O(1)/像素。投影不需要"完美高斯"，只需要"别是硬边"。
// 预乘空间里直接模糊是正确的：透明像素的 RGB 是 0，不会把黑色渗进阴影边缘。
// ------------------------------------------------------------
void Blur(Surface& s, float radius) {
    if (s.empty() || radius <= 0.0f) return;
    const int rad = static_cast<int>(std::lround(radius));
    if (rad < 1) return;
    const int w = s.width(), h = s.height();
    const int window = rad * 2 + 1;
    std::vector<uint8_t> line(static_cast<size_t>(w) * 4);

    // 横向
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = s.row(y);
        std::memcpy(line.data(), src, line.size());
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = -rad; k <= rad; ++k) {
                int xx = x + k;
                xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
                const uint8_t* p = line.data() + static_cast<size_t>(xx) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            uint8_t* dst = s.row(y) + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<uint8_t>((sum[c] + window / 2) / window);
        }
    }

    // 纵向（用临时列缓冲）
    std::vector<uint8_t> col(static_cast<size_t>(h) * 4);
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            const uint8_t* p = s.row(y) + static_cast<size_t>(x) * 4;
            std::memcpy(col.data() + static_cast<size_t>(y) * 4, p, 4);
        }
        for (int y = 0; y < h; ++y) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = -rad; k <= rad; ++k) {
                int yy = y + k;
                yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
                const uint8_t* p = col.data() + static_cast<size_t>(yy) * 4;
                for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            uint8_t* dst = s.row(y) + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<uint8_t>((sum[c] + window / 2) / window);
        }
    }
}

} // namespace bin2d