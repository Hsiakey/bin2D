#pragma once
// ============================================================
// src/text/font_impl.h —— Library 的内部状态（FreeType / HarfBuzz 会话表）
//
// 只有 libbin2d_text 自己的翻译单元能看见这份头。对外公开的只是 bin2d/text.h。
// 分成三份实现（face / raster / shaping）是为了让每份都能单独读懂：
//   · font_face.cpp    —— 注册表、Resolver、回退链、会话与度量、豆腐块
//   · font_raster.cpp  —— 单字形光栅化（hinting / gamma / ¼ 相位）+ COLR/CPAL 彩色
//   · font_shaping.cpp —— HarfBuzz 整形与复杂脚本检测
// ============================================================

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_COLOR_H
#include <freetype/ftsynth.h>   // FT_GlyphSlot_Embolden（合成加粗）
#include FT_TRUETYPE_TABLES_H

#include <hb.h>
#include <hb-ft.h>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "bin2d/text.h"

namespace bin2d {
namespace text {

// ============================================================
// 字形光栅化的两个"观感"参数（只影响灰阶覆盖率，不影响度量）
// ============================================================
// 小字号启用"轻 hinting"：只调竖笔位置/宽度，救 12~16px 的清晰度；
// 更大字号仍走 NO_HINTING —— 保留"大字号跨平台一致"这条承诺。
constexpr float kLightHintMaxPx = 16.5f;

// 灰度覆盖率 gamma：FreeType 给的线性覆盖率直接当 alpha 会让文字偏细
// （亮底黑字偏粗、暗底白字偏细）。γ=1.3 接近 DirectWrite 的默认观感。
constexpr float kGlyphGamma = 1.3f;

int Round26d6(long v);

FT_Int32 LoadFlagsFor(float sizePx);
// 256 项 gamma 查表（首末两端保持 0/255，单调不溢出）
const uint8_t* GammaLut();

bool GlyphHasColorLayers(FT_Face f, FT_UInt gid);
bool GlyphIsColor(FT_Face f, FT_UInt gid);

struct Library::Impl {
    FT_Library ft = nullptr;

    struct Session {
        FT_Face face = nullptr;
        hb_font_t* hfont = nullptr;
        Metrics m;
        std::vector<uint8_t> scratch;   // 灰度字形 + gamma 修正后的输出
    };

    // 名字 → 文件路径（addFont 登记 / Resolver 解析出来的都进这里）
    std::map<std::string, std::string> faces;
    std::string defaultFace;
    FaceResolver resolver;

    std::unordered_map<std::string, Session*> sessions;   // key: face|sizePx
    FT_Color* palette = nullptr;      // 当前 face 的 palette 0（随最近一次加载）
    FT_Face palFace = nullptr;

    std::vector<uint8_t> colorBuf;    // 彩色合成输出缓冲
    std::vector<uint8_t> tofuBuf;     // 豆腐块位图缓冲

    // ---- 逐字形回退 ----
    std::vector<std::string> fallback;                  // 有序候选 face 名
    std::unordered_map<std::string, FT_Face> probes;    // 面名 → 只用于 cmap 查询的 FT_Face
    std::unordered_map<uint32_t, int> coverCache;       // 码点 → 链上下标（-1 = 都没有）

    ~Impl();

    // 名字 → 路径：先查登记表，再问 Resolver（问到的结果会记下来）
    std::string* findPath(const std::string& name);
    static FT_Face probeFace(Impl* p, const std::string& name);
    static Session* getSession(Impl* p, const std::string& face, float size);
    void releaseSessions();
};

} // namespace text
} // namespace bin2d
