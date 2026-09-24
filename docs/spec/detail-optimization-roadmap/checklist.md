# Checklist

- [x] `docs/spec/optimization/00-ROADMAP.md` 存在，含阶段划分、前提依赖、真机验收锚点清单
- [x] `docs/spec/optimization/01-PA-performance.md` 存在，锚点数字与 master 一致（B-1≤175/1500ms、B-2≤80ms、B-3≤16ms、D-1≤1.2GB、D-2≤400MB）
- [x] `docs/spec/optimization/02-PB-offline-ai.md` 存在，含 B-1+B-2 细化，验收=能跑+单测绿，B-3/B-4 标 optional
- [x] `docs/spec/optimization/03-PD-stability.md` 存在，列出稳定项优先级与回归要求
- [x] `docs/spec/optimization/04-drop-lr-compat.md` 存在，含移除点清单与迁移方案，结论吻合 ADR-0001
- [x] `optimization-positioning-spec.md` 已追加"下一步"指针
- [x] README.md / ARCHITECTURE.md 定位表述与"自成体系、不兼容 LR"一致
- [x] 五篇细分文档与 master/ADR 的支柱优先级（A>B>D）、锚点数字、弃 LR 结论无冲突
- [x] 本 change 未改动 src/catalog 等任何代码（纯文档交付）