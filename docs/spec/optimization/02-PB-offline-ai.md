# P-B 离线 AI 支柱（细化 Spec）

> Status: DRAFT
> Last updated: 2026-09-22
>
> 本文档是 [optimization-positioning-spec.md](../optimization-positioning-spec.md)（master）中
> **P-B 离线 AI** 支柱的细化文档，纯规格，不含实施计划。上游输入：
> - 定位 master：[optimization-positioning-spec.md](../optimization-positioning-spec.md)（P-B 章节、AC-2）
> - 既有 AI 基建：[competitive-optimization-implementation-spec.md](../competitive-optimization-implementation-spec.md)
>   （P1-1 DirectML、P2-3 AI 蒙版扩展、§8 编码/文档约定）
> - 术语：[CONTEXT.md](../../CONTEXT.md)

## 1. 支柱目标

打造"**隐私 + 本地**"的差异化特色：所有 AI 能力都在本机运行、无需联网，用户原始 RAW
数据不离开设备。P-B 以**强化"离线批量生产力"与"本地自动出片"**为两大抓手，呼应产品定位
"大图零等待、常驻轻量、离线 AI 的本地 RAW 编辑器"。

**验收基调**（继承 master，不强设性能门槛）：**能跑 + headless `[ai]` 单测绿**。
P-B 明确**不设吞吐/耗时性能门槛**——不承诺"某任务 N 秒内完成"这类量化锚点（对比 P-A 的
量化锚点风格）；只要求功能可用、模型正确注册与校验、headless 门控测试通过。

本支柱的 In-scope 能力为 **B-1（AI 自动出片辅助）+ B-2（导入批量 AI 预分级）**；B-3/B-4 为
optional，不作承诺（见 §5、§6）。

## 2. 支柱能力与接线点

### 2.1 B-1 AI 自动出片辅助

**能力定义**：本地一次自动建议 **曝光 / 对比 / 白平衡 / 裁剪**，并支持"**1 键应用到参数堆栈**"
一键落地为用户可撤销的编辑。

- 属**参数建议**（对调整参数堆栈给出建议值），**非生成式**（不产出新像素、不做修复/增强）。
- B-1 双击现有 `estimateWhiteBalance`（`src/engine/Pipeline.*`，返回估计的 WB 增益）的思路：
  自动出片 = 多条此类**轻量统计算法** + 可信任规则组合成一份参数建议；只有在确定性算法方案
  不足时才考虑引入保留级本地模型，避免掉进"生成式/大模型"泥潭。
- 建议是一次性的：一条"智能建议"生成一组调整参数（曝光/对比/WB/裁剪），用户可一键套用到当前
  参数堆栈，或放弃。
- 一键应用必须落在既有**非破坏性编辑**路径上，进编辑历史、可撤销，符合现有 `edit_history` / 快照 /
  撤销体系，不得绕过参数堆栈直接改像素。

**候选接线点**：

| 接线点 | 位置 | 说明 |
|---|---|---|
| Development 自动按钮 | `src/ui/DevelopPanel.*` + `src/ui/MainWindow.cpp` | 呼应既有「自动」按钮；新增触发"AI 自动出片建议"的动作与结果预览 |
| 参数堆栈 | `src/engine/Adjustments.*`（曝光/对比/WB/裁剪字段） | 建议值写入参数堆栈后按常规 `commitAdjustments` 提交，走撤销/历史 |
| 预览管线 | `src/engine/Pipeline.*` + `src/ui/PreviewWorker.*` | 建议生成后复用现有预览管线实时出片，用户可比对"自动 vs 无编辑" |

**建议输入剖面**：当前图的线性/未做调整的预览缓冲（可降采样），输出一组参数建议；不修改原缓冲
（沿用管线的非破坏性约束）。

### 2.2 B-2 导入批量 AI 预分级

**能力定义**：导入时在**后台批量**运行既有 AI 模型（去噪 / 主体分离 / 质量分），给胶片栏对应缩略图
打出标志，强化"**离线批量生产力**"。

- 纯本地：批量打分与标注全部在本机后台线程完成，不中断导入主流程、不阻塞 UI。
- 结果语义：为每张导入图产出一条轻量标记（如：有主体 / 质量分 / 是否建议开自动出片），**不改写入
  像素、不写回 RAW**；预分级信息可选落入 catalog 元数据供后续筛选/排序。
- 复用既有模型（manifest id），避免新增重型模型：
  - 去噪/降噪：`nafnet-denoise`（NAFNet）
  - 主体分离：`rmbg2-subject`（RMBG-2）
  - 天空分割：`skyseg-sky` / `u2net-sky`（SkySeg / U²Net）
  - 质量分：可复用轻量 embedding 特征（`mobilenetv3-embed`）产出特征/参考分，或先用确定性
    Luma/边缘/清晰度统计兜底；**质量分的具体口径在实施时定**，不强制引入新模型。

**候选接线点**：

| 接线点 | 位置 | 说明 |
|---|---|---|
| 导入后台线程 | `src/ui/ImportWorker.*`（`importPaths` Stage 流程） | 落盘/入库后在后台队列串行跑预分级任务，不能阻塞导入主流程 |
| Filmstrip 标志 | 胶片栏/缩略图（`src/ui/` 相应控件） | 依据预分级结果给缩略图打标志（非破坏性标注，仅 UI 呈现） |
| catalog 元数据 | `src/catalog/` | 可选：把预分级结果/质量分落元数据，供筛选排序复用 |
| AI 推理复用 | `src/engine/AiInference.*`、`src/engine/AiDenoiseProcessor.*`、`src/engine/AiMaskGenerator.*` | 复用既有会话/预处理/推理封装，新增预分级专用轻量调用路径 |

> B-1 与 B-2 共享的底座：`ModelManager`（模型 manifest + SHA256 校验）、`AiInference`
> （ONNX Runtime session / DirectML provider / CPU 回退）、`Sha256`（`src/utils/Sha256.*`）。

## 3. 复用既有 AI 基建的约束（强制）

实现 B-1 / B-2 必须遵守与既有 AI 基建一致的约定，不得另起炉灶：

| # | 约束 | 说明 |
|---|---|---|
| 1 | **模型必须经 `ModelManager` manifest 注册 + SHA256 校验** | 新增任何 ONNX 模型条目必须进 `src/app/ModelManager.cpp` 与 `resources/models/manifest.json`，含 id + SHA256；未注册/校验失败的模型不可加载（沿用 AI 基建约定）。 |
| 2 | **engine / catalog 层 Qt-free** | AI 推理、预分级、质量分、参数建议等逻辑落 `src/engine/` / `src/catalog/`，不引入 Qt 依赖；UI 仅做展示层（DevelopPanel / Filmstrip / ImportWorker 除外——但 `ImportWorker` 属 `src/ui/`，其编排可放 UI，像素/推理逻辑仍落 engine）。 |
| 3 | **建议/预分级不直接改像素** | 自动出片 = 写参数堆栈（非破坏性）；预分级 = 写标记/元数据。均不得原地改 RAW 缓冲。 |
| 4 | **EP 与回退沿用 DirectML + CPU** | 推理走 `AiInference` 的 provider 探测逻辑：检测到 DirectML 启用，否则回退 CPU（沿用 P1-1 已修复的大小写比对与优雅回退）。 |
| 5 | **非生成式边界** | B-1 属参数建议、B-2 属离线批量标注，都不属于对象移除/扩展等生成式 AI（该方向已在范围护栏中排除）。 |

## 4. In scope / Out of scope

### In scope

- B-1：AI 自动出片辅助 —— 曝光/对比/白平衡/裁剪建议 + "1 键应用到参数堆栈"（可撤销、进历史）。
- B-2：导入批量 AI 预分级 —— 后台批量跑既有模型（去噪/主体分离/质量分）+ 胶片栏标志。
- 配套的 headless `[ai]` 门控单测，及复用 `ModelManager` manifest 注册/SHA256、`AiInference` 的接线。

### Out of scope

- **任何生成式 AI**（对象移除、扩展、修复、创意生成）。
- **GPU/DirectML 提速的量化承诺**（本支柱不设性能门槛；DirectML 可用则顺带启用，不承诺倍数）。
- **新增重型模型**（如更大的超分/去马赛克模型）纳入本支柱必做范围 —— 列为 optional（B-3/B-4，见 §6）。
- 云/AI 服务/联网打分；人脸识别；地理标签；联机拍摄（继承 competitive spec §0 护栏）。
- 强度不收敛的"全图逐像素 AI 重排/风格化"类新能力。

## 5. 验收

**验收基调**：能跑 + headless `[ai]` 门控单测绿；**不设吞吐/耗时性能门槛**。

| 能力 | 验收项 | 验收方式 |
|---|---|---|
| B-1 | 自动出片生成一组"曝光/对比/白平衡/裁剪"建议，1 键应用到参数堆栈后，堆栈字段被正确赋值且进编辑历史/可撤销 | headless `[ai]` 单测（+ GUI 自动按钮 smoke） |
| B-1 | 建议生成不改原始预览/RAW 缓冲（非破坏性） | headless 单测断言原缓冲字节不变 |
| B-2 | 导入后对一批图在后台产出预分级结果（去噪/主体/质量分），Filmstrip 能读标志 | headless `[ai]` 单测 + 后台队列完成断言 |
| B-2 | 预分级失败/缺模型时平稳跳过单张、不中断整批导入 | headless `[ai]` 容错用例 |
| 基建 | 复用模型均以既有 manifest id + SHA256 加载通过；engine 层保持 Qt-free 构建通过 | headless 全量 + `[ai]` 组绿 |

**本条明确不作承诺**（optional，见 master "B-3/B-4 不作承诺"）：
- **B-3 本地超分 / 去马赛克增强** —— optional。
- **B-4 更多本地蒙版模型**（除现有 RMBG-2/SkySeg/MobileSAM 之外的蒙版模型）—— optional。
> optional 项只有在主项（B-1/B-2）稳定绿且资源允许时才纳入，**不因 optional 未做而视为本支柱未达成**。

## 6. 风险

| 类别 | 风险 | Mitigation / 说明 |
|---|---|---|
| 模型规模与首次下载 | 离线模型体积大（如 NAFNet 数百 MB），首次加载/下载耗时、磁盘占用 | 沿用既有 `ModelManager` 按需/延迟下载 + SHA256 校验与缓存；未下载时 B-2 平滑跳过、B-1 回退到纯统计算法。 |
| CPU 推理耗时 | 不设门槛 ≠ 无限资源；CPU 上跑全离线推理可能让批量预分级或自动出片较慢 | 不承诺吞吐，但应给"无 GPU 时也能用"的体验：后台队列、降采样输入、可取消、失败/超时跳过；DirectML 可用则顺带加速。 |
| 与既有 AI 蒙版/去噪共存 | B-1/B-2 新路径可能与 `AiMaskGenerator`（蒙版）、`AiDenoiseProcessor`（去噪）共享 `AiInference` session/资源 | 复用 `getOrCreateSession` 式会话管理与同一 manifest 校验；避免并发资源竞争（后台预分级与前台蒙版需串行/隔离调度）。 |
| 非破坏性契约被绕过 | 建议或预分级误写原图 | 强化约束 §3 之 3；验收以"原缓冲字节不变"断言兜底。 |
| 数据库/目录膨胀 | 预分级元数据写入 catalog 若失控会膨胀/慢 | 预分级结果以轻量标记/分值为主，落元数据增量可选、可清理。 |

## 7. 与 master 的对账

- 支柱验收基调：与 `optimization-positioning-spec.md` §P-B + AC-2（B-1+B-2 落地 + headless `[ai]` 绿）一致。
- optional B-3/B-4：与 master"不作承诺"一致，本文件 §5 显式声明。
- P-D（稳定工程债）为支撑项，本支柱不重复规划；AI 相关 flaky/泄漏归 P-D 范畴。

> 仓储约定：落地时新增 ONNX 模型须经 `ModelManager` manifest 注册 + SHA256；遵循
> `competitive-optimization-implementation-spec.md` §8 编码与文档约定（Doxygen、Qt-free、中文正文）。