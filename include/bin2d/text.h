#pragma once
// ============================================================
// bin2d/text.h —— 文字模块（可选静态库 libbin2d_text）的对外接口
//
// 为什么单开一个模块：bin2d 核心库的定位是"纯 CPU、零第三方依赖"，而字形光栅
// 与整形必然要 FreeType + HarfBuzz。所以文字单独成一个可选静态库：
//   · 只画图形的人不装字体库，`make all` 照常过（核心库不链它）；
//   · 要画字的调用方链 libbin2d_text.a（自带/外带那两个静态库）。
//
// 边界（这条是判据，不是建议）——本模块**只认**：
//   · 一个 face（family + 字号 + 字重 + 斜体）与一段 UTF-8 / 码点序列；
//   · 一个"把位图变成纹理"的出口（bin2d::Canvas 的外部纹理通道）。
// 本模块**不认识**：CSS 属性、文本节点、窗口、DPI、换行策略、行高、对齐、
// 省略号、bidi 方向。那些都是上层的排版语义，用这里给出的原语自己算。
//
// 一句话：
//   "字符串 + face + 字号 → 字形序列（glyph id + advance）+ 字形位图 + 图集纹理"
//   这一半归这里；"这段字落在哪个盒子里、行高多少、怎么折、对齐到哪"归上层。
//
// 平台字体发现（EnumFontFamiliesEx / fontconfig / CoreText）不在本模块：
// 只定义 FaceResolver 这个注入契约，具体枚举由外壳实现 —— bin2d 不碰平台 API。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "bin2d/core.h"

namespace bin2d {

class Canvas;

namespace text {

// ------------------------------------------------------------
// 光栅化产物
// ------------------------------------------------------------
// 灰度字形：8bit alpha 覆盖（白字），调用方按 alpha 上色。下一次 renderGlyph
// 之后失效（缓冲随会话复用）。
struct GrayGlyph {
    int width = 0, height = 0;
    int pitch = 0;
    int left = 0, top = 0;         // 相对基线的偏移（top > 0 = 基线以上）
    int advance = 0;               // 前进宽（像素）
    const uint8_t* alpha = nullptr;
};

// 彩色字形：COLR/CPAL 层合成结果（预乘 BGRA），同样在下一次调用后失效。
struct ColorGlyph {
    int width = 0, height = 0;
    int baseline = 0;              // 基线在缓冲内的 y 坐标
    int advance = 0;               // 簇总前进宽（像素）
    const uint8_t* bgra = nullptr;
};

// 整形输出（glyph 级）
struct ShapedGlyph {
    uint32_t gid = 0;              // 字体内 glyph id
    uint32_t cluster = 0;          // 该 glyph 归属的文本簇起点（码点下标）
    int xAdvance = 0;
    int xOffset = 0;
};

// 度量（像素）
struct Metrics {
    int ascent = 0;
    int descent = 0;
    int height() const { return ascent + descent; }
};

// 一个字形簇的度量：簇切分规则（emoji ZWJ / 肤色 / 变体选择符 / 键帽 / 区域旗帜）
// 属于整形，所以在这里给出；上层换行只按"簇"推进即可。
struct Cluster {
    size_t len = 1;                // 该簇吃掉的码点数
    float advance = 0.0f;          // 簇总前进宽（像素）
    bool color = false;            // 是否走了彩色字形（COLR/CPAL）
};

// ------------------------------------------------------------
// face 描述与平台注入
// ------------------------------------------------------------
struct FaceDesc {
    std::string family;
    float sizePx = 0.0f;
    int weight = 400;
    bool italic = false;
};

// 平台字体发现：给"家族名 + 样式"返回字体文件路径。
// 返回空串 = 本机没有这个家族（模块会跳过它，与"没注册过"同一条路）。
// 具体枚举（EnumFontFamiliesEx / fontconfig / CoreText）由外壳实现。
using FaceResolver = std::function<std::string(const FaceDesc&)>;

// 文字阴影（多层，层序 = 绘制顺序，下标 0 在最下面）。
// 参数语义与 CSS text-shadow 的三个数一一对应，**取值由上层从样式里翻译**：
// 这里只收 dx / dy / blur（像素）与颜色，不认识"哪个节点的哪条声明"。
struct ShadowLayer {
    float dx = 0.0f;
    float dy = 0.0f;
    float blur = 0.0f;
    uint32_t color = 0;            // 0xAARRGGBB 直通（与 bin2d 对外颜色口径一致）
};

constexpr int kMaxTextShadows = 4;   // 与上层布局段的层数上限同量级

// ------------------------------------------------------------
// Library：face 加载与生命周期 + 光栅化 + 整形
//
// 一个进程一份就够（内部是 FreeType/HarfBuzz 的会话表，按 face|sizePx 缓存）。
// 线程不安全 —— 与 bin2d 的 Canvas 同口径：谁画谁的单线程。
// ------------------------------------------------------------
class Library {
public:
    Library();
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // FreeType 初始化。false = 文字不可用（不致命：上层把文字当空白画）。
    bool init();
    void shutdown();
    bool ready() const;

    // 平台发现注入（见 FaceResolver）。必须在 setFallbackChain / 首次使用之前设。
    void setResolver(FaceResolver resolver);
    // 默认家族名（"本机没指定时用哪个 face"）。空 = 无默认。
    void setDefaultFace(const std::string& name);
    const std::string& defaultFace() const;

    // 直接登记名字 → 文件路径（不走平台的调用方用这个；重复登记会覆盖）。
    bool addFont(const std::string& name, const std::string& path);

    // 有序列的候选链：命中即停。名字既没登记、Resolver 也答不出来的**跳过**。
    // 逐码点问字体（FT_Get_Char_Index）得到"谁有字形"，不按码点区间猜。
    void setFallbackChain(const std::vector<std::string>& faces);
    const std::vector<std::string>& fallbackChain() const;
    // 链上下标 → 名字（越界返回 defaultFace）
    const std::string& faceName(int index) const;
    // 返回第一个真正含该码点的链上下标；全都没有返回 -1。按码点缓存。
    int faceIndexFor(uint32_t cp);

    // 度量
    Metrics metrics(const std::string& face, float size);

    // ---- 光栅化 ----
    // 单码点灰度字形。subpixelX ∈ {0,1,2,3} = ¼ 像素横向相位（位图自带那份位移）。
    // false = 该 face 没有这个码点（advance 置 0，调用方兜底）。
    bool renderGlyph(const std::string& face, float size, uint32_t cp, bool bold, bool italic,
                     int subpixelX, GrayGlyph& out);
    bool renderGlyph(const std::string& face, float size, uint32_t cp, GrayGlyph& out);
    // 缺失字形（豆腐块）：空心方框 + 有效 advance。留白会被当成引擎 bug，
    // 方框才是"这个字体链里没有这个字符"的明确信号。
    bool renderMissingGlyph(float size, bool bold, bool italic, GrayGlyph& out);
    bool renderMissingGlyph(float size, GrayGlyph& out);

    // ---- 彩色字形（COLR/CPAL + 内嵌彩色位图）----
    // 整个簇一起合成（内部先整形：GSUB 合体/旗帜/肤色都归它）。
    // false = 该簇没有彩色层（调用方降级灰度）。
    bool renderColorCluster(const std::string& face, float size, const uint32_t* cps, size_t n,
                            ColorGlyph& out);
    bool hasColor(const std::string& face, float size, const uint32_t* cps, size_t n);

    // ---- shaping ----
    std::vector<ShapedGlyph> shape(const std::string& face, float size, const uint32_t* cps,
                                   size_t n);
    // 粗略脚本检测（阿拉伯 / 希伯来 / 印度系 / 泰文…）：只用来判断"要不要整段整形"
    bool needsShaping(const uint32_t* cps, size_t n);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ------------------------------------------------------------
// Atlas：图集打包与缓存（含文字阴影的模糊图集）+ 逐字形发射
//
// 纹理句柄与 bin2d 的外部纹理通道**同一套**：
//   · 挂了 GPU 执行体的 Canvas → Canvas::createTexture/uploadTexture/destroyTexture，
//     每次只上传新格子那块矩形；
//   · 纯 CPU 的 Canvas → 模块自有的 Image + Canvas::drawImage，逐像素立即落盘。
// 两种出口的字形位图逐字节相同，差异只在"位图怎么变成可采样的纹理"。
// ------------------------------------------------------------
class Atlas {
public:
    Atlas();
    ~Atlas();
    Atlas(const Atlas&) = delete;
    Atlas& operator=(const Atlas&) = delete;

    // 字体初始化（FreeType + 链上第一个可用 face）。false = 字体不可用，文字降级为空白。
    bool init(Library* lib);
    bool fontReady() const;

    // 纹理出口：每帧（Canvas 重建时）重设一次。CPU/GPU 出口切换会自动作废旧纹理。
    void setCanvas(Canvas* canvas);
    Canvas* canvas() const;

    // 主 face 名（回退链命中时的 0 号）。换 face 会让整形缓存与图集一起作废。
    void setFontName(const char* name);
    const std::string& fontName() const;

    // letter-spacing：每个字形簇之后额外的前进量（像素，可为负）。
    // 只影响推进量，不改字形位图 —— 所以它参与整形缓存键。
    void setLetterSpacing(float px);
    float letterSpacing() const;

    // true = 文字按整像素落位（默认，最锐利）；false = 把 x 的小数部分量化成
    // ¼ 像素相位，每份相位一张位图（动画里的连续滑动靠它）。
    void setSnapText(bool on);
    bool snapText() const;

    // 文字阴影：层序 = 绘制顺序（下标 0 在最下面）。blur < 0.5px 视为硬阴影
    // （直接复用字形格、只做偏移），更大则进模糊图集（每格每边多一圈余量）。
    void setTextShadows(const ShadowLayer* layers, int count);
    void clearTextShadow();
    bool textShadowActive() const;
    int textShadowCount() const;

    // 文本宽度（像素，逐字形簇 advance 之和，含 letter-spacing）。与 drawText
    // 用的是同一套度量与同一份缓存，所以"测宽 == 实际前进量"。
    float textWidth(float size, int weight, const std::string& utf8, bool italic);
    Metrics metrics(float size, int weight, bool italic);
    // 单个字形簇的度量（上层按簇换行时用：簇切分规则在模块里，不在上层）
    Cluster measureCluster(float size, int weight, const uint32_t* cps, size_t n, bool italic);

    // 画一行：y 是**基线**。居中/对齐由上层用它自己的度量算好再传进来。
    void drawText(float x, float baseline, float size, int weight, const std::string& utf8,
                  uint32_t color, bool italic);

    // 释放全部图集纹理与整形缓存（shutdown / 执行体重建时调）。
    void clear();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// ------------------------------------------------------------
// UTF-8 小工具（整形与簇切分都要，上层换行时也要按码点走）
// ------------------------------------------------------------
struct Utf8Iter {
    const std::string& s;
    size_t i = 0;
    uint32_t cp = 0;
    explicit Utf8Iter(const std::string& str) : s(str) {}
    bool next() {
        if (i >= s.size()) return false;
        const uint8_t b0 = static_cast<uint8_t>(s[i]);
        if (b0 < 0x80) { cp = b0; i++; return true; }
        const int n = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : 4;
        uint32_t c = b0 & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); k++)
            c = (c << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
        cp = c;
        i += n;
        return true;
    }
};

inline std::vector<uint32_t> Utf8ToCodepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    Utf8Iter it(s);
    while (it.next()) cps.push_back(it.cp);
    return cps;
}

inline std::string CodepointsToUtf8(const uint32_t* cps, size_t n) {
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; i++) {
        const uint32_t c = cps[i];
        if (c < 0x80) {
            s += static_cast<char>(c);
        } else if (c < 0x800) {
            s += static_cast<char>(0xC0 | (c >> 6));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            s += static_cast<char>(0xE0 | (c >> 12));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (c >> 18));
            s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return s;
}

// emoji 簇的长度（ZWJ 连写 / 肤色修饰 / 变体选择符 / 键帽 / 区域旗帜）。
// 整形侧与度量侧共用这一份，否则"测宽"和"绘制"会按不同粒度切簇。
size_t EmojiClusterLen(const uint32_t* cps, size_t n);

} // namespace text
} // namespace bin2d
