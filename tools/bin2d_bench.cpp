// ============================================================
// tools/bin2d_bench.cpp —— 单线程吞吐基准（512×512 目标，6 个固定场景）
//
// 为什么固定场景而不是"随机压力测试"：
//   优化要能对比。场景固定、尺寸固定、每项取**多次运行的最好值**，
//   机器负载抖动只影响最差值，不影响结论。
//
// 指标口径：
//   ms      ：该项最好一次的墙钟时间
//   ns/px   ：每**目标像素**的纳秒数（512×512 = 262144 像素）
//   MPix/s  ：每秒填多少百万像素（= 1e3 / ns_per_px）
//
// 六个场景对应五条用户能感觉到的路径 + 一条模糊：
//   ① 纯色圆角矩形（最常见的"卡片底"）
//   ② 线性渐变矩形
//   ③ 星形路径填充（多边、带扫描线穿越）
//   ④ 位图 2× 放大（双线性采样）
//   ⑤ 组透明度图层（离屏 + 整块合成）
//   ⑥ 盒式模糊（投影用）
// ============================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/image.h"
#include "bin2d/paint.h"
#include "bin2d/path.h"
#include "bin2d/raster.h"
#include "bin2d/surface.h"

using namespace bin2d;

namespace {

constexpr int kSize = 512;
constexpr int kPixels = kSize * kSize;

struct BenchResult {
    std::string name;
    const char* id = "";
    double ms = 0.0;
    double nsPerPx = 0.0;
    double mpix = 0.0;
};

Image MakeTestImage() {
    return Image::generate(256, 256, [](int x, int y) {
        const uint8_t r = static_cast<uint8_t>((x * 255) / 255);
        const uint8_t g = static_cast<uint8_t>((y * 255) / 255);
        const uint8_t b = static_cast<uint8_t>(((x + y) * 255) / 510);
        // 半透明格子：既考采样也考 tint 与 alpha
        const uint8_t a = ((x / 16 + y / 16) % 2) ? 255 : 160;
        return Color::straight(r, g, b, a);
    });
}

// 跑一项：重复到累计 ≥ 40ms 再取最好的一次；warmup 一次不计数
BenchResult Run(const char* name, const char* id, const std::function<void(Surface&)>& fn,
                std::vector<Surface>& keepAlive) {
    Surface surface(kSize, kSize);
    keepAlive.push_back(surface);

    fn(keepAlive.back());   // warmup（也是首次分配走完）
    double best = 1e30;
    int iters = 0;
    double total = 0.0;
    while (total < 40.0 && iters < 400) {
        auto t0 = std::chrono::steady_clock::now();
        fn(keepAlive.back());
        auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = std::min(best, ms);
        total += ms;
        ++iters;
    }
    BenchResult r;
    r.name = name;
    r.id = id;
    r.ms = best;
    r.nsPerPx = best * 1e6 / static_cast<double>(kPixels);
    r.mpix = (best > 0.0) ? (static_cast<double>(kPixels) / (best * 1e-3)) / 1e6 : 0.0;
    return r;
}

void PrintTable(const std::vector<BenchResult>& rows) {
    std::printf("%-26s %10s %10s %10s\n", "场景", "ms", "ns/px", "MPix/s");
    std::printf("%s\n", std::string(60, '-').c_str());
    for (const BenchResult& r : rows)
        std::printf("%-26s %10.3f %10.2f %10.1f\n", r.name.c_str(), r.ms,
                    r.nsPerPx, r.mpix);
}

} // namespace

int main(int argc, char** argv) {
    const bool json = (argc > 1 && std::string(argv[1]) == "--json");
    std::vector<Surface> keepAlive;
    keepAlive.reserve(8);
    std::vector<BenchResult> rows;

    const Image image = MakeTestImage();
    Path star;
    star.addStar(256.0f, 256.0f, 220.0f, 88.0f, 5);
    const Paint solid = Paint::solid(Color::argb(0xFF3E8BFF));
    const Paint gradient = Paint::linearAngle(Rect(40, 40, 432, 432), 35.0f,
                                              {GradientStop(0.0f, 0xFF3E8BFF),
                                               GradientStop(0.55f, 0xFF2FBF9A),
                                               GradientStop(1.0f, 0xFFF2B544)});

    rows.push_back(Run("圆角矩形（纯色）", "rounded_solid", [&](Surface& s) {
        Canvas c(s);
        c.clear(Color::trans());
        c.fillRounded(Rect(40, 40, 432, 432), 64.0f, solid);
    }, keepAlive));

    rows.push_back(Run("圆角矩形（线性渐变）", "rounded_gradient", [&](Surface& s) {
        Canvas c(s);
        c.clear(Color::trans());
        c.fillRounded(Rect(40, 40, 432, 432), 64.0f, gradient);
    }, keepAlive));

    rows.push_back(Run("星形路径填充", "star_path", [&](Surface& s) {
        Canvas c(s);
        c.clear(Color::trans());
        c.fillPath(star, solid);
    }, keepAlive));

    rows.push_back(Run("位图 2× 放大", "image_2x", [&](Surface& s) {
        Canvas c(s);
        c.clear(Color::trans());
        c.drawImage(image, Rect(0, 0, 512, 512), Rect(0, 0, 1, 1), Color::white(),
                    ImageFilter::Linear);
    }, keepAlive));

    rows.push_back(Run("组透明度图层合成", "layer_alpha", [&](Surface& s) {
        Canvas c(s);
        c.clear(Color::argb(0xFF101418));
        c.pushLayer(Rect(0, 0, 512, 512), 0.5f);
        Path block;
        block.addRoundedRect(Rect(40, 40, 200, 200), 24.0f);
        c.fillPath(block, solid);
        Path block2;
        block2.addRoundedRect(Rect(120, 120, 200, 200), 24.0f);
        c.fillPath(block2, Paint::solid(Color::argb(0xFFFF6B6B)));
        c.popLayer();
    }, keepAlive));

    rows.push_back(Run("盒式模糊 r=8", "blur_r8", [&](Surface& s) {
        Canvas c(s);
        c.clear(Color::argb(0xFF101418));
        c.fillRounded(Rect(96, 96, 320, 320), 48.0f, solid);
        Blur(s, 8.0f);
    }, keepAlive));

    if (json) {
        std::printf("{\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            std::printf("  \"%s\": {\"ms\": %.4f, \"ns_per_px\": %.3f, \"mpix\": %.2f}%s\n",
                        rows[i].id, rows[i].ms, rows[i].nsPerPx, rows[i].mpix,
                        i + 1 == rows.size() ? "" : ",");
        }
        std::printf("}\n");
    } else {
        PrintTable(rows);
    }

    // 把结果也打到 stdout 末尾（make bench 会 echo 这一行，方便粘贴到 docs/perf.md）
    double sum = 0.0;
    for (const BenchResult& r : rows) sum += r.nsPerPx;
    std::printf("\n合计 ns/px（六项之和，越小越好）: %.2f\n", sum);
    return 0;
}
