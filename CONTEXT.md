# Context

## Glossary

| Term | Meaning | Notes |
|---|---|---|
| 自成体系 | 产品不再向 Lightroom 兼容，读/写均移除，数据契约以自有 `.lrp`/sidecar 演进 | 见 ADR-0001；不要再以 LR 互导做验收 |
| P-A 性能极致 | 大图(≥50MP)零等待 + 内存回落 + 主线程帧预算 | 差异化主战场，锚点见 optimization-positioning-spec |
| P-B 离线 AI | 本地自动出片 + 导入批量预分级 | 能跑即达标，不强设性能门槛 |
| P-D 稳定工程债 | flaky / 泄漏 / 并发支撑项 | 回归绿为基础 |
| 大图零等待 | 50MP 1:1 出图 ≤175ms(热) / 精帧 ≤80ms | 1.1 里程碑目标 |