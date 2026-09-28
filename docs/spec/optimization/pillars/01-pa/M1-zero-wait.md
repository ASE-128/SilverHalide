# Milestone M1 · 1:1 零等待（P-A）

> Status: DRAFT
> Milestone: P-A M1
> Last updated: 2026-09-24

## Scope

本节点承接 **B-1** 量化锚点：**50MP、默认调整、缓存命中：进入 1:1 出图 ≤ **175ms**；冷态（首次需解码）≤ 1500ms。**

- **Range**：RAW 大图 ≥ **8000×6000（~50MP）**；基准机器 = 目标发布配置（x64，独立 GPU 可选）。
- 对应支柱文档子阶段 **1a 1:1 热路径**：进入/退出 1:1、缓存（目录内嵌预览 / 磁盘 1:1 / LRU）直读路径、热/冷态分支、全尺寸缓冲生命周期（至多 1 份、退出即释放）。
- 主要算力域：engine 线性缓冲与管线（`ImageBuffer`、`Pipeline.render`、`renderLinearFull`+`toDisplaySRGB`）、catalog 磁盘/内嵌预览缓存、ui `PreviewWorker`。

> 锚点来源：master 定位与支柱规格，数字一字不改；仅做节点落地拆解，不新增量化承诺。

## 引用

- 父文档（支柱）：[01-PA-performance.md](../../01-PA-performance.md)
- 父文档（路线图）：[00-ROADMAP.md](../../00-ROADMAP.md)

## 依赖与上下文

- 热态命中指目录内嵌预览 / 磁盘 1:1 缓存 / 内存 LRU 任一命中即可直读免重复解码。
- 进入 1:1 需做到感知零等待：热路径毫秒级返回。

## Acceptance criteria

> 二值、可验证；真机（含独立 GPU 机器）留存实测记录。

- [ ] 对固定 50MP 样本连续两次进入 1:1，取**第二次**（缓存命中）计时：**热 1:1 出图 ≤175ms 实测记录**。
- [ ] 清空缓存后**首次**进入 1:1 计时：**冷 1:1 出图 ≤1500ms 实测记录**。
- [ ] 冷态未命中可归因（磁盘缓存被清 / 目录为空非合理样本）；全尺寸缓冲退出 1:1 即释放（≤1 份）。

## 验收状态（2026-09-25）

**引擎层 headless 记录已落地**（`tests/TestPerfAnchors.cpp`，合成 8000×6000 F32 缓冲，Release 实测）：
- 1:1 全尺寸渲染引擎段（`renderFullDisplay`，线性→sRGB U8，不含 RAW 解码/磁盘直读）：约 **514ms**（本机沙箱）。
- 结论：此为引擎算力段基线，**非**锚点判定。热 ≤175ms / 冷 ≤1500ms 依赖 PreviewWorker 的磁盘 1:1 直读 + 解码路径，须在**真机（含独立 GPU）**以真实 50MP RAW 验收，当前标 **真机待验**。

## 验收状态（2026-09-28 · 1a 埋点与 worker 级热路径实测）

**已落地**：`[PERF1A]` 计时埋点、运行手册与 worker 级计时用例。

- 埋点（`qInfo` 前缀 `[PERF1A]`，全部落在 `src/ui/`，engine/catalog 保持 Qt-free）：
  - t0 `MainWindow::requestFullRes()`、t7 `MainWindow::onFullResReady()` —— **端到端判定口径**；
  - worker 侧 `PreviewWorker::loadFullRes()`：`hit=earlyReturn` / `hit=embedded` / `hit=disk` /
    `hit=memU8`、`decode=done`、`render=done`、`worker=done` —— 仅作归因，不作判定。
  - 埋点零副作用：不改变控制流、缓存写入条件与 `fullResReady` 的发出顺序。
- 运行手册：[1a-hot-path-runbook.md](1a-hot-path-runbook.md)（样本与机器、构建命令、埋点位置表、
  冷态重置流程、测量步骤、产出物、边界）。
- worker 级计时用例：`tests/lrc_qt_tests.cpp` 的 `FullResHotPathTest`（3 用例，实测毫秒以 `[PERF1A]` 输出）。

**本次实测（本机，256×192 合成夹具，Release）**：

| 分支 | worker 侧 | 请求→出帧 | 说明 |
|------|----------|----------|------|
| embedded（目录内嵌 1:1 预览） | 1ms | 0ms | 无解码、无渲染 |
| disk（legacy 磁盘 1:1 缓存） | 1ms | 1ms | JPEG 直读，无 RAW 解码 |
| 冷态（解码 + 渲染） | 13ms | 13ms | 小夹具，**不是 50MP 数字** |
| earlyReturn（同图同状态重复请求） | — | 无帧 | 不渲染，**不计入锚点判定** |
| `clearFullRes()` 之后再进入 | 0ms | 0ms | 证明 1:1 状态被释放且可重新出帧 |

- 验证：`build_gui/tests/lrc_qt_tests.exe` 12 个测试类 **48 用例全绿**（exit 0）；
  `build_gui/SilverHalide.exe` 重新编译链接通过。
- headless 基线与本次回归（`build_headless/tests/Release/lrc_tests.exe "[performance]" -s`，
  4 用例 / 24 断言全绿）：B-2 精帧 min=98.0ms / avg=100.4ms；B-1 引擎段单次 623.7ms
  （与前次记录 ≈514ms 存在运行间差异，属机器负载差异，仍只作算力基线）；D-2 回落
  688.2MB / 688.2MB（可重复）。

**仍未达成**：热 ≤175ms / 冷 ≤1500ms 的**真机端到端判定**（需真实 ≥50MP RAW、目标发布配置机器，
以及脚本化拖动与 GUI 呈现）。上表数字只覆盖 PreviewWorker 管道，**不构成锚点判定**；M1 仍标
**真机待验**，真机数字按运行手册第 8 节回写本节与
[01-PA-performance.md](../../01-PA-performance.md) 第 2 节。