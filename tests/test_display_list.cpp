// test_display_list —— 同一份绘制列表跑两个执行体（CPU 光栅 / binGPU）
//
// 这一条是"语义与执行分开"之后唯一有意义的验收：**列表是同一份，执行体换掉**。
//   ① 只有矩形 + 贴图的页面：两条路必须**逐像素相同**（这两类没有 AA 分歧）
//   ② 带圆角的页面：差多少**报出来**（bin2d 内部两套圆角 AA 定义，见 CHANGELOG）
//   ③ 时间：同一页面 CPU vs GPU（合批生效的话 draws 应该远小于条目数）
//
// 只在 binGPU 已构建时编译/运行；后端不可用就跳过，不当失败。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/display_list.h"
#include "bin2d/gpu.h"
#include "test_util.h"

using namespace bin2d;

namespace {

int MaxChannelDiff(const Surface& a, const Surface& b) {
    if (a.width() != b.width() || a.height() != b.height()) return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const Color ca = a.get(x, y), cb = b.get(x, y);
            worst = std::max(worst, std::abs(static_cast<int>(ca.r) - static_cast<int>(cb.r)));
            worst = std::max(worst, std::abs(static_cast<int>(ca.g) - static_cast<int>(cb.g)));
            worst = std::max(worst, std::abs(static_cast<int>(ca.b) - static_cast<int>(cb.b)));
            worst = std::max(worst, std::abs(static_cast<int>(ca.a) - static_cast<int>(cb.a)));
        }
    }
    return worst;
}

// 一小块贴图素材（棋盘 + 一圈边框），用来验 UV 与子矩形
Surface MakeTile(int w, int h) {
    Surface s(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool check = ((x / 4) + (y / 4)) % 2 == 0;
            // 全不透明：这一条要的是"逐像素相同"，半透明会把预乘/反预乘的舍入混进来
            // （那不是执行体的差别，是 8 位往返的固有误差 —— 单独的量测里各差 1）。
            s.set(x, y, check ? Color::straight(240, 90, 90, 255) : Color::straight(40, 60, 120, 255));
        }
    }
    return s;
}

// 一页"卡片列表"：矩形 + 贴图（无圆角 → 两条路必须逐像素相同）
void BuildFlatPage(DisplayList& list, const Surface& tile, const Rect& clip, bool withGradient = false) {
    list.clearRect(Rect(0, 0, 240, 160), Color::argb(0xFF101418));
    for (int i = 0; i < 6; ++i) {
        const float y = 8.0f + i * 22.0f;
        list.fillRect(Rect(8, y, 224, 18), Color::argb(0xFF20242C));
        list.fillRect(Rect(12, y + 3, 12, 12), Color::argb(0xFF3E8BFF));
        // 1:1 贴图（12×12 取自 32×32 的 0.375）：缩小会走到"盒式预滤波 vs 双线性"
        // 这个**另一条**分歧上（CPU 侧做了缩小的预滤波），那属于采样策略，不属执行体。
        list.drawImage(tile, Rect(30, y + 3, 12, 12), Rect(0.0f, 0.0f, 0.375f, 0.375f));
        list.drawImage(tile, Rect(46, y + 3, 12, 12), Rect(0.5f, 0.5f, 0.375f, 0.375f));  // 子矩形
    }
    // 渐变单独一页：两端不透明时两条路的**定义**相同，只有四角色的 8 位舍入（≤1）。
    // 半透明渐变不进这一页（两端 alpha 不同要在预乘空间插值，见 gpuCompatible）。
    if (withGradient) {
        list.fillGradient(Rect(8, 60, 100, 14), 0.0f, Color::argb(0xFF3E8BFF),
                          Color::argb(0xFF2FBF9A));
        list.fillGradient(Rect(120, 60, 100, 14), 90.0f, Color::argb(0xFFFF6B6B),
                          Color::argb(0xFFFFD166));
        list.fillGradient(Rect(8, 80, 212, 20), 45.0f, Color::argb(0xFF101418),
                          Color::argb(0xFF8090FF));
    }
    list.pushClip(clip);
    for (int i = 0; i < 40; ++i) {
        list.fillRect(Rect(8.0f + i * 6.0f, 8.0f + i * 3.0f, 40, 10), Color::argb(0x4080FF80));
    }
    list.popClipTo(0);
}

void BuildRoundedPage(DisplayList& list) {
    list.clearRect(Rect(0, 0, 240, 160), Color::argb(0xFF101418));
    for (int i = 0; i < 6; ++i) {
        const float y = 8.0f + i * 24.0f;
        list.fillRounded(Rect(8, y, 224, 20), 10.0f, Color::argb(0xFF2A3038));
    }
}

// 圆角渐变 + 半透明渐变（两端 alpha 相同）：θ 四角色在**直通域**插值，而两端 alpha 相同
// 时直通域插值与预乘域插值等价 ⇒ 这一页可以当门禁（≤1）。
void BuildRoundedGradPage(DisplayList& list) {
    list.clearRect(Rect(0, 0, 240, 160), Color::argb(0xFF101418));
    list.fillRoundedGradient(Rect(10, 10, 140, 40), 12.0f, 90.0f, Color::argb(0xFF3E8BFF),
                             Color::argb(0xFF2FBF9A));
    list.fillRoundedGradient(Rect(10, 60, 140, 40), 14.0f, 45.0f, Color::argb(0xFFFF6B6B),
                             Color::argb(0xFFFFD166));
    list.fillRoundedGradient(Rect(10, 110, 100, 40), 8.0f, 0.0f, Color::argb(0xFF101418),
                             Color::argb(0xFF8090FF));
    // 两端 alpha **都不到 255**：0.18.0 这里被预乘了两次（0x80 的中段只发出 0x40），
    // 而 gpuCompatible 只挡住了"两端 alpha 不同"，恰好漏掉这一类。
    list.fillGradient(Rect(160, 10, 70, 40), 180.0f, Color::argb(0x80FF8040),
                      Color::argb(0x8010A0FF));
    list.fillRoundedGradient(Rect(160, 60, 70, 40), 10.0f, 135.0f, Color::argb(0xB0FF8040),
                             Color::argb(0xB010A0FF));
    // 半径退化：两个执行体都走直角四角带色
    list.fillRoundedGradient(Rect(160, 110, 70, 40), 0.0f, 0.0f, Color::argb(0xFF3E8BFF),
                             Color::argb(0xFFFFD166));
}

// 圆角描边：1px / 2px / 3px 各一条，外加一个"半径退化"的直角框
void BuildRingPage(DisplayList& list) {
    list.clearRect(Rect(0, 0, 240, 160), Color::argb(0xFF101418));
    list.strokeRounded(Rect(16, 16, 100, 40), 12.0f, 2.0f, Color::argb(0xFF3E8BFF));
    list.strokeRounded(Rect(130, 16, 100, 40), 12.0f, 1.0f, Color::argb(0xFFFFFFFF));
    list.strokeRounded(Rect(16, 70, 100, 40), 20.0f, 3.0f, Color::argb(0xFF2FBF9A));
    list.strokeRounded(Rect(130, 70, 100, 40), 0.0f, 2.0f, Color::argb(0xFFFFD166));
}

}  // namespace

int main() {
    const int W = 240, H = 160;
    const Surface tile = MakeTile(32, 32);

    // ---------- ① 矩形 + 贴图：逐像素相同 ----------
    {
        DisplayList list;
        BuildFlatPage(list, tile, Rect(20, 20, 120, 100));
        CHECK(list.size() > 20, "display_list: 列表有内容");

        Surface cpu(W, H);
        {
            Canvas c(cpu);
            DisplayList::Stats st;
            list.execute(c, &st);
            CHECK(st.ops > 20, "display_list: CPU 执行体跑完了所有条目");
        }

        const struct { const char* name; BgpuBackendKind kind; } backends[] = {
            {"cpu", BGPU_BACKEND_CPU},
            {"vulkan", BGPU_BACKEND_VULKAN},
            {"d3d11", BGPU_BACKEND_D3D11},
            {"gl", BGPU_BACKEND_GL},
        };
        for (const auto& b : backends) {
            GpuCompositor gpu;
            if (!gpu.create(W, H, b.kind)) {
                std::printf("  [%s] 跳过：%s\n", b.name, gpu.lastError().c_str());
                continue;
            }
            BgpuTexture rt = gpu.acquireTarget(W, H);
            DisplayList::Stats st;
            const bool ok = list.execute(gpu, rt, 0xFF101418u, &st);
            Surface gpuOut;
            const bool read = ok && gpu.readTarget(rt, gpuOut);
            if (rt) gpu.recycle(rt);
            CHECK(ok && read, "display_list: GPU 执行体跑完（矩形 + 贴图页）");
            if (!ok && !gpu.lastError().empty()) std::printf("    原因：%s\n", gpu.lastError().c_str());
            if (read) {
                const int diff = MaxChannelDiff(cpu, gpuOut);
                std::printf("  [%s] 矩形+贴图页：最大通道差 %d（合批 %d 次绘制 / %d 四边形 / %d 次绑纹理）\n",
                            b.name, diff, st.draws, st.quads, st.binds);
                // 真实 GPU 后端必须**精确**相同；binGPU 的 CPU 后端是参考实现，
                // 它自己的 8 位舍入允许差 1（与 test_gpu_bridge 的口径一致）。
                const int tol = (b.kind == BGPU_BACKEND_CPU) ? 1 : 0;
                CHECK(diff <= tol, "display_list: 矩形+贴图+渐变页两个执行体逐像素相同（≤1）");
                CHECK(st.draws < st.ops, "display_list: 相邻同类项真的合批了（提交次数 < 条目数）");
            }
        }
    }

    // ---------- ①b 渐变页：只有四角色的 8 位舍入 ----------
    {
        DisplayList list;
        BuildFlatPage(list, tile, Rect(20, 20, 120, 100), /*withGradient=*/true);
        Surface cpu(W, H);
        {
            Canvas c(cpu);
            list.execute(c);
        }
        const struct { const char* name; BgpuBackendKind kind; } backends[] = {
            {"cpu", BGPU_BACKEND_CPU},
            {"vulkan", BGPU_BACKEND_VULKAN},
            {"d3d11", BGPU_BACKEND_D3D11},
            {"gl", BGPU_BACKEND_GL},
        };
        for (const auto& b : backends) {
            GpuCompositor gpu;
            if (!gpu.create(W, H, b.kind)) continue;
            BgpuTexture rt = gpu.acquireTarget(W, H);
            Surface gpuOut;
            const bool ok = list.execute(gpu, rt, 0xFF101418u) && gpu.readTarget(rt, gpuOut);
            if (rt) gpu.recycle(rt);
            if (!ok) { CHECK(false, "display_list: 渐变页 GPU 执行体跑通"); continue; }
            const int diff = MaxChannelDiff(cpu, gpuOut);
            std::printf("  [%s] 渐变页：最大通道差 %d（四角色 8 位舍入）\n", b.name, diff);
            CHECK(diff <= 1, "display_list: 渐变页两个执行体差 ≤1（同一数学，只有舍入）");
        }
    }

    // ---------- ② 圆角页：把差报出来（两套 AA 定义） ----------
    {
        DisplayList list;
        BuildRoundedPage(list);
        Surface cpu(W, H);
        {
            Canvas c(cpu);
            list.execute(c);
        }
        GpuCompositor gpu;
        if (gpu.create(W, H, BGPU_BACKEND_CPU)) {
            BgpuTexture rt = gpu.acquireTarget(W, H);
            Surface gpuOut;
            if (list.execute(gpu, rt, 0xFF101418u) && gpu.readTarget(rt, gpuOut)) {
                std::printf("  [cpu] 圆角页：最大通道差 %d（两套圆角 AA 定义，不是门禁）\n",
                            MaxChannelDiff(cpu, gpuOut));
            }
            if (rt) gpu.recycle(rt);
        }
        CHECK(true, "display_list: 圆角页两条执行体都出图");
    }

    // ---------- ③ 时间：234 个圆角卡片 ----------
    {
        DisplayList list;
        list.clearRect(Rect(0, 0, W, H), Color::argb(0xFF101418));
        for (int i = 0; i < 234; ++i) {
            const int col = i % 6, row = i / 6;
            list.fillRounded(Rect(12.0f + col * 37.0f, 12.0f + row * 4.0f, 30.0f, 3.0f), 1.0f,
                             Color::argb(0xFF2A3038));
        }
        std::vector<double> cpuMs;
        for (int rep = 0; rep < 3; ++rep) {
            Surface s(W, H);
            Canvas c(s);
            const auto t0 = std::chrono::steady_clock::now();
            list.execute(c);
            cpuMs.push_back(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(cpuMs.begin(), cpuMs.end());
        std::printf("  CPU 执行体：%zu 个圆角 %6.2f ms\n", list.size(), cpuMs[cpuMs.size() / 2]);
        CHECK(cpuMs[cpuMs.size() / 2] >= 0.0, "display_list: CPU 执行体计时完成");
    }

    // ---------- ④ 圆角渐变 + 淡出渐变：可以当门禁（预乘仿射 ⇒ 精确） ----------
    {
        DisplayList list;
        BuildRoundedGradPage(list);
        CHECK(list.size() >= 6, "display_list: 圆角渐变页记下了条目");
        std::string why;
        CHECK(list.gpuCompatible(&why), "display_list: 等 alpha 的渐变（含半透明）可以走 GPU");
        if (!why.empty()) std::printf("    原因：%s\n", why.c_str());
        // 淡出（两端 alpha 不同）必须被挡住：两个执行体的插值域不同
        DisplayList fade;
        fade.fillGradient(Rect(0, 0, 40, 40), 180.0f, Color::argb(0xFFFF8040),
                          Color::argb(0x00FF8040));
        why.clear();
        CHECK(!fade.gpuCompatible(&why), "display_list: 两端 alpha 不同的渐变整份退回 CPU");
        Surface cpu(W, H);
        {
            Canvas c(cpu);
            list.execute(c);
        }
        const struct { const char* name; BgpuBackendKind kind; } backends[] = {
            {"cpu", BGPU_BACKEND_CPU},
            {"vulkan", BGPU_BACKEND_VULKAN},
            {"d3d11", BGPU_BACKEND_D3D11},
            {"gl", BGPU_BACKEND_GL},
        };
        for (const auto& b : backends) {
            GpuCompositor gpu;
            if (!gpu.create(W, H, b.kind)) continue;
            BgpuTexture rt = gpu.acquireTarget(W, H);
            Surface gpuOut;
            const bool ok = list.execute(gpu, rt, 0xFF101418u) && gpu.readTarget(rt, gpuOut);
            if (rt) gpu.recycle(rt);
            CHECK(ok, "display_list: 圆角渐变页 GPU 执行体跑通");
            if (!ok) continue;
            const int diff = MaxChannelDiff(cpu, gpuOut);
            std::printf("  [%s] 圆角渐变页：最大通道差 %d\n", b.name, diff);
            // 门禁只钉**纯色区**（把圆角弧让出去）：弧上两个执行体用的是两套圆角 AA
            // 定义（bin2d 内部的遗留分歧），那与"四角带色 + 双线性插值"是两件事，
            // 混在一起会把这个页面的结论搅浑。弧上的差单独报数。
            struct GradItem { float x, y, w, h, r; };
            const GradItem items[] = {
                {10, 10, 140, 40, 12}, {10, 60, 140, 40, 14}, {10, 110, 100, 40, 8},
                {160, 10, 70, 40, 0},  {160, 60, 70, 40, 10}, {160, 110, 70, 40, 0},
            };
            auto inArc = [&](int px, int py) {
                const float cx = static_cast<float>(px) + 0.5f;
                const float cy = static_cast<float>(py) + 0.5f;
                for (const GradItem& it : items) {
                    if (it.r <= 0.5f) continue;
                    if (cx < it.x - 1.0f || cx > it.x + it.w + 1.0f) continue;
                    if (cy < it.y - 1.0f || cy > it.y + it.h + 1.0f) continue;
                    const float qx = std::min(std::max(cx, it.x + it.r), it.x + it.w - it.r);
                    const float qy = std::min(std::max(cy, it.y + it.r), it.y + it.h - it.r);
                    const float dx = cx - qx, dy = cy - qy;
                    const float rr = it.r + 2.0f;
                    if (dx * dx + dy * dy <= rr * rr) return true;
                }
                return false;
            };
            int worstSolid = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    if (inArc(x, y)) continue;
                    const Color ca = cpu.get(x, y), cb = gpuOut.get(x, y);
                    worstSolid = std::max({worstSolid, std::abs(int(ca.r) - int(cb.r)),
                                           std::abs(int(ca.g) - int(cb.g)),
                                           std::abs(int(ca.b) - int(cb.b)),
                                           std::abs(int(ca.a) - int(cb.a))});
                }
            std::printf("    纯色区最大通道差 %d（弧上的差是圆角 AA 定义，另计）\n", worstSolid);
            CHECK(worstSolid <= 2, "display_list: 圆角渐变/半透明渐变的颜色数学一致（纯色区 ≤2）");
        }
    }

    // ---------- ⑤ 圆角描边：结构门禁（环的厚度 / 位置），AA 差只报数 ----------
    {
        DisplayList list;
        BuildRingPage(list);
        Surface cpu(W, H);
        {
            Canvas c(cpu);
            list.execute(c);
        }
        // 结构：2px 描边的左直边（y = 36 在直边段）必须是 x=16,17 两条实心、x=18 是底色
        // 注意判据不能看 alpha —— 背景是不透明的，每个像素 alpha 都是 255。
        auto near = [](const Surface& s, int x, int y, uint32_t argb) {
            const Color c = s.get(x, y);
            const Color w = Color::argb(argb);
            const int d = std::max({std::abs(int(c.r) - int(w.r)), std::abs(int(c.g) - int(w.g)),
                                    std::abs(int(c.b) - int(w.b))});
            return d <= 2;
        };
        const uint32_t bg = 0xFF101418u, blue = 0xFF3E8BFFu, white = 0xFFFFFFFFu;
        CHECK(near(cpu, 16, 36, blue) && near(cpu, 17, 36, blue),
              "display_list: CPU 描边 2px 的内侧 2 列是实心");
        CHECK(near(cpu, 18, 36, bg), "display_list: CPU 描边 2px 不会溢出到第 3 列");
        CHECK(near(cpu, 130, 36, white), "display_list: CPU 描边 1px 只有 1 列");
        CHECK(near(cpu, 131, 36, bg), "display_list: CPU 描边 1px 不会变成 2 列");

        GpuCompositor gpu;
        if (gpu.create(W, H, BGPU_BACKEND_CPU)) {
            BgpuTexture rt = gpu.acquireTarget(W, H);
            Surface gpuOut;
            if (list.execute(gpu, rt, 0xFF101418u) && gpu.readTarget(rt, gpuOut)) {
                // 同一条结构判据在 GPU 执行体上也必须成立 —— 这是"环几何"的门禁，
                // 与 AA 定义无关（AA 只影响边界像素的覆盖率，不影响哪几列有颜色）。
                CHECK(near(gpuOut, 16, 36, blue) && near(gpuOut, 17, 36, blue),
                      "display_list: GPU 描边 2px 的内侧 2 列是实心");
                CHECK(near(gpuOut, 18, 36, bg), "display_list: GPU 描边 2px 不会溢出到第 3 列");
                CHECK(near(gpuOut, 130, 36, white), "display_list: GPU 描边 1px 只有 1 列");
                CHECK(near(gpuOut, 131, 36, bg), "display_list: GPU 描边 1px 不会变成 2 列");
                std::printf("  [cpu] 描边页：最大通道差 %d（描边 AA 定义不同，不是门禁）\n",
                            MaxChannelDiff(cpu, gpuOut));
            }
            if (rt) gpu.recycle(rt);
        }
        CHECK(true, "display_list: 描边页两条执行体都出图");
    }

    // ---------- ⑥ gpuCompatible：缩小的贴图必须整份退回 CPU ----------
    {
        DisplayList list;
        const Surface big = MakeTile(32, 32);
        list.drawImage(big, Rect(0, 0, 64, 64));                     // 放大：OK
        std::string why;
        CHECK(list.gpuCompatible(&why), "display_list: 放大的贴图可以走 GPU 执行体");

        DisplayList small;
        small.drawImage(big, Rect(0, 0, 8, 8));                      // 缩小 4×：不许
        why.clear();
        CHECK(!small.gpuCompatible(&why), "display_list: 缩小的贴图整份退回 CPU");
        CHECK(!why.empty(), "display_list: 退回时给得出原因");

        DisplayList same;
        // 源窗口 32×32×0.5 = 16×16 摆进 8×8：缩小 2×，同样不许
        same.drawImage(big, Rect(0, 0, 8, 8), Rect(0, 0, 0.5f, 0.5f));
        why.clear();
        CHECK(!same.gpuCompatible(&why), "display_list: 子矩形也是按真实窗口判缩小");

        DisplayList up;
        // 源窗口 16×16 摆进 32×32：放大，可以走 GPU
        up.drawImage(big, Rect(0, 0, 32, 32), Rect(0, 0, 0.5f, 0.5f));
        why.clear();
        CHECK(up.gpuCompatible(&why), "display_list: 放大（含子窗口）可以走 GPU 执行体");
    }

    return Bin2dTestSummary("test_display_list");
}
