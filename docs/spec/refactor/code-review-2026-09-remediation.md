# 代码审查整改记录（2026-09-29）

来源：本轮全量代码审查（F-01…F-22）的逐项修复。本文记录修复落点，以及**未在代码层
完成**的两项（F-14 上帝类拆分、F-22 测试套件偶发崩溃）及其证据与方案，避免被误认为已完成。

## 已完成（代码已落地并验证）

| 编号 | 问题 | 落点 |
|------|------|------|
| F-01 | 预览导出无进展循环 | `src/catalog/CatalogDatabase.cpp` finishPreviewExport |
| F-02 | 解码异常逃逸 / 畸形输入 | `src/engine/ImageIO.cpp`、`src/engine/ImageBuffer.h` |
| F-03 | 占位 SHA 绕过完整性校验 | `src/app/ModelManager.cpp` |
| F-05 | MSVC 路径编码混用 | `utils/MathUtils.h`、`ColorManager`、`SoftProofing`、`CatalogDatabase`、`ModelManager` |
| F-06 | 1:1 打点语义错误 | `src/ui/MainWindow.cpp/.h` |
| F-07 | 打点日志常开 | `src/ui/Logging.{h,cpp}`、`src/main.cpp`、`PreviewWorker`、`MainWindow` |
| F-09 | 迁移失败路径不恢复 FK | `src/catalog/CatalogDatabase.cpp` migrateSqlVersion |
| F-10 | 迁移性能 / 静默失败 | 同上 finishPreviewExport |
| F-11 | 下载回调无锁访问注册表 | `src/app/ModelManager.cpp`（含 cancelDownload 残留条目） |
| F-12 | ZIP 解压越界写入 | `src/app/ModelManager.cpp`（中央目录原始条目名校验） |
| F-13 | 时序测试依赖固定睡眠 | `tests/lrc_qt_tests.cpp` |
| F-15 | 用户可见文案含 TODO | `src/ui/DevelopPanel.cpp`、`Pipeline.cpp`、`PanoramaStitcher.h` |
| F-16 | 注释以非 ASCII 结尾 | `AiInference.cpp`、`ImageIO.cpp`、`FolderMonitor.cpp` |
| F-17 | 构建标志隐式传递 | `CMakeLists.txt`、`tests/CMakeLists.txt` |
| F-18 | ModelManager 无单测 | `tests/lrc_qt_tests.cpp` ModelManagerTest（测试目标接入 Network/CorePrivate） |
| F-19 | 缺少 .gitattributes | 仓库根 `.gitattributes` |
| F-20 | JSON 数值精度 / 共享可变 Null | `src/utils/Json.{h,cpp}` |
| F-21 | build_x64.bat 引号缺陷 | `build_x64.bat`（`-S "%SRC%"`；已用 cmd 复现旧写法失败、新写法成功） |

## F-22：headless 套件在随机顺序下约 10% 偶发崩溃（未修复，最高优先级）

现象：`build_gui/tests/lrc_tests.exe "~[performance]" --order rand` 偶发失败，观察到两种形态：

- `SIGSEGV (0xC0000005)`，报告位置为
  `Pipeline: texture+clarity+sharpening is deterministic with pooled scratch`；
- `0xC0000409`（fail-fast 一类），或进程在打印 `All tests passed` 之后不退出。

证据（同一台机器、同一协议、各 10 次 `--order rand`）：

| 构建 | 失败次数 | 形态 |
|------|----------|------|
| 修复前（HEAD，本轮改动 stash 掉） | 1/10 | `0xC0000409` |
| 修复后（本轮全部改动） | 1/10 | `SIGSEGV` |

补充证据：

- 两个可疑用例单独跑 30 次全部通过（pipeline 0/30、catalog query-plan 0/30）；
- 记下出问题的随机 seed 复跑 3 次均通过；
- 挂起时进程只有 3 个线程，主线程停在同步对象等待上（不是 sqlite 忙等）。

结论：这是**修复前就存在**的内存/时序类偶发缺陷（两侧同样 1/10），与本次修复无因果，
但会让任何“全绿”结论失真，必须单独定位。

定位建议：

1. WinDbg/procdump（`-ma`）抓崩溃调用栈，先分清是 `RenderScratch` 线程局部缓冲复用，
   还是 `Parallel` 池的任务生命周期。
2. 在 `ScratchBuffer` 获取/归还与 `parallelFor` 任务队列上加断言（同一线程不得同时持有
   同一槽位、任务捕获的栈帧必须在返回前释放），再跑多轮 `--order rand`。
3. 复现后按 `--rng-seed` 固化顺序，纳入 CI 必跑集。

## F-14：GUI 上帝类拆分（未做代码拆分，已给方案）

现状（2026-09-29）：`src/ui/MainWindow.cpp` 3732 行 / 148 个成员函数 / 125 处 connect，
`MainWindow.h` 553 行；`src/ui/DevelopPanel.cpp` 2900 行 / 110 个函数；
`src/catalog/CatalogDatabase.cpp` 3550 行。**本轮不拆分**：这是跨文件的架构重构，没有 GUI
自动化测试兜底，盲目拆分会把可编译、可测试的现状换成不可验证的中间态。

建议的增量拆分顺序（每步独立提交，每步都要能构建 + 跑通 lrc_tests / lrc_qt_tests）：

1. **目录操作控制器**（与 F-08 同域）：把 backup / optimize / open / migrate 的 UI 编排
   从 MainWindow 抽到 `ui/CatalogOpsController`，MainWindow 只保留菜单接线与结果提示。
2. **导出编排**：`exportImage` / `batchExport` / 重命名 / 水印参数收拢到
   `ui/ExportCoordinator`，与 `ExportDialog`/`PreviewWorker` 的职责边界写进头文件。
3. **蒙版与取样**：蒙版创建、AI 主体/天空、颜色取样（点选吸管、色卡）归入
   `ui/MaskEditController`，把 `DevelopPanel` 的信号编排从 MainWindow 移出。
4. **DevelopPanel 拆分**：按面板分组（基础/曲线/颜色/效果/镜头/AI）拆成多个 `PanelSection`
   子控件，每个子控件只暴露“值变化 / 请求渲染”两类信号。
5. **CatalogDatabase 拆分**：迁移（`SchemaMigrator`）、预览存储（`PreviewStore`）、检索
   （`SearchQuery`）三块各自成 TU，CatalogDatabase 退化为连接与事务门面。

验收标准：拆分后 `MainWindow.cpp` < 1500 行、`DevelopPanel.cpp` < 1200 行，且 `lrc_tests`
全绿、`lrc_qt_tests` 的 DialogSmokeTest / ImportWorkerTest / BatchExportTest 覆盖不变。
