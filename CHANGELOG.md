# 更新日志

本文件记录 bin2d 的版本变化。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [0.6.0] - 2026-11-18

**新能力：描边覆盖率查询**（`bin2d::RasterizeStrokeCoverage`）。0.5.0 导出了填充的覆盖率，
这次补上描边 —— 同一批宿主（不往 CPU 像素上落、把覆盖率拼成条带/四边形下发的）终于能把
描边也从"逐段自己混合"换到"一张并集覆盖率"。

语义与 `StrokePath` **同源**：同一份 `BuildStrokePolys`（每段一个四边形、每个拐点一个连接块
= 圆点/斜接/斜面、开放两端加端帽），逐块栅格化后在同一像素上**取 max**。

为什么是「逐块取 max」而不是另外两种更"整"的做法（这是本次唯一的设计选择）：

- **逐段各自混合**（老 binUI 就是这么干的）：接缝处覆盖率各算一次、混两次 → 半透明描边的
  每个接缝深一条。这是真实存在的画质 bug，不是理论问题。
- **把多块丢进同一个 nonzero 边表一次算完**：方向相反的四边形绕向相消 → 拐点会开洞。
- **逐块取 max**：接缝取大者、每个像素只混一次 = 并集语义。代价是每块各走一次行循环，
  所以每块只用**自己**的像素矩形调行循环（成本 ∝ 块面积，不是整条路径的包围盒）。

### Added

- `StrokeCoverageRequest`（交错 float + 折线表 + `closed` 表 + `StrokeStyle` + clip +
  `subSamples`）与两个入口：请求结构版、`std::vector<Polyline>` 版。
- 内部 `BuildEdgesRing`（闭合小环 → 边表）与 `RingBounds`：描边块都是四边形/三角/圆环，
  用"整圈隐式闭合"建边比走 `Polyline` 批更直接。

### 新增测试（test_raster 30 → 43 项）

六组硬约束：整像素平头直线的导出矩形 = 线包围盒、内部 255 / 外部 0；square 端帽两端各外延
半线宽；round 端帽只多出亚像素覆盖率且不外延到半线宽之外；斜接尖角被覆盖而超 miter-limit
的斜面不被覆盖、斜面仍填满拐点内侧；**与 `StrokePath` 的覆盖率逐像素一致（实测最大差 0 级）**；
半透明描边的接缝像素正好是 50%（混两次会到 ~75%，这一条直接钉住"并集不是叠加"）；clip
决定导出块的位置与尺寸。

### 版本

- `BIN2D_VERSION_MINOR` 5 → 6（`BIN2D_VERSION_STRING` = "0.6.0"）。

---

## [0.5.0] - 2026-11-18

**新能力：覆盖率查询**（`bin2d::RasterizeCoverage`）。给"自己有条带/四边形管线"的宿主用 ——
binUI 就是第一个：它的 AA 填充是"逐行等覆盖率的连续像素合并成一条带"，要有覆盖率才能拼，
而不是要一张已经混好的像素。

导出的口径与 `FillPath` **完全相同**（同一个光栅器），不同的只是出口：

- `CoverageRect`：设备像素矩形 + `w*h` 的 8bit 覆盖率（0 = 没盖到，255 = 整像素）；
- 两个入口：`std::vector<Polyline>` 批，或**交错 float + 折线表**（宿主免拷贝，binUI 走这条）；
- `CoverageStyle::Fractional`（边缘按重叠比例，AA 用）/ `HardEdge`（跨度端点四舍五入到
  整像素，覆盖率只有 0/255 —— SVG 的 `shape-rendering="crispEdges"`）；
- `subSamples`：竖直子采样数，默认 `kSubSamples`（8）。参数化只为"宿主与自己的老管线
  逐位对齐"（binUI 迁移期传 4）；bin2d 内部所有落像素路径一律用默认值，**"像素权威"没有
  变成可调项**。

### Changed

- **光栅器拆成"建边 → 行循环（模板 sink）"两段**（`BuildEdges` / `BuildEdgesInterleaved` /
  `RasterEdges`）：落覆盖率的出口从此只有一处，合成路径（float 缓冲）与导出的覆盖率 API
  （直接写 uint8）走同一段行循环。没有这套重构，两边的边缘一定会慢慢漂。
- 行循环的临时缓冲改成**每线程复用**，但**访问一律走裸指针**：直接在热循环里写
  `rowAcc[k]` 会让每次访问都过一遍 TLS，实测慢 40%（1000 万个像素级操作）；
  函数入口取一次 `.data()` 就恢复。这条是量出来的，不是猜的。

### 新增测试（test_raster 23 → 30 项）

覆盖率的四条硬约束：整像素矩形内部 255 / 外部 0；半像素偏移的边缘列 ≈50%；
**与 FillPath 的 alpha 逐像素一致（≤1 级）**；`HardEdge` 只产生 0/255 且左边界 = round(x)；
clip 决定导出块的位置与尺寸。

### 版本

- `BIN2D_VERSION_MINOR` 4 → 5（`BIN2D_VERSION_STRING` = "0.5.0"）。

## [0.4.2] - 2026-11-18

**扫描线性能轮**：把 `binUI` 的迁移**开工条件**从不达标做到达标。口径不变（binUI
`make gate-migrate`：同一份设备空间折线、同一块 1280×720 内存 Surface、都含清屏、
都不含布局遍历）：

| 指标 | 0.4.1 | 0.4.2 |
|---|---|---|
| B bin2d `FillPath`（800×600 卡片 + 200 个 120px 图标） | 15.03 ms | **9.22 ms** |
| B / A2（binUI 条带 + 落像素） | 1.61× ❌ | **0.98×** ✅ |
| `make bench` 圆角矩形（纯色） | 2.51 ns/px | **1.29 ns/px** |
| `make bench` 星形路径填充 | 1.50 ns/px | **0.79 ns/px** |
| `make bench` 六项合计 | 42.9 ns/px | **38.9 ns/px** |

### Changed

- **覆盖率的整像素段累加改成差分**（`src/raster.cpp::RasterPolys`）：原来每条子扫描线
  对跨度的整像素段逐个 `+= subWeight`，一个 120px 宽的行要走 8×120 次加法；现在每个
  跨度只写 4 个量（两端部分像素的 `frac × subWeight`，加 `rowDelta[ia+1] += subWeight`、
  `rowDelta[ib] -= subWeight`），行尾在**本行被碰到的列区间**上跑一次前缀和还原，
  顺手清零并省掉原来"整行再 clamp 一遍"的第二趟。
- **像素值逐位不变**：内部像素的 8 × 0.125 在二进制里精确等于 1.0；`make quality` 的
  七项数字（圆面积 −0.24%、45° 边缘位置 0.000 px、抖动 9→17 级 / 32→12 px、
  8× 缩小 0.0 级、描边 100/100/100/100、GPU 细节场景 0/98304）**逐项与 0.4.1 相同**，
  125 条断言全绿。

### 记下来"试过但没留"的两条

- **跨子扫描线保持交叉点顺序**（把插入排序喂成近有序）：覆盖率 7.34 → 7.48 ms，
  没有收益 —— 真实花销是"每条子扫描线给每条活动边算一次 x"，不是排序。
- **覆盖率缓冲线程本地复用 + 脏区间清零**：`assign` 57.6 KB/图标 ≈ 0.4 ms/帧，
  而脏区间几乎等于包围盒宽度，省不掉，收益在噪声里。

### 归因（消融实验，抄一份 raster.cpp 做对照）

整像素段逐像素累加 5.07 ms（55%）、每条子扫描线重建+排序 4.62 ms（50%，此项被高估，
见上一条）、求交+建边 1.88 ms（20%）。所以 0.4.1 里"改成解析覆盖率能拿 3~5×"的预估
**没有实现**：实际拿到 1.63×，且剩下那 2× 必须改竖直方向的 AA 语义（8 次采样 →
梯形解析面积），已记为 `docs/roadmap.md` ① 的开放项，不阻塞迁移。

### 版本

- `BIN2D_VERSION_PATCH` 1 → 2（`BIN2D_VERSION_STRING` = "0.4.2"）。

## [0.4.1] - 2026-11-18

**只有文档**：把"开工条件"量出来了 —— 与 binUI 的**同口径**对拍（同一份设备空间折线、
同一块 1280×720 内存 Surface、都含清屏、都不含布局遍历）：

| 路线 | 耗时 |
|---|---|
| A1 binUI 只建条带（119 668 条/帧） | 7.30 ms |
| A2 binUI 条带 + 落像素 | 9.31 ms |
| B bin2d `FillPath` | **15.03 ms** |
| B / A2 | **1.61×**（开工条件 ≤1.20×，未达标） |

结论：**先做 bin2d 的扫描线性能轮，再迁 binUI**。根因已定位：每行跑 8 条子扫描线并对整行宽
累加覆盖率，内部像素（覆盖率恰为 1）也被乘了 8 倍 —— 120px 图标 62 µs/个，是大形状
（1.45 ns/px）的 5 倍。改法：逐行**解析覆盖率**（直边在一行内的覆盖面积是梯形解析式）
+ 只在边界像素上细算；预期 3~5×，且**质量更好**（竖直 1/8 量化消失）。
计划与回滚条件见 `binUI/docs/migrate-to-bin2d.md`，本条摘要记在 `docs/layering.md` §4。

### 版本

- `BIN2D_VERSION_PATCH` 0 → 1（`BIN2D_VERSION_STRING` = "0.4.1"）。

## [0.4.0] - 2026-11-18

**画质轮**：两处"看起来没错、量起来有问题"的地方 —— 缩小时没有预滤波（摩尔纹）、
低 alpha 渐变只有个位数色阶（色带）。两条都改成默认生效，并新增 `make quality`
把画质变成**可复现的数字 + 图**，而不是"我觉得还行"。

### Added

- **缩小的盒式预滤波**（`Image::sampleSurface`，mip 的轻量版）：`drawImage` 在
  缩小倍率 > 1 时自动换用按 2 的幂盒式缩小过的副本，取样仍是双线性。
  实测 8× 缩小一张 2px 棋盘：与理想盒式平均的平均偏差 **31.8 级 → 0.0 级**
  （原来满屏摩尔纹）。最近邻不参与（它的语义就是"给我那一个纹素"）；
  1:1 贴图逐像素精确这条**没有变**。副本惰性生成、`surface()` 非 const 取用时作废。
- **渐变有序抖动**（`Paint::dither`，默认开）：8×8 Bayer，量化前加 ±0.5 级。
  低 alpha（7.8%）的彩色渐变在 256 px 上可见级数 **9 → 17**（半级分辨率统计），
  最长同值游程（色带宽度）**32 px → 12 px**。
  抖动与否是**模板参数**（不是行内 if），并且只在"每个色阶 ≥ 3 设备像素"时才抖 ——
  鲜艳的大跨度渐变不抖，省掉 45% 的时间。
- **`tools/bin2d_quality.cpp` + `make quality`**：固定场景出固定数字与图
  （AA 面积/边缘位置、阶调级数与色带宽度、缩小与理想盒式平均的偏差、1px 描边的
  亚像素覆盖率、GPU 与 CPU 的最大通道差）。有 binGPU 时额外跑 GPU 一致性那一项。

### Changed

- 抖动开启后**渐变填充的像素与 0.3.0 不再逐位相同**（差值 ≤ 1 级，且是空间分布的）。
  要逐位复现旧结果：`Paint p = ...; p.dither = false;`。
  非渐变的绘制（纯色/位图/描边覆盖率）逐位不变（`test_raster` 的精确值断言全部保留）。

### 性能

- 渐变填充 6.32 → **6.89 ns/px**（+9%，抖动只在需要时开；全开是 9.6）。
- 六项合计 42.5 → **42.9 ns/px**（+1%），其余五项在噪声范围内。
- 缩小的位图反而更快（预滤波后每个设备像素要取的纹素更少），但基准里的位图项是 2× 放大，不受影响。

### 验收

- `tests/test_raster.cpp` 14 → **23** 项：8× 缩小的偏差与"不会整块翻转"、最近邻缩小保持硬边、
  1:1 贴图精确、抖动把色带打散/可见级数翻倍/图样周期为 8。
- `make test` = 32 + 23 + 30 + 9 + 16 + 15 = **125 项断言**，0 失败。

### 版本

- `BIN2D_VERSION_MINOR` 3 → 4（`BIN2D_VERSION_STRING` = "0.4.0"）；`CMakeLists.txt` 同步。

## [0.3.0] - 2026-11-18

**GPU 路径从"证明链路通"变成"真能合成一帧"**：一帧 N 个图层、一次回读、五种混合模式
与 CPU 逐像素对齐（4 个后端实测最大通道差 **1**，且四个后端彼此完全相同）。顺带**修掉
CPU 侧两个混合模式的 bug** —— 它们是做对齐时被抓出来的，不修就没法对齐。

### Added

- **`GpuCompositor`（`include/bin2d/gpu.h`）**：多图层合成器。
  - `compose(layers, n, out)`：**一个回合**合成 N 层（一次 `frame_begin/frame_end` +
    一次 `frame_read`），而不是每层各一个回合。
  - **合批**：相邻图层只要"纹理 + 混合模式 + 采样器"相同就并进同一次 `draw_quads`。
    `Stats{layers, draws, quads, textureBinds}` 把这件事变成**数字**：
    8 个同纹理同模式的层 → `draws == 1`；交替五种模式 → `draws == 5`。
  - **纹理池**：`acquire/upload/recycle/purgePool`，按 (宽,高,用途) 分桶复用；
    `upload(tex, surface, box)` 只传脏矩形。静态图层可以一直躺在显存里。
  - **离屏渲染目标**：`acquireTarget/composeTo/readTarget` —— 合成结果留在显存里接下一层
    （图层套图层不落 CPU 内存），`waitIdle()` 等 GPU 真的画完（基准/截图必须调）。
  - `ToBgpuBlend(BlendMode)`：bin2d 与 bingpu 的混合模式映射放在**一处**。两边枚举值
    顺序不同（bin2d 的 Erase=2/Keep=3，bingpu 是 KEEP=2/ERASE=3），直接强转会把
    "擦除"和"保留"对调 —— 第一版就是这么错的，靠五模式合成对齐测试抓到（`draws` 都对、
    像素差 154）。
- `tools/bin2d_bench_gpu.cpp` + `make bench-gpu`：1280×720 / 16 层的**三条路线**对比
  （CPU 合成 / GPU 每帧上传 / GPU 纹理常驻），以及"合到离屏目标"和"+ 每帧回读"两档。
  数字见 [docs/perf.md](docs/perf.md)。

### Fixed（CPU 侧，`src/raster.cpp`）

1. **`RasterTarget::blend` 被两条快路径无视**：`DrawSurface` 的"1:1 平移"和"轴对齐缩放"
   两条路径都把混合模式写死成 `SrcOver`（只有旋转/斜切那条用了 `t.blend`）。
   后果：`Canvas::popLayer` 的图层合成**永远**是 `SrcOver` —— 组的 `Replace/Erase/Keep/
   Additive` 一个都不生效，而且不会红任何测试（`test_composite` 那五个手算用例走的是
   `fillRect`，不是图层合成）。现在两条快路径都按混合模式分派一次（模板参数），
   `SrcOver` 逐位不变。
2. **`a == 0` 的源像素一律跳过**，但 `Replace` 要写透明（清掉目标）、`Keep` 要乘 0（也清 0）。
   判据原来散落在 `BlendPixel` 的分支里，现在统一成 `ZeroAlphaMatters(mode)` 一条，
   所有落像素路径（`BlendOne` / 三条行实现 / `DrawSurface` 三条路径）都问它。
3. **`nearest=true` 在轴对齐路径下没生效**（查表双线性照样插值）—— 放大镜/像素图会糊。

### 验收

- `tests/test_composite.cpp` 20 → **30** 项：新增"组混合模式 = 直接按该模式合成"（5 种，
  逐像素 0 差）与"全透明层在 5 种模式下该不该清目标"。
- `tests/test_gpu_bridge.cpp` 3 → **15** 项：五模式多图层合成（CPU / VK / D3D11 / GL
  各一遍）、合批次数、纹理池复用、脏矩形上传、跳过规则、渲染目标套层、`composeOne`
  与老桥一致。全部通过；不可用的后端打印原因跳过。
- `make test` = 32 + 14 + 30 + 9 + 16 + 15 = **116 项断言**，0 失败。

### 版本

- `BIN2D_VERSION_MINOR` 2 → 3（`BIN2D_VERSION_STRING` = "0.3.0"）；`CMakeLists.txt` 同步。

## [0.2.0] - 2026-11-18

**GPU 桥可以点名后端了**，并且顺手当了 binGPU 的一次真验收：同一份 `Surface` 过
CPU / Vulkan / D3D11 / GL 四个后端往返，读回来**都是它自己**（最大通道差 **0**）。

### Added

- `GpuBridge::create(width, height, BgpuBackendKind)`：点名后端（`_CPU` / `_VULKAN` /
  `_D3D11` / `_GL` / `_AUTO`）。原来只有 `preferCpu` 布尔，AUTO 落到哪个后端是黑盒；
  现在 `backend()` / `backendName()` 回报**实际**拿到的后端（AUTO 解析之后的值）。
- `tests/test_gpu_bridge.cpp` 从"只测 CPU 后端"扩成"每个可用后端各测一遍"，不可用就
  打印原因跳过（CI 机没显卡不会红）。

### Fixed（修在 binGPU 那边，这里记一笔）

这条桥上第一次跑真实后端就抓到 binGPU 两个 bug，都在"交换链回读"这条路上：
D3D11 的 HEADLESS 借出对象漏了 `res_w/res_h`（viewport 0×0，一笔画不出来，差 255）、
GL 的 FBO 回读多翻了一次行（整幅上下镜像，差 38）。已修在 **binGPU 0.5.1**，
并在 binGPU 的 `tests/gpu_parity.h` 里补了对应的 `swapchain(frame_read)` 用例。
记在这里是因为**发现它的不是 binGPU 自己的测试，是这层消费者** —— 这正是
"上层一行不改、底下换后端"这条约定该起的作用。

### 版本

- `BIN2D_VERSION_MINOR` 1 → 2（`BIN2D_VERSION_STRING` = "0.2.0"）。

## [0.1.3] - 2026-11-18

**只有文档**：订正 [`docs/layering.md`](docs/layering.md) 里的 binUI 行数（迁移台账引用的是
旧数字）——`binui_path.h` 1112 → **1183** 行、`binui_draw_nodes.h` 1788 → **1860** 行。
接口与像素行为一字未动（94 条断言、demo 图不变）。

### 版本

- `BIN2D_VERSION_PATCH` 2 → 3（`BIN2D_VERSION_STRING` = "0.1.3"）。

## [0.1.2] - 2026-11-18

**只有文档**：新增 [`docs/layering.md`](docs/layering.md)，把"bin2d 在哪一层、和 binGPU / UI 库
怎么分工"写成可执行的边界。接口与像素行为一字未动（94 条断言、demo 图不变）。

### Documented

- **位置**：四层里的第二层（binUI → bin2d → binGPU → VK/D3D11/GL/Metal），三条横向关系都是单向的；
  `gpu.h` 是可选依赖，没有 binGPU 时软件路径照跑。
- **两条一致性承诺的落点**：bin2d 对上层承诺"同一串调用 → 同样的 `Surface` 像素"（裁判是
  `make test` + `make layers` + demo 字节级不变）；对下只消费 binGPU 的 P1（命令 → 像素）。
  GPU 合成路径必须与 CPU 合成**逐像素 0 差** —— 2D 是确定性的，这条能和 bin3d 的"只能统计一致"
  明确区分开。
- **边界判据**：这段代码**认不认节点/布局/文本**？认 → 上层；不认 → bin2d。
- **与 UI 库的迁移台账**（roadmap ② 的可执行版）：`binui_path.h`（1112 行）迁完即删、
  `binui_paint.h` 与 bin2d 收敛成一份、`binui_draw_nodes.h`（1860 行）与字形图集留下；
  每步的删除条件 + 验收线（三端 0 diff、18 张 GDI 基准、单帧 ≤ 现实现 ×1.2）。
- **GPU 阶段的真前置**：binGPU 得有能跑的真后端（VK/D3D11）并通过它自己的 P1；否则"GPU 合成"
  只是跑在 CPU 后端上的链路演示（现状就是）。
- **对 binGPU ABI 2 的诉求：没有新增原语**。现有 `draw_quads` + 5 种混合 + 离屏 + 仿射已够；
  只希望补两条性能契约（句柄热路径、后端负责状态剔除），present 三态与 bin2d 无关。
- 版本 `0.1.2`（`BIN2D_VERSION_PATCH` 1 → 2）；`CMakeLists.txt` 的 `project(VERSION)` 从
  0.1.0 更正到 0.1.2（一直没跟上）。

## [0.1.1] - 2026-11-15

性能轮：**先测再改**，六项单线程吞吐合计 **4.03×**（190.56 → 47.33 ns/px），
像素结果保持不变（94 条断言一条没改，全绿）。基线与方法见 [docs/perf.md](docs/perf.md)。

### Optimized

- **扫描线**：折线先建边数组（`x(y)=x0+(y-y0)·dyInv`，每条边只算一次除法）+
  **活动边表**（按 ymin 排序、每行增删）+ 跨度拆成"左端部分/整像素段/右端部分"，
  整像素段一次加法写完；交叉点改用插入排序。纯色填充 **4.88×**、星形路径 **3.31×**。
- **合成分派**：按颜料种类（纯色/线性渐变/位图）与混合模式各分派一次，
  行内跑专用循环；纯色不再逐像素做逆矩阵映射；混合的 4 次 `/255` 换成
  `(x+128+((x+128)>>8))>>8`（已穷举验证与 `/255` 逐值相等）。线性渐变 **2.36×**。
- **位图**：1:1 平移走整数直拷（图层合成 **8.39×**）；轴对齐缩放把源索引与
  双线性权重预算成表，去掉每像素两次除法与 4 次边界钳制，内部像素覆盖率恒为 1。
  位图 2× 放大 **2.72×**。
- **模糊**：盒式模糊改成前缀和（O(1)/像素，原来是 O(r)），结果逐位相同。**4.74×**。
- **画布**：裁剪遮罩惰性分配 —— 不再在构造/`save()`/`pushLayer()` 时 memset
  一份目标尺寸的 float 遮罩（512² 就是 1 MB）。
- 新增 `make bench`（`tools/bin2d_bench.cpp`，512×512 六项场景，ms / ns/px / MPix/s）。

### Fixed

- 1:1 的 `drawSurface`/`drawImage` 不再经过双线性采样：原来会整体偏半个纹素并轻微模糊
  （图层内容也走这条路），现在按像素直接拷贝。

### 版本

- `BIN2D_VERSION_PATCH` 0 → 1（`BIN2D_VERSION_STRING` = "0.1.1"）。

## [0.1.0] - 2026-11-15

首个版本：2D 绘制核的地基。**不是** UI 库、不认布局/文本/窗口。

### 新增：光栅核心

- `raster::FillPath`：扫描线 + 8 条竖直子扫描线，水平方向解析求交。
  覆盖率误差 ≤ 1/8 像素，整像素对齐时精确为 1；`FillRule::NonZero` / `EvenOdd` 都支持。
- `raster::StrokePath`：每段一个四边形 + 圆角/尖角连接 + 平/圆/方端帽。
  所有碎块叠进同一张覆盖率（取 max）后统一混合一次，接缝不会重复混、不会出现深缝。
- `raster::FillRoundedRect / FillRect / FillLinearGradient`：轴对齐纯色走解析覆盖率快路径。
- `raster::DrawImage / DrawNineSlice`：双线性/最近邻、UV 子矩形、tint、九宫格；
  旋转/斜切时按 4×4 子采样算边缘覆盖率。
- `raster::Blur`：两次可分离盒式模糊（投影用）。

### 新增：路径与颜料

- `Path`：`moveTo/lineTo/quadTo/cubicTo/close` + `addRect/addRoundedRect/addEllipse/addCircle/addRegularPolygon/addStar`。
  命令级存储，`flatten(tolerance)` 自适应细分（容差 = 最大弦高误差）；`bounds()` 是**精确**包围盒
  （曲线取导数零点）；`contains()` 支持两种填充规则。
- `Paint`：纯色 / 线性渐变（两点或 CSS 角度 + 多档 stop）/ 位图（UV + tint）；
  `FadePaint` 把不透明度折进颜料；`Shadow` 默认透明（不画）。

### 新增：画布与场景

- `Canvas`：变换栈、裁剪栈（矩形求交 + 路径覆盖率遮罩）、图层栈（`pushLayer/popLayer`，
  组透明度靠离屏合成）、5 种混合模式（`SrcOver/Replace/Erase/Keep/Additive`，与 binGPU 的
  `BgpuBlend` 一一对应）。
- `Scene`：`Group`（transform/opacity/blend/clip）+ `Shape`（路径填充/描边）+ `ImageShape`，
  支持投影；`version()` 是缓存阶段的钩子。渲染结果与 Canvas 直接画逐像素相同。

### 新增：位图与导出

- `Surface`（预乘 RGBA8 + stride）、`Image`（素材 + 采样 + 九宫格参数）。
- `png.h`：自带 PNG 编码器（stored deflate + CRC32/Adler32），同输入逐字节可复现。
- `gpu.h`：binGPU 桥（HEADLESS CPU 设备往返，只依赖 `bingpu.h`）；缺 `libbingpu.a` 时不进库。

### 测试与工具

- 5 套测试：`test_geometry`（32 项）、`test_raster`（14 项）、`test_composite`（20 项）、
  `test_scene`（9 项）、`test_png`（16 项）；可选 `test_gpu_bridge`（3 项，需 binGPU 已构建）。
- `tools/bin2d_demo.cpp` → `build-mingw/out/bin2d-demo.png`（660×470，能力展示图）。
- `make layers`：层边界守卫 —— bin2d 里不许出现上层词汇（`Node`/`layout`/`dom`）与上层依赖。
