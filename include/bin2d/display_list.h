#pragma once
// ============================================================
// bin2d/display_list.h —— 绘制**语义**的记录：一份列表，两种执行体
//
// 为什么要这个东西（分层那件事的落点）：
//   bin2d 的 `Canvas` 现在是"语义 + 执行"绑在一起的 —— 每条画令立刻落进 Surface 的
//   像素。可 binUI 的那种页面（几百个圆角卡片 + 文字四边形）真正想要的是：
//   **先把这一帧要画的东西按顺序记下来，再让执行体决定怎么画**。
//   于是"圆角半径夹到多少、UV 怎么取、裁剪怎么求交"仍然只有 bin2d 一份（语义），
//   而"拆几块、合不合批、走 CPU 还是显卡"交给执行体（现在两条：
//   `execute(Canvas&)` 走 bin2d 的光栅，`execute(GpuCompositor&, ...)` 走 binGPU）。
//
// 为什么不是"给 Canvas 加一个 GPU 模式"：
//   Canvas 的每条画令都直接操作 Surface（连 clearRect 都分块算），把它拆成
//   "记录 / 执行"两半比在两个执行体之间塞分支干净得多；而且列表是可比较的
//   —— 同一份列表跑两条路，像素差多少、谁快多少，都是数字。
//
// 边界（现在**刻意**只覆盖这些）：
//   · 矩形 / 圆角矩形 / 图像四边形 / 矩形裁剪 / 清屏；
//   · 还没有：路径（要三角化）、渐变（要四角色）、图层组透明度、混合模式、模糊。
//     这些不是"以后也许"，是**下一步**；现在每加一个都要有对应的像素对拍。
// ============================================================

#include <cstdint>
#include <vector>

#include "bin2d/core.h"
#include "bin2d/gpu.h"
#include "bin2d/paint.h"
#include "bin2d/surface.h"

namespace bin2d {

class Canvas;

class DisplayList {
public:
    // 记录时的坐标是**目标像素**（调用方自己把布局坐标映射过来）。
    // 不做变换栈：UI 那份变换（scroll / 缩放）在上层已经摊平了，
    // 这里留着变换栈只会多一份"到底谁在变"的不确定性。
    struct Op {
        enum Kind : uint8_t {
            Clear = 0,    // 清成 color（Replace）
            FillRect = 1,  // color 填充
            FillRounded = 2, // 圆角矩形填充（radius 夹到 min(w,h)/2）
            FillImage = 3, // 贴图（预乘 Surface 当纹理，uv 是归一化子矩形）
            ClipPush = 4, // 压一个矩形裁剪（与当前裁剪求交）
            ClipPop = 5,  // 回到 depth（绝对深度）
        };
        Kind kind = FillRect;
        Rect rect;
        Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
        float radius = 0.0f;
        uint32_t color = 0;          // 0xAARRGGBB 直通（与 binGPU 顶点色同口径）
        const Surface* image = nullptr;
    };

    // 执行体的观察值：合批有没有生效看 draws，纹理换了多少次看 binds。
    struct Stats {
        int ops = 0;        // 真正执行的条目
        int quads = 0;      // 四边形总数
        int draws = 0;      // 提交次数（一次 draw_quads 算一次）
        int binds = 0;      // 纹理绑定次数
        int clips = 0;      // 裁剪状态切换次数
    };

    DisplayList() { ops_.reserve(256); }

    void clear() { ops_.clear(); }
    bool empty() const { return ops_.empty(); }
    size_t size() const { return ops_.size(); }
    const Op& at(size_t i) const { return ops_[i]; }

    // ---- 记录 ----
    void clearRect(const Rect& r, const Color& c);
    void fillRect(const Rect& r, const Color& c);
    void fillRounded(const Rect& r, float radius, const Color& c);
    // 子矩形贴图：uv 是源图的归一化窗口（0..1）
    void drawImage(const Surface& image, const Rect& dst, const Rect& uv = Rect(0, 0, 1, 1));
    // 矩形裁剪：返回压之后的深度；popClipTo(深度) 回到那个深度
    int pushClip(const Rect& r);
    void popClipTo(int depth);
    int clipDepth() const { return clipDepth_; }

    // ---- 执行：CPU（bin2d 光栅）----
    void execute(Canvas& canvas, Stats* stats = nullptr) const;

    // ---- 执行：GPU（binGPU 的渲染目标）----
    // clear 为 0xAARRGGBB（A=0 = 清成全透明）。返回 false 时 lastError 有原因。
    bool execute(GpuCompositor& gpu, BgpuTexture target, uint32_t clearArgb,
                 Stats* stats = nullptr) const;
    const std::string& lastError() const { return error_; }

    // 内部：给 GPU 执行体用的白纹素与覆盖率模板缓存都在 GpuCompositor 里，
    // 所以这里不持有任何 GPU 资源 —— 列表可以跨后端重放。

private:
    std::vector<Op> ops_;
    int clipDepth_ = 0;
    mutable std::string error_;
};

} // namespace bin2d
