#pragma once
// ============================================================
// bin2d/filter.h —— 图像滤镜（颜色矩阵：灰度 / 饱和度 / 色相 / 亮度 / 对比度 / 反相）
//
// 为什么用"4×5 颜色矩阵"而不是给每个滤镜写一个循环：
//   ① 这些滤镜在数学上是同一个东西 —— 每个输出通道 = 四个输入通道的线性组合
//      + 一个偏移。矩阵是它们的公共形式，`concat()` 还能把"先降饱和再提亮"
//      折成一个矩阵（一次反预乘/预乘，而不是两次）；
//   ② 每个滤镜单独写循环，就要各自处理"预乘的 r<=a 约束"，四份代码四种边界；
//   ③ 真需要时（比如 feColorMatrix）矩阵本身就是 API，不用再长一组函数。
//
// 为什么矩阵作用在**直通（非预乘）**空间：
//   灰度的 0.213/0.715/0.072 是从 sRGB 亮度定义来的系数，它对"原色"有物理意义；
//   预乘空间里 r<=a，把红通道乘 0.213 之后还得再按 a 归一，否则同一颜色在不同
//   alpha 下灰度值不同（那是错的）。代价是每像素一次反预乘除法 —— 滤镜是
//   "整张图一次"的操作，不在每帧的绘制热路径上，这点开销换来的是公式可读。
//
// 为什么不做色彩空间转换（sRGB ↔ 线性光）：
//   CSS filter 与大多数 UI 工具链都直接在 8 位 sRGB 值上算这些矩阵；转线性光再转
//   回来会让"亮度 0.5"的结果与 CSS 不一致，而且 8 位直通值的反预乘已经引入了
//   ~1/255 的往返误差，再加一次 gamma 往返只会更糊。真要线性光处理，那是 HDR
//   管线的事（bin2d 的格式是预乘 8 位，做不到）。
//
// 已知边界（写在这里，省得将来当 bug 报）：
//   · 矩阵可能产生 r > a 的中间值：一律在**乘回 alpha 之后**夹紧，所以结果仍满足预乘；
//   · alpha=0 的像素直通 RGB 无定义（除法会炸），按 0 处理 —— 若矩阵给 alpha 行加了
//     偏移把它"点亮"，出来的就是纯黑/纯偏移色，这是数学上唯一自洽的结果；
//   · 滤镜是"一次性"的破坏性操作，没有 undo：要保留原图请先拷贝（Image 是值类型，
//     拷贝很便宜，但 mip 缓存会跟着拷，见 image.h 的说明）。
// ============================================================

#include <cmath>
#include <cstdint>

#include "bin2d/core.h"
#include "bin2d/surface.h"

namespace bin2d {

// ------------------------------------------------------------
// 4×5 颜色矩阵
//
// 行 = 输出通道 R,G,B,A；列 = 输入 r,g,b,a 与常数项 1。
//   out[R] = m[0][0]*r + m[0][1]*g + m[0][2]*b + m[0][3]*a + m[0][4]
// 系数按 0..1 的**直通**分量给（不是 0..255）。
// ------------------------------------------------------------
struct ColorMatrix {
    float m[4][5];

    // ---- 基础 ----
    static ColorMatrix identity() {
        ColorMatrix o{};
        for (int i = 0; i < 4; ++i) o.m[i][i] = 1.0f;
        return o;
    }
    static ColorMatrix scaleRgb(float r, float g, float b) {
        ColorMatrix o = identity();
        o.m[0][0] = r;
        o.m[1][1] = g;
        o.m[2][2] = b;
        return o;
    }
    static ColorMatrix offsetRgb(float r, float g, float b) {
        ColorMatrix o = identity();
        o.m[0][4] = r;
        o.m[1][4] = g;
        o.m[2][4] = b;
        return o;
    }

    bool isIdentity() const {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 5; ++j) {
                const float want = (i == j) ? 1.0f : 0.0f;
                if (std::fabs(m[i][j] - want) > 1e-7f) return false;
            }
        return true;
    }

    // ---- 常用滤镜（系数与 CSS filter / SVG feColorMatrix 同一套）----
    // 亮度权重用 0.213 / 0.715 / 0.072（Rec.709 的四舍五入，三者之和 = 1.000）。
    static constexpr float kLumaR = 0.213f;
    static constexpr float kLumaG = 0.715f;
    static constexpr float kLumaB = 0.072f;

    // 0 = 原图，1 = 全灰。中间值按"向单位矩阵插值"给（与 CSS grayscale() 一致）
    static ColorMatrix grayscale(float amount = 1.0f) {
        const ColorMatrix g = saturate(0.0f);
        return lerpToIdentity(g, amount);
    }

    // 0 = 全灰，1 = 原图，>1 = 过饱和（负值按 0 处理）
    static ColorMatrix saturate(float s) {
        if (s < 0.0f) s = 0.0f;
        const float r = kLumaR, g = kLumaG, b = kLumaB;
        ColorMatrix o = identity();
        o.m[0][0] = r + (1.0f - r) * s;
        o.m[0][1] = g - g * s;
        o.m[0][2] = b - b * s;
        o.m[1][0] = r - r * s;
        o.m[1][1] = g + (1.0f - g) * s;
        o.m[1][2] = b - b * s;
        o.m[2][0] = r - r * s;
        o.m[2][1] = g - g * s;
        o.m[2][2] = b + (1.0f - b) * s;
        return o;
    }

    // 色相旋转（SVG feColorMatrix type="hueRotate" 的矩阵，逐位同式）
    static ColorMatrix hueRotate(float degrees) {
        constexpr float kPi = 3.14159265358979323846f;
        const float a = degrees * kPi / 180.0f;
        const float c = std::cos(a), s = std::sin(a);
        const float r = kLumaR, g = kLumaG, b = kLumaB;
        ColorMatrix o = identity();
        o.m[0][0] = r + c * (1.0f - r) - s * r;
        o.m[0][1] = g - c * g - s * g;
        o.m[0][2] = b - c * b + s * (1.0f - b);
        o.m[1][0] = r - c * r + s * 0.143f;
        o.m[1][1] = g + c * (1.0f - g) + s * 0.140f;
        o.m[1][2] = b - c * b - s * 0.283f;
        o.m[2][0] = r - c * r - s * (1.0f - r);
        o.m[2][1] = g - c * g + s * g;
        o.m[2][2] = b + c * (1.0f - b) + s * b;
        return o;
    }

    // 乘性亮度（1 = 原图，0.5 = 减半）。乘性而不是加性：加性会把黑色抬成灰，
    // 与 CSS brightness() 的语义不一致。
    static ColorMatrix brightness(float b) {
        if (b < 0.0f) b = 0.0f;
        return scaleRgb(b, b, b);
    }

    // 对比度：以 0.5 为轴缩放（1 = 原图，0 = 全灰，2 = 对比翻倍）
    static ColorMatrix contrast(float c) {
        ColorMatrix o = scaleRgb(c, c, c);
        const float t = 0.5f - 0.5f * c;
        o.m[0][4] = t;
        o.m[1][4] = t;
        o.m[2][4] = t;
        return o;
    }

    // 反相：amount=0 原图、1 全反（out = (1-2a)x + a）。alpha 不动。
    static ColorMatrix invert(float amount = 1.0f) {
        const float k = 1.0f - 2.0f * amount;
        ColorMatrix o = scaleRgb(k, k, k);
        o.m[0][4] = amount;
        o.m[1][4] = amount;
        o.m[2][4] = amount;
        return o;
    }

    // 组合：out = a(b(x))（**先 b 后 a**）。用于把两三个滤镜折成一次像素遍历。
    static ColorMatrix concat(const ColorMatrix& a, const ColorMatrix& b) {
        ColorMatrix o{};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += a.m[i][k] * b.m[k][j];
                o.m[i][j] = sum;
            }
            float sum = a.m[i][4];
            for (int k = 0; k < 4; ++k) sum += a.m[i][k] * b.m[k][4];
            o.m[i][4] = sum;
        }
        return o;
    }

    // 向单位矩阵插值（amount=0 原图，1 本矩阵）
    static ColorMatrix lerpToIdentity(const ColorMatrix& target, float amount) {
        const ColorMatrix id = identity();
        float t = amount;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        ColorMatrix o{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 5; ++j)
                o.m[i][j] = id.m[i][j] + (target.m[i][j] - id.m[i][j]) * t;
        return o;
    }
};

// ------------------------------------------------------------
// 应用
//
// 原地作用在整张 Surface 上（预乘 RGBA8）。identity 直接返回（逐位不变，不付反预乘）。
// 复杂度 O(w·h)，单线程；1 MP 量级在毫秒级（见 tools/bin2d_quality 的实测数字）。
// ------------------------------------------------------------
void ApplyColorMatrix(Surface& surface, const ColorMatrix& matrix);

} // namespace bin2d
