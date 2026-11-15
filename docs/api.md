# API

> 全部在 `namespace bin2d`，头文件在 `include/bin2d/`。除 `src/*.cpp` 里的实现外都是 header-only
> 可见的类型；库只需链接 `libbin2d.a`。

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

Shadow sh;
sh.dx = 4; sh.dy = 10; sh.blur = 12; sh.color = Color::argb(0x77000000);
```

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
Blur(surface, 8.0f);   // 两次可分离盒式模糊
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
```

## image.h —— 位图素材

```cpp
Image img = Image::generate(64, 64, [](int x, int y) { return Color::straight(r,g,b,a); });
Color texel = img.sample(u, v, ImageFilter::Linear);   // UV 0..1，CLAMP
```

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
