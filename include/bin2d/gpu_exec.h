#pragma once
// ============================================================
// bin2d/gpu_exec.h —— GPU 执行体（layering 第 2 步）
//
// 一份绘制语义、两个执行体：Canvas 的覆盖光栅（raster.h）是"任何地方都能跑"的
// 底线，GpuExec 是"同一套图元交给 binGPU"的那条路。上层画的仍然是 Rect / Color /
// Paint / Transform，像素由 binGPU 出。
//
// 分工（结构照 binUI 的 BinUIGpu，内核换成 bin2d 的类型与语义）：
//   · bin2d 负责**语义**：预乘颜色、局部坐标、变换/裁剪/混合/图层的栈语义、
//     渐变的四角色（LinearGradientCorners 是精确解，不是条带近似）；
//   · binGPU 负责**执行**：设备、交换链、管线、采样器、命令缓冲、解析圆角（SDF）
//     的 AA 数学、每 quad 4 顶点的展开。
//
// 三条颜色口径（错一条整页偏色，理由见 core.h / gpu.cpp 的注释）：
//   1. bin2d 的 Color 是**预乘**；binGPU 的顶点色是**直通**（片元自己乘 srcA）——
//      所以顶点色一律 `c.toArgbStraight()`，这里没有第二个口径；
//   2. 纹理上传用 BGPU_TEXTURE_PREMULTIPLIED（bin2d 的 Surface 本来就是预乘，
//      见 gpu.cpp 的 uploadSurface()：零拷贝直传，不做反预乘往返）；
//   3. 线性渐变用四角直通色 —— 屏幕空间的线性渐变是坐标的仿射函数，而四边形上的
//      线性插值对仿射函数是**精确**的（见 paint.h 的 LinearGradientCorners）。
//
// 回合结构（与 binUI 那一版逐字同构，因为 binGPU 的帧模型就这一种）：
//
//     create(…)
//     beginFrame(clearArgb, clear)   → frame_begin + 开 canvas 的 render pass
//       … 图元 …
//     pushLayer/popLayer             → 在 canvas 与层纹理之间切 pass
//     backdropBlur(…)                → 抓背景 → 降采样 → 分离高斯 → 圆角合成
//     present()                      → canvas 贴到交换链目标（REPLACE/NEAREST/1:1）
//                                      + frame_end
//     readback(out)                  → frame_read → 反预乘成 Surface
//
// **canvas 是持久纹理**：`beginFrame(_, false)` 不清它，上一帧的内容原地保留 ——
// 这就是"局部重绘"（脏矩形）能成立的前提。交换链上的图像则相反：binGPU 不保证
// 保留，所以每帧都是"canvas → 交换链"整张贴一次。
//
// 坐标：所有图元收的是**局部坐标**，由 setTransform 的仿射（bingpu_cmd_set_affine
// 是**屏幕空间**状态，顶点位置不变）落到屏幕上；pushClipRect 收的也是局部矩形，
// 内部按当前变换映射成轴对齐包围盒（旋转下的精确裁剪要路径裁剪，矩形只当滚动
// 视口这类近似用 —— 与 Canvas::clipRect 同一条口径）。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bingpu.h"
#include "bin2d/core.h"
#include "bin2d/paint.h"       // Paint / LinearGradientCorners（渐变的四角色在那里）
#include "bin2d/raster.h"      // BlendMode / ZeroAlphaMatters（混合语义的唯一一份）
#include "bin2d/surface.h"
#include "bin2d/transform.h"

namespace bin2d {

// 句柄都直接用 binGPU 的 uint64；外部描述符不透明。
class GpuExec {
public:
    GpuExec() = default;
    ~GpuExec();
    GpuExec(const GpuExec&) = delete;
    GpuExec& operator=(const GpuExec&) = delete;

    // window == nullptr → HEADLESS（离屏，present() 之后 readback 出像素）
    // 后端不可用时返回 false（lastError 里有原因），调用方据此跳过而不是当失败。
    bool create(void* window, int width, int height,
                BgpuBackendKind backend = BGPU_BACKEND_AUTO, bool vsync = true);
    void destroy();
    bool ready() const;
    int width() const;
    int height() const;
    BgpuBackendKind backend() const;
    const char* backendName() const;
    // 实际开到的 MSAA 档（1 = 没开）。**不是**"想要多少"，而是设备上报的生效值
    // （create 时从 bingpu_swapchain_info().msaa_samples 读回来）。
    // 想要多少由 binGPU 的 bingpu_render_plan 按后端定（VK 4 / GL·D3D11·CPU 1），
    // 这里只负责如实转述，供上层诊断与日志用。
    int msaaSamples() const;
    const std::string& lastError() const;

    // ---- 帧 ----
    // clearArgb: 0xAARRGGBB 直通（A=0 = 清成全透明）。clear==false 时保留上一帧内容
    // （局部重绘要用；canvas 是持久纹理）。
    // beginFrame 只重置"这一帧的批次 / 统计 / 图层栈"；**变换、混合模式、裁剪栈跨帧
    // 保留**（它们是与 Canvas 状态栈同一口径的持久状态，执行体不替调用方清）。
    bool beginFrame(uint32_t clearArgb, bool clear);
    bool present();               // 窗口：把 canvas 贴到交换链并 frame_end；HEADLESS：同上
    bool readback(Surface& out);  // present 之后拿像素（预乘 Surface）

    // ---- 帧节拍（0.22.2）：**knob 在这里，闸门在 binGPU** ----
    //
    // 与 GpuCompositor 的那一组**逐字同构**（见 bin2d/gpu.h 的说明）：这一层只是把
    // "这个画布最多一秒出多少帧"的策略往下传，"现在离下一拍还差多久"往上要。
    // 分家会出第二份节拍逻辑，所以这里一条都没自己算。
    //
    //   · setPacePolicy 要在 create() **之前**调：交换链的垂直同步位是建链时定的，
    //     而"要跑得比刷新快"（unlimited / 240）必须关掉 vsync —— 建完链再改就得重建。
    //   · paceFrameBegin 由外壳在自己的 beginFrame 里调（锚点 = 帧开始，不是 present
    //     开始，否则外壳量到的帧间隔会跟着绘制耗时抖）。
    //   · present() 里**自己会收尾**（锚点没开过就补一次 frame_begin），所以外壳在
    //     present 之后那次 paceFrameEnd 是空操作 —— 见 gpu_exec.cpp 里的守卫。
    bool setPacePolicy(BgpuPacePolicy policy);
    BgpuPacePolicy pacePolicy() const;
    double paceFrameMs() const;          // 0 = 不掐拍
    int64_t paceWaitUs() const;          // 现在离下一拍还差多少微秒；0 = 立刻能画
    void paceFrameBegin();
    void paceFrameEnd(double presentMs);
    bool paceWantsVsync() const;
    int displayRefreshHz() const;        // 0 = 探测不到
    double paceRefreshMs() const;        // 0 = 探测不到
    bool presentPacesFrames() const;     // present 会不会自己掐拍（问交换链）

    // ---- 状态 ----
    void setTransform(const Transform& xf);   // 单位阵时走"轴对齐快路"
    const Transform& transform() const;
    void setBlendMode(BlendMode mode);
    int pushClipRect(const Rect& r);          // 返回进入前深度
    // 绝对设置（**设备像素**矩形，不再过变换）：Canvas 那边自己维护整份状态栈，
    // 交过来的已经是 intersect 好的矩形，这里只需要一个"当前裁剪"。
    void setClipRect(const Rect& deviceRect);
    void popClipTo(int depth);

    // ---- 图元（坐标是**局部坐标**，按当前 transform 落到屏幕）----
    void fillRect(const Rect& r, const Color& c);
    void fillRounded(const Rect& r, float radius, const Color& c);
    void strokeRounded(const Rect& r, float radius, float thickness, const Color& c);
    void fillGradient(const Rect& r, float angleDeg, const Color& from, const Color& to);
    void fillRoundedGradient(const Rect& r, float radius, float angleDeg,
                             const Color& from, const Color& to);
    void drawSurface(const Surface& src, const Rect& dst, const Rect& uv,
                     const Color& tint);
    // 九宫格：切分数学与 src/raster.cpp 的 DrawNineSlice **逐字同一套**
    // （四角保尺寸、四边与中心拉伸），这里只是把 9 块各自发成一个四边形。
    void drawNineSlice(const Surface& src, const Rect& dst, float insetL, float insetT,
                       float insetR, float insetB, const Color& tint);
    void drawQuadUV(const float pos[8], const float uv[8], const Color col[4]);  // 任意四边形

    // ---- 外部纹理（字形图集 / 视频帧；句柄对调用方不透明）----
    // "谁改像素、谁管句柄"：内容边画边变的素材（字形图集每帧往里贴新格子）走这一组，
    // 上传只传改过的那块矩形。为什么不能靠 drawSurface + imageChanged：那条路的缓存
    // 口径是"图片不变"，改一下就要整张重传（1024² = 4 MB），而且整张重传与延迟执行的
    // 命令流有先后关系，缓存里的旧内容会被后续的笔采样到。详见 canvas_gpu.h。
    uint64_t texCreate(int w, int h, bool dynamic, bool nearest);
    void texUpload(uint64_t tex, const Rect& box, const uint8_t* pixels, uint32_t stride);
    void texDestroy(uint64_t tex);   // 延迟到帧尾之后真正释放（见 pendingFree_）
    void drawTexture(uint64_t tex, const Rect& dst, const Rect& uv, const Color& tint);

    // 这张 Surface 的像素被**原地改过**了（图片素材 / 视频帧）。
    // drawSurface 的纹理缓存键是 Surface 的实例身份 + 尺寸，原地改像素不改身份 ⇒
    // 不通知的话 GPU 一直采样**修改前**的那份纹理，表现是"文字画成了上一批字形
    // 的样子/位置"（接线时实测到）。
    // 调用方在写完像素之后调一次；句柄所有权不变（这里只打脏标记）。
    void imageChanged(const Surface& src);

    // ---- 图层（组透明度）与毛玻璃 ----
    bool pushLayer(const Rect& localBounds, float alpha);
    void popLayer();
    bool backdropBlur(const Rect& area, float radius, float blurPx, float grain);

    struct Stats {
        int quads = 0;    // 提交的普通四边形数（每 quad 4 顶点）
        int shapes = 0;   // 提交的解析圆角四边形数
        int draws = 0;    // bingpu_cmd_draw_quads / draw_shapes 的调用次数
        int binds = 0;    // bingpu_cmd_set_texture 次数
        int clips = 0;    // bingpu_cmd_set_scissor 次数
        int layers = 0;   // pushLayer 成功的层数
    };
    Stats stats() const;

private:
    // ============================================================
    // 内部类型
    // ============================================================
    static constexpr size_t kNoBatch = static_cast<size_t>(-1);

    // 一次 draw：顶点流里的连续区间 + 全部状态（状态一变就结批）
    struct Batch {
        uint32_t first = 0;      // 顶点流里的四边形下标（不是顶点下标）
        uint32_t count = 0;
        int sx = 0, sy = 0, sw = 0, sh = 0;   // 设备像素裁剪（flush 时设）
        BgpuTexture tex = 0;                  // 形状批次恒 0（不采样纹理）
        BgpuSampler samp = 0;
        BgpuBlend blend = BGPU_BLEND_PREMULTIPLIED;
        bool shape = false;
        float xf[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};   // set_affine 的参数
    };

    struct LayerTarget {
        BgpuTexture tex = 0;
        bool inUse = false;
    };

    struct LayerFrame {
        int idx = -1;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // 屏幕（设备）矩形
        float alpha = 1.0f;
        BgpuTexture prevTarget = 0;
    };

    struct BlurTex {
        BgpuTexture tex = 0;
        int w = 0;
        int h = 0;
    };

    // 毛玻璃的降采样链：0 级 = 抓下来的背景，1..5 = 逐级 2× 缩小，scratch = 分离高斯
    // 的中转，mask = "模糊结果 + 颗粒，再乘圆角覆盖率"的成品。
    struct BlurChain {
        int w = 0;
        int h = 0;
        BlurTex lv[6];
        BlurTex scratch;
        BlurTex mask;
    };

    struct TexMeta {
        BgpuTexture tex = 0;
        bool nearest = false;
    };

    // drawSurface 的纹理缓存：键是 Surface 的**实例身份**（`Surface::id()`，0.22.3）
    // + 尺寸，不是像素指针 —— 指针会被复用（一张图销毁后下一张常拿到同一段内存），
    // 撞上就会把新图画成旧内容（字形图集换页时实测到）。详见 surface.h 的 id() 说明。
    // dirty = 像素被原地改过：下次用之前**重传进同一张纹理**（不销毁句柄 ——
    // 帧内销毁会被已经录进命令缓冲的那几笔引用着）。
    // mips = 这张纹理带真 mip 链（请求过 BGPU_TEXTURE_MIPMAPS 且设备有 CAP_MIPMAPS）。
    // 缩小采样才需要它；建链在上传时多一条 blit 链 + 一次 submit/fence，所以按需请求
    // （判据见 src/gpu_exec.cpp 的 kMipDownscale）。
    struct ImgEntry {
        uint64_t id = 0;
        int w = 0;
        int h = 0;
        BgpuTexture tex = 0;
        bool dirty = false;
        bool mips = false;
    };

    // 帧内**不许当场销毁**纹理（0.22.3）：命令流是延迟执行的（帧尾才提交），
    // 本帧已经录进去的那几笔还引用着句柄，而 binGPU 的句柄槽会被复用 ——
    // 当场 destroy 之后，先前那批笔会采样到"下一张落进同一个槽的纹理"的内容
    // （实测：字形图集淘汰时整页文字串格、阴影层错位）。
    // 所以销毁统一进这张表，等"上一帧已经 fence 等完"再真正释放：入口是 beginFrame
    // （frame_begin 之后：上一帧要么已经 submit+等 fence，要么那次重新录制已经把它
    // 作废）与 destroy。
    std::vector<BgpuTexture> pendingFree_;
    void drainPendingFree();

    // ============================================================
    // 设备层
    // ============================================================
    BgpuDevice device_ = 0;
    BgpuSwapchain swapchain_ = 0;
    BgpuCmd cmd_ = 0;
    BgpuPipeline pipe2d_[5] = {0, 0, 0, 0, 0};      // 下标 = BgpuBlend 的值
    BgpuPipeline pipeShape_[5] = {0, 0, 0, 0, 0};   // 同上；没有 CAP_SHAPE 时全 0
    bool shapeOk_ = false;
    // 设备声明的两项能力（**能力位**，不是"已经开了"）：mip 链与 MSAA。
    // 拿不到 caps 时两个都当"没有" —— 请求 MIPMAPS/4× 也不会发生任何事，
    // 只是让后端多走一遍降级判断。
    bool mipOk_ = false;
    uint32_t msaaSamples_ = 1;   // 交换链上报的**实际生效**档（1 = 没开）
    BgpuSampler sampLinear_ = 0;
    BgpuSampler sampNearest_ = 0;
    BgpuSampler sampRepeat_ = 0;
    BgpuTexture white_ = 0;      // 1×1 全白（直角矩形 / 渐变四边形）
    BgpuTexture noise_ = 0;      // 64×64 确定性噪声（毛玻璃颗粒）
    int width_ = 0;
    int height_ = 0;
    BgpuBackendKind backend_ = BGPU_BACKEND_AUTO;
    std::string error_;
    // 帧节拍：pacer 的状态全在 binGPU（它知道刷新率 / present mode / present 耗时），
    // 这里只存一份策略与探测到的刷新周期。create() 里 init，destroy() 里复位。
    BgpuPacer pace_{};
    double refreshMs_ = 0.0;   // 建链时探到的刷新周期；0 = 探测不到（按 60 算）

    // ============================================================
    // 帧 / pass
    // ============================================================
    bool inFrame_ = false;
    bool inPass_ = false;
    bool canvasUndefined_ = true;   // 新建 / 改尺寸：第一次必须先清一遍
    BgpuTexture scTarget_ = 0;      // 本帧借来的交换链目标（不许 destroy）
    BgpuTexture passTarget_ = 0;
    int passW_ = 0, passH_ = 0;
    BgpuTexture canvas_ = 0;        // 窗口大小的持久画布
    // 内部 pass（图层合成 / 毛玻璃 / 贴屏）：忽略调用方的变换与裁剪栈，
    // 用整目标裁剪 + 单位阵。它画的四边形不计入 Stats（那是实现成本，不是图元数）。
    bool internalPass_ = false;
    // 顶点已经是**目标像素坐标**（图层合成、毛玻璃合成）：仿射用单位阵，
    // 但裁剪栈照常生效（不能因为"内部产物"就画到视口外面去）。
    bool screenSpace_ = false;

    // ============================================================
    // 当前状态
    // ============================================================
    Transform xf_;
    BlendMode blend_ = BlendMode::SrcOver;
    std::vector<Rect> clipStack_;
    BgpuTexture curTex_ = 0;
    BgpuSampler curSamp_ = 0;

    // ============================================================
    // 顶点流与批次
    // ============================================================
    std::vector<BgpuVertex> vq_;
    size_t vcount_ = 0;
    std::vector<BgpuShapeVertex> sq_;
    size_t scount_ = 0;
    std::vector<Batch> batches_;
    size_t curBatch_ = kNoBatch;
    Stats stats_{};
    bool overflowWarned_ = false;

    // ============================================================
    // 图层 / 毛玻璃 / 纹理
    // ============================================================
    static constexpr int kMaxLayers = 4;
    std::vector<LayerTarget> layerPool_;
    std::vector<LayerFrame> layerStack_;
    BlurChain blur_;
    std::vector<TexMeta> texMeta_;       // texCreate 出来的纹理（含 nearest 采样口径）
    std::vector<ImgEntry> imgCache_;     // drawSurface 的纹理缓存
    static constexpr size_t kMaxImgCache = 32;

    // ============================================================
    // 内部工具（实现见 src/gpu_exec.cpp）
    // ============================================================
    bool ok() const { return device_ != 0 && swapchain_ != 0 && cmd_ != 0; }
    bool drawing() const { return ok() && inPass_ && cmd_ != 0; }
    void fail(const char* what, BgpuResult r);
    void noteError(const char* text);

    BgpuTexture makeTexture(int w, int h, uint32_t flags, const void* data, uint32_t stride);
    // 图元纹理的建/传要**在 pass 外面**做（binGPU 的 texture_create/upload 会重置
    // 当前 pass 的 viewport / scissor / affine，插在 begin_render 之间会让之后的图元
    // 按默认视口落像素）。这两个封装负责 flush + endPass + 传完重新 begin。
    // mips = 建完整 mip 链（只有"会被缩小采样"的图元纹理才要，见 kMipDownscale）。
    BgpuTexture makeImageTexture(int w, int h, const uint8_t* pixels, uint32_t stride,
                                 bool mips);
    bool reuploadImageTexture(BgpuTexture tex, const uint8_t* pixels, uint32_t stride);
    BgpuTexture whiteTexture();
    BgpuTexture ensureNoise();
    // wantMips = 这一笔会把它缩小采样。缓存里的纹理第一次按 1:1 建、后来才被缩着画时
    // 会**升一次级**（重建带 mip 链的纹理，旧句柄挂 pendingFree_）。
    BgpuTexture surfaceTexture(const Surface& src, bool wantMips);
    BgpuSampler samplerFor(BgpuTexture tex) const;
    bool ensureCanvas();
    void resetFrameState();
    void beginPass(BgpuTexture target, int tw, int th, bool clear, uint32_t clearColor);
    void endPass();
    bool blitCanvasToTarget();
    void computeScissor(int& sx, int& sy, int& sw, int& sh) const;
    void activeAffine(float out[6]) const;

    void beginBatch(bool shape);
    void emitQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                  float v1, uint32_t color);
    void emitQuadColors(float x0, float y0, float x1, float y1, float u0, float v0,
                        float u1, float v1, const uint32_t colors[4]);
    void emitQuadUV(const float pos[8], const float uv[8], const uint32_t colors[4]);
    void emitShape(float x, float y, float w, float h, float radius, float thick,
                   uint32_t color);
    void emitShapeColors(float x, float y, float w, float h, float radius, float thick,
                         const uint32_t colors[4]);
    void flushAll();

    int acquireLayer();
    void destroyLayerPool();
    bool ensureBlurChain(int w, int h);
    bool ensureBlurLevel(int index, int w, int h);
    bool ensureBlurSlot(BlurTex& slot, int w, int h);
    bool gaussLevel(int level, float blurPx);
    void destroyBlurChain();
};

} // namespace bin2d
