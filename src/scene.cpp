// ============================================================
// src/scene.cpp —— 场景图里唯一需要"多步"的部分：投影
//
// 为什么投影不在 raster 里做成"带模糊的填充"：
//   投影 = 形状 → 模糊 → 当半透明位图贴回来，是**三步**而不是"填充的一种参数"。
//   放进 raster 会让每个填充入口都多一个分支，而真正需要它的只有 Scene 的节点。
//
// 为什么自己开临时位图而不是用 Canvas 的图层栈：
//   图层栈的语义是"画完整体乘 alpha 合成"，中间不允许插手（不能在合成前模糊）。
//   要模糊就得在内容画完之后、合成之前拿到那张位图 —— 那就是自己开着。
// ============================================================

#include "bin2d/scene.h"

#include <algorithm>
#include <cmath>

namespace bin2d {

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

    const int w = static_cast<int>(std::ceil(dev.w));
    const int h = static_cast<int>(std::ceil(dev.h));
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;

    Surface layer(w, h);
    Canvas lc(layer);
    lc.setTransform(Transform::translate(-dev.x, -dev.y) * xf);
    lc.fillPath(path, sh.color, FillRule::NonZero);
    if (blur > 0.0f) Blur(layer, blur);

    // 贴回画布：先切到设备坐标（画布的当前变换不再参与），再按设备矩形画
    canvas.save();
    canvas.setTransform(Transform::identity());
    canvas.drawSurface(layer, dev, Rect(0, 0, 1, 1), Color::white(),
                       ImageFilter::Linear);
    canvas.restore();
}

} // namespace bin2d
