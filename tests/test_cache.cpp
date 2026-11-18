// test_cache —— Scene 子树缓存（0.9.0）
//
// 三条底线：
//   ① 命中缓存的那一帧必须与"老老实实重画"**逐像素相同**（缓存只换成本，不换语义）；
//   ② 版本号变了必须重画（改形状 touch()、改组变换自动失效）；
//   ③ 缓存只吃自己认得的那一类组（opacity/blend/裁剪/投影一律走原路径）。
#include "bin2d/canvas.h"
#include "bin2d/scene.h"
#include "test_util.h"

#include <memory>

using namespace bin2d;

static int CountDiff(const Surface& a, const Surface& b) {
    if (a.width() != b.width() || a.height() != b.height()) return -1;
    int diff = 0;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.row(y), b.row(y), static_cast<size_t>(a.width()) * 4) != 0) ++diff;
    }
    return diff;
}

static std::unique_ptr<Group> MakeCard(float x, float y, Color accent) {
    auto g = std::make_unique<Group>();
    g->transform = Transform::translate(x, y);
    const Rect card(0, 0, 140, 100);
    {
        auto s = std::make_unique<Shape>();
        s->path.addRoundedRect(card, 12.0f);
        s->paint = Paint::solid(Color::argb(0xFF1E2732));
        g->add(std::move(s));
    }
    {
        auto s = std::make_unique<Shape>();
        s->path.addRoundedRect(Rect(10, 10, 34, 34), 8.0f);
        s->paint = Paint::solid(accent);
        g->add(std::move(s));
    }
    {
        auto s = std::make_unique<Shape>();
        s->path.addRoundedRect(Rect(54, 16, 70, 9), 4.0f);
        s->paint = Paint::solid(Color::argb(0xFF9DB2C8));
        g->add(std::move(s));
    }
    return g;
}

static Scene MakeScene(int cards) {
    Scene scene(640, 480);
    scene.setBackground(Color::argb(0xFF0B0E12));
    for (int i = 0; i < cards; ++i) {
        const float x = 20.0f + (i % 4) * 152.0f;
        const float y = 20.0f + (i / 4) * 118.0f;
        scene.root().add(MakeCard(x, y, Color::argb(0xFF3E8BFF)));
    }
    return scene;
}

// 拿"无缓存路径"当参考，逐像素比
static int CompareWithUncached(const Scene& scene, Surface& cachedOut) {
    Surface reference(scene.size().w > 0 ? static_cast<int>(scene.size().w) : 640,
                      static_cast<int>(scene.size().h));
    scene.draw(reference);
    return CountDiff(cachedOut, reference);
}

static void TestCachedEqualsUncached() {
    Scene scene = MakeScene(8);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    const int diff1 = CompareWithUncached(scene, out);
    CHECK(diff1 == 0, "cache: 首帧（全部 miss）与无缓存渲染逐像素相同");
    if (diff1 != 0) std::printf("  差异行 %d\n", diff1);

    scene.drawCached(out, cache);   // 第二帧：全部命中
    const int diff2 = CompareWithUncached(scene, out);
    CHECK(diff2 == 0, "cache: 命中帧与无缓存渲染逐像素相同");
    CHECK(cache.stats().hits == cache.stats().groups_total,
          "cache: 第二帧全部命中");
    CHECK(cache.stats().misses == 0, "cache: 第二帧没有重画");
    std::printf("  （%d 个组，%d 次命中，%zu KB）\n", cache.stats().groups_total,
                cache.stats().hits, cache.stats().bytes / 1024);
}

static void TestTouchInvalidates() {
    Scene scene = MakeScene(8);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    scene.drawCached(out, cache);

    // 改第二张卡片的图标位置，然后 touch()：只有那一个组该重画
    Group& g = *static_cast<Group*>(scene.root().children[1].get());
    Shape& icon = *static_cast<Shape*>(g.children[1].get());
    icon.path.clear();
    icon.path.addRoundedRect(Rect(20, 60, 30, 30), 8.0f);
    icon.touch();

    scene.drawCached(out, cache);
    CHECK(cache.stats().misses == 1, "cache: 改一个子形状 → 只有它所在的组重画");
    CHECK(cache.stats().hits == cache.stats().groups_total - 1, "cache: 其余组仍然命中");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 失效重画后像素与无缓存一致");
}

static void TestTransformAutoInvalidates() {
    Scene scene = MakeScene(4);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    scene.drawCached(out, cache);

    // 只改组变换（框架字段自动参与哈希，不需要 touch）
    Group& g = *static_cast<Group*>(scene.root().children[2].get());
    g.transform = Transform::translate(60.0f, 300.0f);
    scene.drawCached(out, cache);
    CHECK(cache.stats().misses == 1, "cache: 改组变换 → 该组自动失效（不用 touch）");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 组变换后像素与无缓存一致");
}

static void TestContractWithoutTouch() {
    Scene scene = MakeScene(4);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    scene.drawCached(out, cache);

    // 不调用 touch 就改几何 → 缓存按契约返回旧像素。这是**已知契约**（版本号靠调用方维护），
    // 写成断言是为了它哪天变了能被立刻发现。
    Group& g = *static_cast<Group*>(scene.root().children[0].get());
    Shape& icon = *static_cast<Shape*>(g.children[1].get());
    icon.path.clear();
    icon.path.addRect(Rect(10, 60, 20, 20));
    scene.drawCached(out, cache);
    CHECK(cache.stats().misses == 0, "cache: 不 touch 的几何改动不会被发现（契约）");
    CHECK(CompareWithUncached(scene, out) != 0, "cache: 不 touch 时画面仍是旧的（契约的另一面）");
}

static void TestRemoveGroupPrunes() {
    Scene scene = MakeScene(8);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    const size_t bytesBefore = cache.stats().bytes;
    const int entriesBefore = cache.stats().entries;
    CHECK(entriesBefore == 8, "cache: 8 张卡片 = 8 个缓存条目");

    scene.root().children.pop_back();
    scene.drawCached(out, cache);
    CHECK(cache.stats().entries == entriesBefore - 1, "cache: 删掉一个组 → 条目被淘汰");
    CHECK(cache.stats().bytes < bytesBefore, "cache: 淘汰后常驻字节数下降");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 淘汰后像素与无缓存一致");
}

static void TestNonCacheableGroups() {
    Scene scene = MakeScene(4);
    // 加一个 opacity<1 的组：它不该进缓存，但像素必须对
    auto faded = MakeCard(20.0f, 380.0f, Color::argb(0xFFFF6B6B));
    faded->opacity = 0.5f;
    scene.root().add(std::move(faded));

    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    scene.drawCached(out, cache);
    CHECK(cache.stats().groups_total == 4, "cache: 半透明组不被当成可缓存组");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 半透明组仍走原路径且像素正确");

    // 面积门槛：调大之后连卡片的组也不再缓存
    SceneCache strict;
    strict.minGroupArea = 1000000;
    Surface out2(640, 480);
    scene.drawCached(out2, strict);
    scene.drawCached(out2, strict);
    CHECK(strict.empty(), "cache: 面积门槛调高 → 什么都不缓存");
    CHECK(CompareWithUncached(scene, out2) == 0, "cache: 不缓存时像素同样正确");
}

static void TestRootLayerFallsBack() {
    Scene scene = MakeScene(4);
    scene.root().opacity = 0.5f;
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    CHECK(cache.empty(), "cache: 根组半透明 → 整体退化，不留缓存");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 退化路径的像素与无缓存一致");
}

int main() {
    TestCachedEqualsUncached();
    TestTouchInvalidates();
    TestTransformAutoInvalidates();
    TestContractWithoutTouch();
    TestRemoveGroupPrunes();
    TestNonCacheableGroups();
    TestRootLayerFallsBack();
    return Bin2dTestSummary("test_cache");
}
