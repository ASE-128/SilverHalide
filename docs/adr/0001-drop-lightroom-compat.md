# ADR 0001: 放弃 Lightroom 兼容，自成体系

Status: Accepted
Date: 2026-09-22

## Context

Silver Halide 上一波"竞品对齐优化"已把功能追平 Lightroom Classic。新定位主张
在"纯 C++ 高性能 + 离线 AI"上打差异化，且现有 XMP/crs、sidecar、LR 预设的兼容
路径带来持续的迁移与约束成本。用户明确要求：**连读取兼容也一并移除**，产品数据契约
以自有 `.lrcat→`、`.lrp`、XMP-proprietary 自洽演进，不再向 LR 对齐。

## Decision

全面放弃 Lightroom 兼容——**读与写均移除**。不再新增任何 LR 对齐投入，不再以
"能否与 LR 互导"作为验收标准；产品数据契约自成体系，用自有 `.lrp` 预设、
自有 sidecar 与目录语义演进（见 docs/spec/optimization-positioning-spec.md）。

## Consequences

- 正向：摆脱 LR 命名空间/XMP 格式/虚拟副本语义的牵制；降低维护与测试面；强化"独立、
  轻量、本地 AI"的产品身份；可自由演进数据契约。
- 负向 / tradeoff：LR 既有用户数据（预设、sidecar、目录）将无法再导入，潜在迁移
  门槛上升；移除代码是一次性破坏性改动，回退成本高。
- 后续约束：需在 1.x 提供一次性提示/引导（或转化工具，见 spec Open questions）；
  README/ARCHITECTURE 更新为"不兼容 LR"的定位表述，防止未来误加回。