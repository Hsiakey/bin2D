// ============================================================
// src/canvas.cpp —— 即时模式画布的状态机（变换 / 裁剪 / 图层）
//
// 状态只有一份 stack_，save/restore 就是压栈/出栈**整份状态**（含裁剪遮罩）。
// 为什么不留"引用式状态"（只存变了哪些字段）：裁剪遮罩是 std::vector，
// 复制它才是最贵的；但绘制里 push/pop 的频率远低于绘制调用，而"整份复制"
// 换来的是 save/restore 永远正确 —— 局部撤销那种优化在这层没有意义。
//
// 图层的坐标系：入层时把当前绘制的**设备原点**记下来（originX/Y），层内的
// 所有落像素坐标都减掉它。这样层内层外的几何代码完全一样，不需要"图层专用的
// 变换"，也就不会出现"层内坐标漂了半个像素"这类经典 bug。
// ============================================================

#include "bin2d/canvas.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace bin2d {

Canvas::Canvas(Surface& target) : target_(&target), baseTarget_(&target) {
    State s;
    s.clipRect = target.bounds();
    // 遮罩惰性分配：没有路径裁剪时不要 1MB 的 memset（每次 save/pushLayer 都会付）
    s.hasMask = false;
    stack_.push_back(std::move(s));
}

Canvas::Canvas(Surface& target, const Rect& clip) : Canvas(target) {
    stack_.back().clipRect = Rect::intersect(target.bounds(), clip);
}

void Canvas::save() {
    State copy = state();          // 遮罩会跟着复制：只在真的用了路径裁剪时才贵
    stack_.push_back(std::move(copy));
}

void Canvas::restore() {
    if (stack_.size() <= 1) return;   // 栈底不能弹（弹空了 state() 就没状态了）
    stack_.pop_back();
}

void Canvas::translate(float tx, float ty) { concat(Transform::translate(tx, ty)); }
void Canvas::scale(float sx, float sy) { concat(Transform::scale(sx, sy)); }
void Canvas::rotate(float radians) { concat(Transform::rotate(radians)); }
void Canvas::rotateDeg(float degrees) { concat(Transform::rotateDeg(degrees)); }
void Canvas::concat(const Transform& xf) { state().xf = state().xf * xf; }
void Canvas::setTransform(const Transform& xf) { state().xf = xf; }

void Canvas::clipRect(const Rect& r) {
    // 局部矩形 → 设备空间必须走 deviceTransform()（= xf 再减掉层原点），
    // 而不是裸 state().xf：图层里的落像素坐标是 xf.apply(x) - origin，裁剪矩形
    // 若按 xf 映射，就会与 clipRect 自身所在的空间差一个 origin ——
    // 表现是"层内第一次 pushClip 把内容裁成 0×0"（整个层空白）。
    // clipPath 一直是按 deviceTransform 算的，这里补齐同一条口径。
    const Rect dev = deviceTransform().mapBounds(r);
    state().clipRect = Rect::intersect(state().clipRect, dev);
}

void Canvas::resetClip() {
    state().clipRect = target_->bounds();
    state().hasMask = false;
    state().mask.clear();
    state().mask.shrink_to_fit();
}

void Canvas::clipPath(const Path& path, FillRule rule) {
    if (path.empty()) {
        state().clipRect = Rect(0, 0, 0, 0);   // 空路径 = 什么都不画（SVG 语义）
        return;
    }
    const Transform dev = deviceTransform();
    const int w = target_->width(), h = target_->height();
    // 先把路径栅成一张与目标同尺寸的覆盖率图，再与已有遮罩相乘。
    // 同尺寸而不是"裁剪到包围盒"：遮罩要跨多次绘制复用，尺寸必须稳定。
    std::vector<float> mask(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
    Surface probe(w, h);   // 只为复用 FillPath 的覆盖率路径；不参与合成
    RasterTarget rt;
    rt.surface = &probe;
    rt.clip = state().clipRect;
    FillPath(rt, path, rule, Paint::solid(Color::white()), dev);
    // FillPath 的结果在 probe 的 alpha 里（白色预乘覆盖），把它读成覆盖率
    for (int y = 0; y < h; ++y) {
        const uint8_t* p = probe.row(y);
        float* d = mask.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x, p += 4) d[x] = static_cast<float>(p[3]) / 255.0f;
    }
    if (state().hasMask) {
        for (size_t i = 0; i < mask.size(); ++i) mask[i] *= state().mask[i];
    }
    state().mask = std::move(mask);
    state().hasMask = true;
}

void Canvas::clear(const Color& c) {
    if (!target_) return;
    const Rect area = Rect::intersect(state().clipRect, target_->bounds());
    if (area.empty()) return;
    if (!state().hasMask) {
        // 无遮罩：走 Surface 的整像素快路径（Replace 语义：连 alpha 一起换）
        target_->fillRect(area, c);
        return;
    }
    // 有裁剪遮罩：按覆盖率把颜色混进去（Replace 在遮罩边缘会留下"半透明的色"，
    // 那不是 clear 想要的效果）
    Surface& dst = *target_;
    const int dw = dst.width();
    const int x0 = static_cast<int>(std::floor(area.x));
    const int y0 = static_cast<int>(std::floor(area.y));
    const int x1 = static_cast<int>(std::ceil(area.right()));
    const int y1 = static_cast<int>(std::ceil(area.bottom()));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float m = state().mask[static_cast<size_t>(y) * static_cast<size_t>(dw) +
                                         static_cast<size_t>(x)];
            if (m <= 0.0f) continue;
            dst.set(x, y, BlendPixel(dst.get(x, y), c * m, BlendMode::SrcOver));
        }
    }
}

RasterTarget Canvas::makeTarget() const {
    RasterTarget rt;
    rt.surface = target_;
    rt.clip = Rect::intersect(state().clipRect, target_->bounds());
    rt.mask = maskData();
    rt.blend = state().blend;
    return rt;
}

void Canvas::fillRect(const Rect& r, const Color& c) { fillRect(r, Paint::solid(c)); }

void Canvas::fillRect(const Rect& r, const Paint& paint) {
    if (!target_ || r.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    FillRect(rt, r, paint, deviceTransform());
}

void Canvas::fillRounded(const Rect& r, float radius, const Paint& paint) {
    if (!target_ || r.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    FillRoundedRect(rt, r, radius, paint, deviceTransform());
}

void Canvas::fillRounded(const Rect& r, float radius, const Color& c) {
    fillRounded(r, radius, Paint::solid(c));
}

void Canvas::fillPath(const Path& path, const Paint& paint, FillRule rule) {
    if (!target_ || path.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    FillPath(rt, path, rule, paint, deviceTransform());
}

void Canvas::fillPath(const Path& path, const Color& c, FillRule rule) {
    fillPath(path, Paint::solid(c), rule);
}

void Canvas::strokePath(const Path& path, const Paint& paint, const StrokeStyle& style) {
    if (!target_ || path.empty() || style.width <= 0.0f) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    StrokePath(rt, path, paint, style, deviceTransform());
}

void Canvas::strokePath(const Path& path, const Color& c, float width) {
    StrokeStyle st;
    st.width = width;
    strokePath(path, Paint::solid(c), st);
}

void Canvas::drawImage(const Image& image, const Rect& dstLocal, const Rect& srcUv,
                       const Color& tint, ImageFilter filter) {
    if (!target_ || image.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    DrawImage(rt, image, dstLocal, srcUv, tint, filter == ImageFilter::Nearest,
              deviceTransform());
}

void Canvas::drawSurface(const Surface& surface, const Rect& dstLocal, const Rect& srcUv,
                         const Color& tint, ImageFilter filter) {
    if (!target_ || surface.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    DrawSurface(rt, surface, dstLocal, srcUv, tint, filter == ImageFilter::Nearest,
                deviceTransform());
}

void Canvas::drawNineSlice(const Image& image, const Rect& dstLocal, float insetL,
                           float insetT, float insetR, float insetB, const Color& tint,
                           ImageFilter filter) {
    if (!target_ || image.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    DrawNineSlice(rt, image, dstLocal, insetL, insetT, insetR, insetB, tint,
                  filter == ImageFilter::Nearest, deviceTransform());
}

void Canvas::drawNineSliceAuto(const Image& image, const Rect& dstLocal, const Color& tint,
                               ImageFilter filter) {
    if (!target_ || image.empty()) return;
    const NineSliceInsets ins = DetectNineSliceInsets(image);
    // 探测不到"可拉伸带"就退回普通拉伸：拿 0 当边界画九宫格等于把图画成 9 个拉伸块，
    // 那比"没有九宫格"更糟（圆角会被拉成斜边）。
    if (!ins.detected) {
        drawImage(image, dstLocal, Rect(0, 0, 1, 1), tint, filter);
        return;
    }
    drawNineSlice(image, dstLocal, ins.l, ins.t, ins.r, ins.b, tint, filter);
}

void Canvas::blendCoverageMask(const uint8_t* mask, int w, int h, int dstX, int dstY,
                               const Color& color) {
    if (!target_ || !mask || w <= 0 || h <= 0) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    BlendCoverageMask(rt, mask, w, h, dstX, dstY, color);
}

// ------------------------------------------------------------
// 图层
// ------------------------------------------------------------
bool Canvas::pushLayer(const Rect& localBounds, float opacity, BlendMode blend) {
    if (!target_ || opacity <= 0.0f) return false;
    const Transform dev = deviceTransform();
    const Rect devRect = dev.mapBounds(localBounds).snappedOut();
    const Rect clipped = Rect::intersect(devRect, state().clipRect);
    if (clipped.empty()) return false;

    auto layerPtr = std::make_unique<Layer>();
    Layer& layer = *layerPtr;
    const int lw = static_cast<int>(std::ceil(clipped.w));
    const int lh = static_cast<int>(std::ceil(clipped.h));
    if (!layer.surface.resize(lw, lh)) return false;
    layer.originX = static_cast<int>(clipped.x);
    layer.originY = static_cast<int>(clipped.y);
    layer.opacity = opacity;
    layer.blend = blend;
    layer.stackMark = stack_.size();   // 出层时截断：层内漏 call restore 也不会串状态
    layers_.push_back(std::move(layerPtr));

    // 目标切到层；层的画布同样从"整块层"开始，裁剪矩形换成层内矩形
    Layer& top = *layers_.back();
    target_ = &top.surface;
    originX_ = top.originX;
    originY_ = top.originY;
    State s;
    s.xf = state().xf;            // 变换不动：几何代码在层内层外同一套
    s.clipRect = target_->bounds();
    s.blend = BlendMode::SrcOver; // 层内默认正常混合（层的合成模式在 pop 时生效）
    // 遮罩惰性分配：没有路径裁剪就不申请（否则每次入层都要 memset 一份整层大小）
    s.hasMask = false;
    stack_.push_back(std::move(s));
    return true;
}

void Canvas::compositeSurface(const Surface& surface, int originX, int originY, float opacity,
                              BlendMode blend) {
    if (!target_ || surface.empty() || opacity <= 0.0f) return;
    RasterTarget rt;
    rt.surface = target_;
    rt.clip = target_->bounds();
    rt.mask = nullptr;              // 层外已经有了自己的裁剪：层已按裁剪尺寸分配
    rt.blend = blend;
    const Rect dstRect(static_cast<float>(originX), static_cast<float>(originY),
                       static_cast<float>(surface.width()),
                       static_cast<float>(surface.height()));
    const Color tint = Color::white() * opacity;
    DrawSurface(rt, surface, dstRect, Rect(0, 0, 1, 1), tint, false, Transform::identity());
}

void Canvas::popLayer() {
    if (layers_.empty()) return;
    std::unique_ptr<Layer> layerPtr = std::move(layers_.back());
    layers_.pop_back();
    Layer& layer = *layerPtr;
    if (stack_.size() > layer.stackMark) stack_.resize(layer.stackMark);
    if (stack_.empty()) return;

    // 目标切回上一层：parent 的 target 就是外层 Surface（layers_ 的栈顶或构造函数给的那张）
    Surface* parent = layers_.empty() ? baseTarget_ : &layers_.back()->surface;
    target_ = parent;
    if (!layers_.empty()) {
        originX_ = layers_.back()->originX;
        originY_ = layers_.back()->originY;
    } else {
        originX_ = 0;
        originY_ = 0;
    }
    if (!target_ || layer.surface.empty()) return;

    // 合成：层内容整体乘 opacity（预乘空间就是纯乘法），再按层的混合模式压到父目标
    compositeSurface(layer.surface, layer.originX, layer.originY, layer.opacity, layer.blend);
}

// 离屏内容：入层语义与 pushLayer 相同（opacity=1、SrcOver），只是出层时不合成
bool Canvas::pushOffscreen(const Rect& localBounds) {
    return pushLayer(localBounds, 1.0f, BlendMode::SrcOver);
}

bool Canvas::popOffscreen(Surface& out, int& originX, int& originY) {
    if (layers_.empty()) return false;
    std::unique_ptr<Layer> layerPtr = std::move(layers_.back());
    layers_.pop_back();
    Layer& layer = *layerPtr;
    if (stack_.size() > layer.stackMark) stack_.resize(layer.stackMark);
    if (stack_.empty()) return false;

    target_ = layers_.empty() ? baseTarget_ : &layers_.back()->surface;
    if (!layers_.empty()) {
        originX_ = layers_.back()->originX;
        originY_ = layers_.back()->originY;
    } else {
        originX_ = 0;
        originY_ = 0;
    }
    originX = layer.originX;
    originY = layer.originY;
    out = std::move(layer.surface);
    return !out.empty();
}

} // namespace bin2d
