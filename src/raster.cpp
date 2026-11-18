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
// 边表：跨调用复用（每线程一份；容量按最长路径增长一次就不再分配）
std::vector<Edge>& edgesScratch() {
    static thread_local std::vector<Edge> edges;
    return edges;
}

// 建边表：设备空间折线（bin2d 自己的 Polyline 批）
void BuildEdges(const std::vector<Polyline>& polys, std::vector<Edge>& edges) {
    size_t edgeCount = 0;
    for (const Polyline& pl : polys) {
        const size_t n = pl.pts.size();
        if (n >= 2) edgeCount += pl.closed ? n : n - 1;
    }
    edges.clear();
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
}

// 建边表：交错 float + 折线表（宿主免拷贝的那条路；填充语义 = 首尾自动闭合）
void BuildEdgesInterleaved(const float* xy, const uint32_t* first, const uint32_t* count,
                           int contours, std::vector<Edge>& edges) {
    size_t total = 0;
    for (int c = 0; c < contours; c++)
        if (count[c] >= 2) total += count[c];
    edges.clear();
    edges.reserve(total);
    for (int c = 0; c < contours; c++) {
        const uint32_t f = first[c];
        const uint32_t n = count[c];
        if (n < 2) continue;
        for (uint32_t i = 0; i < n; i++) {
            const size_t ka = static_cast<size_t>(f + i) * 2;
            const size_t kb = static_cast<size_t>(f + (i + 1) % n) * 2;
            const float ax = xy[ka], ay = xy[ka + 1];
            const float bx = xy[kb], by = xy[kb + 1];
            if (ay == by) continue;
            Edge e;
            const float dy = by - ay;
            e.dyInv = (bx - ax) / dy;
            if (ay < by) {
                e.x0 = ax; e.y0 = ay; e.dir = 1;
                e.ymin = ay; e.ymax = by;
            } else {
                e.x0 = bx; e.y0 = by; e.dir = -1;
                e.ymin = by; e.ymax = ay;
            }
            edges.push_back(e);
        }
    }
}

// 交叉点数组：跨调用复用（每线程一份）
std::vector<Crossing>& xsScratch() {
    static thread_local std::vector<Crossing> xs;
    return xs;
}

std::vector<int>& activeScratch() {
    static thread_local std::vector<int> v;
    return v;
}

// 三个行缓冲：一次长到最宽的那个形状，之后不再分配
float* rowAccScratch(int w, bool& grew) {
    static thread_local std::vector<float> v;
    if (static_cast<int>(v.size()) < w) {
        v.resize(static_cast<size_t>(w), 0.0f);
        grew = true;
    }
    return v.data();
}

float* rowDeltaScratch(int w, bool& grew) {
    static thread_local std::vector<float> v;
    if (static_cast<int>(v.size()) < w) {
        v.resize(static_cast<size_t>(w), 0.0f);
        grew = true;
    }
    return v.data();
}

float* rowCovScratch(int w, bool& grew) {
    static thread_local std::vector<float> v;
    if (static_cast<int>(v.size()) < w) {
        v.resize(static_cast<size_t>(w), 0.0f);
        grew = true;
    }
    return v.data();
}



// 行循环：一批已建好的边 → 逐行覆盖率，每落一个像素回调 sink(yy, xi, c)，c 已钳到 [0,1]。
// **落覆盖率的出口只有这一处**：合成路径（float 缓冲）与导出的覆盖率 API（uint8）都走
// 这里，所以"覆盖率"在库里只有一份实现，不会漂。
template <class RowSink>
void RasterEdges(std::vector<Edge>& edges, FillRule rule, int baseX, int y0, int w, int h,
                 int nSub, bool hardEdge, RowSink&& sink) {
    if (edges.empty() || w <= 0 || h <= 0) return;
    std::sort(edges.begin(), edges.end(),
              [](const Edge& a, const Edge& b) { return a.ymin < b.ymin; });
    const float subStep = 1.0f / static_cast<float>(nSub);
    const float subWeight = 1.0f / static_cast<float>(nSub);
    // 全部缓冲**每线程复用**（一个形状每帧只走一次这里），但访问一律走裸指针：
    // 热循环里直接写 `rowAcc[k]` 会让每次访问都过一遍 TLS（实测慢 ~40%），
    // 函数入口取一次 `.data()` 就没有这个问题。
    std::vector<Crossing>& xsV = xsScratch();
    if (xsV.size() < edges.size()) xsV.resize(edges.size());   // 指针写入前必须保证容量
    Crossing* xs = xsV.data();
    std::vector<int>& active = activeScratch();
    active.clear();
    active.reserve(edges.size());
    // rowAcc = 每行的"端像素小数覆盖率"；rowDelta = 整像素段的差分（前缀和还原）；
    // rowCov = 本行的最终覆盖率（交给 sink 一整行，而不是逐像素回调）。
    // 三者只在**被碰到的列**上读写，行尾按 touched 区间清回 0 —— 见 0.4.2 的性能轮。
    bool grew = false;
    float* rowAcc = rowAccScratch(w, grew);
    float* rowDelta = rowDeltaScratch(w, grew);
    float* rowCov = rowCovScratch(w, grew);
    size_t nextEdge = 0;

    for (int yy = 0; yy < h; ++yy) {
        const int py = y0 + yy;
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

        // 本行被写过的列区间（含右端那个"取消项"列）：行内全部子扫描线共用，
        // 前缀和只在 [tLo,tHi] 上跑一遍，不再整行遍历
        int tLo = w;
        int tHi = -1;

        for (int sub = 0; sub < nSub; ++sub) {
            const float sy = rowTop + (static_cast<float>(sub) + 0.5f) * subStep;
            int nCross = 0;
            for (size_t k = 0; k < active.size(); ++k) {
                const Edge& e = edges[static_cast<size_t>(active[k])];
                if (sy < e.ymin || sy >= e.ymax) continue;
                xs[nCross++] = Crossing{e.x0 + (sy - e.y0) * e.dyInv, e.dir};
            }
            if (nCross == 0) continue;
            const int nXs = nCross;

            // 交叉点通常只有 2~8 个：插入排序比 std::sort 的通用派发快一个数量级
            for (int i = 1; i < nXs; ++i) {
                const Crossing key = xs[i];
                int j = i;
                while (j > 0 && (xs[j - 1].x > key.x ||
                                 (xs[j - 1].x == key.x && xs[j - 1].dir > key.dir))) {
                    xs[j] = xs[j - 1];
                    --j;
                }
                xs[j] = key;
            }
            // 把 [xa,xb] 区间按像素小数部分累进本行覆盖率。
            //
            // 0.4.2：整像素段原来是"逐像素 += subWeight"的循环（一个 120px 宽的行，
            // 每条子扫描线要走一遍 120 次加法）。现在改成差分：两端各写一次
            // +subWeight / -subWeight，行尾用一次前缀和还原。加法次序变了，
            // 但浮点结果在端像素之外**恰好等于** 8 × 1/8 = 1.0（0.125 是二进制精确值）。
            //
            // touched 区间必须同时盖住"最后一个落覆盖率的列"(ib-1) 和"取消项所在的列"(ib)：
            // 漏掉 ib 会让 rowDelta 留下没被清零的残值，污染下一行。
            auto note = [&](int xiLo, int xiHi) {
                if (xiLo < tLo) tLo = xiLo;
                const int hiCol = xiHi < w ? xiHi : w - 1;
                if (hiCol > tHi) tHi = hiCol;
            };
            auto addSpan = [&](float xa, float xb) {
                if (xb <= xa) return;
                const float minX = static_cast<float>(baseX);
                const float maxX = static_cast<float>(baseX + w);
                float lo = xa < minX ? minX : xa;
                float hi = xb > maxX ? maxX : xb;
                if (hardEdge) {
                    // 硬边：端点先落到整像素（与老 binUI 的 round(端点) 铺满整像素等价）。
                    // 亚像素宽的细条因此仍然占一整像素，不会因为"覆盖率不到一半"消失。
                    lo = std::round(lo);
                    hi = std::round(hi);
                    if (hi > maxX) hi = maxX;
                }
                if (hi <= lo) return;
                const int ia = static_cast<int>(std::floor(lo));
                const int ib = static_cast<int>(std::floor(hi));
                const int xiA = ia - baseX;
                const int xiB = ib - baseX;
                if (ia == ib) {
                    if (xiA >= 0 && xiA < w) {
                        rowAcc[static_cast<size_t>(xiA)] += (hi - lo) * subWeight;
                        note(xiA, xiA);
                    }
                    return;
                }
                note(xiA, xiB);
                // 左端的不完整像素
                if (xiA >= 0 && xiA < w)
                    rowAcc[static_cast<size_t>(xiA)] +=
                        (static_cast<float>(ia + 1) - lo) * subWeight;
                // 中间整像素段 [ia+1, ib-1]：差分，一次加减代替整段循环
                if (xiA + 1 < w) rowDelta[static_cast<size_t>(xiA + 1)] += subWeight;
                if (xiB < w) rowDelta[static_cast<size_t>(xiB)] -= subWeight;
                // 右端的不完整像素
                const float rightFrac = hi - static_cast<float>(ib);
                if (rightFrac > 0.0f && xiB >= 0 && xiB < w)
                    rowAcc[static_cast<size_t>(xiB)] += rightFrac * subWeight;
            };

            if (rule == FillRule::EvenOdd) {
                for (int i = 0; i + 1 < nXs; i += 2) addSpan(xs[i].x, xs[i + 1].x);
            } else {
                int winding = 0;
                for (int i = 0; i + 1 < nXs; ++i) {
                    winding += xs[i].dir;
                    if (winding != 0) addSpan(xs[i].x, xs[i + 1].x);
                }
            }
        }

        if (tHi < tLo) continue;
        // 前缀和：只在 [tLo,tHi] 上，顺带把两个累加器清回 0。
        // 每个像素最多收到 nSub × 1/nSub，min(1,c) 一次就够，省掉整行的二次 clamp。
        float run = 0.0f;
        for (int xi = tLo; xi <= tHi; ++xi) {
            const size_t k = static_cast<size_t>(xi);
            run += rowDelta[k];
            const float c = rowAcc[k] + run;
            rowAcc[k] = 0.0f;
            rowDelta[k] = 0.0f;
            rowCov[k] = c <= 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
        }
        sink(yy, tLo, tHi, rowCov);
    }
}

// 公共尾巴：建好的边 + 覆盖率缓冲 → 8bit 导出
// 包围盒 ∩ 裁剪 → 整数像素矩形（与 Cover::reset 同一套取整规则，只是不分配/不清零）
bool ClipPixels(const Rect& bbox, const Rect& clip, int& x0, int& y0, int& w, int& h) {
    const Rect r = Rect::intersect(bbox, clip);
    if (r.empty()) return false;
    x0 = static_cast<int>(std::floor(r.x));
    y0 = static_cast<int>(std::floor(r.y));
    const int x1 = static_cast<int>(std::ceil(r.right()));
    const int y1 = static_cast<int>(std::ceil(r.bottom()));
    w = x1 - x0;
    h = y1 - y0;
    return w > 0 && h > 0;
}

bool RasterizeCoverageEdges(std::vector<Edge>& edges, FillRule rule, int x0, int y0, int w,
                            int h, int subSamples, CoverageStyle style, CoverageRect& out) {
    if (edges.empty() || w <= 0 || h <= 0) return false;
    const int nSub = subSamples < 1 ? 1 : (subSamples > 32 ? 32 : subSamples);
    out.x0 = x0;
    out.y0 = y0;
    out.w = w;
    out.h = h;
    out.v.assign(static_cast<size_t>(out.w) * static_cast<size_t>(out.h), 0);
    RasterEdges(edges, rule, out.x0, out.y0, out.w, out.h, nSub,
                style == CoverageStyle::HardEdge,
                [&out](int yy, int tLo, int tHi, const float* cov) {
                    uint8_t* row = out.v.data() + static_cast<size_t>(yy) *
                                                     static_cast<size_t>(out.w);
                    for (int xi = tLo; xi <= tHi; ++xi) {
                        const float c = cov[xi];
                        // 与内部 Composite 的"整像素 = 1.0"同一判据：1.0 直接给 255
                        row[xi] = (c <= 0.0f) ? 0
                                              : (c >= 1.0f ? 255
                                                           : static_cast<uint8_t>(c * 255.0f + 0.5f));
                    }
                });
    return true;
}

// 覆盖率 → float 缓冲（FillPath / StrokePath 用；unionMode = 描边多块取 max）
void RasterPolys(const std::vector<Polyline>& polys, FillRule rule, bool unionMode,
                 Cover& cover, int subSamples = kSubSamples, bool hardEdge = false) {
    if (cover.empty()) return;
    // 竖直子采样数：默认 kSubSamples（8）。参数化是为了导出的覆盖率 API ——
    // 宿主若要和自己的老管线逐位对齐（binUI 迁移期 = 4），就得能指定。
    // 内部所有落像素路径一律用默认值，所以"像素权威"没有变成可调项。
    const int nSub = subSamples < 1 ? 1 : (subSamples > 32 ? 32 : subSamples);
    std::vector<Edge>& edges = edgesScratch();
    BuildEdges(polys, edges);
    RasterEdges(edges, rule, cover.x0, cover.y0, cover.w, cover.h, nSub, hardEdge,
                [&cover, unionMode](int yy, int tLo, int tHi, const float* cov) {
                    float* dstRow = cover.row(yy);
                    for (int xi = tLo; xi <= tHi; ++xi) {
                        const float c = cov[xi];
                        if (c <= 0.0f) continue;
                        if (unionMode) dstRow[xi] = std::max(dstRow[xi], c);
                        else dstRow[xi] += c;
                    }
                });
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
    // Replace/Keep 在 src.a==0 时仍然要落笔（清成透明），其余三种是真·空操作
    if (src.a == 0 && !ZeroAlphaMatters(M)) return;
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
        if (color.a == 0 && !ZeroAlphaMatters(M)) continue;
        // 内部像素覆盖率恰好是 1（整像素段），省掉 4 次通道乘法；
        // `color * 1.0f` 本来就返回原值，所以这里逐位等价
        if (c >= 1.0f) {
            BlendOne<M>(dstRow + static_cast<size_t>(x0 + i) * 4, color);
        } else {
            BlendOne<M>(dstRow + static_cast<size_t>(x0 + i) * 4, color * c);
        }
    }
}

template <BlendMode M, bool kDither>
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
        // 抖动与否是**模板参数**（不是行内 if）：行内分支会让整条循环无法向量化，
        // 实测渐变从 6.3 掉到 9.6 ns/px；分派到两个版本后回到 6.6。
        // 抖动图样钉在**设备像素**上（不是局部坐标），否则形状一动抖点就爬行。
        const Color src0 = kDither ? Paint::SampleStopsDithered(paint.stops, t, px, py)
                                   : Paint::SampleStops(paint.stops, t);
        if (src0.a == 0 && !ZeroAlphaMatters(M)) continue;
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
        if (src0.a == 0 && !ZeroAlphaMatters(M)) continue;
        const Color src = src0 * (c > 1.0f ? 1.0f : c);
        BlendOne<M>(dstRow + static_cast<size_t>(px) * 4, src);
    }
}

enum class CompositeKind { Solid, Linear, Generic };

// 这条渐变**会不会出现宽于 3 像素的色带**？会才抖动。
//
// 抖动是给"级数不够"擦屁股的：预乘 8 位下浅渐变（低 alpha、同色系）几百像素才走完
// 八级，色带宽 30+ 像素，一眼看得见；而鲜艳的大跨度渐变本来每 3~4 像素就换一级，
// 抖它既改善不了观感，又要多付 45% 的时间（实测 6.3 → 9.2 ns/px）。
// 判据：渐变轴在设备空间的像素长度 / 该段最大通道差 = 每一级占几个像素。
inline bool GradientNeedsDither(const Paint& paint, const Transform& invXf) {
    if (!paint.dither || paint.stops.size() < 2) return false;
    const float ax = paint.g1.x - paint.g0.x, ay = paint.g1.y - paint.g0.y;
    const float len = std::sqrt(ax * ax + ay * ay);
    if (len < 1e-6f) return false;
    // 局部长度 → 设备长度：各向同性近似（各向异性缩放下取几何平均）
    const float detInv = std::fabs(invXf.det());
    const float devLen = len / std::sqrt(std::max(1e-12f, detInv));
    float minPxPerLevel = 1e30f;
    for (size_t i = 1; i < paint.stops.size(); ++i) {
        const float dt = paint.stops[i].t - paint.stops[i - 1].t;
        const int d = std::max({std::abs(int(paint.stops[i].color.r) - int(paint.stops[i - 1].color.r)),
                                std::abs(int(paint.stops[i].color.g) - int(paint.stops[i - 1].color.g)),
                                std::abs(int(paint.stops[i].color.b) - int(paint.stops[i - 1].color.b)),
                                std::abs(int(paint.stops[i].color.a) - int(paint.stops[i - 1].color.a))});
        if (d == 0) continue;   // 这一段是纯色，不产生色带
        minPxPerLevel = std::min(minPxPerLevel, (devLen * dt) / static_cast<float>(d));
    }
    return minPxPerLevel > 3.0f;
}

template <CompositeKind K>
inline void CompositeRowDispatch(BlendMode blend, bool dither, uint8_t* dstRow, const float* cov,
                                 const float* mask, int x0, int w, float alphaScale,
                                 const Paint& paint, const Color& color,
                                 const Transform& invXf, int py) {
    auto run = [&](auto modeTag) {
        constexpr BlendMode M = decltype(modeTag)::value;
        if (K == CompositeKind::Solid)
            RowSolid<M>(dstRow, cov, mask, x0, w, alphaScale, color);
        else if (K == CompositeKind::Linear) {
            // 抖动开关按整次绘制分派一次（模板参数 → 两条不分支的内层循环），
            // 而"要不要抖"由色带宽度决定（见 GradientNeedsDither）
            if (dither)
                RowLinear<M, true>(dstRow, cov, mask, x0, w, alphaScale, paint, invXf, py);
            else
                RowLinear<M, false>(dstRow, cov, mask, x0, w, alphaScale, paint, invXf, py);
        } else {
            RowGeneric<M>(dstRow, cov, mask, x0, w, alphaScale, paint, invXf, py);
        }
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
    // 抖动决策一次（整次绘制），不是逐像素也不是逐行
    const bool dither = (K == CompositeKind::Linear) && GradientNeedsDither(paint, invXf);
    for (int yy = 0; yy < cover.h; ++yy) {
        const int py = cover.y0 + yy;
        if (py < 0 || py >= dh) continue;
        const float* cov = cover.row(yy);
        const float* mask = t.mask ? t.mask + static_cast<size_t>(py) * static_cast<size_t>(dw)
                                   : nullptr;
        CompositeRowDispatch<K>(t.blend, dither, dst.row(py), cov, mask, x0, w, alphaScale, paint,
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
// ------------------------------------------------------------
// 覆盖率查询（给"自己有条带/四边形管线"的宿主用）
//
// 与 FillPath 共用**同一个光栅器**（RasterEdges）：这里只是把覆盖率导成 8bit，
// 不另写一套扫描线。刻意如此 —— 两套实现一定会漂移，而边缘漂移在 AA 上是肉眼可见的。
// 交错数组那条路直接建边、直接写 uint8，不拷贝 Polyline、不经过 float 缓冲。
// ------------------------------------------------------------
bool RasterizeCoverage(const std::vector<Polyline>& polys, FillRule rule, const Rect& clip,
                       int subSamples, CoverageRect& out, CoverageStyle style) {
    out.x0 = out.y0 = out.w = out.h = 0;
    out.v.clear();
    if (polys.empty() || clip.empty()) return false;
    const Rect bbox = PolysBounds(polys);
    if (bbox.empty()) return false;
    int x0 = 0, y0 = 0, w = 0, h = 0;
    if (!ClipPixels(bbox, clip, x0, y0, w, h)) return false;
    std::vector<Edge>& edges = edgesScratch();
    BuildEdges(polys, edges);
    return RasterizeCoverageEdges(edges, rule, x0, y0, w, h, subSamples, style, out);
}

bool RasterizeCoverage(const CoverageRequest& req, CoverageRect& out) {
    out.x0 = out.y0 = out.w = out.h = 0;
    out.v.clear();
    if (!req.valid() || req.clip.empty()) return false;
    // 包围盒自己算（宿主给的是整条路径的设备坐标）
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool first = true;
    for (int c = 0; c < req.contours; c++) {
        const uint32_t f = req.first[c];
        const uint32_t n = req.count[c];
        for (uint32_t i = 0; i < n; i++) {
            const size_t k = static_cast<size_t>(f + i) * 2;
            const float x = req.xy[k], y = req.xy[k + 1];
            if (first) {
                minx = maxx = x; miny = maxy = y; first = false;
                continue;
            }
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    if (first) return false;
    int x0 = 0, y0 = 0, w = 0, h = 0;
    if (!ClipPixels(Rect(minx, miny, maxx - minx, maxy - miny), req.clip, x0, y0, w, h))
        return false;
    std::vector<Edge>& edges = edgesScratch();
    BuildEdgesInterleaved(req.xy, req.first, req.count, req.contours, edges);
    return RasterizeCoverageEdges(edges, req.rule, x0, y0, w, h, req.subSamples, req.style,
                                  out);
}

// ------------------------------------------------------------
// 描边覆盖率（0.6.0）
//
// 与 StrokePath 同源：BuildStrokePolys 出"四边形 + 连接块 + 端帽"，然后**逐块**栅格化、
// 在同一张 uint8 上取 max —— 这就是并集。逐块而不是合成一个边表：方向相反的四边形放进
// 同一个 nonzero 边表会互相抵消（拐点开洞），而逐块取 max 与"一个像素只混一次"的语义
// 完全对齐（半透明描边的接缝不会变深）。
//
// 每块只用**自己**的像素矩形调用行循环（RasterEdges 的 baseX/y0/w/h），所以一块只是一小段
// 行，成本跟"块的面积"成正比，而不是跟整条路径的包围盒成正比。
// ------------------------------------------------------------
namespace {

// 一圈顶点（隐式闭合）→ 边表。描边块全部是闭合小环（四边形 / 三角 / 圆）。
void BuildEdgesRing(const std::vector<Point>& ring, std::vector<Edge>& edges) {
    edges.clear();
    const size_t n = ring.size();
    if (n < 3) return;
    edges.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Point& a = ring[i];
        const Point& b = ring[(i + 1) % n];
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

Rect RingBounds(const std::vector<Point>& ring) {
    if (ring.empty()) return Rect();
    float minx = ring[0].x, maxx = ring[0].x, miny = ring[0].y, maxy = ring[0].y;
    for (const Point& p : ring) {
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    return Rect(minx, miny, maxx - minx, maxy - miny);
}

} // namespace

bool RasterizeStrokeCoverage(const std::vector<Polyline>& polys, const StrokeStyle& style,
                            const Rect& clip, int subSamples, CoverageRect& out) {
    out.x0 = out.y0 = out.w = out.h = 0;
    out.v.clear();
    if (polys.empty() || clip.empty() || style.width <= 0.0f) return false;

    std::vector<std::vector<Point>> pieces;
    BuildStrokePolys(polys, style, pieces);
    if (pieces.empty()) return false;

    // 并集的包围盒 = 块的顶点框（块的顶点已经是"最外的那层"，端口帽/斜接尖角都在里面）。
    // 不外扩：块的顶点就是几何的极值，floor/ceil 已经覆盖到"被斜边切到一半"的边像素 ——
    // 外扩只会让宿主多扫一圈空行（导出矩形是宿主要遍历的范围，别白给）。
    Rect bbox;
    bool first = true;
    for (const std::vector<Point>& piece : pieces) {
        if (piece.size() < 3) continue;
        const Rect pb = RingBounds(piece);
        if (first) {
            bbox = pb;
            first = false;
        } else {
            const float minx = std::min(bbox.x, pb.x);
            const float miny = std::min(bbox.y, pb.y);
            const float maxx = std::max(bbox.right(), pb.right());
            const float maxy = std::max(bbox.bottom(), pb.bottom());
            bbox = Rect(minx, miny, maxx - minx, maxy - miny);
        }
    }
    if (first) return false;

    int ax0 = 0, ay0 = 0, aw = 0, ah = 0;
    if (!ClipPixels(bbox, clip, ax0, ay0, aw, ah)) return false;
    const int nSub = subSamples < 1 ? 1 : (subSamples > 32 ? 32 : subSamples);

    out.x0 = ax0;
    out.y0 = ay0;
    out.w = aw;
    out.h = ah;
    out.v.assign(static_cast<size_t>(aw) * static_cast<size_t>(ah), 0);
    uint8_t* base = out.v.data();
    const Rect outRect(static_cast<float>(ax0), static_cast<float>(ay0),
                       static_cast<float>(aw), static_cast<float>(ah));
    const int ax1 = ax0 + aw, ay1 = ay0 + ah;
    std::vector<Edge>& edges = edgesScratch();

    for (const std::vector<Point>& piece : pieces) {
        if (piece.size() < 3) continue;
        const Rect pr = Rect::intersect(RingBounds(piece), outRect);
        if (pr.empty()) continue;
        int px0 = static_cast<int>(std::floor(pr.x));
        int py0 = static_cast<int>(std::floor(pr.y));
        int px1 = static_cast<int>(std::ceil(pr.right()));
        int py1 = static_cast<int>(std::ceil(pr.bottom()));
        if (px0 < ax0) px0 = ax0;
        if (py0 < ay0) py0 = ay0;
        if (px1 > ax1) px1 = ax1;
        if (py1 > ay1) py1 = ay1;
        if (px1 <= px0 || py1 <= py0) continue;
        BuildEdgesRing(piece, edges);
        if (edges.empty()) continue;
        const int ox = px0 - ax0, oy = py0 - ay0;
        RasterEdges(edges, FillRule::NonZero, px0, py0, px1 - px0, py1 - py0, nSub, false,
                    [base, aw, ox, oy](int yy, int tLo, int tHi, const float* cov) {
                        uint8_t* row = base + static_cast<size_t>(oy + yy) *
                                                  static_cast<size_t>(aw) +
                                       static_cast<size_t>(ox);
                        for (int xi = tLo; xi <= tHi; ++xi) {
                            const float c = cov[xi];
                            if (c <= 0.0f) continue;
                            const uint8_t q =
                                (c >= 1.0f) ? 255
                                            : static_cast<uint8_t>(c * 255.0f + 0.5f);
                            if (q > row[xi]) row[xi] = q;   // 并集 = 取 max，不是相加
                        }
                    });
    }
    return true;
}

bool RasterizeStrokeCoverage(const StrokeCoverageRequest& req, CoverageRect& out) {
    if (!req.valid()) {
        out.x0 = out.y0 = out.w = out.h = 0;
        out.v.clear();
        return false;
    }
    // 交错数组 → Polyline 批（拷贝一次点；描边本来就要展平出块的顶点，这点拷贝不算什么）
    std::vector<Polyline> polys;
    polys.reserve(static_cast<size_t>(req.contours));
    for (int c = 0; c < req.contours; ++c) {
        const uint32_t f = req.first[c];
        const uint32_t n = req.count[c];
        if (n < 2) continue;
        Polyline pl;
        pl.closed = req.closed && req.closed[c] != 0;
        pl.pts.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            const size_t k = static_cast<size_t>(f + i) * 2;
            pl.pts.push_back(Point{req.xy[k], req.xy[k + 1]});
        }
        polys.push_back(std::move(pl));
    }
    return RasterizeStrokeCoverage(polys, req.style, req.clip, req.subSamples, out);
}

// ------------------------------------------------------------
// 圆角模板覆盖率（0.7.0）
//
// 有符号距离的解析式，不经过扫描线：这张模板是"圆角方块"的覆盖率位图，宿主要把它当
// 灰度纹理去 9 宫格拉伸（角片只采样 [0,r] 那一块）。所以这里刻意**不**走 RasterizeCoverage
// —— 那会给出一张"折线圆弧 + 8 条子采样"的覆盖率，和三端现在的模板不是同一张图。
//
// 边界口径：填充 `0.5 - dist`（dist 是到边界的距离，正 = 在外面），于是边界上正好 0.5、
// 往内 0.5px 满覆盖 —— 这就是 1px 宽的抗锯齿过渡。描边把带子整体挪到内侧
// （`min(0.5 - dist, dist + 1.5)`，即 dist ∈ (-1.5, 0.5)），否则环线骑在边界上、靠内的
// 半个像素只有 50%，白色焦点环会画成灰线（binUI 0.16 实测 #858990）。
// ------------------------------------------------------------
bool RasterizeRoundedTileCoverage(int radius, bool stroke, CoverageRect& out) {
    out.x0 = out.y0 = 0;
    out.w = out.h = 0;
    out.v.clear();
    const int r = radius > 0 ? radius : 1;
    const int t = RoundedTileSize(r);
    out.w = t;
    out.h = t;
    out.v.assign(static_cast<size_t>(t) * static_cast<size_t>(t), 0);
    const float half = static_cast<float>(t) / 2.0f;      // 形状半边长
    const float inset = half - static_cast<float>(r);     // 直边段起点距中心
    for (int i = 0; i < t; ++i) {
        for (int j = 0; j < t; ++j) {
            const float fx = std::fabs(static_cast<float>(j) + 0.5f - half) - inset;
            const float fy = std::fabs(static_cast<float>(i) + 0.5f - half) - inset;
            const float dx = fx > 0.0f ? fx : 0.0f;
            const float dy = fy > 0.0f ? fy : 0.0f;
            const float dist = std::sqrt(dx * dx + dy * dy) - static_cast<float>(r);
            float cov = stroke ? std::min(0.5f - dist, dist + 1.5f) : 0.5f - dist;
            cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
            out.v[static_cast<size_t>(i) * static_cast<size_t>(t) + static_cast<size_t>(j)] =
                static_cast<uint8_t>(cov * 255.0f + 0.5f);
        }
    }
    return true;
}

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
    RasterPolys(polys, rule, false, cover, kSubSamples, false);
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
    for (const Polyline& piece : devPieces)
        RasterPolys({piece}, FillRule::NonZero, true, cover, kSubSamples, false);
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

    // 预滤波在这里不做：DrawSurface 收到的是 Surface（可能是图层快照），
    // 没有"原图 + 分级"的信息。缩小时的盒式预缩小在 DrawImage 里选源（见那里），
    // 图层合成按定义是 1:1 的，不需要 mip。
    const Surface& srcSurface = image;
    const int sw = srcSurface.width(), sh = srcSurface.height();

    // ---------------- ① 1:1 平移合成 ----------------
    if (!t.mask && fullUv && axisAligned && nearest == false &&
        std::fabs(xf.a - 1.0f) < 1e-4f && std::fabs(xf.d - 1.0f) < 1e-4f &&
        std::fabs(dstLocal.w - static_cast<float>(sw)) < 1e-3f &&
        std::fabs(dstLocal.h - static_cast<float>(sh)) < 1e-3f) {
        const int dx = static_cast<int>(std::lround(devRect.x));
        const int dy = static_cast<int>(std::lround(devRect.y));
        const bool plainTint = tint.r == 255 && tint.g == 255 && tint.b == 255 && tint.a == 255;
        // 混合模式按整次绘制分派一次（不是逐像素 switch）：模板参数让编译器把
        // 五个分支各自编成一条行内循环，与原来写死 SrcOver 的那份逐位相同。
        auto blit = [&](auto modeTag) {
            constexpr BlendMode M = decltype(modeTag)::value;
            for (int py = y0; py < y1; ++py) {
                const int sy = py - dy;
                if (sy < 0 || sy >= sh) continue;
                const uint8_t* s = srcSurface.row(sy);
                uint8_t* d = dst.row(py);
                for (int px = x0; px < x1; ++px) {
                    const int sx = px - dx;
                    if (sx < 0 || sx >= sw) continue;
                    const uint8_t* sp = s + static_cast<size_t>(sx) * 4;
                    uint8_t* dp = d + static_cast<size_t>(px) * 4;
                    Color c(sp[0], sp[1], sp[2], sp[3]);
                    if (!plainTint) {
                        c = Color(Div255(static_cast<uint32_t>(c.r) * tint.r),
                                  Div255(static_cast<uint32_t>(c.g) * tint.g),
                                  Div255(static_cast<uint32_t>(c.b) * tint.b),
                                  Div255(static_cast<uint32_t>(c.a) * tint.a));
                    }
                    BlendOne<M>(dp, c);
                }
            }
        };
        switch (t.blend) {
        case BlendMode::SrcOver: blit(std::integral_constant<BlendMode, BlendMode::SrcOver>{}); break;
        case BlendMode::Replace: blit(std::integral_constant<BlendMode, BlendMode::Replace>{}); break;
        case BlendMode::Erase: blit(std::integral_constant<BlendMode, BlendMode::Erase>{}); break;
        case BlendMode::Keep: blit(std::integral_constant<BlendMode, BlendMode::Keep>{}); break;
        case BlendMode::Additive: blit(std::integral_constant<BlendMode, BlendMode::Additive>{}); break;
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
            if (nearest) {
                // 最近邻：只取一个纹素。取整口径必须与 SampleSurface 一致
                // （round(fx)，不是 floor），否则"放大镜"放大后差半像素。
                const int n = static_cast<int>(fx + 0.5f);
                const int c = n < 0 ? 0 : (n >= sw ? sw - 1 : n);
                ix0[static_cast<size_t>(i)] = c;
                ix1[static_cast<size_t>(i)] = c;
                wx[static_cast<size_t>(i)] = 0.0f;
                continue;
            }
            const int base = static_cast<int>(std::floor(fx));
            ix0[static_cast<size_t>(i)] = base < 0 ? 0 : (base >= sw ? sw - 1 : base);
            const int next = base + 1;
            ix1[static_cast<size_t>(i)] = next < 0 ? 0 : (next >= sw ? sw - 1 : next);
            wx[static_cast<size_t>(i)] = fx - static_cast<float>(base);
        }
        const uint8_t* srcRows = srcSurface.row(0);
        const int stride = srcSurface.stride();
        const bool plainTint = tint.r == 255 && tint.g == 255 && tint.b == 255 && tint.a == 255;
        const Rect inner(x0 + 1.0f, y0 + 1.0f, static_cast<float>(spanW) - 2.0f,
                         static_cast<float>(y1 - y0) - 2.0f);
        // 同 ①：混合模式按整次绘制分派一次，模板参数把这五个分支各编成一条行内循环
        auto scan = [&](auto modeTag) {
            constexpr BlendMode M = decltype(modeTag)::value;
            for (int py = y0; py < y1; ++py) {
                const float ly = (static_cast<float>(py) + 0.5f - xf.f) * invD;
                const float v = srcUv.y + ((ly - dstLocal.y) / dstLocal.h) * srcUv.h;
                const float cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                const float fy = cv * static_cast<float>(sh - 1);
                int by = static_cast<int>(std::floor(fy));
                int ry0 = by < 0 ? 0 : (by >= sh ? sh - 1 : by);
                int ry1 = (by + 1) < 0 ? 0 : ((by + 1) >= sh ? sh - 1 : (by + 1));
                float tyy = fy - static_cast<float>(by);
                if (nearest) {   // 同横向：纵向也只取一个纹素（口径 = SampleSurface）
                    const int ny = static_cast<int>(fy + 0.5f);
                    ry0 = ny < 0 ? 0 : (ny >= sh ? sh - 1 : ny);
                    ry1 = ry0;
                    tyy = 0.0f;
                    by = ry0;
                }
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
                    // a==0 要不要落笔由 BlendOne 判（Replace/Keep 必须写透明）
                    c = c * (cov > 1.0f ? 1.0f : cov);
                    BlendOne<M>(drow + static_cast<size_t>(px) * 4, c);
                }
            }
        };
        switch (t.blend) {
        case BlendMode::SrcOver: scan(std::integral_constant<BlendMode, BlendMode::SrcOver>{}); break;
        case BlendMode::Replace: scan(std::integral_constant<BlendMode, BlendMode::Replace>{}); break;
        case BlendMode::Erase: scan(std::integral_constant<BlendMode, BlendMode::Erase>{}); break;
        case BlendMode::Keep: scan(std::integral_constant<BlendMode, BlendMode::Keep>{}); break;
        case BlendMode::Additive: scan(std::integral_constant<BlendMode, BlendMode::Additive>{}); break;
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

            Color c = SampleSurface(srcSurface, u, v, nearest);
            c = Color(Div255(static_cast<uint32_t>(c.r) * tint.r),
                      Div255(static_cast<uint32_t>(c.g) * tint.g),
                      Div255(static_cast<uint32_t>(c.b) * tint.b),
                      Div255(static_cast<uint32_t>(c.a) * tint.a));
            // 同 ①②：a==0 要不要落笔交给混合模式（BlendPixel 自己会判）
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
    if (image.empty()) return;
    // 缩小预滤波（mip 的轻量版）：算"每个设备像素要盖几个源纹素"，>1 就换用盒式
    // 预缩小过的副本。nearest 不参与 —— 最近邻的语义就是"给我那一个纹素"。
    // 这一步放在这里（而不是 DrawSurface）是因为只有 Image 持有分级副本；
    // DrawSurface 的入参是 Surface（图层快照），图层合成按定义是 1:1 的。
    const float srcTexelsX = srcUv.w * static_cast<float>(image.width());
    const float srcTexelsY = srcUv.h * static_cast<float>(image.height());
    float texelsPerPixel = 1.0f;
    if (std::fabs(xf.b) < 1e-6f && std::fabs(xf.c) < 1e-6f) {
        const float devW = std::fabs(dstLocal.w * xf.a);
        const float devH = std::fabs(dstLocal.h * xf.d);
        texelsPerPixel = std::max(srcTexelsX / std::max(1e-6f, devW),
                                  srcTexelsY / std::max(1e-6f, devH));
    } else {
        const float devArea = std::fabs(xf.det()) * dstLocal.w * dstLocal.h;
        texelsPerPixel = std::sqrt((srcTexelsX * srcTexelsY) / std::max(1e-6f, devArea));
    }
    const Surface& src = nearest ? image.surface() : image.sampleSurface(texelsPerPixel);
    DrawSurface(t, src, dstLocal, srcUv, tint, nearest, xf);
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

// ------------------------------------------------------------
// 覆盖率遮罩的模糊 / 合成（0.8.0：投影走这条）
//
// 单通道版的前缀和盒式：与 BoxBlurRows/Cols 逐行对应（同样的 padded 填充 + 前缀和 +
// (sum + win/2)/win），只是每像素 1 个通道而不是 4 个。刻意不做"两次盒式"：
// 现有 Blur 就是"行一遍 + 列一遍"= 一个方形核，投影的观感与它一致才好换。
// ------------------------------------------------------------
void BlurMask(uint8_t* mask, int w, int h, float radius) {
    if (!mask || w <= 0 || h <= 0 || radius <= 0.0f) return;
    const int rad = static_cast<int>(std::lround(radius));
    if (rad < 1) return;
    const int win = rad * 2 + 1;

    // 行方向
    {
        const int padded = w + rad * 2;
        std::vector<uint8_t> pad(static_cast<size_t>(padded));
        std::vector<uint32_t> pre(static_cast<size_t>(padded + 1));
        for (int y = 0; y < h; ++y) {
            uint8_t* row = mask + static_cast<size_t>(y) * static_cast<size_t>(w);
            for (int i = 0; i < padded; ++i) {
                int x = i - rad;
                x = x < 0 ? 0 : (x >= w ? w - 1 : x);
                pad[static_cast<size_t>(i)] = row[x];
            }
            pre[0] = 0;
            for (int i = 0; i < padded; ++i)
                pre[static_cast<size_t>(i + 1)] = pre[static_cast<size_t>(i)] + pad[static_cast<size_t>(i)];
            for (int x = 0; x < w; ++x) {
                const uint32_t sum = pre[static_cast<size_t>(x + win)] - pre[static_cast<size_t>(x)];
                row[x] = static_cast<uint8_t>((sum + static_cast<uint32_t>(win / 2)) /
                                              static_cast<uint32_t>(win));
            }
        }
    }
    // 列方向
    {
        const int padded = h + rad * 2;
        std::vector<uint8_t> pad(static_cast<size_t>(padded));
        std::vector<uint32_t> pre(static_cast<size_t>(padded + 1));
        for (int x = 0; x < w; ++x) {
            for (int i = 0; i < padded; ++i) {
                int y = i - rad;
                y = y < 0 ? 0 : (y >= h ? h - 1 : y);
                pad[static_cast<size_t>(i)] =
                    mask[static_cast<size_t>(y) * static_cast<size_t>(w) + x];
            }
            pre[0] = 0;
            for (int i = 0; i < padded; ++i)
                pre[static_cast<size_t>(i + 1)] = pre[static_cast<size_t>(i)] + pad[static_cast<size_t>(i)];
            for (int y = 0; y < h; ++y) {
                const uint32_t sum = pre[static_cast<size_t>(y + win)] - pre[static_cast<size_t>(y)];
                mask[static_cast<size_t>(y) * static_cast<size_t>(w) + x] =
                    static_cast<uint8_t>((sum + static_cast<uint32_t>(win / 2)) /
                                         static_cast<uint32_t>(win));
            }
        }
    }
}

void BlendCoverageMask(const RasterTarget& t, const uint8_t* mask, int mw, int mh, int dstX,
                       int dstY, const Color& color) {
    if (!t.valid() || !mask || mw <= 0 || mh <= 0 || color.a == 0) return;
    Surface& dst = *t.surface;
    const Rect area = Rect::intersect(t.clip, dst.bounds());
    if (area.empty()) return;

    const int x0 = std::max(dstX, static_cast<int>(std::floor(area.x)));
    const int y0 = std::max(dstY, static_cast<int>(std::floor(area.y)));
    const int x1 = std::min(dstX + mw, static_cast<int>(std::ceil(area.right())));
    const int y1 = std::min(dstY + mh, static_cast<int>(std::ceil(area.bottom())));
    if (x1 <= x0 || y1 <= y0) return;
    const int sw = dst.width();
    for (int y = y0; y < y1; ++y) {
        const uint8_t* mrow = mask + static_cast<size_t>(y - dstY) * static_cast<size_t>(mw);
        uint8_t* drow = dst.row(y);
        for (int x = x0; x < x1; ++x) {
            float k = static_cast<float>(mrow[x - dstX]) * (1.0f / 255.0f);
            if (k <= 0.0f) continue;
            if (t.mask) {
                k *= t.mask[static_cast<size_t>(y) * static_cast<size_t>(sw) +
                            static_cast<size_t>(x)];
                if (k <= 0.0f) continue;
            }
            const Color src = color * k;   // 预乘空间逐通道乘（含 alpha）
            if (src.a == 0) continue;
            uint8_t* p = drow + static_cast<size_t>(x) * 4;
            const Color out = BlendPixel(Color(p[0], p[1], p[2], p[3]), src, t.blend);
            p[0] = out.r;
            p[1] = out.g;
            p[2] = out.b;
            p[3] = out.a;
        }
    }
}

} // namespace bin2d
