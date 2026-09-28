# 1a 运行手册 · 1:1 热路径验收（P-A / B-1）

> Status: ACTIVE
> Milestone: P-A M1（子阶段 1a）
> Last updated: 2026-09-26
>
> 本文是 [M1-zero-wait.md](M1-zero-wait.md) 的**可执行操作手册**：把 B-1 锚点的验收过程落到
> 命令、埋点与判定规则上。锚点数字与机器约定以
> [01-PA-performance.md](../../01-PA-performance.md)（支柱）与
> [optimization-positioning-spec.md](../../../optimization-positioning-spec.md)（master）为准，
> 本文不新增任何量化承诺。

## 1. 目标与口径

**锚点 B-1**：50MP（≥8000×6000）、默认调整、缓存命中进入 1:1 出图 ≤ **175ms**；冷态（首次需解码）
≤ **1500ms**。

**端到端口径**（判定值）：

| 阶段 | 位置 | 说明 |
|------|------|------|
| t0 请求下发 | `MainWindow::requestFullRes()` | 1:1 请求离开 UI 线程，计时器启动 |
| t7 呈现终点 | `MainWindow::onFullResReady()`（`setImageKeepView` 之后） | 帧已上屏，输出 `total` 毫秒 |

**worker 分段日志只作归因，不作判定**。`PreviewWorker::loadFullRes()` 内部还会打印
`decode=done` / `render=done`，用来判断预算被哪一段吃掉；它们不是 B-1 结论。

判定规则：B-1/B-2/B-3/D-1/D-2 五条真机实测 **≥4/5 达成**即里程碑成立，不达标项须写明差距与后续计划
（D-3 为 optional，不进入计数）。

## 2. 样本与机器

- **样本**：固定一份 RAW 大图 ≥ **8000×6000（~50MP）**，登记文件名、像素尺寸、文件大小、mtime 与
  SHA256；放入独立测试目录，不要与其他图混用。
- **对照样本**：第二张不同的 50MP RAW（切图/回落对照）、若干普通小图（验证退出 1:1 后常规预览不受影响）。
- **机器 A（必测）**：目标发布配置 x64，**无独立 GPU 也必须达标**；登记 CPU、内存容量与频率、存储类型
  （SSD / NVMe）、Windows 版本与内部版本号。
- **机器 B（最终窗口）**：含独立 GPU 的机器作为最终验收窗口；受沙箱 GPU 限制、设备不可用的机器
  **不作为唯一裁决来源**。
- **对比组（可选）**：分别用 `LRC_ENABLE_SIMD=ON`、`LRC_ENABLE_D3D=ON` 重建后重复冷/热测量。

## 3. 构建与运行

GUI 构建（Release + AI，脚本已内联 `vcvars64.bat` 与 `ONNXRUNTIME_ROOT`）：

```bat
.\build_x64.bat
```

等价手写命令：

```bat
cmake -S . -B build_gui -G Ninja ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DLRC_ENABLE_AI=ON ^
      -DONNXRUNTIME_ROOT=onnxruntime/onnxruntime-win-x64-1.23.2
cmake --build build_gui -j
```

部署 Qt 运行时并启动：

```powershell
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build_gui\SilverHalide.exe
.\build_gui\SilverHalide.exe
```

可选加速对比组（分别重建后再重复第 6 节测量）：

```bat
cmake -S . -B build_gui -G Ninja -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_SIMD=ON
cmake -S . -B build_gui -G Ninja -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_D3D=ON
```

## 4. 计时埋点（`[PERF1A]`）

全部埋点走 `qInfo`，前缀统一 `[PERF1A]`，可用一次过滤取出完整链路。**行号为当前工作区快照，
改动后请以符号名定位。**

| 埋点 | 位置（符号名优先） | 记录内容 | 作用 |
|------|-------------------|---------|------|
| t0 请求下发 | `MainWindow::requestFullRes()` | `id` + `gen` | 端到端口径起点 |
| t1 worker 入口 | `PreviewWorker::loadFullRes()` | `id` + `gen` | 排队延迟 `t1 - t0` |
| hit=earlyReturn | `loadFullRes` 的「同图同帧状态已发出即返回」分支 | `id` + `gen` | **不渲染，排除在判定之外** |
| hit=embedded | 目录内嵌 FullRes 预览命中分支 | `id` + `gen` + worker 耗时 | 最快热路径 |
| hit=disk | `ThumbnailCache::findFullRes` 命中分支 | `id` + `gen` + worker 耗时 | legacy 磁盘缓存路径 |
| hit=memU8 | `findFullResU8` 命中分支 | `id` + `gen` + worker 耗时 | 非默认调整下的内存复用 |
| decode=done | `decodeFullRes()` 返回后 | worker 耗时 | 冷态主成本 |
| render=done | `Pipeline::renderLinearFull()` 返回后 | worker 耗时 | 引擎算力段 |
| worker=done | `emit fullResReady` 之后 | worker 耗时 | worker 侧完成 |
| t7 呈现终点 | `MainWindow::onFullResReady()` | `id` + `total` 毫秒 | **锚点判定值** |

约束：`engine/` 与 `catalog/` 必须保持 Qt-free，因此 `qInfo` 这类埋点只能落在 `src/ui/`；
若要在引擎段计时，请用 `std::chrono` 且不得引入 Qt 头，否则 headless 构建会直接失败。

## 5. 冷态重置流程

1. **重启应用**，清掉内存侧状态：Pipeline 预览 LRU（默认约 256MB）、`PreviewWorker` 保留的
   全尺寸线性缓冲与全尺寸 U8 复用缓存。
2. **删磁盘 1:1 缓存**：`ThumbnailCache` 的 full-res 目录文件（`findFullRes` / `writeFullRes` 对应）。
3. **删目录内嵌全尺寸预览**：`<previewDir>/<id 低 3 位 hex>/<id>_2.shcat`
   （`CatalogDatabase::PreviewKind::FullRes = 2` 对应的外置文件）；或用全新库只导入该样本。
4. **冷态自检**：启动后首次进入 1:1，日志中必须**看不到** `hit=embedded`、`hit=disk`、
   `hit=memU8`、`hit=earlyReturn` 任一命中标记，才确认是真正的冷态。
5. **记录清空动作**：把本次实际执行的第 1–3 步写进验收记录——缓存早被清掉的样本不能当热态测。

## 6. 测量步骤

1. **冷态**：按第 5 节重置后首次进入 1:1，记录 `t7 - t0`（`[PERF1A] t7 presented total=`），
   重复 ≥3 次取 min/avg，目标 ≤ **1500ms**。
2. **热态**：退出 1:1 → 切到对照图再切回（或重启应用、保留磁盘与内嵌缓存），进入 1:1 并**按命中层级
   分别报数**（embedded / disk / memU8），各自计时，目标 ≤ **175ms**。
3. **earlyReturn 单列**：同一图同一调整状态连续第二次进入会命中早退分支，耗时接近 0ms。它不渲染任何
   像素，**必须单列统计，不能用作 B-1 达标证据**。
4. **单缓冲生命周期核验**：反复进入/退出 1:1 若干轮，每轮记录退出后的内存；确认同一时刻至多 1 份
   全尺寸缓冲、退出即释放（`PreviewWorker::clearFullRes()`），无累积增长。
5. **归因**：热态超标时用 `[PERF1A]` 分段日志判定瓶颈是在磁盘读取、JPEG 解码还是显示转换；
   冷态超标时先看 `decode=done`。
6. **对比组（可选）**：在 SIMD / D3D 构建下重复第 1–2 步给出差值，并确认默认关态输出与基线一致。

## 7. 回归守卫

headless 合成 50MP 基线（`tests/TestPerfAnchors.cpp`）用于改动前后差分，防止性能悄悄退化：

```bat
cmake -S . -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j

.\build_headless\lrc_tests.exe "[performance][pa]"
.\build_headless\lrc_tests.exe "[performance][memory][pa]"
```

Qt 层的 worker 级热路径计时（`tests/lrc_qt_tests.cpp` 的 `FullResHotPathTest`）覆盖
embedded / disk / earlyReturn 三条分支，可在无 GUI 交互的情况下拿到请求→出帧的管道耗时：

```bat
.\build_gui\tests\lrc_qt_tests.exe
```

## 8. 产出物与不达标处理

必须留下的记录：

- `[PERF1A]` 原始日志（含 `id` / `gen` 对齐，便于把 t0/t1/t7 串成一条链）；
- 冷态 ≥3 次的 min/avg，附清空动作与「无命中标记」自检结果；
- 热态按 embedded / disk / memU8 分层的数字，earlyReturn 单列；
- 单缓冲生命周期核验记录（N 轮进出无累积增长）；
- 对比组差值（可选）与默认关态一致性结论。

数字回写位置：本目录 [M1-zero-wait.md](M1-zero-wait.md) 的「验收状态」小节，以及
[01-PA-performance.md](../../01-PA-performance.md) 第 2 节锚点表的实测记录。不达标项必须写明
**差距数值 + 归因 + 后续计划**，不得只写「未达标」。

## 9. 边界与已知前提

- 50MP 之上（100MP / 拼接图）**不作达标承诺**。
- 冷态 ≤1500ms 依赖磁盘 1:1 缓存命中；缓存被清或目录为空的样本**不能**作为冷态达标判据。
- 「热态」的定义是缓存命中即可直读免重复解码，命中来源必须逐条标注，避免把不同档位混为一谈。
- 真机（含 GPU）数字与沙箱/合成基线不可互换：`tests/TestPerfAnchors.cpp` 的引擎段数字
  （1:1 ≈514ms、1920 精帧 min≈99ms/avg≈100ms、作用域释放回落 ≈688MB）只作算力基线与回归参照。
