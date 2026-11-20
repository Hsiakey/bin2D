// text_unicode.cpp —— 簇切分（emoji 序列）与簇哈希
#include "text_unicode.h"

namespace bin2d {
namespace text {

namespace {

bool IsEmojiCp(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;
    if (cp >= 0x2190 && cp <= 0x21FF) return true;
    if (cp >= 0x2300 && cp <= 0x27BF) return true;
    if (cp >= 0x2900 && cp <= 0x297F) return true;
    if (cp >= 0x2B00 && cp <= 0x2BFF) return true;
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
    if (cp == 0x200D) return true;
    if (cp == 0x20E3) return true;
    return cp == 0x00A9 || cp == 0x00AE || cp == 0x203C || cp == 0x2049 ||
           cp == 0x2122 || cp == 0x2139 || cp == 0x3030 || cp == 0x303D ||
           cp == 0x3297 || cp == 0x3299;
}

} // namespace

// emoji 簇的长度：ZWJ 连写、肤色修饰、变体选择符、键帽、区域旗帜
size_t EmojiClusterLen(const uint32_t* cps, size_t n) {
    if (!cps || n == 0) return 1;
    const uint32_t first = cps[0];
    if ((first == '#' || first == '*' || (first >= '0' && first <= '9')) && n > 1) {
        if (cps[1] == 0xFE0F && n > 2 && cps[2] == 0x20E3) return 3;
        if (cps[1] == 0x20E3) return 2;
    }
    if (!IsEmojiCp(first)) return 1;
    size_t j = 1;
    int ris = (first >= 0x1F1E6 && first <= 0x1F1FF) ? 1 : 0;
    while (j < n) {
        const uint32_t c = cps[j];
        if (c == 0x200D) {
            if (j + 1 >= n) break;
            j += 2;
            continue;
        }
        if (c == 0xFE0F || c == 0xFE0E || c == 0x20E3) { j++; continue; }
        if (c >= 0x1F3FB && c <= 0x1F3FF) { j++; continue; }
        if (c >= 0x1F1E6 && c <= 0x1F1FF) {
            if (ris == 1) { j++; ris = 2; continue; }
            break;
        }
        break;
    }
    return j;
}

namespace detail {

uint64_t EmojiClusterKey(const uint32_t* cps, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) {
        uint64_t c = cps[i];
        for (int b = 0; b < 4; b++) {
            h ^= (c & 0xFF);
            h *= 1099511628211ull;
            c >>= 8;
        }
    }
    return h;
}

} // namespace detail

} // namespace text
} // namespace bin2d
