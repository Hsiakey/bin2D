#pragma once
// ============================================================
// src/text/text_unicode.h —— 簇切分 / 预乘 / 图集键这些"纯数学"小工具
//
// 为什么单独一份：整形（face.cpp）、图集（glyph_atlas.cpp）与度量都要用同一份
// 簇切分规则。分开写两份的后果很具体 —— "测宽"和"绘制"按不同粒度切簇，
// 换行位置就会与绘制位置分叉（历史上前端与后端各写一份时就是这么翻车的）。
// ============================================================

#include <cstdint>

#include "bin2d/text.h"

namespace bin2d {
namespace text {
namespace detail {

// 设计上就不可见的码点：不画豆腐块（否则一串 emoji 里会凭空多出方框）
inline bool IsInvisibleCp(uint32_t cp) {
    if (cp < 0x20) return true;                                    // C0 控制符
    if (cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;                  // 变体选择符 VS1–16
    if (cp >= 0xE0100 && cp <= 0xE01EF) return true;                // 变体选择符 VS17–256
    if (cp == 0x20E3) return true;                                  // 键帽组合符
    return false;
}

// 颜色预乘（图集/模板本身已是预乘，顶点色输出预乘 alpha）
inline void PremultColor(uint32_t& c) {
    const uint32_t a = (c >> 24) & 0xFF;
    if (a == 255) return;
    if (a == 0) { c = 0; return; }
    const uint32_t r = (((c >> 16) & 0xFF) * a) / 255;
    const uint32_t g = (((c >> 8) & 0xFF) * a) / 255;
    const uint32_t b = ((c & 0xFF) * a) / 255;
    c = (a << 24) | (r << 16) | (g << 8) | b;
}

// 一个簇的哈希（彩色图集的缓存键）：码点序列 → 稳定的 64 位值
uint64_t EmojiClusterKey(const uint32_t* cps, size_t n);

} // namespace detail
} // namespace text
} // namespace bin2d
