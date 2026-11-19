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

// 子节点在**设备空间**的包围盒（0.10.0 脏矩形用）。
//
// 必须把"绘制时会画到哪儿"全部算进去，漏一点就会留下旧像素：
//   · 描边：path.bounds() 是路径中心线的包围盒，线宽会往外探半个宽度；
//   · 投影：偏移 + 模糊半径（偏移要跟着子节点自己的 transform 走）；
//   · AA：再留 1px。
// 宁可算大一点（多重画几个像素），不能算小 —— 算小的症状是"移动的图标拖一条残影"。
struct DevBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};

DevBox ChildBox(const SceneItem& ch, const Transform& groupDevXf) {
    Rect b = ch.localBounds();
    const float aa = 1.0f;
    b = Rect(b.x - aa, b.y - aa, b.w + aa * 2.0f, b.h + aa * 2.0f);
    if (const Shape* sh = dynamic_cast<const Shape*>(&ch)) {
        if (sh->stroked && sh->stroke.width > 0.0f) {
            const float hw = sh->stroke.width * 0.5f;
            b = Rect(b.x - hw, b.y - hw, b.w + hw * 2.0f, b.h + hw * 2.0f);
        }
    }
    const Transform dev = groupDevXf * ch.transform;
    Rect r = dev.mapBounds(b).snappedOut();
    if (const Shape* sh = dynamic_cast<const Shape*>(&ch)) {
        if (sh->shadow.visible()) {
            // 偏移属于子节点局部空间：只取线性部分（平移不参与方向）
            const Transform linear(ch.transform.a, ch.transform.b, ch.transform.c,
                                   ch.transform.d, 0.0f, 0.0f);
            const Point off = groupDevXf.map(linear.map(sh->shadow.dx, sh->shadow.dy));
            const float m = sh->shadow.blur;
            r = Rect(std::min(r.x, r.x + off.x - m), std::min(r.y, r.y + off.y - m),
                     std::max(r.right(), r.right() + off.x + m) - std::min(r.x, r.x + off.x - m),
                     std::max(r.bottom(), r.bottom() + off.y + m) - std::min(r.y, r.y + off.y - m));
        }
    }
    DevBox o;
    o.x0 = static_cast<int>(std::floor(r.x));
    o.y0 = static_cast<int>(std::floor(r.y));
    o.x1 = static_cast<int>(std::ceil(r.right()));
    o.y1 = static_cast<int>(std::ceil(r.bottom()));
    return o;
}

// 把子节点的版本/包围盒记进条目（全量与局部重画共用）
void StampChildren(const Group& g, const Transform& devXf, SceneCache::Entry& e) {
    e.children.resize(g.children.size());
    for (size_t i = 0; i < g.children.size(); ++i) {
        const SceneItem* ch = g.children[i].get();
        auto& st = e.children[i];
        st.ptr = ch;
        if (!ch) {
            st.version = 0;
            st.x0 = st.y0 = st.x1 = st.y1 = 0;
            continue;
        }
        st.version = ch->version();
        const DevBox b = ChildBox(*ch, devXf);
        st.x0 = b.x0;
        st.y0 = b.y0;
        st.x1 = b.x1;
        st.y1 = b.y1;
    }
}

// 算脏矩形：返回 false = 不能局部重画（结构变了 / 自身变了 / 没有子节点变化 / 太大）
bool ComputeDirty(const Group& g, const Transform& devXf, const SceneCache::Entry& e,
                  float maxRatio, int& x0, int& y0, int& x1, int& y1, bool& structural) {
    structural = false;
    if (e.children.size() != g.children.size()) {
        structural = true;
        return false;
    }
    bool any = false;
    int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    for (size_t i = 0; i < g.children.size(); ++i) {
        const SceneItem* ch = g.children[i].get();
        const auto& st = e.children[i];
        if (st.ptr != ch) {   // 子节点被换过：局部重画没意义
            structural = true;
            return false;
        }
        if (!ch || st.version == ch->version()) continue;
        if (!ch->visible) {   // 隐藏/显示会改变绘画顺序的语义，交给整组
            structural = true;
            return false;
        }
        const DevBox nb = ChildBox(*ch, devXf);
        if (!any) {
            ux0 = std::min(st.x0, nb.x0);
            uy0 = std::min(st.y0, nb.y0);
            ux1 = std::max(st.x1, nb.x1);
            uy1 = std::max(st.y1, nb.y1);
            any = true;
        } else {
            ux0 = std::min(ux0, std::min(st.x0, nb.x0));
            uy0 = std::min(uy0, std::min(st.y0, nb.y0));
            ux1 = std::max(ux1, std::max(st.x1, nb.x1));
            uy1 = std::max(uy1, std::max(st.y1, nb.y1));
        }
    }
    if (!any) return false;   // 版本变了却没有子节点变化 → 保守走整组
    x0 = ux0;
    y0 = uy0;
    x1 = ux1;
    y1 = uy1;
    const long long area = static_cast<long long>(x1 - x0) * static_cast<long long>(y1 - y0);
    const long long total = static_cast<long long>(e.w) * static_cast<long long>(e.h);
    return !(area > static_cast<long long>(maxRatio * static_cast<float>(total)));
}

void DrawGroupFull(Group& g, Canvas& canvas, SceneCache& cache, SceneCache::Entry* hitEntry,
                   const Transform& devXf, int dw, int dh, uint64_t ver, uint64_t selfVer) {
    const Rect localB = g.localBounds();
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
    if (!rendered) return;

    SceneCache::Entry& slot = hitEntry ? *hitEntry : cache.Insert(&g);
    slot.surface = std::move(content);
    slot.ox = ox;
    slot.oy = oy;
    slot.w = dw;
    slot.h = dh;
    slot.version = ver;
    slot.selfVersion = selfVer;
    slot.stamp = cache.frame();
    StampChildren(g, devXf, slot);

    canvas.save();
    canvas.setTransform(Transform::identity());
    canvas.drawSurface(slot.surface,
                       Rect(static_cast<float>(ox), static_cast<float>(oy),
                            static_cast<float>(slot.surface.width()),
                            static_cast<float>(slot.surface.height())));
    canvas.restore();
}

// 脏矩形局部重画：清掉脏区 → 只重画与脏区相交的子节点（按顺序）→ 只贴回脏区
bool DrawGroupPartial(Group& g, Canvas& canvas, SceneCache::Entry& e, const Transform& devXf,
                      int x0, int y0, int x1, int y1) {
    const int lx = x0 - e.ox;
    const int ly = y0 - e.oy;
    const int lw = x1 - x0;
    const int lh = y1 - y0;
    if (lx < 0 || ly < 0 || lw <= 0 || lh <= 0) return false;
    if (lx + lw > e.surface.width() || ly + lh > e.surface.height()) return false;

    const Rect dirtyLocal(static_cast<float>(lx), static_cast<float>(ly),
                          static_cast<float>(lw), static_cast<float>(lh));
    e.surface.clearRect(dirtyLocal);

    {
        Canvas sub(e.surface, dirtyLocal);
        sub.setTransform(Transform::translate(static_cast<float>(-e.ox),
                                             static_cast<float>(-e.oy)) * devXf);
        for (const auto& ch : g.children) {
            if (!ch) continue;
            const DevBox b = ChildBox(*ch, devXf);
            if (b.x1 <= x0 || b.x0 >= x1 || b.y1 <= y0 || b.y0 >= y1) continue;
            ch->draw(sub);
        }
    }

    // 注意：只重画脏区，但**整组贴回**。目标是每帧清过屏的，"只贴脏区"会让组里
    // 其余部分这一帧根本没被贴上去（症状：卡片变成底色）。贴图本身很便宜，
    // 省下来的是栅格化 —— 这才是脏矩形的收益所在。
    canvas.save();
    canvas.setTransform(Transform::identity());
    canvas.drawSurface(e.surface,
                       Rect(static_cast<float>(e.ox), static_cast<float>(e.oy),
                            static_cast<float>(e.surface.width()),
                            static_cast<float>(e.surface.height())));
    canvas.restore();
    return true;
}

void DrawGroupCached(Group& g, Canvas& canvas, SceneCache& cache) {
    const Rect localB = g.localBounds();
    const Transform devXf = canvas.transform() * g.transform;
    const Rect dev = devXf.mapBounds(localB).snappedOut();
    if (dev.empty()) return;
    const int dw = static_cast<int>(dev.w), dh = static_cast<int>(dev.h);
    const uint64_t ver = g.contentVersion();
    const uint64_t selfVer = g.selfVersion();

    ++cache.stats().groups_total;
    SceneCache::Entry* entry = cache.Find(&g);
    const bool sameRect = entry && entry->w == dw && entry->h == dh;

    if (entry && sameRect && entry->version == ver) {
        entry->stamp = cache.frame();
        ++cache.stats().hits;
        canvas.save();
        canvas.setTransform(Transform::identity());
        canvas.drawSurface(entry->surface,
                           Rect(static_cast<float>(entry->ox), static_cast<float>(entry->oy),
                                static_cast<float>(entry->surface.width()),
                                static_cast<float>(entry->surface.height())));
        canvas.restore();
        return;
    }
    ++cache.stats().misses;

    if (cache.partialRepaint && sameRect && entry && entry->selfVersion == selfVer &&
        entry->version != ver) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool structural = false;
        if (ComputeDirty(g, devXf, *entry, cache.partialMaxRatio, x0, y0, x1, y1, structural)) {
            // 夹到组矩形里（子节点可能探出组包围盒：只影响组外的像素，那里本来就没画）
            x0 = std::max(x0, static_cast<int>(dev.x));
            y0 = std::max(y0, static_cast<int>(dev.y));
            x1 = std::min(x1, static_cast<int>(dev.x) + dw);
            y1 = std::min(y1, static_cast<int>(dev.y) + dh);
            if (x1 > x0 && y1 > y0 && DrawGroupPartial(g, canvas, *entry, devXf, x0, y0, x1, y1)) {
                entry->version = ver;
                entry->stamp = cache.frame();
                StampChildren(g, devXf, *entry);
                ++cache.stats().partials;
                cache.stats().partial_pixels += static_cast<long long>(x1 - x0) * (y1 - y0);
                return;
            }
        }
    }

    DrawGroupFull(g, canvas, cache, entry, devXf, dw, dh, ver, selfVer);
    cache.stats().full_pixels += static_cast<long long>(dw) * static_cast<long long>(dh);
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
    stats_.partials = 0;
    stats_.evicted = 0;
    stats_.partial_pixels = 0;
    stats_.full_pixels = 0;
}

void SceneCache::EndFrame() {
    // ① 淘汰"这一帧没再出现"的组（被删掉的子树），否则缓存会永久占着内存
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.stamp != frame_) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    auto totalBytes = [this]() {
        size_t b = 0;
        for (const auto& kv : entries_) b += kv.second.surface.byteSize();
        return b;
    };
    size_t bytes = totalBytes();

    // ② 字节上限：按"最久没被访问"淘汰。本帧用过的条目也在候选里 —— 到预算上限时
    //    宁可下一帧重画一次，也不能让缓存无限吃内存；stats().evicted 让这件事可观测。
    if (byteBudget > 0 && bytes > byteBudget) {
        std::vector<std::pair<uint64_t, const void*>> order;
        order.reserve(entries_.size());
        for (const auto& kv : entries_) order.emplace_back(kv.second.stamp, kv.first);
        std::sort(order.begin(), order.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& o : order) {
            if (bytes <= byteBudget) break;
            auto it = entries_.find(o.second);
            if (it == entries_.end()) continue;
            bytes -= it->second.surface.byteSize();
            entries_.erase(it);
            ++stats_.evicted;
        }
    }
    stats_.entries = static_cast<int>(entries_.size());
    stats_.bytes = totalBytes();
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
