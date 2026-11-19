# bin2d

**C++17 的 2D 绘制核**：路径光栅化 + 覆盖率抗锯齿 + 变换/裁剪/图层/混合模式 + 场景图。
纯 CPU、无第三方依赖、输出预乘 RGBA8 位图；可选出图（自带极小 PNG 编码器）与
binGPU 桥（往返验证 + **多图层 GPU 合成**）。

## 是什么

- **一层"画东西"的库**：给它 `Path` + `Paint`（纯色/线性渐变/位图），它给你像素。
- 两套用法共用一个光栅核心，且**结果逐像素相同**（`tests/test_scene.cpp` 保证）：
  - `Canvas` 即时模式：变换栈 / 裁剪栈（矩形 + 路径覆盖率）/ 图层栈（组透明度）/ 5 种混合模式；
  - `Scene` 保留模式：`Group`（transform/opacity/blend/clip）+ `Shape`（路径或图片）+ 投影。
- **抗锯齿是"覆盖率"而不是模糊**：先算每像素覆盖率再乘颜料，边缘不渗色、整像素处精确为 1。
- **图层是真的离屏**：组透明度把子树画进一张位图再整体合成，重叠区不会叠深。
- **保留模式可以按组缓存，并且组内只重画脏区**（0.9.0 / 0.10.0）：内容没变的组直接贴上一帧的位图
  （10.45 → **1.28 ms，8.2×**）；组里只有一小块变了时只重画那一块（10 张卡片在动：3.76 → **1.65 ms**）。

## 不是什么

- 不认识"节点 / 布局 / 样式表 / 事件 / 窗口"——那是上层 UI 库的事，`make layers` 会守住这条边界；
- 不排版文字（字形图集与整形属于上层库），只画它给的位图/形状；
- 不做 3D（见 bin3d）、不做动画时间轴（调用方每帧改参数重画即可）；
- 缓存粒度是组：**组内脏区**能只重画那一块（0.10.0），但"跨组"的脏区合并、以及"缓存命中率进 UI 帧统计"
  还没做（见 roadmap ⑤）。

## 目录

```
include/bin2d/   core.h surface.h transform.h png.h           地基（类型/位图/矩阵/PNG）
                 paint.h path.h raster.h canvas.h scene.h      绘制
                 filter.h image.h gpu.h                        滤镜 / 素材 / binGPU 桥
src/             raster.cpp canvas.cpp scene.cpp image.cpp filter.cpp gpu.cpp   实现
tests/           test_geometry/raster/composite/scene/filter/image/png[_gpu_bridge].cpp
tools/           bin2d_demo.cpp bin2d_bench.cpp bin2d_bench_gpu.cpp bin2d_quality.cpp
docs/            architecture.md api.md roadmap.md
```

## 性能

`make bench` 跑 512×512 的六项固定场景（纯色/渐变/路径/位图/图层合成/模糊），
输出 ms、ns/px、MPix/s。0.1.1 相对 0.1.0 的完整对照与每条优化的收益见
[docs/perf.md](docs/perf.md)。

## 构建

MinGW / GCC / Clang，`-std=c++17`：

```sh
make all       # 库 + 测试 + 演示（零警告）
make test      # 跑全部测试，失败非 0 退出
make demo      # 出图：build-mingw/out/bin2d-demo.png
make bench-gpu # 图层合成基准（CPU / GPU 每帧上传 / GPU 纹理常驻；要显卡）
make quality   # 绘制质量取证：数值 + build-mingw/out/quality/*.png
make layers    # 层边界检查
make clean
```

CMake 也支持：

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

> Windows 上 `make` 的 SHELL 指向 Git for Windows 的 `sh.exe`（路径含空格）；
> Makefile 内部用 8.3 短路径固定住它，所以 `-j8` 并行不会因为命令行未加引号而失败。

## 状态

**0.15.0（2026-11-20）**：`GpuCompositor` 有**帧节拍**了 —— 策略在这一层给
（`setPacePolicy`，档位 standard/unlimited/240/120/90/60/30），闸门在 **binGPU** 算
（`bingpu_pace_*`：刷新率、present mode、present 真实耗时只有设备层知道）。
`paceFrameBegin/presentSurface/paceWaitUs` 三件套即可，外壳只负责睡到点。
建链时按策略决定 vsync（目标帧率 > 刷新率就强制关掉，否则"240 fps"是空话）。
上层可见的变化：同样一帧 CPU 光栅（9 ms）从 56 fps 变成 **93–96 fps** ——
"标准"不再把装不下的帧凑整成 60。
**0.1.1（2026-11-15）**：性能轮。六项单线程吞吐合计 **4.03×**（190.56 → 47.33 ns/px，
圆角矩形 2.51 ns/px、星形路径 1.53 ns/px、512² 图层合成 4.94 ns/px），像素结果不变；
94 项断言全绿，GPU 桥往返最大通道差 0。基线与方法见 [docs/perf.md](docs/perf.md)。下一步是把上层 UI 库的路径渲染迁到 bin2d（验收 = 三端像素基准 0 diff），
见 [docs/roadmap.md](docs/roadmap.md)；**边界与分工线**（和 binGPU / UI 库各管什么、迁移台账）
见 [docs/layering.md](docs/layering.md)。

**0.2.0（2026-11-18）**：GPU 桥可以点名后端（CPU / Vulkan / D3D11 / GL），四个后端往返
最大通道差 **0**。

**0.3.0（2026-11-18）**：多图层合成器 GpuCompositor —— 一帧一次回读、相邻图层合批
（8 层同纹理 → 1 次绘制）、纹理池 + 脏矩形上传、离屏渲染目标（图层套图层不落 CPU 内存）、
五种混合模式与 CPU 逐像素对齐（4 个后端最大通道差 1，且四端彼此完全相同）。
顺带修掉 CPU 侧两个混合模式 bug（`DrawSurface` 快路径无视 `blend`、`a==0` 一律跳过）。
合成基准见 [docs/perf.md](docs/perf.md)。

![能力展示](docs/img/bin2d-demo.png)

**0.4.0（2026-11-18）**：画质轮。① *缩小的盒式预滤波*（mip 的轻量版）：8× 缩小一张
细纹图与理想盒式平均的偏差 **31.8 级 → 0.0 级**（原来满屏摩尔纹）；② *渐变有序抖动*
（8×8 Bayer，默认开）：低 alpha 渐变的可见级数 **9 → 17**、色带宽度 **32 px → 12 px**；
③ `make quality`：把画质做成可复现的数字 + 图。125 项断言全绿；
渐变填充 +9% 时间、其余五项不动。

**0.4.2（2026-11-18）**：扫描线性能轮，**把上层 UI 库的迁移开工条件从不达标做到达标**。
覆盖率的整像素段累加改成"差分 + 行尾一次前缀和"（内部像素的 8 × 0.125 恰好是 1.0，
**像素值逐位不变**）：binUI `make gate-migrate` 上 **1.61× ❌ → 0.98× ✅**
（15.03 → 9.22 ms），`make bench` 圆角矩形纯色 2.51 → **1.29 ns/px**、
星形 1.50 → **0.79 ns/px**、六项合计 42.9 → **38.9 ns/px**。
归因与"试过没留"的两条见 [docs/perf.md](docs/perf.md)。

**0.5.0 / 0.6.0（2026-11-18/19）**：把**覆盖率**本身导出来，给"不往 CPU 像素上落、而是把
覆盖率拼成条带/四边形下发给 GPU"的宿主用 —— binUI 是第一个：0.5.0 导出填充
（`RasterizeCoverage`，binUI 0.21.0 用它换掉了自己的扫描线），0.6.0 导出描边
（`RasterizeStrokeCoverage`：每段四边形 + 拐点连接块 + 端帽逐块栅格化、同一像素取 max
= 并集，binUI 0.22.0 用它换掉了逐段 `fillLine`）。数值口径与库内的 `FillPath` / `StrokePath`
**同一个光栅器**，所以宿主的像素和这里逐像素一致（binUI 侧实测：GDI 与 GL 在描边用例上
从 1016~1284 个不同像素降到 **0**）。

**0.7.0（2026-11-19）**：导出**圆角模板覆盖率**（`RasterizeRoundedTileCoverage`）——
宿主三个 GPU 后端各自手写了一份一样的有符号距离循环，改一处就差一圈像素；现在
三端共用 bin2d 这一份（binUI 0.23.0 接入，像素一位没动）。

**0.11.0（2026-11-19）**：**窗口呈现**（`GpuCompositor::createWindow` +
`presentSurface`）—— 一张 Surface 直接送进真实窗口的交换链、不回读，这就是
"bin2d 的后端交给 binGPU"的落点。另外给 `DrawSurface` 加了**图集 1:1 快路径**：
UV 落在整纹素边界且 1:1 时，采样点在纹素中心，双线性退化成取一个纹素，字形
从 25 µs 降到几个 µs；顺带修掉 `u*(sw-1)` 在 1:1 下偏 0.4 纹素的精度问题
（binUI 侧文字用例最大通道差 171 → 79）。features 页 900×932：全屏 23.6 ms，
脏区 120×40 **3.56 ms**、900×60 **1.13 ms**。

**0.10.1（2026-11-19）**：修 `Canvas::clipRect` 的坐标系 —— 它用裸 `state().xf` 映射，
落像素却用 `deviceTransform()`（xf 再减层原点），两者在图层里差一个 origin，症状是
"层内第一次 `clipRect` 就把内容裁成 0×0"。`clipPath` 一直是按 `deviceTransform` 算的，
这次补齐。**这是 binUI 接 bin2d 后端时踩出来的**（binUI 0.26.0：`opacity` 用例
48.9% → 0% 不同像素）。新增回归 `test_composite::TestClipRectInsideLayer`。

**0.10.0（2026-11-19）**：路线图 ⑤ 收尾 —— **组内真脏矩形** + **缓存字节上限**。
脏区由"每个子节点的版本 + 设备包围盒（含描边/投影/AA）"算出，超过整组 55% 退回整组重画；
结构变化一律整组。实测：每帧动 1 张卡片 1.51 → **1.32 ms**，动 10 张 3.76 → **1.65 ms（2.3×）**，
每帧重画像素 1806 vs 16520。`test_cache` 21 → **50 项**（含"脏矩形开/关像素完全相同"）。

**0.9.0（2026-11-19）**：路线图 ⑤ 第一半 —— **版本驱动的子树缓存**（`SceneCache`）。
粒度是组，失效靠内容哈希（框架字段自动、形状靠 `touch()`）；条目按帧打戳、没再出现的自动淘汰。
1280×720 / 40 组 / 200 图元：全量 **10.34 ms** → 命中 **1.29 ms（8.0×）**，常驻 2581 KB。
命中帧与无缓存渲染逐像素相同（`tests/test_cache.cpp`，21 项）。223 项断言全绿。

**0.8.0（2026-11-19）**：收尾路线图 ④。① **颜色矩阵滤镜**（灰度/饱和/色相/亮度/对比度/反相，
5.83 ns/像素，identity 逐位不变）；② **投影改走 alpha 罩**（内存 4.0×、模糊 4.3× 更省；
并修掉"dx/dy 从来没生效过"的老 bug —— 以前投影永远贴在形状正中）；③ **九宫格边界自动探测**
（48×36 圆角 12 的卡片探出精确 12/12/12/12，3.1 µs/次）；④ **mip 显存上限**
（级数 + 字节预算，大图不再悄悄多占几十 MB）。202 项断言全绿。

## 画质（`make quality` 的固定数字）

| 指标 | 数值 | 口径 |
|---|---|---|
| 圆（r=40，半像素圆心）面积误差 | **−0.24%** | 覆盖率求和 vs πr² |
| 45° 边缘位置误差 | **0.000 px** | 水平方向解析求交 |
| 1px 线在 4 个亚像素偏移下的覆盖率总和 | **100.0**（理想 100） | 亚像素平移不改变总墨水 |
| 0.1 / 0.5 px 竖线的峰值覆盖率 | **26 / 128** | 再细的线也不会消失 |
| 8× 缩小 vs 理想盒式平均 | **0.0 级** | 0.3.0 是 31.8 级（摩尔纹） |
| 低 alpha 渐变可见级数 / 色带宽度 | **17 级 / 12 px** | 0.3.0 是 9 级 / 32 px |
| 滤镜（六个，1 MP 单张） | **5.83 ns/像素** | 灰度纯红 = 54（解析 54） |
| 投影模糊（224×144，r=12） | **31 KB / 0.11 ms** | 旧 RGBA 图层 126 KB / 0.41–0.50 ms（3.6–4.3×） |
| 九宫格自动边界（48×36 圆角 12） | **12/12/12/12**，3.1 µs | 与逐像素相等列一致 |
| 子树缓存（1280×720 / 40 组 / 200 图元） | **10.45 → 1.28 ms（8.2×）** | 命中帧与无缓存逐像素相同 |
| 组内脏矩形（10 张卡片每帧动图标） | **3.76 → 1.65 ms（2.3×）** | 重画 1/9.1 的像素，像素与整组重画相同 |
| GPU 细节场景 vs CPU | **0 / 98304 px 差** | 细网格 + 1.3px 弧 + 渐变 + 像素图标 |

对应图：[AA](docs/img/quality-aa.png)、[阶调](docs/img/quality-gradient.png)、
[缩小](docs/img/quality-minify.png)；滤镜与九宫格的新图由 `make quality` 写到
`build-mingw/out/quality/quality_filters.png` / `quality_nineslice.png`。
