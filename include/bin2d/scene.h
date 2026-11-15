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
#include <memory>
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

    // 把当前节点的 transform/裁剪应用到画布上（子类进来第一件事就是调它）
    void applyState(Canvas& canvas) const {
        canvas.concat(transform);
        if (hasClipRect) canvas.clipRect(clipRect);
        if (hasClip && !clip.empty()) canvas.clipPath(clip);
    }
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
    float insetL = 0.0f, insetT = 0.0f, insetR = 0.0f, insetB = 0.0f;

    Rect localBounds() const override { return dst; }

    void draw(Canvas& canvas) const override {
        if (!visible || opacity <= 0.0f || !image || image->empty()) return;
        canvas.save();
        applyState(canvas);
        const float k = opacity > 1.0f ? 1.0f : opacity;
        const Color t = tint * k;
        if (nineSlice)
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
    void clearChildren() { children.clear(); }
    size_t childCount() const { return children.size(); }

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

private:
    Size size_;
    Group root_;
    uint64_t version_ = 1;
};

} // namespace bin2d
