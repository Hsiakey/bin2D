#pragma once
// ============================================================
// bin2d/raster.h —— 光栅核心（扫描线 + 覆盖率 AA + 合成）
//
// 算法（一句话）：**先算覆盖率，再乘颜料**。
//   · 竖直方向：每个像素行切 8 条子扫描线（`kSubSamples`），每条子扫描线
//     中心取 y，与多边形求交得到若干区间；
//   · 水平方向：区间端点的**小数部分**直接当覆盖率（不做水平子采样 ——
//     水平方向是解析求交，本来就没有量化误差）；
//   · 该行覆盖率 = 8 条子扫描线的水平覆盖率之和 / 8，天然落在 [0,1]；
//     累积方式是实现细节（0.4.2 起是"端像素直加 + 整像素段差分、行尾一次前缀和"，
//     内部像素恰好等于 8 × 1/8 = 1.0，与逐条相加逐位相同）；
//   · 最后 `paint(像素中心) × 覆盖率` 再与目标合成。
//
// 误差界：竖直方向 1/8 像素量化（`kSubSamples = 8` 时，边缘位置误差 ≤ 1/16 px，
// 覆盖率误差 ≤ 1/8）；水平方向只有浮点误差（≈1e-6 px）。整像素对齐的矩形
// 因此**精确命中**（覆盖率恰为 1，像素值逐像素可预测），这也是测试里能写
// "逐像素精确值"的前提。斜边会出现最多 1/8 像素的位置量化 —— 表现在像素值上
// 是相邻两行覆盖率差最多 12.5%，肉眼看就是平滑过渡（没有锯齿也没有跳变）。
//
// 为什么不用"超采样后模糊"式的 AA：那是先写色再滤波，颜色会从透明区渗到
// 不透明区（深色底上画浅色形状，边缘会发灰）。覆盖率 AA 的每个像素只混一次，
// 且混的是**同一个颜料在像素中心的值**，不会串色。
//
// 描边为什么走"多边形并集"而不是逐段画：逐段各自混合的话，相邻段在接缝处
// 覆盖率各算一半、混合两次 → 接缝颜色比两侧深一条（老 bug）。这里先把每一段
// 的四边形/圆角/端帽都叠进同一张覆盖率缓冲（取 max 不取和），最后统一混一次。
// ============================================================

#include <cstdint>
#include <vector>

#include "bin2d/core.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/surface.h"
#include "bin2d/transform.h"

namespace bin2d {

// ------------------------------------------------------------
// 混合模式（与 bingpu.h 的 BgpuBlend 一一对应；这里是 CPU 侧的定义）
// ------------------------------------------------------------
enum class BlendMode : uint8_t {
    SrcOver = 0,   // src + dst*(1-srcA)     普通半透明
    Replace = 1,   // src                    覆盖（不跟底色混合）
    Erase = 2,     // dst*(1-srcA)           擦除（裁剪补集）
    Keep = 3,      // dst*srcA               保留（遮罩）
    Additive = 4,  // src + dst              叠加（饱和到 255）
};

// 单像素合成。src/dst 都是预乘；src 的 alpha 已经乘过覆盖率。
inline Color BlendPixel(const Color& dst, const Color& src, BlendMode mode) {
    switch (mode) {
    case BlendMode::Replace:
        return src;
    case BlendMode::Additive: {
        auto add = [](uint8_t a, uint8_t b) {
            const unsigned v = static_cast<unsigned>(a) + static_cast<unsigned>(b);
            return static_cast<uint8_t>(v > 255u ? 255u : v);
        };
        return Color(add(dst.r, src.r), add(dst.g, src.g), add(dst.b, src.b),
                     add(dst.a, src.a));
    }
    case BlendMode::Erase: {
        const uint32_t inv = 255u - src.a;
        auto mul = [inv](uint8_t v) {
            return static_cast<uint8_t>((static_cast<uint32_t>(v) * inv + 127u) / 255u);
        };
        return Color(mul(dst.r), mul(dst.g), mul(dst.b), mul(dst.a));
    }
    case BlendMode::Keep: {
        auto mul = [&src](uint8_t v) {
            return static_cast<uint8_t>((static_cast<uint32_t>(v) * src.a + 127u) / 255u);
        };
        return Color(mul(dst.r), mul(dst.g), mul(dst.b), mul(dst.a));
    }
    case BlendMode::SrcOver:
    default: {
        Color out = dst;
        blendOver(out, src);
        return out;
    }
    }
}

// 源 alpha == 0 时还要不要落笔：Replace 写透明（把目标清成 0）、Keep 乘 0（同样清 0），
// 另外三种模式下 src.a==0 真的什么都不改。
// 这条判据原本只写在 BlendPixel 的分支里，而**落像素的快路径**（整行搬运、查表双线性）
// 各自写死了"src.a==0 就 continue"—— 于是"组混合模式 = Replace/Keep"在 1:1 图层合成
// 上被悄悄跳过（0.3.0 修）。现在只有这一个判据，所有落像素路径都问它。
inline bool ZeroAlphaMatters(BlendMode mode) {
    return mode == BlendMode::Replace || mode == BlendMode::Keep;
}

// ------------------------------------------------------------
// 描边参数
// ------------------------------------------------------------
struct StrokeStyle {
    float width = 1.0f;
    bool roundCap = false;     // 圆端帽；与 squareCap 互斥（square 优先）
    bool squareCap = false;    // 方端帽：两端各外延半个线宽
    bool roundJoin = true;     // 圆角连接；false = 尖角（miter，超限退化成 bevel）
    float miterLimit = 4.0f;   // SVG 默认 4；只有 roundJoin=false 时有意义
};

// ------------------------------------------------------------
// 绘制目标：表面 + 裁剪矩形 + 可选覆盖率遮罩 + 混合模式
//
// mask 是**设备空间**的覆盖率（长度 = surface->width() * surface->height()，
// 下标 y*w+x），值为 nullptr 表示不裁；路径裁剪（clipPath）就是往这里塞一张
// 覆盖率图 —— 这样裁剪与形状用同一套覆盖率语义，边缘不会出现"形状有 AA、
// 裁剪是硬边"的断层。
// ------------------------------------------------------------
struct RasterTarget {
    Surface* surface = nullptr;
    Rect clip;                     // 设备空间轴对齐裁剪（推荐 = surface->bounds()）
    const float* mask = nullptr;   // 可选覆盖率遮罩
    BlendMode blend = BlendMode::SrcOver;

    bool valid() const { return surface && !surface->empty(); }
};

// ------------------------------------------------------------
// 光栅 API（全部在**设备空间**落像素；几何/颜料在**局部空间**给）
// ------------------------------------------------------------
// 填充路径。tolerancePx 是展平容差（设备像素），内部按变换缩放折成用户单位。
void FillPath(const RasterTarget& t, const Path& path, FillRule rule,
              const Paint& paint, const Transform& xf, float tolerancePx = 0.25f);

// 描边路径：四边形 + 圆角/尖角连接 + 端帽，叠一张覆盖率再统一混合。
void StrokePath(const RasterTarget& t, const Path& path, const Paint& paint,
                const StrokeStyle& style, const Transform& xf,
                float tolerancePx = 0.25f);

// 圆角矩形填充（内部就是 addRoundedRect + FillPath，AA 口径与路径一致）
void FillRoundedRect(const RasterTarget& t, const Rect& r, float radius,
                     const Paint& paint, const Transform& xf);

// 轴对齐矩形填充（整像素边界时覆盖率精确 = 1，测试拿它当基准）
void FillRect(const RasterTarget& t, const Rect& r, const Paint& paint,
              const Transform& xf);

// 线性渐变填充一个矩形（= FillRect + Paint::Linear；保留这个名字是因为
// "只看渐变是否对"的测试/调用点用它更直白）
void FillLinearGradient(const RasterTarget& t, const Rect& r, const Paint& gradient,
                        const Transform& xf);

// 画位图：dstLocal 是图片铺到的矩形（局部坐标），srcUv 是取源图的 UV 子矩形。
// nearest=true 用最近邻（像素图/放大镜），否则双线性。
// 变换是旋转/斜切时按 4×4 超采样算边缘覆盖率（轴对齐时用矩形解析覆盖率，更准）。
void DrawImage(const RasterTarget& t, const Image& image, const Rect& dstLocal,
               const Rect& srcUv, const Color& tint, bool nearest,
               const Transform& xf);

// 九宫格拉伸：把图按 (l,t,r,b) 四边切成 9 块，四角不缩放、四边单向拉伸、中心双向。
// 圆角卡片/气泡/滚动条都靠它 —— 同一个人写的三次九宫格一定会有一处坐标错。
void DrawNineSlice(const RasterTarget& t, const Image& image, const Rect& dstLocal,
                   float insetL, float insetT, float insetR, float insetB,
                   const Color& tint, bool nearest, const Transform& xf);

// 直接画一张 Surface（图层合成 / 投影 / 蒙版预览用；语义同 DrawImage）
void DrawSurface(const RasterTarget& t, const Surface& src, const Rect& dstLocal,
                 const Rect& srcUv, const Color& tint, bool nearest,
                 const Transform& xf);

// 盒式模糊（两次可分离，近似高斯）。radius 是像素半径；0 或负数不动。
// 只模糊当前像素矩形内、且按 srcAlpha 归一化（预乘空间直接模糊即可）。
void Blur(Surface& s, float radius);

// ------------------------------------------------------------
// 覆盖率遮罩的模糊与合成（0.8.0：投影走这条，不再"画彩色图层再模糊"）
//
// 为什么单开一条：投影只需要 alpha —— 把形状栅成 1 字节/像素的覆盖率、模糊它、
// 最后乘上阴影色合成，内存是原来（RGBA8 图层）的 1/4，且模糊的对象从
// "颜色×alpha"变成"alpha"，语义更直接（模糊核是线性的，两种顺序在实数上等价，
// 差的是两次 8 位取整的位置，实测 ≤1 级，见 tests/test_scene 的投影断言）。
//
// BlurMask 的口径与 Blur 完全相同（同一套前缀和盒式、同一 rad = lround(radius)、
// 同样的"边缘复制"边界），所以两者的模糊半径不会一个算 3 一个算 4。
// ------------------------------------------------------------
void BlurMask(uint8_t* mask, int w, int h, float radius);

// 把 mask（w×h，0..255 覆盖率）当成 alpha 乘到 color 上，按 t 的裁剪 / 遮罩 / 混合模式
// 合成到目标。dstX/dstY 是 mask 左上角在设备空间的位置（整数像素）。
void BlendCoverageMask(const RasterTarget& t, const uint8_t* mask, int w, int h, int dstX,
                       int dstY, const Color& color);

// 子采样数（竖直方向）。标准 8：覆盖率误差 ≤ 1/8 像素，成本约 8 次求交/行。
constexpr int kSubSamples = 8;

// ------------------------------------------------------------
// 覆盖率查询（给"自己有条带 / 四边形管线"的宿主用）
//
// 背景：bin2d 内部的落像素路径是「覆盖率 → 乘颜料 → 混合」（见 FillPath）。但有些宿主
// 不往 CPU 的 Surface 上落像素 —— 它们把覆盖率拼成**条带/四边形**下发给 GPU（binUI
// 就是这样：AA 填充 = 逐行等覆盖率的连续像素合并成一条带）。这类宿主需要的不是像素，
// 而是**同一份覆盖率**，否则两边各写一套扫描线，边缘必然对不上。
//
// 所以这里把覆盖率本身导出：设备空间折线 → 8bit 覆盖率矩形。数值口径与内部完全一致
//（同一个光栅器、同一套水平解析求交），不同的只是出口。
// ------------------------------------------------------------
struct CoverageRect {
    int x0 = 0, y0 = 0, w = 0, h = 0;   // 覆盖 [x0,x0+w) × [y0,y0+h) 这些设备像素
    std::vector<uint8_t> v;             // w*h，行优先；0 = 没盖到，255 = 整像素

    bool empty() const { return w <= 0 || h <= 0; }
    uint8_t at(int x, int y) const {
        return v[static_cast<size_t>(y - y0) * static_cast<size_t>(w) +
                 static_cast<size_t>(x - x0)];
    }
    const uint8_t* row(int y) const {
        return v.data() + static_cast<size_t>(y - y0) * static_cast<size_t>(w);
    }
};

// 覆盖率风格：
//   Fractional —— 边缘像素按重叠比例（0..255 连续），AA 用这个；
//   HardEdge   —— 每个跨度的端点先四舍五入到整像素再累加，覆盖率只有 0 / 255
//                 （SVG 的 shape-rendering="crispEdges" / optimizeSpeed 就是这个语义）。
// 它必须落在光栅器里而不是"调用方自己把 0..255 二值化"：二值化会丢掉"亚像素宽的细条
// 仍然占一整像素"这条语义（老的 binUI 硬边填充就是按 round(端点) 铺满整像素的）。
enum class CoverageStyle : uint8_t { Fractional = 0, HardEdge = 1 };

// 覆盖率请求。几何用**交错 float + 折线表**（和 binUI 的受控内存布局一致，免一次拷贝）：
//   xy[2i], xy[2i+1] 是两个 float；第 c 条折线 = xy[first[c]*2 .. (first[c]+count[c])*2]，
//   折线首尾自动闭合（填充语义）。
// clip 是设备像素裁剪矩形；subSamples 是竖直子采样数（默认 kSubSamples）。
//   传 1 = 硬边（覆盖率只有 0 / 1 两档，等价于"像素中心在形状内"）。
struct CoverageRequest {
    const float* xy = nullptr;
    const uint32_t* first = nullptr;
    const uint32_t* count = nullptr;
    int contours = 0;
    FillRule rule = FillRule::NonZero;
    CoverageStyle style = CoverageStyle::Fractional;
    Rect clip;
    int subSamples = kSubSamples;

    bool valid() const { return xy && first && count && contours > 0; }
};

// 折线批量版本（bin2d 自己的调用方 / 测试用）
bool RasterizeCoverage(const std::vector<Polyline>& polys, FillRule rule, const Rect& clip,
                       int subSamples, CoverageRect& out,
                       CoverageStyle style = CoverageStyle::Fractional);

// 交错数组版本（宿主免拷贝）
bool RasterizeCoverage(const CoverageRequest& req, CoverageRect& out);

// ------------------------------------------------------------
// 描边覆盖率（0.6.0）：给同一批宿主用 —— 它们要描边的**覆盖率**，不要像素。
//
// 语义与 `StrokePath` **完全一致**（同一份 BuildStrokePolys：每段一个四边形、每个拐点
// 一个连接块、开放两端加端帽；然后**逐块栅格化取 max** 合成并集）。这一点是刻意的：
// 描边最容易漂移的地方不是"四边形怎么摆"，而是"重叠处怎么合"——
//   · 逐段各自混合（老 binUI）= 接缝混合两次，半透明描边的每个接缝都深一条；
//   · 把多块丢进同一个边表用 nonzero = 反向的四边形互相抵消，拐点会开洞；
//   · 取 max = 接缝处覆盖率取大者，混一次，这正是并集的语义。
// 所以这里不做"聪明的单多边形轮廓"优化：宁可多几次小栅格化，也要和 StrokePath 同源。
//
// width 是**目标空间**（设备像素）的线宽，不做变换缩放 —— 宿主若有变换，自己先折进去。
// clip 是设备像素裁剪矩形；subSamples 是竖直子采样数（默认 kSubSamples）。
// 覆盖率风格固定 Fractional：硬边描边没有意义（本来就是细长小面，四舍五入等于丢形）。
// ------------------------------------------------------------
struct StrokeCoverageRequest {
    const float* xy = nullptr;          // x,y 交错（与 CoverageRequest 同一布局）
    const uint32_t* first = nullptr;    // 子路径起始点下标
    const uint32_t* count = nullptr;    // 子路径点数（≥2）
    const uint8_t* closed = nullptr;    // 每条子路径是否闭合（可为 nullptr = 全开放）
    int contours = 0;
    StrokeStyle style;                  // 线宽 / 端帽 / 连接 / miter-limit
    Rect clip;
    int subSamples = kSubSamples;

    bool valid() const {
        return xy && first && count && contours > 0 && style.width > 0.0f;
    }
};

bool RasterizeStrokeCoverage(const StrokeCoverageRequest& req, CoverageRect& out);

// 折线批量版本（bin2d 自己的调用方 / 测试用）。closed 由 Polyline 自带。
bool RasterizeStrokeCoverage(const std::vector<Polyline>& polys, const StrokeStyle& style,
                             const Rect& clip, int subSamples, CoverageRect& out);

// ------------------------------------------------------------
// 圆角模板覆盖率（0.7.0）：九宫格角片用的那张"圆角方块"覆盖率位图。
//
// 为什么这个也归 bin2d 管：宿主（binUI）的三个 GPU 后端各自写了一份一模一样的解析式
// **有符号距离**覆盖率（30 行 × 3 份），再把结果当灰度纹理去 9 宫格拉伸 —— 三份代码只要
// 有一份改了（边长、内缩、边界半像素的取舍），三端圆角就差一圈像素。这跟 P2 的填充/描边
// 是同一类漂移，所以同样收口到这里。
//
// 尺寸：边长 = max(8, 2r+4)。形状铺满整张模板 [0,t]，于是圆弧的包围盒正好是 [0,r]，
// 角片边长与弧长严格对应 —— 半径取到短边一半时四角才能无缝拼成圆/胶囊。
// 覆盖率口径（与 binUI 三端 0.16 起的那份逐位相同）：
//   · stroke=false：内部，边界处线性 1px 渐变（抗锯齿）；
//   · stroke=true ：贴着**外侧边界往里** 1px 的实心带（骑在边界上会让白色焦点环发灰）。
// 这是**有符号距离**的解析式，不是多边形光栅化 —— 别拿 RasterizeCoverage 拼圆角路径来顶，
// 那会得到另一张模板（弧是折线、边缘是 8 条子采样），三端像素就不再相同了。
// ------------------------------------------------------------
constexpr int kRoundedTileMin = 8;

inline int RoundedTileSize(int radius) {
    const int r = radius > 0 ? radius : 1;
    const int t = r * 2 + 4;
    return t < kRoundedTileMin ? kRoundedTileMin : t;
}

// 导出一张 t×t 的覆盖率（t = RoundedTileSize(radius)；out.x0/y0 = 0，out.v 行优先）。
bool RasterizeRoundedTileCoverage(int radius, bool stroke, CoverageRect& out);

} // namespace bin2d
