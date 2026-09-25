# M1 · AI 自动出片辅助（能力 B-1）

> Status: DRAFT
> Milestone: P-B M1
> Last updated: 2026-09-24
>
> 本文档是 **P-B 离线 AI** 支柱下 **B-1「AI 自动出片辅助」** 的里程碑节点细化。
> 承接父文档：[02-PB-offline-ai.md](../../02-PB-offline-ai.md)（B-1 能力定义与接线点）、
> [00-ROADMAP.md](../../00-ROADMAP.md)（Phase 3 · P-B）。上游定义一律以
> [optimization-positioning-spec.md](../../../optimization-positioning-spec.md)（master）为准。

## 1. 能力定义（转述父文档，不新增承诺）

B-1 在本机**一次**自动建议 **曝光 / 对比 / 白平衡 / 裁剪**，并支持
「**1 键应用到参数堆栈**」——一键落地为用户**可撤销**的编辑。

- 属**参数建议**（对调整参数堆栈给出建议值），**非生成式**（不产出新像素、不做修复/增强）。
- 建议是一次性的：一条"智能建议"生成一组调整参数，用户可一键套用到当前参数堆栈，或放弃。
- 一键应用必须落在既有**非破坏性编辑**路径上，进 `edit_history` / 快照 / 撤销体系，
  不得绕过参数堆栈直接改像素（对齐父文档 §3 约束 3）。

## 2. 承载的能力细节与接线点

**实现思路**：B-1 **复用 `estimateWhiteBalance` 的思路**（`src/engine/Pipeline.*`，
返回估计的 WB 增益）。自动出片 = 多条此类**轻量统计算法** + 可信任规则，组合成一份参数建议；
只有在确定性算法方案不足时才考虑引入保留级本地模型，避免掉进"生成式/大模型"泥潭。

**建议输入剖面**：当前图的线性/未做调整的预览缓冲（可降采样），输出一组参数建议；
不修改原缓冲（沿用管线的非破坏性约束）。

| 接线点 | 位置 | 说明 |
|---|---|---|
| Development 自动按钮 | `src/ui/DevelopPanel.*` + `src/ui/MainWindow.cpp` | 呼应既有「自动」按钮；新增触发"AI 自动出片建议"的动作与结果预览 |
| 参数堆栈 | `src/engine/Adjustments.*`（曝光/对比/WB/裁剪字段） | 建议值写入参数堆栈后按常规 `commitAdjustments` 提交，走撤销/历史 |
| 预览管线 | `src/engine/Pipeline.*` + `src/ui/PreviewWorker.*` | 建议生成后复用现有预览管线实时出片，用户可比对"自动 vs 无编辑" |
| 统计算法参照 | `src/engine/Pipeline.*`（`estimateWhiteBalance`） | 曝光/对比/裁剪建议沿用同一批轻量统计算法 + 阈值规则 |

## 3. 复用 AI 基建的约束

B-1 共享底座（转述父文档 §2 末尾）：`ModelManager`（模型 manifest + SHA256 校验）、
`AiInference`（ONNX Runtime session / DirectML provider / CPU 回退）、`Sha256`（`src/utils/Sha256.*`）。

- 若引入本地模型，新增 ONNX 条目须进 `src/app/ModelManager.cpp` 与
  `resources/models/manifest.json`（含 id + SHA256），未注册/校验不加载。
- 推理走 `AiInference` 的 provider 探测逻辑：检测到 DirectML 启用，否则回退 CPU。
- 参数建议逻辑落在 `src/engine/`（Qt-free），UI 仅做展示层。

## 4. In scope / Out of scope

### In scope

- 曝光/对比/白平衡/裁剪四条建议的统计算法 + 组合逻辑。
- 「1 键应用到参数堆栈」（写字段 + `commitAdjustments`，进历史/可撤销）。
- 配套 headless `[ai]` 门控单测；可选模型按 manifest 注册 + SHA256。

### Out of scope（本节点不作承诺）

- **B-3 本地超分 / 去马赛克增强** —— optional，不作承诺。
- **B-4 更多本地蒙版模型**（除现有 RMBG-2/SkySeg/MobileSAM 之外）—— optional，不作承诺。
- 任何生成式 AI（对象移除、修复、扩展）；GPU/DirectML 提速的量化承诺（本支柱不设性能门槛）。

> optional 项只有在主项 B-1 稳定绿且资源允许时才纳入；**不因 optional 未做而视为本节点未达成**。

## 5. 验收基调

**能跑 + headless `[ai]` 单测绿；不设吞吐/耗时性能门槛。**

P-B 明确**不设吞吐/耗时性能门槛**——不承诺"某任务 N 秒内完成"这类量化锚点；只要求功能可用、
模型正确注册与校验、headless 门控测试通过（对齐父文档 §1 / §5、master P-B Assumptions）。

## 6. Acceptance criteria（二值、可验证）

- AC-1 触发自动出片后产生一组含"曝光/对比/白平衡/裁剪"字段的建议值；
      1 键应用到参数堆栈后，堆栈对应字段被正确赋值且进入 `edit_history` / 可撤销。
      —— headless `[ai]` 单测断言字段值与历史条目；GUI 自动按钮 smoke。
- AC-2 建议生成不改原始预览/RAW 缓冲：headless 单测断言**原缓冲字节不变**（非破坏性契约）。
- AC-3 透传的选项（白平衡）沿用 `estimateWhiteBalance` 思路实现，返回有限、可验证的估计值。
- AC-4 若引入本地模型，其 manifest 条目含 id + SHA256 且校验通过才加载；
      engine 层新增代码保持 Qt-free，headless 全量 + `[ai]` 组构建/测试绿。

> AC-4 在完全不引入模型、纯统计算法即可满足 AC-1~AC-3 时不强制——模型是 optional 路径。