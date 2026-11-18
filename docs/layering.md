# 分层与边界（layering）：bin2d 在哪一层，和 binGPU / UI 库怎么分工

> 写于 2026-11-18，与 [`binGPU/docs/layering.md`](../../binGPU/docs/layering.md) 同一批。
> 那篇定义"设备与呈现层"的位置；这篇定义**它上面这层**：bin2d 吃什么、吐什么、
> 哪些东西故意不收、以及和 UI 库之间那条线画在哪。
>
> 本文不改接口（当前 `BIN2D_VERSION_STRING` 之外没有任何变化）。

---

## 0. 三句话

1. bin2d = **2D 绘制核**：输入路径 / 颜料 / 位图 / 变换，输出**一张预乘 RGBA8 位图**。
   不认节点、不认布局、不认文本、不认窗口。
2. 它有两个执行器：**软件光栅（权威）** 和 **binGPU 合成（可选加速）**。
   前者是像素定义，后者必须和前者一致 —— 顺序和 binGPU 的"CPU 参考后端 vs 真后端"完全同构。
3. 它和 UI 库的分界只有一条：**"这段代码认不认节点/布局？"** 认 → 留 UI 库；不认 → 进 bin2d。

---

## 1. 位置：四层里的第二层

```
  binUI（节点 / 布局 / 文本 / 事件 / 窗口）
        │  只交出"路径 + 颜料 + 变换"，或直接交出位图
  bin2d（Canvas 即时 · Scene 保留 · raster 覆盖率 AA · 图层合成）
        │  Surface ⇄ bingpu 纹理（gpu.h，可选依赖）
  binGPU（设备 / 交换链 / 固定功能批处理 / present）
        │
  Vulkan · D3D11 · GL · (Metal)
```

横向关系（都是**单向**的）：

| 关系 | 说明 |
|---|---|
| binUI → bin2d | UI 把节点翻译成路径/图片，再调 bin2d；bin2d 不知道"按钮"是什么 |
| bin3d → bin2d | bin3d 拿 `bin2d::Surface` 当画布，拿 `bin2d::png` 出图 |
| bin2d → binGPU | 仅 `gpu.h` 这一条桥，且**是可选依赖**：没有 binGPU 时整层不进库，软件路径照跑 |

---

## 2. 两条承诺在这层的落点

[`binGPU/docs/layering.md`](../../binGPU/docs/layering.md) §2 把一致性拆成 P1/P2，这里对应成：

| 承诺 | 谁保证 | 裁判 |
|---|---|---|
| **bin2d 对上层**：同一串 `Canvas`/`Scene` 调用 → 同样的 `Surface` 像素 | bin2d（软件光栅） | `make test` 的 94 条断言 + `make layers` + `make demo`（demo 图字节级不变） |
| **bin2d 消费 binGPU 的 P1**：同一串 `BgpuVertex` → 同样像素 | binGPU | binGPU 的 CPU 参考后端 |
| **bin2d 的 GPU 路径**：同一份场景，GPU 合成 == CPU 合成 | bin2d | 两边各出一张 `Surface`，**逐像素 0 差** |

最后一条能要求 0 差，是因为 **2D 是确定性的**：整数写入、固定遍历顺序、覆盖率有解析定义。
bin3d 没这个待遇（浮点光栅顺序不同，见它的 `roadmap.md` ②）。这也是两个库验收口径的根本差异。

**推论一**：bin2d 的软件光栅**不许为了快而牺牲像素**。`perf.md` 里那轮 4.03× 的做法就是标准答案
（先测、每条优化单独可回滚、像素结果不变）。
**推论二**：GPU 合成没通过 0 差之前，它不算"实现完成"，只能算"链路通了"。

---

## 3. 边界判据：什么进 bin2d，什么留在上层

**一句话**：这段代码**认不认节点/布局/文本**？认 → 上层；不认 → bin2d。

| 属于 bin2d | 现在谁在做 |
|---|---|
| 路径存储与展平、精确包围盒 | bin2d `path.h` |
| 覆盖率 AA 填充/描边（nonzero/evenodd） | bin2d `raster` |
| 纯色/线性渐变/位图颜料、九宫格 | bin2d `paint.h` |
| 变换栈 / 裁剪栈 / 图层栈 / 5 种混合 | bin2d `canvas.h` |
| 场景图 + `version()`（缓存的挂载点） | bin2d `scene.h` |
| 位图内存与采样、PNG 编解码 | bin2d `surface.h` / `image.h` / `png.h` |

| 故意不收 | 归谁 | 为什么 |
|---|---|---|
| 布局 / flex / 节点树遍历 | binUI | 与"画"正交；binUI 的 `binui_draw_nodes.h`（1860 行）就是这条线的实现 |
| 文本排版 / 字形图集 | binUI | 字形是几千个小位图，走 UI 的图集与 hinting；bin2d 只画它给的位图 |
| 窗口 / 事件 / IME / 拖拽 | binUI 外壳 | bin2d 不碰 Win32 |
| 设备 / 交换链 / present | binGPU | 见 binGPU `layering.md` §3 |
| 3D | bin3d | 横向库，不是 bin2d 的子集 |
| 动画 / 时间轴 | 调用方 | 每帧改 transform/paint 再重画即可 |

---

## 4. 和 UI 库的迁移台账（roadmap ② 的可执行版）

binUI 现在自带一份"半成品 bin2d"：路径几何在 `include/binui_path.h`（1183 行），
变换/绘制工具在 `binui_paint.h`，节点遍历在 `binui_draw_nodes.h`（1860 行）。

| binUI 资产 | 行数 | 迁移后 |
|---|---|---|
| `include/binui_path.h`（扫描线填充 + AA + 掩码） | 1112 | **删**（填充/描边换成 bin2d `Path + raster`；只留"把节点转成路径"的那层壳） |
| `binui_paint.h`（变换与绘制工具） | — | 与 `bin2d/transform.h`、`bin2d/path.h` **收敛成一份**；收敛前两边各一份是明确的临时状态 |
| `binui_draw_nodes.h`（节点 → 原语） | 1788 | **留**（它不是几何，是"节点语义") |
| `binui_glyph_atlas.h`（字形图集） | 1169 | **留**（bin2d 只接收位图） |
| `binui_gl.*` / `binui_vk.*` / `binui_d3d11.*` 的离屏图层实现 | 三份 | 图层语义统一到 `Canvas::pushLayer/popLayer` 之后，三份可退休（后端只负责"把 Surface 或四边形刷到屏幕"） |

**每步的删除条件**（沿用 binGPU §5 的"替换不并存"）：

1. `<path>` 的条带数学换成 bin2d 之后，**旧的扫描线代码必须删掉**，不许留开关；
2. 验收线（硬指标，roadmap ② 已写）：三端像素基准 **0 diff** + 18 张 GDI 基准 + 示例无回归；
3. 性能线：UI 最大节点（800×600 卡片 + 200 个图元）单帧 ≤ 现实现 × 1.2。
   **这条是开工条件，也继续当验收条件** —— 迁移完掉到 1.2× 以上就回滚那一步，不带病进 UI。

   **2026-11-18 实测（binUI `make gate-migrate`，同口径：同一份设备空间折线、
   同一块 1280×720 内存 Surface、都含清屏、都不含布局遍历）**：

   | 路线 | 0.4.1 实测 | **0.4.2 实测** |
   |---|---|---|
   | A1 binUI 只建条带（119 668 条/帧） | 7.30 ms | 7.21 ms |
   | A2 binUI 条带 + 落像素 | 9.31 ms | 9.32–9.48 ms |
   | B bin2d `FillPath` | 15.03 ms | **9.22–9.25 ms** |
   | B / A2 | **1.61×** ❌ | **0.98×** ✅ |

   结论（0.4.1）：未达标，先做 bin2d 的扫描线性能轮，不动 binUI 的渲染代码。
   根因：每个像素行跑 8 条子扫描线并**对整行宽逐个像素累加覆盖率** —— 修完之后
   消融实验证明这一笔只占覆盖率开销的 1/3（5.07/9.28 ms），真正的大头是
   "8 条子扫描线 × 每条活动边各算一次 x"（1.9 ms/百万次）+ 排序，**不是**整像素段。
   所以 0.4.1 里"解析覆盖率能拿 3~5×"的预估没有实现，实际拿到的是 1.63×（差分累加）。
   剩下的 2× 只能靠"每行只求 2 次交（行首/行尾）+ 梯形解析面积"，那会改变竖直方向的
   AA 语义（8 次采样 → 精确面积），已记为 roadmap ① 的**开放项**，不阻塞迁移。
   计划、验收与回滚条件见 `binUI/docs/migrate-to-bin2d.md`。

---

## 5. GPU 阶段（roadmap ③）：什么时候才做，验收看什么

**2026-11-18 落地（0.3.0）**：三件工程全做完 —— 多图层批量合成（一回合 N 层 + 合批）、
纹理池（按尺寸分桶复用 + 脏矩形上传）、5 种混合全量对齐（4 个后端与 CPU 最大通道差 1，
四端彼此完全相同）。另加离屏渲染目标（`composeTo`，图层套图层不落 CPU 内存）。
数字与裁决见 [perf.md](perf.md) 与 [roadmap.md](roadmap.md) ③。

**这条路径抓到的 bug（按发现顺序，都是"功能测试能绿、像素也能对上，但语义/性能错"的类型）：**

| 在哪里 | 症状 | 谁该修 |
|---|---|---|
| binGPU D3D11 | 每帧上传 16 层（56 MB）要 **5942 ms**（VK/GL 同工作量 100 ms 量级） | binGPU 0.5.2：改走 `UpdateSubresource` → 100 ms |
| binGPU 全部真后端 | `wait_idle` 直接 `return BGPU_OK`，基准量到"命令录制时间"而非"画完时间" | binGPU 0.5.2：VK `vkDeviceWaitIdle` / D3D11 `QUERY_EVENT` / GL `glFinish` |
| bin2d 自己 | `DrawSurface` 的 1:1 与轴对齐两条快路径**无视 `RasterTarget::blend`** → 组混合模式永远退化成 `SrcOver` | bin2d 0.3.0 |
| bin2d 自己 | `a==0` 的源像素一律跳过，但 `Replace`/`Keep` 需要落笔 | bin2d 0.3.0：统一成 `ZeroAlphaMatters()` |
| bin2d 自己 | `nearest=true` 在轴对齐路径失効（放大镜会糊） | bin2d 0.3.0 |

前两条是**上层逼出来的**（binGPU 的像素测试只问"对不对"，问不出"慢一千倍"和"没在等"）；
后三条是**对齐逼出来的**（不把 CPU 的规则写成一条清晰的判据，就发现不了快路径各自为政）。

真加速的前置（原计划，留作记录）是 binGPU 那边的两件事（顺序不能反）：

1. binGPU 的 **VK 或 D3D11 后端落地** —— 否则"GPU 路径"跑在 CPU 后端上，等于没加速；
2. 该后端通过 binGPU 自己的 **P1**（命令 → 像素 0 差）—— 否则 bin2d 的 0 差验收会变成
   "对着一个不确定的底座做减法"。

开工条件的裁决（原文：每帧 ≥ 8 图层或视口 ≥ 1280×720，且 CPU 合成占帧时间 > 40%，
**并且**上面两件事已就绪）：条件全部满足（1280×720×16 层，CPU 合成 11.1 ms / 16.7 ms 帧
= 66%），但结论是**分场景**的 —— bin2d 的 `Surface → Surface` 收尾要付回读的账
（5.7–10.5 ms/帧），所以默认路径仍是 CPU；不要求 CPU 侧像素的调用方（`composeTo`）
拿到的是 0.17–0.21 ms/帧，比 CPU 快 **53–67 倍**。详见 [perf.md](perf.md)。

**验收**：同一份场景，GPU 合成出的 `Surface` 与 CPU 合成出的 `Surface` 逐像素最大通道差
**1**（实测四个后端一致，见 `docs/img/gpu-compose.png` 与差图 `gpu-compose-diff.png`）；
性能上给出"合成占帧时间"的前后对比，而不是笼统的 fps。

---

## 6. 对 binGPU ABI 2 的诉求：**没有新增原语**

从 bin2d 这一侧反推，现有 ABI 1 已经够用：

| 需要的能力 | binGPU 现状 |
|---|---|
| 四边形批处理（图层 / 九宫格 / 位图） | ✅ `bingpu_cmd_draw_quads` + `BGPU_VERTEX_2D` |
| 5 种混合（预乘/覆盖/保留/擦除/叠加） | ✅ `BgpuBlend` 逐个对应 |
| 离屏目标 + 回读 | ✅ `texture_create(RENDER_TARGET)` + `frame_read`/`texture_read` |
| 任意四边形（仿射） | ✅ `bingpu_cmd_set_affine`（状态，不是逐顶点） |
| 重复上传同一张纹理 | ✅ `bingpu_texture_upload` |

唯一希望 binGPU 补的是**性能契约**而不是新接口：句柄热路径（binGPU `layering.md` §6.1）和
"冗余状态剔除由后端负责"（§6.2）。bin2d 一帧的 `set_texture`/`draw_quads` 次数和 binUI 同量级，
如果每次调用都查对象表，合成省下来的时间会被摊销掉。

**present 三态（binGPU §6.3）跟 bin2d 基本无关**：bin2d 是离屏/HEADLESS 的，呈现是 UI 库的事。

---

## 7. 明确不做

| 不做 | 理由 |
|---|---|
| 文本排版 / 字形光栅 | 归 UI 库；见 §3 |
| 布局 / 事件 / 窗口 | 同上 |
| 3D | 归 bin3d |
| RHI（着色器 / 任意管线状态） | 那是 binGPU 明确不做的东西，bin2d 更不该有 |
| 动画/时间轴 | 调用方每帧改参数即可 |
| 缓存的"聪明实现" | `Scene` 现在**故意**只留 `version()` 钩子，先把"渲染结果 == Canvas 直接画"钉死（roadmap ⑤） |

---

## 8. 一句话总结

bin2d 的边界是"**认不认节点**"，它的质量线是"**像素不变**"。往上它只交出位图/路径，
往下它只依赖 binGPU 的 P1；中间那条 GPU 路径，在 binGPU 有可用的真后端之前，只是"链路已通"。
