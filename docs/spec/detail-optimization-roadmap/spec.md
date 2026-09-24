# 优化方向分阶段分文档细化 Spec

## Why

上游已通过 dev-grill-docs 产出定位主张（docs/spec/optimization-positioning-spec.md）与
ADR-0001（弃 LR 兼容）：对标 Lightroom Classic 功能密度，押"纯 C++ 高性能 + 离线 AI"，
三大支柱 **P-A 性能极致 > P-B 离线 AI > P-D 稳定工程债**，并含 5 个量化性能锚点。
该定位目前只是一份战略总纲，尚未按阶段拆分成可逐步落地的细化文档。本 change 把这份定位
**分阶段、分文档**细化为一套结构化的路线图 + 每支柱专属规格文档，作为后续 dev-plan/dev-tdd 的输入接入点。

## What Changes

- 在 `docs/spec/optimization/` 下新建一套**阶段化细化文档**，锚定已定稿的 master 定位文档：
  - `00-ROADMAP.md` —— 全路线图：阶段划分、支柱排序、依赖关系、里程碑与真机验收窗口。
  - `01-PA-performance.md` —— P-A 性能极致：B-1..D-3 锚点逐条落到子阶段、范围边界、量化验收方法。
  - `02-PB-offline-ai.md` —— P-B 离线 AI：B-1 自动出片 + B-2 批量预分级的能力细化、接入既有模型链、验收（能跑+单测绿）。
  - `03-PD-stability.md` —— P-D 稳定工程债：flaky/泄漏/并发支撑项的优先级与回归要求。
  - `04-drop-lr-compat.md` —— LR 兼容移除专项：范围（读+写）、移除点清单、`Open question`（既有用户数据迁移方案）的落地。
- 各细化文档**只细化、不改动** master 定位主张与 ADR-0001 的结论（consistency 约束）。
- 更新 master 文档的"下一步"与 `docs/ARCHITECTURE.md`/`README.md` 的定位表述引用，保持不漂移。

## Impact

- 受影响文档（新建）: `docs/spec/optimization/00..04`（见下）
- 受影响文档（微改）: `docs/spec/optimization-positioning-spec.md`（补"next step"指针）、`README.md`/`docs/ARCHITECTURE.md`（定位表述同步）
- 受影响决策: ADR-0001（本 change 不改动其结论）
- 不改代码: 本 change 纯文档交付，不触碰 src/catalog

## ADDED Requirements

### Requirement: 阶段化路线图文档
系统 SHALL 提供 `00-ROADMAP.md`，将三大支柱按阶段拆分并标注依赖与真机验收窗口。

#### Scenario: Success case
- **WHEN** 读者打开 `00-ROADMAP.md`
- **THEN** 能看到每个阶段的 支柱归属、前置依赖、交付物文件名、以及需真机（独立 GPU）验收的锚点清单

### Requirement: 分支柱细化文档
系统 SHALL 为每支柱提供独立文档，且不修改 master 结论。

#### Scenario: P-A/P-B/P-D 各有专文
- **WHEN** 打开 `01-PA-performance.md` / `02-PB-offline-ai.md` / `03-PD-stability.md`
- **THEN** 每篇把该支柱的 in-scope/out-of-scope、子阶段、验收方法（量化锚点或"能跑+单测绿"）写清
- **AND** 三者与 `optimization-positioning-spec.md` 的锚点数字/优先级完全一致（consistency）

### Requirement: LR 移除专项文档
系统 SHALL 提供 `04-drop-lr-compat.md`，把 ADR-0001 落实为可执行移除清单。

#### Scenario: 移除清单与迁移方案
- **WHEN** 打开 `04-drop-lr-compat.md`
- **THEN** 列出所有 LR 兼容代码路径（XMP/crs、sidecar、LR 预设 `.xmp`/`.lrcat`）的移除点
- **AND** 把"既有用户数据迁移"Open question 收敛为明确方案（一次性转化工具 or 提示引导）
- **AND** 与 ADR-0001 结论一致（读+写均移除）

## MODIFIED Requirements

### Requirement: master 定位文档新增下一步指针
在 `docs/spec/optimization-positioning-spec.md` 末尾追加"下一步：见 docs/spec/optimization/00-ROADMAP.md"，
其余结论不改动。

### Requirement: 公开文档定位表述同步
`README.md` 与 `docs/ARCHITECTURE.md` 中与"自成体系、不兼容 LR"冲突的表述需核对并同步为一致。

## REMOVED Requirements

无（本 change 纯文档新增/细化，不删除既有功能需求）。