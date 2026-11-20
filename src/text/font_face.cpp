// font_face.cpp —— face 注册表、平台 Resolver 注入、回退链、会话与度量、豆腐块
//
// 这一份只做"face 从哪来、它有多大、缺字怎么兜"这三件事。
// 字形怎么画进位图在 font_raster.cpp，整形在 font_shaping.cpp。
#include "font_impl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bin2d {
namespace text {

int Round26d6(long v) { return static_cast<int>((v + 32) / 64); }

FT_Int32 LoadFlagsFor(float sizePx) {
    // 可用环境变量覆盖，方便不重编就对比三种观感：
    //   BIN2D_TEXT_HINT=0 全关 / 1 全 light / 2 全 full
    // 默认（未设置）= 小字号 light、大字号关
    static const int mode = [] {
        const char* e = std::getenv("BIN2D_TEXT_HINT");
        if (!e || !*e) return 3;
        return std::atoi(e);
    }();
    const bool small = (sizePx <= kLightHintMaxPx);
    if (mode == 0) return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
    if (mode == 1) return FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    if (mode == 2) return FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP;
    // NO_BITMAP：只用轮廓（彩色 bitmap 字形由 COLR/彩色路径单独处理）
    if (small) return FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP;
    return FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
}

const uint8_t* GammaLut() {
    static uint8_t lut[256] = {};
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; i++) {
            const float v = static_cast<float>(i) / 255.0f;
            const float g = std::pow(v, 1.0f / kGlyphGamma);
            int o = static_cast<int>(g * 255.0f + 0.5f);
            if (o < 0) o = 0;
            if (o > 255) o = 255;
            lut[i] = static_cast<uint8_t>(o);
        }
        lut[0] = 0;
        lut[255] = 255;
        built = true;
    }
    return lut;
}

Library::Impl::~Impl() {
    releaseSessions();
    for (auto& kv : probes)
        if (kv.second) FT_Done_Face(kv.second);
    probes.clear();
    if (ft) FT_Done_FreeType(ft);
}

void Library::Impl::releaseSessions() {
    for (auto& kv : sessions) {
        if (kv.second->hfont) hb_font_destroy(kv.second->hfont);
        if (kv.second->face) FT_Done_Face(kv.second->face);
        delete kv.second;
    }
    sessions.clear();
    // 会话一没，palette 指针就悬空了
    palette = nullptr;
    palFace = nullptr;
}

std::string* Library::Impl::findPath(const std::string& name) {
    auto it = faces.find(name);
    if (it != faces.end()) return &it->second;
    if (!resolver) return nullptr;
    // 平台发现：问一次、记住（含"本机没有"这个答案，不在热路径上反复问）
    FaceDesc d;
    d.family = name;
    std::string path = resolver(d);
    if (path.empty()) return nullptr;
    return &(faces[name] = path);
}

// ============================================================
// 逐字形回退
// ============================================================
// 为什么不能"按码点区间猜"：Unicode 只定"U+XXXX 是哪个字符"，谁有它的字形
// 是**字体自报的 cmap**（每个字体文件都不一样）。所以这里就一件事：问字体。
//
// 查询成本：每个码点扫一遍候选链，命中即停；结果按码点缓存。候选 face 的
// FT_Face 也按需加载（只有前面的面都缺才轮到后面的）。

FT_Face Library::Impl::probeFace(Impl* p, const std::string& name) {
    auto it = p->probes.find(name);
    if (it != p->probes.end()) return it->second;
    FT_Face f = nullptr;
    std::string* path = p->findPath(name);
    if (p->ft && path && !path->empty()) {
        if (FT_New_Face(p->ft, path->c_str(), 0, &f) != 0) f = nullptr;
    }
    p->probes[name] = f;   // 失败也缓存（nullptr），不在热路径上反复尝试
    return f;
}

// 取 (face, sizePx) 会话，必要时创建（FT_Face + hb_font + 度量）
Library::Impl::Session* Library::Impl::getSession(Impl* p, const std::string& face, float size) {
    int sizePx = static_cast<int>(std::lround(size));
    std::string key = face + "|" + std::to_string(sizePx);
    auto it = p->sessions.find(key);
    if (it != p->sessions.end()) return it->second;

    std::string* path = p->findPath(face);
    if (!path || path->empty()) {
        path = p->findPath(p->defaultFace);
        if (!path || path->empty()) return nullptr;
    }

    FT_Face f = nullptr;
    if (FT_New_Face(p->ft, path->c_str(), 0, &f) != 0) return nullptr;
    FT_Set_Pixel_Sizes(f, 0, sizePx > 0 ? static_cast<unsigned>(sizePx) : 1);

    auto* s = new Session();
    s->face = f;
    s->hfont = hb_ft_font_create_referenced(f);
    // HarfBuzz 的整形 advance 必须与光栅化用同一套 hinting 开关，否则定位会偏
    hb_ft_font_set_load_flags(s->hfont, LoadFlagsFor(static_cast<float>(sizePx)));
    s->m.ascent = Round26d6(f->size->metrics.ascender);
    s->m.descent = -Round26d6(f->size->metrics.descender);
    p->sessions[key] = s;
    return s;
}

// ============================================================
// Library 公开接口
// ============================================================
Library::Library() : impl_(new Impl()) {}
Library::~Library() { delete impl_; }

bool Library::init() {
    if (impl_->ft) return true;
    if (FT_Init_FreeType(&impl_->ft) != 0) return false;
    return true;
}

void Library::shutdown() {
    delete impl_;
    impl_ = new Impl();
}

bool Library::ready() const { return impl_->ft != nullptr; }

void Library::setResolver(FaceResolver resolver) { impl_->resolver = std::move(resolver); }

void Library::setDefaultFace(const std::string& name) {
    if (!name.empty()) impl_->defaultFace = name;
}

const std::string& Library::defaultFace() const { return impl_->defaultFace; }

bool Library::addFont(const std::string& name, const std::string& path) {
    if (name.empty() || path.empty()) return false;
    impl_->faces[name] = path;
    return true;
}

void Library::setFallbackChain(const std::vector<std::string>& faces) {
    impl_->fallback.clear();
    for (const auto& n : faces) {
        if (n.empty()) continue;
        std::string* path = impl_->findPath(n);
        if (!path || path->empty()) continue;   // 本机没有这个家族：跳过
        bool dup = false;
        for (const auto& e : impl_->fallback)
            if (e == n) { dup = true; break; }
        if (!dup) impl_->fallback.push_back(n);
    }
    impl_->coverCache.clear();   // 链变了，覆盖缓存作废
}

const std::vector<std::string>& Library::fallbackChain() const { return impl_->fallback; }

const std::string& Library::faceName(int index) const {
    if (index >= 0 && static_cast<size_t>(index) < impl_->fallback.size())
        return impl_->fallback[static_cast<size_t>(index)];
    return impl_->defaultFace;
}

int Library::faceIndexFor(uint32_t cp) {
    auto it = impl_->coverCache.find(cp);
    if (it != impl_->coverCache.end()) return it->second;

    int found = -1;
    if (!impl_->fallback.empty()) {
        for (size_t i = 0; i < impl_->fallback.size(); i++) {
            FT_Face f = Impl::probeFace(impl_, impl_->fallback[i]);
            if (!f) continue;
            if (FT_Get_Char_Index(f, cp) != 0) { found = static_cast<int>(i); break; }
        }
    } else {
        // 没设链：至少查一下默认 face，别把"能画"也说成缺字
        FT_Face f = Impl::probeFace(impl_, impl_->defaultFace);
        if (f && FT_Get_Char_Index(f, cp) != 0) found = 0;
    }
    impl_->coverCache[cp] = found;
    return found;
}

Metrics Library::metrics(const std::string& face, float size) {
    auto* s = Impl::getSession(impl_, face, size);
    if (!s) {
        Metrics m;
        m.ascent = static_cast<int>(std::ceil(size * 0.8f));
        m.descent = static_cast<int>(std::ceil(size * 0.2f));
        return m;
    }
    return s->m;
}

// 豆腐块：空心方框。几何写在这一处，两个出口（CPU / GPU）同一份位图，
// 也方便测试断言「四边有墨、中间空心」。
// 「留白」和「方框」的区别不是观感问题：留白看起来像引擎 bug，方框是
// "当前字体里没有这个字符"的明确信号 —— 用户能判断是缺字体还是缺数据。
bool Library::renderMissingGlyph(float size, GrayGlyph& out) {
    return renderMissingGlyph(size, false, false, out);
}

bool Library::renderMissingGlyph(float size, bool bold, bool italic, GrayGlyph& out) {
    (void)bold;    // 方框用几何画，加粗/倾斜没有意义
    (void)italic;
    const int w = std::max(4, static_cast<int>(std::lround(size * 0.58f)));
    const int h = std::max(5, static_cast<int>(std::lround(size * 0.72f)));
    const int th = size >= 26.0f ? 2 : 1;   // 大字号用 2px 边，否则细得看不清
    impl_->tofuBuf.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const bool edge = (x < th || x >= w - th || y < th || y >= h - th);
            impl_->tofuBuf[static_cast<size_t>(y) * w + x] = edge ? 255 : 0;
        }
    }
    out.width = w;
    out.height = h;
    out.pitch = w;
    out.left = 0;
    out.top = h;                              // 整块在基线之上，和普通字形同一套坐标
    out.advance = w + std::max(1, th);
    out.alpha = impl_->tofuBuf.data();
    return true;
}

} // namespace text
} // namespace bin2d
