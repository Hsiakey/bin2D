# bin2d

**C++17 的 2D 绘制核**：路径光栅化 + 覆盖率抗锯齿 + 变换/裁剪/图层/混合模式 + 场景图。
纯 CPU、无第三方依赖、输出预乘 RGBA8 位图；可选出图（自带极小 PNG 编码器）与
binGPU 桥（GPU 往返验证）。

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
                 image.h gpu.h                                 素材 / binGPU 桥
src/             raster.cpp canvas.cpp scene.cpp gpu.cpp      实现
tests/           test_geometry/raster/composite/scene/png[_gpu_bridge].cpp
tools/           bin2d_demo.cpp                               能力展示图
docs/            architecture.md api.md roadmap.md
```

## 构建

MinGW / GCC / Clang，`-std=c++17`：

```sh
make all       # 库 + 测试 + 演示（零警告）
make test      # 跑全部测试，失败非 0 退出
make demo      # 出图：build-mingw/out/bin2d-demo.png
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

**0.1.0（2026-11-15）**：地基完成。94 项断言全绿；GPU 桥在 binGPU 的 CPU 后端上往返
最大通道差 0。下一步是把上层 UI 库的路径渲染迁到 bin2d（验收 = 三端像素基准 0 diff），
见 [docs/roadmap.md](docs/roadmap.md)。

![能力展示](docs/img/bin2d-demo.png)
