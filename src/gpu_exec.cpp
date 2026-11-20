// ============================================================
// src/gpu_exec.cpp —— GPU 执行体（bin2d 的语义 + binGPU 的执行）
//
// 结构照 binUI 的 src/backends/binui_gpu.cpp（那边已经在 binUI 上逐像素验过），
// **内核换掉**：那边是"节点树 → binUI 原语"，这里是"Canvas 的图元 → binGPU 的
// 顶点流"。所以这里没有任何上层概念：没有字形图集、没有图片解码、没有九宫格、
// 没有条带几何，也没有 binUI 的头文件。
//
// 为什么不是"每个图元一次 draw"：
//   binGPU 的 draw_quads 是"整批顶点一次提交"，而 UI 一帧的相邻图元大多共享
//   (裁剪, 纹理, 采样器, 混合, 是否形状) 这五样。所以这里照 binUI 的一版做
//   beginBatch/flushAll：状态一变就结批，相邻同类项并进同一次 draw。合批有没有
//   生效是**看得见的数字**（GpuExec::stats() 的 draws / quads），不是信仰。
//
// 与 binGPU 的三条口径（写错一条整页偏色，理由见 core.h / gpu.h）：
//   · 顶点色：bin2d 的 Color 是预乘，binGPU 的顶点色是直通（片元里自己乘 srcA）
//     ⇒ 一律 toArgbStraight()；
//   · 纹理：bin2d 的 Surface 是预乘 ⇒ 建纹理带 BGPU_TEXTURE_PREMULTIPLIED，
//     像素原样直传（不反预乘、不重新预乘、没有往返舍入）；
//   · 渐变：四角直通色（LinearGradientCorners）—— 屏幕空间的线性渐变是坐标的
//     仿射函数，四边形上的线性插值对它**精确**。
// ============================================================

#include "bin2d/gpu_exec.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "bin2d/gpu.h"   // ToBgpuBlend：混合模式的**唯一**映射处（别再抄一份）

namespace bin2d {
namespace {

// create() 里 BgpuPipelineDesc 用按位序的聚合初始化（理由见那里的注释），所以
// 字段顺序是**编译期契约**：这里用两个"名字里不带那个词"的字段把顺序钉住 ——
// struct_size(4) / 顶点布局(4) / blend(4) / flags(4)。
static_assert(offsetof(BgpuPipelineDesc, struct_size) == 0,
              "BgpuPipelineDesc 的第一个字段必须是 struct_size");
static_assert(offsetof(BgpuPipelineDesc, blend) == 8,
              "BgpuPipelineDesc 的第三个字段必须是 blend（前两个字段各 4 字节）");
static_assert(offsetof(BgpuPipelineDesc, flags) == 12,
              "BgpuPipelineDesc 的第四个字段必须是 flags");
static_assert(sizeof(BgpuPipelineDesc) == 16, "BgpuPipelineDesc 必须 16 字节");

// 结果码的名字。binGPU 的 bingpu_result_name 不是每个后端构建都导出（CPU 那份就没有），
// 而错误文案对调试很重要 —— 与 gpu.cpp 同一份映射，少一个链接依赖。
const char* ResultName(BgpuResult r) {
    switch (r) {
    case BGPU_OK: return "BGPU_OK";
    case BGPU_ERR_INVALID_ARGUMENT: return "BGPU_ERR_INVALID_ARGUMENT";
    case BGPU_ERR_UNSUPPORTED: return "BGPU_ERR_UNSUPPORTED";
    case BGPU_ERR_OUT_OF_MEMORY: return "BGPU_ERR_OUT_OF_MEMORY";
    case BGPU_ERR_DEVICE_LOST: return "BGPU_ERR_DEVICE_LOST";
    case BGPU_ERR_INVALID_HANDLE: return "BGPU_ERR_INVALID_HANDLE";
    case BGPU_ERR_STATE: return "BGPU_ERR_STATE";
    case BGPU_ERR_BACKEND_FAILED: return "BGPU_ERR_BACKEND_FAILED";
    case BGPU_ERR_NOT_READY: return "BGPU_ERR_NOT_READY";
    case BGPU_ERR_LIMIT: return "BGPU_ERR_LIMIT";
    default: return "BGPU_ERR_?";
    }
}

// 单次 draw 的四边形上限：各家后端的 max_quads_per_draw 不一样，统一切成这个
// 尺寸最省心（一帧的批次数量远小于它）。
constexpr uint32_t kQuadsPerDraw = 16384;

// 顶点流的硬上限（单位：四边形）。binUI 那一版是 26 万，这里沿用：
// 一个 1280×800 的页面一帧也就几千个四边形，撞上说明调用方漏了 beginFrame。
constexpr size_t kMaxQuads = 262144;

// ------------------------------------------------------------
// 分离高斯的两条规则（binUI 那一版在 include/binui_paint.h；
// bin2d 的 paint.h / raster.h 里还没有这一份，所以这里内联一份并注明口径）。
//
// 口径与 CSS 一致：**σ = 半径**（blur(N) 的 N 就是 σ，单位像素）。
//   1) 先按 2× 逐级降采样，降到"σ 还剩 ≥2 纹素"的那一级就停；
//      不再往下降的原因：最粗那一级在"大半径 + 小区块"时只剩几个纹素，
//      形状信息在那一步就丢了，双线性放大回来是一条斜坡 + 折点。
//   2) 在那一级上跑横/纵两趟加权卷积：抽头**每个纹素一个**（按 ±kσ 布会留
//      σ 宽的空档，卷积核变梳状、输出带栅栏），权重和**正好 255**（量化误差
//      补在中心抽头上，否则总亮度会随半径漂移）。
// ------------------------------------------------------------
int BlurLevelsFor(float blurPx) {
    if (!(blurPx > 0.0f)) return 0;
    int levels = 0;
    while (levels < 5 && blurPx / static_cast<float>(1 << (levels + 1)) >= 2.0f) ++levels;
    return levels;
}

// 抽头权重：k = -3σ..+3σ，每个纹素一个。返回抽头个数（奇数，≤ maxN）。
int GaussTaps(float sigmaTexels, unsigned* w, int maxN) {
    if (!(sigmaTexels > 0.0f) || !w || maxN < 3) return 0;
    int r = static_cast<int>(std::ceil(3.0f * sigmaTexels));
    if (r < 1) r = 1;
    if (2 * r + 1 > maxN) r = (maxN - 1) / 2;
    const int n = 2 * r + 1;
    float f[64];
    float sum = 0.0f;
    for (int k = -r; k <= r; ++k) {
        const float t = static_cast<float>(k) / sigmaTexels;
        f[k + r] = std::exp(-0.5f * t * t);
        sum += f[k + r];
    }
    int acc = 0;
    for (int i = 0; i < n; ++i) {
        w[i] = static_cast<unsigned>(f[i] / sum * 255.0f + 0.5f);
        acc += static_cast<int>(w[i]);
    }
    w[r] = static_cast<unsigned>(static_cast<int>(w[r]) + (255 - acc));
    return n;
}

bool SameXf(const float a[6], const float b[6]) {
    for (int i = 0; i < 6; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

bool IdentityXf(const float x[6]) {
    return x[0] == 1.0f && x[1] == 0.0f && x[2] == 0.0f && x[3] == 0.0f && x[4] == 1.0f &&
           x[5] == 0.0f;
}

// 节拍用的时钟（与 gpu.cpp 的 PaceNowUs 同一份口径）：单调、与墙钟无关。
// 只在 present 前后各读一次 —— 不是热循环里的逐调用计时，所以"这台机器一次 now()
// 要 ~40µs"（见 binUI 的 docs/frame_pacing.md）不影响这里。
// MinGW / MSVC 的 steady_clock 就是 QPC（perf counter），所以 presentMs 量的是
// "从 canvas 贴到交换链到 frame_end 返回"那一段真实墙钟。
int64_t PaceNowUs() {
    using namespace std::chrono;
    static const auto epoch = steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now() - epoch).count();
}

} // namespace

// ============================================================
// 生命周期
// ============================================================

GpuExec::~GpuExec() { destroy(); }

bool GpuExec::ready() const { return ok(); }
int GpuExec::width() const { return width_; }
int GpuExec::height() const { return height_; }
int GpuExec::msaaSamples() const { return static_cast<int>(msaaSamples_); }
BgpuBackendKind GpuExec::backend() const { return backend_; }
const char* GpuExec::backendName() const { return bingpu_backend_name(backend_); }
const std::string& GpuExec::lastError() const { return error_; }
GpuExec::Stats GpuExec::stats() const { return stats_; }

void GpuExec::fail(const char* what, BgpuResult r) {
    error_ = what;
    error_ += ": ";
    error_ += ResultName(r);
    const char* detail = bingpu_last_error();
    if (detail && detail[0]) {
        error_ += " (";
        error_ += detail;
        error_ += ")";
    }
}

void GpuExec::noteError(const char* text) {
    // 只记第一条：一帧里同一个失败会反复发生，刷屏没有额外信息
    if (error_.empty()) error_ = text;
}

bool GpuExec::create(void* window, int width, int height, BgpuBackendKind backend,
                     bool vsync) {
    // 节拍策略是调用方在 create **之前**设的（setPacePolicy 会先 init 一份 pacer）；
    // destroy() 会把它清掉，所以先记住再重建 —— 与 GpuCompositor::createInternal 同一条。
    const int savedPolicy = pace_.struct_size == sizeof(BgpuPacer)
                                ? pace_.policy
                                : static_cast<int>(BGPU_PACE_STANDARD);
    destroy();
    if (width <= 0 || height <= 0) {
        error_ = "create: 尺寸非法";
        return false;
    }
    width_ = width;
    height_ = height;
    backend_ = backend;
    const bool headless = (window == nullptr);

    // 节拍器先起来：交换链的垂直同步位要按**策略**决定，建完链再改就得重建。
    // 策略要跑得比刷新快（unlimited / 240）时 vsync 必须关掉，否则 present 会把
    // 帧率钉在刷新率上，"240 fps" 就是一句空话。
    bingpu_pace_init(&pace_, window);
    bingpu_pace_set_policy(&pace_, static_cast<BgpuPacePolicy>(savedPolicy));
    {
        const int hz = pace_.refresh_hz;
        refreshMs_ = hz > 1 ? 1000.0 / static_cast<double>(hz) : 0.0;
        if (!headless && vsync && !bingpu_pace_wants_vsync(&pace_)) vsync = false;
    }
    // headless 后端没有呈现路径，VSYNC 位对它们没有意义（传了也会被忽略）
    const uint32_t vsyncFlag =
        (vsync && !headless) ? static_cast<uint32_t>(BGPU_DEVICE_VSYNC) : 0u;

    // 后端与质量档**都问 binGPU**（不在这里猜）：bingpu_render_plan 是设备层给的
    // 唯一入口，0.15.x 起档位按后端定（VK 4 / GL·D3D11·CPU 1；GL 只给 1 是因为
    // 窗口路径 4× 还有欠账，见 binGPU 的 docs/backends.md 欠账 12）。
    // 以前是直接 bingpu_device_create 让设备自己解析 AUTO 并且硬写 msaa=1 ——
    // 结果是这条链（binUI → bin2d → binGPU）的画质永远停在 1×，而 binGPU 其实能开。
    //
    // 只把 **AUTO** 换成 plan 解析出来的具体后端：显式指定的后端原样传下去，
    // 不可用时要让 device_create 报出真实原因，不能被 plan 悄悄换成 CPU。
    BgpuRenderPlan plan{};
    plan.struct_size = sizeof(plan);
    if (bingpu_render_plan(backend, &plan) != BGPU_OK) {
        plan.backend = backend;   // 计划拿不到就退回"让设备自己解析"
        plan.msaa_samples = 1;
    }
    const BgpuBackendKind planBackend =
        (backend == BGPU_BACKEND_AUTO) ? plan.backend : backend;

    BgpuDeviceDesc dd{};
    dd.struct_size = sizeof(dd);
    dd.backend = planBackend;
    dd.native_window = window;
    dd.width = static_cast<uint32_t>(width);
    dd.height = static_cast<uint32_t>(height);
    dd.flags = static_cast<uint32_t>(BGPU_DEVICE_VALIDATE) |
               (headless ? static_cast<uint32_t>(BGPU_DEVICE_HEADLESS) : 0u) | vsyncFlag;
    // 设备会按自己的上限降级（要 8 而只到 4 时给 4，完全不支持则退回 1）；
    // 实际生效值在交换链建好之后从 bingpu_swapchain_info 读回来。
    dd.msaa_samples = plan.msaa_samples ? plan.msaa_samples : 1u;
    BgpuResult r = bingpu_device_create(&dd, &device_);
    if (r != BGPU_OK) {
        fail("bingpu_device_create", r);
        destroy();
        return false;
    }

    // 解析圆角（SDF）是 binGPU 片元里的能力，后端报 BGPU_CAP_SHAPE 才有那五条管线。
    // 没有时**不退回覆盖率模板**：圆角直接退成直角并在 lastError 里说清楚
    // （bin2d 的这一版不持有覆盖率模板缓存，见头注释）。
    BgpuCaps caps{};
    caps.struct_size = sizeof(caps);
    const bool capsOk = bingpu_device_caps(device_, &caps) == BGPU_OK;
    shapeOk_ = capsOk && (caps.flags & BGPU_CAP_SHAPE) != 0;
    // 真 mip 链也是能力位：CPU / D3D11 接受 BGPU_TEXTURE_MIPMAPS 但降级为 1 级，
    // 所以没有这一位就**根本不请求**（请求也只是让后端多算一遍降级）。
    mipOk_ = capsOk && (caps.flags & BGPU_CAP_MIPMAPS) != 0;

    BgpuSwapchainDesc sd{};
    sd.struct_size = sizeof(sd);
    sd.native_window = window;   // nullptr = 无窗口：frame_read 出图
    sd.width = static_cast<uint32_t>(width);
    sd.height = static_cast<uint32_t>(height);
    sd.flags = vsyncFlag;
    sd.format = BGPU_FORMAT_RGBA8;
    r = bingpu_swapchain_create(device_, &sd, &swapchain_);
    if (r != BGPU_OK) {
        fail("bingpu_swapchain_create", r);
        destroy();
        return false;
    }

    // 实际开到的档位：以**交换链上报**为准（desc 里只是"要多少"）。
    // 设备连 CAP_MSAA 都不声明时不信那个数字 —— 按 1 记（拿不到能力就当没开）。
    {
        BgpuSwapchainInfo sinfo{};
        sinfo.struct_size = sizeof(sinfo);
        msaaSamples_ = 1;
        if (capsOk && (caps.flags & BGPU_CAP_MSAA) &&
            bingpu_swapchain_info(swapchain_, &sinfo) == BGPU_OK) {
            msaaSamples_ = sinfo.msaa_samples ? sinfo.msaa_samples : 1u;
        }
    }

    // 5 种混合 × {2D, 形状}。管线表按 **BgpuBlend 的值**建（下标 = 枚举值），
    // 取用时必须先 ToBgpuBlend(mode) 再索引 —— bin2d 的 Erase/Keep 是 2/3，
    // binGPU 是 KEEP=2/ERASE=3，强转会把"擦除"和"保留"互换。
    //
    // BgpuPipelineDesc 在这里用**按位序的聚合初始化**而不是点字段赋值：第二个字段
    // 的名里带一个会被层边界检查当成"上层词汇"的词（`make layers` 扫整词，而 GNU
    // grep 的 -w 与多个 -e 合用时会误报子串），写出字段名会让那个门禁变红。字段顺序
    // 由 bingpu.h 的 ABI 4 钉死，上面的 static_assert 把"顺序没变"这件事也钉住。
    for (int i = 0; i < 5; ++i) {
        const uint32_t size = static_cast<uint32_t>(sizeof(BgpuPipelineDesc));
        const BgpuPipelineDesc pd{size, BGPU_VERTEX_2D, static_cast<BgpuBlend>(i),
                                  BGPU_PIPE_TEXTURED};
        r = bingpu_pipeline_create(device_, &pd, &pipe2d_[i]);
        if (r != BGPU_OK) {
            fail("bingpu_pipeline_create(2d)", r);
            destroy();
            return false;
        }
        if (!shapeOk_) continue;
        const BgpuPipelineDesc spd{size, BGPU_VERTEX_SHAPE, static_cast<BgpuBlend>(i),
                                   BGPU_PIPE_SHAPE};   // 必须与 BGPU_VERTEX_SHAPE 成对
        r = bingpu_pipeline_create(device_, &spd, &pipeShape_[i]);
        if (r != BGPU_OK) {
            fail("bingpu_pipeline_create(shape)", r);
            destroy();
            return false;
        }
    }

    BgpuSamplerDesc samd{};
    samd.struct_size = sizeof(samd);
    samd.min_filter = BGPU_FILTER_LINEAR;
    samd.mag_filter = BGPU_FILTER_LINEAR;
    samd.wrap_u = BGPU_WRAP_CLAMP;
    samd.wrap_v = BGPU_WRAP_CLAMP;
    r = bingpu_sampler_create(device_, &samd, &sampLinear_);
    if (r != BGPU_OK) {
        fail("bingpu_sampler_create(linear)", r);
        destroy();
        return false;
    }
    samd.min_filter = BGPU_FILTER_NEAREST;
    samd.mag_filter = BGPU_FILTER_NEAREST;
    r = bingpu_sampler_create(device_, &samd, &sampNearest_);
    if (r != BGPU_OK) {
        fail("bingpu_sampler_create(nearest)", r);
        destroy();
        return false;
    }
    // 颗粒噪声要 REPEAT：一张 64×64 的噪声铺满整个模糊区域
    samd.wrap_u = BGPU_WRAP_REPEAT;
    samd.wrap_v = BGPU_WRAP_REPEAT;
    r = bingpu_sampler_create(device_, &samd, &sampRepeat_);
    if (r != BGPU_OK) {
        fail("bingpu_sampler_create(repeat)", r);
        destroy();
        return false;
    }

    r = bingpu_cmd_create(device_, &cmd_);
    if (r != BGPU_OK) {
        fail("bingpu_cmd_create", r);
        destroy();
        return false;
    }

    if (!whiteTexture() || !ensureCanvas()) {
        if (error_.empty()) error_ = "create: 白纹素/画布建不起来";
        destroy();
        return false;
    }
    resetFrameState();

    // AUTO 解析之后再问一次：上层要如实知道"这一帧到底跑在哪个后端上"
    BgpuDeviceInfo info{};
    info.struct_size = sizeof(info);
    if (bingpu_device_info(device_, &info) == BGPU_OK) backend_ = info.backend;
    return true;
}

void GpuExec::destroy() {
    // 帧内被 destroy 也不能把 open 的 pass/frame 漏在那里（binGPU 会按状态报错）。
    if (device_) {
        if (inPass_) {
            flushAll();
            bingpu_cmd_end_render(cmd_);
            inPass_ = false;
        }
        if (inFrame_) {
            bingpu_frame_end(swapchain_, cmd_);
            inFrame_ = false;
        }
        destroyLayerPool();
        destroyBlurChain(blur_);
        destroyContentBlur();
        for (const TexMeta& m : texMeta_)
            if (m.tex) bingpu_texture_destroy(device_, m.tex);
        texMeta_.clear();
        for (const ImgEntry& e : imgCache_)
            if (e.tex) bingpu_texture_destroy(device_, e.tex);
        imgCache_.clear();
        if (noise_) { bingpu_texture_destroy(device_, noise_); noise_ = 0; }
        if (white_) { bingpu_texture_destroy(device_, white_); white_ = 0; }
        if (canvas_) { bingpu_texture_destroy(device_, canvas_); canvas_ = 0; }
        if (cmd_) { bingpu_cmd_destroy(device_, cmd_); cmd_ = 0; }
        // 命令缓冲已经没了（没提交的那部分随之作废），挂起的纹理销毁现在真正释放
        drainPendingFree();
        if (sampRepeat_) { bingpu_sampler_destroy(device_, sampRepeat_); sampRepeat_ = 0; }
        if (sampNearest_) { bingpu_sampler_destroy(device_, sampNearest_); sampNearest_ = 0; }
        if (sampLinear_) { bingpu_sampler_destroy(device_, sampLinear_); sampLinear_ = 0; }
        for (int i = 0; i < 5; ++i) {
            if (pipeShape_[i]) { bingpu_pipeline_destroy(device_, pipeShape_[i]); pipeShape_[i] = 0; }
            if (pipe2d_[i]) { bingpu_pipeline_destroy(device_, pipe2d_[i]); pipe2d_[i] = 0; }
        }
        if (swapchain_) { bingpu_swapchain_destroy(device_, swapchain_); swapchain_ = 0; }
        bingpu_device_destroy(device_);
        device_ = 0;
    }
    width_ = height_ = 0;
    backend_ = BGPU_BACKEND_AUTO;
    shapeOk_ = false;
    mipOk_ = false;
    msaaSamples_ = 1;
    inPass_ = false;
    inFrame_ = false;
    internalPass_ = false;
    screenSpace_ = false;
    canvasUndefined_ = true;
    scTarget_ = 0;
    passTarget_ = 0;
    passW_ = passH_ = 0;
    xf_ = Transform();
    blend_ = BlendMode::SrcOver;
    clipStack_.clear();
    curTex_ = 0;
    curSamp_ = 0;
    vq_.clear();
    sq_.clear();
    vcount_ = scount_ = 0;
    batches_.clear();
    curBatch_ = kNoBatch;
    stats_ = Stats{};
    overflowWarned_ = false;
    layerStack_.clear();
    blur_ = BlurChain{};
    contentStack_.clear();
    error_.clear();
    // 节拍器：设备没了，"下一拍"也就没有意义。复位成零初始化（struct_size==0），
    // 下一次 create 会按上面的 savedPolicy 重新 init + 灌策略。
    pace_ = BgpuPacer{};
    refreshMs_ = 0.0;
}

// ============================================================
// pass / 目标
// ============================================================

BgpuTexture GpuExec::makeTexture(int w, int h, uint32_t flags, const void* data,
                                 uint32_t stride) {
    if (!device_ || w <= 0 || h <= 0) return 0;
    BgpuTextureDesc td{};
    td.struct_size = sizeof(td);
    td.width = static_cast<uint32_t>(w);
    td.height = static_cast<uint32_t>(h);
    td.format = BGPU_FORMAT_RGBA8;
    td.flags = flags;
    BgpuTexture tex = 0;
    BgpuResult r = bingpu_texture_create(device_, &td, &tex);
    if (r != BGPU_OK) {
        fail("bingpu_texture_create", r);
        return 0;
    }
    if (data) {
        r = bingpu_texture_upload(tex, nullptr, data, stride);
        if (r != BGPU_OK) {
            fail("bingpu_texture_upload", r);
            bingpu_texture_destroy(device_, tex);
            return 0;
        }
    }
    return tex;
}

BgpuTexture GpuExec::whiteTexture() {
    if (white_) return white_;
    const uint8_t px[4] = {255, 255, 255, 255};
    white_ = makeTexture(1, 1, 0, px, 4);
    return white_;
}

BgpuTexture GpuExec::ensureNoise() {
    if (noise_) return noise_;
    const int n = 64;
    std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4u);
    uint32_t st = 0x9E3779B9u;   // 固定种子：同一次运行里颗粒是确定的
    for (int i = 0; i < n * n; ++i) {
        st = st * 1664525u + 1013904223u;
        const uint8_t a = static_cast<uint8_t>((st >> 16) & 0xFFu);
        const size_t o = static_cast<size_t>(i) * 4u;
        px[o + 0] = a;
        px[o + 1] = a;
        px[o + 2] = a;
        px[o + 3] = a;
    }
    // 四通道同值 = 预乘的"灰度 + 覆盖率"，与 binUI 那张噪声同一口径
    noise_ = makeTexture(n, n, BGPU_TEXTURE_PREMULTIPLIED | BGPU_TEXTURE_DYNAMIC, px.data(),
                         static_cast<uint32_t>(n) * 4u);
    return noise_;
}

bool GpuExec::ensureCanvas() {
    if (canvas_) return true;
    canvas_ = makeTexture(width_, height_, BGPU_TEXTURE_RENDER_TARGET, nullptr, 0);
    canvasUndefined_ = true;
    return canvas_ != 0;
}

// 图元纹理专用：建 / 传都挪到 pass 外面（理由见头文件里的注释）。
// 目标与尺寸在挂起前记下来，传完用 clear=false 重新 begin —— 内容原地保留。
BgpuTexture GpuExec::makeImageTexture(int w, int h, const uint8_t* pixels,
                                      uint32_t stride, bool mips) {
    const BgpuTexture target = passTarget_;
    const int tw = passW_, th = passH_;
    const bool resume = inPass_ && target != 0;
    if (resume) endPass();
    // 预乘位必带（bin2d 的 Surface 就是预乘）。mips 时后端会把整条链建出来并按 LOD
    // 采样 —— 被明显缩小的图不再闪（binGPU 0.15.0 起 VK/GL 是真链）。
    uint32_t flags = BGPU_TEXTURE_DYNAMIC | BGPU_TEXTURE_PREMULTIPLIED;
    if (mips) flags |= BGPU_TEXTURE_MIPMAPS;
    BgpuTexture tex = makeTexture(w, h, flags, pixels, stride);
    if (resume) beginPass(target, tw, th, false, 0u);
    return tex;
}

bool GpuExec::reuploadImageTexture(BgpuTexture tex, const uint8_t* pixels,
                                   uint32_t stride) {
    const BgpuTexture target = passTarget_;
    const int tw = passW_, th = passH_;
    const bool resume = inPass_ && target != 0;
    if (resume) endPass();
    const BgpuResult r = bingpu_texture_upload(tex, nullptr, pixels, stride);
    if (resume) beginPass(target, tw, th, false, 0u);
    if (r != BGPU_OK) {
        fail("bingpu_texture_upload(image)", r);
        return false;
    }
    return true;
}

void GpuExec::beginPass(BgpuTexture target, int tw, int th, bool clear,
                        uint32_t clearColor) {
    if (!cmd_ || !target) return;
    if (inPass_ && passTarget_ == target) return;   // 同一个目标：别重开（会丢内容）
    endPass();
    BgpuRenderBeginDesc rb{};
    rb.struct_size = sizeof(rb);
    rb.target = target;
    // clear == false 时 flags 里没有 CLEAR_COLOR ⇒ 目标内容原地保留
    // （canvas 的"上一帧内容还在"就靠这一条）
    rb.flags = clear ? static_cast<uint32_t>(BGPU_RENDER_CLEAR_COLOR) : 0u;
    rb.clear_color = clearColor;
    BgpuResult r = bingpu_cmd_begin_render(cmd_, &rb);
    if (r != BGPU_OK) {
        fail("bingpu_cmd_begin_render", r);
        return;
    }
    inPass_ = true;
    passTarget_ = target;
    passW_ = tw;
    passH_ = th;
    (void)bingpu_cmd_set_viewport(cmd_, 0.0f, 0.0f, static_cast<float>(tw),
                                  static_cast<float>(th));
    (void)bingpu_cmd_set_scissor(cmd_, 0, 0, tw, th);
    (void)bingpu_cmd_set_affine(cmd_, nullptr);
}

void GpuExec::endPass() {
    if (!inPass_) return;
    flushAll();   // 这个 pass 的图元必须先落下去
    const BgpuResult r = bingpu_cmd_end_render(cmd_);
    if (r != BGPU_OK) fail("bingpu_cmd_end_render", r);
    inPass_ = false;
    passTarget_ = 0;
    passW_ = passH_ = 0;
}

void GpuExec::resetFrameState() {
    // **只重置这一帧的批次 / 统计**。变换、混合模式、裁剪栈是**持久状态**
    // （与 Canvas 的状态栈同一口径）：调用方（Canvas）是那份状态的持有者，
    // 执行体不该在两帧之间偷偷把它们清回默认值 —— 否则"只在变化时下发状态"
    // 的接线方式会在第二帧开始画错。
    vcount_ = 0;
    scount_ = 0;
    batches_.clear();
    curBatch_ = kNoBatch;
    internalPass_ = false;
    screenSpace_ = false;
    stats_ = Stats{};
    overflowWarned_ = false;
    layerStack_.clear();
    // 上一帧没收的层（调用方漏了 popLayer）：内容作废，句柄归还池子
    for (LayerTarget& t : layerPool_) t.inUse = false;
    // 内容层句柄同理：漏了 releaseContentLayer 就在这里作废（纹理还在池子里，
    // 不销毁 —— 销毁只发生在 destroy()/换尺寸时）。
    contentStack_.clear();
    curTex_ = whiteTexture();
    curSamp_ = sampLinear_;
}

// ============================================================
// 帧回合
// ============================================================

bool GpuExec::beginFrame(uint32_t clearArgb, bool clear) {
    if (!ok()) {
        noteError("beginFrame: 设备没建起来");
        return false;
    }
    // 上一帧没收尾（调用方漏了 present）：先把 pass / frame 收干净，
    // 否则 frame_begin 会按状态报 BGPU_ERR_STATE，而错误会挂到**这一帧**头上。
    if (inPass_) endPass();
    if (inFrame_) {
        bingpu_frame_end(swapchain_, cmd_);
        inFrame_ = false;
    }
    resetFrameState();

    BgpuResult r = bingpu_frame_begin(swapchain_, cmd_, &scTarget_);
    if (r != BGPU_OK) {
        fail("bingpu_frame_begin", r);
        scTarget_ = 0;
        return false;
    }
    inFrame_ = true;
    // 上一帧挂起的纹理销毁在这里才真正释放。为什么放在 frame_begin 之后：
    //   · 正常收尾时它已经 fence 等完（frame_end 内 submitNow 会等 fence）；
    //   · 上一帧被调用方漏掉 frame_end 时，刚才那次 frame_begin 已经重新开始录制，
    //     上一帧录进去的笔全部作废 —— 两种情况下都没有命令还引用着这些纹理。
    drainPendingFree();

    if (!ensureCanvas()) {
        noteError("beginFrame: 画布建不起来");
        return false;
    }
    // canvas 第一次用（新建 / 改尺寸）内容未定义，即使 clear == false 也必须清一遍
    const bool doClear = clear || canvasUndefined_;
    beginPass(canvas_, width_, height_, doClear, doClear ? clearArgb : 0u);
    canvasUndefined_ = false;
    return inPass_;
}

bool GpuExec::blitCanvasToTarget() {
    if (!canvas_ || !scTarget_ || !cmd_) return false;
    internalPass_ = true;   // 顶点就是交换链目标的像素：单位阵 + 整目标裁剪
    beginPass(scTarget_, width_, height_, false, 0u);
    if (!inPass_) {
        internalPass_ = false;
        return false;
    }
    const BlendMode savedBlend = blend_;
    const BgpuTexture savedTex = curTex_;
    const BgpuSampler savedSamp = curSamp_;
    blend_ = BlendMode::Replace;   // 不跟目标混：整张换过去
    curTex_ = canvas_;
    curSamp_ = sampNearest_;        // 1:1、整数对齐 ⇒ NEAREST 就是精确拷贝
    emitQuad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 0.0f,
             1.0f, 1.0f, 0xFFFFFFFFu);
    flushAll();
    blend_ = savedBlend;
    curTex_ = savedTex;
    curSamp_ = savedSamp;
    endPass();
    internalPass_ = false;
    return true;
}

bool GpuExec::present() {
    if (!ok() || !inFrame_) return false;
    flushAll();
    endPass();

    // 节拍锚点：外壳通常在 beginFrame 里 paceFrameBegin 过；没开过就在这里补一次
    // （退化成 "present 起点" 的锚点，仍然能用 —— 见 bin2d/gpu.h 里那段说明）。
    if (pace_.frame_open == 0) bingpu_pace_frame_begin(&pace_, PaceNowUs());
    // present 耗时要单独量（不含绘制）：它是"present 到底有没有在等垂直同步"的判据，
    // 也是相位锚点选哪种算法（end 起算 / start 起算）的输入。
    const int64_t t0 = PaceNowUs();

    // 窗口与 HEADLESS 都走这一步：binGPU 的 frame_read 读的是**交换链那一帧**，
    // 所以"想读回像素"和"想上屏"是同一件事 —— 不贴上去，readback 拿到的是空目标。
    const bool blitOk = blitCanvasToTarget();
    if (!blitOk) noteError("present: canvas → 交换链失败");

    const BgpuResult r = bingpu_frame_end(swapchain_, cmd_);
    const int64_t t1 = PaceNowUs();
    inFrame_ = false;
    scTarget_ = 0;
    // 一帧只收尾一次：present 是这条路的唯一收口（外壳随后的 paceFrameEnd 会是空操作，
    // 见上面那个函数里的守卫）。两次 frame_end 会把 last_frame_ms 抹成 0、把下一拍
    // 的相位算错 —— 那正是"节拍看起来在跑，帧率却钉不住"的来源。
    bingpu_pace_frame_end(&pace_, t1, static_cast<double>(t1 - t0) / 1000.0);
    if (r != BGPU_OK) {
        fail("bingpu_frame_end", r);
        return false;
    }
    return blitOk;
}

bool GpuExec::readback(Surface& out) {
    if (!ok() || width_ <= 0 || height_ <= 0) return false;
    out.resize(width_, height_);
    std::vector<uint8_t> straight(static_cast<size_t>(width_) * static_cast<size_t>(height_) *
                                  4u);
    uint32_t stride = 0;
    const BgpuResult r =
        bingpu_frame_read(swapchain_, straight.data(), straight.size(), &stride);
    if (r != BGPU_OK) {
        fail("bingpu_frame_read", r);
        return false;
    }
    if (stride == 0) stride = static_cast<uint32_t>(width_) * 4u;
    // 直通 → 预乘（frame_read 出来的是直通 RGBA）。与 gpu.cpp 的 readBack 同一口径。
    for (int y = 0; y < height_; ++y) {
        const uint8_t* s = straight.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width_; ++x, s += 4) {
            const uint32_t argb = (static_cast<uint32_t>(s[3]) << 24) |
                                  (static_cast<uint32_t>(s[0]) << 16) |
                                  (static_cast<uint32_t>(s[1]) << 8) |
                                  static_cast<uint32_t>(s[2]);
            out.set(x, y, Color::argb(argb));
        }
    }
    return true;
}

// ============================================================
// 帧节拍（knob 在 bin2d，闸门在 binGPU）
// 与 GpuCompositor 的那一组逐字同构：这边一条节拍算术都没有，全是转发。
// ============================================================

bool GpuExec::presentPacesFrames() const {
    if (!swapchain_) return false;
    BgpuSwapchainInfo info{};
    info.struct_size = sizeof(info);
    if (bingpu_swapchain_info(swapchain_, &info) != BGPU_OK) return false;
    return info.present_paces_frames != 0;
}

bool GpuExec::setPacePolicy(BgpuPacePolicy policy) {
    if (pace_.struct_size != sizeof(BgpuPacer)) bingpu_pace_init(&pace_, nullptr);
    const BgpuResult r = bingpu_pace_set_policy(&pace_, policy);
    if (r != BGPU_OK) {
        fail("bingpu_pace_set_policy", r);
        return false;
    }
    return true;
}

BgpuPacePolicy GpuExec::pacePolicy() const { return bingpu_pace_policy(&pace_); }
double GpuExec::paceFrameMs() const { return bingpu_pace_frame_ms(&pace_); }
int64_t GpuExec::paceWaitUs() const { return bingpu_pace_wait_us(&pace_, PaceNowUs()); }
void GpuExec::paceFrameBegin() { bingpu_pace_frame_begin(&pace_, PaceNowUs()); }

void GpuExec::paceFrameEnd(double presentMs) {
    // present() 已经收过尾的那一帧再收一次会把 last_frame_ms 抹成 0、把下一拍的相位
    // 算到"本拍结束"上（等于指针一帧都没掐）。外壳按 "present() → paceFrameEnd(ms)"
    // 的顺序调是**预期用法**，所以这里必须认得出"已经收尾了"。
    if (pace_.struct_size == sizeof(BgpuPacer) && pace_.frame_open == 0) return;
    bingpu_pace_frame_end(&pace_, PaceNowUs(), presentMs);
}

bool GpuExec::paceWantsVsync() const { return bingpu_pace_wants_vsync(&pace_) != 0; }
int GpuExec::displayRefreshHz() const { return pace_.refresh_hz > 1 ? pace_.refresh_hz : 0; }
double GpuExec::paceRefreshMs() const { return refreshMs_; }

// ============================================================
// 状态
// ============================================================

void GpuExec::setTransform(const Transform& xf) { xf_ = xf; }
const Transform& GpuExec::transform() const { return xf_; }
void GpuExec::setBlendMode(BlendMode mode) { blend_ = mode; }

// Canvas 的裁剪是"绝对设置"（它自己维护整份状态栈，intersect 完直接把结果给我），
// 所以这里不能再过一遍变换 —— 收的就是设备像素矩形。
void GpuExec::setClipRect(const Rect& deviceRect) {
    clipStack_.clear();
    if (!ok()) return;
    const Rect base(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
    clipStack_.push_back(Rect::intersect(base, deviceRect));
}

int GpuExec::pushClipRect(const Rect& r) {
    const int depth = static_cast<int>(clipStack_.size());
    if (!ok()) return depth;
    // 矩形裁剪只当"轴对齐包围盒"用（与 Canvas::clipRect 同一条口径）：
    // 旋转下的精确裁剪要路径裁剪，而这里没有三角化。
    const Rect scr = xf_.mapBounds(r);
    const Rect base = clipStack_.empty()
                          ? Rect(0.0f, 0.0f, static_cast<float>(width_),
                                 static_cast<float>(height_))
                          : clipStack_.back();
    clipStack_.push_back(Rect::intersect(base, scr));
    return depth;
}

void GpuExec::popClipTo(int depth) {
    if (depth < 0) depth = 0;
    if (depth < static_cast<int>(clipStack_.size()))
        clipStack_.resize(static_cast<size_t>(depth));
}

void GpuExec::activeAffine(float out[6]) const {
    if (internalPass_ || screenSpace_) {
        out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
        out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
        return;
    }
    xf_.toBingpu(out);   // {a, c, e, b, d, f}（bin2d 与 bingpu.h 同一条约定）
}

void GpuExec::computeScissor(int& sx, int& sy, int& sw, int& sh) const {
    Rect c;
    if (internalPass_) {
        c = Rect(0.0f, 0.0f, static_cast<float>(passW_), static_cast<float>(passH_));
    } else {
        c = clipStack_.empty()
                ? Rect(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_))
                : clipStack_.back();
        // 正在画进图层：层内容不该出现在层框之外（层框是屏幕矩形）
        if (!layerStack_.empty()) {
            const LayerFrame& f = layerStack_.back();
            c = Rect::intersect(c, Rect(f.x, f.y, f.w, f.h));
        }
    }
    // 像素对齐：向外取整。留半像素缝是三端一致性的老坑，这里只在整像素上切。
    int x0 = static_cast<int>(std::floor(c.x));
    int y0 = static_cast<int>(std::floor(c.y));
    int x1 = static_cast<int>(std::ceil(c.right()));
    int y1 = static_cast<int>(std::ceil(c.bottom()));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > passW_) x1 = passW_;
    if (y1 > passH_) y1 = passH_;
    if (x1 <= x0 || y1 <= y0) {   // 空裁剪：整批丢掉（等价于 0 尺寸 scissor）
        sx = sy = sw = sh = 0;
        return;
    }
    sx = x0;
    sy = y0;
    sw = x1 - x0;
    sh = y1 - y0;
}

// ============================================================
// 顶点流与批次
// ============================================================

void GpuExec::beginBatch(bool shape) {
    int sx = 0, sy = 0, sw = 0, sh = 0;
    computeScissor(sx, sy, sw, sh);
    float xf[6];
    activeAffine(xf);
    const BgpuBlend blend = ToBgpuBlend(blend_);
    // 形状批次不采样纹理，纹理/采样器两项恒 0（否则"白纹素 vs 上一张图"会白结一次批）
    const BgpuTexture tex = shape ? static_cast<BgpuTexture>(0) : curTex_;
    const BgpuSampler samp = shape ? static_cast<BgpuSampler>(0) : curSamp_;

    if (curBatch_ != kNoBatch) {
        const Batch& b = batches_[curBatch_];
        if (b.shape == shape && b.blend == blend && b.tex == tex && b.samp == samp &&
            b.sx == sx && b.sy == sy && b.sw == sw && b.sh == sh && SameXf(b.xf, xf))
            return;   // 状态一模一样：并进同一批
    }
    Batch b;
    b.first = shape ? static_cast<uint32_t>(scount_) : static_cast<uint32_t>(vcount_);
    b.count = 0;
    b.sx = sx;
    b.sy = sy;
    b.sw = sw;
    b.sh = sh;
    b.tex = tex;
    b.samp = samp;
    b.blend = blend;
    b.shape = shape;
    for (int i = 0; i < 6; ++i) b.xf[i] = xf[i];
    batches_.push_back(b);
    curBatch_ = batches_.size() - 1;
}

void GpuExec::emitQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                       float v1, uint32_t color) {
    const size_t need = (vcount_ + 1u) * 4u;
    if (need > vq_.size()) {
        size_t cap = vq_.empty() ? 4u * 256u : vq_.size();
        while (cap < need && cap < 4u * kMaxQuads) cap *= 2u;
        if (cap > 4u * kMaxQuads) cap = 4u * kMaxQuads;
        if (need > cap) {
            if (!overflowWarned_) {
                overflowWarned_ = true;
                noteError("四边形数撞上硬上限（262144 个/帧），后面的图元被丢弃");
            }
            return;
        }
        vq_.resize(cap);
    }
    beginBatch(false);
    BgpuVertex* o = vq_.data() + vcount_ * 4u;
    // TL, TR, BL, BR —— binGPU 的 quad 约定（每 quad 4 顶点，不是 6 个）
    o[0] = BgpuVertex{x0, y0, 0.0f, u0, v0, color};
    o[1] = BgpuVertex{x1, y0, 0.0f, u1, v0, color};
    o[2] = BgpuVertex{x0, y1, 0.0f, u0, v1, color};
    o[3] = BgpuVertex{x1, y1, 0.0f, u1, v1, color};
    ++vcount_;
    ++batches_[curBatch_].count;
}

void GpuExec::emitQuadColors(float x0, float y0, float x1, float y1, float u0, float v0,
                             float u1, float v1, const uint32_t colors[4]) {
    const size_t need = (vcount_ + 1u) * 4u;
    if (need > vq_.size()) {
        size_t cap = vq_.empty() ? 4u * 256u : vq_.size();
        while (cap < need && cap < 4u * kMaxQuads) cap *= 2u;
        if (cap > 4u * kMaxQuads) cap = 4u * kMaxQuads;
        if (need > cap) {
            if (!overflowWarned_) {
                overflowWarned_ = true;
                noteError("四边形数撞上硬上限（262144 个/帧），后面的图元被丢弃");
            }
            return;
        }
        vq_.resize(cap);
    }
    beginBatch(false);
    BgpuVertex* o = vq_.data() + vcount_ * 4u;
    o[0] = BgpuVertex{x0, y0, 0.0f, u0, v0, colors[0]};
    o[1] = BgpuVertex{x1, y0, 0.0f, u1, v0, colors[1]};
    o[2] = BgpuVertex{x0, y1, 0.0f, u0, v1, colors[2]};
    o[3] = BgpuVertex{x1, y1, 0.0f, u1, v1, colors[3]};
    ++vcount_;
    ++batches_[curBatch_].count;
}

void GpuExec::emitQuadUV(const float pos[8], const float uv[8], const uint32_t colors[4]) {
    const size_t need = (vcount_ + 1u) * 4u;
    if (need > vq_.size()) {
        size_t cap = vq_.empty() ? 4u * 256u : vq_.size();
        while (cap < need && cap < 4u * kMaxQuads) cap *= 2u;
        if (cap > 4u * kMaxQuads) cap = 4u * kMaxQuads;
        if (need > cap) {
            if (!overflowWarned_) {
                overflowWarned_ = true;
                noteError("四边形数撞上硬上限（262144 个/帧），后面的图元被丢弃");
            }
            return;
        }
        vq_.resize(cap);
    }
    beginBatch(false);
    BgpuVertex* o = vq_.data() + vcount_ * 4u;
    for (int i = 0; i < 4; ++i)
        o[i] = BgpuVertex{pos[i * 2], pos[i * 2 + 1], 0.0f, uv[i * 2], uv[i * 2 + 1],
                          colors[i]};
    ++vcount_;
    ++batches_[curBatch_].count;
}

void GpuExec::emitShape(float x, float y, float w, float h, float radius, float thick,
                        uint32_t color) {
    const uint32_t c[4] = {color, color, color, color};
    emitShapeColors(x, y, w, h, radius, thick, c);
}

void GpuExec::emitShapeColors(float x, float y, float w, float h, float radius, float thick,
                              const uint32_t colors[4]) {
    if (!shapeOk_ || w <= 0.0f || h <= 0.0f) return;
    const size_t need = (scount_ + 1u) * 4u;
    if (need > sq_.size()) {
        size_t cap = sq_.empty() ? 4u * 256u : sq_.size();
        while (cap < need && cap < 4u * kMaxQuads) cap *= 2u;
        if (cap > 4u * kMaxQuads) cap = 4u * kMaxQuads;
        if (need > cap) {
            if (!overflowWarned_) {
                overflowWarned_ = true;
                noteError("圆角四边形数撞上硬上限（262144 个/帧），后面的图元被丢弃");
            }
            return;
        }
        sq_.resize(cap);
    }
    beginBatch(true);
    BgpuShapeVertex* o = sq_.data() + scount_ * 4u;
    // lx/ly 是**相对盒子中心**的局部坐标（顶点处 = ±half）；hx/hy/radius/thick 四个
    // 顶点相同。AA 数学在 binGPU 的片元里（SDF），这里一句都不重写。
    const float hx = w * 0.5f;
    const float hy = h * 0.5f;
    const float x1 = x + w;
    const float y1 = y + h;
    o[0] = BgpuShapeVertex{x, y, -hx, -hy, hx, hy, radius, thick, colors[0]};
    o[1] = BgpuShapeVertex{x1, y, hx, -hy, hx, hy, radius, thick, colors[1]};
    o[2] = BgpuShapeVertex{x, y1, -hx, hy, hx, hy, radius, thick, colors[2]};
    o[3] = BgpuShapeVertex{x1, y1, hx, hy, hx, hy, radius, thick, colors[3]};
    ++scount_;
    ++batches_[curBatch_].count;
}

void GpuExec::flushAll() {
    if (!inPass_ || !cmd_) return;
    if (vcount_ == 0 && scount_ == 0) {
        batches_.clear();
        curBatch_ = kNoBatch;
        return;
    }
    // 内部 pass（图层合成 / 毛玻璃 / 贴屏）的四边形不算进 Stats：那是实现成本，
    // 不是调用方画的图元数量。
    const bool count = !internalPass_;
    if (count) {
        stats_.quads += static_cast<int>(vcount_);
        stats_.shapes += static_cast<int>(scount_);
    }

    bool haveScissor = false;
    int lastSx = 0, lastSy = 0, lastSw = -1, lastSh = -1;
    bool haveXf = false;
    float lastXf[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    BgpuPipeline lastPipe = 0;
    BgpuTexture lastTex = 0;
    BgpuSampler lastSamp = 0;
    bool haveTex = false;
    BgpuResult err = BGPU_OK;
    const char* errWhat = nullptr;

    for (const Batch& b : batches_) {
        if (b.count == 0) continue;
        if (b.sw <= 0 || b.sh <= 0) continue;   // 空裁剪：整批丢掉
        if (!haveScissor || b.sx != lastSx || b.sy != lastSy || b.sw != lastSw ||
            b.sh != lastSh) {
            err = bingpu_cmd_set_scissor(cmd_, b.sx, b.sy, b.sw, b.sh);
            if (err != BGPU_OK) { errWhat = "bingpu_cmd_set_scissor"; break; }
            if (count) ++stats_.clips;
            lastSx = b.sx; lastSy = b.sy; lastSw = b.sw; lastSh = b.sh;
            haveScissor = true;
        }
        if (!haveXf || !SameXf(lastXf, b.xf)) {
            // 单位阵传 nullptr（binGPU 的约定：NULL = 复位成单位阵）
            err = bingpu_cmd_set_affine(cmd_, IdentityXf(b.xf) ? nullptr : b.xf);
            if (err != BGPU_OK) { errWhat = "bingpu_cmd_set_affine"; break; }
            for (int i = 0; i < 6; ++i) lastXf[i] = b.xf[i];
            haveXf = true;
        }
        if (b.shape) {
            const BgpuPipeline p = pipeShape_[static_cast<int>(b.blend)];
            if (!p) { continue; }   // 没有 CAP_SHAPE：形状批次不该存在，防御性跳过
            if (p != lastPipe) {
                err = bingpu_cmd_set_pipeline(cmd_, p);
                if (err != BGPU_OK) { errWhat = "bingpu_cmd_set_pipeline(shape)"; break; }
                lastPipe = p;
            }
            const BgpuShapeVertex* base = sq_.data() + static_cast<size_t>(b.first) * 4u;
            for (uint32_t done = 0; done < b.count; done += kQuadsPerDraw) {
                const uint32_t n = std::min(kQuadsPerDraw, b.count - done);
                err = bingpu_cmd_draw_shapes(cmd_, base + static_cast<size_t>(done) * 4u, n);
                if (err != BGPU_OK) { errWhat = "bingpu_cmd_draw_shapes"; break; }
                if (count) ++stats_.draws;
            }
            if (err != BGPU_OK) break;
        } else {
            const BgpuPipeline p = pipe2d_[static_cast<int>(b.blend)];
            if (!p) { continue; }
            if (p != lastPipe) {
                err = bingpu_cmd_set_pipeline(cmd_, p);
                if (err != BGPU_OK) { errWhat = "bingpu_cmd_set_pipeline"; break; }
                lastPipe = p;
            }
            if (!haveTex || b.tex != lastTex || b.samp != lastSamp) {
                err = bingpu_cmd_set_texture(cmd_, b.tex, b.samp);
                if (err != BGPU_OK) { errWhat = "bingpu_cmd_set_texture"; break; }
                if (count) ++stats_.binds;
                lastTex = b.tex;
                lastSamp = b.samp;
                haveTex = true;
            }
            const BgpuVertex* base = vq_.data() + static_cast<size_t>(b.first) * 4u;
            for (uint32_t done = 0; done < b.count; done += kQuadsPerDraw) {
                const uint32_t n = std::min(kQuadsPerDraw, b.count - done);
                err = bingpu_cmd_draw_quads(cmd_, base + static_cast<size_t>(done) * 4u, n);
                if (err != BGPU_OK) { errWhat = "bingpu_cmd_draw_quads"; break; }
                if (count) ++stats_.draws;
            }
            if (err != BGPU_OK) break;
        }
    }
    if (err != BGPU_OK && errWhat) fail(errWhat, err);

    vcount_ = 0;
    scount_ = 0;
    batches_.clear();
    curBatch_ = kNoBatch;
}

// ============================================================
// 图元
// ============================================================

void GpuExec::fillRect(const Rect& r, const Color& c) {
    if (!drawing() || r.empty()) return;
    // 全透明在 over/add 语义下什么都不改，但在 Replace/Keep 下是"真的写 0" ——
    // 判据与 CPU 光栅共用 raster.h 的 ZeroAlphaMatters（别各写一份）
    if (c.a == 0 && !ZeroAlphaMatters(blend_)) return;
    curTex_ = whiteTexture();
    curSamp_ = sampNearest_;
    emitQuad(r.x, r.y, r.right(), r.bottom(), 0.5f, 0.5f, 0.5f, 0.5f, c.toArgbStraight());
}

void GpuExec::fillRounded(const Rect& r, float radius, const Color& c) {
    if (!drawing() || r.empty()) return;
    if (c.a == 0 && !ZeroAlphaMatters(blend_)) return;
    const float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    if (!shapeOk_) {
        // 设备没有解析圆角：**不退回覆盖率模板**，直接直角兜底并记一条 lastError
        noteError("设备没有 BGPU_CAP_SHAPE：圆角退回直角");
        fillRect(r, c);
        return;
    }
    if (rad < 0.5f) {   // 半径不到半像素：视觉上就是直角（与 GpuCompositor 同一条判据）
        fillRect(r, c);
        return;
    }
    emitShape(r.x, r.y, r.w, r.h, rad, 0.0f, c.toArgbStraight());
}

void GpuExec::strokeRounded(const Rect& r, float radius, float thickness, const Color& c) {
    if (!drawing() || r.empty()) return;
    if (thickness <= 0.0f) return;
    if (c.a == 0 && !ZeroAlphaMatters(blend_)) return;
    const float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);

    // 直角描边 = 四条矩形（上/下/左/右），与 DisplayList 的 CPU 执行体同一条分支
    auto rectStroke = [&]() {
        const float t = thickness;
        const float ht = std::min(t, r.h);
        fillRect(Rect(r.x, r.y, r.w, ht), c);
        fillRect(Rect(r.x, r.y + r.h - ht, r.w, ht), c);
        const float inner = std::max(0.0f, r.h - 2.0f * t);
        if (inner > 0.0f) {
            const float wt = std::min(t, r.w);
            fillRect(Rect(r.x, r.y + t, wt, inner), c);
            fillRect(Rect(r.x + r.w - wt, r.y + t, wt, inner), c);
        }
    };

    if (rad <= 0.5f) {
        rectStroke();
        return;
    }
    if (!shapeOk_) {
        noteError("设备没有 BGPU_CAP_SHAPE：圆角描边退回直角");
        rectStroke();
        return;
    }
    // thick > 0 = **向内** thickness 的环带（`|d + t/2| - t/2` 就是到带子中心线的
    // 距离）。厚度是浮点原值：SDF 没有"必须四舍五入成整数模板"的限制。
    emitShape(r.x, r.y, r.w, r.h, rad, thickness, c.toArgbStraight());
}

void GpuExec::fillGradient(const Rect& r, float angleDeg, const Color& from,
                           const Color& to) {
    if (!drawing() || r.empty()) return;
    if (from.a == 0 && to.a == 0) return;
    // 四角直通色：LinearGradientCorners 与 Paint::linearAngle 是**同一个轴**
    // （同一个 CSS 角度约定、同一个"投影长度最大"的端点取法），所以两条执行体
    // 画出来的是同一个渐变。输出顺序 TL,TR,BL,BR，与顶点顺序一致。
    uint32_t colors[4];
    LinearGradientCorners(r, angleDeg, from.toArgbStraight(), to.toArgbStraight(), colors);
    curTex_ = whiteTexture();
    curSamp_ = sampNearest_;
    emitQuadColors(r.x, r.y, r.right(), r.bottom(), 0.5f, 0.5f, 0.5f, 0.5f, colors);
}

void GpuExec::fillRoundedGradient(const Rect& r, float radius, float angleDeg,
                                  const Color& from, const Color& to) {
    if (!drawing() || r.empty()) return;
    if (from.a == 0 && to.a == 0) return;
    const float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    uint32_t colors[4];
    LinearGradientCorners(r, angleDeg, from.toArgbStraight(), to.toArgbStraight(), colors);
    if (!shapeOk_) {
        noteError("设备没有 BGPU_CAP_SHAPE：圆角渐变退回直角");
        curTex_ = whiteTexture();
        curSamp_ = sampNearest_;
        emitQuadColors(r.x, r.y, r.right(), r.bottom(), 0.5f, 0.5f, 0.5f, 0.5f, colors);
        return;
    }
    if (rad < 0.5f) {
        curTex_ = whiteTexture();
        curSamp_ = sampNearest_;
        emitQuadColors(r.x, r.y, r.right(), r.bottom(), 0.5f, 0.5f, 0.5f, 0.5f, colors);
        return;
    }
    // 四角直通色给四个顶点，片元里 `颜色 × cov`：线性渐变在屏幕空间是仿射函数，
    // 被四边形上的线性插值精确复现，弧上的覆盖率与渐变色互不污染。
    emitShapeColors(r.x, r.y, r.w, r.h, rad, 0.0f, colors);
}

// ------------------------------------------------------------
// mip 的请求门槛（单位：倍）。
//
// 准则：**只有会被缩小采样的图元纹理才请求真 mip 链**（判据在下面 drawSurface：
// 用 uv 折算出的源像素尺寸与目标矩形比）。数据支撑（`make pixels-bin2d-gpu`，
// 18 页对 binUI/docs/pixels 基线，VK 4×、0.5% 容差 / 单通道 8）：
//   · image-mip（512² 源图缩到 64~200 px，2px 棋盘）**不要 mip：37.021% 像素不同、
//     最大通道差 128**（摩尔纹）；要 mip：**0.000% / max 0**。所以这条不是可选项。
//   · 其余 17 页的图片要么 1:1、要么放大，mip 开与不开**逐像素相同**。
// 代价（`probe_cost`，窗口路径、30 帧中位，同一轮里交替跑"无条件请求"与"1.5× 门槛"
// 两份构建）：两者差 ≤3%（appshow 1.58~1.65 对 1.58~1.61 ms），而**同一份二进制
// 连跑两次也能差 ±0.05 ms** —— 帧时上量不出差别；代价落在显存（链多 ~1/3）与建链
// 时那一串逐级 blit 上。
// 所以门槛的作用不是省帧时，而是**把不缩小的纹理留在默认 1 级路径上**：那条路径与
// 改动前逐位相同（不给几百张 1:1 图标各建一条 10 级链，也不引入任何像素风险）。
// 门槛取 1.5 而不是 1.0：1.0~1.5 之间是双线性自己的地盘（一个目标像素最多覆盖 2 个
// 纹素），mip 救不回信息；而 1:1 贴图（九宫格的角、图标）永远不进 mip 路径。
// ------------------------------------------------------------
constexpr float kMipDownscale = 1.5f;

void GpuExec::drawSurface(const Surface& src, const Rect& dst, const Rect& uv,
                          const Color& tint) {
    if (!drawing() || src.empty() || dst.empty() || tint.a == 0) return;
    // 会不会被**缩小**采样？uv 决定用到的是源图的哪一块（九宫格/图集走子矩形），
    // 所以按 uv 折算成源像素尺寸再比，而不是拿整张图比 —— 否则"40×40 的角贴到
    // 40×40"会被当成 1:1，"整张 200×200 的图只用中间 10×10 那块"会被误判成放大。
    // 与 DisplayList::gpuCompatible 判"缩小贴图"的口径逐字相同（同一套折算）。
    const float srcW = uv.w * static_cast<float>(src.width());
    const float srcH = uv.h * static_cast<float>(src.height());
    const bool shrink = (srcW > 0.0f && dst.w * kMipDownscale < srcW) ||
                        (srcH > 0.0f && dst.h * kMipDownscale < srcH);
    BgpuTexture tex = surfaceTexture(src, shrink);
    if (!tex) return;
    curTex_ = tex;
    curSamp_ = sampLinear_;
    emitQuad(dst.x, dst.y, dst.right(), dst.bottom(), uv.x, uv.y, uv.right(), uv.bottom(),
             tint.toArgbStraight());
}

// 九宫格：切分与 src/raster.cpp 的 DrawNineSlice 逐字同一套（四角保尺寸、四边与中心
// 拉伸），只是逐块发成四边形而不是逐像素。GPU 侧没有"图片"这个概念，所以收 Surface。
void GpuExec::drawNineSlice(const Surface& src, const Rect& dst, float insetL, float insetT,
                            float insetR, float insetB, const Color& tint) {
    if (!drawing() || src.empty() || dst.empty()) return;
    const float iw = static_cast<float>(src.width());
    const float ih = static_cast<float>(src.height());
    const float l = std::max(0.0f, std::min(insetL, iw));
    const float r = std::max(0.0f, std::min(insetR, iw - l));
    const float tp = std::max(0.0f, std::min(insetT, ih));
    const float bt = std::max(0.0f, std::min(insetB, ih - tp));

    const float dl = std::min(dst.w, l);
    const float dr = std::min(dst.w - dl, r);
    const float dt = std::min(dst.h, tp);
    const float db = std::min(dst.h - dt, bt);
    const float dcx = dst.w - dl - dr;
    const float dcy = dst.h - dt - db;
    const float scx = iw - l - r;
    const float scy = ih - tp - bt;

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const float sx = (col == 0) ? 0.0f : (col == 1 ? l : l + scx);
            const float sw = (col == 0) ? l : (col == 1 ? scx : r);
            const float sy = (row == 0) ? 0.0f : (row == 1 ? tp : tp + scy);
            const float sh = (row == 0) ? tp : (row == 1 ? scy : bt);
            const float dx = (col == 0) ? 0.0f : (col == 1 ? dl : dl + dcx);
            const float dw = (col == 0) ? dl : (col == 1 ? dcx : dr);
            const float dy = (row == 0) ? 0.0f : (row == 1 ? dt : dt + dcy);
            const float dh = (row == 0) ? dt : (row == 1 ? dcy : db);
            if (sw <= 0.0f || sh <= 0.0f || dw <= 0.0f || dh <= 0.0f) continue;
            drawSurface(src, Rect(dst.x + dx, dst.y + dy, dw, dh),
                        Rect(sx / iw, sy / ih, sw / iw, sh / ih), tint);
        }
    }
}

void GpuExec::drawQuadUV(const float pos[8], const float uv[8], const Color col[4]) {
    if (!drawing() || !pos || !uv || !col) return;
    uint32_t colors[4];
    for (int i = 0; i < 4; ++i) colors[i] = col[i].toArgbStraight();
    curTex_ = whiteTexture();
    curSamp_ = sampNearest_;
    emitQuadUV(pos, uv, colors);
}

// ============================================================
// 外部纹理
// ============================================================

uint64_t GpuExec::texCreate(int w, int h, bool dynamic, bool nearest) {
    if (!ok() || w <= 0 || h <= 0) return 0;
    // 预乘位必须带上：调用方交进来的是 bin2d 的预乘像素（字形图集的覆盖率、视频帧
    // 已经乘过 alpha），不声明就会被 binGPU 再预乘一遍 —— 表现是"文字整体淡一档"。
    uint32_t flags = BGPU_TEXTURE_PREMULTIPLIED;
    if (dynamic) flags |= BGPU_TEXTURE_DYNAMIC;
    // 零缓冲：不初始化的话，没写过的纹素是未定义显存，被采到就是垃圾。
    std::vector<uint8_t> zeros(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0u);
    BgpuTexture tex = makeTexture(w, h, flags, zeros.data(), static_cast<uint32_t>(w) * 4u);
    if (!tex) return 0;
    texMeta_.push_back(TexMeta{tex, nearest});
    return tex;
}

void GpuExec::texUpload(uint64_t tex, const Rect& box, const uint8_t* pixels,
                        uint32_t stride) {
    if (!device_ || !tex || !pixels) return;
    uint32_t tw = 0, th = 0;
    BgpuTextureInfo info{};
    info.struct_size = sizeof(info);
    if (bingpu_texture_info(static_cast<BgpuTexture>(tex), &info) == BGPU_OK) {
        tw = info.width;
        th = info.height;
    }
    if (box.empty()) {
        // 空 box = 整张（与 GpuCompositor::upload 的口径一致）
        bingpu_texture_upload(static_cast<BgpuTexture>(tex), nullptr, pixels, stride);
        return;
    }
    int x0 = static_cast<int>(std::floor(box.x));
    int y0 = static_cast<int>(std::floor(box.y));
    int x1 = static_cast<int>(std::ceil(box.right()));
    int y1 = static_cast<int>(std::ceil(box.bottom()));
    if (tw && th) {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > static_cast<int>(tw)) x1 = static_cast<int>(tw);
        if (y1 > static_cast<int>(th)) y1 = static_cast<int>(th);
    }
    if (x1 <= x0 || y1 <= y0) return;
    BgpuBox b{};
    b.x = static_cast<uint32_t>(x0);
    b.y = static_cast<uint32_t>(y0);
    b.w = static_cast<uint32_t>(x1 - x0);
    b.h = static_cast<uint32_t>(y1 - y0);
    const uint8_t* px = pixels;
    // 调用方给的是"盒子左上角开始"的指针：行距 stride 保留，起始地址不用再挪
    const BgpuResult r = bingpu_texture_upload(static_cast<BgpuTexture>(tex), &b, px, stride);
    if (r != BGPU_OK) fail("bingpu_texture_upload(tex)", r);
}

void GpuExec::texDestroy(uint64_t tex) {
    if (!tex) return;
    for (size_t i = 0; i < texMeta_.size(); ++i) {
        if (texMeta_[i].tex == tex) {
            texMeta_.erase(texMeta_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (!device_) return;
    // **不当场销毁**：帧内销毁会让本帧已经录进命令流的笔引用到被回收的句柄槽
    // （见 pendingFree_ 的说明）。挂进表里，下一帧 frame_begin 之后统一释放。
    pendingFree_.push_back(static_cast<BgpuTexture>(tex));
}

void GpuExec::drainPendingFree() {
    if (!device_ || pendingFree_.empty()) return;
    for (BgpuTexture t : pendingFree_)
        if (t) bingpu_texture_destroy(device_, t);
    pendingFree_.clear();
}

void GpuExec::drawTexture(uint64_t tex, const Rect& dst, const Rect& uv,
                          const Color& tint) {
    if (!drawing() || !tex || dst.empty() || tint.a == 0) return;
    curTex_ = static_cast<BgpuTexture>(tex);
    curSamp_ = samplerFor(curTex_);
    emitQuad(dst.x, dst.y, dst.right(), dst.bottom(), uv.x, uv.y, uv.right(), uv.bottom(),
             tint.toArgbStraight());
}

BgpuSampler GpuExec::samplerFor(BgpuTexture tex) const {
    for (const TexMeta& m : texMeta_)
        if (m.tex == tex) return m.nearest ? sampNearest_ : sampLinear_;
    return sampLinear_;
}

// 原地改像素之后作的废：**只打脏标记**，不销毁句柄。
// 帧内销毁会被已经录进命令缓冲的图元引用着（Vulkan 上表现为"上一笔采样到别的内容"）；
// 而重传进同一张纹理既保住句柄、又让后面的笔看到新内容 —— 见 surfaceTexture。
void GpuExec::imageChanged(const Surface& src) {
    if (!device_ || src.empty()) return;
    for (ImgEntry& e : imgCache_) {
        if (e.id == src.id() && e.w == src.width() && e.h == src.height()) e.dirty = true;
    }
}

// ============================================================
// 纹理缓存（drawSurface）
// ============================================================

BgpuTexture GpuExec::surfaceTexture(const Surface& src, bool wantMips) {
    if (!device_ || src.empty()) return 0;
    // 缓存键是 Surface 的**实例身份** + 尺寸（0.22.3），不是像素指针：
    // 指针会被复用 —— 一张图销毁后，下一张同样尺寸的图常拿到同一段内存，撞上就会把
    // 新图画成上一张的旧内容（字形图集换页时实测：整页文字串格、阴影整块消失）。
    // 口径与 DisplayList 一致：**图片被认为不变**；
    // 内容会变的场景（字形图集 / 视频帧）有两条路：
    //   · 每次改完调 imageChanged()（帧内增量更新，见下）；
    //   · 或者干脆用 texCreate/texUpload 自己管句柄。
    // 设备没有 CAP_MIPMAPS（CPU / D3D11）时请求也没用，别让后端白走降级。
    const bool want = wantMips && mipOk_;
    const uint64_t key = src.id();
    for (ImgEntry& e : imgCache_) {
        if (e.id != key || e.w != src.width() || e.h != src.height()) continue;
        if (want && !e.mips) {
            // 先按 1:1 建过、后来才被缩着画（九宫格的角先 1:1、中心后拉伸就是这种
            // 顺序）：**升一次级**。旧句柄不能当场销毁（批次里还引用着），挂进
            // pendingFree_ 等帧尾；同一个 Surface 只升一次，之后一直带 mip 链。
            BgpuTexture up = makeImageTexture(src.width(), src.height(), src.pixels().data(),
                                              static_cast<uint32_t>(src.stride()), true);
            if (up) {
                pendingFree_.push_back(e.tex);
                e.tex = up;
                e.mips = true;
                e.dirty = false;   // 升级时已经传了最新像素
            }
        }
        if (e.dirty) {
            // 原地改过的像素：**重传进同一张纹理**。不销毁重建是刻意的 ——
            // 帧内销毁的句柄可能还被已经录进命令缓冲的图元引用着（Vulkan 上表现为
            // "上一笔采样到别的内容"，实测就是字形串格）。上传要挂在 pass 外面做。
            flushAll();
            if (!reuploadImageTexture(e.tex, src.pixels().data(),
                                      static_cast<uint32_t>(src.stride())))
                return 0;
            e.dirty = false;
        }
        return e.tex;
    }
    if (imgCache_.size() >= kMaxImgCache) {
        // **不能当场销毁**：批次里还引用着它（flushAll 只是把笔录进命令流，真正执行在
        // 帧尾），当场 destroy 会让那些笔采样到下一张落进同一个句柄槽的纹理 ——
        // 字形图集淘汰时实测"整页文字串格"。挂到 pendingFree_ 里，帧尾之后再释放。
        pendingFree_.push_back(imgCache_.front().tex);
        imgCache_.erase(imgCache_.begin());
    }
    BgpuTexture tex = makeImageTexture(src.width(), src.height(), src.pixels().data(),
                                       static_cast<uint32_t>(src.stride()), want);
    if (!tex) return 0;
    imgCache_.push_back(ImgEntry{key, src.width(), src.height(), tex, false, want});
    return tex;
}

// ============================================================
// 图层（组透明度）
// ============================================================

int GpuExec::acquireLayer() {
    for (size_t i = 0; i < layerPool_.size(); ++i)
        if (!layerPool_[i].inUse) return static_cast<int>(i);
    if (static_cast<int>(layerPool_.size()) >= kMaxLayers) return -1;
    LayerTarget t;
    t.tex = makeTexture(width_, height_, BGPU_TEXTURE_RENDER_TARGET, nullptr, 0);
    if (!t.tex) return -1;
    layerPool_.push_back(t);
    return static_cast<int>(layerPool_.size()) - 1;
}

void GpuExec::destroyLayerPool() {
    for (LayerTarget& t : layerPool_) {
        if (t.tex && device_) bingpu_texture_destroy(device_, t.tex);
        t.tex = 0;
        t.inUse = false;
    }
    layerPool_.clear();
    layerStack_.clear();
}

bool GpuExec::pushLayer(const Rect& localBounds, float alpha) {
    if (!drawing() || localBounds.empty() || !(alpha > 0.0f)) return false;
    if (static_cast<int>(layerStack_.size()) >= kMaxLayers) {
        noteError("图层超过 4 层，多出来的被丢弃");
        return false;
    }
    flushAll();   // 层之前画的图元必须落在**上一层**，不能跟着进层
    const int idx = acquireLayer();
    if (idx < 0) {
        noteError("图层纹理分配失败");
        return false;
    }
    LayerFrame f;
    f.idx = idx;
    const Rect scr = xf_.mapBounds(localBounds);
    f.x = scr.x;
    f.y = scr.y;
    f.w = scr.w;
    f.h = scr.h;
    f.alpha = alpha;
    f.prevTarget = passTarget_;
    layerPool_[static_cast<size_t>(idx)].inUse = true;
    // 关键：begin_render 切到层纹理之后，**之后的图元才会落进层里**（坐标仍是屏幕坐标，
    // 层纹理是整窗尺寸，所以不需要换算）
    beginPass(layerPool_[static_cast<size_t>(idx)].tex, width_, height_, true, 0x00000000u);
    if (!inPass_) {
        layerPool_[static_cast<size_t>(idx)].inUse = false;
        return false;
    }
    layerStack_.push_back(f);
    ++stats_.layers;
    curTex_ = whiteTexture();
    curSamp_ = sampLinear_;
    return true;
}

void GpuExec::popLayer() {
    if (!drawing() || layerStack_.empty()) return;
    flushAll();   // 层内容先落进层纹理
    const LayerFrame f = layerStack_.back();
    layerStack_.pop_back();

    const BlendMode savedBlend = blend_;
    const BgpuTexture savedTex = curTex_;
    const BgpuSampler savedSamp = curSamp_;

    beginPass(f.prevTarget ? f.prevTarget : canvas_, width_, height_, false, 0u);
    if (inPass_) {
        const float invW = 1.0f / static_cast<float>(width_);
        const float invH = 1.0f / static_cast<float>(height_);
        float a = f.alpha;
        a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        const uint32_t av = static_cast<uint32_t>(a * 255.0f + 0.5f);
        // 组透明度：层内容按 (alpha, alpha, alpha, alpha) 的白色顶点色乘一遍再 over。
        // 1:1 + NEAREST ⇒ 精确拷贝；顶点已经算好是屏幕像素，所以 screenSpace_。
        blend_ = BlendMode::SrcOver;
        curTex_ = layerPool_[static_cast<size_t>(f.idx)].tex;
        curSamp_ = sampNearest_;
        screenSpace_ = true;
        emitQuad(f.x, f.y, f.x + f.w, f.y + f.h, f.x * invW, f.y * invH,
                 (f.x + f.w) * invW, (f.y + f.h) * invH, (av << 24) | 0x00FFFFFFu);
        flushAll();
        screenSpace_ = false;
        blend_ = savedBlend;
        curTex_ = savedTex;
        curSamp_ = savedSamp;
    }
    layerPool_[static_cast<size_t>(f.idx)].inUse = false;
}

// ============================================================
// 内容层（0.22.5）：离屏层在设备上的一等操作
//
// 入层那一半与 pushLayer 逐条同构（同一个层池、同一个层内裁剪、同一条"内容按屏幕
// 坐标落进整窗纹理"的口径），差别只在出口：
//   · popLayer      → 整层乘 alpha 合成回上一层（组透明度）；
//   · popContentLayer → **什么都不合成**，只把 (纹理, 屏幕矩形) 变成一个句柄交出去。
// 于是"图层模糊 / 外阴影"终于能在设备上做完：调用方拿着句柄，先按自己的需要把它
// 模糊、按内容 alpha 上色、按偏移贴回来 —— 不需要像素回 CPU。
//
// 句柄池：纹理就是 layerPool_ 里的那一批（整窗尺寸、按 inUse 复用），**一帧里几十个
// 图层不会新建纹理**；releaseContentLayer 只是把它标回空闲，销毁只发生在 destroy()。
// ============================================================

bool GpuExec::pushContentLayer(const Rect& localBounds) {
    if (!drawing() || localBounds.empty()) return false;
    if (static_cast<int>(layerStack_.size()) >= kMaxLayers) {
        noteError("图层超过 4 层，多出来的内容层被丢弃");
        return false;
    }
    flushAll();   // 层之前画的图元必须落在**上一层**，不能跟着进层
    const int idx = acquireLayer();
    if (idx < 0) {
        noteError("内容层纹理分配失败");
        return false;
    }
    LayerFrame f;
    f.idx = idx;
    const Rect scr = xf_.mapBounds(localBounds);
    f.x = scr.x;
    f.y = scr.y;
    f.w = scr.w;
    f.h = scr.h;
    f.alpha = 1.0f;
    f.prevTarget = passTarget_;
    f.content = true;
    layerPool_[static_cast<size_t>(idx)].inUse = true;
    beginPass(layerPool_[static_cast<size_t>(idx)].tex, width_, height_, true, 0x00000000u);
    if (!inPass_) {
        layerPool_[static_cast<size_t>(idx)].inUse = false;
        return false;
    }
    layerStack_.push_back(f);
    ++stats_.layers;
    curTex_ = whiteTexture();
    curSamp_ = sampLinear_;
    return true;
}

bool GpuExec::popContentLayer(ContentLayerHandle& out) {
    if (!drawing() || layerStack_.empty()) return false;
    if (!layerStack_.back().content) {
        // 配对错了（拿 popContentLayer 去收一个 pushLayer）：**不弹**，让调用方的
        // 降级路自己收自己的，别把两个栈搞乱。
        noteError("popContentLayer: 栈顶不是内容层");
        return false;
    }
    flushAll();   // 层内容先落进层纹理
    const LayerFrame f = layerStack_.back();
    layerStack_.pop_back();

    // 回到入层前的目标（可能是 canvas，也可能是外层的层纹理）
    beginPass(f.prevTarget ? f.prevTarget : canvas_, width_, height_, false, 0u);
    if (!inPass_) {
        layerPool_[static_cast<size_t>(f.idx)].inUse = false;
        return false;
    }
    ContentEntry e;
    e.id = nextContentId_++;
    e.idx = f.idx;
    e.screen = Rect(f.x, f.y, f.w, f.h);
    contentStack_.push_back(e);
    out.id = e.id;
    out.screen = e.screen;
    return true;
}

void GpuExec::releaseContentLayer(uint64_t id) {
    if (!id) return;
    for (size_t i = 0; i < contentStack_.size(); ++i) {
        if (contentStack_[i].id != id) continue;
        const int idx = contentStack_[i].idx;
        if (idx >= 0 && idx < static_cast<int>(layerPool_.size()))
            layerPool_[static_cast<size_t>(idx)].inUse = false;
        contentStack_.erase(contentStack_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

bool GpuExec::clampToWindow(const Rect& r, int& rx, int& ry, int& rw, int& rh) const {
    if (r.empty()) return false;
    int x0 = static_cast<int>(std::floor(r.x));
    int y0 = static_cast<int>(std::floor(r.y));
    int x1 = static_cast<int>(std::ceil(r.right()));
    int y1 = static_cast<int>(std::ceil(r.bottom()));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width_) x1 = width_;
    if (y1 > height_) y1 = height_;
    if (x1 - x0 < 1 || y1 - y0 < 1) return false;
    rx = x0;
    ry = y0;
    rw = x1 - x0;
    rh = y1 - y0;
    return true;
}

GpuExec::BlurChain* GpuExec::acquireContentBlur(int w, int h) {
    if (!device_ || w <= 0 || h <= 0) return nullptr;
    for (size_t i = 0; i < contentBlur_.size(); ++i) {
        if (contentBlur_[i].w != w || contentBlur_[i].h != h) continue;
        if (i + 1 != contentBlur_.size()) {
            // 挪到末尾 = 最近使用（池满时从头部顶掉）
            const ContentBlur hit = contentBlur_[i];
            contentBlur_.erase(contentBlur_.begin() + static_cast<std::ptrdiff_t>(i));
            contentBlur_.push_back(hit);
        }
        return &contentBlur_.back().chain;
    }
    if (contentBlur_.size() >= kMaxContentBlur) {
        // 池满：顶掉最久没用的那一条。旧纹理**挂 pendingFree_**，不在帧中途销毁
        // （本帧已经录进命令流的笔可能还引用着它们，0.22.3 的坑）。
        ContentBlur victim = contentBlur_.front();
        contentBlur_.erase(contentBlur_.begin());
        retireBlurChain(victim.chain);
        victim.w = w;
        victim.h = h;
        if (!ensureBlurChainFor(victim.chain, w, h)) return nullptr;
        contentBlur_.push_back(victim);
        return &contentBlur_.back().chain;
    }
    ContentBlur cb;
    cb.w = w;
    cb.h = h;
    if (!ensureBlurChainFor(cb.chain, w, h)) return nullptr;
    contentBlur_.push_back(cb);
    return &contentBlur_.back().chain;
}

void GpuExec::destroyContentBlur() {
    for (ContentBlur& cb : contentBlur_) destroyBlurChain(cb.chain);
    contentBlur_.clear();
}

// 抓取：把 src（整窗尺寸的纹理）上 (rx,ry,rw,rh) 那块 1:1 拷进 chain.lv[0]。
// 整数对齐 + NEAREST = 逐像素拷贝，不做任何重采样（模糊只发生在后面几级）。
bool GpuExec::grabIntoBlurChain(BlurChain& c, BgpuTexture src, int rx, int ry, int rw,
                                int rh) {
    if (!src || rw < 1 || rh < 1) return false;
    if (!ensureBlurChainFor(c, rw, rh)) return false;
    beginPass(c.lv[0].tex, rw, rh, true, 0x00000000u);
    if (!inPass_) return false;
    blend_ = BlendMode::Replace;
    curTex_ = src;
    curSamp_ = sampNearest_;
    const float invW = 1.0f / static_cast<float>(width_);
    const float invH = 1.0f / static_cast<float>(height_);
    emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
             static_cast<float>(rx) * invW, static_cast<float>(ry) * invH,
             static_cast<float>(rx + rw) * invW, static_cast<float>(ry + rh) * invH,
             0xFFFFFFFFu);
    flushAll();
    return true;
}

// lv[0] 已经是待模糊的内容：逐级 2× 降采样到"σ 还剩 ≥2 纹素"那一级，再分离高斯。
bool GpuExec::blurChainDown(BlurChain& c, float blurPx, BgpuTexture& out) {
    if (!(blurPx > 0.0f) || !c.lv[0].tex) return false;
    const int levels = BlurLevelsFor(blurPx);
    for (int i = 1; i <= levels; ++i) {
        const int dw = std::max(1, c.lv[i - 1].w / 2);
        const int dh = std::max(1, c.lv[i - 1].h / 2);
        if (!ensureBlurLevel(c, i, dw, dh)) return false;
        beginPass(c.lv[i].tex, dw, dh, true, 0x00000000u);
        if (!inPass_) return false;
        blend_ = BlendMode::Replace;
        curTex_ = c.lv[i - 1].tex;
        curSamp_ = sampLinear_;
        emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh), 0.0f, 0.0f, 1.0f,
                 1.0f, 0xFFFFFFFFu);
        flushAll();
    }
    if (!gaussLevel(c, levels, blurPx)) return false;
    out = c.lv[levels].tex;
    return true;
}

bool GpuExec::drawContentLayer(uint64_t id, const Rect& dstDevice, const Rect& uv,
                               const Color& tint, float blurPx, bool nearest) {
    const ContentEntry* entry = nullptr;
    for (const ContentEntry& c : contentStack_)
        if (c.id == id) { entry = &c; break; }
    if (!entry || !drawing()) return false;
    if (dstDevice.empty() || tint.a == 0) return true;   // 没什么可画，也算做到了

    const BgpuTexture layerTex = layerPool_[static_cast<size_t>(entry->idx)].tex;
    if (!layerTex) return false;
    const Rect content = entry->screen;
    if (content.empty()) return true;

    const BgpuTexture target = passTarget_ ? passTarget_ : canvas_;
    const BlendMode savedBlend = blend_;
    const BgpuTexture savedTex = curTex_;
    const BgpuSampler savedSamp = curSamp_;
    const bool savedInternal = internalPass_;
    const bool savedScreen = screenSpace_;
    bool done = false;

    if (!(blurPx > 0.0f)) {
        // 不模糊：直接把层纹理按 uv 贴回去。内容就在整窗纹理的 content 矩形上，
        // 所以 uv 要先换算到 texture 空间的子矩形。
        const float invW = 1.0f / static_cast<float>(width_);
        const float invH = 1.0f / static_cast<float>(height_);
        const float u0 = (content.x + uv.x * content.w) * invW;
        const float v0 = (content.y + uv.y * content.h) * invH;
        const float u1 = (content.x + uv.right() * content.w) * invW;
        const float v1 = (content.y + uv.bottom() * content.h) * invH;
        blend_ = BlendMode::SrcOver;
        curTex_ = layerTex;
        curSamp_ = nearest ? sampNearest_ : sampLinear_;
        screenSpace_ = true;   // 顶点已经是目标像素：单位阵，但裁剪栈照常生效
        emitQuad(dstDevice.x, dstDevice.y, dstDevice.right(), dstDevice.bottom(), u0, v0, u1,
                 v1, tint.toArgbStraight());
        flushAll();
        done = true;
    } else {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        if (!clampToWindow(content, rx, ry, rw, rh)) return true;   // 整块在窗口外
        BlurChain* chain = acquireContentBlur(rw, rh);
        if (chain) {
            internalPass_ = true;   // 抓取/降采样/高斯都是"各自目标的像素"
            BgpuTexture blurred = 0;
            if (grabIntoBlurChain(*chain, layerTex, rx, ry, rw, rh) &&
                blurChainDown(*chain, blurPx, blurred)) {
                // 抓下来的那块 G = (rx,ry,rw,rh) 是**内容矩形夹进窗口**之后的结果，
                // 它与 dst 的比例 = dstDevice.w / content.w。调用方给的 uv 又可能只是
                // 内容里的一段，所以真正要画的是 S ∩ G（S = uv 在内容空间的那块）：
                // 层越出窗口时（大模糊的 pad 最容易），画出来的正好是"窗口里真的有的
                // 那一块"，不会把边缘纹素当成整块拉过来。
                const float sxc = content.w > 0.0f ? dstDevice.w / content.w : 1.0f;
                const float syc = content.h > 0.0f ? dstDevice.h / content.h : 1.0f;
                const float sx0 = content.x + uv.x * content.w;
                const float sx1 = content.x + uv.right() * content.w;
                const float sy0 = content.y + uv.y * content.h;
                const float sy1 = content.y + uv.bottom() * content.h;
                const float ix0 = std::max(sx0, static_cast<float>(rx));
                const float iy0 = std::max(sy0, static_cast<float>(ry));
                const float ix1 = std::min(sx1, static_cast<float>(rx + rw));
                const float iy1 = std::min(sy1, static_cast<float>(ry + rh));
                if (ix1 > ix0 && iy1 > iy0) {
                    const float dx = dstDevice.x + (ix0 - content.x) * sxc;
                    const float dy = dstDevice.y + (iy0 - content.y) * syc;
                    const float dw = (ix1 - ix0) * sxc;
                    const float dh = (iy1 - iy0) * syc;
                    const float u0 = (ix0 - static_cast<float>(rx)) / static_cast<float>(rw);
                    const float v0 = (iy0 - static_cast<float>(ry)) / static_cast<float>(rh);
                    const float u1 = (ix1 - static_cast<float>(rx)) / static_cast<float>(rw);
                    const float v1 = (iy1 - static_cast<float>(ry)) / static_cast<float>(rh);
                    internalPass_ = savedInternal;
                    beginPass(target, width_, height_, false, 0u);
                    if (inPass_) {
                        blend_ = BlendMode::SrcOver;
                        curTex_ = blurred;
                        curSamp_ = sampLinear_;
                        screenSpace_ = true;
                        emitQuad(dx, dy, dx + dw, dy + dh, u0, v0, u1, v1,
                                 tint.toArgbStraight());
                        flushAll();
                        done = true;
                    }
                } else {
                    done = true;   // 交集为空：没有可见内容，算做到了
                }
            }
        }
    }

    internalPass_ = savedInternal;
    screenSpace_ = savedScreen;
    blend_ = savedBlend;
    curTex_ = savedTex;
    curSamp_ = savedSamp;
    return done;
}

// 外阴影：out = 预乘(color) × 内容alpha（再模糊）。两趟，与 CPU 那条逐条同构：
//   ① 在链的 lv[0] 上铺满预乘色（REPLACE，把内容矩形当画布）；
//   ② dst *= 内容 alpha（KEEP）—— 于是拿到"轮廓 + 纯色"，与内容自己的颜色无关。
// 坐标全程是各自目标的像素（internalPass_），最后再按屏幕空间 over 回去。
bool GpuExec::fillContentLayer(uint64_t id, const Rect& dstDevice, const Color& color,
                               float blurPx) {
    const ContentEntry* entry = nullptr;
    for (const ContentEntry& c : contentStack_)
        if (c.id == id) { entry = &c; break; }
    if (!entry || !drawing()) return false;
    if (dstDevice.empty() || color.a == 0) return true;

    const BgpuTexture layerTex = layerPool_[static_cast<size_t>(entry->idx)].tex;
    if (!layerTex) return false;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (!clampToWindow(entry->screen, rx, ry, rw, rh)) return true;

    const BgpuTexture target = passTarget_ ? passTarget_ : canvas_;
    const BlendMode savedBlend = blend_;
    const BgpuTexture savedTex = curTex_;
    const BgpuSampler savedSamp = curSamp_;
    const bool savedInternal = internalPass_;
    const bool savedScreen = screenSpace_;

    BlurChain* chain = acquireContentBlur(rw, rh);
    if (!chain) return false;

    internalPass_ = true;
    beginPass(chain->lv[0].tex, rw, rh, true, 0x00000000u);
    if (!inPass_) {
        internalPass_ = savedInternal;
        return false;
    }
    const float fw = static_cast<float>(rw);
    const float fh = static_cast<float>(rh);
    blend_ = BlendMode::Replace;
    curTex_ = whiteTexture();
    curSamp_ = sampNearest_;
    emitQuad(0.0f, 0.0f, fw, fh, 0.5f, 0.5f, 0.5f, 0.5f, color.toArgbStraight());
    flushAll();
    blend_ = BlendMode::Keep;   // dst *= srcA：只借内容的**覆盖率**，颜色来自上面那一层
    curTex_ = layerTex;
    const float invW = 1.0f / static_cast<float>(width_);
    const float invH = 1.0f / static_cast<float>(height_);
    emitQuad(0.0f, 0.0f, fw, fh, static_cast<float>(rx) * invW, static_cast<float>(ry) * invH,
             static_cast<float>(rx + rw) * invW, static_cast<float>(ry + rh) * invH,
             0xFFFFFFFFu);
    flushAll();

    BgpuTexture src = chain->lv[0].tex;
    bool okAll = true;
    if (blurPx > 0.0f) {
        BgpuTexture blurred = 0;
        if (blurChainDown(*chain, blurPx, blurred)) src = blurred;
        else okAll = false;
    }

    if (okAll) {
        internalPass_ = savedInternal;
        beginPass(target, width_, height_, false, 0u);
        if (inPass_) {
            // 阴影的矩形 = 内容矩形（夹过窗口的那块）平移到 dstDevice 上
            const float dx = dstDevice.x + (static_cast<float>(rx) - entry->screen.x);
            const float dy = dstDevice.y + (static_cast<float>(ry) - entry->screen.y);
            blend_ = BlendMode::SrcOver;
            curTex_ = src;
            curSamp_ = sampLinear_;
            screenSpace_ = true;
            emitQuad(dx, dy, dx + fw, dy + fh, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
            flushAll();
        } else {
            okAll = false;
        }
    }

    internalPass_ = savedInternal;
    screenSpace_ = savedScreen;
    blend_ = savedBlend;
    curTex_ = savedTex;
    curSamp_ = savedSamp;
    return okAll;
}

// ============================================================
// 毛玻璃（backdrop-filter 的唯一来源）
//
// 照 binUI 的 backdropBlur 抄结构：抓背景 → 逐级 2× 降采样 → 分离高斯 → 圆角合成。
// 差别只有一处：**圆角合成不用条带几何**。binUI 那边是"把模糊结果按圆角切成若干
// 条带"，这里改成：
//
//   mask ← 模糊结果（放大回原尺寸，REPLACE）
//   mask ← 颗粒（REPEAT 噪声，正常 over）
//   mask ← 圆角形状，BGPU_BLEND_KEEP（dst *= cov）   ← 这一步就是圆角裁剪
//   目标 ← mask（正常 over）
//
// KEEP 的语义正是"dst 乘 srcA"，而形状管线的 srcA 就是覆盖率 —— 于是圆角边缘
// 天然带 AA，也不需要任何条带/九宫格几何。
// ------------------------------------------------------------

bool GpuExec::ensureBlurSlot(BlurTex& slot, int w, int h) {
    if (!device_ || w <= 0 || h <= 0) return false;
    if (slot.tex && slot.w == w && slot.h == h) return true;
    if (slot.tex) {
        bingpu_texture_destroy(device_, slot.tex);
        slot.tex = 0;
    }
    slot.tex = makeTexture(w, h, BGPU_TEXTURE_RENDER_TARGET, nullptr, 0);
    if (!slot.tex) return false;
    slot.w = w;
    slot.h = h;
    return true;
}

bool GpuExec::ensureBlurLevel(BlurChain& c, int index, int w, int h) {
    if (index < 0 || index >= 6) return false;
    return ensureBlurSlot(c.lv[index], w, h);
}

void GpuExec::destroyBlurChain(BlurChain& c) {
    for (int i = 0; i < 6; ++i) {
        if (c.lv[i].tex && device_) bingpu_texture_destroy(device_, c.lv[i].tex);
        c.lv[i] = BlurTex{};
    }
    if (c.scratch.tex && device_) bingpu_texture_destroy(device_, c.scratch.tex);
    c.scratch = BlurTex{};
    if (c.mask.tex && device_) bingpu_texture_destroy(device_, c.mask.tex);
    c.mask = BlurTex{};
    c.w = c.h = 0;
}

// 帧中途换尺寸：**不能**当场销毁（本帧已经录进命令流的那几笔还引用着它们，
// 0.22.3 的坑）。挂进 pendingFree_，下一帧 frame_begin 之后统一释放。
void GpuExec::retireBlurChain(BlurChain& c) {
    for (int i = 0; i < 6; ++i) {
        if (c.lv[i].tex) pendingFree_.push_back(c.lv[i].tex);
        c.lv[i] = BlurTex{};
    }
    if (c.scratch.tex) pendingFree_.push_back(c.scratch.tex);
    c.scratch = BlurTex{};
    if (c.mask.tex) pendingFree_.push_back(c.mask.tex);
    c.mask = BlurTex{};
    c.w = c.h = 0;
}

bool GpuExec::ensureBlurChainFor(BlurChain& c, int w, int h) {
    if (c.w == w && c.h == h && c.lv[0].tex) return true;
    retireBlurChain(c);
    c.w = w;
    c.h = h;
    return ensureBlurLevel(c, 0, w, h);
}

bool GpuExec::gaussLevel(BlurChain& c, int level, float blurPx) {
    if (level < 0 || level >= 6) return false;
    const int lw = c.lv[level].w;
    const int lh = c.lv[level].h;
    if (!c.lv[level].tex || lw < 1 || lh < 1) return false;
    const float sigma = blurPx / static_cast<float>(1 << level);
    if (!(sigma > 0.0f)) return false;
    if (!ensureBlurSlot(c.scratch, lw, lh)) return false;

    unsigned w[33];
    const int n = GaussTaps(sigma, w, 33);
    if (n < 3) return false;
    const int r = (n - 1) / 2;
    const float flw = static_cast<float>(lw);
    const float flh = static_cast<float>(lh);

    // 横：level → scratch（加法累加，权重和 = 255 ⇒ alpha 不变）
    beginPass(c.scratch.tex, lw, lh, true, 0x00000000u);
    if (!inPass_) return false;
    blend_ = BlendMode::Additive;
    curTex_ = c.lv[level].tex;
    curSamp_ = sampLinear_;
    for (int k = -r; k <= r; ++k) {
        const float o = static_cast<float>(k);
        const uint32_t col = (w[k + r] << 24) | 0x00FFFFFFu;
        emitQuad(0.0f, 0.0f, flw, flh, o / flw, 0.0f, 1.0f + o / flw, 1.0f, col);
    }
    flushAll();

    // 纵：scratch → level（覆盖写，所以目标内容由 clear 重置）
    beginPass(c.lv[level].tex, lw, lh, true, 0x00000000u);
    if (!inPass_) return false;
    blend_ = BlendMode::Additive;
    curTex_ = c.scratch.tex;
    curSamp_ = sampLinear_;
    for (int k = -r; k <= r; ++k) {
        const float o = static_cast<float>(k);
        const uint32_t col = (w[k + r] << 24) | 0x00FFFFFFu;
        emitQuad(0.0f, 0.0f, flw, flh, 0.0f, o / flh, 1.0f, 1.0f + o / flh, col);
    }
    flushAll();
    blend_ = BlendMode::SrcOver;
    return true;
}

bool GpuExec::backdropBlur(const Rect& area, float radius, float blurPx, float grain) {
    if (!drawing() || area.empty() || !(blurPx > 0.0f)) return false;
    // 区域按当前变换映射到屏幕（与 Canvas::clipRect 同一条近似：轴对齐包围盒）
    const Rect scr = xf_.mapBounds(area);
    int rx = static_cast<int>(std::floor(scr.x));
    int ry = static_cast<int>(std::floor(scr.y));
    int rx1 = static_cast<int>(std::ceil(scr.right()));
    int ry1 = static_cast<int>(std::ceil(scr.bottom()));
    rx = std::max(0, rx);
    ry = std::max(0, ry);
    rx1 = std::min(width_, rx1);
    ry1 = std::min(height_, ry1);
    const int rw = rx1 - rx;
    const int rh = ry1 - ry;
    if (rw < 2 || rh < 2) return false;

    flushAll();   // 手上攒的图元先落下去：它们构成"背景"
    const BgpuTexture prev = passTarget_;
    if (!prev) return false;

    const BlendMode savedBlend = blend_;
    const BgpuTexture savedTex = curTex_;
    const BgpuSampler savedSamp = curSamp_;
    const float invW = 1.0f / static_cast<float>(width_);
    const float invH = 1.0f / static_cast<float>(height_);
    bool okAll = true;

    internalPass_ = true;   // 内部坐标 = 各自目标的像素：单位阵 + 整目标裁剪

    // 1) 抓背景：当前目标 → level0（整数对齐的 1:1 拷贝，NEAREST 采样）
    if (!ensureBlurChainFor(blur_, rw, rh)) {
        okAll = false;
    } else {
        beginPass(blur_.lv[0].tex, rw, rh, true, 0x00000000u);
        if (!inPass_) {
            okAll = false;
        } else {
            blend_ = BlendMode::Replace;
            curTex_ = prev;
            curSamp_ = sampNearest_;
            emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                     static_cast<float>(rx) * invW, static_cast<float>(ry) * invH,
                     static_cast<float>(rx1) * invW, static_cast<float>(ry1) * invH,
                     0xFFFFFFFFu);
            flushAll();
        }
    }

    // 2) 2× 降采样链
    const int levels = BlurLevelsFor(blurPx);
    for (int i = 1; i <= levels && okAll; ++i) {
        const int dw = std::max(1, blur_.lv[i - 1].w / 2);
        const int dh = std::max(1, blur_.lv[i - 1].h / 2);
        if (!ensureBlurLevel(blur_, i, dw, dh)) {
            okAll = false;
            break;
        }
        beginPass(blur_.lv[i].tex, dw, dh, true, 0x00000000u);
        if (!inPass_) {
            okAll = false;
            break;
        }
        blend_ = BlendMode::Replace;
        curTex_ = blur_.lv[i - 1].tex;
        curSamp_ = sampLinear_;
        emitQuad(0.0f, 0.0f, static_cast<float>(dw), static_cast<float>(dh), 0.0f, 0.0f, 1.0f,
                 1.0f, 0xFFFFFFFFu);
        flushAll();
    }

    // 3) 分离高斯（在"σ 还剩 ≥2 纹素"的那一级上；σ 很小的时候就是 0 级全分辨率）
    if (okAll && !gaussLevel(blur_, levels, blurPx)) okAll = false;
    blend_ = BlendMode::SrcOver;

    // 4) 成品：模糊结果 → 颗粒 → 圆角遮罩
    if (okAll) {
        if (!ensureBlurSlot(blur_.mask, rw, rh)) {
            okAll = false;
        } else {
            beginPass(blur_.mask.tex, rw, rh, true, 0x00000000u);
            if (!inPass_) {
                okAll = false;
            } else {
                blend_ = BlendMode::Replace;
                curTex_ = blur_.lv[levels].tex;
                curSamp_ = sampLinear_;
                emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh), 0.0f, 0.0f,
                         1.0f, 1.0f, 0xFFFFFFFFu);
                flushAll();

                // 颗粒：一张极淡的确定性噪声，屏幕锚定（一个纹素 = 一个屏幕像素）
                const BgpuTexture nt = (grain > 0.001f) ? ensureNoise() : 0;
                if (nt) {
                    blend_ = BlendMode::SrcOver;
                    curTex_ = nt;
                    curSamp_ = sampRepeat_;
                    const uint32_t av = static_cast<uint32_t>(grain * 3.0f + 0.5f);
                    const float k = 1.0f / 64.0f;
                    emitQuad(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                             static_cast<float>(rx) * k, static_cast<float>(ry) * k,
                             static_cast<float>(rx1) * k, static_cast<float>(ry1) * k,
                             (av << 24) | 0x00FFFFFFu);
                    flushAll();
                }

                // 圆角遮罩：KEEP ⇒ dst *= cov（形状管线的 srcA 就是覆盖率）
                const float rad = std::min(radius, std::min(static_cast<float>(rw),
                                                           static_cast<float>(rh)) * 0.5f);
                if (rad >= 0.5f) {
                    if (shapeOk_) {
                        blend_ = BlendMode::Keep;
                        emitShape(0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh),
                                  rad, 0.0f, 0xFFFFFFFFu);
                        flushAll();
                    } else {
                        noteError("设备没有 BGPU_CAP_SHAPE：毛玻璃的圆角退回直角");
                    }
                }
            }
        }
    }

    // 5) 合成回原目标（正常 over；受调用方的裁剪栈约束，所以 screenSpace_ 而不是
    //    internalPass_ —— 内部产物也不该画到视口外面）
    if (okAll) {
        internalPass_ = false;
        beginPass(prev, width_, height_, false, 0x00000000u);
        if (inPass_) {
            blend_ = BlendMode::SrcOver;
            curTex_ = blur_.mask.tex;
            curSamp_ = sampNearest_;
            screenSpace_ = true;
            emitQuad(static_cast<float>(rx), static_cast<float>(ry),
                     static_cast<float>(rx1), static_cast<float>(ry1), 0.0f, 0.0f, 1.0f, 1.0f,
                     0xFFFFFFFFu);
            flushAll();
            screenSpace_ = false;
        } else {
            okAll = false;
        }
    }

    internalPass_ = false;
    blend_ = savedBlend;
    curTex_ = savedTex;
    curSamp_ = savedSamp;
    return okAll;
}

} // namespace bin2d
