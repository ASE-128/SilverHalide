# Tasks

- [x] Task 1: 创建 `00-ROADMAP.md` 阶段化路线图
  - [x] 1.1 定义阶段划分（如 Phase1 性能-P-A / Phase2 离线AI-P-B / Phase3 稳定-P-D），标注每阶段前置依赖
  - [x] 1.2 列出需真机（独立 GPU）验收的锚点清单（B-1..D-3）与验收机器/样本约定
  - [x] 1.3 标注每阶段交付物文件名与入口指针（指向 01/02/03）

- [x] Task 2: 创建 `01-PA-performance.md` P-A 性能细化
  - [x] 2.1 逐条细化 B-1(≤175ms热/≤1500ms冷)、B-2(≤80ms)、B-3(≤16ms帧)、D-1(≤1.2GB)、D-2(≤400MB)、D-3(optional) 到子阶段
  - [x] 2.2 明确 P-A in-scope/out-of-scope 与涉及代码域（engine/catalog/ui）
  - [x] 2.3 给出量化验收方法与回归要求（含基准机器、样本、对比基线）

- [x] Task 3: 创建 `02-PB-offline-ai.md` P-B 离线 AI 细化
  - [x] 3.1 细化 B-1 自动出片（自动曝光/对比/白平衡/裁剪建议，复用 estimateWhiteBalance 思路）能力与接线点
  - [x] 3.2 细化 B-2 导入批量预分级（后台批量去噪/主体分离/质量分 + 胶片栏标志）能力与接线点
  - [x] 3.3 明确验收 = 能跑 + headless `[ai]` 单测绿（不强设性能门槛）
  - [x] 3.4 B-3/B-4 列为 optional 不作承诺

- [x] Task 4: 创建 `03-PD-stability.md` P-D 稳定工程债细化
  - [x] 4.1 列出 P-A/P-B 依赖的稳定项（flaky/泄漏/并发）优先级
  - [x] 4.2 明确回归要求（headless 全绿）与已知既有 flaky（AiInference SIGSEGV、raw ICC 阈值）

- [x] Task 5: 创建 `04-drop-lr-compat.md` LR 移除专项
  - [x] 5.1 列出 LR 兼容代码路径（XMP/crs、sidecar、LR 预设 `.xmp`/`.lrcat`、crs 字段映射）移除点清单
  - [x] 5.2 收敛"既有用户数据迁移"Open question 为明确方案（一次性转化工具 or 提示引导）
  - [x] 5.3 确保与 ADR-0001（读+写均移除）结论一致

- [x] Task 6: 同步 master 与公开文档
  - [x] 6.1 `optimization-positioning-spec.md` 末尾加"下一步：见 docs/spec/optimization/00-ROADMAP.md"
  - [x] 6.2 核对并同步 `README.md`/`docs/ARCHITECTURE.md` 中与"自成体系、不兼容 LR"冲突的表述
  - [x] 6.3 校验五篇细化文档与 master/ADR 锚点数字、优先级一致性

# Task Dependencies

- Task 1 先行（定义阶段划分，作为其余文档的分组索引）
- Task 2 / 3 / 4 并行（分支柱编写，互不依赖）
- Task 5 独立，可并行于 2/3/4
- Task 6 依赖 Task 1–5 完成后再统一同步与核验