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
// 用法：make quality
//   有 binGPU（-DBIN2D_QUALITY_GPU）时额外跑 GPU 一致性那一项，否则打印跳过。
//   每一项都是"固定场景 + 固定尺寸"，所以两次运行的数字可以直接对比。
// ============================================================

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "bin2d/canvas.h"
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

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) g_dir = argv[1];
    MeasureAA();
    MeasureBanding();
    MeasureMinify();
    MeasureStroke();
    MeasureGpuParity();
    std::printf("图已写到 %s\n", g_dir.c_str());
    return 0;
}
