# bin2d 文档索引

| 文档 | 内容 |
|---|---|
| [architecture.md](architecture.md) | 分层、职责划分、一次填充的完整数据流、关键技术决定（为什么预乘 / 为什么覆盖率 AA / 为什么描边走并集） |
| [api.md](api.md) | 每个头的关键类型 + 可直接抄的代码片段 |
| [perf.md](perf.md) | 吞吐基线：怎么测、0.1.0 → 0.1.1 的同机数字对照、每条优化对应哪一步、迁移开工条件那一轮（0.4.2，1.61× → 0.98×）、没做的项与原因 |
| [roadmap.md](roadmap.md) | 五个阶段：地基 → 上层 UI 库迁移（0 diff 验收） → GPU 合成 → 滤镜/mip → 缓存与脏区；每阶段写开工条件 |
| [layering.md](layering.md) | **这一层在哪、和 binGPU / UI 库的分工线、两条一致性承诺的落点、与 UI 库的迁移台账（含删除条件）、GPU 阶段的开工前置、对 binGPU ABI 2 的诉求** |

测试说明见 [`../tests/README.md`](../tests/README.md)。

## 一分钟上手

```cpp
#include "bin2d/canvas.h"
#include "bin2d/png.h"
using namespace bin2d;

Surface canvas(320, 200);
Canvas c(canvas);
c.clear(Color::argb(0xFF10141A));

Path star;
star.addStar(160, 100, 70, 28, 5);
c.fillPath(star, Paint::linearAngle(Rect(90, 30, 140, 140), 90.0f,
                                    0xFF3E8BFF, 0xFFF2B544));

writePng(canvas, "star.png");
```

## 构建 / 验证

```sh
make all      # 库 + 测试 + 演示，零警告是验收线
make test     # 全部测试（失败非 0 退出）
make demo     # 出图 build-mingw/out/bin2d-demo.png
make layers   # 层边界检查：不许出现上层词汇/依赖
make bench    # 单线程吞吐：512x512 六项场景（ms / ns/px / MPix/s）
```
