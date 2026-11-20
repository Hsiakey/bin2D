// canvas_gpu.cpp —— canvas_gpu.h 的实现（只在有 binGPU 时进库，见 Makefile 的 GPU_OBJS）
//
// 单独一个 TU 的理由：这一份要 bingpu.h（经由 gpu_exec.h），而 canvas.cpp 那一份
// 必须能在没有 binGPU 的环境里编过 —— 依赖面切在这里，两边都不用宏开关。
#include "bin2d/canvas_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "bin2d/gpu_exec.h"

namespace bin2d {
namespace {

// 多档渐变在 GPU 侧没有对应图元（CPU 光栅是逐像素求值），退化成两端色并只告警一次
void WarnMultiStop(size_t n) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    std::fprintf(stderr,
                 "[bin2d] GPU 执行体只支持两档线性渐变，%zu 档的这一次按两端色画\n", n);
}

} // namespace

void GpuSyncState(GpuExec& gpu, const Transform& xf, BlendMode blend,
                  const Rect& deviceClip) {
    gpu.setTransform(xf);
    gpu.setBlendMode(blend);
    gpu.setClipRect(deviceClip);
}

void GpuFillPaint(GpuExec& gpu, const Rect& r, float radius, float thickness,
                  const Paint& paint) {
    switch (paint.kind) {
        case PaintKind::Solid:
            if (thickness > 0.0f) gpu.strokeRounded(r, radius, thickness, paint.color);
            else if (radius > 0.0f) gpu.fillRounded(r, radius, paint.color);
            else gpu.fillRect(r, paint.color);
            return;
        case PaintKind::Linear: {
            const Color from = paint.stops.empty() ? paint.color : paint.stops.front().color;
            const Color to = paint.stops.empty() ? paint.color : paint.stops.back().color;
            if (paint.stops.size() > 2) WarnMultiStop(paint.stops.size());
            // Paint 的渐变轴是两点；GpuExec 收 CSS 角度。两者的对应关系与
            // Paint::linearAngle 那条构造完全一样（0° 向上、顺时针增），所以这里反解角度。
            const float dx = paint.g1.x - paint.g0.x;
            const float dy = paint.g1.y - paint.g0.y;
            const float angle = std::atan2(dx, -dy) * 57.29577951308232f;
            if (radius > 0.0f) gpu.fillRoundedGradient(r, radius, angle, from, to);
            else gpu.fillGradient(r, angle, from, to);
            return;
        }
        case PaintKind::Image:
            if (paint.image && !paint.image->empty())
                gpu.drawSurface(paint.image->surface(), paint.imageDst, paint.srcUv,
                                paint.tint);
            return;
    }
}

void GpuClearRect(GpuExec& gpu, const Rect& deviceRect, const Color& c) {
    // Replace 语义（连 alpha 一起换）：矩形已经是设备像素，用单位阵画
    gpu.setTransform(Transform::identity());
    gpu.setBlendMode(BlendMode::Replace);
    gpu.setClipRect(deviceRect);
    gpu.fillRect(deviceRect, c);
}

void GpuStrokeRounded(GpuExec& gpu, const Rect& r, float radius, float thickness,
                      const Color& c) {
    // 矩形原样交给执行体：它的 SDF 环带是"从边界向内 thickness"，
    // 正好等于 Canvas 那条 CPU 路径（外框内缩 t/2 + 居中描边 t）的外沿不变语义。
    gpu.strokeRounded(r, radius, thickness, c);
}

bool GpuBackdropBlur(GpuExec& gpu, const Rect& area, float radius, float blurPx,
                     float grain) {
    return gpu.backdropBlur(area, radius, blurPx, grain);
}

void GpuDrawSurface(GpuExec& gpu, const Surface& src, const Rect& dst, const Rect& uv,
                    const Color& tint) {
    gpu.drawSurface(src, dst, uv, tint);
}

void GpuDrawNineSlice(GpuExec& gpu, const Surface& src, const Rect& dst, float l, float t,
                      float r, float b, const Color& tint) {
    gpu.drawNineSlice(src, dst, l, t, r, b, tint);
}

void GpuImageChanged(GpuExec& gpu, const Surface& src) { gpu.imageChanged(src); }

// ---- 外部纹理（见 canvas_gpu.h 的说明）----
uint64_t GpuTexCreate(GpuExec& gpu, int w, int h, bool nearest) {
    return gpu.texCreate(w, h, /*dynamic=*/true, nearest);
}
void GpuTexUpload(GpuExec& gpu, uint64_t tex, const Rect& box, const uint8_t* pixels,
                  uint32_t stride) {
    gpu.texUpload(tex, box, pixels, stride);
}
void GpuTexDestroy(GpuExec& gpu, uint64_t tex) { gpu.texDestroy(tex); }
void GpuDrawTexture(GpuExec& gpu, uint64_t tex, const Rect& dst, const Rect& uv,
                    const Color& tint) {
    gpu.drawTexture(tex, dst, uv, tint);
}

bool GpuPushLayer(GpuExec& gpu, const Rect& localBounds, float alpha) {
    return gpu.pushLayer(localBounds, alpha);
}

void GpuPopLayer(GpuExec& gpu) { gpu.popLayer(); }

} // namespace bin2d
