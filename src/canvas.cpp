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
#include <cstdio>
#include <memory>

#include "bin2d/canvas_gpu.h"

namespace bin2d {
namespace {

// GPU 模式下"做不到"的图元：一次告警，别刷屏。这些都在 CPU 执行体里照常工作。
void GpuUnsupported(const char* what) {
    static const char* warned[8] = {nullptr};
    for (int i = 0; i < 8; ++i) {
        if (warned[i] == what) return;
        if (!warned[i]) { warned[i] = what; break; }
    }
    std::fprintf(stderr, "[bin2d] GPU 执行体不支持 %s，这一次调用被跳过（CPU 执行体照常）\n",
                 what);
}

} // namespace

// GPU 执行体：把"当前状态"推下去。Canvas 的变换栈 / 裁剪栈仍然只有这一份实现，
// 执行体只吃"结果"（设备变换矩阵 + 交好的裁剪矩形 + 混合模式）。
// 为什么每次绘制都推一遍而不是监听状态变化：Canvas 的状态是**整份压栈**的
// （save/restore 复制 State），没有"变化通知"可挂；而这三个 setter 在执行体里
// 都是几百纳秒级的比较+赋值，一帧几百次可以忽略。
void Canvas::syncGpu() {
    if (!gpu_) return;
    GpuSyncState(*gpu_, deviceTransform(), state().blend, state().clipRect);
}

bool Canvas::attachGpu(GpuExec* exec) {
    gpu_ = exec;
    return gpu_ != nullptr;
}

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
    if (gpu_) {
        // Replace 语义（连 alpha 一起换），矩形已经是设备像素 → 单位阵画
        GpuClearRect(*gpu_, area, c);
        return;
    }
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

void Canvas::imageChanged(const Surface& surface) {
    if (gpu_) GpuImageChanged(*gpu_, surface);
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
    if (gpu_) { syncGpu(); GpuFillPaint(*gpu_, r, 0.0f, 0.0f, paint); return; }
    FillRect(rt, r, paint, deviceTransform());
}

void Canvas::fillRounded(const Rect& r, float radius, const Paint& paint) {
    if (!target_ || r.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    if (gpu_) { syncGpu(); GpuFillPaint(*gpu_, r, radius, 0.0f, paint); return; }
    FillRoundedRect(rt, r, radius, paint, deviceTransform());
}

void Canvas::fillRounded(const Rect& r, float radius, const Color& c) {
    fillRounded(r, radius, Paint::solid(c));
}

void Canvas::fillPath(const Path& path, const Paint& paint, FillRule rule) {
    if (!target_ || path.empty()) return;
    if (gpu_) { GpuUnsupported("fillPath（任意路径要三角化）"); return; }
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    FillPath(rt, path, rule, paint, deviceTransform());
}

void Canvas::fillPath(const Path& path, const Color& c, FillRule rule) {
    fillPath(path, Paint::solid(c), rule);
}

void Canvas::strokePath(const Path& path, const Paint& paint, const StrokeStyle& style) {
    if (!target_ || path.empty() || style.width <= 0.0f) return;
    if (gpu_) { GpuUnsupported("strokePath（任意路径描边要三角化）"); return; }
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    StrokePath(rt, path, paint, style, deviceTransform());
}

void Canvas::strokePath(const Path& path, const Color& c, float width) {
    StrokeStyle st;
    st.width = width;
    strokePath(path, Paint::solid(c), st);
}

// 圆角矩形描边：两条执行体给的是**同一个视觉环**，但落像素的方式不同。
//   · CPU：外圆角路径描边（厚度的中心线在内缩 t/2 的矩形上 ⇒ 外沿恰好是 r 的边界，
//     向内 t）。这就是"外圆角矩形 + 内圆角矩形组成环"的等价写法，AA 与 strokePath
//     同一套覆盖率；
//   · GPU：执行体的 SDF 环带（|d + t/2| - t/2），一圈解析解，不做路径差集。
// 半径退化（<=0）走四条矩形：与"环 = 四个 1px 宽的带"的老实现在像素上一致，
// 而且避免给 0 半径建路径（addRoundedRect 会把它当折线）。
void Canvas::strokeRounded(const Rect& r, float radius, float thickness, const Color& c) {
    if (!target_ || r.empty() || thickness <= 0.0f) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    const float t = thickness;
    const float in = t * 0.5f;
    // 外框内缩半个线宽：描边的中心线在这里，外沿回到 r 的边界
    const Rect outer(r.x + in, r.y + in, r.w - t, r.h - t);
    const float rad = std::min(radius, std::min(r.w, r.h) * 0.5f) - in;
    if (gpu_) {
        // 执行体收**原矩形 + 原半径**：它自己把环带向内铺（见 GpuStrokeRounded）
        syncGpu();
        GpuStrokeRounded(*gpu_, r, radius, thickness, c);
        return;
    }
    if (radius <= 0.0f) {
        // 直角环：上 / 下 / 左 / 右四条。缺角由左右两条补齐（重叠区在 SrcOver 下
        // 会叠两次 —— 这是既有口径，别"优化"成路径，像素会变）
        const float ht = std::min(t, r.h);
        FillRect(rt, Rect(r.x, r.y, r.w, ht), Paint::solid(c), deviceTransform());
        FillRect(rt, Rect(r.x, r.y + r.h - ht, r.w, ht), Paint::solid(c), deviceTransform());
        const float inner = std::max(0.0f, r.h - 2.0f * t);
        if (inner > 0.0f) {
            const float wt = std::min(t, r.w);
            FillRect(rt, Rect(r.x, r.y + t, wt, inner), Paint::solid(c), deviceTransform());
            FillRect(rt, Rect(r.x + r.w - wt, r.y + t, wt, inner), Paint::solid(c),
                     deviceTransform());
        }
        return;
    }
    if (outer.w <= 0.0f || outer.h <= 0.0f) return;
    if (rad <= 0.0f) {
        // 环厚度吃掉了整个圆角：退化成内缩后的实心矩形（与路径描边的极限一致）
        FillRect(rt, outer, Paint::solid(c), deviceTransform());
        return;
    }
    Path path;
    path.addRoundedRect(outer, rad);
    StrokeStyle style;
    style.width = t;
    style.roundCap = false;
    style.squareCap = false;
    style.roundJoin = true;
    StrokePath(rt, path, Paint::solid(c), style, deviceTransform());
}

bool Canvas::backdropBlur(const Rect& area, float radius, float blurPx, float grain) {
    // CPU 执行体不实现：Surface 上没有"已经画过的中间态"可供回过头去糊。
    // 上层（binUI）自己有一份 CPU 实现，这条返回 false 就是让位给它。
    if (!gpu_) return false;
    if (!target_ || area.empty() || !(blurPx > 0.0f)) return false;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return false;
    syncGpu();
    return GpuBackdropBlur(*gpu_, area, radius, blurPx, grain);
}

void Canvas::drawImage(const Image& image, const Rect& dstLocal, const Rect& srcUv,
                       const Color& tint, ImageFilter filter) {
    if (!target_ || image.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    if (gpu_) { syncGpu(); GpuDrawSurface(*gpu_, image.surface(), dstLocal, srcUv, tint); return; }
    DrawImage(rt, image, dstLocal, srcUv, tint, filter == ImageFilter::Nearest,
              deviceTransform());
}

void Canvas::drawSurface(const Surface& surface, const Rect& dstLocal, const Rect& srcUv,
                         const Color& tint, ImageFilter filter) {
    if (!target_ || surface.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    if (gpu_) { syncGpu(); GpuDrawSurface(*gpu_, surface, dstLocal, srcUv, tint); return; }
    DrawSurface(rt, surface, dstLocal, srcUv, tint, filter == ImageFilter::Nearest,
                deviceTransform());
}

// ---- 外部纹理（见 canvas.h 的说明）----
uint64_t Canvas::createTexture(int w, int h, bool nearest) {
    if (!gpu_ || w <= 0 || h <= 0) return 0;
    return GpuTexCreate(*gpu_, w, h, nearest);
}

void Canvas::uploadTexture(uint64_t tex, const Rect& box, const uint8_t* pixels,
                           uint32_t stride) {
    if (!gpu_ || !tex || !pixels) return;
    GpuTexUpload(*gpu_, tex, box, pixels, stride);
}

void Canvas::destroyTexture(uint64_t tex) {
    if (!gpu_ || !tex) return;
    GpuTexDestroy(*gpu_, tex);
}

void Canvas::drawTexture(uint64_t tex, const Rect& dstLocal, const Rect& srcUv,
                         const Color& tint) {
    if (!target_ || !gpu_ || !tex || dstLocal.empty()) return;
    syncGpu();
    GpuDrawTexture(*gpu_, tex, dstLocal, srcUv, tint);
}

void Canvas::drawNineSlice(const Image& image, const Rect& dstLocal, float insetL,
                           float insetT, float insetR, float insetB, const Color& tint,
                           ImageFilter filter) {
    if (!target_ || image.empty()) return;
    RasterTarget rt = makeTarget();
    if (rt.clip.empty()) return;
    if (gpu_) {
        // 九宫格的切分数学只有一份（这个函数），GPU 执行体照它切完再逐块贴图 ——
        // 九块各自是一个带 UV 子矩形的四边形，与 CPU 那条路同一套切分。
        syncGpu();
        GpuDrawNineSlice(*gpu_, image.surface(), dstLocal, insetL, insetT, insetR, insetB, tint);
        return;
    }
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
    if (gpu_) {
        // GPU 模式的图层由执行体持有（窗口大小的离屏纹理）：Canvas 这边**不改**
        // target_/originX_ —— 设备坐标始终是同一套，层内层外的几何代码完全一样。
        syncGpu();
        if (!GpuPushLayer(*gpu_, localBounds, opacity)) return false;
        state().blend = BlendMode::SrcOver;
        return true;
    }
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
    // 绝对原点 = 父目标的绝对原点 + 本层在父目标里的位置。
    // clipped 是**父目标像素**坐标（deviceTransform 已经减过父层的 originX_），
    // 所以这里必须把父原点加回来 —— 以前直接把 originX_ 赋成 clipped.x，
    // 嵌套层的层内坐标就少减了父原点，内容整块偏移（0.22.8 修）。
    layer.absX = originX_ + layer.originX;
    layer.absY = originY_ + layer.originY;
    layer.opacity = opacity;
    layer.blend = blend;
    layer.stackMark = stack_.size();   // 出层时截断：层内漏 call restore 也不会串状态
    layers_.push_back(std::move(layerPtr));

    // 目标切到层；层的画布同样从"整块层"开始，裁剪矩形换成层内矩形
    Layer& top = *layers_.back();
    target_ = &top.surface;
    originX_ = top.absX;
    originY_ = top.absY;
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
    // 合成也是一次**真实的绘制**：必须按当前 clipRect 落像素（与 makeTarget 同一口径）。
    // 只按 target 边界裁是不够的 —— 局部重绘（脏矩形）时，层内容是按"入层那一刻的
    // clipRect"分配尺寸的，而外阴影会按 (sdx,sdy) 偏移贴回，偏移量就能把阴影推到
    // 脏矩形外面去（脏矩形边界上留下一圈硬边旧阴影）。GPU 那条出口（GpuPopLayer /
    // drawContentLayer / fillContentLayer）是按裁剪栈 scissor 的，这里补齐同一条语义。
    rt.clip = Rect::intersect(state().clipRect, target_->bounds());
    if (rt.clip.empty()) return;
    rt.mask = nullptr;              // 层外已经有了自己的裁剪：层已按裁剪尺寸分配
    rt.blend = blend;
    const Rect dstRect(static_cast<float>(originX), static_cast<float>(originY),
                       static_cast<float>(surface.width()),
                       static_cast<float>(surface.height()));
    const Color tint = Color::white() * opacity;
    DrawSurface(rt, surface, dstRect, Rect(0, 0, 1, 1), tint, false, Transform::identity());
}

void Canvas::popLayer() {
    if (gpu_) {
        // 出层前的合成是一次**真实的绘制**：它必须按**当前**（已出层的）裁剪落像素。
        // 不 syncGpu 的后果实测过：层内最后一笔的 clip 留在执行体里，层合成被裁成
        // 那一小块 —— 表现是"图层内容只剩最后画的那个子元素，盒子本身没了"
        // （binUI 的 opacity 页面整片白，就是这么来的）。
        syncGpu();
        GpuPopLayer(*gpu_);
        return;
    }
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
        // 回到父层的**绝对**原点（层内坐标要靠它减，见 Layer::absX）
        originX_ = layers_.back()->absX;
        originY_ = layers_.back()->absY;
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
    // GPU 模式的出口是"像素回 CPU"，那正是这一层不该做的事 —— 明确失败，
    // 调用方据此走不需要离屏的那条路（binUI 的 mask/clip-path 就是这样退化的）。
    // **要设备上的离屏层**请用 pushContentLayer：那一对的目的地是设备纹理，
    // 出口是句柄而不是 Surface（0.22.5）。
    if (gpu_) { GpuUnsupported("pushOffscreen（离屏出像素）"); return false; }
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
        // 回到父层的**绝对**原点（层内坐标要靠它减，见 Layer::absX）
        originX_ = layers_.back()->absX;
        originY_ = layers_.back()->absY;
    } else {
        originX_ = 0;
        originY_ = 0;
    }
    originX = layer.originX;
    originY = layer.originY;
    out = std::move(layer.surface);
    return !out.empty();
}

// ------------------------------------------------------------
// 设备侧离屏内容（0.22.5）
//
// 两条出口的分工写在这里一次：GPU 模式下一律走执行体（内容留在设备上，出口是
// 句柄），CPU 模式一律返回 false 让调用方走自己的 Surface 那条路。没有"偷偷降级到
// 另一条"的分支 —— 调用方（binUI 的 pushLayerEx）比这一层更清楚降级该怎么降。
// ------------------------------------------------------------
bool Canvas::pushContentLayer(const Rect& localBounds) {
    if (!gpu_ || !target_ || localBounds.empty()) return false;
    syncGpu();
    if (!GpuPushContentLayer(*gpu_, localBounds)) return false;
    // 与 pushLayer 的 GPU 分支逐条相同：层内默认正常混合（层的语义在贴回时表达）
    state().blend = BlendMode::SrcOver;
    return true;
}

bool Canvas::popContentLayer(uint64_t& outHandle, Rect& outScreen) {
    outHandle = 0;
    if (!gpu_) return false;
    return GpuPopContentLayer(*gpu_, outHandle, outScreen);
}

bool Canvas::drawContentLayer(uint64_t handle, const Rect& dstLocal, const Rect& uv,
                              const Color& tint, float blurPx, bool nearest) {
    if (!gpu_ || !target_ || !handle) return false;
    // dst 走 deviceTransform()（与 clipRect/drawTexture 同一条口径：层内要减掉原点），
    // 执行体那边按屏幕空间贴回 —— 顶点已经是目标像素，不需要再过一遍变换。
    const Rect dst = deviceTransform().mapBounds(dstLocal);
    if (dst.empty()) return false;
    syncGpu();
    return GpuDrawContentLayer(*gpu_, handle, dst, uv, tint, blurPx, nearest);
}

bool Canvas::fillContentLayer(uint64_t handle, const Rect& dstLocal, const Color& color,
                              float blurPx) {
    if (!gpu_ || !target_ || !handle) return false;
    const Rect dst = deviceTransform().mapBounds(dstLocal);
    if (dst.empty()) return false;
    syncGpu();
    return GpuFillContentLayer(*gpu_, handle, dst, color, blurPx);
}

void Canvas::releaseContentLayer(uint64_t handle) {
    if (!gpu_ || !handle) return;
    GpuReleaseContentLayer(*gpu_, handle);
}

} // namespace bin2d
