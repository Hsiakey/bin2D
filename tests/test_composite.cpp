// test_composite —— 五种混合模式的精确期望值、组透明度不重复乘、路径裁剪覆盖率、图层套图层
#include "bin2d/canvas.h"
#include "test_util.h"

using namespace bin2d;

static uint32_t Pixel(const Surface& s, int x = 0, int y = 0) {
    const Color c = s.get(x, y);
    return (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.r) << 16) |
           (static_cast<uint32_t>(c.g) << 8) | static_cast<uint32_t>(c.b);
}

static void TestBlendModes() {
    // dst = 不透明蓝 (0,0,255)，src = 半透明红（预乘 r=128,a=128）
    const Color dst = Color::argb(0xFF0000FF);
    const Color src = Color::argb(0x80FF0000);
    struct Case { BlendMode mode; uint32_t expect; const char* name; };
    // 期望值一次算清（预乘、255 为 1）：
    //   SrcOver : r=128, b=255*127/255=127, a=255
    //   Replace : (128,0,0,128)
    //   Erase   : dst*(1-128/255) → b=127, a=127
    //   Keep    : dst*(128/255)   → b=128, a=128
    //   Additive: (128,0,255,255)
    const Case cases[] = {
        {BlendMode::SrcOver, 0xFF80007Fu, "SrcOver"},
        {BlendMode::Replace, 0x80800000u, "Replace"},
        {BlendMode::Erase, 0x7F00007Fu, "Erase"},
        {BlendMode::Keep, 0x80000080u, "Keep"},
        {BlendMode::Additive, 0xFF8000FFu, "Additive"},
    };
    for (const Case& c : cases) {
        Surface s(1, 1);
        s.set(0, 0, dst);
        Canvas canvas(s);
        canvas.setBlendMode(c.mode);
        canvas.fillRect(Rect(0, 0, 1, 1), src);
        const uint32_t got = Pixel(s);
        CHECK(got == c.expect, c.name);
        if (got != c.expect)
            std::printf("  期望 %08X 实得 %08X\n", c.expect, got);
    }
}

static void TestGroupOpacity() {
    // 两个半透明方块在组里重叠：组 opacity=0.5 时，重叠区**不能**比非重叠区更深
    Surface s(100, 100);
    Canvas canvas(s);
    canvas.clear(Color::black());

    const Paint p = Paint::solid(Color::argb(0xFFFFFFFF));
    const bool ok = canvas.pushLayer(Rect(0, 0, 100, 100), 0.5f);
    CHECK(ok, "composite: 图层可以建立");
    canvas.fillRect(Rect(10, 10, 40, 40), FadePaint(p, 0.5f));   // 组内元素自身 50%
    canvas.fillRect(Rect(30, 10, 40, 40), FadePaint(p, 0.5f));   // 与上一个重叠 20px
    canvas.popLayer();

    // 非重叠区 = 白 50% × 组 50% = 25%；重叠区在层内混合成 75% 再 ×50% = 37.5%
    const uint8_t left = s.get(20, 20).r;    // 非重叠（底色不透明，alpha 恒 255，看颜色通道）
    const uint8_t mid = s.get(40, 20).r;     // 重叠
    CHECK_NEAR(static_cast<int>(left), 64, 2, "composite: 组透明度 50% × 元素 50% = 25%");
    CHECK_NEAR(static_cast<int>(mid), 96, 2, "composite: 重叠区只在层内混合一次（37.5%）");

    // 反例（错误实现）：逐个形状乘 alpha 时，重叠区 = 1-(1-0.25)² = 43.75% ≈ 112
    CHECK(mid < 100, "composite: 重叠区没有出现'两次乘 alpha'的暗/亮块");
}

static void TestPathClip() {
    Surface s(120, 120);
    Canvas canvas(s);
    canvas.clear(Color::black());
    canvas.save();
    Path clip;
    clip.addCircle(60.3f, 60.6f, 40.2f);   // 半像素偏置：避免边缘正好压在像素边界上
    canvas.clipPath(clip);
    canvas.fillRect(Rect(0, 0, 120, 120), Color::white());
    canvas.restore();

    CHECK(s.get(60, 60).r == 255, "composite: 路径裁剪内部全不透明");
    CHECK(s.get(5, 5).r == 0, "composite: 路径裁剪外部完全不动（还是底色黑）");
    // 裁剪边缘必须是覆盖率（不是硬边）：统计"半覆盖"像素的数量
    int partial = 0;
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 120; ++x) {
            const int v = s.get(x, y).r;
            if (v > 0 && v < 255) ++partial;
        }
    CHECK(partial > 100, "composite: 路径裁剪边缘是覆盖率（半覆盖像素 > 100）");
    std::printf("  （裁剪边缘半覆盖像素 %d 个）\n", partial);
    // 恢复裁剪后又能画满
    canvas.fillRect(Rect(0, 0, 120, 120), Color::white());
    CHECK(s.get(5, 5).r == 255, "composite: 裁剪在 restore 后恢复");
}

static void TestNestedLayers() {
    Surface s(80, 80);
    Canvas canvas(s);
    canvas.clear(Color::black());
    CHECK(canvas.pushLayer(Rect(0, 0, 80, 80), 0.5f), "composite: 外层图层");
    // 外层只画右下角，保证采样点 (20,20) 在层里本来是空的
    // （否则"半透明白盖在不透明白上"仍然是白的，测不出透明度连乘）
    canvas.fillRect(Rect(50, 50, 20, 20), Color::white());
    CHECK(canvas.pushLayer(Rect(10, 10, 20, 20), 0.5f), "composite: 内层图层（套层）");
    canvas.fillRect(Rect(10, 10, 20, 20), Color::white());
    canvas.popLayer();
    canvas.popLayer();
    // 内层：白 ×0.5 合成到外层 → 50%；外层整体 ×0.5 → 25%
    CHECK_NEAR(static_cast<int>(s.get(20, 20).r), 64, 3,
               "composite: 图层套图层的透明度连乘（50% × 50%）");
    CHECK(s.get(5, 5).r == 0, "composite: 层外区域不受影响");
    // 外层自己那块：只乘一次 50%
    CHECK_NEAR(static_cast<int>(s.get(60, 60).r), 128, 3, "composite: 外层自己的内容只乘一次组透明度");
}

// 组混合模式：pushLayer 的 blend 必须真的生效。
// 权威口径 = "把整块层内容按该模式合到同一个底上"（RasterTarget + DrawSurface），
// 这条以前是坏的：DrawSurface 的 1:1 快路径把混合模式写死成 SrcOver，
// 于是 Replace/Erase/Keep/Additive 在图层合成时全部退化成 SrcOver（0.3.0 修）。
static void TestLayerBlendModes() {
    const BlendMode modes[] = {BlendMode::SrcOver, BlendMode::Replace, BlendMode::Erase,
                               BlendMode::Keep, BlendMode::Additive};
    const char* names[] = {"SrcOver", "Replace", "Erase", "Keep", "Additive"};

    // 层内容：左半不透明、右半半透明、中间带一整列**全透明**孔（零 alpha 的坑就在这里）
    Surface layer(20, 20);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            if (x == 9 || x == 10) continue;                       // 全透明孔
            const uint8_t a = (x < 9) ? 255 : 110;
            layer.set(x, y, Color::straight(static_cast<uint8_t>(40 + x * 8),
                                           static_cast<uint8_t>(200 - y * 5), 90, a));
        }
    }

    for (int i = 0; i < 5; ++i) {
        const Color bg = Color::argb(0xFF204060);
        // ① 权威：直接按模式合成
        Surface direct(40, 40);
        direct.clear(bg);
        {
            RasterTarget rt;
            rt.surface = &direct;
            rt.clip = direct.bounds();
            rt.blend = modes[i];
            DrawSurface(rt, layer, Rect(10, 10, 20, 20), Rect(0, 0, 1, 1), Color::white(),
                        false, Transform::identity());
        }
        // ② 经 Canvas 图层（popLayer 的合成路径）
        Surface via(40, 40);
        via.clear(bg);
        {
            Canvas c(via);
            if (c.pushLayer(Rect(10, 10, 20, 20), 1.0f, modes[i])) {
                c.drawSurface(layer, Rect(10, 10, 20, 20));
                c.popLayer();
            }
        }
        int diff = 0;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const Color a = direct.get(x, y), b = via.get(x, y);
                diff = std::max(diff, std::abs(a.r - b.r));
                diff = std::max(diff, std::abs(a.g - b.g));
                diff = std::max(diff, std::abs(a.b - b.b));
                diff = std::max(diff, std::abs(a.a - b.a));
            }
        char msg[128];
        std::snprintf(msg, sizeof(msg), "composite: 组混合模式 %s 与直接合成逐像素相同", names[i]);
        CHECK(diff == 0, msg);
        if (diff != 0) std::printf("  最大通道差 %d\n", diff);
    }

    // 零 alpha 的两种模式必须**落笔**（把底清成透明），其余三种是真·空操作
    for (int i = 0; i < 5; ++i) {
        Surface s(4, 4);
        s.clear(Color::argb(0xFF204060));
        RasterTarget rt;
        rt.surface = &s;
        rt.clip = s.bounds();
        rt.blend = modes[i];
        DrawSurface(rt, Surface(2, 2), Rect(0, 0, 2, 2), Rect(0, 0, 1, 1), Color::white(),
                    false, Transform::identity());
        const bool shouldClear = (modes[i] == BlendMode::Replace || modes[i] == BlendMode::Keep);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "composite: 全透明层在 %s 下%s", names[i],
                      shouldClear ? "清空目标" : "不动目标");
        if (shouldClear)
            CHECK(s.get(0, 0).a == 0 && s.get(1, 1).a == 0, msg);
        else
            CHECK(s.get(0, 0).a == 255, msg);
    }
}

static void TestClipRect() {    Surface s(50, 50);
    Canvas canvas(s);
    canvas.clear(Color::black());
    canvas.clipRect(Rect(10, 10, 20, 20));
    canvas.fillRect(Rect(0, 0, 50, 50), Color::white());
    CHECK(s.get(15, 15).r == 255, "composite: 矩形裁剪内部");
    CHECK(s.get(5, 5).r == 0, "composite: 矩形裁剪外部");
}

// 层内矩形裁剪（0.10.1 修）：clipRect 一度用裸 state().xf 映射，而落像素用
// deviceTransform()（xf 再减层原点）。在图层里两者差一个 origin，于是"层内第一次
// clipRect 就把内容裁成 0×0"，整层空白。上面 TestClipRect 在根层测，测不出来。
static void TestClipRectInsideLayer() {
    Surface s(100, 100);
    Canvas canvas(s);
    canvas.clear(Color::black());

    const bool ok = canvas.pushLayer(Rect(20, 20, 60, 60), 1.0f);
    CHECK(ok, "composite: 层内裁剪 —— 图层可以建立");
    // 层内传的坐标是**父级设备坐标**（落像素时内部再减掉层原点 (20,20)）——
    // 这也是 binUI 那类"传屏幕坐标"的调用方依赖的口径。
    canvas.clipRect(Rect(20, 20, 60, 60));
    canvas.fillRect(Rect(20, 20, 60, 60), Color::white());
    canvas.popLayer();

    CHECK(s.get(50, 50).r == 255, "composite: 层内 clipRect 不吞掉内容（整层可见）");
    CHECK(s.get(25, 25).r == 255, "composite: 层内 clipRect 保留左上角");
    CHECK(s.get(75, 75).r == 255, "composite: 层内 clipRect 保留右下角");
    CHECK(s.get(10, 10).r == 0, "composite: 层外坐标仍然没被画到");
}

int main() {
    TestBlendModes();
    TestGroupOpacity();
    TestPathClip();
    TestNestedLayers();
    TestLayerBlendModes();
    TestClipRect();
    TestClipRectInsideLayer();
    return Bin2dTestSummary("test_composite");
}
