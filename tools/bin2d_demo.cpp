// ============================================================
// tools/bin2d_demo.cpp —— 能力展示图（660×470）
//
// 这张图是"看图验收"用的，所以每一块都对应 bin2d 的一条能力：
//   1 背景纵向渐变        → Paint::linearAngle
//   2 圆角渐变卡片 + 投影  → addRoundedRect / Shadow / Blur / 离屏
//   3 星形 + 圆环 + 贝塞尔 → Path（曲线细分）/ 描边
//   4 组透明度叠色        → pushLayer 组语义（重叠不叠深）
//   5 圆形路径裁剪        → clipPath（覆盖率裁剪）
//   6 五种混合模式条带    → BlendMode
//   7 程序生成图片九宫格   → Image + DrawNineSlice
//
// 输出目录由命令行给（Makefile 传 build-mingw/out/bin2d-demo.png）。
// ============================================================

#include <cmath>
#include <cstdio>
#include <string>

#include "bin2d/canvas.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/png.h"
#include "bin2d/scene.h"
#include "bin2d/surface.h"

using namespace bin2d;

namespace {

// 程序生成一枚"圆角芯片"素材（8 的倍数、四角圆、内部有高光）——九宫格演示用
Image MakeChipImage() {
    return Image::generate(64, 64, [](int x, int y) {
        const float fx = static_cast<float>(x) + 0.5f;
        const float fy = static_cast<float>(y) + 0.5f;
        const float r = 16.0f;
        // 圆角矩形（距离场的近似）：四角只算角落那块
        float cx = fx, cy = fy;
        if (cx < r) cx = r;
        if (cx > 64.0f - r) cx = 64.0f - r;
        if (cy < r) cy = r;
        if (cy > 64.0f - r) cy = 64.0f - r;
        const float d = std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
        if (d > r) return Color::trans();
        const float edge = std::max(0.0f, std::min(1.0f, r - d));   // 1px 抗锯齿
        const float t = fy / 64.0f;
        const float glow = 0.55f + 0.45f * std::max(0.0f, 1.0f - std::fabs(fx - 20.0f) / 40.0f);
        const uint8_t rr = static_cast<uint8_t>((60.0f + 120.0f * (1.0f - t)) * glow);
        const uint8_t gg = static_cast<uint8_t>((140.0f + 90.0f * (1.0f - t)) * glow);
        const uint8_t bb = static_cast<uint8_t>((220.0f + 30.0f * (1.0f - t)) * glow);
        Color c = Color::straight(rr, gg, bb, static_cast<uint8_t>(235.0f * edge));
        return c;
    });
}

void DrawBackground(Canvas& c, float w, float h) {
    c.fillRect(Rect(0, 0, w, h),
               Paint::linearAngle(Rect(0, 0, w, h), 200.0f, 0xFF0F131A, 0xFF1B2433));
    // 顶部一抹冷光，让深色底有层次而不是死黑
    c.fillRect(Rect(0, 0, w, h * 0.5f),
               Paint::linearAngle(Rect(0, 0, w, h * 0.5f), 180.0f,
                                  Color::argb(0x223E8BFF), Color::argb(0x00000000)));
}

void DrawCard(Canvas& c) {
    Shape card;
    card.path.addRoundedRect(Rect(24, 24, 300, 148), 18.0f);
    card.paint = Paint::linearAngle(Rect(24, 24, 300, 148), 35.0f,
                                    std::vector<GradientStop>{
                                        GradientStop(0.00f, 0xFF3E8BFF),
                                        GradientStop(0.55f, 0xFF2FBF9A),
                                        GradientStop(1.00f, 0xFFF2B544)});
    card.shadow.dx = 4.0f;
    card.shadow.dy = 10.0f;
    card.shadow.blur = 12.0f;
    // 深色底上黑投影几乎看不见，用同色系的柔光能一眼看出'形状浮起来了'
    card.shadow.color = Color::argb(0x772FBF9A);
    card.draw(c);

    // 卡片上的装饰：一枚内嵌的半透明圆环 + 高光条
    Path ring;
    ring.addCircle(286, 56, 26);
    c.strokePath(ring, Color::argb(0x55FFFFFF), 6.0f);
    Path line;
    line.moveTo(44, 132).lineTo(120, 132);
    c.strokePath(line, Color::argb(0x66FFFFFF), 5.0f);
}

void DrawShapes(Canvas& c) {
    // 星形（自交 → 用 nonzero 填充成实心）
    Path star;
    star.addStar(78, 226, 34, 14, 5);
    c.fillPath(star, Paint::linearAngle(Rect(44, 192, 68, 68), 90.0f,
                                        0xFFF2B544, 0xFFFF8A65),
               FillRule::NonZero);

    // 圆环：描边走四边形+圆角连接
    Path ring;
    ring.addCircle(178, 226, 30);
    c.strokePath(ring, Paint::linearAngle(Rect(148, 196, 60, 60), 0.0f,
                                          0xFF3E8BFF, 0xFF7AAAFF), 9.0f);

    // 贝塞尔山丘：填充区（曲线细分 + 扫描线）
    Path hill;
    hill.moveTo(232, 254);
    hill.cubicTo(252, 186, 288, 268, 308, 214);
    hill.lineTo(308, 254);
    hill.close();
    c.fillPath(hill, Paint::linearAngle(Rect(232, 186, 76, 68), 90.0f,
                                        0xFF2FBF9A, 0xFF3E8BFF));
}

void DrawGroupOpacity(Canvas& c) {
    // 组透明度：两个重叠的方块，重叠区不能更亮（离屏合成）
    Group group;
    group.opacity = 0.62f;
    {
        auto a = std::make_unique<Shape>();
        a->path.addRoundedRect(Rect(44, 288, 96, 96), 22.0f);
        a->paint = Paint::solid(Color::argb(0xFF3E8BFF));
        group.add(std::move(a));
    }
    {
        auto b = std::make_unique<Shape>();
        b->path.addRoundedRect(Rect(108, 288, 96, 96), 22.0f);
        b->paint = Paint::solid(Color::argb(0xFFFF6B6B));
        group.add(std::move(b));
    }
    group.draw(c);

    // 旁边一小块"圆角矩形裁剪"：亮渐变 + 斜纹都只出现在圆角框内，
    // 边缘是覆盖率（不是硬切），和左边两块的方角形成对照
    c.save();
    Path clip;
    clip.addRoundedRect(Rect(236, 292, 84, 92), 18.0f);
    c.clipPath(clip);
    c.fillRect(Rect(236, 292, 84, 92),
               Paint::linearAngle(Rect(236, 292, 84, 92), 120.0f, 0xFF7AAAFF, 0xFFF2B544));
    for (int i = 0; i < 6; ++i) {
        Path stripe;
        const float x = 236.0f + static_cast<float>(i) * 18.0f;
        stripe.moveTo(x, 384.0f).lineTo(x + 60.0f, 292.0f);
        c.strokePath(stripe, Color::argb(0x33FFFFFF), 7.0f);
    }
    c.restore();
}

void DrawCircularClip(Canvas& c) {
    const Point center{492.0f, 120.0f};
    const float radius = 74.0f;
    c.save();
    Path clip;
    clip.addCircle(center.x, center.y, radius);
    c.clipPath(clip);
    c.fillRect(Rect(center.x - radius, center.y - radius, radius * 2, radius * 2),
               Paint::linearAngle(Rect(center.x - radius, center.y - radius,
                                       radius * 2, radius * 2),
                                  55.0f, 0xFF7AAAFF, 0xFF2FBF9A));
    // 圆内的斜纹（展示裁剪对多条内容都生效）
    for (int i = -6; i <= 6; ++i) {
        Path stripe;
        const float x = center.x + static_cast<float>(i) * 22.0f;
        stripe.moveTo(x - 120.0f, center.y + 120.0f);
        stripe.lineTo(x + 120.0f, center.y - 120.0f);
        c.strokePath(stripe, Color::argb(0x22FFFFFF), 9.0f);
    }
    c.restore();
    Path outline;
    outline.addCircle(center.x, center.y, radius + 4.0f);
    c.strokePath(outline, Color::argb(0x88FFFFFF), 2.0f);
}

void DrawBlendStrips(Canvas& c) {
    // 底色：深色面板 + 一条**亮**渐变带。五种模式都作用在亮带上，差异才看得出：
    //   SrcOver 半透蓝、Replace 平涂红、Erase 擦成底色、Keep 乘出暗色、Additive 提亮
    c.fillRounded(Rect(340, 214, 296, 116), 14.0f, Color::argb(0xFF2A3441));
    c.fillRounded(Rect(352, 250, 272, 40), 8.0f,
                  Paint::linearAngle(Rect(352, 250, 272, 40), 90.0f, Color::argb(0xFFF4F7FC),
                                     Color::argb(0xFF9DBBEE)));
    const BlendMode modes[5] = {BlendMode::SrcOver, BlendMode::Replace, BlendMode::Erase,
                                BlendMode::Keep, BlendMode::Additive};
    const uint32_t colors[5] = {0xB33E8BFFu, 0x99FF6B6B, 0x99FFFFFF, 0xFFFF8A65, 0x662FBF9A};
    for (int i = 0; i < 5; ++i) {
        c.save();
        c.setBlendMode(modes[i]);
        const float x = 356.0f + static_cast<float>(i) * 54.0f;
        c.fillRounded(Rect(x, 226, 42, 92), 10.0f, Paint::solid(Color::argb(colors[i])));
        c.restore();
    }
}

void DrawNineSlice(Canvas& c, const Image& chip) {
    // 拉伸后的九宫格：四角不变形、中段被拉长（圆角与高光都保持原样）
    c.drawNineSlice(chip, Rect(340, 348, 250, 88), 20.0f, 20.0f, 20.0f, 20.0f,
                    Color::white(), ImageFilter::Linear);
    // 原尺寸对照放在右边：一眼能看出"角没动、中间被拉开"
    c.drawImage(chip, Rect(604, 368, 44, 44), Rect(0, 0, 1, 1), Color::white(),
                ImageFilter::Linear);
}

void DrawAccent(Canvas& c) {
    c.fillRounded(Rect(24, 404, 300, 6), 3.0f,
                  Paint::linearAngle(Rect(24, 404, 300, 6), 90.0f,
                                     Color::argb(0xFF3E8BFF), Color::argb(0x002FBF9A)));
}

} // namespace

int main(int argc, char** argv) {
    const std::string outPath = (argc > 1) ? argv[1] : "bin2d-demo.png";
    const int w = 660, h = 470;

    Surface canvas(w, h);
    Canvas c(canvas);
    DrawBackground(c, static_cast<float>(w), static_cast<float>(h));
    DrawCard(c);
    DrawShapes(c);
    DrawGroupOpacity(c);
    DrawCircularClip(c);
    DrawBlendStrips(c);
    const Image chip = MakeChipImage();
    DrawNineSlice(c, chip);
    DrawAccent(c);

    if (!writePng(canvas, outPath)) {
        std::printf("写图失败: %s\n", outPath.c_str());
        return 1;
    }
    std::printf("已写出 %s (%dx%d)\n", outPath.c_str(), w, h);
    return 0;
}
