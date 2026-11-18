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

// ------------------------------------------------------------
// 投影（0.8.0 改成"alpha 罩"时顺带修的 bug：旧实现的 dx/dy 只用来移动**源矩形**，
// 没有作用到填充上 —— 结果是投影永远贴在形状正中（被形状盖住），只在四周漏一圈模糊）
//
// 所以这里不写"看起来像阴影"的软断言，而是三条硬性质：
//   ① 形状+偏移的**落点**必须出现阴影色（按公式算出的精确值）；
//   ② 反方向（形状-偏移）必须**一点都没有** —— 这条正是旧 bug 会红的断言；
//   ③ 形状本体、裁剪、模糊半径各自的行为。
// ------------------------------------------------------------
static void TestShadowOffset() {
    Surface s(140, 140);
    Canvas c(s);
    const Color bg = Color::argb(0xFF202020);
    c.clear(bg);

    Shape shape;
    shape.path.addRect(Rect(40, 40, 60, 40));
    shape.paint = Paint::solid(Color::argb(0xFF3E8BFF));
    shape.shadow.dx = 12.0f;
    shape.shadow.dy = 10.0f;
    shape.shadow.blur = 0.0f;                       // 硬边：每个像素可预测
    shape.shadow.color = Color::argb(0x99000000);   // a=153
    shape.draw(c);

    // ① 落点：形状 (40..100,40..80) 平移 (12,10) → (52..112, 50..90)。
    //    取 (70,85)：在投影矩形内、在形状外（y>80）→ 纯阴影压在底色上。
    //    预乘 src-over：r = (0 + 32*102 + 127)/255 = 13（bg 0x20 = 32）
    const Color hit = s.get(70, 85);
    CHECK(hit.r == 13 && hit.g == 13 && hit.b == 13,
          "scene: 投影出现在形状+偏移的落点（精确值 source-over）");
    std::printf("  hit(%d,%d)= (%d,%d,%d,%d)  rev=(%d,%d,%d,%d) shape=(%d,%d,%d,%d)\n", 70, 85, hit.r, hit.g, hit.b, hit.a, s.get(34,34).r, s.get(34,34).g, s.get(34,34).b, s.get(34,34).a, s.get(70,60).r, s.get(70,60).g, s.get(70,60).b, s.get(70,60).a);

    // ② 反向落点（形状 - 偏移 ≈ (28..88, 30..70) 的形状外区域）必须与底色逐位相同
    CHECK(s.get(34, 34).r == 32 && s.get(34, 34).g == 32,
          "scene: 反方向没有投影（旧实现的 dx/dy 失效会在这里红）");
    // 形状本体不受影响
    CHECK(s.get(70, 60).r == 0x3E && s.get(70, 60).a == 255,
          "scene: 投影不改变形状本体像素");
}

static void TestShadowBlurHalo() {
    Surface s(180, 180);
    Canvas c(s);
    c.clear(Color::argb(0xFF202020));
    Shape shape;
    shape.path.addRect(Rect(50, 50, 60, 40));
    shape.paint = Paint::solid(Color::argb(0xFFFFFFFF));
    shape.shadow.dx = 14.0f;
    shape.shadow.dy = 14.0f;
    shape.shadow.blur = 12.0f;
    shape.shadow.color = Color::argb(0x80000000);
    shape.draw(c);

    // 模糊后投影是"形状(偏移后) → 半径 12 的方形核"。
    // ① 偏移侧形状右缘外仍有一块**实心**投影（偏移 14 > 0 才看得见；旧实现里它不存在）
    double nearSum = 0.0, farSum = 0.0;
    int nearN = 0, farN = 0;
    int farMin = 255;
    for (int y = 55; y < 85; ++y) {
        for (int x = 112; x < 124; ++x) { nearSum += s.get(x, y).r; ++nearN; }   // 形状(110)右侧
        for (int x = 24; x < 36; ++x) {
            const int v = s.get(x, y).r;
            farSum += v; ++farN;
            if (v < farMin) farMin = v;
        }
    }
    const double nearAvg = nearSum / static_cast<double>(nearN);
    const double farAvg = farSum / static_cast<double>(farN);
    CHECK(nearAvg < farAvg - 5.0, "scene: 偏移侧形状外有实心投影（反向没有）");
    // 比"平均值低"更硬：反方向**一个像素**都不许被投影碰到
    CHECK(farMin == 32, "scene: 反方向区域逐点等于背景（投影完全不泄漏）");
    std::printf("  （偏移侧 %.2f vs 反向 %.2f）\n", nearAvg, farAvg);

    // ② 投影外缘必须是渐弱的（模糊生效），而不是一刀切
    const int atShadowEdge = s.get(126, 70).r;
    const int atHalo = s.get(132, 70).r;
    const int atBg = s.get(146, 70).r;
    CHECK(atShadowEdge < atHalo && atHalo <= atBg,
          "scene: 模糊投影向外逐渐衰减到背景");

    // 形状本体仍是纯白（模糊的罩不能反过来压暗本体）
    CHECK(s.get(80, 70).r == 255, "scene: 模糊投影之上本体仍是白色");
}

static void TestShadowRespectsClip() {
    Surface s(160, 160);
    Canvas c(s);
    c.clear(Color::argb(0xFF202020));
    c.clipRect(Rect(0, 0, 80, 160));   // 右半边被裁掉

    Shape shape;
    shape.path.addRect(Rect(60, 40, 60, 40));
    shape.paint = Paint::solid(Color::argb(0xFFFFFFFF));
    shape.shadow.dx = 10.0f;
    shape.shadow.dy = 10.0f;
    shape.shadow.blur = 0.0f;
    shape.shadow.color = Color::argb(0x99000000);
    shape.draw(c);

    // 裁剪内（x<80）的投影落点被画上；裁剪外（x>=80）原样
    CHECK(s.get(78, 80).r == 13, "scene: 投影在裁剪内正常合成");
    CHECK(s.get(100, 80).r == 32, "scene: 投影不越过画布裁剪矩形");
}

// 覆盖率遮罩合成：裁剪 + 源 alpha + 混合模式各一条（投影之外的直接调用者也用得到）
static void TestCoverageMaskComposite() {
    Surface s(8, 8);
    Canvas c(s);
    c.clear(Color::argb(0xFF000000));
    std::vector<uint8_t> mask(16, 255);
    c.blendCoverageMask(mask.data(), 4, 4, 2, 2, Color::argb(0xFFFFFFFF));
    CHECK(s.get(2, 2).r == 255 && s.get(5, 5).r == 255, "mask: 整块遮罩按颜色铺满");
    CHECK(s.get(1, 1).r == 0 && s.get(6, 6).r == 0, "mask: 遮罩外不动");

    std::vector<uint8_t> half(4, 128);
    c.blendCoverageMask(half.data(), 2, 2, 0, 0, Color::argb(0xFFFFFFFF));
    CHECK(s.get(0, 0).r == 128, "mask: 覆盖率 128 → 50% 混合（四舍五入）");
}

int main() {
    TestSceneEqualsCanvas();
    TestGroupTransformAndOpacity();
    TestShadowOffset();
    TestShadowBlurHalo();
    TestShadowRespectsClip();
    TestCoverageMaskComposite();
    return Bin2dTestSummary("test_scene");
}
