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

static void TestClipRect() {
    Surface s(50, 50);
    Canvas canvas(s);
    canvas.clear(Color::black());
    canvas.clipRect(Rect(10, 10, 20, 20));
    canvas.fillRect(Rect(0, 0, 50, 50), Color::white());
    CHECK(s.get(15, 15).r == 255, "composite: 矩形裁剪内部");
    CHECK(s.get(5, 5).r == 0, "composite: 矩形裁剪外部");
}

int main() {
    TestBlendModes();
    TestGroupOpacity();
    TestPathClip();
    TestNestedLayers();
    TestClipRect();
    return Bin2dTestSummary("test_composite");
}
