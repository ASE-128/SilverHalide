# P-D M1 稳定加固（崩溃 / 泄漏 / flaky）节点文档

> Status: DRAFT
> Milestone: P-D M1
> Last updated: 2026-09-24

## 1. 定位

M1 是 P-D 稳定支柱下两个实施节点中的**第一节点**，收敛 03-PD 稳定项分类中的 **S1 + S3**：

- **S1（崩溃 / 内存泄漏）**：最高优先级、阻断性，处理 SIGSEGV、访问非法内存、use-after-free、
  长跑内存单调上涨、异常路径未释放。
- **S3（flaky 测试 / 阈值）**：中优先级、回归可信度，处理同一用例运行间非确定性通过/失败、
  断言阈值过近、依赖 DLL 搜索顺序。

本节点**不产生新功能**，只负责把 03-PD 已登记（或后续发现的）S1/S3 稳定项做成
"可复现、可回归、不闹鬼"。工作节奏跟随 P-A/P-B 每轮改动同步收口，不单独开大线。

## 2. Scope

### 2.1 In scope（本节点承接）

| 类别 | 具体承接 | 03-PD 关联条目 |
|------|----------|----------------|
| S1 崩溃 / 泄漏 | 任意 SIGSEGV / 断言触发点定位与修复；热点路径 RAII 审计；退出/异常路径释放路径复核；长跑内存单调上涨判据建立 | S-r-01（ORT DLL 误加载触发的 SIGSEGV，属构建/部署隔离，不是改测试绕过） |
| S1 泄漏 | 热点路径（解码 / 渲染 / 缓存 / 目录写库）长跑内存曲线不再单调上涨 | — |
| S3 flaky | 消灭运行间非确定性；阈值类断言给足余量或改确定性参考；环境隔离（DLL 部署与搜索顺序） | S-r-02（`TestRawDecoder.cpp:556` R-08 embedded-ICC 断言，Δ~0.001–0.019 与阈值 0.02 过近） |
| 回归门槛 | 每次 S1/S3 修复的回归测试登记 [tests/CMakeLists.txt](../../../../tests/CMakeLists.txt)，纳入 headless 全量回归 | §5 新增回归测试登记 |

### 2.2 Out of scope（本节点不承接）

- S2（并发 / 竞态，如 S-r-03/S-r-04）归 **M2** 承接，本节点不处理 worker 生命周期竞态。
- S4（目录并发 / 大库，如 S-r-07）归 **M2** 承接，本节点不处理目录大库支撑。
- 新增产品功能（由 P-A/P-B/竞品对齐波承担）。
- 指针级全量内存审计（只覆盖热点/崩溃路径）。
- 已知取舍项的能力补齐（如 S-r-05 TIFF 压缩读取、S-r-06 全尺寸预览）不归本节点。

## 3. S 分类映射

| 03-PD 稳定项 ID | 现象简述 | 性质 | S 分类 | 归属节点 | 本节点动作 |
|-----------------|----------|------|--------|----------|-----------|
| S-r-01 | `TestAiInference.cpp:149` 空引擎 SIGSEGV，Debug 目录缺 ORT 1.23 DLL 误加载系统旧版 | 环境性 | **S1**（崩溃信号） | M1 | 构建/部署隔离：Release 与 Debug 输出目录都部署正确 ORT 1.23.0 DLL |
| S-r-02 | `TestRawDecoder.cpp:556` R-08 embedded-ICC 断言 Δ 与阈值 0.02 过近，运行间非确定 | 代码阈值质量 | **S3** | M1 | 阈值加余量 / 确定性参考，连续 N 次零随机失败 |
| S-r-03 | ImportWorker / PreviewWorker 关窗竞态（曾 use-after-free） | 已修复 | S2 | M2 | 不留存回归测试，转 M2 防回退 |
| S-r-04 | FolderMonitor 事件线程异常退出回退 / D3D11 blur 负尺寸 | 已修复 | S2 | M2 | 转 M2 防回退 |
| S-r-07 | 全景无预览 UI 线程解码 / 智能收藏夹阻塞式全表 | 已知局限 | S4 | M2 | 随 S4 目录大库关注 |

> 映射原则：以 [03-PD-stability.md](../../03-PD-stability.md) §3/§4 分类为准，S1/S3 归本节点 M1，
> S2/S4 归下一节点 M2。若 03-PD 后续改分类名，以 03-PD 为准重新对齐本表。

## 4. 实施要点

### 4.1 S1 崩溃 / 泄漏

- 以"崩溃率与泄漏"为第一目标，任何崩溃（断言、SIGSEGV）都先于性能/AI 落地。
- 泄漏以"长跑内存曲线单调上涨"为判据，不追求指针级全覆盖审计，优先覆盖热点路径
  （RAW 解码、预览渲染、缓存、目录写库事务）。
- 对 S-r-01 这类**环境性** SIGSEGV：通过构建/部署隔离（Release 与 Debug 输出目录均部署
  ORT 1.23.0 DLL，杜绝 DLL 搜索顺序误加载系统旧版）解决，**不得**修改测试断言绕过。

### 4.2 S3 flaky / 阈值

- 目标是**消灭非确定性**：同一测试 N 次运行结果一致。
- 阈值类断言（如 R-08 ICC Δ）给足余量，或用确定性参考替代运行间非确定比较。
- 环境隔离：依赖的 DLL 部署与搜索顺序在验收时锁定，避免"单跑过、全量偶尔挂"。

## 5. 回归硬门槛

本节点任何改动合入前必须满足（来源：[03-PD-stability.md](../../03-PD-stability.md) §5）：

- **全量 headless 全绿**：`cmake -S . -B build_headless -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF`
  → `cmake --build build_headless -j` → `ctest --test-dir build_headless --output-on-failure`。
- **GUI 冒烟 `SMOKE_EXIT=0`**：`build_gui/tests/lrc_tests.exe`（GUI 用例）启动冒烟退出码必须为 0，
  不得出现假卡死/退出崩溃。
- **连续零随机失败**：被判定为稳定的 S1/S3 用例，连续 N 次（建议 ≥3）零随机失败后方可从"待稳定"转绿。
- **新增回归测试登记**：每次 S1/S3 修复对应回归测试登记到
  [tests/CMakeLists.txt](../../../../tests/CMakeLists.txt)，纳入 headless 全量回归。

## 6. 关联文档

- 父文档（支柱细化）：[03-PD-stability.md](../../03-PD-stability.md)
- 父文档（阶段路线图）：[00-ROADMAP.md](../../00-ROADMAP.md)
- 上游权威来源（master 定位）：[optimization-positioning-spec.md](../../../optimization-positioning-spec.md)
- 实时项目状态：[STATUS.md](../../../STATUS.md)

## 7. Acceptance criteria（二值、可验证）

> 以下标准以**可二进制判断**为准，落地后勾选 通过/未达。

| ID | 验收标准（可验证） | 判定 |
|----|--------------------|------|
| M1-AC-1 | S1：任意 SIGSEGV / 断言触发点在 headless 全量中重现次数降为 **0** | 通过/未达 |
| M1-AC-2 | S1：热点路径长跑内存曲线不再单调上涨（以内存看护判据为准） | 通过/未达 |
| M1-AC-3 | S1：S-r-01 两个输出目录（Release/Debug）均已部署 ORT 1.23.0 DLL，`TestAiInference.cpp:149` 不再误加载系统旧 DLL | 通过/未达 |
| M1-AC-4 | S3：R-08 ICC 断言（S-r-02）改为确定性 / 有余量后，**连续 ≥3 次 headless 全量零随机失败** | 通过/未达 |
| M1-AC-5 | 硬门槛：**全量 headless 全绿 + GUI 冒烟 `SMOKE_EXIT=0`** | 通过/未达 |
| M1-AC-6 | 每次 S1/S3 修复的回归测试已登记 [tests/CMakeLists.txt](../../../../tests/CMakeLists.txt) | 通过/未达 |

> 约定：N（连续绿运行次数）取 ≥3；修复项不得带出回归覆盖范围。