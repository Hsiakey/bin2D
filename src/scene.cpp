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

// ============================================================
// 子树缓存（0.9.0）
//
// 缓存粒度是**组**：一张卡片 5 个图元，逐个缓存既费内存又省不下合成，整组一张
// 位图才划算（roadmap ⑤ 的"粒度按图层不按节点"）。命中时把那一位图贴回去，
// 像素与"重新画一遍子树"逐像素相同 —— 这条由 tests/test_cache.cpp 钉死。
//
// 只缓存"能直接当位图贴回"的组（见 CacheableGroup）：组自身 opacity=1、blend=SrcOver、
// 无裁剪/投影。其余组走原路径 —— 缓存只替换**同语义**的路径，不许为了缓存改渲染语义。
// ============================================================

namespace {

bool CacheableGroup(const Group& g, int minArea) {
    if (!g.visible || g.opacity < 1.0f || g.blend != BlendMode::SrcOver) return false;
    if (g.hasClip || g.hasClipRect || g.shadow.visible()) return false;
    if (g.children.size() < 2) return false;
    const Rect b = g.localBounds();
    return b.w * b.h >= static_cast<float>(minArea);
}

void DrawGroupCached(Group& g, Canvas& canvas, SceneCache& cache) {
    const Rect localB = g.localBounds();
    const Transform devXf = canvas.transform() * g.transform;
    const Rect dev = devXf.mapBounds(localB).snappedOut();
    if (dev.empty()) return;
    const int dx = static_cast<int>(dev.x), dy = static_cast<int>(dev.y);
    const int dw = static_cast<int>(dev.w), dh = static_cast<int>(dev.h);
    const uint64_t ver = g.contentVersion();

    ++cache.stats().groups_total;
    SceneCache::Entry* hitEntry = cache.Find(&g);
    if (hitEntry && hitEntry->version == ver && hitEntry->w == dw && hitEntry->h == dh) {
        hitEntry->stamp = cache.frame();
        ++cache.stats().hits;
        canvas.save();
        canvas.setTransform(Transform::identity());
        canvas.drawSurface(hitEntry->surface,
                           Rect(static_cast<float>(dx), static_cast<float>(dy),
                                static_cast<float>(dw), static_cast<float>(dh)));
        canvas.restore();
        return;
    }
    ++cache.stats().misses;

    canvas.save();
    g.applyState(canvas);
    Surface content;
    int ox = 0, oy = 0;
    bool rendered = false;
    if (canvas.pushOffscreen(localB)) {
        for (const auto& ch : g.children)
            if (ch) ch->draw(canvas);
        rendered = canvas.popOffscreen(content, ox, oy);
    }
    canvas.restore();   // 回到父坐标系（popOffscreen 已把目标/原点还原）

    if (!rendered) return;   // 完全被裁掉：什么都不画，也不进缓存（下次同样廉价）

    SceneCache::Entry& slot = hitEntry ? *hitEntry : cache.Insert(&g);
    slot.surface = std::move(content);
    slot.ox = ox;
    slot.oy = oy;
    slot.w = dw;
    slot.h = dh;
    slot.version = ver;
    slot.stamp = cache.frame();

    canvas.save();
    canvas.setTransform(Transform::identity());
    canvas.drawSurface(slot.surface,
                       Rect(static_cast<float>(ox), static_cast<float>(oy),
                            static_cast<float>(slot.surface.width()),
                            static_cast<float>(slot.surface.height())));
    canvas.restore();
}

} // namespace

void SceneCache::clear() {
    entries_.clear();
    stats_ = Stats{};
    frame_ = 0;
}

void SceneCache::BeginFrame() {
    ++frame_;
    stats_.groups_total = 0;
    stats_.hits = 0;
    stats_.misses = 0;
}

void SceneCache::EndFrame() {
    // 淘汰"这一帧没再出现"的组（被删掉的子树），否则缓存会永久占着内存
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.stamp != frame_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    size_t bytes = 0;
    for (const auto& kv : entries_) bytes += kv.second.surface.byteSize();
    stats_.entries = static_cast<int>(entries_.size());
    stats_.bytes = bytes;
}

void SceneCache::Erase(const void* key) { entries_.erase(key); }

void Scene::drawCached(Surface& target, SceneCache& cache) const {
    // 根组自身的 opacity/blend 需要"整棵树一层"的合成语义，不在缓存范围内：直接退化。
    if (root_.opacity < 1.0f || root_.blend != BlendMode::SrcOver) {
        cache.clear();
        draw(target);
        return;
    }
    Canvas canvas(target);
    canvas.clear(background);
    cache.BeginFrame();
    canvas.save();
    root_.applyState(canvas);
    for (const auto& ch : root_.children) {
        if (!ch || !ch->visible || ch->opacity <= 0.0f) continue;
        Group* g = dynamic_cast<Group*>(ch.get());
        if (g && CacheableGroup(*g, cache.minGroupArea)) {
            DrawGroupCached(*g, canvas, cache);
        } else {
            ch->draw(canvas);
        }
    }
    canvas.restore();
    cache.EndFrame();
}

} // namespace bin2d
