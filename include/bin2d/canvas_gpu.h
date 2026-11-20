#pragma once
// ============================================================
// bin2d/canvas_gpu.h —— Canvas 与 GPU 执行体之间的**窄接口**
//
// 为什么不让 canvas.cpp 直接 include gpu_exec.h：
//   gpu_exec.h 要 bingpu.h（C 头，路径在 ../binGPU/include），而 bin2d 必须能在
//   **没有 binGPU 的环境**里编过（Makefile 里那条"只有 libbingpu.a 存在才编 GPU 桥"
//   的约定）。所以这里只前置声明 GpuExec，把"要用执行体做什么"写成几个自由函数，
//   实现放在 gpu_exec.cpp（那一份只在有 binGPU 时才进库）。
//
// 于是 canvas.cpp 的依赖面从"整个 binGPU"缩成"9 个函数声明"，两边都不用加宏开关。
// ============================================================

#include "bin2d/core.h"
#include "bin2d/paint.h"
#include "bin2d/raster.h"   // BlendMode
#include "bin2d/surface.h"
#include "bin2d/transform.h"

namespace bin2d {

class GpuExec;

// 每笔绘制前把 Canvas 的状态推下去（设备变换 / 混合模式 / 已交好的裁剪矩形）
void GpuSyncState(GpuExec& gpu, const Transform& xf, BlendMode blend, const Rect& deviceClip);

// 一次 Paint 落成图元（纯色 / 两档渐变 / 图片铺放）
void GpuFillPaint(GpuExec& gpu, const Rect& r, float radius, float thickness,
                  const Paint& paint);

// 清屏（Replace 语义，矩形是设备像素）
void GpuClearRect(GpuExec& gpu, const Rect& deviceRect, const Color& c);

// 圆角矩形描边（0.22.2）：**环的解析解只在 GPU 上有**。
// 传的是"外框矩形 + 半径 + 厚度"，与 CPU 执行体那条"外圆角 + 内圆角组成环"的路径
// 覆盖同一个视觉结果 —— 但 GPU 侧是 SDF 一圈，不是两条路径的差集，所以弧上的
// AA 会与 CPU 差几级（CPU 是 8 子采样扫描线）。厚度向内，外的边界就是 r。
void GpuStrokeRounded(GpuExec& gpu, const Rect& r, float radius, float thickness,
                      const Color& c);

// 毛玻璃（backdrop-filter）：area 是**局部坐标**矩形，执行体按当前变换映射成屏幕
// 包围盒。返回 false = 执行体这条路上做不了（调用方自己决定退化成什么）。
bool GpuBackdropBlur(GpuExec& gpu, const Rect& area, float radius, float blurPx,
                     float grain);

// 图片 / 九宫格
void GpuDrawSurface(GpuExec& gpu, const Surface& src, const Rect& dst, const Rect& uv,
                    const Color& tint);
void GpuDrawNineSlice(GpuExec& gpu, const Surface& src, const Rect& dst, float l, float t,
                      float r, float b, const Color& tint);

// 这张 Surface 的像素被原地改过了（字形图集 / 阴影图集）：丢掉执行体的纹理缓存。
// 键是"像素指针 + 尺寸"，原地改不改指针 ⇒ 不通知就会一直采样旧内容。
void GpuImageChanged(GpuExec& gpu, const Surface& src);

// 图层（组透明度）
bool GpuPushLayer(GpuExec& gpu, const Rect& localBounds, float alpha);
void GpuPopLayer(GpuExec& gpu);

} // namespace bin2d
