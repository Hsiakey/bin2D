// test_gpu_bridge —— 只在 binGPU 已构建（有 libbingpu.a）时编译/运行。
//
// 目的（两件，缺一不可）：
//   ① 软件画出来的这一帧，过一遍 bingpu 的**CPU 后端**再读回来，必须还是它；
//   ② 同一帧过**每个可用的真实后端**（Vulkan / D3D11 / GL）读回来，同样必须是它 ——
//      这条是 bin2d 对 binGPU 的 P1（命令 → 像素）的直接消费方证据：上层一行不改，
//      底下换后端，像素不许动。
//
// 容差：REPLACE 混合 + 线性采样 + 预乘往返，允许 ≤1 的通道差（量化误差）。
// 后端不可用时**跳过**（打印原因），不是失败 —— CI 机上没有显卡不能红。
#include "bin2d/canvas.h"
#include "bin2d/gpu.h"
#include "test_util.h"

using namespace bin2d;

namespace {

// 一帧测试图：底色 + 圆角渐变 + 圆环描边（尽量把"几何/渐变/混合"都带上）
Surface MakeSource() {
    Surface src(96, 64);
    Canvas c(src);
    c.clear(Color::argb(0xFF101418));
    c.fillRounded(Rect(8, 8, 80, 48), 12.0f,
                  Paint::linearAngle(Rect(8, 8, 80, 48), 45.0f, 0xFF3E8BFF, 0xFF2FBF9A));
    Path ring;
    ring.addCircle(48, 32, 18);
    c.strokePath(ring, Color::argb(0xFFFF6B6B), 4.0f);
    return src;
}

int MaxChannelDiff(const Surface& a, const Surface& b) {
    int maxDiff = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const Color ca = a.get(x, y);
            const Color cb = b.get(x, y);
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(ca.r) - static_cast<int>(cb.r)));
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(ca.g) - static_cast<int>(cb.g)));
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(ca.b) - static_cast<int>(cb.b)));
            maxDiff = std::max(maxDiff, std::abs(static_cast<int>(ca.a) - static_cast<int>(cb.a)));
        }
    }
    return maxDiff;
}

// 一个后端跑一遍；返回 true = 通过（或跳过）
bool RunBackend(const char* label, BgpuBackendKind kind, const Surface& src) {
    GpuBridge gpu;
    if (!gpu.create(src.width(), src.height(), kind)) {
        std::printf("  [%s] 跳过：%s（%s）\n", label, "这个后端在这台机器上不可用",
                    gpu.lastError().c_str());
        return true;
    }
    Surface out;
    if (!gpu.render(src, out)) {
        std::printf("  [%s] FAIL：%s\n", label, gpu.lastError().c_str());
        return false;
    }
    const int diff = MaxChannelDiff(src, out);
    std::printf("  [%s] %s 往返：最大通道差 %d\n", label, gpu.backendName(), diff);
    return diff <= 1;
}

}  // namespace

int main() {
    const Surface src = MakeSource();

    // ① CPU 后端（确定性，不需要驱动）
    {
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
        const int maxDiff = MaxChannelDiff(src, out);
        CHECK(maxDiff <= 1, "gpu: CPU 后端往返与软件路径逐像素一致（容差 ≤1）");
        std::printf("  （最大通道差 %d）\n", maxDiff);
    }

    // ② 每个真实后端各跑一遍（不可用就跳过）
    {
        const struct {
            const char* label;
            BgpuBackendKind kind;
        } kBackends[] = {
            {"vulkan", BGPU_BACKEND_VULKAN},
            {"d3d11", BGPU_BACKEND_D3D11},
            {"gl", BGPU_BACKEND_GL},
        };
        for (const auto& b : kBackends) {
            if (!bingpu_backend_available(b.kind)) {
                std::printf("  [%s] 跳过：bingpu_backend_available == 0\n", b.label);
                continue;
            }
            CHECK(RunBackend(b.label, b.kind, src),
                  "gpu: 真实后端往返与软件路径逐像素一致（容差 ≤1）");
        }
    }
    return Bin2dTestSummary("test_gpu_bridge");
}
