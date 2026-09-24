# 产品定位与优化方向 Spec（追平 Lightroom · 保持特色）

> Status: ALIGNED
> Author: user
> Last updated: 2026-09-22
>
> 本文档是 Silver Halide 的**战略定位与优化路线图**，不是实施计划。它与既有
> [competitive-optimization-implementation-spec.md](competitive-optimization-implementation-spec.md)
>（"追平 LR" 的具体功能波 P1/P2/P3/T，已实现）衔接：那波已把功能追平 LR，
> 本方向在此基础上**打差异化**——押"纯 C++ 高性能 + 离线 AI"，并**放弃 LR 兼容、自成体系**。

## Background

Silver Halide 已是一套成熟的 RAW 摄影工作流桌面应用（v1.0.0，C++20+Qt6，Windows-only，MIT）。
上一波"竞品对齐优化"（P1/P2/P3/T）已把非破坏性编辑、目录 DAM、局部蒙版、离线 AI、导出/导入、
全景/HDR 等功能追平 Lightroom Classic。接下来需要一条**长期优化方向**，在追平 LR 的同时
保持自身不可替代的特色：纯 C++ 性能与本地离线 AI，并与 LR 生态彻底解耦、自成体系。

## 定位主张（一句话）

> **大图零等待、常驻轻量、离线 AI 的本地 RAW 编辑器** —— 对标 Lightroom Classic 的功能密度，
> 但以"重图编辑既不卡、也不吃爆内存 + 隐私友好的本地 AI"为差异化标签，功能与数据契约自成体系、
> 不再向 Lightroom 兼容。

## 对标基准

- **Lightroom Classic**：功能密度与编目 DAM 语义对标基准（本方向继续沿用它）。
- Capture One / Darktable / RawTherapee：仅在能力选项上参考，不设为基准。

## 三大支柱（优先级 A > B > D）

| 支柱 | 主题 | 内容 | 验收基调 |
|------|------|------|---------|
| **P-A** | 性能极致 | 1:1 放大零等待、编辑手感、内存回落、主线程帧预算 | 量化锚点（见下） |
| **P-B** | 离线 AI | 本地自动出片辅助 + 导入批量 AI 预分级，强化"隐私+本地"标签 | 能跑 + 单测绿，不强设门槛 |
| **P-D** | 稳定与工程债 | flaky、内存泄漏、目录并发、崩溃率 | 支撑前两支柱，回归绿 |

P-C（LR 兼容/预设生态）**不再是支柱**，见 In scope / Out of scope。

## P-A 性能锚点（接受标准，带机器与样本约定）

约定：RAW 大图 ≥ **8000×6000（~50MP）**；基准机器 = 目标发布配置（x64，独立 GPU 可选）。

| ID | 接受标准（可二值验证） |
|----|-----------------------|
| B-1 | 50MP、默认调整、缓存命中：进入 1:1 出图 ≤ **175ms**；冷态（首次需解码）≤ 1500ms。 |
| B-2 | 滑块拖动松手/停顿 → 1920 精帧 ≤ **80ms**。 |
| B-3 | 50MP 连续编辑全程主线程问询 ≤ 16ms/帧（无 UI 卡顿）。 |
| D-1 | 编辑单张 50MP RAW 峰值内存 ≤ **1.2 GB**。 |
| D-2 | 切图后内存回落到常驻基线 ≤ **400 MB**。 |
| D-3（optional） | 打开 1 万张目录，胶片栏滚动 / 缩略图加载不掉帧。 |

## P-B 离线 AI（1.1 里程碑能力）

- **B-1 AI 自动出片辅助**：本地一次自动建议曝光/对比/白平衡/裁剪并"1 键应用到参数堆栈"
  （复用 `estimateWhiteBalance` 思路，属参数建议，非生成式）。呼应现有「自动」按钮。
- **B-2 导入批量 AI 预分级**：导入时后台批量跑既有模型（去噪/主体分离/质量分），
  给胶片栏打出标志，强化"离线批量生产力"。

> B-3/B-4（本地超分/去马赛克增强、更多本地蒙版模型）列为 optional，不作承诺。

## In scope

- P-A 三条量化承诺的落地与真机（含 GPU 可用机器）量化验收。
- P-B，B-1 + B-2 两项能力（能跑 + headless 单测绿）。
- P-D 支撑性稳定项（上两支柱依赖的 flaky/泄漏/并发修复）。
- 逐步**移除** Lightroom 兼容路径（读 + 写），数据契约彻底 `.lrp`/XMP-proprietary 自洽。

## Out of scope

- macOS / Linux 移植、插件体系 / SDK、生成式 AI（对象移除/扩展）、云同步 / 移动端、
  联机拍摄、打印、人脸识别、地理标签地图（沿用 competitive spec §0 护栏）。
- 任何新增的 Lightroom 对齐投入；不再以"能否与 LR 互导"作为验收标准。
- P-C（LR 兼容性）整体不作为支柱。

## Assumptions

- 现有对 LR 的读取兼容路径（XMP/crs、sidecar、LR 预设 `.xmp`/`.lrcat`）将被**删除**；
  已有数据将由本应用自有格式 `.shcat`/`.lrp` 承担，旧 LR 产物不再识别。
- P-B 不设吞吐/耗时性能门槛，只有"能跑 + 单测绿"。
- 真机（独立 GPU）在 P-A 验收前应可用；当前沙箱 GPU 受限已在竞争 spec 记录。

## Edge cases & risks

| Category | Notes |
|----------|-------|
| 边界条件 | 50MP 之上（100MP/长焦拼接图）是否同样达标未承诺；B-1 冷态 <1.5s 依赖磁盘缓存命中。 |
| 失败模式 | 移除 LR 读取后，用户从 LR 迁移而来的旧预设/目录将无法导入 → 需迁移引导或一次性转化工具。 |
| 风险 | "自成体系"使产品不再进入 LR 生态，潜在用户迁移门槛上升。 |
| Mitigation | 在 1.x 提供一次性的 `.lrp` 自有格式归档/迁移提示；README 明确"不再兼容 LR 预设"。 |

## Acceptance criteria（方向级）

- AC-1 P-A 的 B-1(≤175ms)/B-2(≤80ms)/B-3(≤16ms 帧)/D-1(≤1.2GB)/D-2(≤400MB) 在真机有实测记录，
  ≥4/5 达成即视为 P-A 里程碑成立（不达标项给出差距与后续计划）。
- AC-2 P-B 的 B-1 + B-2 均有落地代码 + headless `[ai]` 门控单测绿。
- AC-3 移除 LR 兼容路径（读+写）后，全量 headless 回归绿；`.lrp`/自有 sidecar 往返自洽。
- AC-4 README/ARCHITECTURE 更新为"自成体系、不兼容 LR"的定位表述。

## Open questions

- 移除 LR 读取对**现有用户数据**的一次性迁移/引导方案（转化工具 or 仅提示）未定 —— 需产品决策。

## Core entities (ontology)

| Entity | Type | Key fields | Relationship |
|---|---|---|---|
| 定位主张 | 战略 | 大图零等待 / 常驻轻量 / 离线 AI | 统领支柱 |
| P-A 性能极致 | 支柱 | B-1..D-3 锚点 | 主战场 |
| P-B 离线 AI | 支柱 | B-1 自动出片 / B-2 批量预分级 | 特色标签 |
| P-D 稳定工程债 | 支柱 | flaky/泄漏/并发 | 支撑 |
| 自成体系 | 决策 | 弃 LR 读+写 | 见 ADR-0001 |

## Interview metadata

- Mode: --deep
- Waves: 5
- Final ambiguity: 20%
- Status: ALIGNED (用户在波末直接锁定位，非 STUCK)

### Clarity breakdown

| Dimension | Score | Weight | Weighted |
|---|---|---|---|
| Goal | 0.85 | 0.40 | 0.34 |
| Scope | 0.65 | 0.25 | 0.1625 |
| AC | 0.85 | 0.25 | 0.2125 |
| Context | 0.85 | 0.10 | 0.085 |
| Ambiguity | | | 20% |

### Ontology convergence

- 对标基准：Lightroom Classic（stable）
- 特色：纯 C++ 高性能 → 大图零等待 + 常驻轻量（renamed）
- 特色：离线 AI（stable）
- 决策：自成体系 / 弃 LR（new）

## 下一步

本定位已按支柱拆分为阶段化细化文档，索引见
[docs/spec/optimization/00-ROADMAP.md](optimization/00-ROADMAP.md)；
各支柱专文为 optimization/01-PA-performance.md、02-PB-offline-ai.md、03-PD-stability.md、
04-drop-lr-compat.md。