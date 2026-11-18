// test_filter —— 颜色矩阵滤镜：非单位矩阵的逐位往返、六个滤镜的解析期望值、组合律、预乘约束
//
// 断言口径（为什么能写"精确值"）：
//   · 颜色矩阵框架本身不能偷偷改像素：identity 必须逐位不变，而"交换 R/G"这种
//     **非单位**矩阵必须逐位正确（这才真正验证了反预乘/预乘是一对可逆操作）；
//   · 其余滤镜在 alpha=255 的像素上可以按公式算出解析值；
//   · 半透明像素只断言"预乘约束 r<=a"与解析值 ±1，因为 8 位反预乘只保证 1 级精度。
#include "bin2d/filter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "test_util.h"

using namespace bin2d;

namespace {

// 与 ApplyColorMatrix 相同的量化口径（+0.5 截断），用来写"解析期望值"
uint8_t Q(float straight) {
    const float v = straight * 255.0f + 0.5f;
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<uint8_t>(v);
}

uint32_t Pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint32_t Packed(const Surface& s, int x = 0, int y = 0) {
    const Color c = s.get(x, y);
    return Pack(c.r, c.g, c.b, c.a);
}

void CheckSolid(uint32_t argb, const ColorMatrix& m, uint32_t expectArgb, const char* msg) {
    Surface s(1, 1);
    s.set(0, 0, Color::argb(argb));
    ApplyColorMatrix(s, m);
    const uint32_t packed = Packed(s);
    CHECK(packed == expectArgb, msg);
    if (packed != expectArgb) std::printf("  期望 %08X 实得 %08X\n", expectArgb, packed);
}

} // namespace

// ------------------------------------------------------------
// ① identity 不动（快路径），② 非单位矩阵逐位正确（可逆性）
// ------------------------------------------------------------
static void TestIdentityAndSwap() {
    // 一张覆盖所有 (alpha, 分量<=alpha) 组合的图
    Surface s(256, 256);
    for (int a = 0; a < 256; ++a) {
        for (int v = 0; v < 256; ++v) {
            const uint8_t pa = static_cast<uint8_t>(a);
            const uint8_t pv = static_cast<uint8_t>(v <= a ? v : a);
            s.set(v, a, Color(pv, static_cast<uint8_t>(pv / 2), static_cast<uint8_t>(pv / 3), pa));
        }
    }
    Surface copy = s;
    ApplyColorMatrix(s, ColorMatrix::identity());
    int bad = 0;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const Color b = copy.get(x, y), a = s.get(x, y);
            if (b.r != a.r || b.g != a.g || b.b != a.b || b.a != a.a) ++bad;
        }
    CHECK(bad == 0, "filter: identity 逐位不变（65536 个预乘像素）");
    if (bad) std::printf("  不一致 %d 像素\n", bad);

    // 交换 R/G：逐位必须等于原图的 G/R 互换（用 alpha 也是逐位，因为 alpha 行是单位）
    ColorMatrix swap = ColorMatrix::identity();
    swap.m[0][0] = 0.0f;
    swap.m[0][1] = 1.0f;
    swap.m[1][0] = 1.0f;
    swap.m[1][1] = 0.0f;
    Surface t = copy;
    ApplyColorMatrix(t, swap);
    int swappedBad = 0;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const Color b = copy.get(x, y), a = t.get(x, y);
            if (a.r != b.g || a.g != b.r || a.b != b.b || a.a != b.a) ++swappedBad;
        }
    CHECK(swappedBad == 0, "filter: 交换 R/G 逐位正确（证明反预乘/预乘可逆）");
    if (swappedBad) std::printf("  不一致 %d 像素\n", swappedBad);
}

// ------------------------------------------------------------
// 六个滤镜的解析期望（alpha=255 时预乘值就是直通值）
// ------------------------------------------------------------
static void TestNamedFilters() {
    constexpr float R = ColorMatrix::kLumaR, G = ColorMatrix::kLumaG, B = ColorMatrix::kLumaB;

    // 灰度：单色输入 → 三个通道都等于该通道的亮度系数 × 255
    CheckSolid(0xFFFF0000u, ColorMatrix::grayscale(1.0f), Pack(Q(R), Q(R), Q(R)),
               "filter: 灰度(1) 纯红 = 0.213×255");
    CheckSolid(0xFF00FF00u, ColorMatrix::grayscale(1.0f), Pack(Q(G), Q(G), Q(G)),
               "filter: 灰度(1) 纯绿 = 0.715×255");
    CheckSolid(0xFF0000FFu, ColorMatrix::grayscale(1.0f), Pack(Q(B), Q(B), Q(B)),
               "filter: 灰度(1) 纯蓝 = 0.072×255");
    CheckSolid(0xFFFFFFFFu, ColorMatrix::grayscale(1.0f), 0xFFFFFFFFu,
               "filter: 灰度(1) 白色不变（系数和 = 1）");
    CheckSolid(0xFFFF0000u, ColorMatrix::grayscale(0.0f), 0xFFFF0000u,
               "filter: 灰度(0) = 原图");
    // 饱和度 0 与灰度 1 必须是同一个矩阵
    CheckSolid(0xFFFF0000u, ColorMatrix::saturate(0.0f), Pack(Q(R), Q(R), Q(R)),
               "filter: 饱和(0) = 灰度(1)");

    // 亮度是乘性的
    CheckSolid(0xFF804020u, ColorMatrix::brightness(0.5f), Pack(0x40, 0x20, 0x10),
               "filter: 亮度 0.5 逐通道减半");
    CheckSolid(0xFF804020u, ColorMatrix::brightness(0.0f), 0xFF000000u, "filter: 亮度 0 = 黑");
    CheckSolid(0xFF804020u, ColorMatrix::brightness(2.0f), Pack(0xFF, 0x80, 0x40),
               "filter: 亮度 2 未溢出时翻倍");

    // 对比度以 0.5 为轴
    CheckSolid(0xFF804020u, ColorMatrix::contrast(1.0f), 0xFF804020u, "filter: 对比度 1 = 原图");
    CheckSolid(0xFF804020u, ColorMatrix::contrast(0.0f), 0xFF808080u, "filter: 对比度 0 = 中灰");
    {
        // 对比度 2：out = 2x - 0.5，两端夹紧
        const float xs[3] = {0.0f, 0.5f, 1.0f};
        uint8_t got[3];
        for (int i = 0; i < 3; ++i) {
            Surface s(1, 1);
            const uint8_t v = Q(xs[i]);
            s.set(0, 0, Color(v, v, v, 255));
            ApplyColorMatrix(s, ColorMatrix::contrast(2.0f));
            got[i] = s.get(0, 0).r;
        }
        CHECK(got[0] == 0 && got[2] == 255 &&
                  std::abs(static_cast<int>(got[1]) - Q(0.5f)) <= 1,
              "filter: 对比度 2 = 2x-0.5（黑/白夹紧、中灰不动）");
    }

    // 反相：out = (1-2a)x + a
    CheckSolid(0xFF102030u, ColorMatrix::invert(0.0f), 0xFF102030u, "filter: 反相 0 = 原图");
    CheckSolid(0xFF102030u, ColorMatrix::invert(1.0f), 0xFFEFDFCFu, "filter: 反相 1 = 255-x");
    CheckSolid(0xFF000000u, ColorMatrix::invert(0.5f), 0xFF808080u, "filter: 反相 0.5 = 中灰");
    CheckSolid(0xFFFF0000u, ColorMatrix::invert(0.25f), Pack(Q(0.75f), Q(0.25f), Q(0.25f)),
               "filter: 反相 0.25 = 0.25+(1-0.5)x");

    // 饱和度 1 = 原图（矩阵恰为单位阵，走快路径）
    CheckSolid(0xFF804020u, ColorMatrix::saturate(1.0f), 0xFF804020u, "filter: 饱和 1 = 原图");
    CheckSolid(0xFFFF0000u, ColorMatrix::saturate(2.0f), 0xFFFF0000u,
               "filter: 饱和 2 纯红不变（已是最饱和）");

    // 色相 360° = 原图（cos≈1、sin≈-2.4e-16）
    for (uint32_t argb : {0xFF3399CCu, 0xFF204060u, 0xFFCC9933u}) {
        Surface s(1, 1);
        s.set(0, 0, Color::argb(argb));
        ApplyColorMatrix(s, ColorMatrix::hueRotate(360.0f));
        const Color c = s.get(0, 0);
        const Color e = Color::argb(argb);
        CHECK(std::abs(static_cast<int>(c.r) - e.r) <= 1 &&
                  std::abs(static_cast<int>(c.g) - e.g) <= 1 &&
                  std::abs(static_cast<int>(c.b) - e.b) <= 1,
              "filter: 色相 360° ≈ 原图（±1）");
    }
}

// 中性灰在任意色相旋转下不变（SVG hueRotate 矩阵每行系数和为 1，对三通道相等的输入
// 就是恒等 —— 这条性质比"亮度守恒"稳，因为后者会被夹紧破坏）
static void TestNeutralGrayInvariant() {
    for (float deg : {30.0f, 97.0f, 210.0f, 333.0f}) {
        Surface s(1, 1);
        s.set(0, 0, Color(0x80, 0x80, 0x80, 0xFF));
        ApplyColorMatrix(s, ColorMatrix::hueRotate(deg));
        const Color c = s.get(0, 0);
        CHECK(std::abs(static_cast<int>(c.r) - 0x80) <= 1 &&
                  std::abs(static_cast<int>(c.g) - 0x80) <= 1 &&
                  std::abs(static_cast<int>(c.b) - 0x80) <= 1,
              "filter: 中性灰在色相旋转下不变（±1）");
    }
}

// 组合律：concat(a,b) 与"先 b 后 a"逐像素最多差 2 级
// （两次 8 位量化的差被 a 放大：a 是 1.7× 对比度，1 级量化误差 → 1.7 级 → 2 级）
static void TestConcat() {
    Surface s1(64, 64), s2(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const uint32_t argb = 0xFF000000u | (static_cast<uint32_t>(x * 4) << 16) |
                                  (static_cast<uint32_t>(y * 4) << 8) | 0x40u;
            s1.set(x, y, Color::argb(argb));
            s2.set(x, y, Color::argb(argb));
        }
    const ColorMatrix first = ColorMatrix::saturate(0.25f);
    const ColorMatrix second = ColorMatrix::contrast(1.7f);
    ApplyColorMatrix(s1, first);
    ApplyColorMatrix(s1, second);
    ApplyColorMatrix(s2, ColorMatrix::concat(second, first));
    int maxDiff = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const Color p = s1.get(x, y), q = s2.get(x, y);
            maxDiff = std::max<int>({maxDiff, std::abs(p.r - q.r), std::abs(p.g - q.g),
                                     std::abs(p.b - q.b),
                                     std::abs(p.a - q.a)});
        }
    CHECK(maxDiff <= 2, "filter: concat 与两次分别应用最多差 2 级");
    if (maxDiff > 2) std::printf("  最大差 %d\n", maxDiff);
}

// 预乘约束：任何滤镜之后 r/g/b <= a；alpha 不被这六个滤镜改动
static void TestPremulInvariant() {
    Surface s(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            s.set(x, y, Color::argb((static_cast<uint32_t>(8 + x * 7) << 24) | 0xCC8844u));
    const ColorMatrix filters[] = {
        ColorMatrix::grayscale(1.0f), ColorMatrix::saturate(1.6f),
        ColorMatrix::hueRotate(45.0f), ColorMatrix::brightness(1.7f),
        ColorMatrix::contrast(1.4f),   ColorMatrix::invert(0.8f),
    };
    int violations = 0, alphaChanged = 0;
    for (const ColorMatrix& m : filters) {
        Surface t = s;
        ApplyColorMatrix(t, m);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                const Color c = t.get(x, y);
                if (c.r > c.a || c.g > c.a || c.b > c.a) ++violations;
                if (c.a != s.get(x, y).a) ++alphaChanged;
            }
    }
    CHECK(violations == 0, "filter: 六个滤镜后都满足预乘约束 r,g,b <= a");
    CHECK(alphaChanged == 0, "filter: 六个滤镜都不动 alpha");
    if (violations) std::printf("  违反 %d 个像素\n", violations);
}

// 全透明像素：alpha 行没有偏移时仍是全 0（不被点亮、不崩）
static void TestTransparent() {
    Surface s(4, 1);
    ApplyColorMatrix(s, ColorMatrix::invert(1.0f));
    bool allZero = true;
    for (int x = 0; x < 4; ++x) {
        const Color c = s.get(x, 0);
        if (c.r || c.g || c.b || c.a) allZero = false;
    }
    CHECK(allZero, "filter: 全透明像素经过滤镜仍是全 0");
}

int main() {
    TestIdentityAndSwap();
    TestNamedFilters();
    TestNeutralGrayInvariant();
    TestConcat();
    TestPremulInvariant();
    TestTransparent();
    return Bin2dTestSummary("test_filter");
}
