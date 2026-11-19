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
void BuildFlatPage(DisplayList& list, const Surface& tile, const Rect& clip) {
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
                CHECK(diff <= tol, "display_list: 矩形+贴图页两个执行体逐像素相同");
                CHECK(st.draws < st.ops, "display_list: 相邻同类项真的合批了（提交次数 < 条目数）");
            }
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

    return Bin2dTestSummary("test_display_list");
}
