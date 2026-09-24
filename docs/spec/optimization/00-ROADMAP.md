# 优化方向阶段化路线图

> Status: DRAFT
> Last updated: 2026-09-22
>
> 本文档是 Silver Halide 优化方向的**阶段化实施路线图**，是对
> [optimization-positioning-spec.md](../optimization-positioning-spec.md)（master 定位）与
> [ADR-0001](../../adr/0001-drop-lightroom-compat.md) 的落地切分。master 负责"方向与锚点"，
> 本文档负责"**按阶段可交付、可验收怎么切**"。数字、优先级与支柱定义一律以 master 为准，一字不改。

## 1. 定位一句话

> **大图零等待、常驻轻量、离线 AI 的本地 RAW 编辑器** —— 对标 Lightroom Classic 的功能密度，
> 但以"重图编辑既不卡、也不吃爆内存 + 隐私友好的本地 AI"为差异化标签，功能与数据契约自成体系、
> 不再向 Lightroom 兼容。

（来源：optimization-positioning-spec.md「定位主张」）

## 2. 三大支柱与优先级

| 支柱 | 主题 | 优先级 |
|------|------|--------|
| **P-A 性能极致** | 1:1 放大零等待、编辑手感、内存回落、主线程帧预算 | A（最高，主战场） |
| **P-B 离线 AI** | 本地自动出片辅助 + 导入批量 AI 预分级，强化"隐私+本地"标签 | B |
| **P-D 稳定工程债** | flaky、内存泄漏、目录并发、崩溃率 | D（支撑前两支柱） |

> **P-C（LR 兼容 / 预设生态）不再是支柱**。见 In scope / Out of scope 与 ADR-0001。

### 2.1 P-A 量化锚点（接受标准，带机器与样本约定）

约定：RAW 大图 ≥ **8000×6000（~50MP）**；基准机器 = 目标发布配置（x64，独立 GPU 可选）。

| ID | 接受标准（可二值验证） |
|----|-----------------------|
| B-1 | 50MP、默认调整、缓存命中：进入 1:1 出图 ≤ **175ms**；冷态（首次需解码）≤ 1500ms。 |
| B-2 | 滑块拖动松手/停顿 → 1920 精帧 ≤ **80ms**。 |
| B-3 | 50MP 连续编辑全程主线程问询 ≤ 16ms/帧（无 UI 卡顿）。 |
| D-1 | 编辑单张 50MP RAW 峰值内存 ≤ **1.2 GB**。 |
| D-2 | 切图后内存回落到常驻基线 ≤ **400 MB**。 |
| D-3（optional） | 打开 1 万张目录，胶片栏滚动 / 缩略图加载不掉帧。 |

### 2.2 P-B 离线 AI（1.1 里程碑能力）

- **B-1 AI 自动出片辅助**：本地一次自动建议曝光/对比/白平衡/裁剪并"1 键应用到参数堆栈"
  （复用 `estimateWhiteBalance` 思路，属参数建议，非生成式）。呼应现有「自动」按钮。
- **B-2 导入批量 AI 预分级**：导入时后台批量跑既有模型（去噪/主体分离/质量分），
  给胶片栏打出标志，强化"离线批量生产力"。

> B-3/B-4（本地超分/去马赛克增强、更多本地蒙版模型）列为 optional，不作承诺。

## 3. 阶段划分

切分原则：把三大支柱按"**可交付、可验收**"切成阶段；P-D 作为支撑项融入依赖它的阶段，
而不是单列一个大阶段；每条路径都保留 headless 单测作为快速反馈，涉及锚点的条目在阶段内
给出"需真机（独立 GPU）验收"的清单。各阶段交付物文档锚定同一份 master 定位。

| 阶段 | 支柱归属 | 前置依赖 | 交付物 | 真机验收锚点 |
|------|----------|----------|--------|--------------|
| Phase 1 | P-A（性能基底）+ P-D（前置稳定） | master 定位已锁定；真机（独立 GPU）可用的构建机在线 | 01-PA-performance.md（B-1/B-2 基底）、03-PD-stability.md（P-D 前置项） | B-1 热 1:1 ≤175ms / 冷 ≤1500ms；B-2 精帧 ≤80ms；D-2 ≤400MB（基线回落） |
| Phase 2 | P-A（内存极致 + 剩余） | Phase 1 完成（B-1/B-2 已接）；B-3 需基础帧预算就绪 | 01-PA-performance.md（D-1/D-2 收尾 + B-3） | D-1 峰值 ≤1.2GB；D-2 ≤400MB 收尾；B-3 主线程 ≤16ms/帧；D-3(optional) 1 万张滚动不掉帧 |
| Phase 3 | P-B（离线 AI） | Phase 1 稳定基座就绪；既有 AI 模型管线在 headless 可跑 | 02-PB-offline-ai.md（B-1、B-2） | 能跑 + headless `[ai]` 单测绿，不强设性能门槛 |
| Phase 4 | 自成体系（LR 移除落地） | 对应 04 文档；需 ADR-0001 决策已在 1.x 定迁移/引导方案 | 04-drop-lr-compat.md（读 + 写移除） | 全量 headless 回归绿；`.lrp`/自有 sidecar 往返自洽 |

### 3.1 阶段说明

- **Phase 1 · 性能基底 / 真机验收（P-A + P-D 前置）**：先打通 1:1 零等待与编辑手感的量化基线，
  并以"能复现"为第一优先。此阶段同时落地 P-D 里 P-A 依赖的稳定项（关键 flaky/泄漏/并发修复），
  避免性能验收被基础设施问题干扰。真机（独立 GPU）验收清单：B-1（热/冷）、B-2、D-2。
- **Phase 2 · 内存极致（D-1/D-2 收尾）+ 剩余 P-A**：在 Phase 1 基线之上做峰值内存收敛与常驻回落收尾，
  并承接主线程帧预算（B-3）与可选的大目录滚动（D-3）。真机验收清单：D-1、D-2、B-3、D-3(optional)。
- **Phase 3 · P-B 离线 AI（B-1、B-2）**：完成 AI 自动出片辅助与导入批量预分级两项能力，
  验收=能跑 + headless `[ai]` 门控单测绿，明确**不强设吞吐/耗时门槛**（B-3/B-4 标 optional）。
- **Phase 4 · LR 移除落地（对应 04 文档）**：按 ADR-0001 移除 LR 读 + 写路径，数据契约以
  `.lrp`/自有 sidecar 自洽。依赖产品决策（一次性迁移/引导方案），完成后 README/ARCHITECTURE
  更新为"自成体系、不兼容 LR"。

### 3.2 真机（独立 GPU）验收说明

P-A 各锚点需在基准机器（x64，独立 GPU 可选）上留存**实测记录**。方向级验收口径（见 master
Acceptance criteria）：P-A 的 B-1/B-2/B-3/D-1/D-2 在真机有实测记录，**≥4/5 达成**即视为 P-A
里程碑成立（不达标项给出差距与后续计划）。当前沙箱 GPU 受限已在 competitive spec 记录，
真机验收依赖独立 GPU 可用的构建机。

## 4. 关联文档

以下文档锚定同一份 master 定位（optimization-positioning-spec.md），是本路线图各阶段的交付物载体：

| 文档 | 锚定主题 |
|------|----------|
| 01-PA-performance.md | P-A 性能极致：B-1/B-2/B-3 与内存 D-1/D-2/D-3(optional) 锚点的落地与真机验收 |
| 02-PB-offline-ai.md | P-B 离线 AI：B-1 自动出片辅助、B-2 导入批量预分级（能跑 + `[ai]` 单测绿） |
| 03-PD-stability.md | P-D 稳定与工程债：flaky / 泄漏 / 并发支撑项，回归绿 |
| 04-drop-lr-compat.md | 自成体系：LR 兼容路径（读 + 写）的移除与自有 `.lrp`/sidecar 自洽 |

每根支柱已细化为 **Milestone 节点文档**（子文件夹 `pillars/<0x>/`，编号 `M<n>-[slug].md`）。
主文档据此索引：阶段 → 支柱 → 节点：

| 支柱 | 位置 | 节点 | 承接锚点 / 能力 |
|------|------|------|-----------------|
| P-A | [pillars/01-pa](pillars/01-pa/) | [M1-zero-wait](pillars/01-pa/M1-zero-wait.md) | B-1 1:1 零等待 |
| P-A | 同上 | [M2-edit-feel](pillars/01-pa/M2-edit-feel.md) | B-2 / B-3 编辑手感 |
| P-A | 同上 | [M3-memory](pillars/01-pa/M3-memory.md) | D-1 / D-2 内存极致 |
| P-A | 同上 | [M4-library](pillars/01-pa/M4-library.md) | D-3(optional) 大目录 |
| P-B | [pillars/02-pb](pillars/02-pb/) | [M1-auto-develop](pillars/02-pb/M1-auto-develop.md) | B-1 AI 自动出片 |
| P-B | 同上 | [M2-batch-pregrade](pillars/02-pb/M2-batch-pregrade.md) | B-2 批量预分级 |
| P-D | [pillars/03-pd](pillars/03-pd/) | [M1-reliability](pillars/03-pd/M1-reliability.md) | S1 / S3 稳定加固 |
| P-D | 同上 | [M2-concurrency](pillars/03-pd/M2-concurrency.md) | S2 / S4 并发 + 大库 |
| LR | [pillars/04-lr](pillars/04-lr/) | [M1-remove-read](pillars/04-lr/M1-remove-read.md) | 移除 LR 读取 |
| LR | 同上 | [M2-remove-write](pillars/04-lr/M2-remove-write.md) | 移除 LR 写入 |

> 说明：01/02/03 依 pillar 成立，供各阶段持续沉淀；04 独立成册，因其是一次性破坏性改动，
> 且需要产品决策先行（迁移/引导方案未定，见 master Open questions）。
> 节点文档锚点数字均继承 master（详见 `.claude/artifacts/designs/optimization-milestones.md`）。

## 5. 验收口径（方向级，转述 master）

- AC-1 P-A 的 B-1(≤175ms)/B-2(≤80ms)/B-3(≤16ms 帧)/D-1(≤1.2GB)/D-2(≤400MB) 在真机有实测记录，
  ≥4/5 达成即视为 P-A 里程碑成立（不达标项给出差距与后续计划）。
- AC-2 P-B 的 B-1 + B-2 均有落地代码 + headless `[ai]` 门控单测绿。
- AC-3 移除 LR 兼容路径（读+写）后，全量 headless 回归绿；`.lrp`/自有 sidecar 往返自洽。
- AC-4 README/ARCHITECTURE 更新为"自成体系、不兼容 LR"的定位表述。

## 6. Open questions

- 移除 LR 读取对**现有用户数据**的一次性迁移/引导方案（转化工具 or 仅提示）未定 —— 需产品决策；
  Phase 4 前置依赖（转述 master Open questions 与 ADR-0001 Consequences）。