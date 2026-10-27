# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 当前状态来源

项目状态是动态更新的，以下文件按优先级递减作为真实来源：

- [STATUS.md](STATUS.md) — 当前各批次落地状态、验证数字、工作区未提交改动清单
- [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md) — 竞品对齐优化规格（任务入口）
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 稳定架构约束（管线、线程模型、缓存）
- [docs/DEVELOPMENT_EXPERIENCE.md](docs/DEVELOPMENT_EXPERIENCE.md) — 迁移/构建踩坑记录（GBK、/EHsc、CRT 路径等）

历史 PHASE 1–15、旧 speedup 状态、旧审计报告已归档到 [docs/archive](docs/archive)。

## 构建命令

源码根目录 = 工作区根目录 `D:\Desktop\SilverHalide-src`。活跃构建树位于根目录的 `build_gui` / `build_headless`。旧的 `lightroom-clone-cpp/build_gui` 是源码上提前的历史残留，不得作为当前打包或调试入口。

### GUI 构建

```bat
cmake -S . -B build_gui -G Ninja ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j
```

或直接用包装脚本（已内联 `vcvars64.bat` 与 `ONNXRUNTIME_ROOT`）：

```bat
.\build_x64.bat
```

### 无头构建与测试

```bat
cmake -S . -B build_headless -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure
```

### 运行测试

Headless 单测（Catch2）：

```bat
:: 全部
ctest --test-dir build_headless --output-on-failure

:: 直接运行可执行文件（中文路径下 ctest 偶发卡死时推荐）
.\build_headless\tests\lrc_tests.exe

:: 按标签运行单个/一组测试（可执行文件在 build_headless\tests\ 下）
.\build_headless\tests\lrc_tests.exe "[pipeline]"
.\build_headless\tests\lrc_tests.exe "[incremental]"
.\build_headless\tests\lrc_tests.exe "[performance][pa]"

:: 每次重新 configure 后先修 Ninja 的 msvc 依赖前缀（见 DEVELOPMENT_EXPERIENCE §10）
:: powershell -NoProfile -ExecutionPolicy Bypass -File build_headless\_fix_msvc_deps.ps1 -BuildDir build_headless
```

Qt 层测试（GUI 组件、真实导出断言）：

```bat
:: 脚本已封装 vcvars + Qt bin PATH
.\run_qt_tests.bat

:: 或直接运行（需 Qt DLL 在 PATH）
.\build_gui\tests\lrc_qt_tests.exe
```

### 运行 GUI

```powershell
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build_gui\SilverHalide.exe
.\build_gui\SilverHalide.exe
```

### 可选 CMake 开关

| 开关 | 说明 |
|------|------|
| `-DLRC_ENABLE_SIMD=ON` | AVX2 快速路径（默认 OFF，标量路径逐位一致） |
| `-DLRC_ENABLE_D3D=ON` | D3D11 DirectCompute GPU 像素后端（Windows，默认 ON；失败自动回退 CPU，可用「仅 CPU」设置强制） |
| `-DLRC_ENABLE_AI=ON` | ONNX Runtime 推理（`build_x64.bat` 已默认开启） |
| `-DLRC_PROXY=https://ghfast.top/` | FetchContent 代理（中国网络） |

## 架构边界

三层严格分层，依赖单向向下：

```
ui/      (Qt6 Widgets)       MainWindow · 自绘控件 · PreviewWorker / ImportWorker
catalog/ (纯 C++，无 Qt)      CatalogDatabase · ThumbnailCache · FolderMonitor
engine/  (纯 C++，无 Qt)      RawDecoder · Pipeline · ColorManager · ExportEngine
utils/   (纯 C++，共享)       Json · ProfileIO · Parallel · UndoStack
```

- `engine/` 与 `catalog/` 的头文件**禁止包含任何 Qt 头文件**，必须能在 `LRC_ENABLE_QT=OFF` 的 headless 构建中编译与测试。
- 新增 RAW / 色彩 / 非破坏性编辑逻辑优先落在 `engine/`；展示层逻辑优先落在 `ui/`。
- 跨层数据用引擎自有的 `ImageBuffer`（U16/F32/U8 RGB 交错）传递；UI 层仅在 `toQImage()` 处一次性拷贝为 `QImage`。

### 目录与持久化（最新结构）

- **目录即库**：`Default.shcat` 已不再是单文件，而是**目录**（`AppData/…/Default/`），内含 `Default.shcat` 数据库文件 + `previews.shcat/` 预览子目录。`Application::defaultCatalogPath()` 返回目录路径；遗留的 `.lrcat` / `.shcat` 单文件会在首次打开时自动迁移。
- `CatalogDatabase` 使用版本化迁移框架（`PRAGMA user_version` + `schema_migrations` 表），新增列/表走 `migrateEnsureColumn` / `migrateV1~V5`，失败整体回滚并关库。
- `images` 表含 `file_size` / `file_mtime` 指纹，用于外部修改检测；`previews` 表已改为磁盘文件路径（BLOB 外置），写入 `<previewDir>/<id低3位hex>/<id>_<kind>.shcat`。
- 批量导入在单连接的 `BEGIN IMMEDIATE` 事务内完成；`busy_timeout=5000` 替代旧双连接设计。

### 线程模型

| 线程 | 职责 | 与 UI 通信 |
|------|------|-----------|
| 主线程 | 布局、事件、直方图、DB 读 | — |
| `PreviewWorker` (QThread) | 解码 + 管线渲染（1920 精 / 480 粗 / 全尺寸 1:1）+ 导出 | `previewReady` / `previewReadyCoarse` / `fullResReady` 队列信号 |
| `ImportWorker` (QThread) | 事务批导入 + 缩略图/1:1 预生成 + 后台预分级 | `thumbReadyImage` / `progress` / `finishedImport` 队列信号 |

UI 通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 下发请求。自定义类型（`ImageBufferPtr`、`AdjustmentsPtr = std::shared_ptr<const Adjustments>`）已在 `main()` 中注册元类型（声明唯一入口为 `ui/MetaTypes.h`）。**队列里传的是共享只读快照**：UI 侧持可编辑副本，发布后不再改 pointee，一次编辑派生的多个请求共享同一份栈，避免深拷贝 `std::vector<Mask>`。Worker 带 `std::atomic` abort 标志，退出时先 `requestAbort` 再 `quit + wait`。

### 非破坏性编辑管线要点

- 调整只在线性光（线性 ProPhoto RGB）下进行，sRGB gamma 仅在最后一步由 lcms 施加。
- **coarse-then-refine**：拖动滑块期间 30ms 节流先出 480px 粗帧（`previewReadyCoarse`），停顿/松手后 1920 精帧；`fullResActive_`（1:1）时 coarse 仍更新预览层、但不触发全尺寸刷新。
- **缓存键**：`PipelineCacheKey = hash(imageId, adjustments.hash(), maxDim)`，256MB LRU；maxDim 参与键，粗/精/全尺寸天然分离。
- **逐级增量渲染**：`renderPreview` 维护 A~D+F 的逐级缓存（默认 150 MB），保留阶段为 `shared_ptr<const ImageBuffer>`（只读共享），写路径用私有工作缓冲池轮转；恒等 stage 直接别名共享，仅 D→E 保留 1 次整幅拷贝。改管线时不要破坏「保留快照绝不可写」这一约束。
- **1:1 全尺寸**：`halfSize=false, maxDim=0` 解码，`renderLinearFull` + `toDisplaySRGB` 渲染，**不塞 LRU**；同一时刻至多 1 份全尺寸缓冲，退出 1:1 / 换图即释放。默认调整下优先从目录内嵌预览 / 磁盘 1:1 缓存直读。

## 当前实现概览

已落地的关键成果（截至 2026-10-20）：

- **AI 模型分发与 UI 管理**：下载 / 解压 / SHA256 校验 / ONNX Runtime 推理 / 缓存目录管理；NAFNet ZIP / external-data 兼容修复；SHA256 余数块回归修复与单元测试。
- **P-A 性能极致**：引擎层 headless 量化验收已落地（`TestPerfAnchors.cpp`，合成 50MP 锚点），M1/M2/M3 实测记录已登记；真机 GPU 锚点待验。
- **P-B 离线 AI**：自动出片建议（`Pipeline::autoDevelop`）+ 批量预分级（`PregradeAnalyzer`）全链路已落地；目录层 V5 迁移含 `pregrade_has_subject/score/auto` 三列；UI  Filmstrip 打「AI」角标。
- **P3 体验打磨**：直方图线性化（worker 线程全遍历）、事件驱动文件夹监视（`ReadDirectoryChangesW` + 轮询回退）、编辑预设管理（`PresetLibrary`）、GPU 像素管线后端抽象（`SpatialFilterBackend`，D3D11 DirectCompute 默认 OFF）。
- **P1/P2 竞品对齐**：DirectML 回退、导出 ICC、批量重命名（`RenameTemplate`）、水印、快照、Lensfun 查询器、DCP 注入渲染、MobileSAM 点击式蒙版、SIMD AVX2 扩展。
- **目录持久化重构**：CatalogDatabase 版本化迁移、previews BLOB 外置、文件指纹、FTS5 + sqlite-vec 检索、单连接并发、自动编辑历史保留 20 条。
- **现代 C++ 改造（IM-1 ~ IM-11）**：管线 stage 去深拷贝（B-2 1920 refine 同机 A/B −17%~−33%）、`ImageBuffer` span 视图、`parallelFor` 内联快路径、`Json::Value` variant 化、`string_view` 化、`Mask` variant 化、lcms RAII、`jthread`、`noexcept`/`[[nodiscard]]` 补齐；两份逐位 oracle 守护输出不变。详见 [docs/spec/modern-cpp-adoption-report.md](docs/spec/modern-cpp-adoption-report.md)。
- **标准预览 v2 / preview pipeline 复用优化、四阶段加速报告**已完成并归档。

当前进行中的任务规格入口仍为 [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md)，但 P1/P2/P3/T 已按 spec 验收；P-A 真机锚点、P-B 端到端实机验证为后续待验项。实际落地状态以 [STATUS.md](STATUS.md) 为准。

## 关键文档

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — 管线数据流、色彩管理、线程模型、已知取舍
- [STATUS.md](STATUS.md) — 当前各功能批次状态、测试基线、已知缺陷
- [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md) — 竞品优化任务规格
- [docs/DEVELOPMENT_EXPERIENCE.md](docs/DEVELOPMENT_EXPERIENCE.md) — 迁移踩坑（GBK、/EHsc、CRT 路径、优化标志丢失、Ninja/中文 MSVC 的 /showIncludes 前缀转码等）
- [docs/spec/modern-cpp-adoption-audit.md](docs/spec/modern-cpp-adoption-audit.md) — 现代 C++ 审计与 IM-1~IM-11 改进清单
- [docs/spec/modern-cpp-adoption-baseline.md](docs/spec/modern-cpp-adoption-baseline.md) — T0 基线、perf 数字与两份逐位 oracle
- [docs/spec/modern-cpp-adoption-report.md](docs/spec/modern-cpp-adoption-report.md) — 现代 C++ 改造实施报告（before/after、验证、遗留）
- [docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md) — AI 模型分发状态
- [docs/archive/Speedup_Four_Stages_Report.md](docs/archive/Speedup_Four_Stages_Report.md) — 导入/预览/编辑/导出四阶段加速报告

## 约定

- 默认使用 Ninja，尤其是根目录下的 `build_gui` / `build_headless`。
- 优先用 headless 构建与测试作为快速反馈；UI 改动再补 GUI 验证。
- 新增单测优先放置于 `tests/` 并注册到 `tests/CMakeLists.txt`；Qt 层测试（真实导出、对话框 smoke）放在 `tests/lrc_qt_tests.cpp`。
- 引擎/目录层中文注释行**必须以 ASCII 字符结尾**（如英文句号 `.`），否则 MSVC 在 GBK 代码页下会把 UTF-8 悬挂字节与换行配对、吞掉下一行。
- 若修改构建或依赖，同步更新 README / CLAUDE.md / docs 中的真实状态说明。
- 不要把旧 VS 构建树或历史阶段文档当作当前真实状态的来源。
