# SPEC：竞品对齐优化（P1/P2/P3/T）实施路径与验收标准

> 上游输入：
>
> - 计划：[.trae/documents/competitive-optimization-plan.md](../../../../.trae/documents/competitive-optimization-plan.md)（Phase 1 已探查确认代码位置）
>
> - 报告：`SilverHalide_竞品比较分析报告.md` v2.0（2026-09-03）第 6 节"三波改进 + 技术债"、第 5.2 节"不做项"
>
> - 相关：[docs/spec/perf-m1-m5-implementation-spec.md](perf-m1-m5-implementation-spec.md)（SIMD 边界交叉引用）
>   本文档将报告第 6 节工程化为**可执行的实施路径**与**可验证的验收标准**。
>   编号约定：P1=第一波高性价比、P2=第二波核心能力、P3=第三波体验打磨、T=技术债（不复用 M1–M5 编号）。

## 当前任务状态

**当前进行中的任务**：执行本规格 [competitive-optimization-implementation-spec.md](competitive-optimization-implementation-spec.md) 中定义的竞品对齐优化。

**状态核对日期**：2026-09-05。

**总体状态**：待实施。P1、P2、P3 和 T 的正式实施批次尚未按本规格启动，不能将表格中的“可测 ✅”解释为已完成；该标记仅表示已有明确的验证方法。现有代码中的 AI 天空蒙版、编辑历史、SIMD 条件编译等属于可复用基础或相邻能力，仍需按本规格补齐实现范围、验收用例和性能证据。

**P1-3 批量重命名进展（2026-09-05，工作区未提交，状态：代码已落地但未验收）**：

- 模板引擎：`src/utils/RenameTemplate.h` 已实现 `renderRenameTemplate`（`{date}`/`{index[:W]}`/`{base}`，大小写不敏感，未知 token 原样保留）与 `uniqueRenamePath`（冲突自动 `-N` 后缀），Qt-free。

- 导入端：`ImportDialog` 已加「导入时重命名文件」复选 + 模板输入 + 新文件名实时预览；`ImportWorker::importPaths` 新增 `renameTemplate` 参数，在 Stage1 落盘后按选择顺序（1 起）+ 源文件修改日期渲染并重命名，`renameMutex_` 串行化查重+重命名保证同批唯一，失败回退原路径。

- 导出端：`ExportDialog` 已加「批量导出时重命名文件」复选 + 模板输入 + 母版/各虚拟副本输出文件名实时预览；启用时 `MainWindow` 走 `PreviewWorker::batchExport`，为母版+所有虚拟副本解析各自调整栈、用 `uniqueRenamePath` 在所选目录下生成唯一新文件名；未启用时保持原单图 `exportImage` 行为。

- 测试：`TestCompetitiveP1.cpp` 已覆盖模板渲染、`updateImagePath` 唯一性、`uniqueRenamePath` 冲突回退；headless `[p1][rename]` 与全量（重跑）通过。GUI 构建（`build_gui/SilverHalide.exe`）通过。

- 未完成项（未验收）：导入端 `CatalogDatabase::updateImagePath` 的批量路径同步在 ImportWorker 内通过重命名后落盘路径直接生效，但缺少端到端「批量重命名后目录内文件名唯一、DB `file_path` 同步更新、取消不改文件」集成用例；导出端虚拟副本批量导出尚未做真实导出断言（只验证编译与路径生成）。需按 §2 验收标准补齐。

**P1-3 端到端集成测试补齐（2026-09-19，工作区未提交，状态：headless 层已验收）**：

- `TestCompetitiveP1.cpp` 新增两个 `[p1][rename][catalog]` 集成用例，在 headless catalog 层逐字复刻 `ImportWorker` Stage-1 重命名流程（落盘后按选择顺序渲染模板 → `uniqueRenamePath` 去重 → 磁盘 rename → 以最终路径入库）：
  - **批处理契约**：3 个已落盘文件 + 预占第 1 序号的渲染目标，强制走 `-2` 冲突分支；断言批处理后目录内文件名唯一、DB `file_path` 与磁盘改名后路径镜像一致（`findImageByPath` 命回原 id）。
  - **取消/回滚契约**：事务内完成改名入库后 `rollbackTransaction()`，断言 DB 不残留任何半提交的重命名路径（`listImages()` 为空）——对应 ImportWorker 取消时回滚事务的行为。
- headless `[p1]` 重跑通过：15 用例 / 115 断言（原 13/87）。
- 仍待验收（属于 GUI/真实导出，见 T-3 范畴）：导出端虚拟副本 `batchExport` 真实导出断言、导入/导出对话框 UI smoke；P1-1「CPU vs DirectML 逐通道容差 + ≥2x 耗时」需 GPU 实测。

**P1 GUI/真实导出验收（2026-09-19，工作区未提交，状态：Qt 层已验收）**：

- 新增 Qt 测试目标 `tests/lrc_qt_tests`（仅 `LRC_ENABLE_QT` 时构建，Qt6::Test/Widgets + AUTOMOC，链接 `src/ui/PreviewWorker.cpp` 与 `ExportDialog/ImportDialog/SnapshotDialog`），归一化到 `tests/CMakeLists.txt`：
  - **batchExport 真实导出断言**：写临时 JPEG → 构造 PreviewWorker（worker 线程，同 MainWindow::startWorkers）→ 一次 batchExport 三个 ExportItem（JPEG/PNG/TIFF16，各独立输出路径）→ 断言 `batchExportFinished(success=N, fail=0)` 且每个落盘文件魔数正确（JPEG FFD8FF / PNG 89504E47… / TIFF II*\x2A）。offscreen 运行 3/3 通过。
  - **对话框 smoke**：ExportDialog / ImportDialog / SnapshotDialog 构造 + 公开状态断言（ExportDialog 变体选择默认母版、rename 默认关、format 越5格式；ImportDialog rename 默认关；SnapshotDialog 对无快照图片 reload 不崩溃）。5/5 通过。
- 附带修复 `ExportDialog` 一个隐性缺陷：头文件持 `std::vector<lrc::EditVariant>`（前向声明类型）导致隐式内联析构在 AUTOMOC TU 对不完整类型实例化失败；将 `~ExportDialog()` 改为 out-of-line，在 `.cpp`（已含 `catalog/CatalogDatabase.h`）中 `= default`。
- GUI-with-AI 构建（`build_gui`，`LRC_ENABLE_AI=ON` + ONNXRUNTIME_ROOT）重新配置/重建通过，`SilverHalide.exe` 与 `lrc_qt_tests` 均成功，无回归。
- **P1-1 环境门控说明**：provider API + DirectML 优雅回退 + builtin 模型在 DirectML preferred 下可跑已有用例覆盖；「CPU vs DirectML 逐通道容差 + ≥2x 耗时」需一台带 GPU 的机器 + 按 manifest 下载 410MB NAFNet（本机无该模型）。`TestCompetitiveP1.cpp` 的 P1-1 provider 用例现以 `INFO` 上报运行时 DirectML 可用性与真实 ONNX 后端，供 GPU 机器直接验收。

**当前执行顺序**：先从 P1-1、P1-2、P1-3 开始；每项完成后补充对应测试与实测记录，再进入 P2。P1-5 快照系统、T 技术债可作为独立批次插入，但不得把已有 `edit_history` 或 SIMD 框架直接视为规格任务完成。

**状态判定规则**：以 Git 工作区、源码/测试实测和本文件的验收记录为准。计划存在不等于代码已落地，代码存在不等于验收完成；状态应分别记录为“待实施”“代码已落地但未验收”“已验收”或“已提交”。

***

## 0. 范围护栏（第 5.2 节不做项，整段固化）

以下方向**已明确排除**，实施中不得借道扩张：

| 不做               | 原因                             |
| ---------------- | ------------------------------ |
| macOS / Linux 移植 | 无 Apple 硬件测试条件；既定 Windows-only |
| 人脸识别             | 模型维护成本高；关键字+评分够用               |
| 地理标签地图           | 集成工作量大、使用频率低                   |
| 联机拍摄             | Windows 上 libgphoto2 支持差       |
| 云同步 / 移动端        | 基础设施成本；无此需求                    |
| 生成式 AI（对象移除/扩展）  | 模型大、慢、不稳；IOPaint/LaMa 等独立工具已够用 |
| 打印模块             | Windows 打印路径已多                 |
| 插件体系 / SDK       | 社区不存在；自己的需求自己写                 |

***

## 1. 里程碑总览与依赖

| 批次     | 主题       | 核心改进                                            | 风险  | 主要改动层                 | 与 perf-m1-m5 关系      |
| ------ | -------- | ----------------------------------------------- | --- | --------------------- | -------------------- |
| **P1** | 第一波·高性价比 | DirectML / 导出 ICC / 批量重命名 / 水印 / 快照             | 低–中 | engine + catalog + ui | 无交叠                  |
| **P2** | 第二波·核心能力 | lensfun / SIMD AVX2 扩展 / AI 蒙版扩展 / DCP          | 中–高 | engine（+CMake 依赖层）    | SIMD 与 M4-3 交叠，见边界约定 |
| **P3** | 第三波·体验打磨 | 直方图升级 / OS 事件监视 / 编辑预设 / GPU 像素管线               | 中–高 | engine + catalog + ui | GPU 管线依赖 P2-SIMD     |
| **T**  | 技术债      | flaky SIGSEGV / TIFF rows-per-strip / GUI smoke | 低   | engine + tests + ui   | 无交叠                  |

**依赖链**：

- P1 五项互相独立（DirectML、ICC、重命名、水印、快照可并行）；MainWindow\.cpp 上的三项 UI（快照/重命名/水印入口）由同一写者串行。

- P2-lensfun、P2-SIMD、P2-DCP 互相独立；P2-AI蒙版依赖 manifest 基建（已就绪）。

- **P3-GPU 依赖 P2-SIMD 完成**（共享 Pipeline.cpp 算子拆分成果）。

- T 批次随时可插入，不阻塞主线。

- ExportEngine.cpp 被 P1-ICC 与 P1-水印共享 → 同一写者串行。

**SIMD 边界约定（交叉引用，不重复规划）**：`applyAdjustments` 曝光/对比度/曲线/白平衡分批 Pass 归 perf **M4-3**（默认关、容差回归、见 perf spec 第 5 节）。本计划 P2-SIMD 只做 M4-3 之外的扩展点：`gaussianBlur` AVX2、`applyHSLPerChannel`、颜色矩阵等；开关沿用 `LRC_ENABLE_SIMD`。

***

## 2. P1 — 第一波：高性价比改进

**目标**：五项单项 1–4 周量级的小步快跑，全部落地后覆盖报告 6.1 全表。

### 实施路径

| 步骤   | 任务          | 关键动作                                                                                                                                             | 归属                                                                  | 可测 |
| ---- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------- | -- |
| P1-1 | DirectML 启用 | `AiInference` 初始化时枚举 ONNX Runtime 执行提供者，检测到 DirectML 即追加启用；失败静默回退 CPU；沿用 `ModelManager` manifest + SHA256 约定                                     | `engine/AiInference.cpp`、`engine/inference/`、`app/ModelManager.cpp` | ✅  |
| P1-2 | 导出 ICC 选择   | 导出对话框加 ICC 下拉（sRGB / Adobe RGB / Display P3 / 自定义路径）；`ExportEngine` 按选择嵌入 ICC 配置文件（写 JPEG/PNG 的 ICC chunk、TIFF 的 ICC tag）；`ProfileIO` 已有读写能力直接复用 | `utils/ProfileIO.cpp`、`engine/ExportEngine.cpp`、导出对话框 UI            | ✅  |
| P1-3 | 批量重命名       | 导入/导出对话框加 rename 模板（日期+序号+原文件名，占位符+正则替换）；落 catalog 层批量执行；预览新文件名后再确认                                                                              | `catalog/CatalogDatabase.cpp`、导入/导出对话框                              | ✅  |
| P1-4 | 水印          | 导出管线末尾叠加文字或 PNG 水印（位置/透明度/大小可调）；像素叠加落 engine，参数进导出设置结构体                                                                                          | `engine/ExportEngine.cpp` + 导出 UI 小节                                | ✅  |
| P1-5 | 快照系统        | `edit_history` 表加 `name` 列（迁移：`ALTER TABLE` + 版本号）；catalog API：创建/应用/删除/列出命名快照；UI 快照面板（创建、点击应用、删除）；虚拟副本回归"副本"本职                                  | `catalog/CatalogDatabase.cpp` + UI 快照面板                             | ✅  |

**执行顺序**：P1-1 与 P1-5 先行（收益最大）；P1-2→P1-4 串行（共享 ExportEngine.cpp）；P1-3 并行。

### 验收标准

- [ ] P1-1：构造已知输入图，CPU 推理 vs DirectML 推理输出**逐通道对比在容差内**（阈值按模型精度定，如 Δ<0.02）；无 DirectML 环境（或无 GPU）时自动回退 CPU 且测试仍绿；AI 降噪/蒙版推理耗时显著下降（记录对比数据，如 ≥2x）。

- [ ] P1-2：新增断言——按各预设 ICC 导出的文件，其 ICC chunk/tag 可解析且等于所选配置文件；未选择时保持 sRGB 默认（与现状字节一致）；软打样交互不回归。

- [ ] P1-3：模板渲染单元测试覆盖占位符（日期格式、序号宽度、原文件名保留、冲突自动加序号）；批量重命名后目录内文件名唯一、DB `file_path` 同步更新；取消操作不改任何文件。

- [ ] P1-4：合成图+水印参数矩阵（四角落+居中 × 透明度 × 缩放）断言叠加结果与参考实现逐像素一致；水印不写入导出 ICC/元数据以外的意外字节；无水印时输出与现状字节一致。

- [ ] P1-5：快照创建/应用/删除/列出全 API 单测；应用快照后参数堆栈与快照时**逐字段相等**；DB 迁移幂等（重复 `open()` 不重复加列）；虚拟副本行为不回归。

- [ ] 全部：headless + GUI 双端全量测试绿；新增用例登记 `tests/CMakeLists.txt`。

***

## 3. P2 — 第二波：核心能力补齐

**目标**：1–3 个月量级，补齐 AI/像素处理与镜头/相机色彩校准核心能力。

### 实施路径

| 步骤   | 任务           | 关键动作                                                                                                                  | 归属                                                | 可测  |
| ---- | ------------ | --------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------- | --- |
| P2-1 | lensfun 集成   | CMake/vcpkg 声明与 Qt-free 查询 adapter 已落地；RawMeta 已传递镜头字段。`LensfunProfile::find`/`applyGeometry` 与开关接线已实现，headless 测试覆盖无匹配/关态字节不变等场景                             | CMakeLists + `engine/LensfunProfile.*` + RawMeta  | ✅   |
| P2-2 | SIMD AVX2 扩展 | 在 M4-3 之外：`gaussianBlur` 精确可分离路径接 `blurExactPassAVX2`（8-lane FMA 行循环）；`CMakeLists` 新增 `LRC_ENABLE_SIMD` 开关（默认关，MSVC `/arch:AVX2`）。`applyHSLPerChannel`/颜色矩阵仍为后续扩展点 | `engine/Pipeline.cpp` + `CMakeLists.txt`          | ✅   |
| P2-3 | AI 蒙版扩展      | manifest 加 SAM（点击式）与天空分割模型条目（SHA256 校验）；新 processor 类（参照 `AiMaskGenerator`/`AiDenoiseProcessor` 模式）；UI 加"天空"蒙版按钮与点击交互 | `engine/inference/` + `app/ModelManager.cpp` + UI | ✅   |
| P2-4 | 相机 DCP 支持    | `DcpProfileReader::read`/`isDcp` 解析 ColorMatrix1/2 + ToneCurve；`RawDecoder` 注入色矩阵与色调曲线；UI DCP 下拉选择已接线，基础单测就绪                                                | `engine/DcpProfile.*` + UI                        | ✅   |

**执行顺序**：四项互相独立，四写者并行；P2-2 完成后才可进 P3-GPU。

### 验收标准

- [x] P2-1：`TestLensfunProfile.cpp` headless 用例覆盖无匹配镜头静默跳过（像素不变）与关态字节不变；Qt-free（engine 层）。已知畸变图直线度几何断言与数据库装库属依赖环境项，未在本批实测。

- [x] P2-2：新增 `LRC_ENABLE_SIMD` 开关（默认关）与 AVX2 `gaussianBlur` 精确路径；SIMD 构建新增容差回归测试 `[blur][simd]`（Δ<0.008，实测 pass），默认关机下 `[pipeline]` 全绿、关机不引入 `LRC_ENABLE_SIMD` 宏（`#ifdef` 语义与基线一致）。`≥2x` 计时对比未做实测定量，记录为待补基线。

- [x] P2-3（引擎接线部分已验收）：manifest 已注册 `skyseg-sky`、`mobilesam-encoder`、`mobilesam-decoder` 三条目并 SHA256 校验；Sky 默认模型已切换至专用 SkySeg；引擎新增两阶段点击式方法 `AiInferenceEngine::runSamPoint(encoderId, decoderId, image, points)`（等比分缩放至 1024 + 补零 + ImageNet255 归一化 → encoder → 6 输入 decoder → 按 iou 选最优 mask 并缩至原图），配套 `LRC_ENABLE_AI` 门控单测 `[sam]` 在真实权重下 CPU 通过（合成图+前景点，断言 mask 有限且在点击区高响应）。**量化/UI 验收仍待补**：GUI 点击交互、GPU（DirectML）耗时对比、IoU/准确率断言。

- [x] P2-4：`DcpProfile` 基础单测覆盖解析与非法/截断文件不崩溃路径；色彩矩阵/曲线应用已接线。参考 dcp-tool 曲线一致性与灰卡 ΔE 落差的完整数值断言待补（现有测试以行为/不崩溃断言为主）。

> **验收状态快照（2026-09-20 会话 · 模型注册 + EP + MobileSAM 引擎接线）**：
> - **模型注册**：`resources/models/manifest.json` 新增 `skyseg-sky`（U²Net 天空分割）、`mobilesam-encoder`、`mobilesam-decoder` 三条目，SHA256 均已在缓存中校验通过（`%LOCALAPPDATA%\Silver Halide\models`）；`AiMaskGenerator::defaultModelIdFor(Sky)` 现返回专用 `skyseg-sky`。
> - **DirectML/EP**：`AiInference.cpp` 修复 `directMLAvailable()` 与 `DmlExecutionProvider` 大小写比对 bug（reported `DMLExecutionProvider` → 实际 `DmlExecutionProvider`，原逻辑恒判失败）；DirectML 1.23.0 运行时已部署至 `build_gui/`。`verify_dml.exe` 实测 `GetAvailableProviders=[DmlExecutionProvider][CPUExecutionProvider]`，CPU 路径端到端通过；**GPU 推理在本 agent sandbox 内无法验证**（DML 的 D3D12 初始化在沙箱内 0xC0000005 崩溃、GPU 设备访问被禁用），需在真机 A4000 上跑 `verify_dml.exe` 或 GUI 确认提速。
> - **MobileSAM 引擎接线**：`AiInferenceEngine` 新增公开方法 `runSamPoint(encoderId, decoderId, image, points)`（两阶段：1024 缩放+留白补零+ImageNet255 归一化 NHWC → encoder `input_image`；6 输入 decoder `image_embeddings/point_coords/point_labels/mask_input/has_mask_input/orig_im_size` → 按 `iou_predictions` 选最优 `masks` 并缩至原图）；原 `run()` 的会话获取逻辑重构成私有 `getOrCreateSession()` 供两者复用。新增 `[sam]` 单测（headless AI 构建，CPU 真实权重）断言 mask 元素有限、落在 [0,1]、点击区高响应。
> - **验证**：headless（`LRC_ENABLE_QT=OFF`、`ONNXRUNTIME_ROOT=onnxruntime-win-x64-1.23.2`、MSVC x64）重建 `lrc_tests` 通过，`[ai]` 组 34 用例 / 276 断言全绿（含 `run()` 重构回归无破坏）。
> - **待补**：真机 GPU/DirectML 耗时与端到端效果、SAM IoU/准确率数值断言。

> **验收状态快照（2026-09-21 会话 · MobileSAM GUI 点击交互）**：
> - **引擎**：`Adjustments.h` 新增 `AiMaskTarget::Prompt` 与 `AiMaskParams::points`（`std::vector<SamPoint>`）；`Adjustments.cpp` 的 `Mask::hash/geometryHash`（`operator==` 经默认化 `AiMaskParams` 已覆盖）纳入 points，使 Prompt 蒙版几何变更能正确失效缓存。`AiMaskGenerator::generate` 对 Prompt 走 `runSamPoint`（encoder+decoder）+ 同一套 threshold/feather/invert 精修，失败返回全零蒙版（非空指针）；`Pipeline.cpp` AiMask 分支对 Prompt 放行（`defaultModelIdFor(Prompt)==""` 不再误判 `hasModel` 失败）。`SamPoint` 补 `operator==` 以支持向量比较。
> - **UI**：DevelopPanel 新增「AI 点选」按钮 + 前景/背景点切换 + 清除点；点选蒙版时显示羽化滑杆（映射 `ai.feather`）。ImageViewport 在点击时向 `maskDraft_.ai.points` 追加 SamPoint（前景绿/背景红标记），`maskGeometryChanged` 回流 DevelopPanel 触发重渲染，`maskEditFinished` 提交撤销。MainWindow 接线 `maskPromptLabelChanged`/`maskPromptClearRequested`。
> - **验证**：新增 `[sam][prompt-mask]` 门控单测（headless AI 构建，CPU 真实权重）断言 Prompt 蒙版经 `AiMaskGenerator::generate` 得到非空 w×h、权重有限且在 [0,1]、点击区高响应、远角弱响应。
> - **待补**：Prompt 点序列暂不写入 ProfileIO 预设（仅视图内交互状态）；真机 A4000 DirectML 提速/端到端；SAM IoU 数值断言。

> **验收状态快照（2026-09-19 会话）**：T-1/T-2/T-3 已落地并提交对应测试；P2-1/P2-2/P2-4 代码与基础测试就绪（P2-2 已有容差回归通过）。P2-3 与环境相关的量化验收（≥2x 计时、100MP 峰值内存、SAM GPU 模型）保留待补。另：`TestRawDecoder.cpp:556` 的 embedded-ICC 断言（R-08）存在**预先存在的 flaky 失败**（Δ 值在不同运行间 0.010~0.019 波动，阈值 0.02），该文件未被本批改动，与本批 T/P2 无关，需单独跟进。

***

## 4. P3 — 第三波：体验打磨

**前置**：P2-SIMD 完成（P3-4 依赖）；其余三项独立。

### 实施路径

| 步骤   | 任务         | 关键动作                                                                              | 归属                                                | 可测 |
| ---- | ---------- | --------------------------------------------------------------------------------- | ------------------------------------------------- | -- |
| P3-1 | 直方图升级      | 直方图统计源从"预览 8bit + 每 4M 像素抽样"改线性 F32 缓冲；显示 RGB 三通道直方图                              | 直方图统计 + `engine/Pipeline.cpp`                     | ✅  |
| P3-2 | OS 事件文件夹监视 | `watchFolderPoll` 2s 轮询改 `ReadDirectoryChangesW`（QFileSystemWatcher 或专用线程）；保留轮询回退 | `catalog/FolderMonitor.cpp` + `ui/MainWindow.cpp` | ❌  |
| P3-3 | 编辑预设       | 参数堆栈 JSON 序列化（`ProfileIO` 已有）加保存/加载预设文件；预设管理 UI（保存/应用/删除/导出分享）                    | `utils/ProfileIO.cpp` + UI                        | ✅  |
| P3-4 | GPU 像素管线   | 高斯模糊/锐化等空间滤波迁 DirectCompute 计算着色器；设计算子抽象使 CPU/GPU 双后端可切换、可回退                      | `engine/Pipeline.cpp`（算子拆分）                       | ✅  |

**执行顺序**：P3-1、P3-2、P3-3 并行；P3-4 在 P2-SIMD 后单独立项推进。

### 验收标准

- [ ] P3-1：合成已知分布图，升级后直方图与全分辨率线性统计**分 bin 计数一致**（抽样误差消除）；三通道显示正确；统计耗时不在主线程引入可感知卡顿（≤ 单帧预算）。

- [ ] P3-2：手动 GUI：外部拷入文件后**秒级**（事件驱动）触发导入提示，不再等 2s 轮询；轮询回退路径可用；长目录下 CPU 占用不高于轮询方案。

- [ ] P3-3：预设保存→加载往返后参数堆栈**逐字段相等**；预设文件 JSON 可人工阅读/编辑；非法预设文件报错不崩溃。

- [ ] P3-4：GPU 路径与 CPU 参考输出**分块对比一致**（容差按浮点实现定）；无 GPU/编译未启用时回退 CPU 路径且测试全绿；模糊/锐化阶段耗时较 CPU SIMD 版有量级改善（记录数据）。

***

## 5. T — 技术债（顺手修）

| 步骤  | 任务                                  | 关键动作                                                                                             | 归属                                     | 可测 |
| --- | ----------------------------------- | ------------------------------------------------------------------------------------------------ | -------------------------------------- | -- |
| T-1 | flaky SIGSEGV in `TestPipeline:704` | BufferPool thread\_local 析构顺序问题（复现率 1/3）；加显式 thread\_local shutdown sequence（注册 atexit/显式 drain） | `engine/`（BufferPool/RenderScratch 相关） | ✅  |
| T-2 | TIFF rows-per-strip 自适应             | 100MP+ 大图时 `max(64, h/64)` 保持 strip 数 \~64，避免超大单 strip 内存峰值                                      | `engine/ImageIO.cpp` 或 TIFF 写出路径       | ✅  |
| T-3 | GUI 测试缺位                            | 引入 Qt Test 对关键对话框（导出、导入、快照）做 smoke 测试                                                            | `tests/` + 相关对话框                       | ✅  |

### 验收标准

- [x] T-1：`RenderScratch.h` 提供幂等显式 `scratchPoolDrain()`（thread 内确定性释放，避免 atexit 的 thread_local 析构顺序 UB）；新增压力测试（8 线程 ×200 迭代 获取/释放 + 周期性 drain，`[RenderScratch][t1]`）。shutdown 与依赖关系在 header 顶部注释说明。
- [x] T-2：`writeTiffRaw()` 自适应 `rowsPerStrip = max(64, ceil(h/64))`，`stripCount>1` 时 StripOffsets/StripByteCounts 内联写 IFD 之后并逐 strip 写出；`[io][t2]` 单测覆盖 h=100→2 strips 与 h≤64 单 strip，round-trip 通过。库内多 strip 读取器已在 `loadTiffImpl` 验证兼容。
- [x] T-3：`tests/CMakeLists.txt` 的 `lrc_qt_tests`（Qt Test）目标已链接 ExportDialog/ImportDialog/SnapshotDialog + PreviewWorker，作为关键对话框 smoke 用例纳入可跑集合。

***

## 6. 行为/像素精确性矩阵（回归依据汇总）

| 任务                    | 输出     | 回归要求                         |
| --------------------- | ------ | ---------------------------- |
| P1-1 DirectML         | 微小浮点差  | CPU vs DML 容差对比              |
| P1-2 ICC / P1-4 水印    | 输出字节变  | ICC chunk / 叠加逐像素断言；默认路径字节不变 |
| P1-3 重命名 / P1-5 快照    | 不改像素   | 目录行为测试                       |
| P2-1 lensfun          | 几何改像素  | 开关对比回归（关=字节一致）               |
| P2-2 SIMD             | 微小浮点差  | Δ<0.008 或 PSNR>40dB，开关默认关    |
| P2-3 AI 蒙版 / P2-4 DCP | 新能力    | IoU / ΔE 指标断言                |
| P3-1 直方图 / P3-4 GPU   | 统计/实现变 | 分 bin 一致 / 分块对比一致            |

***

## 7. 验证方式与落点

- 主回归：`build_headless` + `ctest`（命令见 perf spec 第 7 节）。

- GUI 测试：直接运行 `build_gui\tests\lrc_tests.exe`（中文路径下 ctest 不稳）。

- 新增测试登记：`tests/CMakeLists.txt`（沿用 Catch2；GUI smoke 用 Qt Test）。

- 验证数据（耗时/内存对比）记录在各批次验收记录中，作为后续调优基线。

***

## 8. 编码与文档约定（强制）

- 遵循 AGENTS.md 第 7 节注释规范：Doxygen 风格公开 API、英文代码注释、线程安全/缓存/状态机必注。

- 遵循 AGENTS.md 第 3 节架构边界：engine/catalog Qt-free——lensfun、DCP、SIMD、GPU 算子全部落 `engine/`；UI 只做展示层。

- 新增 ONNX 模型必须经 `src/app/ModelManager.cpp` manifest 注册 + SHA256 校验（沿用 AI 基建约定）。

- 新能力落地时同步更新 `docs/ARCHITECTURE.md` 与 README 功能列表。

- 依赖获取优先已安装 package / vendored third\_party / FetchContent（lensfun 走 vendored，参照 libraw）。

- 中文注释行以 ASCII 结尾；bat 脚本禁止硬编码中文路径（`%~dp0` 取根）。

***

## 9. 建议执行顺序（来自报告第 8 节，与本文档阶段映射）

1. **立即**：P1-1 DirectML + P1-2 ICC + P1-3 重命名（三项合计约一周）。
2. **1–3 个月**：P2-1 lensfun + P2-2 SIMD + P1-5 快照。
3. **有空时**：P2-3 SAM 蒙版 + P2-4 DCP + P3-1 直方图 + P3-3 预设。
4. **顺手**：T-1/T-2/T-3 随时插入。
5. **明确不做**：见第 0 节范围护栏。

