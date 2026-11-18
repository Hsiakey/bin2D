// ============================================================
// tools/bin2d_bench_gpu.cpp —— 图层合成的三条路线对比（CPU / GPU 每次上传 / GPU 纹理常驻）
//
// 为什么单独一个工具：bin2d 的主基准（bin2d_bench）是"纯 CPU 吞吐"，不需要显卡；
// 这个工具回答的是**路线问题**，而且必须在真实后端上跑：
//   路线 A：CPU 合成（DrawSurface 逐层压到底上）——总是可用，是权威口径；
//   路线 B：GPU 合成，但每帧把 N 个图层重新传一遍（= 只做了"批量"，没做"缓存"）；
//   路线 C：GPU 合成，纹理常驻显存，只在脏了才 upload（合成器 + 池子的目标形态）。
//
// 三个数字放在一起才有意义：
//   · B 比 A 慢是**正常且应该被看见的**（每帧 16×3.5 MB 的 PCIe 拷贝 + 反预乘）；
//   · C 才是 GPU 该有的样子，但它要求"图层内容不每帧变"——这正是 bin2d 的
//     Scene::version() 将来要喂的东西；
//   · 回读（frame_read）算在 GPU 两条路线里：bin2d 的口径是 Surface → Surface，
//     窗口呈现不是它的接口。要评估"不走回读的呈现"，请参考 binUI 的基准。
//
// 用法：bin2d_bench-gpu [cpu|vulkan|d3d11|gl|auto]   默认 auto（每个可用后端各跑一遍）
// ============================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/gpu.h"
#include "bin2d/paint.h"
#include "bin2d/surface.h"

using namespace bin2d;

namespace {

struct Result {
    std::string name;
    double ms = 0.0;
};

// 重复到累计 ≥ 60 ms（或 30 次），取最好一次
template <typename Fn>
double TimeIt(Fn&& fn) {
    fn();   // warmup
    double best = 1e30, total = 0.0;
    int iters = 0;
    while (total < 60.0 && iters < 30) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = std::min(best, ms);
        total += ms;
        ++iters;
    }
    return best;
}

// 一张"图层"素材：半透明 + 透明孔，尺寸给定
Surface MakeLayer(int w, int h, int seed) {
    Surface s(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (((x + seed * 5) % 37) == 0) continue;   // 透明列：零 alpha 也要走一遍
            const uint8_t a = ((x / 40 + y / 40 + seed) % 3 == 0)
                                  ? static_cast<uint8_t>(120 + (seed * 7) % 100)
                                  : 255;
            s.set(x, y, Color::straight(static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1)),
                                        static_cast<uint8_t>(seed * 37 + 40),
                                        static_cast<uint8_t>((y * 255) / (h > 1 ? h - 1 : 1)), a));
        }
    }
    return s;
}

const BlendMode kBlends[5] = {BlendMode::SrcOver, BlendMode::Replace, BlendMode::Erase,
                              BlendMode::Keep, BlendMode::Additive};

void PrintRow(const char* name, double ms, double baseline) {
    std::printf("%-46s %9.3f ms", name, ms);
    if (baseline > 0.0) std::printf("   %6.2fx", baseline / ms);
    std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
    const std::string want = (argc > 1) ? argv[1] : "auto";
    const int W = 1280, H = 720;
    const int kLayers = 16;

    // 两种图层尺寸：满幅（合成型界面的极端）和卡片型（真实 UI 的常态）
    const struct {
        const char* id;
        int w, h;
        int count;
    } kCases[] = {
        {"满幅 1280x720", W, H, kLayers},
        {"卡片 320x240", 320, 240, kLayers},
    };

    struct BackendChoice {
        const char* label;
        BgpuBackendKind kind;
    };
    std::vector<BackendChoice> backends;
    if (want == "cpu") backends.push_back({"cpu", BGPU_BACKEND_CPU});
    else if (want == "vulkan") backends.push_back({"vulkan", BGPU_BACKEND_VULKAN});
    else if (want == "d3d11") backends.push_back({"d3d11", BGPU_BACKEND_D3D11});
    else if (want == "gl") backends.push_back({"gl", BGPU_BACKEND_GL});
    else {
        backends.push_back({"cpu", BGPU_BACKEND_CPU});
        for (BgpuBackendKind k : {BGPU_BACKEND_VULKAN, BGPU_BACKEND_D3D11, BGPU_BACKEND_GL}) {
            if (bingpu_backend_available(k)) backends.push_back({bingpu_backend_name(k), k});
        }
    }

    for (const auto& c : kCases) {
        std::printf("\n=== 目标 %dx%d，%d 个图层（%s）===\n", W, H, c.count, c.id);
        std::vector<Surface> layers;
        for (int i = 0; i < c.count; ++i) layers.push_back(MakeLayer(c.w, c.h, i));
        // 图层位置：错开铺开，模拟真实叠层（不是完全重叠，也不是完全分开）
        std::vector<Rect> dst;
        for (int i = 0; i < c.count; ++i) {
            const float fx = static_cast<float>((i * 61) % std::max(1, W - c.w / 2));
            const float fy = static_cast<float>((i * 47) % std::max(1, H - c.h / 2));
            dst.push_back(Rect(fx, fy, static_cast<float>(c.w), static_cast<float>(c.h)));
        }

        // ---- 路线 A：CPU 合成 ----
        Surface cpuTarget(W, H);
        const double cpuMs = TimeIt([&]() {
            cpuTarget.clear(Color::trans());
            for (int i = 0; i < c.count; ++i) {
                RasterTarget rt;
                rt.surface = &cpuTarget;
                rt.clip = cpuTarget.bounds();
                rt.blend = kBlends[i % 5];
                DrawSurface(rt, layers[static_cast<size_t>(i)], dst[static_cast<size_t>(i)],
                            Rect(0, 0, 1, 1), Color::white(), false, Transform::identity());
            }
        });
        PrintRow("A  CPU 合成（逐层 DrawSurface）", cpuMs, cpuMs);

        for (const auto& b : backends) {
            GpuCompositor comp;
            if (!comp.create(W, H, b.kind)) {
                std::printf("   [%s] 跳过：%s\n", b.label, comp.lastError().c_str());
                continue;
            }
            // ---- 路线 B：每帧重新上传所有图层 ----
            double naiveMs = TimeIt([&]() {
                std::vector<GpuCompositor::Layer> gl;
                std::vector<BgpuTexture> texs;
                for (int i = 0; i < c.count; ++i) {
                    BgpuTexture t = comp.acquire(c.w, c.h);
                    comp.upload(t, layers[static_cast<size_t>(i)]);
                    GpuCompositor::Layer L;
                    L.texture = t;
                    L.dst = dst[static_cast<size_t>(i)];
                    L.blend = kBlends[i % 5];
                    gl.push_back(L);
                    texs.push_back(t);
                }
                Surface out;
                comp.compose(gl.data(), gl.size(), out);
                for (BgpuTexture t : texs) comp.recycle(t);
            });
            PrintRow((std::string("B  ") + b.label + " 每帧上传 + 合成").c_str(), naiveMs, cpuMs);

            // ---- 路线 C：纹理常驻，只合成 ----
            std::vector<BgpuTexture> texs;
            std::vector<GpuCompositor::Layer> gl;
            for (int i = 0; i < c.count; ++i) {
                BgpuTexture t = comp.acquire(c.w, c.h);
                comp.upload(t, layers[static_cast<size_t>(i)]);
                GpuCompositor::Layer L;
                L.texture = t;
                L.dst = dst[static_cast<size_t>(i)];
                L.blend = kBlends[i % 5];
                gl.push_back(L);
                texs.push_back(t);
            }
            Surface out;
            const double cachedMs = TimeIt([&]() { comp.compose(gl.data(), gl.size(), out); });
            PrintRow((std::string("C  ") + b.label + " 纹理常驻 + 合成（含回读）").c_str(),
                     cachedMs, cpuMs);

            // ---- 路线 D：纹理常驻 + 合成到离屏渲染目标（只量录制/提交）----
            BgpuTexture rt = comp.acquireTarget(W, H);
            const double noReadMs = TimeIt([&]() { comp.composeTo(gl.data(), gl.size(), rt); });
            PrintRow((std::string("D  ") + b.label + " 合到离屏目标（只录制，不等 GPU）").c_str(),
                     noReadMs, cpuMs);
            // ---- 路线 F：同样合成，但等 GPU 真的画完 ----
            const double doneMs = TimeIt([&]() {
                comp.composeTo(gl.data(), gl.size(), rt);
                comp.waitIdle();
            });
            PrintRow((std::string("F  ") + b.label + " 同上 + 等 GPU 画完").c_str(), doneMs, cpuMs);
            // ---- 路线 E：同样合成，但每帧只回读一次 ----
            const double oneReadMs = TimeIt([&]() {
                comp.composeTo(gl.data(), gl.size(), rt);
                comp.readTarget(rt, out);
            });
            PrintRow((std::string("E  ") + b.label + " 同上 + 每帧回读一次").c_str(), oneReadMs,
                     cpuMs);
            comp.recycle(rt);
            std::printf("     （C 路径 %d 层 → %d 次绘制，显存占用 %.1f MB）\n", comp.stats().layers,
                        comp.stats().draws,
                        static_cast<double>(c.count) * c.w * c.h * 4.0 / (1024.0 * 1024.0));
            for (BgpuTexture t : texs) comp.recycle(t);
        }
    }
    std::printf("\n（倍数列 = 相对 CPU 合成，>1 表示更快。D 只量命令录制；F 等 GPU 画完（真实帧时长）；E 再加一次回读。）\n");
    return 0;
}
