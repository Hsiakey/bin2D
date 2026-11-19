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

// ------------------------------------------------------------
// 0.10.0：真脏矩形 + 字节上限
// ------------------------------------------------------------
static Shape& IconOf(Scene& scene, int card) {
    Group& g = *static_cast<Group*>(scene.root().children[static_cast<size_t>(card)].get());
    return *static_cast<Shape*>(g.children[1].get());
}

static void TestPartialRepaintMatchesFull() {
    Scene scene = MakeScene(4);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);   // 首帧全量
    scene.drawCached(out, cache);   // 第二帧全命中

    int partialFrames = 0;
    long long fullPerFrame = cache.stats().full_pixels;   // 第二帧全命中，用首帧的数当基准
    {
        SceneCache fresh;
        Surface tmp(640, 480);
        Scene probe = MakeScene(4);
        probe.drawCached(tmp, fresh);
        fullPerFrame = fresh.stats().full_pixels;
    }
    long long worstPartial = 0;
    for (int i = 0; i < 6; ++i) {
        // 每帧把第 1 张卡片的图标挪 3px（只动卡片里的一小块）
        Shape& icon = IconOf(scene, 1);
        icon.path.clear();
        icon.path.addRoundedRect(Rect(10.0f + i * 3.0f, 10, 34, 34), 8.0f);
        icon.touch();
        scene.drawCached(out, cache);
        CHECK(CompareWithUncached(scene, out) == 0, "cache: 局部重画后与无缓存逐像素相同");
        if (cache.stats().partials == 1) ++partialFrames;
        if (cache.stats().partial_pixels > worstPartial) worstPartial = cache.stats().partial_pixels;
    }
    CHECK(partialFrames == 6, "cache: 连续 6 帧都走了脏矩形局部重画");
    CHECK(worstPartial * 4 < fullPerFrame,
          "cache: 每帧重画的像素不足整组重画的 1/4");
    std::printf("  （局部重画 %lld px/帧 vs 首帧整组 %lld px）\n", worstPartial, fullPerFrame);
}

static void TestPartialTogglePixelsIdentical() {
    // 同一个改动序列，partialRepaint 开/关必须给出同一张图（缓存只换成本，不换像素）
    Scene a = MakeScene(3);
    Scene b = MakeScene(3);
    SceneCache on;
    SceneCache off;
    off.partialRepaint = false;
    Surface sa(640, 480), sb(640, 480);
    for (int i = 0; i < 5; ++i) {
        for (Scene* sc : {&a, &b}) {
            Shape& icon = IconOf(*sc, 2);
            icon.path.clear();
            icon.path.addRoundedRect(Rect(12.0f + i * 2.0f, 20, 30, 30), 8.0f);
            icon.touch();
        }
        a.drawCached(sa, on);
        b.drawCached(sb, off);
        CHECK(CountDiff(sa, sb) == 0, "cache: 脏矩形开/关的像素完全相同");
    }
    CHECK(on.stats().partials > 0, "cache: 开关打开时确实走了脏矩形");
    CHECK(off.stats().partials == 0, "cache: 开关关闭时一次局部重画都没有");
}

static void TestLargeChangeFallsBackToFull() {
    Scene scene = MakeScene(4);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    scene.drawCached(out, cache);

    // 把图标从卡片左上挪到右下：新旧包围盒的并集几乎覆盖整张卡片 → 应当退回整组重画
    Group& g = *static_cast<Group*>(scene.root().children[2].get());
    Shape& icon = *static_cast<Shape*>(g.children[1].get());
    icon.path.clear();
    icon.path.addRoundedRect(Rect(96, 60, 34, 34), 8.0f);
    icon.touch();
    scene.drawCached(out, cache);
    CHECK(cache.stats().partials == 0 && cache.stats().misses == 1,
          "cache: 脏区太大 → 退回整组重画（不算局部）");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 退回整组后像素仍然正确");
}

static void TestStructuralChangesGoFull() {
    Scene scene = MakeScene(3);
    SceneCache cache;
    Surface out(640, 480);
    scene.drawCached(out, cache);
    scene.drawCached(out, cache);

    // ① 往组里加一个子节点 → 结构变化 → 整组
    Group& g = *static_cast<Group*>(scene.root().children[0].get());
    {
        auto extra = std::make_unique<Shape>();
        extra->path.addRect(Rect(10, 80, 20, 10));
        extra->paint = Paint::solid(Color::argb(0xFFFF6B6B));
        g.add(std::move(extra));
    }
    scene.drawCached(out, cache);
    CHECK(cache.stats().partials == 0, "cache: 加子节点 → 整组重画");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 加子节点后像素正确");

    // ② 隐藏一个子节点 → 整组（可见性影响覆盖关系）
    Group& g2 = *static_cast<Group*>(scene.root().children[1].get());
    g2.children[1]->visible = false;
    scene.drawCached(out, cache);
    CHECK(cache.stats().partials == 0, "cache: 隐藏子节点 → 整组重画");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 隐藏子节点后像素正确");
}

static void TestByteBudget() {
    Scene scene = MakeScene(8);
    SceneCache cache;
    cache.byteBudget = 200 * 1024;   // 8 张卡片约 437 KB，必须淘汰一半以上
    Surface out(640, 480);
    scene.drawCached(out, cache);
    CHECK(cache.stats().evicted > 0, "cache: 字节上限触发淘汰");
    CHECK(cache.stats().bytes <= cache.byteBudget, "cache: 常驻字节数落回预算内");
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 淘汰后本帧像素仍然正确");

    // 下一帧：被淘汰的组重画，其余命中 —— 像素必须继续正确（淘汰不能破坏画面）
    const size_t before = cache.stats().bytes;
    scene.drawCached(out, cache);
    CHECK(CompareWithUncached(scene, out) == 0, "cache: 淘汰后的下一帧像素仍然正确");
    CHECK(cache.stats().bytes <= cache.byteBudget, "cache: 下一帧仍在预算内");
    (void)before;
}

static void TestClearRect() {
    Surface s(16, 12);
    s.clear(Color::white());
    s.clearRect(Rect(4, 3, 5, 4));
    CHECK(s.get(4, 3).a == 0 && s.get(8, 6).a == 0, "cache: clearRect 真的写入全 0（不是 over 混合）");
    CHECK(s.get(3, 3).a == 255 && s.get(9, 7).a == 255, "cache: clearRect 范围外不动");
    s.clearRect(Rect(-5, -5, 8, 8));   // 越界自动裁
    CHECK(s.get(0, 0).a == 0 && s.get(15, 11).a == 255, "cache: clearRect 越界自动裁剪");
}

int main() {
    TestCachedEqualsUncached();
    TestTouchInvalidates();
    TestTransformAutoInvalidates();
    TestContractWithoutTouch();
    TestRemoveGroupPrunes();
    TestNonCacheableGroups();
    TestRootLayerFallsBack();
    TestPartialRepaintMatchesFull();
    TestPartialTogglePixelsIdentical();
    TestLargeChangeFallsBackToFull();
    TestStructuralChangesGoFull();
    TestByteBudget();
    TestClearRect();
    return Bin2dTestSummary("test_cache");
}
