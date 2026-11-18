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

## 不是什么

- 不认识"节点 / 布局 / 样式表 / 事件 / 窗口"——那是上层 UI 库的事，`make layers` 会守住这条边界；
- 不排版文字（字形图集与整形属于上层库），只画它给的位图/形状；
- 不做 3D（见 bin3d）、不做动画时间轴（调用方每帧改参数重画即可）；
- 本版**不做缓存与脏区**（`Scene::version()` 已留钩子，见 roadmap 的阶段 ⑤）。

## 目录

```
include/bin2d/   core.h surface.h transform.h png.h           地基（类型/位图/矩阵/PNG）
                 paint.h path.h raster.h canvas.h scene.h      绘制
                 image.h gpu.h                                 素材 / binGPU 桥（往返 + 合成器）
src/             raster.cpp canvas.cpp scene.cpp gpu.cpp      实现
tests/           test_geometry/raster/composite/scene/png[_gpu_bridge].cpp
tools/           bin2d_demo.cpp bin2d_bench.cpp bin2d_bench_gpu.cpp  演示 / 基准
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

## 画质（`make quality` 的固定数字）

| 指标 | 数值 | 口径 |
|---|---|---|
| 圆（r=40，半像素圆心）面积误差 | **−0.24%** | 覆盖率求和 vs πr² |
| 45° 边缘位置误差 | **0.000 px** | 水平方向解析求交 |
| 1px 线在 4 个亚像素偏移下的覆盖率总和 | **100.0**（理想 100） | 亚像素平移不改变总墨水 |
| 0.1 / 0.5 px 竖线的峰值覆盖率 | **26 / 128** | 再细的线也不会消失 |
| 8× 缩小 vs 理想盒式平均 | **0.0 级** | 0.3.0 是 31.8 级（摩尔纹） |
| 低 alpha 渐变可见级数 / 色带宽度 | **17 级 / 12 px** | 0.3.0 是 9 级 / 32 px |
| GPU 细节场景 vs CPU | **0 / 98304 px 差** | 细网格 + 1.3px 弧 + 渐变 + 像素图标 |

对应图：[AA](docs/img/quality-aa.png)、[阶调](docs/img/quality-gradient.png)、
[缩小](docs/img/quality-minify.png)。
