// glyph_atlas.cpp —— 字形图集：打包、缓存、文字阴影模糊图集、逐字形发射
//
// 这份代码是"几千个小位图怎么进一张纹理"的全部实现，迁移前后**逐字节相同**：
// 图集尺寸/格子布局/缓存键/阴影核/模糊半径上限都没有动，只换了所在的位置。
// 移动的判据只有一条：它不认识 CSS、不认识节点、不认识窗口。
//
// 纹理出口有两条（见 TextureStore）：
//   · 挂了 GPU 执行体的 Canvas → 外部纹理通道（create/upload/destroy），
//     每次只上传新格子那一块矩形。图集是边画边贴，整张重传既贵又有先后关系问题；
//   · 纯 CPU 的 Canvas → 模块自有的 Image + drawImage，逐像素立即落盘。
// 两条出口喂的是同一份 BGRA 位图，差异只在采样口径（见各出口的注释）。
#include "bin2d/image.h"
#include "bin2d/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bin2d/canvas.h"
#include "text_unicode.h"

namespace bin2d {
namespace text {

namespace {

// 预乘 BGRA（图集位图）→ 预乘 Surface：只换字节序，**不再乘一次 alpha**。
//
// 字形位图出来时已经是**预乘白色**（`p[0]=p[1]=p[2]=p[3]=覆盖率`）。曾经这里按
// "直通"再预乘一次，于是每个纹素变成 `rgb = a²/255, alpha = a` —— 覆盖率在 RGB 上
// 被平方了。症状很具体：白字压在浅底上时字缘比底色更暗，一眼是"白字带灰边"。
void BlitBgraPremulInto(Surface& dst, int x, int y, int w, int h, const uint8_t* bgra,
                        int srcPitch) {
    for (int row = 0; row < h; ++row) {
        const int dy = y + row;
        if (dy < 0 || dy >= dst.height()) continue;
        const uint8_t* s = bgra + static_cast<size_t>(row) * static_cast<size_t>(srcPitch);
        uint8_t* d = dst.row(dy);
        for (int col = 0; col < w; ++col) {
            const int dx = x + col;
            if (dx < 0 || dx >= dst.width()) continue;
            const uint8_t* sp = s + static_cast<size_t>(col) * 4;
            uint8_t* dp = d + static_cast<size_t>(dx) * 4;
            dp[0] = sp[2];   // B G R A → R G B A（分量照抄，alpha 已预乘过）
            dp[1] = sp[1];
            dp[2] = sp[0];
            dp[3] = sp[3];
        }
    }
}

// ------------------------------------------------------------------
// 纹理出口：位图 → 可采样的纹理 → 一个四边形
//
// 句柄就是 uint64：GPU 路是 Canvas::createTexture 的返回值，CPU 路是自有 Image 的
// 地址（同一个进程里唯一，且生命周期由此处的 owned 管）。两条路上"句柄"的语义
// 一致 —— 调用方只负责把它交给 store.draw。
// ------------------------------------------------------------------
struct TextureStore {
    Canvas* canvas = nullptr;
    bool device = false;                        // true = 句柄归 GPU 执行体
    std::vector<std::unique_ptr<Image>> owned;  // CPU 路：图集纹理就是这些 Image
    std::vector<uint8_t> swz;                   // BGRA → RGBA 交换缓冲

    bool ready() const { return canvas != nullptr; }

    uint64_t createTex(int w, int h) {
        if (!canvas || w <= 0 || h <= 0) return 0;
        if (device) {
            // 必须全 0（透明）：图集里的"空格子"会被当真字形画，未初始化的显存
            // 会画成垃圾；执行体的 createTexture 自带零上传。
            return canvas->createTexture(w, h, /*nearest=*/false);
        }
        // 必须全 0（透明）：Surface::resize 已经保证清零。
        auto img = std::make_unique<Image>(w, h);
        if (img->empty()) return 0;
        const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(img.get()));
        owned.push_back(std::move(img));
        return handle;
    }

    void updateTex(uint64_t tex, int x, int y, int w, int h, const uint8_t* bgra) {
        if (!tex || !bgra || w <= 0 || h <= 0 || !canvas) return;
        if (device) {
            // 只传这一块：执行体的上传是同步的、图集只追加不改写，所以先传后画/
            // 先画后传在设备上看到的都是同一份最终内容。
            const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
            swz.resize(n * 4u);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t* s = bgra + i * 4u;
                uint8_t* d = swz.data() + i * 4u;
                d[0] = s[2];   // B G R A → R G B A
                d[1] = s[1];
                d[2] = s[0];
                d[3] = s[3];
            }
            canvas->uploadTexture(tex,
                                  Rect(static_cast<float>(x), static_cast<float>(y),
                                       static_cast<float>(w), static_cast<float>(h)),
                                  swz.data(), static_cast<uint32_t>(w) * 4u);
            return;
        }
        auto* img = reinterpret_cast<Image*>(static_cast<uintptr_t>(tex));
        if (!img) return;
        // 字形位图是**预乘**的（覆盖率同时写在 RGB 和 alpha 上），别再预乘一次
        BlitBgraPremulInto(img->surface(), x, y, w, h, bgra, w * 4);
        // 原地改完必须作废执行体的纹理缓存（CPU 执行体上这是空操作）
        canvas->imageChanged(img->surface());
    }

    void destroyTex(uint64_t tex) {
        if (!tex) return;
        if (device) {
            // 执行体的 destroy 是延迟的（帧尾之后才真正释放），所以帧内销毁 + 重建
            // 不会让已经录好的笔采样到新纹理。
            if (canvas) canvas->destroyTexture(tex);
            return;
        }
        for (size_t i = 0; i < owned.size(); ++i) {
            if (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owned[i].get())) == tex) {
                owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    void drawTex(uint64_t tex, float x, float y, float w, float h, float u0, float v0, float u1,
                 float v1, uint32_t argb) {
        if (!canvas || !tex) return;
        const Rect dst(x, y, w, h);
        const Rect uv(u0, v0, u1 - u0, v1 - v0);
        const Color c = Color::argb(argb);
        if (device) {
            canvas->drawTexture(tex, dst, uv, c);
            return;
        }
        auto* img = reinterpret_cast<Image*>(static_cast<uintptr_t>(tex));
        if (!img || img->empty()) return;
        // 与迁移前同一条采样口径（Linear）：字形位图是 1:1 落位的，UV 又是整纹素
        // 边界，所以这条过滤器不会真的插值 —— 但改它就会改像素，别顺手改。
        canvas->drawImage(*img, dst, uv, c, ImageFilter::Linear);
    }

    void clearOwned() {
        owned.clear();
        swz.clear();
        swz.shrink_to_fit();
    }
};

} // namespace

// ============================================================
// 图集内部结构
// ============================================================
struct Atlas::Impl {
    // 一张图集里的一个字形：格子下标 + advance
    struct Glyph {
        uint16_t cell = 0;
        uint8_t advW = 0;
    };

    struct GlyphAtlasPage {
        uint64_t tex = 0;
        int sizePx = 1024;
        int cellW = 0, cellH = 0;
        int cols = 0;
        int next = 1;             // 格子 0 保留给"纯色"（见 rasterGlyph 的空白字形分支）
        int ascent = 0, descent = 0;
        float size = 0;
        int weight = 400;
        int faceIdx = 0;          // 回退链上的面下标（0 = 主面；豆腐块也放这里）
        bool italic = false;
        std::string face;
        bool warnedFull = false;  // 图集满时只喊一次
        std::unordered_map<uint32_t, Glyph> glyphs;
    };

    struct ColorGlyphCell {
        uint16_t cell = 0;
        uint16_t advW = 0;
        uint16_t w = 0;
        uint16_t h = 0;
        uint16_t baseline = 0;
    };

    struct ColorAtlasPage {
        uint64_t tex = 0;
        int sizePx = 1024;
        int cellW = 0, cellH = 0;
        int cols = 0;
        int next = 0;
        float size = 0;
        bool warnedFull = false;
        std::unordered_map<uint64_t, ColorGlyphCell> glyphs;
    };

    // ---------- 文字阴影的模糊图集 ----------
    // 阴影要的是"字形的模糊覆盖率"，与字形图集不同：格子每边要大出一圈模糊余量
    // （pad），而且内容是可复用的灰度覆盖率。所以按 (字号, 字重, 斜体, 面, 模糊半径)
    // 单独开图集 —— 一个页面通常只有一两个模糊半径，实际就一两张纹理。
    struct ShadowGlyph {
        uint16_t cell = 0;
    };
    struct ShadowAtlasPage {
        uint64_t tex = 0;
        int sizePx = 1024;
        int cell = 0;        // 格子边长 = 基础字形格 + 2*pad
        int pad = 0;         // 每边外扩的模糊余量（像素）
        int cols = 0;
        int next = 0;
        float size = 0;
        int weight = 400;
        int faceIdx = 0;     // 合成面下标（回退链）
        bool italic = false;
        int blurQ = 0;       // ¼ 像素量化的模糊半径
        float blur = 0.0f;
        bool warnedFull = false;
        std::unordered_map<uint32_t, ShadowGlyph> glyphs;   // 码点（相位 0）→ 格
    };

    // 一个字形对应的阴影格引用（UV + 外扩量）
    struct ShadowRef {
        uint64_t tex = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        float pad = 0;
    };

    // 当前生效的文字阴影（由 setTextShadows 写入）：一层一个条目，数组顺序 = 绘制顺序
    struct ShadowStateLayer {
        float dx = 0, dy = 0, blur = 0;
        int blurQ = 0;
        uint32_t color = 0;
    };
    struct ShadowState {
        int count = 0;
        ShadowStateLayer layer[kMaxTextShadows];
    };

    // 整形结果缓存里的一格四边形（相对 (0, baseline) 的位置）
    struct RunQuad {
        uint64_t tex = 0;
        float x = 0, y = 0, w = 0, h = 0;
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        bool white = false;   // 彩色字形用白顶点（不染色）
        // 同一字形的每一层阴影格（与 setTextShadows 给进来的层一一对应）：
        //   blur = 0 → 直接复用字形格（tex == 本四边形的 tex、同 UV、pad = 0）；
        //   blur > 0 → 模糊图集里的一格，每边比字形格多 pad 像素，画的时候按 pad 外扩。
        // 彩色字形（emoji）不挂阴影，tex 保持 0。
        ShadowRef s[kMaxTextShadows];
    };
    // 一整条整形结果：同一串文本每帧都要打，而"码点 → 面 → 图集格子"这条链每次
    // 都要 UTF-8 解码 + 逐簇 faceIdxForCp + 两级哈希查表。静态页面上这些结果其实
    // **永远不变**，所以按 (文本, 字号, 字重, 斜体, 字体面, 相位, 字间距, 阴影签名)
    // 缓存一份四边形表，命中时只做发射。图集代数一变整表作废。
    struct Run {
        uint64_t gen = 0;                 // 建立时的图集代数
        float advance = 0;                // 总前进宽（textWidth 直接复用）
        float lineH = 0;                  // 行高
        float ascent = 0;                 // 基线偏移
        std::vector<RunQuad> quads;
        uint64_t lastUse = 0;             // LRU 时钟
    };

    struct ClusterMetric {
        size_t len = 1;
        float adv = 0;
        bool color = false;
    };

    // ---- 状态 ----
    Library* lib = nullptr;
    bool fontReady = false;
    bool hasCanvas = false;
    TextureStore store;

    std::string fontName = "Segoe UI";   // 主 face 名（回退链 0 号）
    float letterSpacing = 0.0f;
    bool snap = true;
    ShadowState shadow;

    std::unordered_map<uint64_t, std::vector<GlyphAtlasPage*>> pages;
    std::unordered_map<uint32_t, ColorAtlasPage*> colorPages;
    std::vector<ShadowAtlasPage*> shadowPages;
    std::unordered_map<uint64_t, Run> runs;
    uint64_t runClock = 0;
    uint64_t atlasGen = 1;   // 任何纹理新建/销毁都 ++，缓存条目按它判断是否过期
    std::vector<uint8_t> scratch;   // 光栅化临时缓冲（cellW*cellH*4，BGRA）
    bool emojiIdxInit = false;
    int emojiIdx = -1;

    static constexpr const char* kEmojiFont = "Segoe UI Emoji";
    static constexpr size_t kMaxRuns = 1024;   // 整形缓存条目上限（LRU）
    // 文字阴影的模糊半径上限（¼ 像素单位：64 = 16px）。再大就只是"一团灰"，
    // 而图集格子会膨胀到装不下几个字形 —— 超过的按上限处理。
    static constexpr int kMaxShadowBlurQ = 64;

    ~Impl() { clearAll(); }

    // ---- 代数与缓存 ----
    void bumpGen() {
        ++atlasGen;
        runs.clear();
    }

    void clearAll() {
        ++atlasGen;
        runs.clear();
        for (auto& kv : pages) {
            for (GlyphAtlasPage* a : kv.second) {
                if (a->tex) store.destroyTex(a->tex);
                delete a;
            }
        }
        pages.clear();
        for (auto& kv : colorPages) {
            ColorAtlasPage* a = kv.second;
            if (a->tex) store.destroyTex(a->tex);
            delete a;
        }
        colorPages.clear();
        for (ShadowAtlasPage* sa : shadowPages) {
            if (sa->tex) store.destroyTex(sa->tex);
            delete sa;
        }
        shadowPages.clear();
        shadow.count = 0;
        scratch.clear();
        scratch.shrink_to_fit();
        store.clearOwned();
    }

    // ---- 图集键与取页 ----
    uint64_t atlasKey(float size, int weight, int faceIdx, bool italic) const {
        // 斜体与粗体一样是"另一张图集"：合成倾斜发生在光栅化阶段，不能共用缓存。
        // 低 6 位放面下标（同一个码点解析到不同字体时必须分图集，否则字形会串）。
        const uint32_t fidx = static_cast<uint32_t>(faceIdx < 0 ? 0 : faceIdx) & 0x3Fu;
        return (static_cast<uint64_t>(std::lround(size)) << 32) |
               (static_cast<uint64_t>((weight & 0x7FFF) | (italic ? 0x8000 : 0)) << 16) | fidx;
    }

    // 图集里一个字形格的键：码点 × 4 + 相位（码点 ≤ 0x10FFFF，不会溢出）
    static uint32_t GlyphKey(uint32_t cp, int phase) {
        return (cp << 2) | static_cast<uint32_t>(phase & 3);
    }

    static bool hasRoom(const GlyphAtlasPage* a) {
        return a && a->next < a->cols * (a->sizePx / a->cellH);
    }

    GlyphAtlasPage* makePage(uint64_t key, float size, int weight, int faceIdx, bool italic) {
        if (!store.ready()) return nullptr;
        const uint32_t fidx = static_cast<uint32_t>(faceIdx < 0 ? 0 : faceIdx) & 0x3Fu;
        GlyphAtlasPage* a = new GlyphAtlasPage();
        a->size = size;
        a->weight = weight;
        a->faceIdx = static_cast<int>(fidx);
        a->italic = italic;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH;
        a->cols = a->sizePx / a->cellW;
        a->face = fontReady ? lib->faceName(a->faceIdx) : fontName;
        a->tex = store.createTex(a->sizePx, a->sizePx);
        if (!a->tex) {
            delete a;
            return nullptr;
        }
        if (lib) {
            const Metrics fm = lib->metrics(a->face, size);
            a->ascent = fm.ascent;
            a->descent = fm.descent;
        } else {
            a->ascent = static_cast<int>(std::ceil(size * 0.8f));
            a->descent = static_cast<int>(std::ceil(size * 0.2f));
        }
        pages[key].push_back(a);
        return a;
    }

    // 主图集（不带码点）：测量用。有空间就用，全满则开新一代。
    GlyphAtlasPage* getPage(float size, int weight, int faceIdx, bool italic = false) {
        const uint64_t key = atlasKey(size, weight, faceIdx, italic);
        auto it = pages.find(key);
        if (it != pages.end()) {
            for (GlyphAtlasPage* a : it->second)
                if (hasRoom(a)) return a;
            return it->second.back();   // 全满：交给 rasterGlyph 的告警兜底
        }
        return makePage(key, size, weight, faceIdx, italic);
    }

    // 按码点取图集：优先返回**已经含有该字形**的那一代（UV 必须和纹理对得上），
    // 否则找有空间的一代，都没有就再开一代 —— 图集满了是"多占一张纹理"，
    // 而不是"新字直接不出现"。
    GlyphAtlasPage* getPageForCp(float size, int weight, int faceIdx, bool italic, uint32_t cp,
                                 int phase = 0) {
        const uint64_t key = atlasKey(size, weight, faceIdx, italic);
        const uint32_t gk = GlyphKey(cp, phase);
        auto it = pages.find(key);
        if (it != pages.end()) {
            for (GlyphAtlasPage* a : it->second)
                if (a->glyphs.count(gk)) return a;
            for (GlyphAtlasPage* a : it->second)
                if (hasRoom(a)) return a;
            if (it->second.size() < 8) return makePage(key, size, weight, faceIdx, italic);
            return it->second.back();
        }
        return makePage(key, size, weight, faceIdx, italic);
    }

    // x 量化成"整数落位 + ¼ 像素相位"：0.37 → (floor, 1)；0.9 → (floor+1, 0)。
    // 相位取"最近的 ¼"，所以最坏位置误差 0.125px（肉眼不可见），换来 4 份位图。
    static void QuantizeSubpixelX(float x, float& outX, int& outPhase) {
        const float fl = std::floor(x);
        int ph = static_cast<int>(std::lround((x - fl) * 4.0f));
        float ix = fl;
        if (ph >= 4) { ph = 0; ix = fl + 1.0f; }
        else if (ph < 0) ph = 0;
        outX = ix;
        outPhase = ph & 3;
    }

    // A/B 开关：BIN2D_TEXT_PHASE_OFF=1 退回"小数落位 + 图集线性采样"的行为。
    // 只用来做锐利度前后对比的证据图，运行期读一次环境变量，不参与任何逻辑判断。
    static bool PhaseEnabled() {
        static const bool on = std::getenv("BIN2D_TEXT_PHASE_OFF") == nullptr;
        return on;
    }

    bool ensureScratch(int w, int h) {
        const size_t need = static_cast<size_t>(w) * h * 4;
        if (scratch.size() < need) scratch.resize(need);
        std::memset(scratch.data(), 0, need);
        return true;
    }

    // 把单个码点（指定 ¼ 像素相位）光栅化进图集格子（FreeType 灰度字形 → 白字预乘 alpha）。
    // 相位是**位图的一部分**：renderGlyph 用 FT_Set_Transform 把轮廓先右移 phase/4 像素，
    // 所以同一个字形最多占 4 个格子；只有真的用到某个相位时才造它（懒加载）。
    void rasterGlyph(GlyphAtlasPage* a, uint32_t cp, int phase = 0) {
        if (!lib) return;   // 没有字体服务：字形与豆腐块都画不出，留空
        const int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) {
            // 图集满了：以前这里**静默 return**，新字形直接不出现 —— 用户看到的是
            // "某些字时有时无"，最难查。至少喊一声（同一张图集只喊一次）。
            if (!a->warnedFull) {
                a->warnedFull = true;
                std::fprintf(stderr,
                    "[text] 字形图集已满（%.0fpx / weight %d / face #%d，共 %d 格）："
                    "该组合下的新字形将被丢弃。\n",
                    static_cast<double>(a->size), a->weight, a->faceIdx, total);
            }
            return;
        }
        GrayGlyph g;
        // 粗体：atlas key 里带 weight，粗体是独立的一张图集；这里让 FreeType
        // 合成加粗（FT_GlyphSlot_Embolden），否则 font-weight:700 只是换个空图集重复光栅化
        bool ok = lib->renderGlyph(a->face, a->size, cp, a->weight >= 600, a->italic, phase, g);
        if (!ok && !detail::IsInvisibleCp(cp)) {
            // 缺字 → 画豆腐块。留白会被当成"引擎显示 bug"，空方框才是给用户的
            // 明确信号：这个字体链里没有这个字符。
            ok = lib->renderMissingGlyph(a->size, a->weight >= 600, a->italic, g);
        }
        if (!ok || !g.alpha || g.height <= 0) {
            // 空白字形（如空格）：无像素但有有效 advance —— 仍占一格并缓存，
            // 否则 glyphFor 返回空，测宽/绘制都不推进，单词会挤在一起
            if (ok && g.advance > 0) {
                Glyph gl;
                gl.cell = static_cast<uint16_t>(a->next++);   // 格子保持全零纹理（不可见）
                gl.advW = static_cast<uint8_t>(std::min(255, std::max(0, g.advance)));
                a->glyphs[GlyphKey(cp, phase)] = gl;
            }
            return;
        }

        ensureScratch(a->cellW, a->cellH);
        const int ox = 1 + g.left;
        const int oy = (a->ascent + 1) - g.top;
        uint8_t* dst = scratch.data();
        for (int y = 0; y < g.height; y++) {
            const int py = oy + y;
            if (py < 0 || py >= a->cellH) continue;
            const uint8_t* src = g.alpha + static_cast<size_t>(y) * g.pitch;
            uint8_t* row = dst + static_cast<size_t>(py) * a->cellW * 4;
            for (int x = 0; x < g.width; x++) {
                const int px = ox + x;
                if (px < 0 || px >= a->cellW) continue;
                const uint8_t al = src[x];
                if (al == 0) continue;
                uint8_t* p = row + static_cast<size_t>(px) * 4;
                p[0] = p[1] = p[2] = p[3] = al;   // BGRA 全 = alpha（白字预乘）
            }
        }

        const int cell = a->next++;
        const int cx = (cell % a->cols) * a->cellW;
        const int cy = (cell / a->cols) * a->cellH;
        store.updateTex(a->tex, cx, cy, a->cellW, a->cellH, scratch.data());

        Glyph gl;
        gl.cell = static_cast<uint16_t>(cell);
        // advance 用 HarfBuzz/FreeType 的真实值（不回退成 +1：那样每个字形多占 1px，
        // 文字比真实排版宽、组合符号被推离基字、换行宽度与实际绘制不符）
        gl.advW = static_cast<uint8_t>(std::min(255, std::max(0, g.advance)));
        a->glyphs[GlyphKey(cp, phase)] = gl;
    }

    const Glyph* glyphFor(GlyphAtlasPage* a, uint32_t cp, int phase = 0) {
        const uint32_t gk = GlyphKey(cp, phase);
        if (!a) return nullptr;
        auto it = a->glyphs.find(gk);
        if (it != a->glyphs.end()) return &it->second;
        rasterGlyph(a, cp, phase);
        it = a->glyphs.find(gk);
        return it != a->glyphs.end() ? &it->second : nullptr;
    }

    // ---------- 逐字形回退 ----------
    // 谁有字形是字体自报的 cmap，不是码点区间能推出来的（Unicode 的 Block 只是
    // 管理分组，标准明确说不能用来选字体）。字体服务返回 -1 = 所有候选都没有
    // → 用 0 号面（豆腐块是几何，跟哪个字体无关）。
    int faceIdxForCp(uint32_t cp) {
        if (!fontReady || !lib) return 0;
        const int idx = lib->faceIndexFor(cp);
        return idx < 0 ? 0 : idx;
    }

    // emoji 面在链上的下标（-1 = 没有这个候选）。彩色路径只在这个面上尝试：
    // 真 emoji 走彩色，符号（⌘⌫✕）命中 Segoe UI Symbol 就不会被画成彩色图形。
    int emojiFaceIdx() {
        if (!emojiIdxInit) {
            emojiIdxInit = true;
            emojiIdx = -1;
            if (fontReady && lib) {
                const auto& chain = lib->fallbackChain();
                for (size_t i = 0; i < chain.size(); i++)
                    if (chain[i] == kEmojiFont) { emojiIdx = static_cast<int>(i); break; }
            }
        }
        return emojiIdx;
    }

    // ---------- 彩色字形（COLR/CPAL 合成） ----------
    bool rasterColorCluster(ColorAtlasPage* a, uint64_t key, const uint32_t* cps, size_t n) {
        const int total = a->cols * (a->sizePx / a->cellH);
        if (a->next >= total) {
            if (!a->warnedFull) {
                a->warnedFull = true;
                std::fprintf(stderr,
                    "[text] 彩色字形图集已满（%.0fpx，共 %d 格）：彩色簇将退回灰度/豆腐块。\n",
                    static_cast<double>(a->size), total);
            }
            return false;
        }

        if (!lib) return false;
        ColorGlyph cg;
        if (!lib->renderColorCluster(kEmojiFont, a->size, cps, n, cg)) return false;
        if (cg.width <= 0 || cg.height <= 0 || !cg.bgra) return false;

        ensureScratch(a->cellW, a->cellH);
        const int cw = std::min(cg.width, a->cellW);
        const int ch = std::min(cg.height, a->cellH);
        uint8_t* dst = scratch.data();
        for (int y = 0; y < ch; y++)
            std::memcpy(dst + static_cast<size_t>(y) * a->cellW * 4,
                        cg.bgra + static_cast<size_t>(y) * cg.width * 4,
                        static_cast<size_t>(cw) * 4);

        const int cell = a->next++;
        const int cx = (cell % a->cols) * a->cellW;
        const int cy = (cell / a->cols) * a->cellH;
        store.updateTex(a->tex, cx, cy, a->cellW, a->cellH, scratch.data());

        ColorGlyphCell g;
        g.cell = static_cast<uint16_t>(cell);
        g.advW = static_cast<uint16_t>(std::min(4095, cg.advance + 1));
        g.w = static_cast<uint16_t>(cw);
        g.h = static_cast<uint16_t>(ch);
        g.baseline = static_cast<uint16_t>(std::min(4095, cg.baseline));
        a->glyphs[key] = g;
        return true;
    }

    ColorAtlasPage* getColorPage(float size) {
        const uint32_t key = static_cast<uint32_t>(std::lround(size));
        auto it = colorPages.find(key);
        if (it != colorPages.end()) return it->second;
        if (!store.ready()) return nullptr;

        ColorAtlasPage* a = new ColorAtlasPage();
        a->size = size;
        a->cellH = static_cast<int>(std::ceil(size * 1.4f)) + 2;
        a->cellW = a->cellH * 4;
        a->cols = a->sizePx / a->cellW;
        if (a->cols < 1) { delete a; return nullptr; }
        a->tex = store.createTex(a->sizePx, a->sizePx);
        if (!a->tex) { delete a; return nullptr; }
        colorPages[key] = a;
        return a;
    }

    const ColorGlyphCell* colorGlyphFor(ColorAtlasPage* a, const uint32_t* cps, size_t n) {
        const uint64_t key = detail::EmojiClusterKey(cps, n);
        auto it = a->glyphs.find(key);
        if (it != a->glyphs.end()) return &it->second;
        rasterColorCluster(a, key, cps, n);
        it = a->glyphs.find(key);
        return it != a->glyphs.end() ? &it->second : nullptr;
    }

    // ---------- 文字阴影 ----------
    // 可分离高斯模糊（σ = blur/2，核截断 3σ）。核按权重和归一化：阴影的"深浅"必须
    // 与字形覆盖率线性对应，否则大模糊会整体发灰。
    static void BlurAlpha(std::vector<float>& buf, int w, int h, float sigma) {
        if (sigma <= 0.0f || w <= 0 || h <= 0) return;
        const int rad = std::max(1, static_cast<int>(std::ceil(sigma * 3.0f)));
        std::vector<float> k(static_cast<size_t>(rad) * 2 + 1);
        float sum = 0.0f;
        const float inv = 1.0f / (2.0f * sigma * sigma);
        for (int i = -rad; i <= rad; i++) {
            const float v = std::exp(-static_cast<float>(i * i) * inv);
            k[static_cast<size_t>(i + rad)] = v;
            sum += v;
        }
        for (float& v : k) v /= sum;
        std::vector<float> tmp(buf.size(), 0.0f);
        for (int y = 0; y < h; y++) {
            const float* src = buf.data() + static_cast<size_t>(y) * w;
            float* dst = tmp.data() + static_cast<size_t>(y) * w;
            for (int x = 0; x < w; x++) {
                float acc = 0.0f;
                const int lo = std::max(0, x - rad), hi = std::min(w - 1, x + rad);
                for (int xx = lo; xx <= hi; xx++)
                    acc += src[xx] * k[static_cast<size_t>(xx - x + rad)];
                dst[x] = acc;
            }
        }
        for (int y = 0; y < h; y++) {
            float* dst = buf.data() + static_cast<size_t>(y) * w;
            const int lo = std::max(0, y - rad), hi = std::min(h - 1, y + rad);
            for (int x = 0; x < w; x++) {
                float acc = 0.0f;
                for (int yy = lo; yy <= hi; yy++)
                    acc += tmp[static_cast<size_t>(yy) * w + x] * k[static_cast<size_t>(yy - y + rad)];
                dst[x] = acc;
            }
        }
    }

    // 取（必要时建）某个 (字号, 字重, 斜体, 面, 模糊半径) 的模糊图集。
    // 条目很少（一页通常 1~2 个模糊半径），线性找足够。
    ShadowAtlasPage* getShadowPage(GlyphAtlasPage* base, int blurQ) {
        if (!store.ready() || !base || blurQ <= 0) return nullptr;
        for (ShadowAtlasPage* sa : shadowPages) {
            if (sa->blurQ == blurQ && sa->faceIdx == base->faceIdx &&
                sa->weight == base->weight && sa->italic == base->italic &&
                static_cast<int>(sa->size) == static_cast<int>(base->size))
                return sa;
        }
        // 模糊半径 → σ = blur/2 → 余量 3σ+1
        const float blur = static_cast<float>(blurQ) * 0.25f;
        const float sigma = blur * 0.5f;
        int pad = static_cast<int>(std::ceil(sigma * 3.0f)) + 1;
        int cell = base->cellW + 2 * pad;
        const int kMaxCell = 192;   // 再大就装不下几个格子了，模糊按上限收
        if (cell > kMaxCell) {
            pad = std::max(0, (kMaxCell - base->cellW) / 2);
            cell = base->cellW + 2 * pad;
        }
        if (cell <= 0) return nullptr;
        ShadowAtlasPage* sa = new ShadowAtlasPage();
        sa->size = base->size;
        sa->weight = base->weight;
        sa->faceIdx = base->faceIdx;
        sa->italic = base->italic;
        sa->blurQ = blurQ;
        sa->blur = blur;
        sa->pad = pad;
        sa->cell = cell;
        sa->cols = std::max(1, sa->sizePx / cell);
        sa->tex = store.createTex(sa->sizePx, sa->sizePx);
        if (!sa->tex) { delete sa; return nullptr; }
        shadowPages.push_back(sa);
        return sa;
    }

    // 在模糊图集里造一格：先按相位 0 光栅化字形，把覆盖率放进"基础格 + 每边 pad"
    // 的大格子里，再做高斯模糊 —— 模糊的结果就是这块字形在该半径下的柔影。
    void rasterShadowGlyph(ShadowAtlasPage* sa, GlyphAtlasPage* base, uint32_t cp) {
        if (!lib) return;
        const int total = sa->cols * (sa->sizePx / sa->cell);
        if (sa->next >= total) {
            if (!sa->warnedFull) {
                sa->warnedFull = true;
                std::fprintf(stderr,
                    "[text] 文字阴影图集已满（%.0fpx / blur %.2f / 共 %d 格）："
                    "该组合下新字形的阴影将缺失（文字本身正常）。\n",
                    static_cast<double>(sa->size), static_cast<double>(sa->blur), total);
            }
            return;
        }
        GrayGlyph g;
        bool ok = lib->renderGlyph(base->face, base->size, cp, base->weight >= 600,
                                   base->italic, 0, g);
        if (!ok && !detail::IsInvisibleCp(cp))
            ok = lib->renderMissingGlyph(base->size, base->weight >= 600, base->italic, g);
        // 空白字形（空格等）没有像素：还是占一格全零（与字形图集一致的做法），
        // 免得每次查表都要重新光栅化
        const int cs = sa->cell;
        std::vector<float> cov(static_cast<size_t>(cs) * cs, 0.0f);
        if (ok && g.alpha && g.height > 0) {
            const int ox = 1 + g.left + sa->pad;
            const int oy = (base->ascent + 1) - g.top + sa->pad;
            for (int y = 0; y < g.height; y++) {
                const int py = oy + y;
                if (py < 0 || py >= cs) continue;
                const uint8_t* src = g.alpha + static_cast<size_t>(y) * g.pitch;
                float* row = cov.data() + static_cast<size_t>(py) * cs;
                for (int x = 0; x < g.width; x++) {
                    const int px = ox + x;
                    if (px < 0 || px >= cs) continue;
                    row[px] = static_cast<float>(src[x]);
                }
            }
        } else if (!ok) {
            return;                      // 连豆腐块都画不出：不占格子（下帧再试）
        }

        BlurAlpha(cov, cs, cs, sa->blur * 0.5f);

        ensureScratch(cs, cs);
        uint8_t* dst = scratch.data();
        for (int i = 0; i < cs * cs; i++) {
            int a = static_cast<int>(cov[static_cast<size_t>(i)] + 0.5f);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            // 白字预乘：BGRA 全 = 覆盖率（画的时候用顶点色染成阴影色）
            dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = dst[i * 4 + 3] =
                static_cast<uint8_t>(a);
        }
        const int cell = sa->next++;
        const int cx = (cell % sa->cols) * sa->cell;
        const int cy = (cell / sa->cols) * sa->cell;
        store.updateTex(sa->tex, cx, cy, sa->cell, sa->cell, scratch.data());
        ShadowGlyph sg;
        sg.cell = static_cast<uint16_t>(cell);
        sa->glyphs[cp] = sg;
    }

    // 取一个字形的某一层阴影格。没有阴影 / 造不出来都返回空（调用方跳过这个字的这层）。
    ShadowRef shadowRef(GlyphAtlasPage* base, uint32_t cp, int blurQ) {
        ShadowRef r;
        if (!base || blurQ <= 0) return r;
        ShadowAtlasPage* sa = getShadowPage(base, blurQ);
        if (!sa) return r;
        auto it = sa->glyphs.find(cp);
        if (it == sa->glyphs.end()) {
            rasterShadowGlyph(sa, base, cp);
            it = sa->glyphs.find(cp);
            if (it == sa->glyphs.end()) return r;
        }
        const float cs = static_cast<float>(sa->cell);
        const float t = static_cast<float>(sa->sizePx);
        r.tex = sa->tex;
        r.u0 = (it->second.cell % sa->cols) * cs / t;
        r.v0 = (it->second.cell / sa->cols) * cs / t;
        r.u1 = (it->second.cell % sa->cols) * cs / t + cs / t;
        r.v1 = (it->second.cell / sa->cols) * cs / t + cs / t;
        r.pad = static_cast<float>(sa->pad);
        return r;
    }

    // 给一个字形四边形挂上每层阴影格：
    //   blur = 0 → 复用字形自己那一格（阴影就是偏移一份字形，最精确、零额外纹理）；
    //   blur > 0 → 模糊图集里的一格（每边多 pad 像素）。
    void FillShadowQuad(RunQuad& q, GlyphAtlasPage* a, uint32_t cp, float gx, float gy, float cell,
                        float t) {
        for (int li = 0; li < shadow.count; li++) {
            const int blurQ = shadow.layer[li].blurQ;
            ShadowRef& r = q.s[li];
            if (blurQ <= 0) {
                r.tex = a->tex;
                r.u0 = gx / t; r.v0 = gy / t;
                r.u1 = (gx + cell) / t; r.v1 = (gy + cell) / t;
                r.pad = 0.0f;
                continue;
            }
            const ShadowRef sg = shadowRef(a, cp, blurQ);
            if (!sg.tex) continue;
            r.tex = sg.tex;
            r.u0 = sg.u0; r.v0 = sg.v0; r.u1 = sg.u1; r.v1 = sg.v1;
            r.pad = sg.pad;
        }
    }

    // 一个簇的度量：彩色优先（只在 emoji 面覆盖时才尝试），否则逐字形灰度。
    ClusterMetric clusterMetric(float size, int weight, const uint32_t* cps, size_t n,
                                bool italic) {
        ClusterMetric m;
        m.len = EmojiClusterLen(cps, n);
        const float fallback = std::ceil(size * 0.6f);
        const int firstFace = faceIdxForCp(cps[0]);
        // 彩色只在该码点确实由 emoji 面覆盖时才尝试：符号（⌘⌫✕）命中 Segoe UI Symbol
        // 就走灰度，不会被画成彩色图形；真 emoji 才走 COLR。
        const bool tryColor = fontReady && (m.len > 1 || firstFace == emojiFaceIdx());
        if (tryColor) {
            ColorAtlasPage* ca = getColorPage(size);
            const ColorGlyphCell* g = ca ? colorGlyphFor(ca, cps, m.len) : nullptr;
            if (g) {
                m.adv = static_cast<float>(g->advW);
                m.color = true;
                return m;
            }
        }
        if (m.len <= 1) {
            GlyphAtlasPage* a = getPageForCp(size, weight, firstFace, italic, cps[0]);
            const Glyph* g = a ? glyphFor(a, cps[0]) : nullptr;
            m.adv = g ? static_cast<float>(g->advW) : fallback;
            return m;
        }
        for (size_t i = 0; i < m.len; i++) {
            GlyphAtlasPage* a = getPageForCp(size, weight, faceIdxForCp(cps[i]), italic, cps[i]);
            const Glyph* g = a ? glyphFor(a, cps[i]) : nullptr;
            m.adv += g ? static_cast<float>(g->advW) : fallback;
        }
        return m;
    }

    // ---- 整形缓存的键 ----
    // 文本 + 字号 + 字重 + 斜体 + 字体面 + 亚像素相位 + 字间距（1/4px 量化）+ 阴影签名
    uint64_t runKey(const std::string& s, float size, int weight, bool italic,
                    int phase = 0) const {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; i++) {
                h ^= (v >> (i * 8)) & 0xFF;
                h *= 1099511628211ull;
            }
        };
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        uint32_t su = 0;
        std::memcpy(&su, &size, sizeof(su));
        mix(su);
        mix(static_cast<uint64_t>(weight));
        mix(italic ? 1u : 0u);
        mix(static_cast<uint64_t>(phase & 3));
        // letter-spacing 参与 key（¼ 像素量化）—— 间距不同就是另一份 run
        mix(static_cast<uint64_t>(static_cast<uint32_t>(
            static_cast<int32_t>(std::lround(letterSpacing * 4.0f)))));
        // **阴影签名**参与 key（层数 + 每层的量化模糊半径）：不混进来会串味，
        // 两个方向都踩得到 ——
        //   1) 同一句话在两个节点上模糊半径不同 → 烘进去的阴影 UV 不通用，
        //      第二个节点会默默画成第一个节点的模糊半径；
        //   2) 同一句话先被**无阴影**节点排版、再被有阴影的节点复用 → 命中缓存
        //      直接重放，阴影整条丢失。
        // 只混模糊半径：dx/dy/颜色是发射期叠加的，不进位图。
        mix(static_cast<uint64_t>(shadow.count));
        for (int li = 0; li < shadow.count; li++)
            mix(static_cast<uint64_t>(shadow.layer[li].blurQ));
        for (unsigned char c : fontName) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

    // 重放整形结果：颜色按调用方给的（灰字形染色，彩色字形白顶点）。
    // 有文字阴影时**先逐层整条阴影、再整条字形** —— 逐字交错会让后面那个字的阴影
    // 压到前面那个字上（半透明文字会看出脏边）。多层时"层"在外、"字"在内：
    // 层序 = 绘制顺序（下标 0 在最下面）。
    void emitRun(const Run& run, float x, float baseline, uint32_t color) {
        for (int li = 0; li < shadow.count; li++) {
            const ShadowStateLayer& L = shadow.layer[li];
            uint32_t shColor = L.color;
            detail::PremultColor(shColor);
            for (const RunQuad& q : run.quads) {
                const ShadowRef& r = q.s[li];
                if (!r.tex) continue;
                store.drawTex(r.tex, x + q.x - r.pad + L.dx, baseline + q.y - r.pad + L.dy,
                              q.w + 2.0f * r.pad, q.h + 2.0f * r.pad, r.u0, r.v0, r.u1, r.v1,
                              shColor);
            }
        }
        for (const RunQuad& q : run.quads) {
            if (!q.tex) continue;
            store.drawTex(q.tex, x + q.x, baseline + q.y, q.w, q.h, q.u0, q.v0, q.u1, q.v1,
                          q.white ? 0xFFFFFFFFu : color);
        }
    }

    // 一整条 run 的构建（整形缓存未命中时走这里）
    void buildRun(const std::string& utf8, float size, int weight, bool italic, int phase,
                  float qx, float baseline, uint64_t key, uint32_t premult) {
        const int ascent = pageAscent(size, weight, italic);
        const int lineH = pageLineHeight(size, weight, italic);

        const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);

        // 边画边记一份 run（相对 x=0、基线 = 0），下次直接重放
        Run built;
        built.gen = atlasGen;
        built.lineH = static_cast<float>(lineH);
        built.ascent = static_cast<float>(ascent);
        const float originX = qx;
        const float originBaseline = baseline;
        auto record = [&](uint64_t tex, float qx0, float qy0, float qw, float qh,
                          float gx, float gy, float gw, float gh, float t, bool white) -> RunQuad& {
            RunQuad q;
            q.tex = tex;
            q.x = qx0 - originX;
            q.y = qy0 - originBaseline;
            q.w = qw; q.h = qh;
            q.u0 = gx / t; q.v0 = gy / t;
            q.u1 = (gx + gw) / t; q.v1 = (gy + gh) / t;
            q.white = white;
            built.quads.push_back(q);
            return built.quads.back();
        };

        float cx = qx;
        auto drawGray = [&](uint32_t cp) {
            GlyphAtlasPage* a = getPageForCp(size, weight, faceIdxForCp(cp), italic, cp, phase);
            if (!a) return;
            const Glyph* g = glyphFor(a, cp, phase);
            if (!g) return;
            const float t = static_cast<float>(a->sizePx);
            const float cell = static_cast<float>(a->cellW);
            const float gx = (g->cell % a->cols) * cell;
            const float gy = (g->cell / a->cols) * cell;
            const float top = baseline - (a->ascent + 1);
            RunQuad& q = record(a->tex, cx, top, cell, cell, gx, gy, cell, cell, t, false);
            FillShadowQuad(q, a, cp, gx, gy, cell, t);
            cx += g->advW;
        };

        size_t i = 0;
        while (i < cps.size()) {
            const size_t n = EmojiClusterLen(cps.data() + i, cps.size() - i);
            // 彩色只在该码点确实由 emoji 面覆盖时才尝试（真 emoji 走 COLR；符号走灰度）
            const bool colorCp = fontReady &&
                                 (n > 1 || faceIdxForCp(cps[i]) == emojiFaceIdx());
            if (colorCp) {
                ColorAtlasPage* ca = getColorPage(size);
                const ColorGlyphCell* g = ca ? colorGlyphFor(ca, cps.data() + i, n) : nullptr;
                if (g) {
                    const float t = static_cast<float>(ca->sizePx);
                    const float ccw = static_cast<float>(ca->cellW);
                    const float gx = (g->cell % ca->cols) * ccw;
                    const float gy = (g->cell / ca->cols) * ca->cellH;
                    const float top = baseline - g->baseline;
                    // 彩色字形不挂文字阴影：阴影要用"覆盖率"染色，而彩色格子里
                    // 每个像素自带颜色（乘上阴影色会糊成一团彩色，不是剪影）。
                    record(ca->tex, cx, top, static_cast<float>(g->w), static_cast<float>(g->h),
                           gx, gy, static_cast<float>(g->w), static_cast<float>(g->h), t, true);
                    cx += g->advW + letterSpacing;
                    i += n;
                    continue;
                }
            }
            for (size_t k = 0; k < n; k++) drawGray(cps[i + k]);
            cx += letterSpacing;      // letter-spacing 按"字形簇"加一次（含末尾那个）
            i += n;
        }

        built.advance = cx - originX;
        built.lastUse = ++runClock;
        // 缓存上限（主题/字号一多会一路涨）：超了按 LRU 淘汰
        if (runs.size() >= kMaxRuns && !built.quads.empty()) {
            auto victim = runs.begin();
            for (auto k = runs.begin(); k != runs.end(); ++k)
                if (k->second.lastUse < victim->second.lastUse) victim = k;
            runs.erase(victim);
        }
        if (!built.quads.empty()) {
            emitRun(built, qx, baseline, premult);
            runs.emplace(key, std::move(built));
        }
    }

    int pageAscent(float size, int weight, bool italic) {
        GlyphAtlasPage* a = getPage(size, weight, 0, italic);
        return a ? a->ascent : static_cast<int>(size);
    }
    int pageLineHeight(float size, int weight, bool italic) {
        GlyphAtlasPage* a = getPage(size, weight, 0, italic);
        return a ? a->ascent + a->descent : static_cast<int>(size);
    }
};

// ============================================================
// Atlas 公开接口
// ============================================================
Atlas::Atlas() : impl_(new Impl()) {}
Atlas::~Atlas() { delete impl_; }

bool Atlas::init(Library* lib) {
    impl_->lib = lib;
    impl_->fontReady = lib && lib->init();
    return impl_->fontReady;
}

bool Atlas::fontReady() const { return impl_->fontReady; }

void Atlas::setCanvas(Canvas* canvas) {
    const bool device = canvas && canvas->gpuAttached();
    // 先换出口指针：上层可能刚把旧 Canvas 析构掉（beginFrame 每帧重建一个），
    // 旧指针不能留到销毁纹理的时候才用。
    impl_->store.canvas = canvas;
    // 出口变了（CPU 的 Image ↔ 执行体的设备纹理）：旧纹理在新出口上既销毁不了、
    // 也不能再采样，必须先按**旧**出口清掉（此刻 device 还是旧值，销毁走对的通道）。
    // CPU 路每帧重建 Canvas 只是换了指针，出口没变，此时**不能**清 ——
    // 清了就是每帧重光栅整页文字。
    if (impl_->hasCanvas && device != impl_->store.device) impl_->clearAll();
    impl_->store.device = device;
    if (canvas) impl_->hasCanvas = true;
}

Canvas* Atlas::canvas() const { return impl_->store.canvas; }

void Atlas::setFontName(const char* name) {
    if (name && *name && impl_->fontName != name) {
        impl_->fontName = name;
        impl_->bumpGen();   // 换 face → 整形缓存里的 tex/UV 全部作废
    }
}

const std::string& Atlas::fontName() const { return impl_->fontName; }

void Atlas::setLetterSpacing(float px) { impl_->letterSpacing = px; }
float Atlas::letterSpacing() const { return impl_->letterSpacing; }

void Atlas::setSnapText(bool on) { impl_->snap = on; }
bool Atlas::snapText() const { return impl_->snap; }

void Atlas::setTextShadows(const ShadowLayer* layers, int count) {
    impl_->shadow.count = 0;
    if (!layers || count <= 0) return;
    if (count > kMaxTextShadows) count = kMaxTextShadows;
    for (int i = 0; i < count; i++) {
        const ShadowLayer& src = layers[i];
        if (((src.color >> 24) & 0xFFu) == 0) continue;   // 全透明的层不占状态
        Impl::ShadowStateLayer& d = impl_->shadow.layer[impl_->shadow.count++];
        d.dx = src.dx;
        d.dy = src.dy;
        d.blur = src.blur > 0.0f ? src.blur : 0.0f;
        d.color = src.color;
        int q = static_cast<int>(std::lround(d.blur * 4.0f));
        if (q < 2) q = 0;                 // < 0.5px 的模糊等于没模糊（硬阴影）
        if (q > Impl::kMaxShadowBlurQ) q = Impl::kMaxShadowBlurQ;
        d.blurQ = q;
    }
}

void Atlas::clearTextShadow() { impl_->shadow.count = 0; }
bool Atlas::textShadowActive() const { return impl_->shadow.count > 0; }
int Atlas::textShadowCount() const { return impl_->shadow.count; }

float Atlas::textWidth(float size, int weight, const std::string& utf8, bool italic) {
    if (!impl_->hasCanvas || utf8.empty()) return 0;
    const uint64_t key = impl_->runKey(utf8, size, weight, italic);
    auto it = impl_->runs.find(key);
    if (it != impl_->runs.end() && it->second.gen == impl_->atlasGen) {
        it->second.lastUse = ++impl_->runClock;
        return it->second.advance;
    }
    const std::vector<uint32_t> cps = Utf8ToCodepoints(utf8);
    float w = 0;
    size_t i = 0;
    while (i < cps.size()) {
        const Impl::ClusterMetric m =
            impl_->clusterMetric(size, weight, cps.data() + i, cps.size() - i, italic);
        w += m.adv + impl_->letterSpacing;
        i += m.len;
    }
    return w;
}

Metrics Atlas::metrics(float size, int weight, bool italic) {
    Metrics m;
    Impl::GlyphAtlasPage* a = impl_->getPage(size, weight, 0, italic);
    if (!a) {
        m.ascent = m.descent = static_cast<int>(size);
        return m;
    }
    m.ascent = a->ascent;
    m.descent = a->descent;
    return m;
}

Cluster Atlas::measureCluster(float size, int weight, const uint32_t* cps, size_t n,
                              bool italic) {
    Cluster out;
    if (!cps || n == 0 || !impl_->hasCanvas) return out;
    const Impl::ClusterMetric m = impl_->clusterMetric(size, weight, cps, n, italic);
    out.len = m.len;
    out.advance = m.adv;
    out.color = m.color;
    return out;
}

void Atlas::drawText(float x, float baseline, float size, int weight, const std::string& utf8,
                     uint32_t color, bool italic) {
    if (!impl_->store.ready() || utf8.empty()) return;
    uint32_t premult = color;
    detail::PremultColor(premult);

    // 落位：x 量化到"整数 + ¼ 相位"，基线取整
    float qx = x;
    int phase = 0;
    if (impl_->snap) qx = std::floor(x + 0.5f);
    else if (Impl::PhaseEnabled()) Impl::QuantizeSubpixelX(x, qx, phase);
    const float base = std::floor(baseline + 0.5f);

    // ---- 整形缓存：命中时只剩"按四边形表发射"这一件事 ----
    const uint64_t key = impl_->runKey(utf8, size, weight, italic, phase);
    auto it = impl_->runs.find(key);
    if (it != impl_->runs.end() && it->second.gen == impl_->atlasGen) {
        it->second.lastUse = ++impl_->runClock;
        impl_->emitRun(it->second, qx, base, premult);
        return;
    }
    impl_->buildRun(utf8, size, weight, italic, phase, qx, base, key, premult);
}

void Atlas::clear() {
    // 先用**当前**出口把纹理销毁（设备句柄必须还活着才销毁得掉），再回到 Image 出口：
    // 与迁移前 setGlyphDeviceTextures(false) 的语义一致 —— 调用方拆执行体之后，
    // 后续懒加载的字形不该再往那个句柄空间里建纹理。
    impl_->clearAll();
    impl_->store.device = false;
}

} // namespace text
} // namespace bin2d
