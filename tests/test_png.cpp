// test_png —— IHDR/CRC 自检、同输入逐字节可复现、反预乘正确
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bin2d/png.h"
#include "bin2d/surface.h"
#include "test_util.h"

using namespace bin2d;

static std::vector<uint8_t> ReadFile(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    if (n > 0 && std::fread(out.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n))
        out.clear();
    std::fclose(f);
    return out;
}

static uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// 自己算一遍 CRC（PNG 用的是 CRC-32/ISO-HDLC，初值全 1、末尾取反）
static uint32_t RefCrc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void TestPngStructure() {
    Surface s(17, 9);
    s.clear(Color::argb(0xFF3366CC));
    const char* path = "test_png_tmp.png";
    CHECK(writePng(s, path), "png: 写出成功");

    const std::vector<uint8_t> bytes = ReadFile(path);
    CHECK(bytes.size() > 8, "png: 文件非空");
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::memcmp(bytes.data(), sig, 8) == 0, "png: 8 字节签名正确");

    // 逐个 chunk 走一遍：长度 + 类型 + 数据 + CRC 必须自洽
    size_t off = 8;
    int chunks = 0;
    bool crcOk = true;
    std::vector<std::string> order;
    uint32_t width = 0, height = 0;
    int bitDepth = 0, colorType = -1;
    while (off + 12 <= bytes.size()) {
        const uint32_t len = ReadBE32(bytes.data() + off);
        const char* tag = reinterpret_cast<const char*>(bytes.data() + off + 4);
        const size_t dataAt = off + 8;
        if (dataAt + len + 4 > bytes.size()) { crcOk = false; break; }
        const uint32_t expect = RefCrc32(bytes.data() + off + 4, 4 + len);
        const uint32_t got = ReadBE32(bytes.data() + dataAt + len);
        if (expect != got) crcOk = false;
        order.push_back(std::string(tag, 4));
        if (std::strncmp(tag, "IHDR", 4) == 0) {
            width = ReadBE32(bytes.data() + dataAt);
            height = ReadBE32(bytes.data() + dataAt + 4);
            bitDepth = bytes[dataAt + 8];
            colorType = bytes[dataAt + 9];
        }
        ++chunks;
        off = dataAt + len + 4;
    }
    CHECK(chunks == 3, "png: 只有 IHDR/IDAT/IEND 三个 chunk");
    CHECK(crcOk, "png: 每个 chunk 的 CRC 与自算值一致");
    CHECK(order.size() == 3 && order[0] == "IHDR" && order[1] == "IDAT" && order[2] == "IEND",
          "png: chunk 顺序 IHDR → IDAT → IEND");
    CHECK(width == 17 && height == 9, "png: IHDR 宽高正确");
    CHECK(bitDepth == 8 && colorType == 6, "png: 8 位 / RGBA(color type 6)");

    // 同输入逐字节可复现
    const char* path2 = "test_png_tmp2.png";
    CHECK(writePng(s, path2), "png: 第二次写出成功");
    const std::vector<uint8_t> bytes2 = ReadFile(path2);
    CHECK(bytes.size() == bytes2.size() && std::memcmp(bytes.data(), bytes2.data(), bytes.size()) == 0,
          "png: 同输入两次写出逐字节相同");
    std::remove(path);
    std::remove(path2);
}

static void TestUnpremultiply() {
    // 50% 白压在黑底上 → 0x808080 灰；反预乘要还原成 (128,128,128,255)
    Surface s(1, 1);
    const Color dst = Color::black();
    Color c = dst;
    blendOver(c, Color::argb(0x80FFFFFF));   // 50% 白
    s.set(0, 0, c);
    CHECK(c.a == 255, "png: 不透明底上合成后 alpha = 255");
    CHECK_NEAR(static_cast<int>(c.r), 128, 1, "png: 50% 白压黑 → 128");
    CHECK(c.toArgbStraight() == 0xFF808080u, "png: 反预乘得到 0xFF808080");

    // 半透明预乘：a=128, r=64 → 直通 r = 64*255/128 = 127.5 → 128
    const Color pc(64, 0, 0, 128);
    const uint32_t argb = pc.toArgbStraight();
    CHECK(((argb >> 24) & 0xFFu) == 128u, "png: 反预乘保留 alpha");
    CHECK_NEAR(static_cast<int>((argb >> 16) & 0xFFu), 128, 1, "png: 反预乘还原直通红分量");

    // alpha = 0 的像素写成全 0（不反预乘，避免除 0）
    CHECK(Color(0, 0, 0, 0).toArgbStraight() == 0u, "png: 全透明像素写成 0");
}

int main() {
    TestPngStructure();
    TestUnpremultiply();
    return Bin2dTestSummary("test_png");
}
