// test_gpu_bridge —— 只在 binGPU 已构建（有 libbingpu.a）时编译/运行。
//
// 目的（四件，缺一不可）：
//   ① 软件画出来的这一帧，过一遍 bingpu 的**CPU 后端**再读回来，必须还是它；
//   ② 同一帧过**每个可用的真实后端**（Vulkan / D3D11 / GL）读回来，同样必须是它 ——
//      这条是 bin2d 对 binGPU 的 P1（命令 → 像素）的直接消费方证据：上层一行不改，
//      底下换后端，像素不许动。
//   ③ 合成器（GpuCompositor）：**五种混合模式** × 多图层的合成结果，必须与 CPU 的
//      DrawSurface 逐像素一致；合批（几层几次绘制）与纹理池（同尺寸复用）是**数字**
//      验收，不是"看起来对"。
//   ④ 两个回读路径都要考：GpuBridge 走帧目标、GpuCompositor 也走帧目标，
//      而 bingpu 的 texture_read 是另一条路（binGPU 自己测）。
//
// 容差：预乘 Surface → 直通上传（后端立刻再预乘）→ 回读反预乘 → 再预乘，
// 两次 8 位反预乘各可能差 1，所以合成结果允许 ≤1 的通道差；混合模式的映射关系
// 与合批次数是**精确**断言。
// 后端不可用时**跳过**（打印原因），不是失败 —— CI 机上没有显卡不能红。
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bin2d/canvas.h"
#include "bin2d/gpu.h"
#include "test_util.h"

using namespace bin2d;

namespace {

// ------------------------------------------------------------
// 素材
// ------------------------------------------------------------
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

// 图层素材：不透明块 + 半透明块 + 一整列**全透明孔**（零 alpha 是 Replace/Keep 的坑）
Surface MakeTile(int w, int h) {
    Surface s(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if ((x % 7) == 3) continue;   // 透明列：把 alpha == 0 的路径也考到
            const bool solid = (x < w / 2);
            const uint8_t a = solid ? 255 : static_cast<uint8_t>(90 + (y * 5) % 120);
            s.set(x, y, Color::straight(static_cast<uint8_t>((x * 255) / (w > 1 ? w - 1 : 1)),
                                        static_cast<uint8_t>((y * 255) / (h > 1 ? h - 1 : 1)),
                                        static_cast<uint8_t>(40 + (x + y) % 160), a));
        }
    }
    return s;
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

// ------------------------------------------------------------
// CPU 权威：按顺序合成一张图层清单（DrawSurface 是唯一落像素的出口，
// Canvas::popLayer 走的也是它 —— 所以这里的"权威"与宿主实际路径同源）
// ------------------------------------------------------------
struct CpuLayer {
    const Surface* src = nullptr;
    Rect dst;
    Rect uv = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    Color tint = Color::white();
    BlendMode blend = BlendMode::SrcOver;
    bool nearest = false;
};

Surface CpuCompose(int w, int h, const std::vector<CpuLayer>& layers) {
    Surface out(w, h);
    for (const CpuLayer& L : layers) {
        if (!L.src || L.src->empty() || L.dst.empty()) continue;
        RasterTarget rt;
        rt.surface = &out;
        rt.clip = out.bounds();
        rt.blend = L.blend;
        DrawSurface(rt, *L.src, L.dst, L.uv, L.tint, L.nearest, Transform::identity());
    }
    return out;
}

// 把 CPU 的图层清单翻译成合成器的图层清单（纹理由调用方提前上传好）
GpuCompositor::Layer GpuLayerOf(const CpuLayer& L, BgpuTexture tex) {
    GpuCompositor::Layer out;
    out.texture = tex;
    out.dst = L.dst;
    out.uv = L.uv;
    out.tint = L.tint;
    out.blend = L.blend;
    out.filter = L.nearest ? ImageFilter::Nearest : ImageFilter::Linear;
    return out;
}

// ------------------------------------------------------------
// ③ 五种混合模式 × 多图层：GPU 合成 == CPU 合成
// ------------------------------------------------------------
bool RunBlendParity(GpuCompositor& comp, const char* label) {
    const int W = 96, H = 64;
    const Surface tile = MakeTile(24, 20);
    const Surface wide = MakeTile(40, 30);

    std::vector<CpuLayer> cpu;
    cpu.push_back({&tile, Rect(4, 4, 24, 20), Rect(0, 0, 1, 1), Color::white(),
                   BlendMode::Replace, false});
    cpu.push_back({&wide, Rect(20, 10, 40, 30), Rect(0, 0, 1, 1), Color::white() * 0.75f,
                   BlendMode::SrcOver, false});
    cpu.push_back({&wide, Rect(40, 30, 40, 30), Rect(0, 0, 1, 1), Color::white(),
                   BlendMode::Erase, false});
    cpu.push_back({&tile, Rect(58, 8, 24, 20), Rect(0, 0, 1, 1), Color::white(),
                   BlendMode::Keep, false});
    cpu.push_back({&tile, Rect(10, 40, 24, 20), Rect(0, 0, 1, 1), Color::white(),
                   BlendMode::Additive, true});
    const Surface expected = CpuCompose(W, H, cpu);

    // 纹理：两张素材各上传一次（池里各拿一块）
    BgpuTexture texTile = comp.acquire(tile.width(), tile.height());
    BgpuTexture texWide = comp.acquire(wide.width(), wide.height());
    if (!texTile || !texWide || !comp.upload(texTile, tile) || !comp.upload(texWide, wide)) {
        std::printf("  [%s] FAIL：纹理上传失败（%s）\n", label, comp.lastError().c_str());
        return false;
    }
    std::vector<GpuCompositor::Layer> gpu;
    gpu.push_back(GpuLayerOf(cpu[0], texTile));
    gpu.push_back(GpuLayerOf(cpu[1], texWide));
    gpu.push_back(GpuLayerOf(cpu[2], texWide));
    gpu.push_back(GpuLayerOf(cpu[3], texTile));
    gpu.push_back(GpuLayerOf(cpu[4], texTile));

    Surface got;
    const bool ok = comp.compose(gpu.data(), gpu.size(), got);
    int diff = ok ? MaxChannelDiff(expected, got) : 9999;
    std::printf("  [%s] 五模式多图层合成：最大通道差 %d（%d 层 / %d 次绘制 / %d 次绑纹理）\n",
                label, diff, comp.stats().layers, comp.stats().draws,
                comp.stats().textureBinds);

    comp.recycle(texTile);
    comp.recycle(texWide);
    return ok && diff <= 1;
}

// 合批与纹理池是数字验收：8 个同纹理同模式的层必须合成 1 次绘制
bool RunBatchAndPool(GpuCompositor& comp, const char* label) {
    const Surface tile = MakeTile(16, 16);
    Surface out;
    bool pass = true;

    BgpuTexture tex = comp.acquire(16, 16);
    pass = pass && tex != 0 && comp.upload(tex, tile);
    std::vector<GpuCompositor::Layer> layers;
    for (int i = 0; i < 8; ++i) {
        GpuCompositor::Layer L;
        L.texture = tex;
        L.dst = Rect(static_cast<float>((i % 4) * 20), static_cast<float>((i / 4) * 24), 16, 16);
        layers.push_back(L);
    }
    pass = pass && comp.compose(layers.data(), layers.size(), out);
    const bool batched = comp.stats().draws == 1 && comp.stats().quads == 8 &&
                         comp.stats().textureBinds == 1 && comp.stats().layers == 8;
    std::printf("  [%s] 合批：8 层 → %d 次绘制，%d 个四边形，%d 次绑纹理\n", label,
                comp.stats().draws, comp.stats().quads, comp.stats().textureBinds);
    pass = pass && batched;

    // 交替混合模式：状态每层都变 → 绘制次数 = 层数
    const BlendMode modes[] = {BlendMode::SrcOver, BlendMode::Replace, BlendMode::Erase,
                               BlendMode::Keep, BlendMode::Additive};
    layers.clear();
    for (int i = 0; i < 5; ++i) {
        GpuCompositor::Layer L;
        L.texture = tex;
        L.blend = modes[i];
        L.dst = Rect(static_cast<float>(i * 18), 0, 16, 16);
        layers.push_back(L);
    }
    pass = pass && comp.compose(layers.data(), layers.size(), out);
    const bool unbatched = comp.stats().draws == 5;
    std::printf("  [%s] 换模式：5 层 → %d 次绘制\n", label, comp.stats().draws);
    pass = pass && unbatched;

    // 池：归还后同尺寸必须复用同一块句柄
    comp.recycle(tex);
    const bool idled = comp.idleOfSize(16, 16) == 1;
    BgpuTexture again = comp.acquire(16, 16);
    const bool reused = (again == tex);
    comp.recycle(again);
    comp.purgePool();
    const bool purged = comp.idleOfSize(16, 16) == 0 && comp.pooledCount() == 0;
    std::printf("  [%s] 纹理池：复用=%d 清空=%d\n", label, reused ? 1 : 0, purged ? 1 : 0);
    pass = pass && idled && reused && purged;
    return pass;
}

// 脏矩形上传：整张传一次，再只传中心块，合成结果必须只有中心块变了
bool RunBoxUpload(GpuCompositor& comp, const char* label) {
    Surface tile(32, 32);
    tile.clear(Color::argb(0xFFFF0000));
    BgpuTexture tex = comp.acquire(32, 32);
    bool pass = tex != 0 && comp.upload(tex, tile);

    Surface patch(32, 32);
    patch.clear(Color::argb(0xFF00FF00));
    // 只传 (8,8)-(24,24) 这一块：patch 的其它像素内容不该出现在结果里
    pass = pass && comp.upload(tex, patch, Rect(8, 8, 16, 16));

    GpuCompositor::Layer L;
    L.texture = tex;
    L.dst = Rect(8, 8, 32, 32);
    Surface out;
    pass = pass && comp.compose(&L, 1, out);
    if (pass) {
        const Color inside = out.get(8 + 12, 8 + 12);   // 脏区内
        const Color outside = out.get(8 + 2, 8 + 2);    // 脏区外
        const Color edge = out.get(8 + 31, 8 + 31);     // 右边界外一格
        const bool ok = inside.g == 255 && inside.r == 0 && outside.r == 255 && outside.g == 0 &&
                        edge.r == 255;
        std::printf("  [%s] 脏矩形上传：内 (%d,%d,%d) 外 (%d,%d,%d)\n", label, inside.r,
                    inside.g, inside.b, outside.r, outside.g, outside.b);
        pass = pass && ok;
    }
    comp.recycle(tex);
    return pass;
}

// 空层/全透明层/空纹理：不提交任何绘制，输出全透明
bool RunSkips(GpuCompositor& comp, const char* label) {
    BgpuTexture tex = comp.acquire(8, 8);
    const bool up = tex != 0 && comp.upload(tex, Surface(8, 8));

    GpuCompositor::Layer layers[3];
    layers[0].texture = 0;                                    // 没纹理
    layers[0].dst = Rect(0, 0, 10, 10);
    layers[1].texture = tex;                                  // 空矩形
    layers[1].dst = Rect(0, 0, 0, 0);
    layers[2].texture = tex;                                  // 全透明 tint
    layers[2].dst = Rect(0, 0, 8, 8);
    layers[2].tint = Color::trans();

    Surface out;
    const bool ok = comp.compose(layers, 3, out);
    bool empty = ok;
    for (int y = 0; y < out.height() && empty; ++y)
        for (int x = 0; x < out.width(); ++x)
            if (out.get(x, y).a != 0) { empty = false; break; }
    std::printf("  [%s] 跳过规则：提交 %d 层 → %d 次绘制，输出%s\n", label,
                comp.stats().layers, comp.stats().draws, empty ? "全透明" : "非空");
    comp.recycle(tex);
    return up && ok && empty && comp.stats().layers == 0 && comp.stats().draws == 0;
}

// 离屏渲染目标：合成到纹理（不回读），再把这块纹理当图层用 —— 图层套图层不落 CPU 内存。
// 这条路径的关键性质：**渲染目标里存的是预乘像素**，当纹理采样时不会再"直通→预乘"
// 转换一次，所以它比"上传纹理"那条路少一次舍入。
bool RunRenderTarget(GpuCompositor& comp, const char* label) {
    const int W = 96, H = 64;
    const Surface tile = MakeTile(24, 20);
    const Rect dst(10, 10, 24, 20);
    bool pass = true;

    BgpuTexture src = comp.acquire(24, 20);
    pass = pass && src != 0 && comp.upload(src, tile);
    BgpuTexture rt = comp.acquireTarget(W, H);
    pass = pass && rt != 0;

    // 第一趟：tile 按 Replace 画进 RT
    GpuCompositor::Layer l0;
    l0.texture = src;
    l0.dst = dst;
    l0.blend = BlendMode::Replace;
    pass = pass && comp.composeTo(&l0, 1, rt);

    // 第二趟：RT 当纹理、整幅 50% 压到帧目标上
    GpuCompositor::Layer l1;
    l1.texture = rt;
    l1.dst = Rect(0, 0, static_cast<float>(W), static_cast<float>(H));
    l1.tint = Color::white() * 0.5f;
    Surface got;
    pass = pass && comp.compose(&l1, 1, got);

    // CPU 权威：同一套两步
    Surface mid(W, H);
    {
        RasterTarget r;
        r.surface = &mid;
        r.clip = mid.bounds();
        r.blend = BlendMode::Replace;
        DrawSurface(r, tile, dst, Rect(0, 0, 1, 1), Color::white(), false, Transform::identity());
    }
    Surface expect(W, H);
    {
        RasterTarget r;
        r.surface = &expect;
        r.clip = expect.bounds();
        r.blend = BlendMode::SrcOver;
        DrawSurface(r, mid, Rect(0, 0, static_cast<float>(W), static_cast<float>(H)),
                    Rect(0, 0, 1, 1), Color::white() * 0.5f, false, Transform::identity());
    }
    const int diff = pass ? MaxChannelDiff(expect, got) : 9999;
    std::printf("  [%s] 渲染目标套层：最大通道差 %d\n", label, diff);
    pass = pass && diff <= 1;

    // readTarget 读回同一块 RT，必须与"把 mid 当纹理再合成"的结果一致
    Surface readBack;
    pass = pass && comp.readTarget(rt, readBack);
    if (pass) {
        const int rd = MaxChannelDiff(mid, readBack);
        std::printf("  [%s] readTarget 与 CPU 中间结果：最大通道差 %d\n", label, rd);
        pass = pass && rd <= 1;
    }
    comp.recycle(src);
    comp.recycle(rt);
    return pass;
}

// 一个后端跑一遍往返；返回 true = 通过（或跳过）
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

// 一个后端跑一遍合成器全套
bool RunCompositor(const char* label, BgpuBackendKind kind) {
    GpuCompositor comp;
    if (!comp.create(96, 64, kind)) {
        std::printf("  [%s] 跳过合成器：%s\n", label, comp.lastError().c_str());
        return true;   // 不可用就跳过（同往返测试的口径）
    }
    bool pass = true;
    pass = RunBlendParity(comp, label) && pass;
    pass = RunBatchAndPool(comp, label) && pass;
    pass = RunBoxUpload(comp, label) && pass;
    pass = RunSkips(comp, label) && pass;
    pass = RunRenderTarget(comp, label) && pass;

    // composeOne 与老桥（GpuBridge::render）必须是同一个结果
    const Surface src = MakeSource();
    Surface viaComposer;
    if (comp.composeOne(src, viaComposer)) {
        GpuBridge bridge;
        if (bridge.create(src.width(), src.height(), kind)) {
            Surface viaBridge;
            if (bridge.render(src, viaBridge)) {
                const int d = MaxChannelDiff(viaBridge, viaComposer);
                std::printf("  [%s] composeOne vs 老桥：最大通道差 %d\n", label, d);
                pass = (d <= 1) && pass;
            }
        }
    }
    return pass;
}

}  // namespace

int main() {
    const Surface src = MakeSource();

    // 混合模式映射：bin2d 与 bingpu 的枚举值顺序不同（Erase/Keep 互换），
    // 映射错了会把"擦除"和"保留"对调 —— 这条不需要设备，先钉死。
    {
        CHECK(ToBgpuBlend(BlendMode::SrcOver) == BGPU_BLEND_PREMULTIPLIED,
              "gpu: SrcOver → PREMULTIPLIED");
        CHECK(ToBgpuBlend(BlendMode::Replace) == BGPU_BLEND_REPLACE, "gpu: Replace → REPLACE");
        CHECK(ToBgpuBlend(BlendMode::Erase) == BGPU_BLEND_ERASE, "gpu: Erase → ERASE");
        CHECK(ToBgpuBlend(BlendMode::Keep) == BGPU_BLEND_KEEP, "gpu: Keep → KEEP");
        CHECK(ToBgpuBlend(BlendMode::Additive) == BGPU_BLEND_ADDITIVE, "gpu: Additive → ADDITIVE");
    }

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

    // ①b CPU 后端的合成器
    CHECK(RunCompositor("cpu", BGPU_BACKEND_CPU),
          "gpu: CPU 后端五模式合成 / 合批 / 纹理池 / 脏区上传全部通过");

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
            CHECK(RunCompositor(b.label, b.kind),
                  "gpu: 真实后端五模式合成 / 合批 / 纹理池 / 脏区上传全部通过");
        }
    }
    return Bin2dTestSummary("test_gpu_bridge");
}
