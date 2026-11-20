// font_raster.cpp —— 单字形光栅化（hinting / gamma / ¼ 像素相位）+ COLR/CPAL 彩色
//
// 这里的每一个"观感"常量都刻意与迁移前逐位一致：
//   · 小字号 light hinting、大字号 NO_HINTING（LoadFlagsFor，见 font_impl.h）
//   · 覆盖率 gamma 1.3（GammaLut）
//   · 横向亚像素只做 ¼ 像素四相位，纵向仍按整像素吸附
// 换画质要另开一版、并重出像素基线；这一步只搬位置。
#include "font_impl.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bin2d {
namespace text {

bool GlyphHasColorLayers(FT_Face f, FT_UInt gid) {
    FT_LayerIterator it = {};
    FT_UInt lg = 0, lpal = 0;
    return FT_Get_Color_Glyph_Layer(f, gid, &lg, &lpal, &it) != 0;
}

// 彩色判定：COLR 层或内嵌彩色位图（CBDT/SBBIX，如区域指示符国旗）
bool GlyphIsColor(FT_Face f, FT_UInt gid) {
    if (GlyphHasColorLayers(f, gid)) return true;
    if (FT_Load_Glyph(f, gid, FT_LOAD_COLOR) != 0) return false;
    return f->glyph->format == FT_GLYPH_FORMAT_BITMAP &&
           f->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
}

bool Library::renderGlyph(const std::string& face, float size, uint32_t cp, GrayGlyph& out) {
    return renderGlyph(face, size, cp, false, false, 0, out);
}

// bold=true：先按轮廓加载（FT_LOAD_NO_BITMAP），再用 FT_GlyphSlot_Embolden 合成加粗。
// 该 API 会同时加粗轮廓、加宽 advance、调整 bearing —— 测宽/换行/绘制用同一份度量。
// italic=true：FT_GlyphSlot_Oblique 只做轮廓切变，advance/bearing 不变 ——
// 所以斜体不改变排版宽度。
//
// subpixelX（¼ 像素相位）：delta 用 26.6 定点（1px = 64），所以 ¼ 像素 = 16。
// FT_Set_Transform 在 FT_Load_Glyph 里生效，因此必须在 Load_Glyph **之前**设置，
// 渲染完再复位 —— 这个 face 是共享会话，变换残留会串到下一个字形。
bool Library::renderGlyph(const std::string& face, float size, uint32_t cp, bool bold,
                          bool italic, int subpixelX, GrayGlyph& out) {
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->face) return false;
    FT_Face f = s->face;

    const int phase = (subpixelX & 3);
    if (phase != 0) {
        FT_Vector delta;
        delta.x = static_cast<FT_Pos>(phase * 16);   // 26.6：16 = 0.25px
        delta.y = 0;
        FT_Set_Transform(f, nullptr, &delta);
    }
    // 复位 + 返回：把"清理变换"收在一个小 lambda 里，避免每个 return 都写一遍
    auto clearXform = [&]() { if (phase != 0) FT_Set_Transform(f, nullptr, nullptr); };

    FT_UInt gid = FT_Get_Char_Index(f, cp);
    if (gid == 0) { clearXform(); return false; }
    FT_Int32 flags = LoadFlagsFor(size);
    if (bold || italic) flags |= FT_LOAD_NO_BITMAP;  // 两个合成变换都只作用于矢量轮廓
    if (FT_Load_Glyph(f, gid, flags) != 0) { clearXform(); return false; }
    if (f->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (bold)   FT_GlyphSlot_Embolden(f->glyph);
        if (italic) FT_GlyphSlot_Oblique(f->glyph);
    }
    if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) { clearXform(); return false; }
    clearXform();   // 位图已经烘好，变换不用再挂在这个共享 face 上

    const FT_Bitmap& bm = f->glyph->bitmap;
    out.width = static_cast<int>(bm.width);
    out.height = static_cast<int>(bm.rows);
    out.left = f->glyph->bitmap_left;
    out.top = f->glyph->bitmap_top;
    out.advance = Round26d6(f->glyph->advance.x);

    // gamma 修正：把线性覆盖率映射成观感接近 DirectWrite 的 alpha。
    // 需要可写副本（FreeType 的 bitmap buffer 是只读语义且下一次调用即失效，
    // 与原契约一致：out.alpha 在下次 renderGlyph 前有效）。
    if (!bm.buffer || bm.rows == 0 || bm.width == 0) {
        out.pitch = 0;
        out.alpha = nullptr;
        return true;
    }
    const uint8_t* lut = GammaLut();
    const size_t rowBytes = static_cast<size_t>(bm.width);
    s->scratch.assign(rowBytes * bm.rows, 0);
    for (unsigned r = 0; r < bm.rows; r++) {
        const uint8_t* src = bm.buffer + static_cast<ptrdiff_t>(r) * bm.pitch;
        uint8_t* dst = s->scratch.data() + r * rowBytes;
        for (size_t c = 0; c < rowBytes; c++) dst[c] = lut[src[c]];
    }
    out.pitch = static_cast<int>(rowBytes);
    out.alpha = s->scratch.data();
    return true;
}

bool Library::hasColor(const std::string& face, float size, const uint32_t* cps, size_t n) {
    if (!cps || n == 0) return false;
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->face) return false;
    std::vector<ShapedGlyph> gs = shape(face, size, cps, n);
    if (gs.empty()) return false;
    return GlyphIsColor(s->face, gs[0].gid);
}

// 彩色渲染整个簇（内部经 HarfBuzz 做 GSUB 合体/旗帜/肤色等）；
// false = 该簇无 COLR 层（调用方降级灰度）
bool Library::renderColorCluster(const std::string& face, float size, const uint32_t* cps,
                                 size_t n, ColorGlyph& out) {
    if (!cps || n == 0) return false;
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->face) return false;
    FT_Face f = s->face;

    std::vector<ShapedGlyph> gs = shape(face, size, cps, n);
    if (gs.empty()) return false;
    if (!GlyphIsColor(f, gs[0].gid)) return false;

    // 每层：位置 + 像素拷贝 + 颜色（bgra=true 时为直通 BGRA 内嵌位图）
    // 注意：必须立即拷贝像素——后续 FT_Load_Glyph 会覆盖 face->glyph->bitmap
    struct Layer { int x, y, w, h, pitch; std::vector<uint8_t> px; uint8_t r, g, b, a; bool bgra; };
    std::vector<Layer> layers;
    int xAccum = 0;
    int advTotal = 0;
    for (const auto& g : gs) {
        advTotal += g.xAdvance;
        if (FT_Load_Glyph(f, g.gid, FT_LOAD_COLOR) != 0) return false;

        // 直接彩色位图（CBDT/SBBIX 内嵌位图）
        if (f->glyph->format == FT_GLYPH_FORMAT_BITMAP) {
            const FT_Bitmap& bm = f->glyph->bitmap;
            if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
                Layer L;
                L.x = f->glyph->bitmap_left + xAccum + g.xOffset;
                L.y = f->glyph->bitmap_top;
                L.w = static_cast<int>(bm.width);
                L.h = static_cast<int>(bm.rows);
                L.pitch = bm.pitch;
                L.px.assign(bm.buffer, bm.buffer + static_cast<size_t>(bm.rows) * bm.pitch);
                L.bgra = true;
                if (L.w > 0 && L.h > 0) layers.push_back(L);
                xAccum += g.xAdvance;
                continue;
            }
            // 其他位图模式：不处理，走层路径
        }

        // COLR 层枚举
        if (!GlyphHasColorLayers(f, g.gid)) {
            // 无 COLR 层（如旗帜第二半等）：按单色轮廓走
            if (FT_Load_Glyph(f, g.gid, LoadFlagsFor(size)) != 0) return false;
            if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;
            const FT_Bitmap& bm = f->glyph->bitmap;
            Layer L;
            L.x = f->glyph->bitmap_left + xAccum + g.xOffset;
            L.y = f->glyph->bitmap_top;
            L.w = static_cast<int>(bm.width);
            L.h = static_cast<int>(bm.rows);
            L.pitch = bm.pitch;
            L.px.assign(bm.buffer, bm.buffer + static_cast<size_t>(bm.rows) * bm.pitch);
            L.bgra = false;
            L.r = L.g = L.b = L.a = 255;
            if (L.w > 0 && L.h > 0) layers.push_back(L);
            xAccum += g.xAdvance;
            continue;
        }

        if (impl_->palFace != f) {
            impl_->palette = nullptr;
            if (FT_Palette_Select(f, 0, &impl_->palette) != 0) impl_->palette = nullptr;
            impl_->palFace = f;
        }

        FT_LayerIterator it = {};
        FT_UInt lg = 0, lpal = 0;
        int nLayers = 0;
        while (FT_Get_Color_Glyph_Layer(f, g.gid, &lg, &lpal, &it)) nLayers++;
        it.num_layers = 0; it.layer = 0; it.p = nullptr;
        for (int i = 0; i < nLayers && FT_Get_Color_Glyph_Layer(f, g.gid, &lg, &lpal, &it); i++) {
            if (FT_Load_Glyph(f, lg, LoadFlagsFor(size)) != 0) continue;
            if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
            const FT_Bitmap& bm = f->glyph->bitmap;
            Layer L;
            L.x = f->glyph->bitmap_left + xAccum + g.xOffset;
            L.y = f->glyph->bitmap_top;
            L.w = static_cast<int>(bm.width);
            L.h = static_cast<int>(bm.rows);
            L.pitch = bm.pitch;
            L.px.assign(bm.buffer, bm.buffer + static_cast<size_t>(bm.rows) * bm.pitch);
            L.bgra = false;
            if (impl_->palette) {
                const FT_Color& c = impl_->palette[lpal];
                L.r = c.red; L.g = c.green; L.b = c.blue; L.a = c.alpha;
            } else { L.r = L.g = L.b = L.a = 255; }
            if (L.w > 0 && L.h > 0) layers.push_back(L);
        }
        xAccum += g.xAdvance;
    }
    if (layers.empty()) return false;

    // 合成 bounding box（y 向上为基线上方）
    int leftMin = 0x7FFFFFFF, rightMax = -0x7FFFFFFF;
    int topMax = -0x7FFFFFFF, bottomMin = 0x7FFFFFFF;
    for (const auto& L : layers) {
        leftMin = std::min(leftMin, L.x);
        rightMax = std::max(rightMax, L.x + L.w);
        topMax = std::max(topMax, L.y);
        bottomMin = std::min(bottomMin, L.y - L.h);
    }
    if (rightMax <= leftMin || topMax <= bottomMin) return false;
    int W = rightMax - leftMin;
    int H = topMax - bottomMin;

    impl_->colorBuf.assign(static_cast<size_t>(W) * H * 4, 0);
    uint8_t* dst = impl_->colorBuf.data();
    // COLR 层列表顺序 = 绘制顺序（顶→底）：合成时反转，底先画
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const Layer& L = *it;
        int ox = L.x - leftMin;
        int oy = (topMax - L.y);   // 缓冲 y（顶=0）
        if (L.a == 0) continue;
        for (int y = 0; y < L.h; y++) {
            const uint8_t* src = L.px.data() + static_cast<size_t>(y) * L.pitch;
            uint8_t* row = dst + static_cast<size_t>(oy + y) * W * 4 + static_cast<size_t>(ox) * 4;
            if (L.bgra) {
                // 内嵌 BGRA 位图：直通 alpha → 预乘 over
                for (int x = 0; x < L.w; x++) {
                    const uint8_t* sp = src + static_cast<size_t>(x) * 4;
                    uint8_t sa = sp[3];
                    if (sa == 0) continue;
                    uint8_t* p = row + static_cast<size_t>(x) * 4;
                    uint32_t sb = static_cast<uint32_t>(sp[0]) * sa / 255;
                    uint32_t sg = static_cast<uint32_t>(sp[1]) * sa / 255;
                    uint32_t sr = static_cast<uint32_t>(sp[2]) * sa / 255;
                    uint32_t inv = 255 - p[3];
                    uint8_t na = static_cast<uint8_t>(sa + static_cast<uint32_t>(p[3]) * inv / 255);
                    p[0] = static_cast<uint8_t>(p[0] + sb * inv / 255);
                    p[1] = static_cast<uint8_t>(p[1] + sg * inv / 255);
                    p[2] = static_cast<uint8_t>(p[2] + sr * inv / 255);
                    p[3] = na;
                }
            } else {
                // 单色 alpha 层 + 层色：预乘 over
                for (int x = 0; x < L.w; x++) {
                    uint8_t sa = static_cast<uint8_t>((src[x] * L.a) / 255);
                    if (sa == 0) continue;
                    uint8_t* p = row + static_cast<size_t>(x) * 4;
                    uint32_t sb = static_cast<uint32_t>(L.b) * sa / 255;
                    uint32_t sg = static_cast<uint32_t>(L.g) * sa / 255;
                    uint32_t sr = static_cast<uint32_t>(L.r) * sa / 255;
                    uint32_t inv = 255 - p[3];
                    uint8_t na = static_cast<uint8_t>(sa + static_cast<uint32_t>(p[3]) * inv / 255);
                    p[0] = static_cast<uint8_t>(p[0] + sb * inv / 255);
                    p[1] = static_cast<uint8_t>(p[1] + sg * inv / 255);
                    p[2] = static_cast<uint8_t>(p[2] + sr * inv / 255);
                    p[3] = na;
                }
            }
        }
    }

    out.width = W;
    out.height = H;
    out.baseline = topMax;
    out.advance = advTotal;
    out.bgra = dst;
    return true;
}

} // namespace text
} // namespace bin2d
