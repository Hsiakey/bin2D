#pragma once
// ============================================================
// bin2d/png.h —— 极小的 PNG 编码器（纯头文件，无第三方依赖）
//
// 为什么自己写而不引 zlib/libpng：
//   · bin2d 的定位是"任何宿主都能直接吃"的画布库，多一个二进制依赖就多
//     一份"装不上"的可能；而"把一帧位图存成 PNG"这件事只需要 CRC32、
//     Adler32 和**不压缩**的 deflate 块（BTYPE=00），一百多行就够。
//   · 像素回归 / 文档截图 / 用户导出都走它，确定性比压缩率重要。
//
// 格式细节（写死，别再选）：
//   · 8 位/通道，color type 6（RGBA，非预乘 —— PNG 的规定），filter 全 0；
//   · IDAT 是 zlib 流：78 01 + 若干 stored 块 + Adler32；
//   · 每块 ≤ 65535 字节，最后一块 BFINAL=1。
// 这样输出的文件任何看图软件都认，且**同一位图逐字节可复现**（CRC 稳定）。
//
// 输入 Surface 是预乘的，写文件前反预乘；alpha = 0 的像素写成全 0。
// ============================================================

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "bin2d/core.h"
#include "bin2d/surface.h"

namespace bin2d {
namespace png_detail {

inline uint32_t Crc32Table(uint32_t i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}

inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) table[i] = Crc32Table(i);
        ready = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

inline uint32_t Adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    const size_t kMod = 65521;
    while (n > 0) {
        const size_t chunk = n > 5552 ? 5552 : n;
        for (size_t i = 0; i < chunk; ++i) {
            a += p[i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        p += chunk;
        n -= chunk;
    }
    return (b << 16) | a;
}

inline void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
}

inline void Chunk(std::vector<uint8_t>& out, const char tag[5],
                  const std::vector<uint8_t>& payload) {
    PutBE32(out, static_cast<uint32_t>(payload.size()));
    const size_t tagAt = out.size();
    out.push_back(static_cast<uint8_t>(tag[0]));
    out.push_back(static_cast<uint8_t>(tag[1]));
    out.push_back(static_cast<uint8_t>(tag[2]));
    out.push_back(static_cast<uint8_t>(tag[3]));
    out.insert(out.end(), payload.begin(), payload.end());
    uint32_t crc = Crc32(out.data() + tagAt, 4 + payload.size());
    PutBE32(out, crc ^ 0xFFFFFFFFu);
}

// zlib 流（stored 块）
inline void ZlibStored(std::vector<uint8_t>& out, const std::vector<uint8_t>& raw) {
    out.push_back(0x78);   // CMF：deflate, 32K window
    out.push_back(0x01);   // FLG：无字典、最快压缩
    size_t off = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool final_block = (off + n) >= raw.size();
        out.push_back(final_block ? 0x01 : 0x00);   // BFINAL + BTYPE=00
        out.push_back(static_cast<uint8_t>(n & 0xFFu));
        out.push_back(static_cast<uint8_t>((n >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((~n) & 0xFFu));
        out.push_back(static_cast<uint8_t>(((~n) >> 8) & 0xFFu));
        out.insert(out.end(), raw.begin() + static_cast<long>(off),
                   raw.begin() + static_cast<long>(off + n));
        off += n;
    } while (off < raw.size());
    PutBE32(out, Adler32(raw.data(), raw.size()));
}

} // namespace png_detail

// 把预乘 Surface 写成 PNG；path 是 UTF-8/ANSI 路径（Windows 下用 fopen，够用）。
inline bool writePng(const Surface& surface, const std::string& path) {
    if (surface.empty()) return false;

    // 扫描线：每行 filter=0 + RGBA（反预乘）
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(surface.width() + 1) * surface.height());
    for (int y = 0; y < surface.height(); ++y) {
        raw.push_back(0);
        const uint8_t* p = surface.row(y);
        for (int x = 0; x < surface.width(); ++x, p += 4) {
            const uint32_t v = Color(p[0], p[1], p[2], p[3]).toArgbStraight();
            raw.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
            raw.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
            raw.push_back(static_cast<uint8_t>(v & 0xFFu));
            raw.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
        }
    }

    std::vector<uint8_t> file;
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    file.insert(file.end(), kSignature, kSignature + 8);

    std::vector<uint8_t> ihdr;
    png_detail::PutBE32(ihdr, static_cast<uint32_t>(surface.width()));
    png_detail::PutBE32(ihdr, static_cast<uint32_t>(surface.height()));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // color type: RGBA
    ihdr.push_back(0);   // compression
    ihdr.push_back(0);   // filter
    ihdr.push_back(0);   // interlace
    png_detail::Chunk(file, "IHDR", ihdr);

    std::vector<uint8_t> zlib;
    png_detail::ZlibStored(zlib, raw);
    png_detail::Chunk(file, "IDAT", zlib);
    png_detail::Chunk(file, "IEND", std::vector<uint8_t>());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    return written == file.size();
}

// 调试用 PPM：预乘直接当"压在黑底上"的 RGB 输出，无需反预乘，肉眼就行
inline bool writePpm(const Surface& surface, const std::string& path) {
    if (surface.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", surface.width(), surface.height());
    for (int y = 0; y < surface.height(); ++y) {
        const uint8_t* p = surface.row(y);
        for (int x = 0; x < surface.width(); ++x, p += 4) std::fwrite(p, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

} // namespace bin2d
