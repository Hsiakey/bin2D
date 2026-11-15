// test_raster —— 整像素矩形精确值、圆面积误差、AA 边缘平滑、填充规则差异、描边宽度
#include "bin2d/raster.h"
#include "test_util.h"

using namespace bin2d;

static RasterTarget MakeTarget(Surface& s) {
    RasterTarget t;
    t.surface = &s;
    t.clip = s.bounds();
    return t;
}

// 覆盖率总和（用 alpha 表示）：面积/宽度这类断言都靠它
static double AlphaSum(const Surface& s) {
    double sum = 0.0;
    for (int y = 0; y < s.height(); ++y) {
        const uint8_t* p = s.row(y);
        for (int x = 0; x < s.width(); ++x, p += 4) sum += static_cast<double>(p[3]) / 255.0;
    }
    return sum;
}

static void TestExactRect() {
    Surface s(64, 64);
    RasterTarget t = MakeTarget(s);
    const Color c = Color::argb(0x80FF0000);   // 50% 红（预乘 = (128,0,0,128)）
    FillRect(t, Rect(8, 8, 16, 16), Paint::solid(c), Transform::identity());

    // 内部必须**逐像素**等于颜料原值（整像素对齐 + 覆盖率 1）
    bool insideExact = true, outsideZero = true;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const Color got = s.get(x, y);
            const bool in = (x >= 8 && x < 24 && y >= 8 && y < 24);
            if (in) {
                if (got.r != c.r || got.g != c.g || got.b != c.b || got.a != c.a)
                    insideExact = false;
            } else if (got.a != 0) {
                outsideZero = false;
            }
        }
    }
    CHECK(insideExact, "raster: 整像素矩形内部逐像素精确等于颜料");
    CHECK(outsideZero, "raster: 矩形之外保持全透明");
    // 覆盖率和 = 面积 × 颜料 alpha（alpha 128/255）
    CHECK_NEAR(AlphaSum(s), 16.0 * 16.0 * 128.0 / 255.0, 1e-6,
               "raster: 整像素矩形覆盖率和 = 面积 × 颜料 alpha");
}

static void TestCircleArea() {
    Surface s(200, 200);
    RasterTarget t = MakeTarget(s);
    Path circle;
    circle.addCircle(100.0f, 100.0f, 40.0f);
    FillPath(t, circle, FillRule::NonZero, Paint::solid(Color::white()),
             Transform::identity());
    const double area = AlphaSum(s);
    const double expect = 3.14159265358979323846 * 40.0 * 40.0;
    const double err = std::fabs(area - expect) / expect;
    CHECK(err < 0.015, "raster: 圆面积误差 < 1.5%");
    std::printf("  （圆面积 %.2f / 期望 %.2f，误差 %.3f%%）\n", area, expect, err * 100.0);

    // AA 直接考**覆盖率准确性**：45° 旋转的方块（面积已知），覆盖率和必须对得上，
    // 并且边缘必须有大量"部分覆盖"像素（不是二值化）。
    // 注意别考"相邻行跳变"：竖直方向只有 8 条子扫描线，一条近乎水平的边缘本来就
    // 会在一行之内从 0 跳到 1（位置误差 ≤1/8 px，覆盖率跳变可以是 100%）。
    Surface s3(200, 200);
    Path square;
    square.addRect(Rect(-50, -50, 100, 100));
    const Transform rot = Transform::translate(100.0f, 100.0f) * Transform::rotateDeg(45.0f);
    FillPath(MakeTarget(s3), square, FillRule::NonZero, Paint::solid(Color::white()), rot);
    const double sqArea = AlphaSum(s3);
    const double sqExpect = 100.0 * 100.0;
    CHECK_NEAR(sqArea, sqExpect, sqExpect * 0.005,
               "raster: 45° 旋转方块覆盖率面积误差 < 0.5%（AA 覆盖准确）");
    int partial = 0;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x) {
            const int a = s3.get(x, y).a;
            if (a > 0 && a < 255) ++partial;
        }
    CHECK(partial > 200, "raster: 边缘存在大量部分覆盖像素（AA 生效，不是二值化）");
    std::printf("  （45° 方块面积 %.2f / 期望 %.2f，半覆盖像素 %d 个）\n", sqArea, sqExpect,
                partial);
}

static void TestFillRule() {
    // 两个同心、同向的圆：nonzero 填满，evenodd 只剩圆环
    Surface sn(200, 200), se(200, 200);
    Path donut;
    donut.addCircle(100, 100, 60);
    donut.addCircle(100, 100, 30);
    FillPath(MakeTarget(sn), donut, FillRule::NonZero, Paint::solid(Color::white()),
             Transform::identity());
    FillPath(MakeTarget(se), donut, FillRule::EvenOdd, Paint::solid(Color::white()),
             Transform::identity());
    const double aN = AlphaSum(sn), aE = AlphaSum(se);
    const double expectN = 3.14159265 * 60.0 * 60.0;
    const double expectE = 3.14159265 * (60.0 * 60.0 - 30.0 * 30.0);
    CHECK_NEAR(aN, expectN, expectN * 0.02, "raster: nonzero 同向双圆 = 实心圆面积");
    CHECK_NEAR(aE, expectE, expectE * 0.02, "raster: evenodd 双圆 = 圆环面积");
    CHECK(sn.get(100, 100).a == 255, "raster: nonzero 圆心是实心");
    CHECK(se.get(100, 100).a == 0, "raster: evenodd 圆心被挖空");

    // 五角星（自交多边形）：外顶点隔一个连一个 —— 中心被绕两圈，
    // nonzero 是实的、evenodd 挖空。这是两种规则**唯一**能一眼看出的差别。
    Path star;
    for (int k = 0; k < 5; ++k) {
        const float ang = (-90.0f + 144.0f * static_cast<float>(k)) *
                          3.14159265358979323846f / 180.0f;
        const float px = 100.0f + std::cos(ang) * 60.0f;
        const float py = 100.0f + std::sin(ang) * 60.0f;
        if (k == 0) star.moveTo(px, py);
        else star.lineTo(px, py);
    }
    star.close();
    Surface s1(200, 200), s2(200, 200);
    FillPath(MakeTarget(s1), star, FillRule::NonZero, Paint::solid(Color::white()),
             Transform::identity());
    FillPath(MakeTarget(s2), star, FillRule::EvenOdd, Paint::solid(Color::white()),
             Transform::identity());
    CHECK(s1.get(100, 100).a > 200, "raster: 五角星 nonzero 填充中心重叠区");
    CHECK(s2.get(100, 100).a < 40, "raster: 五角星 evenodd 挖空中心重叠区");
    std::printf("  （五角星面积 nonzero %.1f / evenodd %.1f）\n", AlphaSum(s1), AlphaSum(s2));
}

static void TestStrokeWidth() {
    Surface s(120, 140);
    RasterTarget t = MakeTarget(s);
    Path line;
    line.moveTo(60, 20).lineTo(60, 120);
    StrokeStyle st;
    st.width = 10.0f;
    st.roundCap = false;
    StrokePath(t, line, Paint::solid(Color::white()), st, Transform::identity());
    const int row = 70;
    double cov = 0.0;
    const uint8_t* p = s.row(row);
    for (int x = 0; x < 120; ++x, p += 4) cov += static_cast<double>(p[3]) / 255.0;
    CHECK_NEAR(cov, 10.0, 0.5, "raster: 描边宽度 = 声明的 10px（±0.5）");
    std::printf("  （实测线宽 %.3f）\n", cov);

    Surface sButt(120, 140), sRound(120, 140);
    StrokePath(MakeTarget(sButt), line, Paint::solid(Color::white()), st,
               Transform::identity());
    StrokeStyle st2 = st;
    st2.roundCap = true;
    StrokePath(MakeTarget(sRound), line, Paint::solid(Color::white()), st2,
               Transform::identity());
    CHECK(AlphaSum(sRound) > AlphaSum(sButt) + 30.0, "raster: 圆端帽比平端帽多出半圆面积");
}

int main() {
    TestExactRect();
    TestCircleArea();
    TestFillRule();
    TestStrokeWidth();
    return Bin2dTestSummary("test_raster");
}
