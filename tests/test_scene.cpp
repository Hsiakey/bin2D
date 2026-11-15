// test_scene —— Scene 渲染与 Canvas 直接画逐像素一致；Group 变换/透明度
#include "bin2d/canvas.h"
#include "bin2d/scene.h"
#include "test_util.h"

using namespace bin2d;

// 用即时模式画一份"标准内容"，Scene 再画一份同样的内容，两张必须逐像素相同
static void PaintReference(Canvas& c) {
    c.fillRect(Rect(10, 10, 100, 60), Paint::linearAngle(Rect(10, 10, 100, 60), 30.0f,
                                                         0xFF3E8BFF, 0xFF2FBF9A));
    c.fillRounded(Rect(10, 90, 120, 40), 12.0f, Color::argb(0xFFF2B544));
    Path star;
    star.addStar(180, 60, 40, 16, 5);
    c.fillPath(star, Color::argb(0xFFFF6B6B), FillRule::NonZero);
    Path ring;
    ring.addCircle(90, 180, 50);
    c.strokePath(ring, Color::argb(0xFF7AAAFF), 6.0f);
}

static void PaintSceneContent(Scene& scene) {
    {
        auto shape = std::make_unique<Shape>();
        shape->path.addRect(Rect(10, 10, 100, 60));
        shape->paint = Paint::linearAngle(Rect(10, 10, 100, 60), 30.0f, 0xFF3E8BFF, 0xFF2FBF9A);
        scene.root().add(std::move(shape));
    }
    {
        auto shape = std::make_unique<Shape>();
        shape->path.addRoundedRect(Rect(10, 90, 120, 40), 12.0f);
        shape->paint = Paint::solid(Color::argb(0xFFF2B544));
        scene.root().add(std::move(shape));
    }
    {
        auto shape = std::make_unique<Shape>();
        shape->path.addStar(180, 60, 40, 16, 5);
        shape->paint = Paint::solid(Color::argb(0xFFFF6B6B));
        scene.root().add(std::move(shape));
    }
    {
        auto shape = std::make_unique<Shape>();
        shape->path.addCircle(90, 180, 50);
        shape->paint = Paint::solid(Color::argb(0xFF7AAAFF));
        shape->stroked = true;
        shape->stroke.width = 6.0f;
        scene.root().add(std::move(shape));
    }
}

static int CountDiff(const Surface& a, const Surface& b) {
    int diff = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uint8_t* pa = a.row(y);
        const uint8_t* pb = b.row(y);
        for (int x = 0; x < a.width() * 4; ++x)
            if (pa[x] != pb[x]) { ++diff; break; }
    }
    return diff;
}

static void TestSceneEqualsCanvas() {
    const Color bg = Color::argb(0xFF1B1B1E);
    Surface direct(240, 240);
    {
        Canvas c(direct);
        c.clear(bg);
        PaintReference(c);
    }
    Scene scene(240, 240);
    scene.setBackground(bg);
    PaintSceneContent(scene);
    Surface viaScene(240, 240);
    scene.draw(viaScene);

    CHECK(CountDiff(direct, viaScene) == 0, "scene: 渲染结果与 Canvas 直接画逐像素相同");
    // 再画一遍必须完全一样（确定性 —— 缓存阶段的前提）
    Surface again(240, 240);
    scene.draw(again);
    CHECK(CountDiff(viaScene, again) == 0, "scene: 同一帧重复渲染逐像素稳定");
    CHECK(scene.version() >= 1, "scene: version() 存在（缓存阶段的钩子）");
}

static void TestGroupTransformAndOpacity() {
    Surface s(120, 120);
    Canvas c(s);
    c.clear(Color::black());

    Group g;
    g.transform = Transform::translate(60.0f, 60.0f) * Transform::rotateDeg(45.0f);
    g.opacity = 0.5f;
    {
        auto shape = std::make_unique<Shape>();
        shape->path.addRect(Rect(-30, -30, 60, 60));
        shape->paint = Paint::solid(Color::white());
        g.add(std::move(shape));
    }
    g.draw(c);

    // 中心必然是白的 50%（黑底上 = 127 左右）
    const int mid = s.get(60, 60).r;
    CHECK_NEAR(mid, 127, 3, "scene: Group 的 transform + opacity 生效（中心 50% 白）");
    // 旋转 45° 后四角露黑：左上角 (5,5) 应在方块之外
    CHECK(s.get(5, 5).r == 0, "scene: Group 旋转后角落没有被盖住");

    // 子节点自身的 transform 与组叠加（父 ∘ 子）
    Surface s2(120, 120);
    Canvas c2(s2);
    c2.clear(Color::black());
    Group g2;
    g2.transform = Transform::translate(40.0f, 0.0f);
    auto child = std::make_unique<Shape>();
    child->transform = Transform::translate(10.0f, 0.0f);
    child->path.addRect(Rect(0, 0, 20, 20));
    child->paint = Paint::solid(Color::white());
    Shape* childPtr = child.get();
    g2.add(std::move(child));
    g2.draw(c2);
    CHECK(s2.get(55, 10).r == 255, "scene: 父变换与子变换复合（40 + 10 偏移）");
    CHECK(s2.get(15, 10).r == 0, "scene: 只应用了子变换的位置没有内容");
    (void)childPtr;
}

static void TestShadowDoesNotBreakOpaque() {
    Surface s(160, 160);
    Canvas c(s);
    c.clear(Color::argb(0xFF202020));

    Shape shape;
    shape.path.addRoundedRect(Rect(40, 40, 80, 60), 14.0f);
    shape.paint = Paint::solid(Color::argb(0xFF3E8BFF));
    shape.shadow.dx = 6.0f;
    shape.shadow.dy = 8.0f;
    shape.shadow.blur = 10.0f;
    shape.shadow.color = Color::argb(0x80000000);
    shape.draw(c);

    CHECK(s.get(80, 70).r > 40, "scene: 投影之上的形状本体正常绘制");
    // 投影方向（右下）的平均亮度必须低于远处背景 —— 模糊很大，单点比较会被量化吃掉
    double nearSum = 0.0, farSum = 0.0;
    int nearN = 0, farN = 0;
    for (int y = 40; y < 110; ++y) {
        for (int x = 122; x < 140; ++x) { nearSum += s.get(x, y).r; ++nearN; }
        for (int x = 4; x < 22; ++x) { farSum += s.get(x, y).r; ++farN; }
    }
    const double nearAvg = nearSum / static_cast<double>(nearN);
    const double farAvg = farSum / static_cast<double>(farN);
    CHECK(nearAvg < farAvg - 0.5, "scene: 投影落在偏移方向并压暗底色（区域平均）");
    std::printf("  （投影侧平均 %.2f vs 背景平均 %.2f）\n", nearAvg, farAvg);
}

int main() {
    TestSceneEqualsCanvas();
    TestGroupTransformAndOpacity();
    TestShadowDoesNotBreakOpaque();
    return Bin2dTestSummary("test_scene");
}
