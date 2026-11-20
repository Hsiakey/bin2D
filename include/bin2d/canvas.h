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

class GpuExec;   // 0.22.0：GPU 执行体（include/bin2d/gpu_exec.h）

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
    // 圆角矩形描边（0.22.2）—— **环的解析解只在 GPU 上有**。
    //
    // 存在的理由：UI 里几乎每一条边框都是"圆角矩形环"（焦点圈、卡片边、输入框）。
    // 以前那类调用一律走 `strokePath(addRoundedRect(...))`，而 GPU 执行体对任意路径
    // 描边只有一句"不支持"的告警 —— 挂上 GPU 之后**所有圆角边框会整片消失**。
    // 所以这里把"环"提升成一条原语：
    //   · CPU 执行体：外圆角矩形 + 内圆角矩形组成环（= 外框路径描边），走路径覆盖，
    //     与 strokePath 逐像素同一套 AA；
    //   · GPU 执行体：执行体的圆角 SDF 直接给一圈解析环带，**不是**两条路径相减。
    // 两条路的几何口径相同（外沿 = r 的边界、厚度向内），弧上的 AA 会差几级：
    // 这是"8 子采样扫描线 vs 距离场"的定义差，不是 bug。
    void strokeRounded(const Rect& r, float radius, float thickness, const Color& c);
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

    // 这张 Surface 的像素被**原地改过**了（字形图集往里贴新格子 / 视频帧换内容）。
    // drawSurface 在 GPU 执行体里按"像素指针 + 尺寸"缓存纹理，原地改像素不改指针，
    // 不通知就一直采样旧内容（表现是文字画成了上一批字形）。CPU 执行体没有缓存，
    // 这里是空操作 —— 所以调用方不用判断执行体是哪个。
    void imageChanged(const Surface& surface);

    // ---- 外部纹理（0.22.3）----
    // 给"内容会边画边变、而且每次只改一小块"的素材（字形图集 / 阴影图集 / 视频帧）
    // 一条独立的路：句柄归调用方，上传只传改过的那块矩形。
    //
    // 为什么不做成 Surface 那条路（drawImage + imageChanged）：
    //   · drawSurface 的缓存口径是"图片不变"，改一下就要**整张**重传（字形图集是
    //     1024×1024 = 4 MB，一页文字一帧要改几十个格子）；
    //   · 更糟的是顺序：GPU 的绘制命令延迟到帧尾执行，而图集是边画边往里贴新格子，
    //     "整张重传"与"已经录进命令流的那些笔"之间存在真实的先后关系，缓存里残留
    //     的旧内容会被后续的笔采样到（实测：整页文字串格、阴影层错位）。
    // 这条路上上传是**同步**的（binGPU 提交后等 fence），图集又只追加不改写已有
    // 格子，所以无论先上传还是先录笔，任何一笔的采样结果都一样。
    //
    // 契约：
    //   · 没挂 GPU 执行体时 create 返回 0、其余是空操作 —— CPU 光栅没有"纹理"这个
    //     概念，调用方自己走 Image（Surface）那条路；
    //   · 句柄由调用方拥有：destroyTexture 之前不能丢，也不能在帧内重复 create。
    //     destroy 是**延迟**的（执行体把它们推迟到帧尾之后真正释放），所以同一帧里
    //     先 destroy 再画不会画到别人的内容上。
    //   · 上传的字节序与 Surface 一致（RGBA 预乘）；box 是纹理上的像素矩形。
    uint64_t createTexture(int w, int h, bool nearest = false);
    void uploadTexture(uint64_t tex, const Rect& box, const uint8_t* pixels, uint32_t stride);
    void destroyTexture(uint64_t tex);
    // 画一个纹理四边形：dst 是**局部坐标**（与 drawImage 同一条变换/裁剪语义），
    // uv 是 0..1 的归一化子矩形，tint 是预乘色。
    void drawTexture(uint64_t tex, const Rect& dstLocal, const Rect& srcUv,
                     const Color& tint = Color::white());

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

    // ---- 毛玻璃（backdrop-filter）----
    // area 是**局部坐标**矩形（与 clipRect 同一条近似：按当前变换映射成屏幕包围盒）。
    // 语义：把这块面积的**已经画过的背景**糊掉（Replace，不是叠一层），圆角按 radius
    // 裁成覆盖率，grain 是叠上去的确定性颗粒。
    //
    // 为什么要走 Canvas 而不是让上层自己做：
    //   · GPU 执行体那边的整条链（抓背景 → 2× 降采样 → 分离高斯 → 圆角遮罩）只有它
    //     手上有"当前目标"，上层拿到的是 Surface，抓不到纹理上的背景；
    //   · CPU 执行体**不实现**这条（返回 false）：CPU 侧的毛玻璃是上层的活，bin2d 的
    //     Surface 上没有"已经画过的中间态"这种概念，硬做只会多一份会漂移的实现。
    // 所以返回值是契约的一部分：false = 这次调用什么都没发生，调用方走自己那条路。
    bool backdropBlur(const Rect& area, float radius, float blurPx, float grain);

    // ---- 离屏内容（0.9.0：Scene 的子树缓存用）----
    // 与 pushLayer/popLayer 同一套坐标与裁剪语义，区别只在出口：
    //   pushOffscreen 吃掉接下来的绘制，popOffscreen 把内容**交出来**（不合成）。
    // 调用方拿到 (surface, originX, originY) 之后可以自己决定什么时候、按什么
    // 不透明度/混合模式贴回去（缓存就是"上一帧已经贴过一次了"）。
    //
    // **出口是 Surface** —— 这条只为"真的要拿像素"的调用方存在（子树缓存、导出）。
    // GPU 模式下它恒返回 false（见下面的 pushContentLayer）。
    bool pushOffscreen(const Rect& localBounds);
    bool popOffscreen(Surface& out, int& originX, int& originY);

    // ---- 设备侧离屏内容（0.22.5）----
    // 与 pushOffscreen/popOffscreen **同一套入层语义**（同一个局部→设备映射、同一套
    // 层内裁剪、同一个 4 层上限），但出口是**设备上的句柄**：内容不回到 CPU，
    // 调用方拿着 handle 自己决定怎么贴回去。
    //
    // 为什么需要它："图层模糊 / 外阴影"要的正是"把整层内容当一张图、先在它上面做一次
    // 模糊/上色，再按偏移画回去"。CPU 执行体走 pushOffscreen + Surface；GPU 执行体以前
    // 没有对应物，于是上层只能降级成纯组 alpha（模糊和阴影整片消失）。
    //
    // 契约（三端都按这个写）：
    //   · 只有挂了 GPU 执行体才可能成功；CPU 执行体一律返回 false，调用方据此走
    //     自己的 Surface 那条路 —— 这就是"两个出口"的分工，不是谁替代谁；
    //   · handle 由调用方拥有，**必须** releaseContentLayer 还回来（池子复用，
    //     一帧里几十个图层也不新建纹理）；
    //   · popContentLayer 同时给出内容在**设备像素**空间里的矩形（screen）。
    //     层内层外的坐标口径与 pushLayer 完全一样（层内不改 target_ / 原点）。
    bool pushContentLayer(const Rect& localBounds);
    bool popContentLayer(uint64_t& outHandle, Rect& outScreen);
    // 贴回（内容 × tint，可带模糊）。dst 是**局部坐标**（与 drawTexture 同一条变换
    // 语义，内部映射成设备矩形后按屏幕空间合成），uv 是内容矩形上的归一化子矩形，
    // tint 是预乘色。blurPx > 0 = 先分离高斯（σ = blurPx）再贴回。
    // 合成恒按 SrcOver（与 compositeSurface(…, SrcOver) 同一个出口语义）。
    bool drawContentLayer(uint64_t handle, const Rect& dstLocal, const Rect& uv,
                          const Color& tint, float blurPx, bool nearest = false);
    // 贴回（纯色轮廓）：内容的 alpha 当遮罩填 color，再按 blurPx 模糊。
    // 外阴影要的就是这一条 —— 只借轮廓、不借内容自己的颜色。
    bool fillContentLayer(uint64_t handle, const Rect& dstLocal, const Color& color,
                          float blurPx);
    void releaseContentLayer(uint64_t handle);

    // 把一张已经算好的内容图按"图层合成"语义贴回当前目标：
    // opacity 乘在内容上、blend 是这次合成的模式 —— 与 popLayer 的出口逐位相同
    // （popLayer 内部就调它，所以不存在"两条合成路径慢慢漂移"）。
    void compositeSurface(const Surface& surface, int originX, int originY, float opacity,
                          BlendMode blend);



    // 当前绘制目标上"设备像素矩形"：图层里是层内的局部像素（含原点偏移）
    Rect deviceClip() const { return state().clipRect; }

    // ---- GPU 执行体（0.22.0）----
    //
    // 挂上之后，**所有绘制都交给 GpuExec**（binGPU 的图元），Surface 不再收到像素；
    // 而语义一个都没变：同一个 Canvas API、同一套变换/裁剪栈、同一套图层语义。
    // 这就是 layering 第 2 步："bin2d 的语义 + binGPU 的执行"。
    //
    // 为什么值得：UI 那种一帧几百个图元 + 几个图层的页面，CPU 覆盖光栅实测
    // 9.3~9.8 ms/帧（1020×684 / 1280×800），GPU 执行体 2.7 ms/帧 —— 而其中 2.4 ms
    // 还是上层节点树遍历，**光栅那一段是 20 倍以上**。
    //
    // 边界（0.22.2）：
    //   · target() 只用来回答 bounds()/尺寸 —— 挂上期间它的像素**不会被写**；
    //   · pushOffscreen / popOffscreen 在 GPU 模式下返回 false（那套 API 的出口是
    //     Surface，GPU 侧不该把像素拿回 CPU）；要设备侧的离屏层请用上面那对
    //     pushContentLayer / popContentLayer（0.22.5）；
    //   · 路径（fillPath/clipPath/strokePath 的任意路径）仍然走 CPU 覆盖率——GPU 侧
    //     没有三角化；但**圆角矩形/圆角描边/直线/图片/渐变/毛玻璃**这些 UI 里 99% 的
    //     图元都在 GPU 上（圆角描边是 0.22.2 补的，见 strokeRounded 的注释）。
    bool attachGpu(GpuExec* exec);
    bool gpuAttached() const { return gpu_ != nullptr; }
    GpuExec* gpu() const { return gpu_; }

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

    // GPU 模式：把当前状态（变换 / 裁剪 / 混合）推给执行体，再发图元。
    // **每个绘制入口第一件事就是它**，所以"变换/裁剪栈只有一份实现"这条不变 ——
    // 变的只是最后落到像素的那一步。
    void syncGpu();

    // 整目标尺寸的覆盖率遮罩；没有就返回 nullptr
    const float* maskData() const {
        return state().hasMask ? state().mask.data() : nullptr;
    }

    Surface* target_ = nullptr;
    Surface* baseTarget_ = nullptr;   // 构造时给的那张（图层套图层时出层要回到它或上一层）
    GpuExec* gpu_ = nullptr;          // 0.22.0：非空 = 绘制交给 binGPU 执行体
    std::vector<State> stack_;
    // 用 unique_ptr 装 Layer：pushLayer 会把 &layers_.back().surface 记成当前目标，
    // vector 扩容搬走 Layer 就成了悬空指针（踩过：套层时第二层画进已释放的对象）
    std::vector<std::unique_ptr<Layer>> layers_;
    int originX_ = 0, originY_ = 0;
};

} // namespace bin2d
