// ============================================================
// tools/bin2d_quality.cpp —— 绘制质量的**可复现取证**（数值 + 图）
//
// 为什么质量也要有工具：画质会悄悄退化 —— 一次"优化"删掉了 nearest 的快路径、
// 一次重构放宽了 1:1 特判的边界，像素测试未必覆盖到，肉眼却一眼看出来。
// 这里把"肉眼看到的"固定成两样东西：
//   ① **数字**：AA 面积误差 / 边缘位置误差 / 渐变可见级数与色带宽度 / 缩小预滤波与
//      理想盒式平均的偏差 / 1px 描边的亚像素覆盖率 / GPU 与 CPU 的最大通道差；
//   ② **图**：写到 build-mingw/out/quality/，放大到能直接看。
//
// 输出分流（0.9.1）：**计时的行全部走 stderr**，stdout 只留确定性内容。原因：宿主
// （binUI）把 `make quality` 的 stdout 原文记进它的画质台账并逐字比较，而 ms/µs 每跑
// 一次都不一样 —— 那会让宿主的门禁天天红，红到最后被人关掉。
//
// 用法：make quality
//   有 binGPU（-DBIN2D_QUALITY_GPU）时额外跑 GPU 一致性那一项，否则打印跳过。
//   每一项都是"固定场景 + 固定尺寸"，所以两次运行的数字可以直接对比。
// ============================================================

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/filter.h"
#include "bin2d/scene.h"

#include <memory>
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/png.h"
#include "bin2d/raster.h"
#ifdef BIN2D_QUALITY_GPU
#include "bin2d/gpu.h"
#endif

using namespace bin2d;

namespace {

std::string g_dir = ".";

void Save(const Surface& s, const std::string& name) {
    if (!writePng(s, g_dir + "/" + name)) std::printf("写 %s 失败\n", name.c_str());
}

// 纵向堆叠成一张对比图（每块等宽，四周留 gap）
void SaveSheet(const std::vector<Surface>& panels, const std::string& name, int gap = 6) {
    int w = 0, h = gap;
    for (const Surface& p : panels) {
        w = std::max(w, p.width());
        h += p.height() + gap;
    }
    Surface sheet(w + gap * 2, h);
    sheet.clear(Color::argb(0xFF202329));
    int y = gap;
    for (const Surface& p : panels) {
        for (int py = 0; py < p.height(); ++py)
            for (int px = 0; px < p.width(); ++px) sheet.set(gap + px, y + py, p.get(px, py));
        y += p.height() + gap;
    }
    Save(sheet, name);
}

Surface Upscale(const Surface& s, int k) {
    Surface out(s.width() * k, s.height() * k);
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x) out.set(x, y, s.get(x / k, y / k));
    return out;
}

// ------------------------------------------------------------
// 1) AA：面积守恒 + 边缘位置 + 放大看边缘坡度
// ------------------------------------------------------------
void MeasureAA() {
    const float r = 40.0f;
    Surface s(160, 160);                       // 透明底（面积统计要的是覆盖率）
    Canvas c(s);
    c.clear(Color::trans());
    Path p;
    p.addCircle(80.5f, 80.5f, r);
    c.fillPath(p, Color::white());
    double sum = 0.0;
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 160; ++x) sum += s.get(x, y).a / 255.0;
    const double exact = 3.14159265358979 * r * r;
    std::printf("[AA] 圆面积：覆盖率求和 %.1f，解析值 %.1f，误差 %.2f%%\n", sum, exact,
                (sum - exact) / exact * 100.0);

    Surface e(64, 64);                         // 45° 斜边
    Canvas c2(e);
    c2.clear(Color::trans());
    Path tp;
    tp.moveTo(0.0f, 0.0f);
    tp.lineTo(64.0f, 64.0f);
    tp.lineTo(0.0f, 64.0f);
    tp.close();
    c2.fillPath(tp, Color::white());
    double maxErr = 0.0;
    for (int x = 8; x < 56; ++x) {
        double found = -1.0;
        for (int y = 0; y < 63; ++y) {
            const double a0 = e.get(x, y).a / 255.0, a1 = e.get(x, y + 1).a / 255.0;
            if (a0 >= 0.5 && a1 < 0.5) {
                found = (y + 0.5) + (a0 - 0.5) / std::max(1e-6, a0 - a1);
                break;
            }
        }
        if (found < 0) continue;
        maxErr = std::max(maxErr, std::fabs(found - static_cast<double>(x)));
    }
    std::printf("[AA] 45° 边缘位置：与解析线最大偏差 %.3f px（竖直 8 条子扫描线的量化上限 1/16 px）\n",
                maxErr);

    Surface vis(160, 160);                     // 暗底 + 白色圆 + 4× 放大边缘
    vis.clear(Color::argb(0xFF101418));
    vis.blit(s, 0, 0);
    Surface zoom(160, 160);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x)
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 4; ++dx)
                    zoom.set(x * 4 + dx, y * 4 + dy, vis.get(60 + x, 20 + y));
    SaveSheet({vis, e, zoom}, "quality_aa.png");
}

// ------------------------------------------------------------
// 2) 阶调：不透明渐变 / 低 alpha 渐变的可见级数（抖动开 vs 关）
// ------------------------------------------------------------
void MeasureBanding() {
    Surface opaque(256, 128);
    Canvas c(opaque);
    c.clear(Color::trans());
    c.fillRect(Rect(0, 0, 256, 128),
               Paint::linearAngle(Rect(0, 0, 256, 128), 90.0f, 0xFF000000, 0xFFFFFFFF));

    // alpha = 0x14（7.8%）：预乘 8 位下 rgb 只剩个位数级 —— 8 位预乘的固有上限
    auto lowAlpha = [](bool dither) {
        Surface t(256, 128);
        Canvas cc(t);
        cc.clear(Color::argb(0xFFF0F2F5));
        Paint p = Paint::linearAngle(Rect(0, 0, 256, 128), 90.0f, 0x14C86432, 0x146446C8);
        p.dither = dither;
        cc.fillRect(Rect(0, 0, 256, 128), p);
        return t;
    };
    const Surface off = lowAlpha(false);
    const Surface on = lowAlpha(true);

    // 4px 块平均（半级分辨率）：色带 = 级数少 + 相邻块跳变大
    auto blocks = [](const Surface& s, int* levels, int* maxStep, int* longestRun) {
        std::vector<int> means;
        const int row = s.height() / 2;
        for (int x0 = 0; x0 + 4 <= s.width(); x0 += 4) {
            double acc = 0.0;
            for (int dx = 0; dx < 4; ++dx) acc += s.get(x0 + dx, row).r;
            means.push_back(static_cast<int>(acc / 4.0 * 2.0 + 0.5));
        }
        std::vector<int> uniq = means;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        *levels = static_cast<int>(uniq.size());
        int step = 0;
        for (size_t i = 1; i < means.size(); ++i)
            step = std::max(step, std::abs(means[i] - means[i - 1]));
        *maxStep = step;
        int run = 1, best = 1;
        for (int x = 1; x < s.width(); ++x) {
            if (s.get(x, row).r == s.get(x - 1, row).r) {
                best = std::max(best, ++run);
            } else {
                run = 1;
            }
        }
        *longestRun = best;
    };
    int la = 0, sa = 0, runOpaque = 0, ln = 0, sn = 0, lf = 0, sf = 0;
    blocks(opaque, &la, &sa, &runOpaque);
    int runOff = 0, runOn = 0;
    blocks(off, &ln, &sn, &runOff);
    blocks(on, &lf, &sf, &runOn);
    std::printf("[阶调] 不透明黑→白：%d 个半级，相邻块最大跳变 %d\n", la, sa);
    std::printf("[阶调] 低 alpha 渐变（抖动关）：%d 个半级，最长同值游程 %d px\n", ln, runOff);
    std::printf("[阶调] 低 alpha 渐变（抖动开）：%d 个半级，最长同值游程 %d px\n", lf, runOn);
    SaveSheet({opaque, off, on}, "quality_gradient.png");
}

// ------------------------------------------------------------
// 3) 缩小 8×：理想盒式平均 vs 0.3.0（无预滤波）vs 0.4.0（预滤波）
// ------------------------------------------------------------
void MeasureMinify() {
    Surface big(512, 512);                     // 2px 棋盘：最高频
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x)
            big.set(x, y, ((x / 2) + (y / 2)) % 2 ? Color::white() : Color::black());

    Surface ref(64, 64);                       // 理想：8×8 面积平均
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            double acc = 0.0;
            for (int dy = 0; dy < 8; ++dy)
                for (int dx = 0; dx < 8; ++dx) acc += big.get(x * 8 + dx, y * 8 + dy).r / 255.0;
            const uint8_t v = static_cast<uint8_t>(acc / 64.0 * 255.0 + 0.5);
            ref.set(x, y, Color(v, v, v, 255));
        }

    Surface oldWay(64, 64);                    // 0.3.0：drawImage 直接喂 DrawSurface
    oldWay.clear(Color::black());
    {
        RasterTarget rt;
        rt.surface = &oldWay;
        rt.clip = oldWay.bounds();
        DrawSurface(rt, big, Rect(0, 0, 64, 64), Rect(0, 0, 1, 1), Color::white(), false,
                    Transform::identity());
    }
    Surface now(64, 64);                       // 0.4.0：走 drawImage（有预滤波）
    {
        Canvas c(now);
        c.clear(Color::black());
        c.drawImage(Image(big), Rect(0, 0, 64, 64), Rect(0, 0, 1, 1), Color::white(),
                    ImageFilter::Linear);
    }
    auto meanAbsDiff = [](const Surface& a, const Surface& b) {
        double acc = 0.0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                acc += std::fabs(static_cast<double>(a.get(x, y).r) - b.get(x, y).r);
        return acc / (a.width() * a.height());
    };
    std::printf("[缩小 8x] 2px 棋盘 vs 理想盒式平均：0.3.0 %.1f 级、0.4.0 %.1f 级（0 最好）\n",
                meanAbsDiff(ref, oldWay), meanAbsDiff(ref, now));
    SaveSheet({Upscale(ref, 4), Upscale(oldWay, 4), Upscale(now, 4)}, "quality_minify.png");
}

// ------------------------------------------------------------
// 4) 描边：1px 线的亚像素平移（总墨水不变）、超细线不消失
// ------------------------------------------------------------
void MeasureStroke() {
    Surface s(120, 60);
    Canvas c(s);
    c.clear(Color::black());
    for (int i = 0; i < 4; ++i)
        c.fillRect(Rect(8, 8.0f + i * 12.0f + i * 0.25f, 100, 1.0f), Color::white());
    std::printf("[描边] 1px 水平线在 4 个亚像素偏移下的覆盖率总和：");
    for (int i = 0; i < 4; ++i) {
        double acc = 0.0;
        for (int x = 8; x < 108; ++x) acc += s.get(x, 8 + i * 12).a / 255.0;
        std::printf("%.1f ", acc);
    }
    std::printf("（理想 100）\n");

    Surface t(120, 40);
    Canvas c2(t);
    c2.clear(Color::trans());
    const float widths[4] = {0.1f, 0.5f, 1.0f, 2.0f};
    for (int i = 0; i < 4; ++i)
        c2.fillRect(Rect(20 + i * 25, 4, widths[i], 32), Color::white());
    std::printf("[描边] 0.1/0.5/1/2 px 竖线的峰值覆盖率：");
    for (int i = 0; i < 4; ++i) {
        uint8_t peak = 0;
        for (int y = 4; y < 36; ++y)
            for (int x = 20 + i * 25; x < 20 + i * 25 + 3; ++x) peak = std::max(peak, t.get(x, y).a);
        std::printf("%d ", peak);
    }
    std::printf("\n");
    SaveSheet({s, t}, "quality_stroke.png");
}

// ------------------------------------------------------------
// 5) 细节场景：GPU 合成 vs CPU（细网格 / 1.3px 弧 / 渐变 / 像素图标）
// ------------------------------------------------------------
void MeasureGpuParity() {
#ifdef BIN2D_QUALITY_GPU
    Surface src(384, 256);
    {
        Canvas c(src);
        c.clear(Color::argb(0xFF0B0E12));
        for (int i = 0; i < 24; ++i) {
            c.fillRect(Rect(8.0f + i * 15.5f, 8, 0.5f, 240), Color::argb(0x40FFFFFF));
            c.fillRect(Rect(8, 8.0f + i * 10.0f, 368, 0.5f), Color::argb(0x30FFFFFF));
        }
        Path ring;
        ring.addCircle(120.5f, 128.5f, 70.0f);
        c.strokePath(ring, Color::argb(0xFF5AC8FA), 1.3f);
        c.fillRounded(Rect(210, 40, 150, 180), 22.0f,
                      Paint::linearAngle(Rect(210, 40, 150, 180), 35.0f, 0xCC3E8BFF, 0x662FBF9A));
        Image icon(16, 16);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                icon.surface().set(x, y, ((x + y) % 2) ? Color::argb(0xFFFF6B6B)
                                                       : Color::argb(0xFF2B2F36));
        c.drawImage(icon, Rect(240, 60, 96, 96), Rect(0, 0, 1, 1), Color::white(),
                    ImageFilter::Nearest);
    }
    for (BgpuBackendKind k : {BGPU_BACKEND_VULKAN, BGPU_BACKEND_D3D11, BGPU_BACKEND_GL}) {
        if (!bingpu_backend_available(k)) continue;
        GpuCompositor comp;
        if (!comp.create(src.width(), src.height(), k)) {
            std::printf("[GPU 一致性] %s 不可用：%s\n", bingpu_backend_name(k),
                        comp.lastError().c_str());
            continue;
        }
        BgpuTexture t = comp.acquire(src.width(), src.height());
        comp.upload(t, src);
        GpuCompositor::Layer L;
        L.texture = t;
        L.dst = Rect(0, 0, src.width(), src.height());
        L.blend = BlendMode::Replace;
        Surface out;
        if (!comp.compose(&L, 1, out)) {
            std::printf("[GPU 一致性] %s 合成失败：%s\n", bingpu_backend_name(k),
                        comp.lastError().c_str());
            comp.recycle(t);
            continue;
        }
        int d = 0, over1 = 0;
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) {
                const Color a = src.get(x, y), b = out.get(x, y);
                const int m = std::max(std::max(std::abs(a.r - b.r), std::abs(a.g - b.g)),
                                       std::max(std::abs(a.b - b.b), std::abs(a.a - b.a)));
                d = std::max(d, m);
                if (m > 1) ++over1;
            }
        std::printf("[GPU 一致性] %-7s 细节场景 vs CPU：最大通道差 %d，>1 的像素 %d / %d\n",
                    bingpu_backend_name(k), d, over1, src.width() * src.height());
        comp.recycle(t);
    }
    Save(src, "quality_gpu_parity.png");
#else
    std::printf("[GPU 一致性] 跳过：本次构建没有 binGPU\n");
#endif
}

// ------------------------------------------------------------
// 6) 0.8.0 的三件新东西：颜色矩阵滤镜 / 投影的 alpha 罩 / 九宫格自动边界 + mip 上限
//
// 每一项都给"数字"而不是"看着挺好"：
//   · 滤镜：1 MP 的单张耗时 + 灰度解析值偏差 + identity 是否逐位不变；
//   · 投影：同尺寸下 RGBA 图层 vs alpha 罩的内存与模糊耗时（4× 内存是本次改动的前提）；
//   · 九宫格：探测到的边界与"应该等于圆角半径"的偏差；
//   · mip：满链与受预算截断的级数 / 字节。
// ------------------------------------------------------------
void MeasureFiltersShadowNine()
{
    // ---- 滤镜：1 MP ----
    Surface big(1000, 1000);
    {
        Canvas c(big);
        c.clear(Color::argb(0xFF1A1F26));
        for (int i = 0; i < 40; ++i)
            c.fillRounded(Rect(10.0f + i * 24.0f, 10.0f + (i % 7) * 130.0f, 120, 110),
                          18.0f, Paint::linearAngle(Rect(0, 0, 1000, 1000), 35.0f,
                                                   0xFF3E8BFF, 0xFF2FBF9A));
    }
    const ColorMatrix filters[] = {
        ColorMatrix::grayscale(1.0f),  ColorMatrix::saturate(1.6f),
        ColorMatrix::hueRotate(30.0f), ColorMatrix::contrast(1.3f),
        ColorMatrix::invert(1.0f),     ColorMatrix::brightness(0.8f),
    };
    const char* names[] = {"灰度", "饱和 1.6", "色相 30°", "对比度 1.3", "反相", "亮度 0.8"};
    double totalMs = 0.0;
    for (int i = 0; i < 6; ++i) {
        Surface work = big;
        const auto t0 = std::chrono::steady_clock::now();
        ApplyColorMatrix(work, filters[i]);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        totalMs += ms;
        std::fprintf(stderr, "[滤镜] %-9s 1 MP 单张 %6.2f ms（%.2f ns/像素）\n", names[i], ms,
                    ms * 1e6 / (1000.0 * 1000.0));
    }
    // 灰度解析值：纯红 → 0.213 × 255 = 54.3 → 54
    {
        Surface s(1, 1);
        s.set(0, 0, Color::argb(0xFFFF0000u));
        ApplyColorMatrix(s, ColorMatrix::grayscale(1.0f));
        const int g = s.get(0, 0).r;
        Surface same = big;
        ApplyColorMatrix(same, ColorMatrix::identity());
        bool identical = true;
        for (int y = 0; y < big.height() && identical; ++y)
            identical = std::memcmp(big.row(y), same.row(y),
                                    static_cast<size_t>(big.width()) * 4) == 0;
        std::fprintf(stderr, "[滤镜] 灰度(纯红)=%d（解析 54）  identity 逐位不变=%s  六张合计 %.2f ms\n", g,
                    identical ? "是" : "否", totalMs);
    }
    // 图：同一张底图依次套六个滤镜（人眼看色相与对比度的方向对不对）
    std::vector<Surface> panels;
    {
        Surface base(160, 90);
        Canvas c(base);
        c.fillRect(Rect(0, 0, 160, 90), Color::argb(0xFF202329));
        c.fillRounded(Rect(8, 8, 60, 74), 10, Paint::linearAngle(Rect(8, 8, 60, 74), 30,
                                                                 0xFF3E8BFF, 0xFF2FBF9A));
        Path star;
        star.addStar(100, 30, 24, 11, 5);
        c.fillPath(star, Color::argb(0xFFFFC857));
        c.fillRounded(Rect(78, 58, 74, 24), 8, Color::argb(0xFFE8EEF7));
        panels.push_back(base);
    }
    for (int i = 0; i < 6; ++i) {
        Surface p = panels.front();
        ApplyColorMatrix(p, filters[i]);
        panels.push_back(p);
    }
    SaveSheet(panels, "quality_filters.png");

    // ---- 投影：RGBA 图层 vs alpha 罩 ----
    {
        // 一张 200×120 卡片、模糊 12px 的投影：旧实现开 (200+24)×(120+24) 的 RGBA 图层
        const int lw = 224, lh = 144;
        const size_t rgbaBytes = static_cast<size_t>(lw) * static_cast<size_t>(lh) * 4u;
        const size_t maskBytes = static_cast<size_t>(lw) * static_cast<size_t>(lh);
        Surface layer(lw, lh);
        std::vector<uint8_t> mask(maskBytes, 200);
        const int iters = 40;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) Blur(layer, 12.0f);
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) BlurMask(mask.data(), lw, lh, 12.0f);
        const auto t2 = std::chrono::steady_clock::now();
        const double rgbaMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
        const double maskMs = std::chrono::duration<double, std::milli>(t2 - t1).count() / iters;
        std::fprintf(stderr, "[投影] %dx%d 模糊 r=12：RGBA 图层 %zu KB / %.3f ms  vs  alpha 罩 %zu KB / %.3f ms"
                    "（内存 %.1f×，耗时 %.1f×）\n",
                    lw, lh, rgbaBytes / 1024, rgbaMs, maskBytes / 1024, maskMs,
                    static_cast<double>(rgbaBytes) / static_cast<double>(maskBytes),
                    rgbaMs / maskMs);
    }

    // ---- 九宫格自动边界 ----
    {
        Surface src(48, 36);
        {
            Canvas c(src);
            c.fillRounded(Rect(0, 0, 48, 36), 12.0f, Color::white());
        }
        const Image img(src);
        const auto t0 = std::chrono::steady_clock::now();
        NineSliceInsets ins;
        for (int i = 0; i < 1000; ++i) ins = DetectNineSliceInsets(img);
        const auto t1 = std::chrono::steady_clock::now();
        const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / 1000.0;
        std::fprintf(stderr, "[九宫格] 48x36 圆角 12 → l=%.0f t=%.0f r=%.0f b=%.0f（应 = 12，±1）  %.1f µs/次\n",
                    ins.l, ins.t, ins.r, ins.b, us);

        // 图：原图 + 自动九宫格拉到 240×72（四角不该被拉成斜边）
        Surface out(248, 80);
        {
            Canvas c(out);
            c.clear(Color::argb(0xFF202329));
            c.drawNineSliceAuto(img, Rect(4, 4, 240, 72), Color::white());
        }
        Save(out, "quality_nineslice.png");
    }

    // ---- mip 上限 ----
    {
        Image img(64, 64);
        Surface& s = img.surface();
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                s.set(x, y, ((x / 8 + y / 8) % 2) ? Color::white() : Color::argb(0xFF202020));
        img.sampleSurface(64.0f);
        std::printf("[mip] 64x64 满链：%d 级 / %zu 字节\n", img.mipLevelCount(), img.mipBytes());
        Image capped(64, 64);
        capped.surface() = img.surface();
        capped.setMipByteBudget(5000);
        capped.sampleSurface(64.0f);
        std::printf("[mip] 预算 5000 字节：%d 级 / %zu 字节（第一级 4096 已用掉大半）\n",
                    capped.mipLevelCount(), capped.mipBytes());
    }
}

// ------------------------------------------------------------
// 7) 子树缓存（0.9.0）：1280×720、40 张卡片（每张 5 个图元）的两种画法
//
// 这是 roadmap ⑤ 的开工判据的实测：全量重绘多少 ms、命中缓存后多少 ms。
// 缓存只改变"重画的数量"，像素由 tests/test_cache.cpp 逐像素钉死。
// ------------------------------------------------------------
void MeasureSceneCache() {
    Scene scene(1280, 720);
    scene.setBackground(Color::argb(0xFF10141A));
    for (int i = 0; i < 40; ++i) {
        auto g = std::make_unique<Group>();
        g->transform = Transform::translate(24.0f + (i % 8) * 154.0f, 24.0f + (i / 8) * 136.0f);
        const Rect card(0, 0, 140, 118);
        auto add = [&](const Rect& r, float radius, const Paint& paint) {
            auto s2 = std::make_unique<Shape>();
            s2->path.addRoundedRect(r, radius);
            s2->paint = paint;
            g->add(std::move(s2));
        };
        add(card, 12.0f, Paint::linearAngle(card, 35.0f, 0xFF22303F, 0xFF1A2430));
        add(Rect(12, 12, 40, 40), 10.0f, Paint::solid(Color::argb(0xFF3E8BFF)));
        add(Rect(62, 16, 66, 9), 4.0f, Paint::solid(Color::argb(0xFF8FA3B8)));
        add(Rect(62, 32, 48, 9), 4.0f, Paint::solid(Color::argb(0xFF8FA3B8)));
        add(Rect(12, 66, 116, 38), 8.0f,
            Paint::linearAngle(Rect(12, 66, 116, 38), 10.0f, 0xFF2FBF9A, 0xFF1E8F74));
        scene.root().add(std::move(g));
    }
    Surface target(1280, 720);
    const int frames = 20;
    const auto bench = [&](auto&& fn) {
        fn();   // 预热（首次会分配缓冲）
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) fn();
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;
    };
    const double uncached = bench([&] { scene.draw(target); });
    SceneCache cache;
    const double cached = bench([&] { scene.drawCached(target, cache); });
    std::fprintf(stderr, "[缓存] 1280x720 / 40 组 / 200 个图元：全量 %.2f ms（%.0f fps） → 缓存命中 %.2f ms（%.0f fps） = %.1f×\n",
                uncached, 1000.0 / uncached, cached, 1000.0 / cached, uncached / cached);
    std::fprintf(stderr, "[缓存] 命中 %d/%d，常驻 %zu KB（%d 条）\n", cache.stats().hits,
                cache.stats().groups_total, cache.stats().bytes / 1024, cache.stats().entries);
    Save(target, "quality_cache.png");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_dir = argv[1];
    MeasureAA();
    MeasureBanding();
    MeasureMinify();
    MeasureStroke();
    MeasureGpuParity();
    MeasureFiltersShadowNine();
    MeasureSceneCache();
    std::printf("图已写到 %s\n", g_dir.c_str());
    return 0;
}
