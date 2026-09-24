# M2 · 导入批量 AI 预分级（能力 B-2）

> Status: DRAFT
> Milestone: P-B M2
> Last updated: 2026-09-24
>
> 本文档是 **P-B 离线 AI** 支柱下 **B-2「导入批量 AI 预分级」** 的里程碑节点细化。
> 承接父文档：[02-PB-offline-ai.md](../02-PB-offline-ai.md)（B-2 能力定义与接线点）、
> [00-ROADMAP.md](../00-ROADMAP.md)（Phase 3 · P-B）。上游定义一律以
> [optimization-positioning-spec.md](../../optimization-positioning-spec.md)（master）为准。

## 1. 能力定义（转述父文档，不新增承诺）

导入时在**后台批量**运行既有 AI 模型（去噪 / 主体分离 / 质量分），给胶片栏（Filmstrip）
对应缩略图打出标志，强化"离线批量生产力"。

- 纯本地：批量打分与标注全部在本机后台线程完成，**不中断导入主流程、不阻塞 UI**。
- 结果语义：为每张导入图产出一条轻量标记（如：有主体 / 质量分 / 是否建议开自动出片），
  **不改写像素、不写回 RAW**；预分级信息可选落入 catalog 元数据供后续筛选/排序。
- 复用既有模型（manifest id），避免新增重型模型：
  - 去噪/降噪：`nafnet-denoise`（NAFNet）
  - 主体分离：`rmbg2-subject`（RMBG-2）
  - 天空分割：`skyseg-sky` / `u2net-sky`（SkySeg / U²Net）
  - 质量分：可复用轻量 embedding 特征（`mobilenetv3-embed`）产出特征/参考分，或先用确定性
    Luma/边缘/清晰度统计兜底；**质量分的具体口径在实施时定**，不强制引入新模型。

## 2. 承载的能力细节与接线点

| 接线点 | 位置 | 说明 |
|---|---|---|
| 导入后台线程 | `src/ui/ImportWorker.*`（`importPaths` Stage 流程） | 落盘/入库后在后台队列**串行**跑预分级任务，不能阻塞导入主流程 |
| Filmstrip 标志 | 胶片栏/缩略图（`src/ui/` 相应控件） | 依据预分级结果给缩略图打标志（非破坏性标注，仅 UI 呈现） |
| catalog 元数据 | `src/catalog/` | 可选：把预分级结果/质量分落元数据，供筛选排序复用 |
| AI 推理复用 | `src/engine/AiInference.*`、`src/engine/AiDenoiseProcessor.*`、`src/engine/AiMaskGenerator.*` | 复用既有会话/预处理/推理封装，新增预分级专用轻量调用路径 |

**后台队列约束**：预分级任务必须与前台蒙版/去噪共享 `AiInference` 会话（`getOrCreateSession`
式管理），避免并发资源竞争——后台预分级与前台推理需**串行/隔离调度**（对齐父文档 §6 风险）。

## 3. 复用 AI 基建的约束

B-2 共享底座（转述父文档 §2 末尾）：`ModelManager`（模型 manifest + SHA256 校验）、
`AiInference`（ONNX Runtime session / DirectML provider / CPU 回退）、`Sha256`（`src/utils/Sha256.*`）。

- 所有复用模型沿用既有 manifest id + SHA256 加载；未下载/校验失败时**平滑跳过单张**，不中断整批导入。
- 推理走 `AiInference` 的 provider 探测逻辑：检测到 DirectML 启用，否则回退 CPU。
- 像素/推理逻辑落 `src/engine/`（Qt-free）；`ImportWorker` 属 `src/ui/`，其编排可放 UI，
  但像素与推理逻辑仍落 engine（对齐父文档 §3 约束 2）。

## 4. In scope / Out of scope

### In scope

- ImportWorker 后台队列串行预分级（去噪/主体/质量分），并产出轻量标记。
- Filmstrip 依据标记打标志（非破坏性标注，仅 UI 呈现）。
- 失败/缺模型时单张跳过、整批不中断。
- 配套 headless `[ai]` 门控单测；复用模型的 manifest 注册 + SHA256 校验通过。

### Out of scope（本节点不作承诺）

- **B-3 本地超分 / 去马赛克增强** —— optional，不作承诺。
- **B-4 更多本地蒙版模型**（除现有 RMBG-2/SkySeg/MobileSAM 之外）—— optional，不作承诺。
- catalog 元数据持久化为**可选**增量（可控、可清理），不承诺目录体积不增长。
- 任何生成式 AI；GPU/DirectML 提速的量化承诺（本支柱不设性能门槛）。

> optional 项只有在主项 B-2 稳定绿且资源允许时才纳入；**不因 optional 未做而视为本节点未达成**。

## 5. 验收基调

**能跑 + headless `[ai]` 单测绿；不设吞吐/耗时性能门槛。**

P-B 明确**不设吞吐/耗时性能门槛**——不承诺"某任务 N 秒内完成"这类量化锚点；只要求功能可用、
模型正确注册与校验、headless 门控测试通过（对齐父文档 §1 / §5、master P-B Assumptions）。
批量的"快/慢"不作为验收条件，但应给"无 GPU 时也能用"的体验：后台队列、降采样输入、可取消、
失败/超时跳过。

## 6. Acceptance criteria（二值、可验证）

- AC-1 导入一批图后，后台队列为每张可处理图产出预分级结果（去噪/主体/质量分），
      Filmstrip 能读取对应标志。 —— headless `[ai]` 单测 + 后台队列完成断言。
- AC-2 预分级不改写像素、不写回 RAW：headless 单测断言原缓冲字节不变（非破坏性契约）。
- AC-3 预分级不影响导入主流程完成——后台任务在导入落盘/入库后串行执行，主流程不阻塞。
- AC-4 预分级失败/缺模型时平稳跳过单张、不中断整批导入。 —— headless `[ai]` 容错用例。
- AC-5 复用模型均以既有 manifest id + SHA256 校验通过才加载；engine 层新增逻辑保持 Qt-free，
      headless 全量 + `[ai]` 组构建/测试绿。