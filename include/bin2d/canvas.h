#pragma once
// ============================================================
// bin2d/canvas.h —— 即时模式画布（变换栈 / 裁剪栈 / 图层栈 / 混合模式）
//
// 为什么同时提供 Canvas（即时）和 Scene（保留）：
//   宿主有两类需求 —— "每帧按顺序画几笔"（报表、图表、导出一帧）用 Canvas
//   最直接，不需要建节点；"一份内容画很多次、要改一处重绘"（UI 里的图标层、
//   缓存卡片）用 Scene 表达意图更清楚。两者共用同一套光栅核心与图层栈，
//   所以同一份内容用两条路画出来必须**逐像素相同**（tests/test_scene.cpp 就测这个）。
//
// 组透明度为什么必须离屏：
//   逐形状乘 alpha 时，两个半透明形状**重叠的部分**会被乘两次（叠深），
//   而"组整体 50% 透明"的语义是"先把组画成一张不透明图，再整体压 50%"。
//   所以 pushLayer/popLayer 是唯一正确的实现，也是投影/遮罩的基础设施。
//
// 裁剪为什么有"矩形"和"路径"两种：
//   矩形裁剪求交即可（零成本、命中/滚动全靠它）；路径裁剪必须有一张覆盖率图，
//   否则边缘是硬边（形状自己有 AA、裁剪没有，交界处一眼能看出断层）。
//   覆盖率遮罩按**整目标尺寸**分配，只在真的用了路径裁剪时才分配。
// ============================================================

#include <cstdint>
#include <memory>
#include <vector>

#include "bin2d/core.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/raster.h"
#include "bin2d/surface.h"
#include "bin2d/transform.h"

namespace bin2d {

class Canvas {
public:
    explicit Canvas(Surface& target);
    Canvas(Surface& target, const Rect& clip);

    Surface& target() { return *target_; }
    const Surface& target() const { return *target_; }
    Rect bounds() const { return target_ ? target_->bounds() : Rect(); }

    // ---- 状态栈 ----
    void save();
    void restore();
    int saveCount() const { return static_cast<int>(stack_.size()) - 1; }

    // ---- 变换（累积到当前状态；与 SVG 的"写在元素上的 transform"同向）----
    void translate(float tx, float ty);
    void scale(float sx, float sy);
    void rotate(float radians);
    void rotateDeg(float degrees);
    void concat(const Transform& xf);
    void setTransform(const Transform& xf);
    Transform transform() const { return state().xf; }

    // ---- 混合模式 ----
    void setBlendMode(BlendMode mode) { state().blend = mode; }
    BlendMode blendMode() const { return state().blend; }

    // ---- 裁剪 ----
    // 矩形裁剪（局部坐标；内部按当前变换映射成轴对齐包围盒 —— 旋转下的精确裁剪
    // 请用 clipPath，矩形只当"滚动视口/圆角外框"这类近似用）
    void clipRect(const Rect& r);
    // 路径裁剪（按覆盖率：边缘有 AA，与形状同一套语义）
    void clipPath(const Path& path, FillRule rule = FillRule::NonZero);
    // 重置到"整块目标"（滚出裁剪栈用；不进 save/restore 栈）
    void resetClip();

    // ---- 清屏 ----
    // 清成给定颜色（Replace 语义：连 alpha 一起换）。空色 = 清成全透明。
    void clear(const Color& c = Color::trans());

    // ---- 绘制 ----
    void fillRect(const Rect& r, const Color& c);
    void fillRect(const Rect& r, const Paint& paint);
    void fillRounded(const Rect& r, float radius, const Paint& paint);
    void fillRounded(const Rect& r, float radius, const Color& c);
    void fillPath(const Path& path, const Paint& paint,
                  FillRule rule = FillRule::NonZero);
    void fillPath(const Path& path, const Color& c,
                  FillRule rule = FillRule::NonZero);
    void strokePath(const Path& path, const Paint& paint, const StrokeStyle& style);
    void strokePath(const Path& path, const Color& c, float width);
    // 宽度的简写（默认圆角连接、平端帽）
    void strokePath(const Path& path, const Paint& paint, float width) {
        StrokeStyle st;
        st.width = width;
        strokePath(path, paint, st);
    }
    void drawImage(const Image& image, const Rect& dstLocal,
                   const Rect& srcUv = Rect(0, 0, 1, 1),
                   const Color& tint = Color::white(),
                   ImageFilter filter = ImageFilter::Linear);
    void drawSurface(const Surface& surface, const Rect& dstLocal,
                     const Rect& srcUv = Rect(0, 0, 1, 1),
                     const Color& tint = Color::white(),
                     ImageFilter filter = ImageFilter::Linear);
    void drawNineSlice(const Image& image, const Rect& dstLocal,
                       float insetL, float insetT, float insetR, float insetB,
                       const Color& tint = Color::white(),
                       ImageFilter filter = ImageFilter::Linear);
    // 九宫格：边界自动探测（详情见 image.h 的 DetectNineSliceInsets）。
    // 探测每次调用都做一遍（对图标尺寸是几十微秒级）；固定边界还是用上面那个重载，
    // 同一个图反复画时自己缓存结果。
    void drawNineSliceAuto(const Image& image, const Rect& dstLocal,
                           const Color& tint = Color::white(),
                           ImageFilter filter = ImageFilter::Linear);

    // ---- 覆盖率遮罩合成（设备空间，不经过当前变换）----
    // 把一张 0..255 的覆盖率罩按 color 合成到当前目标；受当前裁剪 / 路径遮罩 / 混合模式
    // 约束。投影的第二半（模糊后的 alpha 罩）走这里：1 字节/像素，不进颜色管线。
    void blendCoverageMask(const uint8_t* mask, int w, int h, int dstX, int dstY,
                           const Color& color);

    // ---- 图层 ----
    // 把接下来的绘制收进一张离屏（尺寸 = bounds 映射后的设备矩形），
    // popLayer 时整体按 opacity 合成回上一层。opacity >= 1 时不做离屏（省一次全屏合成）。
    // 返回 false = 尺寸非法（调用方可以忽略，随后 pop 是空操作）。
    bool pushLayer(const Rect& localBounds, float opacity,
                   BlendMode blend = BlendMode::SrcOver);
    void popLayer();
    int layerDepth() const { return static_cast<int>(layers_.size()); }

    // ---- 离屏内容（0.9.0：Scene 的子树缓存用）----
    // 与 pushLayer/popLayer 同一套坐标与裁剪语义，区别只在出口：
    //   pushOffscreen 吃掉接下来的绘制，popOffscreen 把内容**交出来**（不合成）。
    // 调用方拿到 (surface, originX, originY) 之后可以自己决定什么时候、按什么
    // 不透明度/混合模式贴回去（缓存就是"上一帧已经贴过一次了"）。
    bool pushOffscreen(const Rect& localBounds);
    bool popOffscreen(Surface& out, int& originX, int& originY);

    // 把一张已经算好的内容图按"图层合成"语义贴回当前目标：
    // opacity 乘在内容上、blend 是这次合成的模式 —— 与 popLayer 的出口逐位相同
    // （popLayer 内部就调它，所以不存在"两条合成路径慢慢漂移"）。
    void compositeSurface(const Surface& surface, int originX, int originY, float opacity,
                          BlendMode blend);



    // 当前绘制目标上"设备像素矩形"：图层里是层内的局部像素（含原点偏移）
    Rect deviceClip() const { return state().clipRect; }

private:
    struct State {
        Transform xf;
        Rect clipRect;          // 设备空间
        bool hasMask = false;
        std::vector<float> mask;   // 目标尺寸的覆盖率（设备空间）
        BlendMode blend = BlendMode::SrcOver;
    };
    struct Layer {
        Surface surface;
        int originX = 0;
        int originY = 0;
        float opacity = 1.0f;
        BlendMode blend = BlendMode::SrcOver;
        size_t stackMark = 1;   // 入层时的状态栈深度（出层时截断，防止层内漏 restore）
    };

    State& state() { return stack_.back(); }
    const State& state() const { return stack_.back(); }

    // 局部 → 设备的**含图层偏移**变换：层里的绘制落到层内像素，
    // 所以设备坐标要**减掉**层的原点（加上 origin 会让内容整体偏移两倍原点，
    // 表现是"层里什么都画不出来"——踩过）
    Transform deviceTransform() const {
        const Transform& xf = state().xf;
        return Transform(xf.a, xf.b, xf.c, xf.d, xf.e - static_cast<float>(originX_),
                         xf.f - static_cast<float>(originY_));
    }
    RasterTarget makeTarget() const;

    // 整目标尺寸的覆盖率遮罩；没有就返回 nullptr
    const float* maskData() const {
        return state().hasMask ? state().mask.data() : nullptr;
    }

    Surface* target_ = nullptr;
    Surface* baseTarget_ = nullptr;   // 构造时给的那张（图层套图层时出层要回到它或上一层）
    std::vector<State> stack_;
    // 用 unique_ptr 装 Layer：pushLayer 会把 &layers_.back().surface 记成当前目标，
    // vector 扩容搬走 Layer 就成了悬空指针（踩过：套层时第二层画进已释放的对象）
    std::vector<std::unique_ptr<Layer>> layers_;
    int originX_ = 0, originY_ = 0;
};

} // namespace bin2d
