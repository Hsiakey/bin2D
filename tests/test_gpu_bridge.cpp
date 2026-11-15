// test_gpu_bridge —— 只在 binGPU 已构建（有 libbingpu.a）时编译/运行。
// 目的：软件画出来的这一帧，过一遍 bingpu 的 CPU 后端再读回来，必须还是它。
// 容差：REPLACE 混合 + 线性采样 + 预乘往返，允许 ≤1 的通道差（量化误差）。
#include "bin2d/canvas.h"
#include "bin2d/gpu.h"
#include "test_util.h"

using namespace bin2d;

int main() {
    Surface src(96, 64);
    {
        Canvas c(src);
        c.clear(Color::argb(0xFF101418));
        c.fillRounded(Rect(8, 8, 80, 48), 12.0f,
                      Paint::linearAngle(Rect(8, 8, 80, 48), 45.0f, 0xFF3E8BFF, 0xFF2FBF9A));
        Path ring;
        ring.addCircle(48, 32, 18);
        c.strokePath(ring, Color::argb(0xFFFF6B6B), 4.0f);
    }

    GpuBridge gpu;
    const bool created = gpu.create(src.width(), src.height());
    CHECK(created, "gpu: HEADLESS 设备 + 交换链 + 管线创建成功");
    if (!created) {
        std::printf("  （binGPU 报错：%s）\n", gpu.lastError().c_str());
        return Bin2dTestSummary("test_gpu_bridge");
    }

    Surface out;
    const bool ok = gpu.render(src, out);
    CHECK(ok, "gpu: 一帧渲染 + frame_read 成功");
    if (!ok) {
        std::printf("  （binGPU 报错：%s）\n", gpu.lastError().c_str());
        return Bin2dTestSummary("test_gpu_bridge");
    }

    int maxDiff = 0;
    for (int y = 0; y < src.height(); ++y) {
        for (int x = 0; x < src.width(); ++x) {
            const Color a = src.get(x, y);
            const Color b = out.get(x, y);
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)));
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)));
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(a.b) - static_cast<int>(b.b)));
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(a.a) - static_cast<int>(b.a)));
        }
    }
    CHECK(maxDiff <= 1, "gpu: GPU 往返与软件路径逐像素一致（容差 ≤1）");
    std::printf("  （最大通道差 %d）\n", maxDiff);
    return Bin2dTestSummary("test_gpu_bridge");
}
