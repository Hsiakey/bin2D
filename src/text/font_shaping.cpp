// font_shaping.cpp —— HarfBuzz 整形（连字 / 复杂脚本 / emoji 合体）+ 复杂脚本检测
//
// 为什么整形跟着字形一起搬进 bin2d：簇切分（shaping 的产出）和字形紧邻 ——
// "这个簇里是哪几个 glyph、总推进多少"必须与"这些 glyph 长什么样"用同一套
// 会话度量，分两处维护的代价是测宽与绘制会慢慢分叉。
#include "font_impl.h"

#include <algorithm>

namespace bin2d {
namespace text {

namespace {

// 复杂脚本范围（HarfBuzz 整形前检测用，粗略覆盖主流）
struct ScriptRange { uint32_t lo, hi; };
const ScriptRange kComplexScripts[] = {
    { 0x0590, 0x08FF },  // Hebrew / Arabic / Syriac / Thaana / N'Ko / Arabic Ext-A
    { 0x0900, 0x0DFF },  // Devanagari..Sinhala（印度语系）
    { 0x0E00, 0x0FFF },  // Thai / Lao / Tibetan
    { 0x1000, 0x109F },  // Myanmar
    { 0x1100, 0x11FF },  // Hangul Jamo（无 GSUB，但保守整形无碍）
    { 0x1700, 0x17FF },  // Tagalog..Khmer
    { 0x1900, 0x1CFF },  // Limbu..Sundanese/Vedic Ext
    { 0xA800, 0xA8FF },  // Kayah Li / Devanagari Extended
    { 0xA900, 0xA9FF },  // Javanese / Myanmar Ext-B
    { 0xAA00, 0xAA5F },  // Cham
    { 0xAA80, 0xAADF },  // Tai Viet
    { 0xAB00, 0xAB2F },  // Ethiopic Extended-A
    { 0xFB50, 0xFDFF },  // Arabic Presentation Forms-A
    { 0xFE70, 0xFEFF },  // Arabic Presentation Forms-B
};

bool InComplexScript(uint32_t cp) {
    for (const auto& r : kComplexScripts)
        if (cp >= r.lo && cp <= r.hi) return true;
    return false;
}

} // namespace

std::vector<ShapedGlyph> Library::shape(const std::string& face, float size, const uint32_t* cps,
                                        size_t n) {
    std::vector<ShapedGlyph> result;
    if (!cps || n == 0) return result;
    auto* s = Impl::getSession(impl_, face, size);
    if (!s || !s->hfont) return result;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf32(buf, cps, static_cast<int>(n), 0, static_cast<int>(n));
    hb_buffer_guess_segment_properties(buf);
    hb_shape(s->hfont, buf, nullptr, 0);

    unsigned int count = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
    result.reserve(count);
    for (unsigned int i = 0; i < count; i++) {
        ShapedGlyph g;
        g.gid = info[i].codepoint;
        g.cluster = info[i].cluster;
        g.xAdvance = Round26d6(pos ? pos[i].x_advance : 0);
        g.xOffset = Round26d6(pos ? pos[i].x_offset : 0);
        result.push_back(g);
    }
    hb_buffer_destroy(buf);
    return result;
}

bool Library::needsShaping(const uint32_t* cps, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (InComplexScript(cps[i])) return true;
    return false;
}

} // namespace text
} // namespace bin2d
