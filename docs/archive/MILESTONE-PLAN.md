# 里程碑实施计划（多子代理并行）

> 本文档由 4 路并行只读调研代理产出（目录/照片管理、序列化/XMP、预览管线/1:1、构建/测试基础设施），
> 汇总为可执行的分里程碑计划。**本计划在开工前需经确认**；确认后按里程碑逐个推进，
> 里程碑内部用多子代理并行，共享文件按「一文件一写者」规则避免冲突。

---

## 0. 调研结论摘要

| 领域 | 关键事实 | 对计划的影响 |
|------|---------|-------------|
| 标签颜色 | `images.label_color` 列、`ImageRecord.labelColor`、`ImageFilter.labelColor` 过滤、`CatalogDatabase::setLabelColor()` **全部已就绪** | 纯前端补充：NavigatorWidget 筛选、Filmstrip 色标绘制、`FilmstripWidget::Entry` 补字段 |
| 收藏夹集合 | 无 `collections` 表 / C++ 实体，需从零建 | 新建 2 张表 + DB 方法 + UI 分组 + 角标 |
| 迁移机制 | `ensureColumn()` = `ALTER TABLE ADD COLUMN`；新表在 schema 末尾加 `CREATE TABLE IF NOT EXISTS`；无版本号 | 加列/加表成本极低，在线迁移 |
| UI 接线模式 | Filmstrip 信号 → MainWindow 槽 → CatalogDatabase → `refreshFilmstrip()`；筛选经 Navigator → `filtersChanged` | 标签/收藏夹可完全照搬既有链路 |
| 序列化 | 自定义扁平 camelCase JSON，`version:5` 纯装饰（读时未校验）；预设为 `.lrp` | XMP 互导需新建 XMP/RDF 解析器 + crs:↔自定义键双向映射 + sidecar I/O |
| EXIF/XMP | `src/` 无任何 exif/xmp/sidecar 代码；libraw 暴露 `imgdata.idata.xmpdata` 但未读取 | 风险最高、前置研究最重的里程碑 |
| 1:1 放大 | 预览恒为 1920px（`kPreviewMaxDim` 在解码阶段生效）；现有 `OneToOne` 是「假 1:1」；全尺寸解码路径（`halfSize=false, maxDim=0`）已存在、仅供导出 | 真正 1:1 是「PreviewWorker 全尺寸请求 + 缓存策略 + Viewport 换图」三层改造 |
| 缓存容量 | Pipeline 实际默认 **48MB**，`Constants.kCacheCapacityMB=64` 声明但从未生效 | M2 顺带对齐 |
| 构建 | CMake 显式源列表（`LRC_ENGINE_SOURCES`/`LRC_CATALOG_SOURCES`/`LRC_UI_SOURCES`），新 `.cpp` 必须手动登记；测试 13 文件显式列于 `tests/CMakeLists.txt` | 新增文件/测试的落地清单见 §7 |
| 测试 | headless（`LRC_ENABLE_QT=OFF`）可全量测 engine/catalog/utils；当前 116 用例（CLAUDE.md/README 写的 95 已过期）；ui/ 层无测试 | 引擎/目录改动优先 headless 验证 |
| 版本号 | `Constants.h kAppVersion = "1.1.0"` 与 STATUS.md v1.9 不一致 | §6 贯穿性收尾 |

---

## 1. 总体里程碑排序与理由

| 里程碑 | 内容 | 版本 | 规模 | 风险 |
|--------|------|------|------|------|
| **M1** | 照片管理：标签颜色 + 收藏夹集合 | v2.0 | 小 | 低 |
| **M2** | 真正 1:1 全尺寸放大（按需全尺寸解码渲染） | v2.1 | 中 | 中 |
| **M3** | XMP 兼容 / Lightroom 预设互导 | v2.2 | 大 | 高 |
| M4（可选） | macOS / Linux 构建验证 | v2.3 | 大 | 中（环境） |

**排序理由**：
1. **M1 先做**：STATUS.md 已标为「进行中」；DB 基础设施（label_color）大半就绪，是低成本高价值的快速胜利，顺手建立「多代理并行」的稳定工作流。
2. **M2 次之**：全尺寸解码路径已存在，改造是「加路径 + 定缓存 + 改视口」，无外部格式研究负担；且 1:1 是查看体验的核心，收益直接。
3. **M3 殿后**：依赖外部格式知识（crs: 命名空间 / RDF/XML）与逐字段映射设计，且需 M1/M2 期间补齐的序列化专项测试兜底；设计先行（C0）。
4. **M4 可选**：无新增功能，纯移植验证，依赖 CI/多平台环境，建议有需要时单独立项。

---

## 2. 里程碑 M1 — 照片管理：标签颜色 + 收藏夹集合（v2.0）

### 2.1 现状盘点（调研已确认）

- **已就绪**：`label_color` 列（`CatalogDatabase.cpp:74`）、`ImageRecord.labelColor`（`CatalogDatabase.h:33`）、`ImageFilter.labelColor` 过滤（`CatalogDatabase.h:51`）、`setLabelColor()`/`listImages()` 过滤（`CatalogDatabase.cpp:221-233, 312-313`）。
- **缺口**：`FilmstripWidget::Entry` 无 `labelColor` 字段（`FilmstripWidget.h:23-29`）；`makeBadgedIcon()` 未绘制色标（`FilmstripWidget.cpp:24-69`）；NavigatorWidget 无色标筛选下拉（现有评分/旗标下拉在 `NavigatorWidget.cpp:27-41`）；收藏夹无任何 DB/UI 支持。

### 2.2 验收标准

- **标签颜色**：任意照片可设/清除 7 色标签；Filmstrip 缩略图显示色标角标；Navigator 可按色标筛选（复用 `ImageFilter.labelColor`）；右键菜单子菜单 + 键盘快捷键；选择持久化到 `label_color`，重启恢复。
- **收藏夹集合**：可创建/重命名/删除集合；可向集合添加/移除照片（重复添加幂等）；Navigator 出现「收藏夹」顶层分组，点击集合过滤胶片栏；集合与成员持久化（`collections` + `collection_images` 两表），级联删除干净。

### 2.3 子任务分解（含文件归属）

| 任务 | 内容 | 归属文件 |
|------|------|---------|
| **M1.1 目录层** | 新建 `collections`（id, name, sort_order）与 `collection_images`（collection_id, image_id, 复合主键）两表；方法：`createCollection`/`renameCollection`/`deleteCollection`/`listCollections`/`addToCollection`/`removeFromCollection`/`isImageInCollection`/`listImagesInCollection`；在 `open()` schema 末尾追加 `CREATE TABLE IF NOT EXISTS` | `src/catalog/CatalogDatabase.h/.cpp`、`tests/TestCatalogDatabase.cpp` |
| **M1.2 Filmstrip** | `Entry` 补 `labelColor`（+可选 `inCollection` 角标标志）；`makeBadgedIcon()` 在右下角/底部绘制色标小方块（映射 1..7 → 7 色）；右键菜单加「色标」子菜单（checkable）与「添加到收藏夹/从收藏夹移除」子菜单；新增信号 `labelColorRequested`/`collectionRequested` | `src/ui/FilmstripWidget.h/.cpp` |
| **M1.3 Navigator** | 加第 3 个筛选下拉（无/红..品红，仿 `NavigatorWidget.cpp:27-41`）并扩展 `filtersChanged`；新增「收藏夹」顶层分组（仿 `libraryRoot_` 模式，`NavigatorWidget.cpp:87-138`），子项为各集合，点击发 `collectionSelected` | `src/ui/NavigatorWidget.h/.cpp` |
| **M1.4 集合管理对话框** | 新建 `CollectionsDialog`（列出/新建/重命名/删除集合，增删成员）；若为 QWidget 子类归入 ui/ | 新文件 `src/ui/CollectionsDialog.h/.cpp` + `CMakeLists.txt` 登记 |
| **M1.5 MainWindow 接线** | 收集 M1.1–M1.3 定义的接口：接 Filmstrip 新信号 → `setLabelColor`/集合增删；Navigator 色标筛选写入 `filterLabelColor_`；收藏夹分组过滤（`refreshFilmstrip()` 内按集合 ID 集合过滤）；目录菜单或照片菜单加「管理收藏夹…」 | `src/ui/MainWindow.h/.cpp` |

### 2.4 并行切分建议（3+1，避免共享文件冲突）

> 仓库非 git，无法用 worktree 隔离。规则：**每个文件同一时刻只允许一个子代理写**。

- **Phase 1（三路并行，文件互不相交）**
  - 代理 A ← M1.1（catalog 层，独占 `CatalogDatabase.*` + `TestCatalogDatabase.cpp`）
  - 代理 B ← M1.2（独占 `FilmstripWidget.*`）
  - 代理 C ← M1.3 + M1.4（独占 `NavigatorWidget.*` + 新建 `CollectionsDialog.*`）
- **Phase 2（单代理收口）**
  - 代理 D ← M1.5（独占 `MainWindow.*`；收集 A/B/C 定义的信号与接口完成接线；跑 headless 测试全绿）

### 2.5 测试计划

- catalog 层（headless 可测）：集合 CRUD、成员增删幂等、级联删除、重名、`listImagesInCollection` 过滤；标签颜色过滤已有覆盖，补充 `labelColor` 组合过滤用例。追加到 `tests/TestCatalogDatabase.cpp`。
- UI 层（无测试基建）：手动验收（设色 → 角标 → 筛选 → 重启恢复；建集合 → 添加照片 → 过滤 → 重启保留）。

### 2.6 风险与取舍

- 低。`labelColor` 主链路已就绪，集合走既有「表 + 分组 + 过滤」模式。
- 取舍：集合仅做「手动集合」（非智能集合）；不做嵌套集合。

---

## 3. 里程碑 M2 — 真正 1:1 全尺寸放大（v2.1）

### 3.1 现状（调研已确认）

- 预览恒为 1920px：`PreviewWorker::loadImage()` 硬编码 `params.maxDim=kPreviewMaxDim=1920` + `halfSize=true`（`PreviewWorker.cpp:39-40`）；`pipeline_.render(..., 0)` 收到的是已缩小缓冲（`PreviewWorker.cpp:50`）。
- 现有 `ZoomMode::OneToOne`（`ImageViewport.cpp:292-319`）只是对 1920 QImage 用 `zoom_` 因子放大 = **假 1:1**（`Ctrl+1` 见 `MainWindow.cpp:264-266`）。
- 全尺寸解码路径**已存在**：`RawDecoder` 在 `halfSize=false, maxDim=0` 时走全尺寸（`RawDecoder.cpp:80-83,118-126`），当前仅供导出。
- Pipeline 对 `maxDim` 无硬限制（`<=0` 即全分辨率），`renderLinearFull` **故意不缓存**（`Pipeline.cpp:1241-1246`）。
- 缓存容量：Pipeline 默认 **48MB**（`Pipeline.h:126`）；`kCacheCapacityMB=64`（`Constants.h:23`）从未生效。

### 3.2 验收标准

- `Ctrl+1`/双击 → 真正的 1:1：**1 图像像素 = 1 物理像素**（含 devicePixelRatio 换算）。
- 进入 1:1 时后台全尺寸解码，就绪后无缝替换预览；等待期间沿用 1920 预览（渐进式，不白屏）。
- 全尺寸渲染不阻塞 UI（沿用 PreviewWorker 代次丢弃机制）。
- 内存受控：全尺寸缓冲同一时刻至多 1 份；不把 72MB 全尺寸 U8 塞进 48MB LRU（明确不缓存，或独立小容量缓存）。
- 修复缓存容量不一致：`Constants.kCacheCapacityMB` 与 Pipeline 实际值对齐。
- 退出 1:1 / 换图时释放全尺寸缓冲。

### 3.3 子任务分解

| 任务 | 内容 | 归属文件 |
|------|------|---------|
| **M2.1 缓存与常量清理** | 统一缓存容量（让 `kCacheCapacityMB` 生效或删除该常量）；`maxDim=0` 全尺寸路径补测试；确认 `renderLinearFull` 不缓存策略保持 | `src/engine/Pipeline.h/.cpp`、`src/app/Constants.h`、`tests/TestPipeline.cpp` |
| **M2.2 PreviewWorker 全尺寸请求** | 新增全尺寸请求路径（`halfSize=false, maxDim=0`），沿用代次机制；定义新信号（如 `fullResReady`）与「当前请求的全尺寸 imageId+adjHash」去重，避免同图重复解码；释放旧全尺寸缓冲 | `src/ui/PreviewWorker.h/.cpp` |
| **M2.3 ImageViewport 真 1:1** | 修正 `OneToOne` 语义：`zoom_==1.0` 时按图像实际分辨率 × devicePixelRatio 映射；缩放越过阈值（如 ≥100% 或用户按 Ctrl+1）时向 PreviewWorker 请求全尺寸，就绪后换入 QImage 并保持视口中心不变；必要时分块绘制控制帧率 | `src/ui/ImageViewport.h/.cpp` |
| **M2.4 MainWindow 联动** | `Ctrl+1`/菜单/滚轮进入 1:1 时触发全尺寸请求；退出时释放；更新直方图数据源 | `src/ui/MainWindow.h/.cpp` |

### 3.4 并行切分建议

- **Phase 1（两路并行，文件不相交）**
  - 代理 A ← M2.1（引擎 + 常量 + 测试，与 UI 完全解耦）
  - 代理 B ← M2.2（PreviewWorker，定义全尺寸请求/就绪接口）
- **Phase 2（接口联调）**
  - 代理 C ← M2.3 + M2.4（ImageViewport + MainWindow；依赖 B 定义的接口）

> 若希望 Phase 2 与 Phase 1 并行：先让 B 在 Phase 1 早期定下头文件接口（PreviewWorker 新槽/新信号签名），C 即可同时开发、联调放在收口阶段。

### 3.5 测试计划

- 引擎：`maxDim=0` 全分辨率渲染正确性；半尺寸 vs 全尺寸关键像素一致性（面积平均语义）；内存边界（全尺寸缓冲分配/释放）；`kCacheCapacityMB` 生效断言。
- 性能：真机验证 6000×4000 ARW 进入 1:1 的延迟与峰值内存（记录到 STATUS.md）。

### 3.6 风险与取舍

- 中。内存峰值（U16 解码 ~144MB + U8 输出 ~72MB）需实测；超大 QImage 绘制可能卡帧 → 取舍为「不缓存全尺寸、同一时刻 1 份缓冲、必要时分块绘制」。
- 风险点：`PreviewWorker` 当前线程模型是串行单队列，全尺寸解码会阻塞后续预览渲染 → 需评估是否单独线程或打断优先级（倾向：1:1 请求取代式进队，沿用代次丢弃）。

---

## 4. 里程碑 M3 — XMP 兼容 / Lightroom 预设互导（v2.2）

### 4.1 现状（调研已确认）

- 序列化为自定义扁平 JSON（`ProfileIO.cpp`），键名 camelCase 无命名空间；`version:5` 仅写入未校验。
- `src/` 无任何 EXIF/XMP/sidecar 代码；libraw 暴露 `imgdata.idata.xmpdata/xmplen`（`libraw_types.h:205-206`）但应用未读取。
- 预设格式为自定义 `.lrp`；无 `.xmp`/`.lrtemplate` 支持。
- 测试缺口：无独立 `TestProfileIO.cpp`；`version` 键、未知键容错、ColorTarget 哈希均未覆盖。

### 4.2 验收标准

- 读取标准 Lightroom XMP sidecar（`rdf:RDF` + `crs:` 命名空间），映射为 `Adjustments` 并应用到当前照片。
- 把当前调整导出为 `.xmp` 预设（crs: 属性），可在 Lightroom 中识别（结构合法、字段名/比例正确）。
- 导入 `.xmp` 预设对话框 + 「存储为预设」；sidecar 与目录 edits 双向同步（可选开关）。
- 对未知/缺失 crs: 字段容错（跳过，不报错）；数值比例与 Lightroom 约定一致（如 Exposure2012 用档、Contrast2012 用 -100..+100）。
- 测试：映射往返、sidecar 读写、golden `.xmp` 样例、未知字段容错。

### 4.3 子任务分解

| 任务 | 内容 | 归属文件 |
|------|------|---------|
| **M3.0 设计（串行门，先行）** | 产出「crs: 属性 ↔ Adjustments 字段」逐字段映射表（名称/单位/比例/默认），覆盖：Exposure2012、Contrast2012、Highlights2012、Shadows2012、Whites2012、Blacks2012、Temperature/Tint、Texture/Clarity/Dehaze/Vibrance、ToneCurve（PV2012 RGB 曲线数组）、HSL（crs:RedHue 等 24 个）、SplitTone、ColorAdjustments（crs:ColorAdjustmentRed…，点颜色近似）、LocalAdjustments（蒙版近似，v1 取舍） | 文档 `docs/XMP-MAPPING.md` |
| **M3.1 XMP/RDF 解析与写出** | **选型取向：引入 pugixml**（轻量、MIT/双许可、经 FetchContent 或 `third_party/` vendored，避开自写 XML 的边界坑；最终确认放在 M3 设计阶段）；封装 `XmpDocument`（读 `rdf:Description`、crs: 属性；写出同样结构）；读取 `imgdata.idata.xmpdata`（RAW 内嵌） | 新模块 `src/engine/Xmp.h/.cpp`（或 `utils/`）+ `CMakeLists.txt` 登记 |
| **M3.2 映射层** | 按 M3.0 映射表实现 `crs → Adjustments`（导入）与 `Adjustments → crs`（导出）；处理未知键、缺省值、浮点解析 | 可改 `src/utils/ProfileIO.h/.cpp` 或新增 `src/engine/XmpProfile.h/.cpp` |
| **M3.3 sidecar I/O** | `.xmp` sidecar 与图像文件同名放置的读写；导入/导出预设文件对话框 | `src/engine/ImageIO.h/.cpp`（或新 `src/engine/XmpSidecar.h/.cpp`） |
| **M3.4 UI** | 菜单：「文件 → 导入预设…/导出预设…」「照片 → 读取 sidecar/写入 sidecar」；应用结果后触发重渲染 | `src/ui/MainWindow.h/.cpp`、新对话框 |
| **M3.5 测试** | `tests/TestXmp.cpp`（映射往返、golden 样例、容错）；`tests/TestProfileIO.cpp`（先补：version 键、完整往返、未知键容错、ColorTarget 哈希） | `tests/TestXmp.cpp`、`tests/TestProfileIO.cpp` + `tests/CMakeLists.txt` 登记 |

### 4.4 并行切分建议

- **前置（M1 或 M2 期间顺带）**：补 `tests/TestProfileIO.cpp` 硬化序列化（低风险，为 M3 兜底）。
- **M3 Phase 1（两路并行）**
  - 代理 A ← M3.1（XMP/RDF 解析器 + XML 选型落地）
  - 代理 B ← M3.0 产出映射表 + M3.2 映射层（以 A 定义的解析结果结构为输入接口，接口先行约定）
- **M3 Phase 2**
  - 代理 C ← M3.3（sidecar I/O，依赖 A+B）
  - 代理 D ← M3.4（UI，依赖 B）
- **M3 Phase 3**：代理 E ← M3.5 测试收口（贯穿，或各代理自备）。

### 4.5 风险与取舍

- 高。核心风险是 Lightroom 预设格式的**精确语义**（crs: 字段单位、比例、ToneCurve 数组编码），需要外部资料核对，无法靠代码库内事实完全推断 → M3.0 必须先出映射表并经确认。
- 取舍：ToneCurve 映射为 PV2012 数组；HSL 24 个属性全量；蒙版/局部调整**不做双向互导**（v1 仅导出自己的 masks 到自定义键，或忽略）；点颜色仅近似映射（与既有「简化实现」一致）。
- XML 解析器选型：**倾向引入 pugixml**（新增依赖，用户已确认取向；是否同意新增此依赖在 M3 设计阶段最终确认）。

---

## 5. 里程碑 M4（可选）— macOS / Linux 构建验证（v2.3）

- 无新增功能；验证 engine/catalog（本就 Qt-free）与 ui/ 层的跨平台构建。
- 子任务：CI 矩阵（GitHub Actions）配置、libraw/lcms/sqlite 在各平台 FetchContent 或 vendored 路径、Qt 安装（aqtinstall）、`half_size`/路径大小写/`std::filesystem` 平台差异排查。
- 依赖：需要多平台环境/CI；若仓库当前非 git，需先 `git init` 并推远端（CI 前置）。

---

## 6. 贯穿性收尾任务（随各里程碑顺手做）

| 项 | 内容 | 归属 |
|----|------|------|
| 版本号同步 | `Constants.h kAppVersion` 目前 `"1.1.0"`，与 STATUS v1.9 不一致；每发一版同步更新并统一 `CLAUDE.md`/`README.md`/`STATUS.md` | M1 开始 |
| 测试计数同步 | CLAUDE.md/README 写「95 用例/512 断言」已过期，当前实际 116 用例；以 `lrc_tests.exe` 运行输出为准更新 | M1 开始 |
| 缓存容量对齐 | `kCacheCapacityMB=64` 与 Pipeline 默认 48MB 二选一 | M2 |
| ProfileIO 专项测试 | 新增 `tests/TestProfileIO.cpp`（version、完整往返、未知键容错、ColorTarget 哈希） | M1 或 M2 顺带 |
| 导航统计 | `setStatistics` 现仅「N 张图片 · M 已评分」，可顺带显示已收藏数 | M1 可选 |

---

## 7. 实施约定（落地必读）

- **构建命令**（见 CLAUDE.md）：headless `cmake -S lightroom-clone-cpp -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF` + `cmake --build build_headless -j` + `ctest --test-dir build_headless`；GUI 用 VS 2022 x64（`build_gui`，实际生成器为 Visual Studio，exe 在 `build_gui/Release/`，见记忆 [[build-layout-vs-generator]]）。
- **新增文件登记**：engine `.cpp` → `LRC_ENGINE_SOURCES`；catalog → `LRC_CATALOG_SOURCES`；ui → `LRC_UI_SOURCES`；测试 → `tests/CMakeLists.txt` 的 `lrc_tests` 源列表（均显式列出，不 glob）。
- **跨线程新类型**：`src/ui/MetaTypes.h` 加 `Q_DECLARE_METATYPE` + `src/main.cpp` 加 `qRegisterMetaType`。
- **分层约束**：engine/catalog 禁止 `#include` Qt 头；新引擎/目录逻辑必须可 headless 测试。
- **中文注释 GBK 约定**：中文注释行必须以 ASCII 结尾（如 `// 集合子项 (collection item).`），否则 MSVC 按 GBK 解析会吞换行；见 CLAUDE.md 与记忆 [[feedback-gbk-comment-encoding]]。
- **并行冲突规则**（仓库非 git，无 worktree）：每个文件同一时刻仅一个子代理可写；跨任务共享文件（MainWindow/Filmstrip/Navigator）按 §2.4/§3.4/§4.4 的相位切分归属。

---

## 8. 下一步

1. **确认本计划**（排序、M1 范围、M3 的取舍与 XML 选型前提）。
2. 开工 **M1**：按 §2.4 派 3 路并行（catalog / Filmstrip / Navigator+CollectionsDialog），收口代理接线 MainWindow，headless 测试全绿后手动验收 UI。
3. M1 验收通过 → 更新 STATUS.md/README/版本号 → 进入 M2。
4. M3 开工前先产出 `docs/XMP-MAPPING.md` 并经确认。
