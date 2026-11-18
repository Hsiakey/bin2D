// ============================================================
// src/filter.cpp —— 颜色矩阵的逐像素实现
//
// 单像素五步（顺序不能换）：
//   ① 预乘 → 直通：sr = r / a（a==0 时直通无定义，按 0）；
//   ② 直通已在 0..1（矩阵系数也按 0..1 给）；
//   ③ 4×5 矩阵乘；
//   ④ 夹到 0..1（矩阵可能产生负值或 >1）；
//   ⑤ 乘回 alpha 得预乘，四舍五入成 8 位。
//
// 为什么 ① 用**精确比值** r/a 而不是"r*255/a 再取整"：后者引入一次 8 位取整，
// 让"身份附近"的矩阵（比如饱和度 0.999）凭空差 1 级；而精确比值乘回 alpha 的
// 浮点误差 < 1e-6 级，四舍五入必然回到原值 —— tests/test_filter.cpp 拿"交换 R/G"
// 这种非单位矩阵在 65536 个预乘像素上逐位验证了这条性质。
// ============================================================

#include "bin2d/filter.h"

#include <algorithm>

namespace bin2d {

void ApplyColorMatrix(Surface& surface, const ColorMatrix& matrix) {
    if (surface.empty() || matrix.isIdentity()) return;

    // 系数提到局部变量：编译器能把 20 次乘法留在寄存器里，也避免每次解引用
    const float m00 = matrix.m[0][0], m01 = matrix.m[0][1], m02 = matrix.m[0][2],
                m03 = matrix.m[0][3], m04 = matrix.m[0][4];
    const float m10 = matrix.m[1][0], m11 = matrix.m[1][1], m12 = matrix.m[1][2],
                m13 = matrix.m[1][3], m14 = matrix.m[1][4];
    const float m20 = matrix.m[2][0], m21 = matrix.m[2][1], m22 = matrix.m[2][2],
                m23 = matrix.m[2][3], m24 = matrix.m[2][4];
    const float m30 = matrix.m[3][0], m31 = matrix.m[3][1], m32 = matrix.m[3][2],
                m33 = matrix.m[3][3], m34 = matrix.m[3][4];

    const int w = surface.width(), h = surface.height();
    for (int y = 0; y < h; ++y) {
        uint8_t* row = surface.row(y);
        for (int x = 0; x < w; ++x, row += 4) {
            const int a8 = row[3];
            float sr = 0.0f, sg = 0.0f, sb = 0.0f;
            if (a8 != 0) {
                // 反预乘用**精确比值**（r/a），不做 0..255 的中间取整：
                // 中间取整会让 identity 以外的近单位矩阵差 1 级，而精确比值乘回 alpha
                // 的误差 < 1e-6 级，四舍五入必然回到原值（tests/test_filter.cpp 用
                // "交换 R/G"这种非单位矩阵逐位验证这一点）。
                const float inv = 1.0f / static_cast<float>(a8);
                sr = static_cast<float>(row[0]) * inv;
                sg = static_cast<float>(row[1]) * inv;
                sb = static_cast<float>(row[2]) * inv;
            }
            const float sa = static_cast<float>(a8) * (1.0f / 255.0f);

            float out[4];
            out[0] = m00 * sr + m01 * sg + m02 * sb + m03 * sa + m04;
            out[1] = m10 * sr + m11 * sg + m12 * sb + m13 * sa + m14;
            out[2] = m20 * sr + m21 * sg + m22 * sb + m23 * sa + m24;
            out[3] = m30 * sr + m31 * sg + m32 * sb + m33 * sa + m34;
            for (int i = 0; i < 4; ++i) {
                if (out[i] < 0.0f) out[i] = 0.0f;
                if (out[i] > 1.0f) out[i] = 1.0f;
            }

            // 乘回 alpha（预乘），四舍五入。out[3] 是新的 alpha
            const int na = static_cast<int>(out[3] * 255.0f + 0.5f);
            row[3] = static_cast<uint8_t>(na);
            row[0] = static_cast<uint8_t>(out[0] * static_cast<float>(na) + 0.5f);
            row[1] = static_cast<uint8_t>(out[1] * static_cast<float>(na) + 0.5f);
            row[2] = static_cast<uint8_t>(out[2] * static_cast<float>(na) + 0.5f);
        }
    }
}

} // namespace bin2d
