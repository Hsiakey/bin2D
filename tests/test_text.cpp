// test_text —— libbin2d_text 的边界用例（0.22.7）
//
// 这个套件守的是"文字模块能单独用"这件事，四条边界一个都不缺：
//   1. 彩色字形（COLR/CPAL）：emoji 簇要出彩色位图，且图集里那格**不是**白字预乘；
//   2. 豆腐块：字体链里没有的码点必须出空心方框（留白会被当成引擎 bug）；
//   3. 逐字形回退：谁有字形靠字体自报的 cmap，解析到的面必须真的画得出该码点；
//   4. 多层 text-shadow：两层阴影各自成图集格，画出来的墨量比无阴影多。
//
// 另外守两条"边界不许破"的性质：
//   · 平台发现是**注入**的（Resolver），模块自己不认识 C:/Windows/Fonts；
//   · 测宽与绘制同源：textWidth 的推进量与 measureCluster 的簇宽一致。
//
// 字体文件从环境里探测（Windows 常见路径 / 其他平台常见路径），一个都没有时
// 与上层一致地 skip —— 核心库不假设系统里必定有字体。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/surface.h"
#include "bin2d/text.h"
#include "test_util.h"

using namespace bin2d;
using namespace bin2d::text;

namespace {

struct FontFile { const char* family; const char* path; };

// 只用来"给测试准备字体文件"，不是模块的一部分 —— 模块拿到的永远是路径。
const FontFile kCandidates[] = {
#ifdef _WIN32
    { "Segoe UI", "C:/Windows/Fonts/segoeui.ttf" },
    { "Microsoft YaHei", "C:/Windows/Fonts/msyh.ttc" },
    { "Segoe UI Symbol", "C:/Windows/Fonts/seguisym.ttf" },
    { "Segoe UI Emoji", "C:/Windows/Fonts/seguiemj.ttf" },
#else
    { "DejaVu Sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" },
    { "Noto Sans CJK SC", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc" },
    { "Noto Color Emoji", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf" },
    { "Apple Color Emoji", "/System/Library/Fonts/Apple Color Emoji.ttc" },
#endif
};

bool FileExists(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// 统计一张 Surface 上的墨量（预乘 alpha 之和）
long long InkOf(const Surface& s) {
    long long ink = 0;
    for (int y = 0; y < s.height(); y++) {
        const uint8_t* row = s.row(y);
        for (int x = 0; x < s.width(); x++) ink += row[static_cast<size_t>(x) * 4 + 3];
    }
    return ink;
}

// 有没有"彩色"像素（R/G/B 不三分量相等 —— 白字预乘恒有 r==g==b）
bool HasChromaticPixel(const Surface& s) {
    for (int y = 0; y < s.height(); y++) {
        const uint8_t* row = s.row(y);
        for (int x = 0; x < s.width(); x++) {
            const uint8_t* p = row + static_cast<size_t>(x) * 4;
            if (p[3] == 0) continue;
            if (p[0] != p[1] || p[1] != p[2]) return true;
        }
    }
    return false;
}

} // namespace

int main() {
    // ---- 0. 平台发现是注入的：模块只认 Resolver 给的路径 ----
    Library lib;
    CHECK(lib.init(), "text: FreeType 初始化");
    if (!lib.ready()) {
        std::printf("[skip] 没有 FreeType，文字模块不可用\n");
        return Bin2dTestSummary("text");
    }

    std::vector<std::string> names;
    std::string emojiFamily, emojiPath;
    for (const auto& c : kCandidates) {
        if (!FileExists(c.path)) continue;
        lib.addFont(c.family, c.path);
        names.push_back(c.family);
        const std::string fam = c.family;
        if (fam == "Segoe UI Emoji" || fam == "Noto Color Emoji" || fam == "Apple Color Emoji") {
            if (emojiPath.empty()) { emojiFamily = fam; emojiPath = c.path; }
        }
    }
    if (names.empty()) {
        std::printf("[skip] 本机一个候选字体都没有，跳过字形用例\n");
        return Bin2dTestSummary("text");
    }

    // Resolver：只在"登记表里没有"的名字上被问到。这里造一个别名、**不登记**，
    // 证明模块不需要任何平台 API 也能拿到 face —— 路径完全由注入方给。
    lib.setResolver([&](const FaceDesc& d) -> std::string {
        if (d.family == "Test Alias") return kCandidates[0].path;
        return std::string();
    });
    std::vector<std::string> chain;
    chain.push_back("Test Alias");
    for (const auto& n : names) chain.push_back(n);
    lib.setFallbackChain(chain);
    CHECK(lib.fallbackChain().size() == names.size() + 1,
          "text: 平台 Resolver 注入的名字进了回退链");
    CHECK(lib.faceName(0) == "Test Alias", "text: 链上 0 号 = Resolver 给的面");
    lib.setDefaultFace(names[0]);

    // ---- 1. 逐字形回退：解析到的面必须真的画得出该码点 ----
    {
        struct Case { const char* name; uint32_t cp; };
        const Case cases[] = { { "拉丁 'A'", 'A' }, { "中文 U+4E2D", 0x4E2D } };
        for (const auto& c : cases) {
            const int idx = lib.faceIndexFor(c.cp);
            char msg[192];
            std::snprintf(msg, sizeof(msg), "text: faceIndexFor(%s) 命中链上 %d", c.name, idx);
            if (idx < 0 && c.cp == 0x4E2D) {
                std::printf("[skip] 本机没有中文字体，跳过 U+4E2D\n");
                continue;
            }
            CHECK(idx >= 0, msg);
            if (idx < 0) continue;
            GrayGlyph g;
            const bool ok = lib.renderGlyph(lib.faceName(idx), 16.0f, c.cp, g);
            std::snprintf(msg, sizeof(msg), "text: %s 解析到的面确实有字形（adv=%d）",
                          c.name, ok ? g.advance : 0);
            CHECK(ok && g.advance > 0, msg);
        }
        CHECK(lib.faceIndexFor(0x10FFFD) == -1, "text: 私用区末尾判定为无字形");
    }

    // ---- 2. 豆腐块：四边有墨、中间空心、advance 合理 ----
    for (float size : { 12.0f, 14.0f, 32.0f }) {
        GrayGlyph g;
        const bool ok = lib.renderMissingGlyph(size, g);
        char msg[192];
        std::snprintf(msg, sizeof(msg), "text: 豆腐块 %.0fpx 有尺寸且 advance >= 宽（%dx%d）",
                      size, g.width, g.height);
        CHECK(ok && g.width > 0 && g.height > 0 && g.advance >= g.width, msg);
        if (!ok || !g.alpha) continue;
        auto at = [&](int x, int y) {
            return static_cast<int>(g.alpha[static_cast<size_t>(y) * g.pitch + x]);
        };
        const int mx = g.width / 2, my = g.height / 2;
        std::snprintf(msg, sizeof(msg), "text: 豆腐块 %.0fpx 四边有墨", size);
        CHECK(at(mx, 0) > 0 && at(mx, g.height - 1) > 0 && at(0, my) > 0 &&
                  at(g.width - 1, my) > 0, msg);
        std::snprintf(msg, sizeof(msg), "text: 豆腐块 %.0fpx 中间空心", size);
        CHECK(at(mx, my) == 0, msg);
    }

    // ---- 3. 彩色字形（COLR/CPAL）----
    // 只有本机真有彩色 emoji 面时才验（否则这一步在本机没有意义）。
    const bool hasEmojiFace = !emojiPath.empty();
    if (hasEmojiFace) {
        const uint32_t cps[] = { 0x1F600 };   // 😀
        ColorGlyph cg;
        const bool ok = lib.renderColorCluster(emojiFamily, 32.0f, cps, 1, cg);
        CHECK(ok, "text: emoji 簇出彩色位图（COLR/CPAL）");
        if (ok) {
            CHECK(cg.width > 0 && cg.height > 0 && cg.bgra, "text: 彩色位图有尺寸");
            long long alpha = 0;
            bool chroma = false;
            for (int y = 0; y < cg.height; y++) {
                const uint8_t* row = cg.bgra + static_cast<size_t>(y) * cg.width * 4;
                for (int x = 0; x < cg.width; x++) {
                    const uint8_t* p = row + static_cast<size_t>(x) * 4;
                    alpha += p[3];
                    if (p[3] && (p[0] != p[1] || p[1] != p[2])) chroma = true;
                }
            }
            CHECK(alpha > 0, "text: 彩色位图非全透明");
            CHECK(chroma, "text: 彩色位图真的有彩色分量（不是灰度）");
            CHECK(cg.advance > 0, "text: 彩色簇 advance > 0");
        }
    } else {
        std::printf("[skip] 本机没有彩色 emoji 面，跳过 COLR/CPAL 用例\n");
    }

    // ---- 图集（CPU 出口）：测宽 / 换行粒度 / 多层阴影 ----
    {
        Surface s(240, 80);
        Canvas canvas(s);
        Atlas atlas;
        CHECK(atlas.init(&lib), "text: Atlas 初始化");
        atlas.setFontName(names[0].c_str());
        atlas.setCanvas(&canvas);

        // 测宽与簇度量同源
        const std::string text = "Ham";
        const float w = atlas.textWidth(16.0f, 400, text, false);
        const std::vector<uint32_t> cps = Utf8ToCodepoints(text);
        float sum = 0;
        size_t i = 0;
        while (i < cps.size()) {
            const Cluster c = atlas.measureCluster(16.0f, 400, cps.data() + i, cps.size() - i,
                                                   false);
            sum += c.advance + atlas.letterSpacing();
            i += c.len;
        }
        CHECK_NEAR(w, sum, 0.01, "text: textWidth == 逐簇 advance 之和");

        // 文字度量：ascent/descent 合理
        const Metrics m = atlas.metrics(16.0f, 400, false);
        CHECK(m.ascent > 0 && m.descent >= 0 && m.height() >= 10,
              "text: metrics 在 16px 下合理");

        // emoji 簇（ZWJ 连写）在簇切分里是一个簇 —— 簇长度是模块的判据
        {
            const std::vector<uint32_t> fam = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467 };
            const Cluster c = atlas.measureCluster(24.0f, 400, fam.data(), fam.size(), false);
            CHECK(c.len == 5, "text: ZWJ 家庭 emoji 切成一个簇（长度 5）");
        }

        // 无阴影：只画字
        const long long inkPlain = [&] {
            Surface t(240, 80);
            Canvas c(t);
            atlas.setCanvas(&c);
            atlas.clearTextShadow();
            atlas.drawText(10.0f, 50.0f, 24.0f, 400, "Shadow", 0xFFFFFFFFu, false);
            return InkOf(t);
        }();
        CHECK(inkPlain > 0, "text: 无阴影时画出了字");

        // 多层 text-shadow：两层各自成图集格，墨量必须更多
        const long long inkShadow = [&] {
            Surface t(240, 80);
            Canvas c(t);
            atlas.setCanvas(&c);
            ShadowLayer layers[2];
            layers[0].dx = 3.0f;  layers[0].dy = 3.0f;  layers[0].blur = 0.0f;
            layers[0].color = 0x80000000u;
            layers[1].dx = 6.0f;  layers[1].dy = 6.0f;  layers[1].blur = 4.0f;
            layers[1].color = 0x60000000u;
            atlas.setTextShadows(layers, 2);
            CHECK(atlas.textShadowCount() == 2, "text: 两层阴影都进了状态");
            atlas.drawText(10.0f, 50.0f, 24.0f, 400, "Shadow", 0xFFFFFFFFu, false);
            atlas.clearTextShadow();
            return InkOf(t);
        }();
        CHECK(inkShadow > inkPlain, "text: 两层 text-shadow 让墨量变多（舒适层真的画了）");

        // 换 Canvas 不能把 CPU 出口的图集纹理扔掉（每帧重建 Canvas 是上层的常态）
        {
            Surface t2(240, 80);
            Canvas c2(t2);
            atlas.setCanvas(&c2);
            atlas.drawText(10.0f, 50.0f, 24.0f, 400, "Shadow", 0xFFFFFFFFu, false);
            CHECK(InkOf(t2) == inkPlain, "text: 换 Canvas 后同一串字像素不变（图集没被清）");
        }
    }

    // ---- 彩色字形在图集这条路上也是彩色（不是白字预乘）----
    // 这里单独用一条**只有 emoji 面**的链：彩色路径只在"该码点确实由 emoji 面覆盖"
    // 时才走（符号命中 Simbol 面就该是黑白），所以要让判据里那一支成立。
    if (hasEmojiFace) {
        Library emojiLib;
        emojiLib.init();
        emojiLib.addFont(emojiFamily, emojiPath);
        emojiLib.setDefaultFace(emojiFamily);
        emojiLib.setFallbackChain({ emojiFamily });

        Surface s(64, 64);
        Canvas canvas(s);
        Atlas atlas;
        CHECK(atlas.init(&emojiLib), "text: emoji-only 链上的 Atlas 初始化");
        atlas.setFontName(emojiFamily.c_str());
        atlas.setCanvas(&canvas);
        atlas.drawText(4.0f, 48.0f, 32.0f, 400, "\xF0\x9F\x98\x80", 0xFFFFFFFFu, false);
        CHECK(HasChromaticPixel(s), "text: 图集路径上 emoji 出彩色像素");
    }

    return Bin2dTestSummary("text");
}
