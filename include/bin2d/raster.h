#pragma once
// ============================================================
// bin2d/raster.h —— 光栅核心（扫描线 + 覆盖率 AA + 合成）
//
// 算法（一句话）：**先算覆盖率，再乘颜料**。
//   · 竖直方向：每个像素行切 8 条子扫描线（`kSubSamples`），每条子扫描线
//     中心取 y，与多边形求交得到若干区间；
//   · 水平方向：区间端点的**小数部分**直接当覆盖率（不做水平子采样 ——
//     水平方向是解析求交，本来就没有量化误差）；
//   · 该行覆盖率 = 8 条子扫描线的水平覆盖率之和 / 8，天然落在 [0,1]；
//   · 最后 `paint(像素中心) × 覆盖率` 再与目标合成。
//
// 误差界：竖直方向 1/8 像素量化（`kSubSamples = 8` 时，边缘位置误差 ≤ 1/16 px，
// 覆盖率误差 ≤ 1/8）；水平方向只有浮点误差（≈1e-6 px）。整像素对齐的矩形
// 因此**精确命中**（覆盖率恰为 1，像素值逐像素可预测），这也是测试里能写
// "逐像素精确值"的前提。斜边会出现最多 1/8 像素的位置量化 —— 表现在像素值上
// 是相邻两行覆盖率差最多 12.5%，肉眼看就是平滑过渡（没有锯齿也没有跳变）。
//
// 为什么不用"超采样后模糊"式的 AA：那是先写色再滤波，颜色会从透明区渗到
// 不透明区（深色底上画浅色形状，边缘会发灰）。覆盖率 AA 的每个像素只混一次，
// 且混的是**同一个颜料在像素中心的值**，不会串色。
//
// 描边为什么走"多边形并集"而不是逐段画：逐段各自混合的话，相邻段在接缝处
// 覆盖率各算一半、混合两次 → 接缝颜色比两侧深一条（老 bug）。这里先把每一段
// 的四边形/圆角/端帽都叠进同一张覆盖率缓冲（取 max 不取和），最后统一混一次。
// ============================================================

#include <cstdint>

#include "bin2d/core.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/surface.h"
#include "bin2d/transform.h"

namespace bin2d {

// ------------------------------------------------------------
// 混合模式（与 bingpu.h 的 BgpuBlend 一一对应；这里是 CPU 侧的定义）
// ------------------------------------------------------------
enum class BlendMode : uint8_t {
    SrcOver = 0,   // src + dst*(1-srcA)     普通半透明
    Replace = 1,   // src                    覆盖（不跟底色混合）
    Erase = 2,     // dst*(1-srcA)           擦除（裁剪补集）
    Keep = 3,      // dst*srcA               保留（遮罩）
    Additive = 4,  // src + dst              叠加（饱和到 255）
};

// 单像素合成。src/dst 都是预乘；src 的 alpha 已经乘过覆盖率。
inline Color BlendPixel(const Color& dst, const Color& src, BlendMode mode) {
    switch (mode) {
    case BlendMode::Replace:
        return src;
    case BlendMode::Additive: {
        auto add = [](uint8_t a, uint8_t b) {
            const unsigned v = static_cast<unsigned>(a) + static_cast<unsigned>(b);
            return static_cast<uint8_t>(v > 255u ? 255u : v);
        };
        return Color(add(dst.r, src.r), add(dst.g, src.g), add(dst.b, src.b),
                     add(dst.a, src.a));
    }
    case BlendMode::Erase: {
        const uint32_t inv = 255u - src.a;
        auto mul = [inv](uint8_t v) {
            return static_cast<uint8_t>((static_cast<uint32_t>(v) * inv + 127u) / 255u);
        };
        return Color(mul(dst.r), mul(dst.g), mul(dst.b), mul(dst.a));
    }
    case BlendMode::Keep: {
        auto mul = [&src](uint8_t v) {
            return static_cast<uint8_t>((static_cast<uint32_t>(v) * src.a + 127u) / 255u);
        };
        return Color(mul(dst.r), mul(dst.g), mul(dst.b), mul(dst.a));
    }
    case BlendMode::SrcOver:
    default: {
        Color out = dst;
        blendOver(out, src);
        return out;
    }
    }
}

// ------------------------------------------------------------
// 描边参数
// ------------------------------------------------------------
struct StrokeStyle {
    float width = 1.0f;
    bool roundCap = false;     // 圆端帽；与 squareCap 互斥（square 优先）
    bool squareCap = false;    // 方端帽：两端各外延半个线宽
    bool roundJoin = true;     // 圆角连接；false = 尖角（miter，超限退化成 bevel）
    float miterLimit = 4.0f;   // SVG 默认 4；只有 roundJoin=false 时有意义
};

// ------------------------------------------------------------
// 绘制目标：表面 + 裁剪矩形 + 可选覆盖率遮罩 + 混合模式
//
// mask 是**设备空间**的覆盖率（长度 = surface->width() * surface->height()，
// 下标 y*w+x），值为 nullptr 表示不裁；路径裁剪（clipPath）就是往这里塞一张
// 覆盖率图 —— 这样裁剪与形状用同一套覆盖率语义，边缘不会出现"形状有 AA、
// 裁剪是硬边"的断层。
// ------------------------------------------------------------
struct RasterTarget {
    Surface* surface = nullptr;
    Rect clip;                     // 设备空间轴对齐裁剪（推荐 = surface->bounds()）
    const float* mask = nullptr;   // 可选覆盖率遮罩
    BlendMode blend = BlendMode::SrcOver;

    bool valid() const { return surface && !surface->empty(); }
};

// ------------------------------------------------------------
// 光栅 API（全部在**设备空间**落像素；几何/颜料在**局部空间**给）
// ------------------------------------------------------------
// 填充路径。tolerancePx 是展平容差（设备像素），内部按变换缩放折成用户单位。
void FillPath(const RasterTarget& t, const Path& path, FillRule rule,
              const Paint& paint, const Transform& xf, float tolerancePx = 0.25f);

// 描边路径：四边形 + 圆角/尖角连接 + 端帽，叠一张覆盖率再统一混合。
void StrokePath(const RasterTarget& t, const Path& path, const Paint& paint,
                const StrokeStyle& style, const Transform& xf,
                float tolerancePx = 0.25f);

// 圆角矩形填充（内部就是 addRoundedRect + FillPath，AA 口径与路径一致）
void FillRoundedRect(const RasterTarget& t, const Rect& r, float radius,
                     const Paint& paint, const Transform& xf);

// 轴对齐矩形填充（整像素边界时覆盖率精确 = 1，测试拿它当基准）
void FillRect(const RasterTarget& t, const Rect& r, const Paint& paint,
              const Transform& xf);

// 线性渐变填充一个矩形（= FillRect + Paint::Linear；保留这个名字是因为
// "只看渐变是否对"的测试/调用点用它更直白）
void FillLinearGradient(const RasterTarget& t, const Rect& r, const Paint& gradient,
                        const Transform& xf);

// 画位图：dstLocal 是图片铺到的矩形（局部坐标），srcUv 是取源图的 UV 子矩形。
// nearest=true 用最近邻（像素图/放大镜），否则双线性。
// 变换是旋转/斜切时按 4×4 超采样算边缘覆盖率（轴对齐时用矩形解析覆盖率，更准）。
void DrawImage(const RasterTarget& t, const Image& image, const Rect& dstLocal,
               const Rect& srcUv, const Color& tint, bool nearest,
               const Transform& xf);

// 九宫格拉伸：把图按 (l,t,r,b) 四边切成 9 块，四角不缩放、四边单向拉伸、中心双向。
// 圆角卡片/气泡/滚动条都靠它 —— 同一个人写的三次九宫格一定会有一处坐标错。
void DrawNineSlice(const RasterTarget& t, const Image& image, const Rect& dstLocal,
                   float insetL, float insetT, float insetR, float insetB,
                   const Color& tint, bool nearest, const Transform& xf);

// 直接画一张 Surface（图层合成 / 投影 / 蒙版预览用；语义同 DrawImage）
void DrawSurface(const RasterTarget& t, const Surface& src, const Rect& dstLocal,
                 const Rect& srcUv, const Color& tint, bool nearest,
                 const Transform& xf);

// 盒式模糊（两次可分离，近似高斯）。radius 是像素半径；0 或负数不动。
// 只模糊当前像素矩形内、且按 srcAlpha 归一化（预乘空间直接模糊即可）。
void Blur(Surface& s, float radius);

// 子采样数（竖直方向）。标准 8：覆盖率误差 ≤ 1/8 像素，成本约 8 次求交/行。
constexpr int kSubSamples = 8;

} // namespace bin2d
