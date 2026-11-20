// test_content_layer —— 设备侧离屏层（bin2d 0.22.5）的契约测试。
//
// 这一组要钉住四件事（都是"错了会在 binUI 那种页面上表现为一整块消失/错位"的坑）：
//   ① 内容层往返：进层 → 画 → 出层拿到句柄 → 贴回去，结果与"同一份内容直接画"
//      逐像素一致（≤2：多一次预乘采样与 MSAA resolve）；
//   ② 外阴影的**口径**：fillContentLayer 是"内容的 alpha 当遮罩填纯色"，不是
//      "内容自己的颜色 × 阴影色" —— 用绿色内容 + 红阴影来验：如果实现成了 tint
//      相乘，贴出来会是黄/粉而不是纯红；
//   ③ 模糊真的发生了（硬阴影的边缘外面有 alpha、原边缘是中间值），并且 blur=0
//      时边缘是硬的（两种模式不能互相串）。层框必须按上层那样**外扩**（PaintLayerPad），
//      否则模糊没有可以蔓延的空间 —— 这里显式外扩，顺便钉住"内容按屏幕坐标落进层纹理"。
//   ④ 句柄的生命周期：pop 与 push 必须配对、release 之后句柄失效、帧末漏掉
//      release 不许把池子卡死（下一帧还能正常分层）。
//
// 后端不可用时**跳过**（打印原因），不是失败 —— CI 机上没有显卡不能红。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bin2d/canvas.h"
#include "bin2d/gpu_exec.h"
#include "test_util.h"

using namespace bin2d;

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 96;

int MaxChannelDiff(const Surface& a, const Surface& b) {
    int maxDiff = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const Color ca = a.get(x, y);
            const Color cb = b.get(x, y);
            maxDiff = std::max(maxDiff, std::abs(int(ca.r) - int(cb.r)));
            maxDiff = std::max(maxDiff, std::abs(int(ca.g) - int(cb.g)));
            maxDiff = std::max(maxDiff, std::abs(int(ca.b) - int(cb.b)));
            maxDiff = std::max(maxDiff, std::abs(int(ca.a) - int(cb.a)));
        }
    }
    return maxDiff;
}

// 一帧的脚手架：HEADLESS 执行体 + 挂在它上面的 Canvas（与 binUI 的用法同构）。
// 每一帧都用一个独立实例：本测试比的是"两帧同一条路的结果"，不共享状态。
struct Frame {
    GpuExec gpu;
    Surface dst;
    Canvas canvas;

    Frame() : dst(kWidth, kHeight), canvas(dst) {}

    bool begin() {
        if (!gpu.create(nullptr, kWidth, kHeight)) return false;
        canvas.attachGpu(&gpu);
        return gpu.beginFrame(0x00000000u, true);
    }
    bool end() {
        if (!gpu.present()) return false;
        return gpu.readback(dst);
    }
};

// ① 内容层往返 == 同一份内容直接画。
//
// 比的是"设备侧离屏层"这一层语义本身对不对（坐标、裁剪、预乘、合成），所以两帧都用
// 同一个 GPU 执行体画：**不**和 bin2d 的 CPU 光栅比 —— 圆角的 AA 是两套定义
// （SDF vs 8 子采样扫描线），拿它当判据等于钉死另一件事（见 test_gpu_bridge 的 ④）。
bool RunRoundTrip() {
    const Rect layerBox(12, 8, 72, 48);   // 外扩过的层框（上层按 PaintLayerPad 算）
    const Rect bodyBox(20, 16, 60, 36);

    auto render = [&](bool throughLayer) {
        Surface out;
        Frame f;
        if (!f.begin()) {
            std::printf("  [content] 跳过：%s\n", f.gpu.lastError().c_str());
            return out;
        }
        Canvas& c = f.canvas;
        uint64_t handle = 0;
        Rect screen;
        bool ok = true;
        if (throughLayer) {
            ok = c.pushContentLayer(layerBox);
            if (!ok) std::printf("  [content] pushContentLayer 失败：%s\n",
                                 f.gpu.lastError().c_str());
        }
        c.fillRounded(bodyBox, 10.0f, Color::argb(0xFF3E8BFF));
        c.fillRect(Rect(40, 30, 18, 14), Color::argb(0x80FFFF00));
        if (throughLayer) {
            ok = ok && c.popContentLayer(handle, screen);
            ok = ok && c.drawContentLayer(handle, screen, Rect(0.0f, 0.0f, 1.0f, 1.0f),
                                          Color::white(), 0.0f, true);
            c.releaseContentLayer(handle);
        }
        if (!ok || !f.end()) {
            std::printf("  [content] 渲染失败：%s\n", f.gpu.lastError().c_str());
            return Surface();
        }
        out = std::move(f.dst);
        return out;
    };

    const Surface viaLayer = render(true);
    const Surface direct = render(false);
    if (viaLayer.empty() || direct.empty()) {
        CHECK(false, "content: 两帧都渲染成功");
        return true;
    }
    const int diff = MaxChannelDiff(direct, viaLayer);
    std::printf("  [content] 内容层往返 vs 直接画：最大通道差 %d\n", diff);
    CHECK(diff <= 2, "content: 内容层贴回 == 同一内容直接画（≤2）");
    return true;
}

// ② 外阴影的口径：绿色内容 + 红色阴影 + 偏移 (7,5) + 不模糊。
//    "内容的 alpha 当遮罩填纯色" ⇒ 偏移出去的那一条必须是**纯红**；
//    错成 tint 相乘就会是绿×红 ≈ 黄。
bool RunShadowTint() {
    const Rect layerBox(8, 8, 64, 48);
    const Rect bodyBox(16, 16, 40, 24);
    Frame f;
    if (!f.begin()) {
        std::printf("  [shadow] 跳过：%s\n", f.gpu.lastError().c_str());
        return true;
    }
    Canvas& c = f.canvas;
    const Color body = Color::argb(0xFF22C55E);   // 绿：错成 tint 会变黄
    const Color shadowColor = Color::argb(0xFFFF0000);
    uint64_t handle = 0;
    Rect screen;
    bool ok = c.pushContentLayer(layerBox);
    c.fillRect(bodyBox, body);                    // 不透明、整数对齐 ⇒ 遮罩恒 255
    ok = ok && c.popContentLayer(handle, screen);
    // 阴影：偏移 (7,5)，不模糊。先贴阴影再贴本体（与 binUI 的顺序一致）
    ok = ok && c.fillContentLayer(handle,
                                  Rect(screen.x + 7.0f, screen.y + 5.0f, screen.w, screen.h),
                                  shadowColor, 0.0f);
    ok = ok && c.drawContentLayer(handle, screen, Rect(0.0f, 0.0f, 1.0f, 1.0f),
                                  Color::white(), 0.0f, true);
    c.releaseContentLayer(handle);
    const bool ended = f.end();
    if (!ok || !ended) {
        std::printf("  [shadow] FAIL：%s\n", f.gpu.lastError().c_str());
        CHECK(false, "shadow: 设备侧外阴影链路跑通");
        return true;
    }

    // 只有阴影、没有本体的一格：本体 y 到 40，阴影下沿到 45 → (30,43)
    const Color s = f.dst.get(30, 43);
    const Color b = f.dst.get(30, 24);
    const Color e = f.dst.get(4, 4);   // 空白处
    std::printf("  [shadow] 阴影处 (%d,%d,%d,%d)，本体处 (%d,%d,%d,%d)，空白处 a=%d\n", s.r,
                s.g, s.b, s.a, b.r, b.g, b.b, b.a, e.a);
    // 正确：阴影 = 预乘红 (255,0,0,255)；错成 tint：绿(34,197,94)×红 ≈ (34,0,0)
    CHECK(std::abs(int(s.r) - 255) <= 2 && s.g <= 2 && s.b <= 2,
          "shadow: 外部阴影是纯阴影色（只借 alpha，不借内容颜色）");
    CHECK(std::abs(int(b.g) - 0xC5) <= 2 && std::abs(int(b.r) - 0x22) <= 2,
          "shadow: 本体仍然是自己的颜色");
    CHECK(e.a == 0, "shadow: 没画到的地方一个像素都不动");
    return true;
}

// ③ 模糊：层框外扩之后，阴影边缘与本体轮廓外面都必须是斜坡；blur=0 时是硬边。
bool RunBlurSpread() {
    const Rect bodyBox(40, 32, 40, 24);
    const Rect layerBox(28, 20, 64, 48);   // 四周各外扩 12：≥ 3σ（σ=4）

    auto render = [&](float shadowBlur, float bodyBlur) {
        Surface out;
        Frame f;
        if (!f.begin()) {
            std::printf("  [blur] 跳过：设备起不来\n");
            return out;
        }
        Canvas& c = f.canvas;
        uint64_t handle = 0;
        Rect screen;
        bool ok = c.pushContentLayer(layerBox);
        c.fillRect(bodyBox, Color::argb(0xFFFFFFFF));
        ok = ok && c.popContentLayer(handle, screen);
        if (shadowBlur >= 0.0f)   // 负值 = 不要阴影（只考本体模糊）
            ok = ok && c.fillContentLayer(handle, screen, Color::argb(0xFF000000),
                                          shadowBlur);
        ok = ok && c.drawContentLayer(handle, screen, Rect(0.0f, 0.0f, 1.0f, 1.0f),
                                      Color::white(), bodyBlur, false);
        c.releaseContentLayer(handle);
        if (!ok || !f.end()) {
            std::printf("  [blur] 渲染失败：%s\n", f.gpu.lastError().c_str());
            return Surface();
        }
        out = std::move(f.dst);
        return out;
    };

    const Surface hard = render(-1.0f, 0.0f);   // 硬边
    const Surface soft = render(6.0f, 4.0f);    // 阴影 σ=6 + 本体 σ=4
    if (hard.empty() || soft.empty()) {
        CHECK(false, "blur: 两帧都渲染成功");
        return true;
    }
    // 本体左边沿在 x=40：硬边那里是 0→255 的台阶；模糊之后 37/38 有蔓延、40 是中间值
    const int hardOutside = hard.get(37, 44).a;
    const int softOutside = soft.get(37, 44).a;
    const int softEdge = soft.get(40, 44).a;
    const int softInside = soft.get(48, 44).a;
    std::printf("  [blur] x=37（外）硬 %d / 软 %d；软 x=40 %d，x=48 %d\n", hardOutside,
                softOutside, softEdge, softInside);
    CHECK(hardOutside == 0, "blur: 不模糊时边缘外一格是空的（硬边）");
    CHECK(softOutside > 0, "blur: 模糊之后边缘外面有蔓延（斜坡）");
    CHECK(softEdge > 0 && softEdge < 255, "blur: 模糊后原边缘是中间值（不是硬台阶）");
    CHECK(softInside >= 250, "blur: 模糊只影响边缘，中心的覆盖率仍然是满的");

    // 阴影（σ=6、无偏移）：本体左沿 x=40，层框左沿 x=28 → 6 像素外面必须还有阴影。
    // 这一条同时钉住"层框要比内容外扩"这件事（不外扩就没有可以蔓延的空间）。
    const int softShadow = soft.get(34, 44).a;
    const int hardShadow = hard.get(34, 44).a;
    std::printf("  [blur] 本体左侧 6px 处：有阴影（σ=6）%d / 没阴影 %d\n", softShadow,
                hardShadow);
    CHECK(softShadow > 0, "blur: 阴影 σ=6 蔓延到本体之外（层框外扩是必要的）");
    CHECK(hardShadow == 0, "blur: 没有阴影时本体外面是空的");
    return true;
}

// ④ 生命周期：未 push 就 pop 必须失败；release 过的句柄不能再画；
//    漏 release 的句柄不能把下一帧的池子卡死。
bool RunLifecycle() {
    Frame f;
    if (!f.begin()) {
        std::printf("  [life] 跳过：设备起不来\n");
        return true;
    }
    Canvas& c = f.canvas;
    uint64_t bogus = 12345;
    Rect screen;
    CHECK(!c.popContentLayer(bogus, screen), "life: 没 push 就 pop 返回 false");
    CHECK(bogus == 0, "life: 失败时输出句柄被清零");
    CHECK(!c.drawContentLayer(12345, Rect(0, 0, 4, 4), Rect(0, 0, 1, 1), Color::white(), 0.0f,
                              false),
          "life: 不存在的句柄画不出来");
    c.releaseContentLayer(12345);   // 不该崩、不该改任何状态

    // 漏 release：pop 之后不还，直接从下一帧开始（模拟调用方写漏）
    bool ok = c.pushContentLayer(Rect(8, 8, 20, 20));
    uint64_t leaked = 0;
    ok = ok && c.popContentLayer(leaked, screen);
    CHECK(ok && leaked != 0, "life: 漏 release 的那一层也建出来了");
    const bool ended = f.end();
    CHECK(ended, "life: 第一帧收尾");

    // 下一帧：池子必须还能用（resetFrameState 把漏掉的句柄作废、纹理归还）
    Frame g;
    if (!g.begin()) {
        std::printf("  [life] 跳过第二帧：设备起不来\n");
        return true;
    }
    bool allOk = true;
    for (int i = 0; i < 6; ++i) {   // 一帧里连着 6 次进出（池子复用而不是每次新建）
        uint64_t h = 0;
        Rect r;
        const bool pushed = g.canvas.pushContentLayer(Rect(4, 4, 40, 24));
        g.canvas.fillRect(Rect(4, 4, 40, 24), Color::argb(0xFF3366CC));
        const bool popped = pushed && g.canvas.popContentLayer(h, r);
        const bool drawn = popped && g.canvas.drawContentLayer(h, r, Rect(0, 0, 1, 1),
                                                               Color::white(), 0.0f, true);
        g.canvas.releaseContentLayer(h);
        allOk = allOk && drawn;
    }
    CHECK(allOk, "life: 第二帧里连续 6 次进出内容层都成功");
    CHECK(g.end(), "life: 第二帧收尾");
    return true;
}

}  // namespace

int main() {
    // 设备不可用：跳过整组（同 test_gpu_bridge 的口径）
    {
        GpuExec probe;
        if (!probe.create(nullptr, 8, 8)) {
            std::printf("  [content] 跳过整组：没有可用后端（%s）\n",
                        probe.lastError().c_str());
            return Bin2dTestSummary("test_content_layer");
        }
        probe.destroy();
    }

    RunRoundTrip();
    RunShadowTint();
    RunBlurSpread();
    RunLifecycle();
    return Bin2dTestSummary("test_content_layer");
}
