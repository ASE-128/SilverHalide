# 优化方向 Milestone 拆分 Spec（支柱 → 节点 / 子文件夹）

> Status: ALIGNED
> Author: user
> Last updated: 2026-09-24

## Background

Silver Halide 优化方向已定稿（master `docs/spec/optimization-positioning-spec.md` + ADR-0001，
三大支柱 P-A 性能 > P-B 离线 AI > P-D 稳定），并细化出 `docs/spec/optimization/`
00-ROADMAP + 01/02/03/04 四篇分支柱文档。本 spec 把这套资源**再细化为每支柱多个 Milestone 节点**，
放入子文件夹，主文档提供引用索引，供后续 dev-plan/dev-tdd 按节点消费。

## In scope

- 每支柱一个子文件夹，内置多个节点文档（命名 `M<n>-<slug>.md`）。
- 主文档 00-ROADMAP 顶部新增支柱→节点引用索引表。
- 各节点文档承接对应分支柱文档中已定锚点/子阶段，不含新结论。

## Out of scope

- 不改 master / ADR-0001 已定结论与锚点数字。
- 不产生实施计划（交 dev-plan）与代码改动。
- 不做 CONTEXT.md / ADR 新增（dev-spec = --spec-only）。

## 节点布局（用户已确认，M 编号）

```
docs/spec/optimization/
├─ 00-ROADMAP.md                     # 主文档，新增引用索引
└─ pillars/
   ├─ 01-pa/  M1-zero-wait.md  M2-edit-feel.md  M3-memory.md  M4-library.md
   ├─ 02-pb/  M1-auto-develop.md  M2-batch-pregrade.md
   ├─ 03-pd/  M1-reliability.md  M2-concurrency.md
   └─ 04-lr/  M1-remove-read.md  M2-remove-write.md
```

- P-A(4)：M1 1:1 零等待(B-1) → M2 编辑手感(B-2/B-3) → M3 内存极致(D-1/D-2) → M4 大目录(D-3, optional)。
- P-B(2)：M1 AI 自动出片(B-1) → M2 导入批量 AI 预分级(B-2)。
- P-D(2)：M1 稳定加固(崩溃/泄漏/flaky) → M2 并发与目录健壮性。
- LR(2)：M1 移除读取 → M2 移除写入。

## Edge cases & risks

| Category | Notes |
|---|---|
| Boundary | 大图约定 ≥8000×6000(~50MP)；锚点数字全部继承 master，节点不得改写 |
| Failure | 节点与分支柱文档若不同步会漂移 → 主文档索引 + 各父文档顶部引用对齐 |
| Risk | 文档数量增多(主 4 根 + 10 节点)，需统一 Status 头与锚点引用规范 |

## Acceptance criteria

- AC-1 `pillars/01-pa|02-pb|03-pd|04-lr` 四个子文件夹各含 `M1..Mn` 节点文档，命名 `M<n>-<slug>.md`。
- AC-2 每个节点文档含 `Status` 头、scope（锚点/子阶段）、引用其父支柱文档、二值 AC。
- AC-3 `00-ROADMAP.md` 新增支柱→节点引用索引表，指向全部 10 节点。
- AC-4 全部节点锚点数字/优先级与 master（B-1≤175/1500ms、B-2≤80ms、B-3≤16ms、D-1≤1.2GB、D-2≤400MB、A>B>D）一致，零矛盾。

## Core entities (ontology)

| Entity | Type | Relationship |
|---|---|---|
| Pillar | 支柱 | 含多个 Milestone 节点 |
| Milestone(节点) | 文档 | 归入 pillar 子文件夹，M 编号 |
| Anchor | 量化验收 | 由 master → 父支柱 → 节点逐级继承 |
| 主文档索引 | 引用表 | 指向全部节点 |