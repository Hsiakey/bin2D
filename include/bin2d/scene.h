#pragma once
// ============================================================
// bin2d/scene.h —— 保留模式场景图（Scene → Group → Shape / ImageShape）
//
// 为什么保留模式不是"再加一层 API"，而是必须的一层：
//   即时模式每帧都要重跑一遍"建路径 + 算包围盒"，一份内容画 3 次就付 3 次。
//   Scene 把"要画什么"留住，渲染时只遍历；将来加缓存（按 version 判断脏不脏）
//   时，正是这一层知道"内容没变"。
//
// **本版故意不做缓存与脏区**：先把"渲染结果 == Canvas 直接画"这条基线钉死
// （tests/test_scene.cpp 逐像素比），缓存才有意义 —— 缓存加在一个还不稳定的
// 渲染路径上，出的问题会看起来像"渲染错"而不是"缓存错"。
// `version()` 就是留给下一阶段的钩子：任何改动都叫 touch()。
//
// 组透明度的语义：Group 的 opacity 把整个子树收进离屏再整体乘 alpha
// （子元素重叠处不会叠深）。单个 Shape 没有重叠问题，直接折进颜料更便宜。
// ============================================================

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/core.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/raster.h"
#include "bin2d/surface.h"
#include "bin2d/transform.h"

namespace bin2d {

// ------------------------------------------------------------
// 内容版本号的哈希（0.9.0 的子树缓存判断"这块内容变没变"）
//
// 为什么用"混合哈希"而不是记一个全局脏标记：缓存要按**组**失效 —— 改了第 3 张
// 卡片的文字，另外 39 个组不该重画。每个节点带自己的 version，组的版本号由
// 子树混合而成；改一个叶子只让祖先的哈希变，兄弟节点不受影响。
//
// 注意：哈希只覆盖"框架字段"（transform / opacity / blend / clip / shadow / 子节点指针）
// 和每个节点的 version()。Shape 的 path/paint 是公开字段、没有迭代器可哈希，
// 所以**改了形状几何或颜料之后必须调用 item.touch()** —— 与 Scene::version()
// 从 0.1 起就有的约定完全一致（"任何改动都叫一次 touch"）。
// ------------------------------------------------------------
namespace detail {
inline uint64_t HashMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
inline uint64_t HashFloat(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
inline uint64_t HashU64(uint64_t v) { return v; }
} // namespace detail

// ------------------------------------------------------------
// 节点基类
// ------------------------------------------------------------
class SceneItem {
public:
    virtual ~SceneItem() = default;

    Transform transform = Transform::identity();
    float opacity = 1.0f;
    BlendMode blend = BlendMode::SrcOver;
    bool visible = true;

    // 局部空间的裁剪路径（空 = 不裁）。组本身不建裁剪矩形：矩形裁剪请用
    // clipRect（更便宜），路径裁剪才会走覆盖率遮罩。
    bool hasClip = false;
    Path clip;
    Rect clipRect;
    bool hasClipRect = false;

    Shadow shadow;   // 只对 Shape / ImageShape 有意义（Group 的投影 = 先画子树再模糊，本版不做）

    // 局部包围盒（不含投影、不含子节点之外的东西）；用于图层分配与脏区
    virtual Rect localBounds() const = 0;
    virtual void draw(Canvas& canvas) const = 0;

    // 内容版本号（0.9.0）：改完任何影响像素的字段后调用 touch()。
    // 子树缓存（SceneCache）靠它判断"这个组能不能直接贴上一帧的位图"。
    uint64_t version() const { return version_; }
    void touch() { ++version_; }

    // 本节点 + 子树的内容版本（框架字段 + 每个节点的 version() 混合而成）。
    // Group 覆盖它把子节点混进来；Shape 的 path/paint 靠 touch() 表达。
    virtual uint64_t contentVersion() const {
        using namespace detail;
        uint64_t h = 0xCBF29CE484222325ull;
        h = HashMix(h, version_);
        h = HashMix(h, HashFloat(transform.a));
        h = HashMix(h, HashFloat(transform.b));
        h = HashMix(h, HashFloat(transform.c));
        h = HashMix(h, HashFloat(transform.d));
        h = HashMix(h, HashFloat(transform.e));
        h = HashMix(h, HashFloat(transform.f));
        h = HashMix(h, HashFloat(opacity));
        h = HashMix(h, visible ? 1u : 0u);
        h = HashMix(h, static_cast<uint64_t>(blend));
        h = HashMix(h, hasClip ? 1u : 0u);
        h = HashMix(h, hasClipRect ? 1u : 0u);
        h = HashMix(h, HashFloat(clipRect.x));
        h = HashMix(h, HashFloat(clipRect.y));
        h = HashMix(h, HashFloat(clipRect.w));
        h = HashMix(h, HashFloat(clipRect.h));
        h = HashMix(h, shadow.visible() ? 1u : 0u);
        h = HashMix(h, HashFloat(shadow.dx));
        h = HashMix(h, HashFloat(shadow.dy));
        h = HashMix(h, HashFloat(shadow.blur));
        h = HashMix(h, static_cast<uint64_t>(shadow.color.toArgbStraight()));
        return h;
    }

    // 把当前节点的 transform/裁剪应用到画布上（子类进来第一件事就是调它）
    void applyState(Canvas& canvas) const {
        canvas.concat(transform);
        if (hasClipRect) canvas.clipRect(clipRect);
        if (hasClip && !clip.empty()) canvas.clipPath(clip);
    }

private:
    uint64_t version_ = 1;
};

// ------------------------------------------------------------
// 形状：一条路径 + 一份颜料（填充或描边，二选一；两者都要就挂两个 Shape）
// ------------------------------------------------------------
class Shape : public SceneItem {
public:
    Path path;
    Paint paint = Paint::solid(Color::white());
    FillRule rule = FillRule::NonZero;

    bool stroked = false;
    StrokeStyle stroke;

    Rect localBounds() const override { return path.bounds(); }

    void draw(Canvas& canvas) const override {
        if (!visible || opacity <= 0.0f || path.empty()) return;
        canvas.save();
        applyState(canvas);
        if (shadow.visible()) DrawOwnShadow(canvas);
        const float k = opacity > 1.0f ? 1.0f : opacity;
        if (stroked) {
            StrokeStyle st = stroke;
            if (st.width <= 0.0f) st.width = 1.0f;
            canvas.strokePath(path, FadePaint(paint, k), st);
        } else {
            canvas.fillPath(path, FadePaint(paint, k), rule);
        }
        canvas.restore();
    }

private:
    // 投影：把形状按偏移画出阴影色 → 模糊 → 贴回来。
    // 必须自己开一张临时位图（Canvas 的图层接口不暴露"合成前的内容"）。
    void DrawOwnShadow(Canvas& canvas) const;
};

// ------------------------------------------------------------
// 图片形状（含九宫格）
// ------------------------------------------------------------
class ImageShape : public SceneItem {
public:
    const Image* image = nullptr;
    Rect dst;                       // 局部目标矩形
    Rect srcUv = Rect(0, 0, 1, 1);
    Color tint = Color::white();
    ImageFilter filter = ImageFilter::Linear;

    bool nineSlice = false;
    // 九宫格边界自动探测（0.8.0）：为 true 时忽略下面四个 inset，每次绘制按图内容
    // 找"可拉伸带"。同一个图每帧重复画时建议自己缓存 DetectNineSliceInsets 的结果，
    // 这个开关是给"素材会换、边界跟着变"的场合用的。
    bool nineSliceAuto = false;
    float insetL = 0.0f, insetT = 0.0f, insetR = 0.0f, insetB = 0.0f;

    Rect localBounds() const override { return dst; }

    void draw(Canvas& canvas) const override {
        if (!visible || opacity <= 0.0f || !image || image->empty()) return;
        canvas.save();
        applyState(canvas);
        const float k = opacity > 1.0f ? 1.0f : opacity;
        const Color t = tint * k;
        if (nineSliceAuto)
            canvas.drawNineSliceAuto(*image, dst, t, filter);
        else if (nineSlice)
            canvas.drawNineSlice(*image, dst, insetL, insetT, insetR, insetB, t, filter);
        else
            canvas.drawImage(*image, dst, srcUv, t, filter);
        canvas.restore();
    }
};

// ------------------------------------------------------------
// 组
// ------------------------------------------------------------
class Group : public SceneItem {
public:
    std::vector<std::unique_ptr<SceneItem>> children;

    SceneItem& add(std::unique_ptr<SceneItem> item) {
        children.push_back(std::move(item));
        return *children.back();
    }
    void clearChildren() {
        children.clear();
        touch();
    }
    size_t childCount() const { return children.size(); }

    uint64_t contentVersion() const override {
        using namespace detail;
        uint64_t h = SceneItem::contentVersion();
        h = HashMix(h, children.size());
        for (const auto& ch : children) {
            h = HashMix(h, reinterpret_cast<uintptr_t>(ch.get()));
            if (ch) h = HashMix(h, ch->contentVersion());
        }
        return h;
    }

    Rect localBounds() const override {
        bool first = true;
        Rect out;
        for (const auto& ch : children) {
            if (!ch || !ch->visible) continue;
            const Rect b = ch->transform.mapBounds(ch->localBounds());
            if (first) { out = b; first = false; continue; }
            const float minx = std::min(out.x, b.x), miny = std::min(out.y, b.y);
            const float maxx = std::max(out.right(), b.right());
            const float maxy = std::max(out.bottom(), b.bottom());
            out = Rect(minx, miny, maxx - minx, maxy - miny);
        }
        return out;
    }

    void draw(Canvas& canvas) const override {
        if (!visible || opacity <= 0.0f) return;
        canvas.save();
        applyState(canvas);
        const float k = opacity > 1.0f ? 1.0f : opacity;
        if (k >= 1.0f && blend == BlendMode::SrcOver) {
            for (const auto& ch : children)
                if (ch) ch->draw(canvas);
        } else {
            // 组透明度 / 组混合模式：必须离屏（重叠处不叠深、混合只作用一次）。
            // 层的矩形给"子内容空间"（applyState 之后画布已经在组的坐标系里，
            // pushLayer 自己会过变换；再乘一次就是双重变换，内容会被画到层外面）。
            const Rect b = localBounds();
            if (canvas.pushLayer(b, k, blend)) {
                for (const auto& ch : children)
                    if (ch) ch->draw(canvas);
                canvas.popLayer();
            } else {
                // 尺寸非法（空组）：退化成"逐个子节点乘 alpha"
                for (const auto& ch : children) {
                    if (!ch) continue;
                    const float save = ch->opacity;
                    ch->opacity = save * k;
                    ch->draw(canvas);
                    ch->opacity = save;
                }
            }
        }
        canvas.restore();
    }
};

// ------------------------------------------------------------
// 子树缓存（0.9.0）
//
// 缓存粒度 = **组**（不是节点）：一张卡片 5 个图元，逐个缓存 5 张位图既费内存又
// 省不下合成；整组一张位图才划算。这也是 roadmap ⑤ 里"缓存粒度按图层不按节点"。
//
// 只缓存"能直接当位图贴回"的组：组自身 opacity=1、blend=SrcOver、没有裁剪/投影、
// 至少 2 个子节点、设备面积 >= minGroupArea。别的组直接走原来的绘制路径 ——
// 缓存只允许替换**同语义**的那条路径，绝不"为了缓存改渲染语义"。
//
// 版本契约（重要）：改 Shape 的 path/paint 之后必须 item.touch()；框架字段
// （transform/opacity/blend/clip/shadow/子节点表）会自动参与哈希，不用手动 touch。
// ------------------------------------------------------------
class SceneCache {
public:
    struct Stats {
        int groups_total = 0;   // 本帧遇到的"可缓存组"数
        int hits = 0;           // 直接贴上一帧位图的
        int misses = 0;         // 重画并重新缓存的
        int entries = 0;        // 当前常驻的缓存条目
        size_t bytes = 0;       // 常驻缓存位图总量
    };

    // 可缓存组的最小设备面积（像素²）。太小的话贴一张位图的采样成本 >= 直接画。
    int minGroupArea = 4096;

    void clear();
    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }
    bool empty() const { return entries_.empty(); }

    // ---- 以下给 Scene::drawCached 的内部实现用（不属于稳定 API）----
    struct Entry {
        Surface surface;
        int ox = 0, oy = 0;
        int w = 0, h = 0;
        uint64_t version = 0;
        uint64_t stamp = 0;   // 最近一次被访问的帧号（用来淘汰没再出现的组）
    };
    void BeginFrame();
    void EndFrame();
    uint64_t frame() const { return frame_; }
    Entry* Find(const void* key) {
        auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : &it->second;
    }
    Entry& Insert(const void* key) { return entries_[key]; }
    void Erase(const void* key);

private:
    std::unordered_map<const void*, Entry> entries_;
    Stats stats_;
    uint64_t frame_ = 0;
};

// ------------------------------------------------------------
// 场景
// ------------------------------------------------------------
class Scene {
public:
    Scene() = default;
    explicit Scene(float w, float h) : size_(Size{w, h}) {}

    Size size() const { return size_; }
    void setSize(const Size& s) { size_ = s; touch(); }

    Group& root() { return root_; }
    const Group& root() const { return root_; }

    Color background = Color::trans();
    void setBackground(const Color& c) { background = c; touch(); }

    // 内容版本号：任何改动都叫一次 touch()。下一阶段的缓存靠它判断脏不脏
    uint64_t version() const { return version_; }
    void touch() { ++version_; }

    Rect bounds() const { return Rect(0, 0, size_.w, size_.h); }

    // 渲染到目标（先按 background 清屏，再画根组）
    void draw(Surface& target) const {
        Canvas canvas(target);
        canvas.clear(background);
        root_.draw(canvas);
    }
    void drawOnto(Canvas& canvas) const { root_.draw(canvas); }

    // 带子树缓存地渲染（0.9.0）。像素必须与 draw() 逐像素相同（tests/test_cache.cpp）。
    // 根组的 opacity<1 或 blend!=SrcOver 时退化成 draw()（根层的合成语义不在缓存范围内）。
    void drawCached(Surface& target, SceneCache& cache) const;

private:
    Size size_;
    Group root_;
    uint64_t version_ = 1;
};

} // namespace bin2d
