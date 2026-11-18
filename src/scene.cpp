// ============================================================
// src/scene.cpp —— 场景图里唯一需要"多步"的部分：投影
//
// 为什么投影不在 raster 里做成"带模糊的填充"：
//   投影 = 形状 → 模糊 → 当半透明位图贴回来，是**三步**而不是"填充的一种参数"。
//   放进 raster 会让每个填充入口都多一个分支，而真正需要它的只有 Scene 的节点。
//
// 为什么投影走"alpha 罩"而不是"画一张彩色图层再模糊"（0.8.0 改）：
//   投影只有 alpha 有意义 —— 阴影色是常量，模糊核是线性的，所以
//   blur(色 × 覆盖率) 与 色 × blur(覆盖率) 在实数上等价。后者只要 1 字节/像素，
//   前者的离屏位图是 4 字节/像素（r=12 的卡片投影约 2.6 MB → 0.65 MB），
//   而且不用把颜色管线（预乘、tint、取样器）绕进来。
//   代价是模糊后做一次乘法，两次 8 位取整的位置变了 —— 与旧路径逐像素实测差
//   ≤1 级（取证：build-mingw/tmp/shadow_probe + 测试里的投影断言），画质不变。
//
// 为什么自己开临时缓冲而不是用 Canvas 的图层栈：
//   图层栈的语义是"画完整体乘 alpha 合成"，中间不允许插手（不能在合成前模糊）。
//   要模糊就得在内容画完之后、合成之前拿到那张位图 —— 那就是自己开着。
// ============================================================

#include "bin2d/scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace bin2d {
namespace {

// 与 raster.cpp 的 FlattenToDevice 同一口径（容差按设备像素折成用户单位）。
// 这里不走 raster 的 FillPath，因为投影要拿到**覆盖率本身**去模糊（见文件头），
// 而不是让它直接乘颜料落像素。
std::vector<Polyline> FlattenShadowPath(const Path& path, const Transform& xf,
                                       float tolerancePx) {
    const float scale = std::sqrt(std::fabs(xf.det()));
    const float tol = (scale > 1e-6f) ? tolerancePx / scale : tolerancePx;
    std::vector<Polyline> polys = path.flatten(tol);
    for (Polyline& pl : polys)
        for (Point& p : pl.pts) p = xf.map(p);
    return polys;
}

} // namespace

void Shape::DrawOwnShadow(Canvas& canvas) const {
    const Shadow& sh = shadow;
    if (!sh.visible()) return;

    const Transform xf = canvas.transform();
    // 偏移在局部空间给，随变换缩放；模糊半径是像素，不随缩放
    const Point off = Transform(xf.a, xf.b, xf.c, xf.d, 0.0f, 0.0f).map(sh.dx, sh.dy);
    const float blur = sh.blur;
    Rect dev = xf.mapBounds(path.bounds());
    dev = Rect(dev.x + off.x - blur, dev.y + off.y - blur,
               dev.w + blur * 2.0f, dev.h + blur * 2.0f).snappedOut();
    if (dev.empty()) return;

    const int devX = static_cast<int>(dev.x);
    const int devY = static_cast<int>(dev.y);
    const int w = static_cast<int>(std::ceil(dev.w));
    const int h = static_cast<int>(std::ceil(dev.h));
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;

    // 形状按"设备空间 + 投影偏移"折线化；缓冲是整个 dev（比形状大一圈），
    // 模糊要往外扩散，罩的矩形必须留出这圈余量。
    const Transform shadowXf(xf.a, xf.b, xf.c, xf.d, xf.e + off.x, xf.f + off.y);
    const std::vector<Polyline> polys = FlattenShadowPath(path, shadowXf, 0.25f);
    if (polys.empty()) return;

    CoverageRect cov;
    if (!RasterizeCoverage(polys, FillRule::NonZero, dev, kSubSamples, cov)) return;
    if (cov.empty()) return;

    std::vector<uint8_t> mask(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    const int ox = cov.x0 - devX, oy = cov.y0 - devY;
    for (int y = 0; y < cov.h; ++y)
        std::memcpy(mask.data() + static_cast<size_t>(oy + y) * static_cast<size_t>(w) + ox,
                    cov.row(cov.y0 + y), static_cast<size_t>(cov.w));

    if (blur > 0.0f) BlurMask(mask.data(), w, h, blur);
    canvas.blendCoverageMask(mask.data(), w, h, devX, devY, sh.color);
}

} // namespace bin2d
