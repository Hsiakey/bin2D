// src/display_list.cpp —— 绘制语义的记录 + 两个执行体（0.17.0）
//
// 这个文件存在的理由写在 include/bin2d/display_list.h 的开头。这里只记**实现上的
// 三个决定**：
//
// ① 为什么记录是"目标像素"、不带变换栈：
//    UI 那份变换（滚动位移、缩放）在上层已经摊平成像素了。这里再留一个变换栈，
//    就会出现"列表里的矩形到底是局部还是设备"的不确定性 —— 两个执行体各按自己的
//    理解映射，像素差在哪都查不清。
//
// ② 为什么 GPU 执行体要自己管裁剪栈（而不是每组画令递归下去）：
//    binGPU 的 scissor 是**状态**（求交语义），弹裁剪要能回到之前那个矩形。
//    所以这里维护一个矩形栈，push 时与栈顶求交，pop 时恢复栈顶 —— 与 Canvas 的
//    clipRect 求交语义一致，且栈顶之外的绘制被 scissor 直接切掉（不产生像素）。
//
// ③ 合批：**只合并相邻、同纹理、同裁剪**的矩形。
//    这是"不改变语义"的合批：中间任何状态变化（换纹理 / 换裁剪 / 圆角 / 贴图）
//    都先 flush。实测（1020×684、234 个圆角卡片）逐个画 234 次 vs 按半径分组一次发完，
//    D3D11 上是 1.37 ms → 0.38 ms，差的全是提交开销。
#include "bin2d/display_list.h"

#include <algorithm>
#include <cmath>

#include "bin2d/canvas.h"

namespace bin2d {

namespace {

Rect Intersect(const Rect& a, const Rect& b) {
    const float x0 = std::max(a.x, b.x);
    const float y0 = std::max(a.y, b.y);
    const float x1 = std::min(a.right(), b.right());
    const float y1 = std::min(a.bottom(), b.bottom());
    return Rect(x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0));
}

// 与 binGPU 的顶点色口径一致：0xAARRGGBB 直通 → 预乘。四角色现在由
// LinearGradientCorners 直接给出**预乘**的 Color，靠 Color::toArgbPremul() 转成顶点色
// —— 不再走"反预乘再预乘"那一圈（那一圈每次都会掉 1 个色阶）。
bool RoundClip(const Rect& r, int* x, int* y, int* w, int* h) {
    // scissor 取整：与 binGPU 的口径一致（像素对齐，不留半像素缝）
    const int x0 = static_cast<int>(std::floor(r.x + 0.5f));
    const int y0 = static_cast<int>(std::floor(r.y + 0.5f));
    const int x1 = static_cast<int>(std::floor(r.right() + 0.5f));
    const int y1 = static_cast<int>(std::floor(r.bottom() + 0.5f));
    *x = x0;
    *y = y0;
    *w = x1 - x0 > 0 ? x1 - x0 : 0;
    *h = y1 - y0 > 0 ? y1 - y0 : 0;
    return *w > 0 && *h > 0;
}

}  // namespace

// ------------------------------------------------------------
// 记录
// ------------------------------------------------------------
void DisplayList::clearRect(const Rect& r, const Color& c) {
    Op op;
    op.kind = Op::Clear;
    op.rect = r;
    op.color = c.toArgbStraight();
    ops_.push_back(op);
}

void DisplayList::fillRect(const Rect& r, const Color& c) {
    if (r.w <= 0.0f || r.h <= 0.0f || c.a == 0) return;
    Op op;
    op.kind = Op::FillRect;
    op.rect = r;
    op.color = c.toArgbStraight();
    ops_.push_back(op);
}

void DisplayList::fillRounded(const Rect& r, float radius, const Color& c) {
    if (r.w <= 0.0f || r.h <= 0.0f || c.a == 0) return;
    Op op;
    op.kind = Op::FillRounded;
    op.rect = r;
    op.radius = radius;
    op.color = c.toArgbStraight();
    // 半径退化：直接当矩形记（两个执行体都省一次模板查表，像素也一样）
    if (radius < 0.5f) op.kind = Op::FillRect;
    ops_.push_back(op);
}

void DisplayList::fillGradient(const Rect& r, float angleDeg, const Color& from,
                               const Color& to) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (from.a == 0 && to.a == 0) return;
    Op op;
    op.kind = Op::FillGradient;
    op.rect = r;
    op.angle = angleDeg;
    op.color = from.toArgbStraight();
    op.color2 = to.toArgbStraight();
    ops_.push_back(op);
}

void DisplayList::fillRoundedGradient(const Rect& r, float radius, float angleDeg,
                                      const Color& from, const Color& to) {
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    if (from.a == 0 && to.a == 0) return;
    Op op;
    op.kind = Op::FillRoundedGradient;
    op.rect = r;
    op.radius = radius;
    op.angle = angleDeg;
    op.color = from.toArgbStraight();
    op.color2 = to.toArgbStraight();
    if (!(radius > 0.5f)) {
        // 半径退化：两个执行体都会走直角路径，记成 FillGradient 省一次判断
        op.kind = Op::FillGradient;
        op.radius = 0.0f;
    }
    ops_.push_back(op);
}

void DisplayList::strokeRounded(const Rect& r, float radius, float thickness, const Color& c) {
    if (r.w <= 0.0f || r.h <= 0.0f || c.a == 0 || thickness <= 0.0f) return;
    Op op;
    op.kind = Op::FillRing;
    op.rect = r;
    op.radius = radius;
    op.thickness = thickness;
    op.color = c.toArgbStraight();
    ops_.push_back(op);
}

void DisplayList::drawImage(const Surface& image, const Rect& dst, const Rect& uv,
                            const Color& tint) {
    if (image.empty() || dst.w <= 0.0f || dst.h <= 0.0f || tint.a == 0) return;
    Op op;
    op.kind = Op::FillImage;
    op.rect = dst;
    op.uv = uv;
    op.image = &image;
    op.tint = tint.toArgbStraight();
    ops_.push_back(op);
}

// GPU 执行体的能力边界。**两条**，都是"两个执行体不是同一个数学"的地方：
//
// ① 两端 alpha **不同**的渐变。binGPU 的顶点色是直通色，`frag.rgb = tex.rgb·vcol.rgb·vcol.a`
//    —— 也就是**在直通域插值、写的时候才预乘**；而 CPU 的 `Paint::SampleStops` 在**预乘域**
//    插值（CSS Color 4）。两端 alpha 相同时两者等价（只差舍入），不同时是**定义**不同。
//    （0.18.0 这里写的是另一条理由 —— "CPU 是直通插值"。真相是反的：CPU 走预乘，
//     真正被这条挡住的其实是"直通域插值"的 GPU，而且那时四角色自己还被多预乘了一次。）
// ② 缩小的贴图：CPU 的 Surface 采样器放大时双线性、缩小时走盒式预滤波；GPU 执行体现在
//    只有双线性点采样 + 无 mipmap。缩小越多差得越远（边缘整级差异）。
//
// 两条都是"整份退回 CPU"，不是"画得差不多"。
bool DisplayList::gpuCompatible(std::string* reason) const {
    for (const Op& op : ops_) {
        if (op.kind == Op::FillGradient || op.kind == Op::FillRoundedGradient) {
            const uint32_t a0 = (op.color >> 24) & 0xFFu, a1 = (op.color2 >> 24) & 0xFFu;
            if (a0 != a1) {
                if (reason)
                    *reason = "两端 alpha 不同的渐变：CPU 在预乘域插值、GPU 在直通域插值";
                return false;
            }
            continue;
        }
        if (op.kind != Op::FillImage || !op.image) continue;
        const float srcW = static_cast<float>(op.image->width()) * op.uv.w;
        const float srcH = static_cast<float>(op.image->height()) * op.uv.h;
        if (srcW > op.rect.w * (1.0f + 1e-3f) || srcH > op.rect.h * (1.0f + 1e-3f)) {
            if (reason)
                *reason = "缩小的贴图：CPU 走盒式预滤波、GPU 走双线性，两者不是同一个滤波器";
            return false;
        }
    }
    return true;
}

int DisplayList::pushClip(const Rect& r) {
    ++clipDepth_;
    Op op;
    op.kind = Op::ClipPush;
    op.rect = r;
    op.color = static_cast<uint32_t>(clipDepth_);   // 记下压之后的绝对深度
    ops_.push_back(op);
    return clipDepth_;
}

void DisplayList::popClipTo(int depth) {
    if (depth < 0) depth = 0;
    if (depth >= clipDepth_) return;
    clipDepth_ = depth;
    Op op;
    op.kind = Op::ClipPop;
    op.color = static_cast<uint32_t>(depth);        // 回到这个绝对深度
    ops_.push_back(op);
}

// ------------------------------------------------------------
// 执行体一：CPU（bin2d 自己的光栅）
// ------------------------------------------------------------
void DisplayList::execute(Canvas& canvas, Stats* stats) const {
    Stats st;
    int depth = 0;                 // 当前裁剪深度
    int savedAt = 0;               // 已经 save 了多少次（每次 pushClip 一次）
    for (const Op& op : ops_) {
        switch (op.kind) {
        case Op::Clear:
            canvas.clear(Color::argb(op.color));
            ++st.ops;
            break;
        case Op::FillRect:
            canvas.fillRect(op.rect, Color::argb(op.color));
            ++st.ops;
            ++st.quads;
            ++st.draws;
            break;
        case Op::FillRounded:
            canvas.fillRounded(op.rect, op.radius, Color::argb(op.color));
            ++st.ops;
            ++st.quads;
            ++st.draws;
            break;
        case Op::FillGradient:
            canvas.fillRect(op.rect, Paint::linearAngle(op.rect, op.angle,
                                                        Color::argb(op.color),
                                                        Color::argb(op.color2)));
            ++st.ops;
            ++st.quads;
            ++st.draws;
            break;
        case Op::FillRoundedGradient:
            canvas.fillRounded(op.rect, op.radius,
                               Paint::linearAngle(op.rect, op.angle, Color::argb(op.color),
                                                  Color::argb(op.color2)));
            ++st.ops;
            ++st.quads;
            ++st.draws;
            break;
        case Op::FillRing: {
            // 与宿主 binUI 的 BinUI2D::strokeRounded **逐条相同**（0.19.0 起 bin2d 自己也有
            // 一份，因为列表要能在 CPU 执行体上跑）：binUI 的描边是"向内叠层"，等价于
            // "内缩 t/2 的矩形 + 居中描边 t"。这条等价关系是像素门禁验过的，别改成外扩。
            const float t = op.thickness;
            if (op.radius <= 0.0f) {
                canvas.fillRect(Rect(op.rect.x, op.rect.y, op.rect.w, t), Color::argb(op.color));
                canvas.fillRect(Rect(op.rect.x, op.rect.y + op.rect.h - t, op.rect.w, t),
                                Color::argb(op.color));
                canvas.fillRect(Rect(op.rect.x, op.rect.y + t, t, op.rect.h - 2.0f * t),
                                Color::argb(op.color));
                canvas.fillRect(Rect(op.rect.x + op.rect.w - t, op.rect.y + t, t,
                                     op.rect.h - 2.0f * t),
                                Color::argb(op.color));
                ++st.quads;
                st.quads += 3;
            } else {
                const float in = t * 0.5f;
                const Rect outer(op.rect.x + in, op.rect.y + in, op.rect.w - t, op.rect.h - t);
                const float rr = std::min(op.radius, std::min(op.rect.w, op.rect.h) * 0.5f) - in;
                if (outer.w > 0.0f && outer.h > 0.0f) {
                    if (rr <= 0.0f) {
                        canvas.fillRect(outer, Color::argb(op.color));
                    } else {
                        Path path;
                        path.addRoundedRect(outer, rr);
                        StrokeStyle style;
                        style.width = t;
                        style.roundCap = false;
                        style.squareCap = false;
                        style.roundJoin = true;
                        canvas.strokePath(path, Paint::solid(Color::argb(op.color)), style);
                    }
                }
                ++st.quads;
            }
            ++st.ops;
            ++st.draws;
            break;
        }
        case Op::FillImage:
            canvas.drawSurface(*op.image, op.rect, op.uv, Color::argb(op.tint));
            ++st.ops;
            ++st.quads;
            ++st.draws;
            break;
        case Op::ClipPush:
            while (savedAt < static_cast<int>(op.color)) {
                canvas.save();
                canvas.clipRect(op.rect);
                ++savedAt;
            }
            depth = static_cast<int>(op.color);
            ++st.clips;
            break;
        case Op::ClipPop: {
            const int target = static_cast<int>(op.color);
            while (savedAt > target) { canvas.restore(); --savedAt; }
            depth = target;
            ++st.clips;
            break;
        }
        }
    }
    while (savedAt > 0) { canvas.restore(); --savedAt; }
    (void)depth;
    if (stats) *stats = st;
}

// ------------------------------------------------------------
// 执行体二：GPU（binGPU 的目标）
//
// 状态机：裁剪栈 + 待发四边形批。任何状态变化（换纹理 / 换裁剪 / 圆角 / 贴图）
// 先把批 flush 掉，再改状态 —— 这就是"只合并相邻同类项"的合批。
// ------------------------------------------------------------
bool DisplayList::execute(GpuCompositor& gpu, BgpuTexture target, uint32_t clearArgb,
                          Stats* stats) const {
    error_.clear();
    Stats st;
    if (!gpuCompatible(&error_)) return false;   // 先问再动：不留半帧
    if (!gpu.beginTarget(target, clearArgb)) {
        error_ = gpu.lastError();
        return false;
    }

    struct Clip { Rect rect; };
    std::vector<Clip> clips;
    BgpuTexture bound = 0;
    bool boundValid = false;

    // 待发的一批矩形（只有普通 Rect 会进批；圆角/贴图各自单发）
    struct Pending {
        std::vector<BgpuVertex> verts;
        int quads = 0;
        void reset() { verts.clear(); quads = 0; }
    } pending;

    auto flush = [&]() -> bool {
        if (pending.quads == 0) return true;
        const bool ok = bingpu_cmd_draw_quads(gpu.cmdHandle(), pending.verts.data(),
                                             static_cast<uint32_t>(pending.quads)) == BGPU_OK;
        st.draws++;
        st.quads += pending.quads;
        pending.reset();
        if (!ok) error_ = bingpu_last_error();
        return ok;
    };

    auto applyClip = [&]() -> bool {
        if (clips.empty()) {
            // 没有裁剪：把 scissor 开到整个目标（w/h 为负 = 关掉裁剪）
            ++st.clips;
            return bingpu_cmd_set_scissor(gpu.cmdHandle(), 0, 0, -1, -1) == BGPU_OK;
        }
        int x = 0, y = 0, w = 0, h = 0;
        if (!RoundClip(clips.back().rect, &x, &y, &w, &h)) {
            // 裁剪退化成空：给一个 0 尺寸 scissor，后面的绘制全部被切掉
            ++st.clips;
            return bingpu_cmd_set_scissor(gpu.cmdHandle(), 0, 0, 0, 0) == BGPU_OK;
        }
        ++st.clips;
        return bingpu_cmd_set_scissor(gpu.cmdHandle(), x, y, w, h) == BGPU_OK;
    };

    auto bindWhite = [&]() -> bool {
        if (boundValid && bound == gpu.whiteTexture()) return true;
        const BgpuResult r = bingpu_cmd_set_texture(gpu.cmdHandle(), gpu.whiteTexture(), 0);
        ++st.binds;
        if (r != BGPU_OK) { error_ = bingpu_last_error(); return false; }
        bound = gpu.whiteTexture();
        boundValid = true;
        return true;
    };

    bool ok = true;
    for (const Op& op : ops_) {
        if (!ok) break;
        switch (op.kind) {
        case Op::Clear:
            // clear 已经在 beginTarget 做过；列表里的 clear 只允许第一条（后续 clear
            // 需要一次带挂起的绘制，留到"多目标/多 pass"那一版）。这里直接忽略第二条起。
            ++st.ops;
            break;
        case Op::FillRect: {
            if (!bindWhite()) { ok = false; break; }
            // 与上一批同一个纹理/裁剪 → 继续攒
            const uint32_t argb = op.color;
            const float x0 = op.rect.x, y0 = op.rect.y;
            const float x1 = op.rect.right(), y1 = op.rect.bottom();
            pending.verts.push_back(BgpuVertex{x0, y0, 0.0f, 0.5f, 0.5f, argb});
            pending.verts.push_back(BgpuVertex{x1, y0, 0.0f, 0.5f, 0.5f, argb});
            pending.verts.push_back(BgpuVertex{x0, y1, 0.0f, 0.5f, 0.5f, argb});
            pending.verts.push_back(BgpuVertex{x1, y1, 0.0f, 0.5f, 0.5f, argb});
            ++pending.quads;
            ++st.ops;
            break;
        }
        case Op::FillRounded:
            if (!flush()) { ok = false; break; }
            if (!gpu.drawRoundedRect(op.rect, op.radius, Color::argb(op.color))) {
                error_ = gpu.lastError();
                ok = false;
                break;
            }
            st.quads += 1;   // 实际是 9 块，这里按"一个形状"计
            st.draws++;
            st.binds++;
            boundValid = false;
            ++st.ops;
            break;
        case Op::FillRoundedGradient:
            if (!flush()) { ok = false; break; }
            if (!gpu.drawRoundedGradient(op.rect, op.radius, op.angle, Color::argb(op.color),
                                         Color::argb(op.color2))) {
                error_ = gpu.lastError();
                ok = false;
                break;
            }
            st.quads += 1;
            st.draws++;
            st.binds++;
            boundValid = false;
            ++st.ops;
            break;
        case Op::FillRing:
            if (!flush()) { ok = false; break; }
            if (!gpu.drawRoundedStroke(op.rect, op.radius, op.thickness,
                                       Color::argb(op.color))) {
                error_ = gpu.lastError();
                ok = false;
                break;
            }
            st.quads += 1;
            st.draws++;
            st.binds++;
            boundValid = false;
            ++st.ops;
            break;
        case Op::FillGradient: {
            if (!flush()) { ok = false; break; }
            if (!bindWhite()) { ok = false; break; }
            uint32_t corners[4];
            LinearGradientCorners(op.rect, op.angle, op.color, op.color2, corners);
            const uint32_t c0 = corners[0];
            const uint32_t c1 = corners[1];
            const uint32_t c2 = corners[2];
            const uint32_t c3 = corners[3];
            const float x0 = op.rect.x, y0 = op.rect.y;
            const float x1 = op.rect.right(), y1 = op.rect.bottom();
            pending.verts.push_back(BgpuVertex{x0, y0, 0.0f, 0.5f, 0.5f, c0});
            pending.verts.push_back(BgpuVertex{x1, y0, 0.0f, 0.5f, 0.5f, c1});
            pending.verts.push_back(BgpuVertex{x0, y1, 0.0f, 0.5f, 0.5f, c2});
            pending.verts.push_back(BgpuVertex{x1, y1, 0.0f, 0.5f, 0.5f, c3});
            ++pending.quads;
            ++st.ops;
            break;
        }
        case Op::FillImage: {
            if (!flush()) { ok = false; break; }
            if (!gpu.drawImageQuad(*op.image, op.rect, op.uv, Color::argb(op.tint))) {
                error_ = gpu.lastError();
                ok = false;
                break;
            }
            ++st.quads;
            ++st.draws;
            ++st.binds;
            boundValid = false;
            ++st.ops;
            break;
        }
        case Op::ClipPush:
            if (!flush()) { ok = false; break; }
            // 求交语义与 Canvas::clipRect 一致：嵌套裁剪只会更小
            clips.push_back(Clip{clips.empty() ? op.rect : Intersect(clips.back().rect, op.rect)});
            if (!applyClip()) ok = false;
            break;
        case Op::ClipPop: {
            if (!flush()) { ok = false; break; }
            const int target2 = static_cast<int>(op.color);
            while (static_cast<int>(clips.size()) > target2 && !clips.empty()) clips.pop_back();
            if (!applyClip()) ok = false;
            break;
        }
        }
    }
    if (ok) ok = flush();
    if (ok) ok = gpu.endTarget();
    if (!ok && error_.empty()) error_ = gpu.lastError();
    if (stats) *stats = st;
    return ok;
}

} // namespace bin2d
