// test_image —— 九宫格边界自动探测 + mip 的显存上限（0.8.0）
//
// 为什么这些能写精确/区间的断言：
//   · 九宫格探测的输入是我们自己用 Canvas 画的圆角矩形，直边段的列/行**逐位相同**
//     （光栅器对整像素列的处理与列位置无关），所以边界应当恰好等于圆角半径；
//   · mip 链是 2 的幂盒式缩小，每级尺寸与总字节数都是确定的整数；
//   · 上限定小了的语义是"退回更深的一级"，返回的 Surface 尺寸可以精确断言。
#include "bin2d/canvas.h"
#include "bin2d/image.h"
#include "test_util.h"

using namespace bin2d;

// ------------------------------------------------------------
// 九宫格
// ------------------------------------------------------------
static Image MakeRoundedImage(int w, int h, float radius) {
    Surface s(w, h);
    Canvas c(s);
    c.fillRounded(Rect(0, 0, static_cast<float>(w), static_cast<float>(h)), radius,
                  Color::white());
    return Image(s);
}

static void TestNineSliceDetectRounded() {
    const Image img = MakeRoundedImage(48, 36, 12.0f);
    const NineSliceInsets ins = DetectNineSliceInsets(img);
    CHECK(ins.detected, "image: 圆角矩形能被探出拉伸带");
    // 直边段的列/行是逐位相同的，所以探测值应当就是半径；AA 的切点列会有 ±1 列误差
    CHECK(std::abs(ins.l - 12.0f) <= 1.0f && std::abs(ins.r - 12.0f) <= 1.0f,
          "image: 水平边界 = 圆角半径（±1）");
    CHECK(std::abs(ins.t - 12.0f) <= 1.0f && std::abs(ins.b - 12.0f) <= 1.0f,
          "image: 垂直边界 = 圆角半径（±1）");
    std::printf("  （探测到 l=%.0f t=%.0f r=%.0f b=%.0f）\n", ins.l, ins.t, ins.r, ins.b);

    // 用探测结果做九宫格：四角与直边之间的过渡不能出现"拉伸的圆角"
    // （用 2× 目标宽度画，角上的像素与半径相关，不在这里逐点断言，只验证不崩且画满）
    Surface dst(96, 36);
    Canvas c(dst);
    c.drawNineSliceAuto(img, Rect(0, 0, 96, 36), Color::white());
    bool filled = true;
    for (int x = 0; x < 96; ++x) {
        if (dst.get(x, 18).r == 0) filled = false;
    }
    CHECK(filled, "image: 自动九宫格把中间行拉满（没有空洞）");
}

static void TestNineSliceDetectBorder() {
    // 1px 边框、内部透明：可拉伸带是第 1..18 行/列，边界必须精确 = 1
    Surface s(20, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x) {
            const bool edge = (x == 0 || x == 19 || y == 0 || y == 19);
            s.set(x, y, edge ? Color::white() : Color::trans());
        }
    const NineSliceInsets ins = DetectNineSliceInsets(Image(s));
    CHECK(ins.detected, "image: 1px 边框能探出拉伸带");
    CHECK(ins.l == 1.0f && ins.r == 1.0f && ins.t == 1.0f && ins.b == 1.0f,
          "image: 1px 边框的边界精确为 1");
}

static void TestNineSliceNoStretchBand() {
    // 纯色图：每一列都相同 —— 没有"边界"可探，必须明确返回 detected = false
    Surface s(24, 24);
    s.clear(Color::white());
    const NineSliceInsets ins = DetectNineSliceInsets(Image(s));
    CHECK(!ins.detected, "image: 纯色图没有拉伸带，返回 detected=false");
}

// ------------------------------------------------------------
// mip 上限
// ------------------------------------------------------------
static Image MakeChecker(int w) {
    Surface s(w, w);
    for (int y = 0; y < w; ++y)
        for (int x = 0; x < w; ++x) {
            const bool on = ((x / 8) + (y / 8)) % 2 == 0;
            s.set(x, y, on ? Color::white() : Color::argb(0xFF202020));
        }
    return Image(s);
}

static void TestMipUnlimited() {
    Image img = MakeChecker(64);
    const Surface& lvl = img.sampleSurface(64.0f);   // 要到 1/64 → 6 级
    CHECK(lvl.width() == 1 && lvl.height() == 1, "image: 不限上限时能取到 1×1 级");
    CHECK(img.mipLevelCount() == 6, "image: 64×64 满链 = 6 级");
    // (32² + 16² + 8² + 4² + 2² + 1²) × 4 = 5460 字节
    CHECK(img.mipBytes() == 5460, "image: mip 总字节数 = 各级像素和 × 4");
}

static void TestMipLevelCap() {
    Image img = MakeChecker(64);
    img.setMipLevelCap(1);
    const Surface& lvl = img.sampleSurface(64.0f);
    CHECK(img.mipLevelCount() == 1, "image: 级数上限 1 → 只生成 1 级");
    CHECK(lvl.width() == 32, "image: 超上限的取样退回最深一级（32×32）");
}

static void TestMipByteBudget() {
    {
        Image img = MakeChecker(64);
        img.setMipByteBudget(1000);   // 连第一级（4096 字节）都放不下
        const Surface& lvl = img.sampleSurface(64.0f);
        CHECK(img.mipLevelCount() == 0, "image: 预算连第一级都不够 → 不生成");
        CHECK(lvl.width() == 64, "image: 没有 mip 时取原图");
    }
    {
        Image img = MakeChecker(64);
        img.setMipByteBudget(5000);   // 4096 可以，+1024 就超了
        img.sampleSurface(64.0f);
        CHECK(img.mipLevelCount() == 1, "image: 预算在第二级处截断");
        CHECK(img.mipBytes() == 4096, "image: 截断后的字节数 = 第一级");
    }
}

static void TestMipInvalidatedByEdit() {
    Image img = MakeChecker(32);
    img.sampleSurface(16.0f);
    CHECK(img.mipLevelCount() > 0, "image: 先有 mip");
    (void)img.surface();   // 非 const 取用 = 要改像素 → mip 作废
    CHECK(img.mipLevelCount() == 0, "image: 改像素后 mip 缓存作废");
}

int main() {
    TestNineSliceDetectRounded();
    TestNineSliceDetectBorder();
    TestNineSliceNoStretchBand();
    TestMipUnlimited();
    TestMipLevelCap();
    TestMipByteBudget();
    TestMipInvalidatedByEdit();
    return Bin2dTestSummary("test_image");
}
