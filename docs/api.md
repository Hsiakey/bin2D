# API

> 全部在 `namespace bin2d`，头文件在 `include/bin2d/`。除 `src/*.cpp` 里的实现外都是 header-only
> 可见的类型；库只需链接 `libbin2d.a`。

## 覆盖率查询（`RasterizeCoverage`，0.5.0）

```cpp
bin2d::CoverageRequest req;
req.xy = xy;                 // 交错 float：x0,y0,x1,y1,…
req.first = first;           // 每条折线的起始点下标
req.count = count;           // 每条折线的点数（首尾自动闭合 = 填充语义）
req.contours = contours;
req.rule = bin2d::FillRule::NonZero;              // 或 EvenOdd
req.style = bin2d::CoverageStyle::Fractional;     // 或 HardEdge（crispEdges）
req.subSamples = bin2d::kSubSamples;              // 默认 8；要和自己老管线逐位对齐时才改
req.clip = bin2d::Rect(0, 0, w, h);               // 设备像素
bin2d::CoverageRect cov;
if (bin2d::RasterizeCoverage(req, cov)) { /* cov.at(x, y) ∈ [0,255] */ }
```

什么时候用它（而不是 `FillPath`）：**宿主不往 CPU 的 Surface 上落像素**，而是把覆盖率
拼成条带或四边形下发给 GPU。这时候要的是"覆盖率"这个中间量，不是混好的像素。
binUI 的 `<path>` 填充就是这种（AA 时逐行把等覆盖率的连续像素合并成一条带）。

为什么不能用 `FillPath` 再读 alpha 顶替：那要求宿主开一块 Surface、走一遍混合、再反解
alpha —— 既多一次全区域读写，又把"颜料"和"覆盖率"耦合在一起。

数值口径与 `FillPath` 完全一致（同一个光栅器）：8bit 是 `round(cov × 255)`，内部整像素
覆盖率恰好 1.0 → 直接 255。

## 描边覆盖率（`RasterizeStrokeCoverage`，0.6.0）

```cpp
bin2d::StrokeCoverageRequest req;
req.xy = xy;                 // 交错 float（与覆盖率查询同一布局）
req.first = first;
req.count = count;
req.closed = closed;         // 每条子路径是否闭合（可为 nullptr = 全开放）
req.contours = contours;
req.style.width = 4.0f;      // 目标空间（设备像素）的线宽
req.style.roundCap = false;  // 端帽：butt / round / square（squareCap 优先）
req.style.roundJoin = true;  // 连接：round / miter（超 miterLimit 退化成 bevel）
req.style.miterLimit = 4.0f; // SVG 默认 4
req.clip = bin2d::Rect(0, 0, w, h);
bin2d::CoverageRect cov;
if (bin2d::RasterizeStrokeCoverage(req, cov)) { /* cov.at(x, y) ∈ [0,255] */ }
```

语义与 `StrokePath` **完全同源**：同一份 `BuildStrokePolys`（每段一个四边形、每个拐点一个
连接块、开放两端加端帽），然后逐块栅格化、在同一个像素上**取 max**。

为什么必须"取 max"而不是"把多块丢进同一个边表一次算完"，或者"逐段各自混合"：

| 做法 | 后果 |
|---|---|
| 逐段各自混合（老 binUI） | 接缝处覆盖率各算一次、混两次 → 半透明描边的每个接缝深一条 |
| 多块同一 nonzero 边表 | 方向相反的四边形互相抵消 → 拐点开洞 |
| 逐块取 max（这里） | 接缝取大者、只混一次 = 并集语义 |

`width` 是**目标空间**线宽，不做变换缩放（宿主有变换时自己先折进去）；覆盖率风格固定
`Fractional` —— 硬边描边没有意义（细长小面四舍五入等于丢形）。`subSamples` 默认
`kSubSamples`，binUI 传 4 以与它自己的填充口径一致。

## 圆角模板覆盖率（`RasterizeRoundedTileCoverage`，0.7.0）

```cpp
const int t = bin2d::RoundedTileSize(radius);   // max(8, 2r+4)
bin2d::CoverageRect cov;
if (bin2d::RasterizeRoundedTileCoverage(radius, /*stroke=*/false, cov)) {
    // cov.w == cov.h == t；cov.v[i] 是 0..255 的覆盖率（行优先）
    // 宿主的用法：把它当灰度纹理，按 9 宫格把四角各裁 t/2、中段取正中 1px 拉伸
}
```

九宫格角片用的"圆角方块"位图。**有符号距离的解析式**，不是多边形光栅化 ——
想要一张"折线圆弧 + 子采样"的覆盖率请走 `RasterizeCoverage`，那是另一张图。

口径：

| 参数 | 含义 |
|---|---|
| 形状范围 | 铺满整张模板 `[0,t]`（直边正好落在模板边界上），于是圆弧包围盒 = `[0,r]` |
| `stroke=false` | 内部：边界处 0.5、往里 0.5px 满覆盖 → 1px 宽的抗锯齿过渡 |
| `stroke=true` | 贴着外侧边界往里的 **1px 实心带**：`min(0.5-dist, dist+1.5)`。骑在边界上画会让靠内半个像素只剩 50%，白色焦点环会变成灰线 |

## core.h —— 基础类型

```cpp
Rect r(10, 10, 100, 60);
Rect i = Rect::intersect(a, b);   // 无交集 → empty()
Rect s = r.snappedOut();          // 光栅化前向外取整，避免半像素缝

Color c = Color::argb(0x803E8BFF);   // 0xAARRGGBB 直通输入 → 内部预乘
Color w = Color::white();
c = c * 0.5f;                        // 预乘空间里乘不透明度 = 纯乘法
uint32_t argb = c.toArgbStraight();  // 导出/上传前反预乘
```

## transform.h —— 2×3 仿射（SVG `matrix(a b c d e f)`）

```cpp
Transform xf = Transform::translate(100, 50) * Transform::rotateDeg(30) * Transform::scale(2, 2);
Point p = xf.map(10, 10);
Transform inv; xf.invert(inv);            // 命中测试 / 反算局部坐标
Rect b = xf.mapBounds(Rect(0, 0, 10, 10));
float xf6[6]; xf.toBingpu(xf6);           // 交给 bingpu_cmd_set_affine
```

> `operator*` 是"this ∘ rhs"：**先应用 rhs**。所以 `T * R` = 先旋转再平移。

## path.h —— 路径

```cpp
Path p;
p.moveTo(10, 10).lineTo(90, 10).quadTo(90, 90, 10, 90).close();
p.addRect(Rect(0, 0, 40, 20));
p.addRoundedRect(Rect(0, 0, 120, 40), 12);   // 半径自动夹到 min(w,h)/2
p.addCircle(50, 50, 30);  p.addEllipse(50, 50, 30, 20);
p.addStar(50, 50, 40, 16, 5);                // 演示/图表用的星形
p.transform(xf);

Rect b = p.bounds();                          // 精确包围盒（曲线取导数零点）
std::vector<Polyline> polys = p.flatten(0.25f);  // 0.25 = 最大弦高误差（用户单位）
bool hit = p.contains(x, y, FillRule::EvenOdd);
```

## paint.h —— 颜料 / 圆角 / 投影

```cpp
Paint solid  = Paint::solid(0xFF3E8BFF);
Paint grad   = Paint::linearAngle(Rect(0, 0, 100, 60), 30.0f,
                                  {GradientStop(0.0f, 0xFF3E8BFF),
                                   GradientStop(0.55f, 0xFF2FBF9A),
                                   GradientStop(1.0f, 0xFFF2B544)});
Paint byPts  = Paint::linear(Point{0, 0}, Point{100, 100}, 0xFFFF0000, 0xFF0000FF);
Paint image  = Paint::withImage(img, Rect(0, 0, 64, 64));

Paint faded = FadePaint(grad, 0.5f);          // 单个形状折 alpha（组透明度要走图层）

grad.dither = false;   // 关掉有序抖动（0.4.0 起默认开；关掉后渐变像素与 0.3.0 逐位相同）

Shadow sh;
sh.dx = 4; sh.dy = 10; sh.blur = 12; sh.color = Color::argb(0x77000000);
```

> 渐变的抖动只在"每个色阶 ≥ 3 设备像素"时生效（色带肉眼可见的场合）：8×8 Bayer、
> 量化前 ±0.5 级、图样钉在**设备像素**上。它把低 alpha 渐变的可见级数从 9 提到 17、
> 色带宽度从 32 px 压到 12 px；代价是渐变填充约 +9% 时间。

## raster.h —— 光栅核心（一般由 Canvas 调）

```cpp
RasterTarget t;
t.surface = &surface;   t.clip = surface.bounds();
t.mask = nullptr;       t.blend = BlendMode::SrcOver;

FillPath(t, path, FillRule::NonZero, paint, xf);
StrokePath(t, path, paint, StrokeStyle{ .width = 8.0f, .roundCap = true }, xf);
FillRoundedRect(t, Rect(0, 0, 120, 40), 12.0f, Paint::solid(0xFFFFFFFF), xf);
FillRect(t, Rect(0, 0, 10, 10), paint, xf);
DrawImage(t, img, Rect(0, 0, 64, 64), Rect(0, 0, 1, 1), Color::white(), false, xf);
DrawNineSlice(t, img, Rect(0, 0, 300, 90), 20, 20, 20, 20, Color::white(), false, xf);
Blur(surface, 8.0f);        // 两次可分离盒式模糊（RGBA）
BlurMask(mask, w, h, 8.0f); // 同一套盒式，但只模糊 1 字节/像素的覆盖率（投影用）
BlendCoverageMask(t, mask, w, h, dstX, dstY, Color::argb(0x77000000));  // 覆盖率 × 颜色，按 t 的 clip/mask/blend 合成
```

`RasterTarget::mask`：设备空间覆盖率图（长 = w×h），路径裁剪就是往这里塞一张覆盖率。

## canvas.h —— 即时模式

```cpp
Surface target(660, 470);
Canvas c(target);
c.clear(Color::argb(0xFF10141A));

c.save();
c.translate(100, 40);
c.rotateDeg(-8);
c.fillRounded(Rect(0, 0, 200, 80), 16, Paint::linearAngle(Rect(0, 0, 200, 80), 20, 0xFF3E8BFF, 0xFF2FBF9A));
c.restore();

c.clipRect(Rect(10, 10, 200, 120));      // 矩形裁剪（求交）
Path clip; clip.addCircle(200, 200, 60);
c.clipPath(clip);                         // 路径裁剪（覆盖率，边缘有 AA）

c.setBlendMode(BlendMode::Additive);
c.fillRect(Rect(0, 0, 10, 10), Color::white());
c.setBlendMode(BlendMode::SrcOver);

if (c.pushLayer(Rect(20, 20, 200, 200), 0.6f)) {   // 组透明度：离屏 + 整体合成
    c.fillRect(Rect(20, 20, 120, 120), Color::white());
    c.fillRect(Rect(80, 80, 120, 120), Color::white());
    c.popLayer();                                   // 重叠区不会叠深
}

c.drawNineSliceAuto(img, Rect(0, 0, 300, 90));   // 0.8.0：边界自动探测（探不到就退回普通拉伸）
c.blendCoverageMask(mask, w, h, x, y, color);    // 设备空间：覆盖率罩合成（投影的第二半）
```

混合模式与 bingpu 一一对应：`SrcOver`（预乘 over）、`Replace`、`Erase`、`Keep`、`Additive`。

## scene.h —— 保留模式

```cpp
Scene scene(660, 470);
scene.setBackground(Color::argb(0xFF10141A));

auto card = std::make_unique<Shape>();
card->path.addRoundedRect(Rect(24, 24, 300, 148), 18);
card->paint = Paint::linearAngle(Rect(24, 24, 300, 148), 35, 0xFF3E8BFF, 0xFFF2B544);
card->shadow = Shadow{4, 10, 12, Color::argb(0x77000000)};
scene.root().add(std::move(card));

auto group = std::make_unique<Group>();
group->opacity = 0.6f;
group->transform = Transform::rotateDeg(10);
// ... group->add(std::move(child)); ...
scene.root().add(std::move(group));

Surface out(660, 470);
scene.draw(out);            // 与 Canvas 直接画逐像素相同（tests/test_scene.cpp 保证）
uint64_t v = scene.version();   // 任何改动都要 scene.touch()；缓存阶段的判断依据

// 0.9.0：按组缓存。命中时贴上一帧的位图，像素与 draw() 逐像素相同
SceneCache cache;
cache.byteBudget = 32u << 20;   // 缓存位图总量上限（0 = 不限制；超了按最久未用淘汰）
cache.partialRepaint = true;    // 组内脏矩形（默认开；false = 0.9.0 的整组重画）
cache.partialMaxRatio = 0.55f;  // 脏区超过整组这个比例就退回整组重画
scene.drawCached(out, cache);
cache.stats();              // groups_total / hits / misses / partials / entries / bytes
                            // / evicted / partial_pixels / full_pixels
item->touch();              // 改完 Shape 的 path/paint/stroke 后必须调（框架字段不用）
```

> **缓存契约**：框架字段（transform / opacity / blend / clip / shadow / 子节点表）自动参与
> 内容哈希；Shape 的 `path` / `paint` / `stroke` 是不可哈希的公开字段，改完必须 `touch()`。
> 只缓存 `opacity=1`、`blend=SrcOver`、无裁剪/投影、≥2 个子节点、设备面积 ≥
> `SceneCache::minGroupArea`（默认 4096 px²）的组；其余走原路径。

## image.h —— 位图素材

```cpp
Image img = Image::generate(64, 64, [](int x, int y) { return Color::straight(r,g,b,a); });
Color texel = img.sample(u, v, ImageFilter::Linear);   // UV 0..1，CLAMP
```

缩小时的预滤波（0.4.0）：

```cpp
// "每个设备像素盖几个源纹素"：>1 就换用盒式预缩小过的副本，取样仍是双线性
const Surface& src = img.sampleSurface(texelsPerPixel);
img.mipLevelCount();   // 已经生成了几级（0 = 还没用到缩小）
```

`drawImage` / `DrawImage` 自动做这件事，调用方一般不用管；`nearest` 不参与
（最近邻的语义是"给我那一个纹素"）。副本惰性生成，`img.surface()` 非 const 取用时作废。

mip 的显存上限（0.8.0，两个口径并用，哪个先到停哪个）：

```cpp
img.setMipLevelCap(4);        // 最多几级（0 = 不限制）
img.setMipByteBudget(256*1024);  // 预缩小副本总字节上限（0 = 不限制）
size_t used = img.mipBytes();    // 当前占了多少
img.clearMips();
```

九宫格边界探测（0.8.0）：

```cpp
NineSliceInsets ins = DetectNineSliceInsets(img);   // 相邻列/行逐像素相等 = 可拉伸
if (ins.detected) c.drawNineSlice(img, dst, ins.l, ins.t, ins.r, ins.b);
```

## filter.h —— 颜色矩阵滤镜（0.8.0）

```cpp
Surface s = ...;
ApplyColorMatrix(s, ColorMatrix::grayscale(1.0f));
ApplyColorMatrix(s, ColorMatrix::hueRotate(30.0f));
ApplyColorMatrix(s, ColorMatrix::saturate(1.6f));
ApplyColorMatrix(s, ColorMatrix::brightness(0.8f));
ApplyColorMatrix(s, ColorMatrix::contrast(1.3f));
ApplyColorMatrix(s, ColorMatrix::invert(0.5f));

// 组合：先把降饱和折进去，再一次性作用（省一遍反预乘/预乘）
ColorMatrix m = ColorMatrix::concat(ColorMatrix::contrast(1.4f),
                                    ColorMatrix::saturate(0.4f));
ApplyColorMatrix(s, m);
```

口径：矩阵作用在**直通**（非预乘）RGBA 0..1 上（系数与 CSS/SVG 同一套），
identity 逐位不变；半透明像素 1 级精度；六个滤镜都不动 alpha，结果始终满足预乘约束。
1 MP 单张约 **5.8 ms**（5.83 ns/像素）。

## gpu.h —— binGPU 桥（可选）

```cpp
GpuBridge gpu;
if (gpu.create(w, h)) {          // HEADLESS + CPU 后端（不需要窗口）
    Surface out;
    gpu.render(src, out);        // 上传 → REPLACE 画四边形 → frame_read → 预乘
}
bool ok = RenderViaGpu(src, out, &err);   // 一次性往返
```

> 桥只 include `bingpu.h`（C 头），不 include binGPU 源码；`libbingpu.a` 不存在时
> 这一层不进库（`make gpu-syntax` 仍做语法检查）。

### 多图层合成器（0.3.0）

```cpp
GpuCompositor comp;
if (!comp.create(1280, 720, BGPU_BACKEND_AUTO)) return;   // 不可用 → 退回 CPU 合成

// ① 纹理池：静态图层只传一次，之后一直躺在显存里
BgpuTexture bg = comp.acquire(1280, 720);
comp.upload(bg, backdrop);                 // 预乘 Surface → 直通上传（后端再预乘）
comp.upload(card, cardSurface, dirtyRect); // 只传脏矩形

// ② 一帧 N 层：一次 frame_begin/frame_end + 一次回读
GpuCompositor::Layer layers[2] = {};
layers[0] = {bg,   Rect(0, 0, 1280, 720), Rect(0,0,1,1), Color::white(),          BlendMode::Replace,   ImageFilter::Linear};
layers[1] = {card, Rect(40, 40, 320, 240), Rect(0,0,1,1), Color::white() * 0.6f,  BlendMode::SrcOver,  ImageFilter::Linear};
Surface out;
comp.compose(layers, 2, out);

// ③ 不想每帧回读？合成到离屏目标，下一层直接当纹理用（图层套图层不落 CPU 内存）
BgpuTexture group = comp.acquireTarget(1280, 720);
comp.composeTo(layers, 2, group);
comp.waitIdle();                    // 基准/截图前必须调；正常渲染别调
comp.readTarget(group, out);        // 真要 CPU 侧像素时

comp.stats();                       // {layers, draws, quads, textureBinds}：合批有没有生效
comp.recycle(bg); comp.recycle(card); comp.recycle(group);
```

语义（与 CPU 逐像素对齐的唯一定义）：图层目标矩形内**每个**像素都参与混合，
`src = 纹理采样（预乘）× tint（预乘）`，再按 `blend` 与底色合成 —— 不像 `Surface::blit`
那样跳过 alpha==0 的源像素（`Replace`/`Keep` 下"跳过"和"写透明"不等价）。

`ToBgpuBlend(mode)` 是两侧混合模式的唯一映射点：**枚举值顺序不同**（bin2d 的
`Erase=2/Keep=3`，bingpu 是 `KEEP=2/ERASE=3`），不要强转。
