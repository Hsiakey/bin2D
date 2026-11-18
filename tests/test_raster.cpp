// test_raster —— 整像素矩形精确值、圆面积误差、AA 边缘平滑、填充规则差异、描边宽度
#include "bin2d/canvas.h"
#include "bin2d/image.h"
#include "bin2d/raster.h"
#include <algorithm>
#include <vector>
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

// ------------------------------------------------------------
// 缩小预滤波（0.4.0）：8× 缩小一张 2px 棋盘，必须接近"理想盒式平均"，
// 而不是把高频折成摩尔纹（0.3.0 实测差 31.8 级）
// ------------------------------------------------------------
static void TestMinifyPrefilter() {
    Image checker(512, 512);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x)
            checker.surface().set(x, y, ((x / 2) + (y / 2)) % 2 ? Color::white() : Color::black());

    // 理想：8×8 面积平均 → 均匀 50% 灰
    Surface ref(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            double acc = 0.0;
            for (int dy = 0; dy < 8; ++dy)
                for (int dx = 0; dx < 8; ++dx)
                    acc += checker.surface().get(x * 8 + dx, y * 8 + dy).r / 255.0;
            const uint8_t v = static_cast<uint8_t>(acc / 64.0 * 255.0 + 0.5);
            ref.set(x, y, Color(v, v, v, 255));
        }

    Surface out(64, 64);
    Canvas c(out);
    c.clear(Color::black());
    c.drawImage(checker, Rect(0, 0, 64, 64), Rect(0, 0, 1, 1), Color::white(),
                ImageFilter::Linear);

    double sumDiff = 0.0;
    int maxDiff = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int d = std::abs(static_cast<int>(out.get(x, y).r) - ref.get(x, y).r);
            sumDiff += d;
            maxDiff = std::max(maxDiff, d);
        }
    const double meanDiff = sumDiff / (64.0 * 64.0);
    CHECK(meanDiff < 4.0, "raster: 8× 缩小的棋盘与理想盒式平均的平均偏差 < 4 级");
    CHECK(maxDiff <= 8, "raster: 8× 缩小的棋盘不会出现整块翻转（最大偏差 ≤ 8 级）");
    std::printf("  （平均偏差 %.2f 级，最大 %d 级）\n", meanDiff, maxDiff);
    CHECK(checker.mipLevelCount() > 0, "raster: 缩小时真的生成了预缩小副本");

    // 最近邻不参与预滤波：缩小后必须是"每块一个纹素"的硬棋盘（不糊）
    Surface nearOut(64, 64);
    Canvas c2(nearOut);
    c2.clear(Color::black());
    c2.drawImage(checker, Rect(0, 0, 64, 64), Rect(0, 0, 1, 1), Color::white(),
                 ImageFilter::Nearest);
    int extremes = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const uint8_t v = nearOut.get(x, y).r;
            if (v <= 8 || v >= 247) ++extremes;
        }
    CHECK(extremes == 64 * 64, "raster: 最近邻缩小保持硬边（全部像素都是极值）");

    // 1:1 画不受影响：逐像素等于原图
    Image tile(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            tile.surface().set(x, y, Color::straight(static_cast<uint8_t>(x * 8),
                                                     static_cast<uint8_t>(y * 8), 90,
                                                     static_cast<uint8_t>(x * 8)));
    Surface one(32, 32);
    Canvas c3(one);
    c3.drawImage(tile, Rect(0, 0, 32, 32));
    int exact = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const Color a = tile.surface().get(x, y), b = one.get(x, y);
            if (a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a) ++exact;
        }
    CHECK(exact == 32 * 32, "raster: 1:1 贴图逐像素精确（预滤波不介入）");
}

// ------------------------------------------------------------
// 渐变抖动（0.4.0）：低 alpha 渐变在预乘 8 位下只有几级，
// 抖动把量化误差摊到相邻像素 —— 判据是"每个色阶占几个像素"
// ------------------------------------------------------------
static Surface LowAlphaGradient(bool dither) {
    Surface s(256, 64);
    Canvas c(s);
    c.clear(Color::argb(0xFFF0F2F5));
    Paint p = Paint::linearAngle(Rect(0, 0, 256, 64), 90.0f, 0x14C86432, 0x146446C8);
    p.dither = dither;
    c.fillRect(Rect(0, 0, 256, 64), p);
    return s;
}

// 一行里"连续相同值"的最长游程：色带有多宽
static int LongestRun(const Surface& s, int row) {
    int best = 1, run = 1;
    for (int x = 1; x < s.width(); ++x) {
        if (s.get(x, row).r == s.get(x - 1, row).r) {
            ++run;
            best = std::max(best, run);
        } else {
            run = 1;
        }
    }
    return best;
}

static void TestGradientDither() {
    const Surface off = LowAlphaGradient(false);
    const Surface on = LowAlphaGradient(true);
    const int row = 32;
    const int runOff = LongestRun(off, row);
    const int runOn = LongestRun(on, row);
    // 256 px 走完约 9 级 → 不抖动时每级约 28 px 一片；抖动后同一片内会有明暗交替，
    // 游程显著变短（色带被打散成网点）
    CHECK(runOff >= 12, "raster: 不抖动的低 alpha 渐变色带宽（≥ 12 px）");
    CHECK(runOn * 2 <= runOff, "raster: 打开抖动后色带被打散（游程 ≤ 不抖动的一半）");
    std::printf("  （最长同值游程：不抖动 %d px，抖动 %d px）\n", runOff, runOn);

    // 块平均后的"可见级数"：抖动应该明显更多（半级分辨率统计）
    auto blockLevels = [](const Surface& s) {
        std::vector<int> means;
        for (int x0 = 0; x0 + 4 <= s.width(); x0 += 4) {
            double acc = 0.0;
            for (int dx = 0; dx < 4; ++dx) acc += s.get(x0 + dx, 32).r;
            means.push_back(static_cast<int>(acc / 4.0 * 2.0 + 0.5));
        }
        std::sort(means.begin(), means.end());
        means.erase(std::unique(means.begin(), means.end()), means.end());
        return static_cast<int>(means.size());
    };
    const int lvOff = blockLevels(off), lvOn = blockLevels(on);
    CHECK(lvOn > lvOff, "raster: 抖动把低 alpha 渐变的可见级数翻倍");
    std::printf("  （4px 块平均后的级数：不抖动 %d，抖动 %d）\n", lvOff, lvOn);

    // 抖动图样是 8×8、且只由**设备像素**决定：同一个 t 下，第 x 列与第 x+8 列必须逐位相同。
    // 这条钉住"图样跟着屏幕走"（而不是跟着局部坐标漂移 —— 那样画面一动抖点会爬行）。
    {
        const Paint p = Paint::linearAngle(Rect(0, 0, 100, 100), 90.0f, 0x14C86432, 0x146446C8);
        int same = 0, total = 0;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 56; ++x) {
                const Color a = Paint::SampleStopsDithered(p.stops, 0.37f, x, y);
                const Color b = Paint::SampleStopsDithered(p.stops, 0.37f, x + 8, y);
                if (a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a) ++same;
                ++total;
            }
        CHECK(same == total, "raster: 抖动图样周期为 8（每列与右边第 8 列逐位相同）");
    }
}

// ------------------------------------------------------------
// 覆盖率查询（RasterizeCoverage）：导出给"自己有条带/四边形管线"的宿主
//
// 这条测试的要点不是"覆盖率对不对"，而是**它与 FillPath 用的是同一份覆盖率** ——
// 两边一旦各写一套扫描线，边缘就会漂（binUI 迁移时就靠这一条兜住）。
// ------------------------------------------------------------
static void TestCoverageQuery() {
    // ① 整像素矩形：内部 255、外部 0，矩形原点/尺寸对得上
    {
        std::vector<float> xy = {4, 4, 20, 4, 20, 12, 4, 12};
        std::vector<uint32_t> first = {0}, count = {4};
        CoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.contours = 1;
        req.clip = Rect(0, 0, 64, 64);
        CoverageRect cov;
        const bool ok = RasterizeCoverage(req, cov);
        CHECK(ok && cov.w == 16 && cov.h == 8 && cov.x0 == 4 && cov.y0 == 4,
              "coverage: 整像素矩形的覆盖率矩形 = 形状包围盒");
        bool insideFull = true, outsideZero = true;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                const bool in = (x >= 4 && x < 20 && y >= 4 && y < 12);
                if (in) {
                    if (!ok || cov.at(x, y) != 255) insideFull = false;
                } else if (ok && x >= cov.x0 && x < cov.x0 + cov.w && y >= cov.y0 &&
                           y < cov.y0 + cov.h && cov.at(x, y) != 0) {
                    outsideZero = false;
                }
            }
        CHECK(insideFull && outsideZero, "coverage: 整像素矩形内部 255、外部 0");
    }

    // ② 半像素偏移：边缘像素约一半，且与 FillPath 的 alpha 逐像素一致（±1）
    {
        std::vector<float> xy = {10.5f, 10.5f, 30.5f, 10.5f, 30.5f, 26.5f, 10.5f, 26.5f};
        std::vector<uint32_t> first = {0}, count = {4};
        CoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.contours = 1;
        req.clip = Rect(0, 0, 64, 64);
        CoverageRect cov;
        RasterizeCoverage(req, cov);
        const int edge = cov.at(10, 15);   // 左边缘列
        CHECK(edge >= 120 && edge <= 135, "coverage: 半像素偏移的左边缘列 ≈ 50%");

        Surface s(64, 64);
        RasterTarget t = MakeTarget(s);
        Path p;
        p.moveTo(10.5f, 10.5f);
        p.lineTo(30.5f, 10.5f);
        p.lineTo(30.5f, 26.5f);
        p.lineTo(10.5f, 26.5f);
        p.close();
        FillPath(t, p, FillRule::NonZero, Paint::solid(Color::argb(0xFFFFFFFF)),
                 Transform::identity());
        int maxDelta = 0;
        for (int y = cov.y0; y < cov.y0 + cov.h; ++y)
            for (int x = cov.x0; x < cov.x0 + cov.w; ++x)
                maxDelta = std::max(maxDelta, std::abs(int(cov.at(x, y)) - int(s.get(x, y).a)));
        CHECK(maxDelta <= 1, "coverage: 与 FillPath 的覆盖率逐像素一致（≤1 级）");
        std::printf("  （覆盖率 vs FillPath alpha 最大差 %d 级）\n", maxDelta);
    }

    // ③ subSamples=1：只有 0 / 255 两档（硬边），且左边界落在 round(x) 上
    {
        std::vector<float> xy = {10.6f, 10.2f, 30.4f, 10.2f, 30.4f, 26.8f, 10.6f, 26.8f};
        std::vector<uint32_t> first = {0}, count = {4};
        CoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.contours = 1;
        req.clip = Rect(0, 0, 64, 64);
        req.subSamples = 1;
        req.style = CoverageStyle::HardEdge;
        CoverageRect cov;
        RasterizeCoverage(req, cov);
        bool binary = true;
        int leftOn = -1;
        for (int y = cov.y0; y < cov.y0 + cov.h; ++y) {
            const uint8_t* r = cov.row(y);
            for (int x = 0; x < cov.w; ++x) {
                if (r[x] != 0 && r[x] != 255) binary = false;
                if (r[x] == 255 && leftOn < 0) leftOn = cov.x0 + x;
            }
        }
        CHECK(binary, "coverage: HardEdge 风格只产生 0/255（等价 crispEdges）");
        CHECK(leftOn == 11, "coverage: 硬边左边界 = round(10.6) = 11");
    }

    // ④ clip 生效：clip 比形状小 → 只导 clip 内那块
    {
        std::vector<float> xy = {0, 0, 40, 0, 40, 40, 0, 40};
        std::vector<uint32_t> first = {0}, count = {4};
        CoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.contours = 1;
        req.clip = Rect(8, 8, 10, 6);
        CoverageRect cov;
        RasterizeCoverage(req, cov);
        CHECK(cov.x0 == 8 && cov.y0 == 8 && cov.w == 10 && cov.h == 6,
              "coverage: clip 决定导出块的位置与尺寸");
    }
}

// ------------------------------------------------------------
// 描边覆盖率（RasterizeStrokeCoverage，0.6.0）
//
// 三条要点：端帽/连接块的几何真的落在覆盖率里；导出值与 `StrokePath` 的像素**同源**；
// 重叠块取 max（并集）—— 不是相加，所以半透明描边的接缝不会混两次。
// ------------------------------------------------------------
static void TestStrokeCoverage() {
    const Rect clip(0, 0, 64, 64);
    // 导出矩形外的像素一律当 0 读（`at()` 不做越界检查，测"外面是 0"必须自己收口）
    auto covAt = [](const CoverageRect& c, int x, int y) -> int {
        if (c.empty() || x < c.x0 || y < c.y0 || x >= c.x0 + c.w || y >= c.y0 + c.h) return 0;
        return c.at(x, y);
    };

    // ① 平头直线：整像素对齐时内部 255、外部 0，导出矩形就是线的包围盒
    {
        std::vector<float> xy = {10, 20, 30, 20};
        std::vector<uint32_t> first = {0}, count = {2};
        std::vector<uint8_t> closed = {0};
        StrokeCoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.closed = closed.data();
        req.contours = 1;
        req.style.width = 4.0f;
        req.clip = clip;
        CoverageRect cov;
        const bool ok = RasterizeStrokeCoverage(req, cov);
        CHECK(ok, "stroke coverage: 平头直线可导出");
        CHECK(ok && cov.x0 == 10 && cov.y0 == 18 && cov.w == 20 && cov.h == 4,
              "stroke coverage: 导出矩形 = 线的包围盒（y = 20 ± 半线宽）");
        CHECK(ok && cov.at(20, 19) == 255 && cov.at(20, 21) == 255,
              "stroke coverage: 内部整像素 255");
        CHECK(ok && covAt(cov, 9, 19) == 0 && covAt(cov, 30, 19) == 0 &&
                  covAt(cov, 20, 17) == 0,
              "stroke coverage: 平头端外/线外 0");

        // ② 端帽：square 外延半线宽（整像素 255），round 只有亚像素的一小片
        StrokeStyle sq = req.style;
        sq.squareCap = true;
        req.style = sq;
        CoverageRect covSq;
        RasterizeStrokeCoverage(req, covSq);
        CHECK(covSq.x0 == 8 && covSq.w == 24 && covSq.at(8, 19) == 255 &&
                  covSq.at(9, 19) == 255,
              "stroke coverage: square 端帽两端各外延半线宽");

        StrokeStyle rd = req.style;
        rd.squareCap = false;
        rd.roundCap = true;
        req.style = rd;
        CoverageRect covRd;
        RasterizeStrokeCoverage(req, covRd);
        const int tip = covRd.at(31, 19);   // 端点 (30,20) 外 1px
        CHECK(tip > 0 && tip < 255, "stroke coverage: round 端帽只多出亚像素覆盖率");
        CHECK(covAt(covRd, 32, 19) == 0, "stroke coverage: round 端帽不外延到半线宽之外");
        std::printf("  （round 端帽外 1px 处覆盖率 %d/255）\n", tip);
    }

    // ③ 斜接 vs 斜面：(25,5) 那个尖角只有 miter 盖得住，bevel/miter-limit 必须盖不住
    {
        std::vector<float> xy = {0, 10, 20, 10, 20, 30};
        std::vector<uint32_t> first = {0}, count = {3};
        std::vector<uint8_t> closed = {0};
        StrokeCoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.closed = closed.data();
        req.contours = 1;
        req.style.width = 10.0f;
        req.style.roundJoin = false;
        req.style.miterLimit = 4.0f;
        req.clip = clip;
        CoverageRect covMiter;
        RasterizeStrokeCoverage(req, covMiter);
        CHECK(covMiter.at(24, 6) == 255, "stroke coverage: 斜接尖角区域被覆盖");
        req.style.miterLimit = 1.0f;   // 直角比值 √2 > 1 → 规范要求退回斜面
        CoverageRect covBevel;
        RasterizeStrokeCoverage(req, covBevel);
        CHECK(covBevel.at(24, 6) == 0, "stroke coverage: 超 miter-limit 退回斜面（尖角不覆盖）");
        CHECK(covBevel.at(21, 9) == 255, "stroke coverage: 斜面仍然填满拐点内侧");
    }

    // ④ 与 StrokePath 同源：同一几何的覆盖率必须逐像素一致（含端帽/连接块）
    {
        Path p;
        p.moveTo(10, 12);
        p.lineTo(40, 20);
        p.lineTo(20, 40);
        StrokeStyle st;
        st.width = 6.0f;
        st.roundCap = true;
        st.roundJoin = true;
        Surface s(64, 64);
        StrokePath(MakeTarget(s), p, Paint::solid(Color::white()), st, Transform::identity());

        std::vector<float> xy = {10, 12, 40, 20, 20, 40};
        std::vector<uint32_t> first = {0}, count = {3};
        std::vector<uint8_t> closed = {0};
        StrokeCoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.closed = closed.data();
        req.contours = 1;
        req.style = st;
        req.clip = clip;
        CoverageRect cov;
        const bool ok = RasterizeStrokeCoverage(req, cov);
        int maxDelta = 0;
        for (int y = cov.y0; y < cov.y0 + cov.h; ++y)
            for (int x = cov.x0; x < cov.x0 + cov.w; ++x)
                maxDelta = std::max(maxDelta, std::abs(int(cov.at(x, y)) - int(s.get(x, y).a)));
        CHECK(ok && maxDelta <= 1, "stroke coverage: 与 StrokePath 的覆盖率逐像素一致（≤1 级）");
        std::printf("  （描边覆盖率 vs StrokePath alpha 最大差 %d 级）\n", maxDelta);
    }

    // ⑤ 半透明描边的接缝：重叠块取 max（并集）→ 接缝只混一次，不会变深
    {
        Path p;
        p.moveTo(10, 10);
        p.lineTo(40, 10);
        p.lineTo(40, 40);   // 直角 + 圆角连接：连接圆与两段四边形在拐点大面积重叠
        StrokeStyle st;
        st.width = 12.0f;
        st.roundJoin = true;
        Surface s(64, 64);
        RasterTarget t = MakeTarget(s);
        FillRect(t, Rect(0, 0, 64, 64), Paint::solid(Color::argb(0xFFFF0000)),
                 Transform::identity());
        StrokePath(t, p, Paint::solid(Color::argb(0x80FFFFFF)), st, Transform::identity());
        // 拐点内侧（两段都盖到、连接圆也盖到）必须正好 50% 白：混两次会到 ~75%
        const Color c = s.get(36, 14);
        CHECK(c.g >= 126 && c.g <= 130, "stroke coverage: 接缝像素只混一次（并集不是叠加）");
        std::printf("  （拐点接缝像素 = (%d,%d,%d)，白色 50%% 叠在红底上应为 g≈128）\n",
                    int(c.r), int(c.g), int(c.b));
    }

    // ⑥ clip：只导 clip 内那块（位置与尺寸都跟 clip 走）
    {
        std::vector<float> xy = {10, 20, 30, 20};
        std::vector<uint32_t> first = {0}, count = {2};
        std::vector<uint8_t> closed = {0};
        StrokeCoverageRequest req;
        req.xy = xy.data();
        req.first = first.data();
        req.count = count.data();
        req.closed = closed.data();
        req.contours = 1;
        req.style.width = 8.0f;
        req.clip = Rect(14, 14, 6, 6);
        CoverageRect cov;
        RasterizeStrokeCoverage(req, cov);
        CHECK(cov.x0 == 14 && cov.y0 == 16 && cov.w == 6 && cov.h == 4,
              "stroke coverage: clip 决定导出块的位置与尺寸");
    }
}

int main() {
    TestStrokeCoverage();
    TestCoverageQuery();
    TestExactRect();
    TestCircleArea();
    TestFillRule();
    TestStrokeWidth();
    TestMinifyPrefilter();
    TestGradientDither();
    return Bin2dTestSummary("test_raster");
}
