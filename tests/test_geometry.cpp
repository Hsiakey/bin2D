// test_geometry —— Transform 复合/逆/往返、Path 展平容差与包围盒、Rect 求交/取整
#include "bin2d/path.h"
#include "bin2d/transform.h"
#include "test_util.h"

using namespace bin2d;

static void TestTransform() {
    // 先缩放再平移（this ∘ rhs：先 rhs）
    const Transform t = Transform::translate(100.0f, 50.0f) * Transform::scale(2.0f, 3.0f);
    const Point p = t.map(10.0f, 10.0f);
    CHECK_NEAR(p.x, 120.0, 1e-4, "transform: 复合后 x = 100 + 10*2");
    CHECK_NEAR(p.y, 80.0, 1e-4, "transform: 复合后 y = 50 + 10*3");

    // 旋转 90°（数学正方向：x 轴转到 +y）
    const Transform r = Transform::rotateDeg(90.0f);
    const Point rp = r.map(1.0f, 0.0f);
    CHECK_NEAR(rp.x, 0.0, 1e-5, "transform: rotate90 的 x 分量");
    CHECK_NEAR(rp.y, 1.0, 1e-5, "transform: rotate90 的 y 分量");

    // 逆矩阵往返
    const Transform combo = Transform::translate(13.0f, -7.0f) * Transform::rotateDeg(37.0f) *
                            Transform::scale(1.7f, 0.6f);
    Transform inv;
    CHECK(combo.invert(inv), "transform: 非退化矩阵可求逆");
    const Point a{3.5f, -2.25f};
    const Point back = inv.map(combo.map(a));
    CHECK_NEAR(back.x, a.x, 1e-4, "transform: 往返 x 还原");
    CHECK_NEAR(back.y, a.y, 1e-4, "transform: 往返 y 还原");

    Transform bad;
    CHECK(!Transform::scale(0.0f, 0.0f).invert(bad), "transform: 退化矩阵求逆失败");

    // mapBounds：旋转后取四角
    const Rect rb = Transform::rotateDeg(45.0f).mapBounds(Rect(-1, -1, 2, 2));
    CHECK_NEAR(rb.w, 2.0f * std::sqrt(2.0f), 1e-3, "transform: 旋转 45° 的包围盒宽度");
}

static void TestPathFlatten() {
    // 圆：展平后每个点都在圆上（半径误差 ≤ 容差），弦高 ≤ 容差
    Path circle;
    circle.addCircle(0.0f, 0.0f, 100.0f);
    const std::vector<Polyline> polys = circle.flatten(0.5f);
    CHECK(polys.size() == 1, "path: 一个圆展平出一条子路径");
    CHECK(polys[0].closed, "path: addCircle 是闭合子路径");
    double maxRadErr = 0.0, maxChord = 0.0;
    const std::vector<Point>& pts = polys[0].pts;
    for (const Point& p : pts) {
        const double rr = std::sqrt(static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y);
        maxRadErr = std::max(maxRadErr, std::fabs(rr - 100.0));
    }
    CHECK(maxRadErr < 0.1, "path: 展平点落在圆上（半径误差 < 0.1，κ 弧近似的固有偏差）");
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Point& a = pts[i];
        const Point& b = pts[i + 1];
        const Point mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        const double distToCenter = std::sqrt(static_cast<double>(mid.x) * mid.x +
                                              static_cast<double>(mid.y) * mid.y);
        maxChord = std::max(maxChord, 100.0 - distToCenter);
    }
    CHECK(maxChord <= 0.5 + 1e-3, "path: 弦高不超过给定容差（0.5）");
    // 容差放宽 → 点数变少（自适应细分真的按容差走）
    const size_t coarse = circle.flatten(4.0f)[0].pts.size();
    CHECK(coarse < pts.size(), "path: 容差越大点数越少（自适应细分生效）");

    // 包围盒：圆 ≈ 直径；短线段精确
    const Rect cb = circle.bounds();
    CHECK_NEAR(cb.w, 200.0, 0.3, "path: 圆的包围盒宽度");
    CHECK_NEAR(cb.h, 200.0, 0.3, "path: 圆的包围盒高度");

    Path line;
    line.moveTo(10, 20).lineTo(30, 25);
    const Rect lb = line.bounds();
    CHECK_NEAR(lb.x, 10.0, 1e-4, "path: 线段包围盒 x");
    CHECK_NEAR(lb.w, 20.0, 1e-4, "path: 线段包围盒宽度");

    // 三次贝塞尔：精确包围盒比"控制点外包盒"更紧
    Path cubic;
    cubic.moveTo(0, 0).cubicTo(0, 100, 100, 100, 100, 0);
    const Rect kb = cubic.bounds();
    CHECK_NEAR(kb.h, 75.0, 0.5, "path: 三次贝塞尔的精确高度 = 0.75×控制点高度");
    CHECK(kb.h < 100.0f, "path: 精确包围盒比控制点外包盒紧");

    Path rounded;
    rounded.addRoundedRect(Rect(0, 0, 40, 20), 100.0f);   // 半径超限 → 夹到 10（胶囊）
    const Rect rb2 = rounded.bounds();
    CHECK_NEAR(rb2.w, 40.0, 0.2, "path: 圆角矩形（半径夹紧）宽度不变");
    CHECK_NEAR(rb2.h, 20.0, 0.2, "path: 圆角矩形（半径夹紧）高度不变");

    // 点在路径内
    CHECK(circle.contains(0.0f, 0.0f, FillRule::NonZero), "path: 圆心在圆内");
    CHECK(!circle.contains(120.0f, 0.0f, FillRule::NonZero), "path: 圆外点不在圆内");
}

static void TestRect() {
    const Rect a(0, 0, 100, 100);
    const Rect b(50, 50, 100, 100);
    const Rect i = Rect::intersect(a, b);
    CHECK_NEAR(i.x, 50.0, 1e-4, "rect: 求交 x");
    CHECK_NEAR(i.w, 50.0, 1e-4, "rect: 求交宽度");
    CHECK(Rect::intersect(a, Rect(200, 200, 10, 10)).empty(), "rect: 无交集为空");

    const Rect sn = Rect(-0.5f, 1.25f, 10.2f, 3.9f).snappedOut();
    CHECK_NEAR(sn.x, -1.0, 1e-4, "rect: 向外取整 x");
    CHECK_NEAR(sn.y, 1.0, 1e-4, "rect: 向外取整 y");
    CHECK_NEAR(sn.right(), 10.0, 1e-4, "rect: 向外取整右边界 = ceil(x+w)");

    CHECK(a.contains(0, 0), "rect: 左上角在内部");
    CHECK(!a.contains(100, 50), "rect: 右边界是开区间");
}

int main() {
    TestTransform();
    TestPathFlatten();
    TestRect();
    return Bin2dTestSummary("test_geometry");
}
