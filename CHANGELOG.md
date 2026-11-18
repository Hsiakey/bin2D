# 更新日志

本文件记录 bin2d 的版本变化。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

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
