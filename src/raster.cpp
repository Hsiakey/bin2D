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
#include <type_traits>
#include <vector>

namespace bin2d {
namespace {

// 与 (x+127)/255 在 x∈[0,65025] 上**逐值相等**（已用穷举验证）：
//   (x + 128 + ((x+128) >> 8)) >> 8
// 换来的是 3 条整数指令而不是一条除法 —— 合成循环里每像素要算 4 次。
inline uint8_t Div255(uint32_t x) {
    const uint32_t t = x + 128u;
    return static_cast<uint8_t>((t + (t >> 8)) >> 8);
}

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

// 预处理的边：x(y) = x0 + (y - y0) * dyInv（直线，所以一次除法
// 就能替代"每条子扫描线各算一次 t"的 8 次除法/乘法）。
//
// 0.1.1 的三处改动（都在同一条语义里，像素结果的差别只在 1e-7 的覆盖率精度）：
//   1. 边只扫一遍建成数组，行内不再遍历全部折线点；
//   2. **活动边表**：按 ymin 排序 + 每行增删，行内只碰与这一行相交的边；
//   3. 跨度的"整像素段"用一次加法循环写完，不再每像素做两次 min/max。
struct Edge {
    float x0 = 0.0f, y0 = 0.0f;   // 低端端点（x0 是该点的 x）
    float dyInv = 0.0f;           // dx/dy
    float ymin = 0.0f, ymax = 0.0f;
    int dir = 0;                  // +1 向下、-1 向上（填充规则的环绕数）
};

// 把一批设备空间折线栅进 cover（union=true 时取 max，用于描边的多块并集）
void RasterPolys(const std::vector<Polyline>& polys, FillRule rule, bool unionMode,
                 Cover& cover) {
    if (cover.empty()) return;
    const int h = cover.h;
    const int w = cover.w;
    const int baseX = cover.x0;
    const float subStep = 1.0f / static_cast<float>(kSubSamples);
    const float subWeight = 1.0f / static_cast<float>(kSubSamples);

    // ---- 建边表 ----
    size_t edgeCount = 0;
    for (const Polyline& pl : polys) {
        const size_t n = pl.pts.size();
        if (n >= 2) edgeCount += pl.closed ? n : n - 1;
    }
    std::vector<Edge> edges;
    edges.reserve(edgeCount);
    for (const Polyline& pl : polys) {
        const size_t n = pl.pts.size();
        if (n < 2) continue;
        const size_t segs = pl.closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            const Point& a = pl.pts[i];
            const Point& b = pl.pts[(i + 1) % n];
            if (a.y == b.y) continue;
            Edge e;
            const float dy = b.y - a.y;
            e.dyInv = (b.x - a.x) / dy;
            if (a.y < b.y) {
                e.x0 = a.x; e.y0 = a.y; e.dir = 1;
                e.ymin = a.y; e.ymax = b.y;
            } else {
                e.x0 = b.x; e.y0 = b.y; e.dir = -1;
                e.ymin = b.y; e.ymax = a.y;
            }
            edges.push_back(e);
        }
    }
    if (edges.empty()) return;
    std::sort(edges.begin(), edges.end(),
              [](const Edge& a, const Edge& b) { return a.ymin < b.ymin; });

    std::vector<Crossing> xs;
    xs.reserve(edges.size());
    std::vector<int> active;
    active.reserve(edges.size());
    std::vector<float> rowAcc(static_cast<size_t>(w), 0.0f);
    size_t nextEdge = 0;

    for (int yy = 0; yy < h; ++yy) {
        const int py = cover.y0 + yy;
        const float rowTop = static_cast<float>(py);
        const float rowBot = rowTop + 1.0f;

        // 活动边表：进这一行的边 → 加；已经走出这一行的边 → 删
        while (nextEdge < edges.size() && edges[nextEdge].ymin < rowBot)
            active.push_back(static_cast<int>(nextEdge++));
        if (!active.empty()) {
            size_t keep = 0;
            for (size_t k = 0; k < active.size(); ++k)
                if (edges[static_cast<size_t>(active[k])].ymax > rowTop)
                    active[keep++] = active[k];
            active.resize(keep);
        }
        if (active.empty()) continue;

        float* dstRow = cover.row(yy);
        bool any = false;

        for (int sub = 0; sub < kSubSamples; ++sub) {
            const float sy = rowTop + (static_cast<float>(sub) + 0.5f) * subStep;
            xs.clear();
            for (size_t k = 0; k < active.size(); ++k) {
                const Edge& e = edges[static_cast<size_t>(active[k])];
                if (sy < e.ymin || sy >= e.ymax) continue;
                xs.push_back(Crossing{e.x0 + (sy - e.y0) * e.dyInv, e.dir});
            }
            if (xs.empty()) continue;

            // 交叉点通常只有 2~8 个：插入排序比 std::sort 的通用派发快一个数量级
            for (size_t i = 1; i < xs.size(); ++i) {
                const Crossing key = xs[i];
                size_t j = i;
                while (j > 0 && (xs[j - 1].x > key.x ||
                                 (xs[j - 1].x == key.x && xs[j - 1].dir > key.dir))) {
                    xs[j] = xs[j - 1];
                    --j;
                }
                xs[j] = key;
            }

            // 把 [xa,xb] 区间按像素小数部分累进本行覆盖率
            auto addSpan = [&](float xa, float xb) {
                if (xb <= xa) return;
                const float minX = static_cast<float>(baseX);
                const float maxX = static_cast<float>(baseX + w);
                const float lo = xa < minX ? minX : xa;
                const float hi = xb > maxX ? maxX : xb;
                if (hi <= lo) return;
                const int ia = static_cast<int>(std::floor(lo));
                const int ib = static_cast<int>(std::floor(hi));
                if (ia == ib) {
                    const int xi = ia - baseX;
                    if (xi >= 0 && xi < w) rowAcc[static_cast<size_t>(xi)] += (hi - lo) * subWeight;
                    any = true;
                    return;
                }
                // 左端的不完整像素
                {
                    const int xi = ia - baseX;
                    if (xi >= 0 && xi < w)
                        rowAcc[static_cast<size_t>(xi)] +=
                            (static_cast<float>(ia + 1) - lo) * subWeight;
                }
                // 中间整像素段：一次加法（循环能被向量化成 4~8 像素一批）
                const int firstFull = ia + 1;
                const int lastFull = ib - 1;
                if (lastFull >= firstFull) {
                    float* p = rowAcc.data() + static_cast<size_t>(firstFull - baseX);
                    const int n = lastFull - firstFull + 1;
                    for (int k = 0; k < n; ++k) p[k] += subWeight;
                }
                // 右端的不完整像素
                const float rightFrac = hi - static_cast<float>(ib);
                if (rightFrac > 0.0f) {
                    const int xi = ib - baseX;
                    if (xi >= 0 && xi < w) rowAcc[static_cast<size_t>(xi)] += rightFrac * subWeight;
                }
                any = true;
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
            const float c = rowAcc[static_cast<size_t>(xi)];
            rowAcc[static_cast<size_t>(xi)] = 0.0f;
            if (c <= 0.0f) continue;
            const float cl = c > 1.0f ? 1.0f : c;
            if (unionMode) dstRow[xi] = std::max(dstRow[xi], cl);
            else dstRow[xi] += cl;
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
//
// 性能重构（0.1.1）：原来每像素都做"颜料种类 switch + 逆矩阵映射 + 通用混合"，
// 对纯色来说前两项完全是多余的（整次绘制颜色是常量）。现在按颜料分派一次、
// 按混合模式分派一次，行内只跑专用循环：
//   · 纯色：连逆映射都不算，只做 覆盖率 × 颜色 + 混合（最热的一条）；
//   · 线性渐变：t 的维度降到"每像素 1 次乘 + 1 次点积"，并且倒数只算一次；
//   · 位图：逐像素反映射 + 采样（采样已经是查表式双线性）。
// 混合从"每像素 4 次整数除法"换成 Div255（与 /255 逐值相等），像素结果逐位不变。

// 逐通道混合。Div255 与 (x+127)/255 在 [0,65025] 上逐值相等（穷举验证过）。
template <BlendMode M>
inline void BlendOne(uint8_t* d, const Color& src) {
    if (src.a == 0) return;
    switch (M) {
    case BlendMode::SrcOver: {
        if (src.a >= 255) { d[0] = src.r; d[1] = src.g; d[2] = src.b; d[3] = 255; return; }
        const uint32_t inv = 255u - src.a;
        d[0] = static_cast<uint8_t>(src.r + Div255(static_cast<uint32_t>(d[0]) * inv));
        d[1] = static_cast<uint8_t>(src.g + Div255(static_cast<uint32_t>(d[1]) * inv));
        d[2] = static_cast<uint8_t>(src.b + Div255(static_cast<uint32_t>(d[2]) * inv));
        d[3] = static_cast<uint8_t>(src.a + Div255(static_cast<uint32_t>(d[3]) * inv));
        return;
    }
    case BlendMode::Replace:
        d[0] = src.r; d[1] = src.g; d[2] = src.b; d[3] = src.a;
        return;
    case BlendMode::Additive: {
        auto add = [](uint8_t a, uint8_t b) {
            const unsigned v = static_cast<unsigned>(a) + static_cast<unsigned>(b);
            return static_cast<uint8_t>(v > 255u ? 255u : v);
        };
        d[0] = add(d[0], src.r); d[1] = add(d[1], src.g);
        d[2] = add(d[2], src.b); d[3] = add(d[3], src.a);
        return;
    }
    case BlendMode::Erase: {
        const uint32_t inv = 255u - src.a;
        d[0] = Div255(static_cast<uint32_t>(d[0]) * inv);
        d[1] = Div255(static_cast<uint32_t>(d[1]) * inv);
        d[2] = Div255(static_cast<uint32_t>(d[2]) * inv);
        d[3] = Div255(static_cast<uint32_t>(d[3]) * inv);
        return;
    }
    case BlendMode::Keep: {
        const uint32_t a = src.a;
        d[0] = Div255(static_cast<uint32_t>(d[0]) * a);
        d[1] = Div255(static_cast<uint32_t>(d[1]) * a);
        d[2] = Div255(static_cast<uint32_t>(d[2]) * a);
        d[3] = Div255(static_cast<uint32_t>(d[3]) * a);
        return;
    }
    }
}

// ---- 一行的三种专用实现 ----
template <BlendMode M>
inline void RowSolid(uint8_t* dstRow, const float* cov, const float* mask, int x0, int w,
                     float alphaScale, const Color& color) {
    for (int i = 0; i < w; ++i) {
        float c = cov[i];
        if (c <= 0.0f) continue;
        if (mask) {
            c *= mask[x0 + i];
            if (c <= 0.0f) continue;
        }
        if (alphaScale != 1.0f) c *= alphaScale;
        if (c <= 0.0f) continue;
        if (color.a == 0) continue;
        // 内部像素覆盖率恰好是 1（整像素段），省掉 4 次通道乘法；
        // `color * 1.0f` 本来就返回原值，所以这里逐位等价
        if (c >= 1.0f) {
            BlendOne<M>(dstRow + static_cast<size_t>(x0 + i) * 4, color);
        } else {
            BlendOne<M>(dstRow + static_cast<size_t>(x0 + i) * 4, color * c);
        }
    }
}

template <BlendMode M>
inline void RowLinear(uint8_t* dstRow, const float* cov, const float* mask, int x0, int w,
                      float alphaScale, const Paint& paint, const Transform& invXf, int py) {
    const float ax = paint.g1.x - paint.g0.x;
    const float ay = paint.g1.y - paint.g0.y;
    const float invLen2 = 1.0f / (ax * ax + ay * ay);
    // 局部坐标在行内是 px 的线性函数：只有 1 次乘 + 1 次加，不是 4 次乘加
    const float rowY = static_cast<float>(py) + 0.5f;
    const float baseX = invXf.c * rowY + invXf.e - paint.g0.x;
    const float baseY = invXf.d * rowY + invXf.f - paint.g0.y;
    for (int i = 0; i < w; ++i) {
        float c = cov[i];
        if (c <= 0.0f) continue;
        const int px = x0 + i;
        if (mask) {
            c *= mask[px];
            if (c <= 0.0f) continue;
        }
        if (alphaScale != 1.0f) c *= alphaScale;
        if (c <= 0.0f) continue;
        const float lx = invXf.a * (static_cast<float>(px) + 0.5f);
        const float ly = invXf.b * (static_cast<float>(px) + 0.5f);
        float t = ((lx + baseX) * ax + (ly + baseY) * ay) * invLen2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        const Color src0 = Paint::SampleStops(paint.stops, t);
        if (src0.a == 0) continue;
        if (c >= 1.0f) BlendOne<M>(dstRow + static_cast<size_t>(px) * 4, src0);
        else BlendOne<M>(dstRow + static_cast<size_t>(px) * 4, src0 * c);
    }
}

template <BlendMode M>
inline void RowGeneric(uint8_t* dstRow, const float* cov, const float* mask, int x0, int w,
                       float alphaScale, const Paint& paint, const Transform& invXf, int py) {
    const float rowY = static_cast<float>(py) + 0.5f;
    for (int i = 0; i < w; ++i) {
        float c = cov[i];
        if (c <= 0.0f) continue;
        const int px = x0 + i;
        if (mask) {
            c *= mask[px];
            if (c <= 0.0f) continue;
        }
        if (alphaScale != 1.0f) c *= alphaScale;
        if (c <= 0.0f) continue;
        const Point lp = invXf.map(static_cast<float>(px) + 0.5f, rowY);
        const Color src0 = SamplePaint(paint, lp.x, lp.y);
        if (src0.a == 0) continue;
        const Color src = src0 * (c > 1.0f ? 1.0f : c);
        BlendOne<M>(dstRow + static_cast<size_t>(px) * 4, src);
    }
}

enum class CompositeKind { Solid, Linear, Generic };

template <CompositeKind K>
inline void CompositeRowDispatch(BlendMode blend, uint8_t* dstRow, const float* cov,
                                 const float* mask, int x0, int w, float alphaScale,
                                 const Paint& paint, const Color& color,
                                 const Transform& invXf, int py) {
    auto run = [&](auto modeTag) {
        constexpr BlendMode M = decltype(modeTag)::value;
        if (K == CompositeKind::Solid)
            RowSolid<M>(dstRow, cov, mask, x0, w, alphaScale, color);
        else if (K == CompositeKind::Linear)
            RowLinear<M>(dstRow, cov, mask, x0, w, alphaScale, paint, invXf, py);
        else
            RowGeneric<M>(dstRow, cov, mask, x0, w, alphaScale, paint, invXf, py);
    };
    switch (blend) {
    case BlendMode::SrcOver: run(std::integral_constant<BlendMode, BlendMode::SrcOver>{}); break;
    case BlendMode::Replace: run(std::integral_constant<BlendMode, BlendMode::Replace>{}); break;
    case BlendMode::Erase: run(std::integral_constant<BlendMode, BlendMode::Erase>{}); break;
    case BlendMode::Keep: run(std::integral_constant<BlendMode, BlendMode::Keep>{}); break;
    case BlendMode::Additive: run(std::integral_constant<BlendMode, BlendMode::Additive>{}); break;
    }
}

template <CompositeKind K>
inline void CompositeImpl(const RasterTarget& t, const Cover& cover, const Paint& paint,
                          const Color& color, const Transform& invXf, float alphaScale) {
    Surface& dst = *t.surface;
    const int dw = dst.width(), dh = dst.height();
    const int x0 = cover.x0, w = cover.w;
    for (int yy = 0; yy < cover.h; ++yy) {
        const int py = cover.y0 + yy;
        if (py < 0 || py >= dh) continue;
        const float* cov = cover.row(yy);
        const float* mask = t.mask ? t.mask + static_cast<size_t>(py) * static_cast<size_t>(dw)
                                   : nullptr;
        CompositeRowDispatch<K>(t.blend, dst.row(py), cov, mask, x0, w, alphaScale, paint,
                                color, invXf, py);
    }
}

void Composite(const RasterTarget& t, const Cover& cover, const Paint& paint,
               const Transform& invXf, float alphaScale) {
    if (cover.empty() || !t.valid()) return;
    if (paint.kind == PaintKind::Solid) {
        CompositeImpl<CompositeKind::Solid>(t, cover, paint, paint.color, invXf, alphaScale);
        return;
    }
    if (paint.kind == PaintKind::Linear) {
        const float ax = paint.g1.x - paint.g0.x;
        const float ay = paint.g1.y - paint.g0.y;
        if (ax * ax + ay * ay < 1e-12f) {
            const Color flat = paint.stops.empty() ? Color::trans() : paint.stops.back().color;
            CompositeImpl<CompositeKind::Solid>(t, cover, paint, flat, invXf, alphaScale);
            return;
        }
        CompositeImpl<CompositeKind::Linear>(t, cover, paint, Color::trans(), invXf, alphaScale);
        return;
    }
    CompositeImpl<CompositeKind::Generic>(t, cover, paint, Color::trans(), invXf, alphaScale);
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

// 位图绘制有三条路径，按"最省"的顺序试：
//   ① 1:1 平移（图层合成、贴图不缩放）：整行整数搬运，连采样都不用；
//   ② 轴对齐缩放（绝大多数图标/贴图）：x/y 方向的源索引与双线性权重**预算成表**，
//      内层只查表，不再每像素做两次除法 + 4 次边界钳制；
//   ③ 旋转/斜切：保留 4×4 子采样的通用路径（这条本来就慢，但不是热点）。
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
    const bool fullUv = srcUv.x == 0.0f && srcUv.y == 0.0f && srcUv.w == 1.0f && srcUv.h == 1.0f;
    const int sw = image.width(), sh = image.height();

    // ---------------- ① 1:1 平移合成 ----------------
    if (!t.mask && fullUv && axisAligned && nearest == false &&
        std::fabs(xf.a - 1.0f) < 1e-4f && std::fabs(xf.d - 1.0f) < 1e-4f &&
        std::fabs(dstLocal.w - static_cast<float>(sw)) < 1e-3f &&
        std::fabs(dstLocal.h - static_cast<float>(sh)) < 1e-3f) {
        const int dx = static_cast<int>(std::lround(devRect.x));
        const int dy = static_cast<int>(std::lround(devRect.y));
        const bool plainTint = tint.r == 255 && tint.g == 255 && tint.b == 255 && tint.a == 255;
        for (int py = y0; py < y1; ++py) {
            const int sy = py - dy;
            if (sy < 0 || sy >= sh) continue;
            const uint8_t* s = image.row(sy);
            uint8_t* d = dst.row(py);
            for (int px = x0; px < x1; ++px) {
                const int sx = px - dx;
                if (sx < 0 || sx >= sw) continue;
                const uint8_t* sp = s + static_cast<size_t>(sx) * 4;
                uint8_t* dp = d + static_cast<size_t>(px) * 4;
                uint8_t r = sp[0], g = sp[1], b = sp[2], a = sp[3];
                if (!plainTint) {
                    r = Div255(static_cast<uint32_t>(r) * tint.r);
                    g = Div255(static_cast<uint32_t>(g) * tint.g);
                    b = Div255(static_cast<uint32_t>(b) * tint.b);
                    a = Div255(static_cast<uint32_t>(a) * tint.a);
                }
                if (a == 0) continue;
                const uint32_t ia = 255u - a;
                dp[0] = static_cast<uint8_t>(r + Div255(static_cast<uint32_t>(dp[0]) * ia));
                dp[1] = static_cast<uint8_t>(g + Div255(static_cast<uint32_t>(dp[1]) * ia));
                dp[2] = static_cast<uint8_t>(b + Div255(static_cast<uint32_t>(dp[2]) * ia));
                dp[3] = static_cast<uint8_t>(a + Div255(static_cast<uint32_t>(dp[3]) * ia));
            }
        }
        return;
    }

    // ---------------- ② 轴对齐缩放：查表 ----------------
    if (axisAligned) {
        const int spanW = x1 - x0;
        std::vector<int> ix0(static_cast<size_t>(spanW)), ix1(static_cast<size_t>(spanW));
        std::vector<float> wx(static_cast<size_t>(spanW));
        const float invA = 1.0f / xf.a, invD = 1.0f / xf.d;
        for (int i = 0; i < spanW; ++i) {
            const int px = x0 + i;
            const float lx = (static_cast<float>(px) + 0.5f - xf.e) * invA;
            const float u = srcUv.x + ((lx - dstLocal.x) / dstLocal.w) * srcUv.w;
            const float cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
            const float fx = cu * static_cast<float>(sw - 1);
            const int base = static_cast<int>(std::floor(fx));
            ix0[static_cast<size_t>(i)] = base < 0 ? 0 : (base >= sw ? sw - 1 : base);
            const int next = base + 1;
            ix1[static_cast<size_t>(i)] = next < 0 ? 0 : (next >= sw ? sw - 1 : next);
            wx[static_cast<size_t>(i)] = fx - static_cast<float>(base);
        }
        const uint8_t* srcRows = image.row(0);
        const int stride = image.stride();
        const bool plainTint = tint.r == 255 && tint.g == 255 && tint.b == 255 && tint.a == 255;
        const Rect inner(x0 + 1.0f, y0 + 1.0f, static_cast<float>(spanW) - 2.0f,
                         static_cast<float>(y1 - y0) - 2.0f);
        for (int py = y0; py < y1; ++py) {
            const float ly = (static_cast<float>(py) + 0.5f - xf.f) * invD;
            const float v = srcUv.y + ((ly - dstLocal.y) / dstLocal.h) * srcUv.h;
            const float cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            const float fy = cv * static_cast<float>(sh - 1);
            const int by = static_cast<int>(std::floor(fy));
            const int ry0 = by < 0 ? 0 : (by >= sh ? sh - 1 : by);
            const int ry1 = (by + 1) < 0 ? 0 : ((by + 1) >= sh ? sh - 1 : (by + 1));
            const float tyy = fy - static_cast<float>(by);
            const bool interiorRow = (static_cast<float>(py) >= inner.y &&
                                      static_cast<float>(py) < inner.bottom());
            uint8_t* drow = dst.row(py);
            for (int i = 0; i < spanW; ++i) {
                const int px = x0 + i;
                float cov;
                if (interiorRow && static_cast<float>(px) >= inner.x &&
                    static_cast<float>(px) < inner.right()) {
                    cov = 1.0f;
                } else {
                    cov = RectCoverage(devRect, px, py);
                    if (cov <= 0.0f) continue;
                }
                const uint8_t* s0 = srcRows + static_cast<size_t>(ry0) * static_cast<size_t>(stride);
                const uint8_t* s1 = srcRows + static_cast<size_t>(ry1) * static_cast<size_t>(stride);
                if (t.mask) {
                    cov *= t.mask[static_cast<size_t>(py) * static_cast<size_t>(dw) +
                                  static_cast<size_t>(px)];
                    if (cov <= 0.0f) continue;
                }
                const int a0 = ix0[static_cast<size_t>(i)] * 4;
                const int a1 = ix1[static_cast<size_t>(i)] * 4;
                const float txw = wx[static_cast<size_t>(i)];
                // 与 core.h 的 lerp() 完全相同的两步取整（先横向再纵向、每步四舍五入），
                // 这样查表版与原来的 SampleSurface 逐值一致，不是"更平滑但不一样"
                auto lerp8 = [txw](uint8_t p0, uint8_t p1) {
                    const float f = static_cast<float>(p0) +
                                    (static_cast<float>(p1) - static_cast<float>(p0)) * txw;
                    return static_cast<uint8_t>(f + 0.5f);
                };
                auto lerpY = [tyy](uint8_t p0, uint8_t p1) {
                    const float f = static_cast<float>(p0) +
                                    (static_cast<float>(p1) - static_cast<float>(p0)) * tyy;
                    return static_cast<uint8_t>(f + 0.5f);
                };
                const uint8_t c0 = lerpY(lerp8(s0[a0], s0[a1]), lerp8(s1[a0], s1[a1]));
                const uint8_t c1 = lerpY(lerp8(s0[a0 + 1], s0[a1 + 1]), lerp8(s1[a0 + 1], s1[a1 + 1]));
                const uint8_t c2 = lerpY(lerp8(s0[a0 + 2], s0[a1 + 2]), lerp8(s1[a0 + 2], s1[a1 + 2]));
                const uint8_t c3 = lerpY(lerp8(s0[a0 + 3], s0[a1 + 3]), lerp8(s1[a0 + 3], s1[a1 + 3]));
                Color c(c0, c1, c2, c3);
                if (!plainTint) {
                    c = Color(Div255(static_cast<uint32_t>(c.r) * tint.r),
                              Div255(static_cast<uint32_t>(c.g) * tint.g),
                              Div255(static_cast<uint32_t>(c.b) * tint.b),
                              Div255(static_cast<uint32_t>(c.a) * tint.a));
                }
                if (c.a == 0) continue;
                c = c * (cov > 1.0f ? 1.0f : cov);
                BlendOne<BlendMode::SrcOver>(drow + static_cast<size_t>(px) * 4, c);
            }
        }
        return;
    }

    // ---------------- ③ 通用（旋转/斜切） ----------------
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
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
            float cov = static_cast<float>(hits) / 16.0f;
            const float u = cu / static_cast<float>(hits);
            const float v = cv / static_cast<float>(hits);
            if (t.mask) cov *= t.mask[static_cast<size_t>(py) * static_cast<size_t>(dw) +
                                      static_cast<size_t>(px)];
            if (cov <= 0.0f) continue;

            Color c = SampleSurface(image, u, v, nearest);
            c = Color(Div255(static_cast<uint32_t>(c.r) * tint.r),
                      Div255(static_cast<uint32_t>(c.g) * tint.g),
                      Div255(static_cast<uint32_t>(c.b) * tint.b),
                      Div255(static_cast<uint32_t>(c.a) * tint.a));
            if (c.a == 0) continue;
            c = c * (cov > 1.0f ? 1.0f : cov);
            const Color out = BlendPixel(dst.get(px, py), c, t.blend);
            dst.set(px, py, out);
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
// 一行/一列的盒式模糊都靠"前缀和"：窗口和 = pre[x+win] - pre[x]。
//
// 为什么要改（原来每像素 21 次累加 ×2 遍 = 42 次/像素/通道，r=8 时 78 ns/px）：
//   · 盒式窗的 taps 就是相邻整数的集合，前缀和差出的和与"逐 tap 累加"**逐位相同**
//     （整数加法无序差），所以结果不变、复杂度从 O(rad) 降到 O(1)。
//   · 边界处"钳制到边缘像素"用一张**填充后的行缓冲**表达：p[i] = row[clamp(i-rad)]，
//     窗口就是 p[x..x+win-1]，不用在循环里写分支。
// 除法的除数 window 是编译期才知道的值，用整数除即可（每像素 4 次，不是热点）。
namespace {

void BoxBlurRows(Surface& s, int rad) {
    const int w = s.width(), h = s.height();
    const int win = rad * 2 + 1;
    const int padded = w + rad * 2;
    std::vector<uint8_t> pad(static_cast<size_t>(padded) * 4);
    std::vector<uint32_t> pre(static_cast<size_t>(padded + 1) * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = s.row(y);
        // 填充（边缘复制的钳制就在这里一次做完）
        for (int i = 0; i < padded; ++i) {
            int x = i - rad;
            x = x < 0 ? 0 : (x >= w ? w - 1 : x);
            std::memcpy(pad.data() + static_cast<size_t>(i) * 4, src + static_cast<size_t>(x) * 4, 4);
        }
        // 前缀和
        for (int c = 0; c < 4; ++c) pre[c] = 0;
        for (int i = 0; i < padded; ++i) {
            const uint8_t* p = pad.data() + static_cast<size_t>(i) * 4;
            uint32_t* q = pre.data() + static_cast<size_t>(i + 1) * 4;
            const uint32_t* r = pre.data() + static_cast<size_t>(i) * 4;
            for (int c = 0; c < 4; ++c) q[c] = r[c] + p[c];
        }
        // 窗口和 → 输出
        uint8_t* dst = s.row(y);
        for (int x = 0; x < w; ++x) {
            const uint32_t* hi = pre.data() + static_cast<size_t>(x + win) * 4;
            const uint32_t* lo = pre.data() + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c)
                dst[static_cast<size_t>(x) * 4 + c] =
                    static_cast<uint8_t>((hi[c] - lo[c] + static_cast<uint32_t>(win / 2)) / static_cast<uint32_t>(win));
        }
    }
}

void BoxBlurCols(Surface& s, int rad) {
    const int w = s.width(), h = s.height();
    const int win = rad * 2 + 1;
    const int padded = h + rad * 2;
    std::vector<uint8_t> pad(static_cast<size_t>(padded) * 4);
    std::vector<uint32_t> pre(static_cast<size_t>(padded + 1) * 4);
    for (int x = 0; x < w; ++x) {
        for (int i = 0; i < padded; ++i) {
            int y = i - rad;
            y = y < 0 ? 0 : (y >= h ? h - 1 : y);
            std::memcpy(pad.data() + static_cast<size_t>(i) * 4,
                        s.row(y) + static_cast<size_t>(x) * 4, 4);
        }
        for (int c = 0; c < 4; ++c) pre[c] = 0;
        for (int i = 0; i < padded; ++i) {
            const uint8_t* p = pad.data() + static_cast<size_t>(i) * 4;
            uint32_t* q = pre.data() + static_cast<size_t>(i + 1) * 4;
            const uint32_t* r = pre.data() + static_cast<size_t>(i) * 4;
            for (int c = 0; c < 4; ++c) q[c] = r[c] + p[c];
        }
        for (int y = 0; y < h; ++y) {
            const uint32_t* hi = pre.data() + static_cast<size_t>(y + win) * 4;
            const uint32_t* lo = pre.data() + static_cast<size_t>(y) * 4;
            uint8_t* dst = s.row(y) + static_cast<size_t>(x) * 4;
            for (int c = 0; c < 4; ++c)
                dst[c] = static_cast<uint8_t>((hi[c] - lo[c] + static_cast<uint32_t>(win / 2)) /
                                              static_cast<uint32_t>(win));
        }
    }
}

} // namespace

void Blur(Surface& s, float radius) {
    if (s.empty() || radius <= 0.0f) return;
    const int rad = static_cast<int>(std::lround(radius));
    if (rad < 1) return;
    BoxBlurRows(s, rad);
    BoxBlurCols(s, rad);
}

} // namespace bin2d