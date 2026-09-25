# P-D M2 并发与目录健壮性（S2 / S4）节点文档

> Status: DRAFT
> Milestone: P-D M2
> Last updated: 2026-09-24

## 1. 定位

M2 是 P-D 稳定支柱下两个实施节点中的**第二节点**，收敛 03-PD 稳定项分类中的 **S2 + S4**：

- **S2（并发 / 竞态）**：高优先级，处理多线程 worker 退出竞态、事件线程异常回退、
  目录/预览 worker 共享状态并发访问。
- **S4（目录并发 / 大库）**：低优先级、随规模增长，处理超万张目录、导入/预览并发写库、
  智能收藏夹全表刷新、UI 线程长任务。

本节点**不产生新功能**，把 worker 生命周期、目录并发与大库场景做成"任何时序下都安全、任何规模下都可回归"。
它是 P-A D-3（打开 1 万张目录不掉帧）的前置条件，节奏跟随 P-A/P-B 每轮集成交互进行。

## 2. Scope

### 2.1 In scope（本节点承接）

| 类别 | 具体承接 | 03-PD 关联条目 |
|------|----------|----------------|
| S2 并发 / 竞态 | 固定"worker abort + quit 生命周期契约"，让"关窗/切图/取消"在任何时序下都安全 | S-r-03（ImportWorker / PreviewWorker 关窗 use-after-free，已修，防回退） |
| S2 并发 / 竞态 | 事件线程异常回退路径长期保持健壮；共享状态并发访问审计（`atomic` / `busy_timeout`） | S-r-04（FolderMonitor 事件线程异常退出 wake 恢复轮询 / D3D11 blur 负尺寸，已修，防回退） |
| S2 竞态回归 | 以并发 / 切图 / 取消时序回归测试锁定生命周期契约，连续 N 次无竞态失败 | §4.1 新增/保留回归测试 |
| S4 目录并发 / 大库 | 超万张目录并发导入/预览写库无数据漂移；智能收藏夹全表刷新后台化；UI 线程长任务剔除 | S-r-07（全景无预览 UI 线程解码 / 智能收藏夹阻塞式全表刷新） |
| 回归门槛 | 每次 S2/S4 修复的回归测试登记 [tests/CMakeLists.txt](../../../../../tests/CMakeLists.txt)，纳入 headless 全量回归 | §5 新增回归测试登记 |

### 2.2 Out of scope（本节点不承接）

- S1（崩溃 / 泄漏，如 S-r-01）归 **M1** 承接，本节点不处理 ORT DLL 误加载等崩溃信号。
- S3（flaky / 阈值，如 S-r-02）归 **M1** 承接，本节点不处理 R-08 ICC 断言阈值。
- 新增产品功能（由 P-A/P-B/竞品对齐波承担）。
- 跨平台移植、插件 SDK、LR 兼容（沿用 master Out of scope 护栏）。
- 已知取舍项能力补齐（如 S-r-05/S-r-06）不归本节点。

## 3. S 分类映射

| 03-PD 稳定项 ID | 现象简述 | 性质 | S 分类 | 归属节点 | 本节点动作 |
|-----------------|----------|------|--------|----------|-----------|
| S-r-03 | ImportWorker / PreviewWorker 关窗竞态（曾 use-after-free：访问已析构的 `catalog_`/`pipeline_`） | 已修复 | **S2** | M2 | 留存/新增并发·切图·取消时序回归测试，防回退 |
| S-r-04 | FolderMonitor 事件线程异常退出需 wake 恢复 2s 轮询回退；D3D11 blur 负尺寸先检查再 resize | 已修复 | **S2** | M2 | 留存回归测试防回退，事件回退路径长期保持健壮 |
| S-r-06 | 文件夹监视 OS 事件不可用时回退 2s 轮询 | 已知取舍 | S2 / 边界 | M2 | 作为 S-r-04 的回退路径，长期保持健壮 |
| S-r-07 | 全景合成无标准预览时 UI 线程解码；智能收藏夹刷新为阻塞式全表（超大库建议后台化） | 已知局限 | **S4** | M2 | 后台化 / 批量化 / 并发写队列压测，为 P-A D-3 铺路 |
| S-r-01 | 空引擎 SIGSEGV 因 ORT DLL 部署 | 环境性 | S1 | M1 | 交 M1 构建/部署隔离 |
| S-r-02 | R-08 ICC 断言阈值过近 | 代码阈值质量 | S3 | M1 | 交 M1 阈值余量处理 |

> 映射原则：以 [03-PD-stability.md](../../03-PD-stability.md) §3/§4 分类为准，S2/S4 归本节点 M2，
> S1/S3 归上一节点 M1。若 03-PD 后续改分类名，以 03-PD 为准重新对齐本表。

## 4. 实施要点

### 4.1 S2 并发 / 竞态

- 固定"worker abort + quit 生命周期契约"：`stopWorkers` 先 `requestAbort`（`std::atomic` 标志）
  再 `quit()` + `wait`，杜绝关窗时访问已析构的 `catalog_`/`pipeline_`。
- 对该契约补充**时序回归测试**：覆盖 切图 / 取消 / 批量导入中关窗 的任意时序，连续 N 次无竞态失败。
- 事件驱动回退路径（FolderMonitor 异常退出）保持健壮，D3D11 路径负尺寸先检查再 resize。
- 目录写库并发访问以 `atomic` / `busy_timeout` 审计收敛（单连接 + busy_timeout 队列）。

### 4.2 S4 目录并发 / 大库

- 以"打开 ≥1 万张目录不影响可用、并发导入/预览写库无数据漂移"为目标，是 P-A D-3 的前置条件。
- 智能收藏夹全表刷新改后台化/批量化，避免阻塞 UI 线程；全景合成无标准预览时不在 UI 线程解码。
- 并发写库走队列/批次压测，确认为高规模输入时无 werror、无数据漂移。

## 5. 回归硬门槛

本节点任何改动合入前必须满足（来源：[03-PD-stability.md](../../03-PD-stability.md) §5）：

- **全量 headless 全绿**：`cmake -S . -B build_headless -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF`
  → `cmake --build build_headless -j` → `ctest --test-dir build_headless --output-on-failure`。
- **GUI 冒烟 `SMOKE_EXIT=0`**：`build_gui/tests/lrc_tests.exe`（GUI 用例）启动冒烟退出码必须为 0，
  不得出现假卡死/退出崩溃。
- **连续零随机失败**：被判定为稳定的 S2/S4 用例，连续 N 次（建议 ≥3）零随机失败后方可从"待稳定"转绿。
- **新增回归测试登记**：每次 S2/S4 修复对应回归测试登记到
  [tests/CMakeLists.txt](../../../../../tests/CMakeLists.txt)，纳入 headless 全量回归。

## 6. 关联文档

- 父文档（支柱细化）：[03-PD-stability.md](../../03-PD-stability.md)
- 父文档（阶段路线图）：[00-ROADMAP.md](../../00-ROADMAP.md)
- 上游权威来源（master 定位）：[optimization-positioning-spec.md](../../../optimization-positioning-spec.md)
- 实时项目状态：[STATUS.md](../../../../../STATUS.md)

## 7. Acceptance criteria（二值、可验证）

> 以下标准以**可二进制判断**为准，落地后勾选 通过/未达。

| ID | 验收标准（可验证） | 判定 |
|----|--------------------|------|
| M2-AC-1 | S2：worker abort+quit 生命周期契约以并发 / 切图 / 取消时序回归测试锁定，**连续 ≥3 次无竞态失败** | 通过/未达 |
| M2-AC-2 | S2：S-r-03/S-r-04 的防回退回归测试（关窗 / 事件线程异常退出 / D3D11 blur 负尺寸）已登记且绿 | 通过/未达 |
| M2-AC-3 | S4：打开 ≥1 万张目录并发导入/预览场景，无明显 werror、无数据漂移 | 通过/未达 |
| M2-AC-4 | S4：大库智能收藏夹刷新 / 全景解码不再阻塞 UI 线程（后台化/批量化落地） | 通过/未达 |
| M2-AC-5 | 硬门槛：**全量 headless 全绿 + GUI 冒烟 `SMOKE_EXIT=0`** | 通过/未达 |
| M2-AC-6 | 每次 S2/S4 修复的回归测试已登记 [tests/CMakeLists.txt](../../../../../tests/CMakeLists.txt) | 通过/未达 |

> 约定：N（连续绿运行次数）取 ≥3；修复项不得带出回归覆盖范围。