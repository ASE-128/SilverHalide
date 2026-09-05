# SPEC：性能优化 M1–M5 实施路径与验收标准

> 上游计划：[docs/plans/perf-m1-m5-plan.md](../plans/perf-m1-m5-plan.md)（v2.10.0 基线，218 测试）
> 本文档将计划细化为**可逐步执行的实施路径**与**可量化验证的验收标准**。
> 状态快照核对日期：2026-09-03（源码扫描）。计划中的行号引用为计划期位置，实施/核对时以符号搜索为准。
> 当前测试基线：38 个测试文件、413 个 TEST_CASE（2026-09-03 统计）。

---

## 0. 总体实施路径

五批按依赖链推进，每批内部按"写者并行 → 构建 → headless 回归 → 批次验收 → 下一批"循环：

```
M1(engine, bit-exact) ──┐
                        ├─→ M4(engine, 像素近似)      M5-1/5/7 与主线并行
M2(ui+catalog) ──→ M3(ui+catalog) ──→ M5-6 ──┘
              M5-2 → M5-3 → M5-4（同文件串行）
```

- **每任务通用出口条件**：headless 构建通过 + `ctest --test-dir build_headless --output-on-failure` 全绿 + 该任务专属验收项通过。
- **每批通用出口条件**：GUI 构建通过 + GUI 测试 exe 全绿 + 该批手动 GUI 验收清单逐项打勾。
- **回滚纪律**：M4-3、M5-4、M5-5、M5-6、M5-7 为可选项，必须带独立开关（编译期或运行期），默认走旧路径，失败可整体回退。

---

## 1. 像素/行为精确性矩阵（所有验收的回归依据）

| 任务 | 输出像素 | 回归要求 |
|---|---|---|
| M1 全部 | **bit-exact** | 全量测试绿 + 新增字节级对比用例 |
| M2/M3 全部 | **不改**（显示/持久化层） | 目录行为测试 + 手动 GUI |
| M4-1 清晰度降采样 | **近似** | 逐通道 Δ<0.008 或 PSNR>40dB |
| M4-2 HSL bitset | bit-exact | 现有 HSL 测试全绿 + 字节相等断言 |
| M4-3 SIMD 分批 | 微小浮点差 | 容差回归；`LRC_ENABLE_SIMD` 默认关 |
| M5-1 导出非缓存 | bit-exact | TestExportEngine 全绿 + 缓存字节数断言 |
| M5-2 缩略图 PPM→JPEG | 文件有损 | 视觉容差 + 头测 + 旧缓存忽略 |
| M5-5 HDR 分块 | 分块 bit-exact | 分块 vs 整帧逐字节对比 |

---

## 2. M1 — 引擎热路径分配消除（bit-exact）

**目标**：消除渲染热路径每次调用的堆分配，合并重复 luma 计算。全分辨率（6000×4000）每帧分配从 ~864MB 降至 0。

### 实施路径

| 步骤 | 任务 | 关键动作 | 归属 |
|---|---|---|---|
| M1-1 | 高斯暂存器池 | 新建 header-only `src/engine/RenderScratch.h`（免登记 CMakeLists）：提供 `thread_local` 的 `tmp/tmpA/tmpB/out` 缓冲池；`gaussianBlur` 从返回 vector 改为写调用者输出缓冲；同步改全部调用点（`applySpatialAdjustments` 的纹理/清晰度/锐化、`applyLocalAdjustments`、`visualizeOverlay`）与测试钩子 `pipelineGaussianBlurTest` | `engine/Pipeline.cpp` + `RenderScratch.h` |
| M1-2 | luma 共享 | `lumaImage` 改输出参数；`applySpatialAdjustments` 内纹理+清晰度+锐化共用一次 luma；调用次数 4→1~2 | `engine/Pipeline.cpp` |
| M1-3 | 蒙版 weight 复用 | `applyLocalAdjustments` 删除 `std::vector<float> weight(n)`，融合循环内 `field[i]*opacity` 就地乘入 | `engine/Pipeline.cpp` |
| M1-4 | toDisplaySRGB thread_local | `ColorManager.cpp` 每块的 `std::vector<float> tmp` 改走 thread_local 池 | `engine/ColorManager.cpp` |
| M1-x | splitTone（已降级，不占主线） | 仅当探查确认编译器未 CSE 提升 hCol/sCol 时才做；否则关闭 | `engine/Pipeline.cpp` |

**执行顺序**：M1-1 → M1-2 → M1-3（同文件串行，同一写者）；M1-4 由第二写者并行。

### 验收标准

- [ ] headless 全量测试绿（含基线 218 + 新增）。
- [ ] 新增 `TestPipeline` 用例：记录一次 golden 输出后连续渲染 N 次，**逐字节相等**。
- [ ] 新增用例：纹理+清晰度+锐化组合渲染与 golden **字节对比一致**。
- [ ] `TestColorManager` 全绿。
- [ ] `TestRenderScratch`：暂存池复用语义（不同尺寸请求、并发线程互不串扰）。
- [ ] 计时冒烟：1920px 渲染耗时较基线**下降 ≥30%**（同一机器、同一测试图、Release 构建，记录于批次验收记录）。

### 状态快照（2026-09-03 源码核对）

`RenderScratch.h`、`TestRenderScratch.cpp` 已存在；`ColorManager.cpp` 含 thread_local 使用 → M1 主体已落地。M1-2/M1-3 未逐一核对，如复核需以符号搜索定位 `lumaImage` 调用点数验证"4→1~2"。

---

## 3. M2 — UI 节流 + 目录查询提速

**目标**：消除主线程高频廉价工作堆积与 slider 释放热路径的 DB 写。

### 实施路径

| 步骤 | 任务 | 关键动作 | 归属 |
|---|---|---|---|
| M2-1 | 直方图/状态栏节流 | `onPreviewReadyCoarse` 只更新 viewport；直方图+状态栏按 ~150ms 节流合并（节流+最后一帧必达，用 one-shot QTimer）；仅 refine/最终帧全量更新。节流常量统一进 `src/app/Constants.h` | `ui/MainWindow.cpp` |
| M2-2 | 角标合成缓存 | `makeBadgedIcon` 结果按 `(imageId, rating, flag, labelColor, inCollection, thumbVersion)` 为 key 存 `QPixmapCache`，上限 ~64 项 | `ui/FilmstripWidget.cpp` |
| M2-3 | onThumbReady 线程解码 | `ImportWorker` 在自身线程 `QImage` 解码后经新信号 `thumbReadyImage(int, QImage)` 发回（元类型注册），主线程不再磁盘解码 | `ui/ImportWorker.cpp` + `ui/MainWindow.cpp` |
| M2-4 | commitAdjustments 异步持久化 | `saveAdjustments` 的 JSON 序列化 + 两次 DB 写移出 slider 释放热路径，合并定时器（~500ms）始终持久化最新值；`onAdjustmentsCommitted` 仍即时更新 UI | `ui/MainWindow.cpp` |
| M2-5 | DB 索引 | `open()` 追加 `CREATE INDEX IF NOT EXISTS`：`idx_images_rating / idx_images_flag / idx_images_import_date / idx_images_label_color` | `catalog/CatalogDatabase.cpp` |
| M2-6 | 预编译语句缓存 | `std::unordered_map<std::string, sqlite3_stmt*>` stmtCache（SQL 文本为 key），复用代替每次 prepare+finalize；按参数类型重绑；`close()` 统一 finalize | `catalog/CatalogDatabase.cpp` |

**执行顺序**：MainWindow.cpp（2-1/2-3/2-4）一个写者；FilmstripWidget.cpp（2-2）、CatalogDatabase.cpp（2-5/2-6）并行。

### 验收标准

- [ ] headless 全绿。
- [ ] 新增 `TestCatalogDatabase`：`EXPLAIN QUERY PLAN` 对 rating/flag/import_date/label_color 过滤查询显示 `SEARCH ... USING INDEX`，**非** `SCAN`；且查询结果与加索引前一致。
- [ ] 新增用例：连续大量交错 `saveAdjustments`（不同参数）/`listImages`，验证 stmt 复用**不串数据**（结果与逐条 prepare 路径一致）；`stmtCacheSize()` 有界。
- [ ] 手动 GUI：拖动曝光滑块时直方图更新频率 ≤7Hz（~150ms 节流）、状态栏不闪、滑块释放时主线程无卡顿；关闭窗口前最后一次调整 500ms 内落库。
- [ ] 手动 GUI：批量导入时主线程不做磁盘缩略图解码（UI 保持响应）。

### 状态快照（2026-09-03 源码核对）

已落地：`kHistogramThrottleMs=150`（Constants.h）+ MainWindow 节流定时器；`badgeCacheKey`/badge 缓存（FilmstripWidget）；`thumbReadyImage` 信号（ImportWorker）;`kAdjustmentDbDebounce` 常量（Constants.h）；4 条索引 SQL 与 `stmtCache_`/`stmtCacheSize()` 均在 CatalogDatabase 中。→ M2 五项全部在码，待计时/GUI 类验收项补做。

---

## 4. M3 — 胶片栏与主窗口刷新（500 张规模）

**前置**：M2 完成（共享 MainWindow.cpp 与 CatalogDatabase.cpp 查询接口）。

### 实施路径

| 步骤 | 任务 | 关键动作 | 归属 |
|---|---|---|---|
| M3-1 | memberships 单查询 | 新增 `listCollectionMemberships()`：一条 SQL JOIN 返回 imageId→collectionIds 映射，替换 refreshFilmstrip 每 collection 一次 `listImagesInCollection` 的 N+1 | `catalog/CatalogDatabase.cpp` |
| M3-2 | refreshFilmstrip 重建收窄 | `onSetLabelColor`/`onCollectionToggle`/`onFiltersChanged` 改增量更新 `records_` + `filmstrip_->setRating/setFlag/setLabelColor`；仅 filter 变化才全量；保留全量兜底路径 | `ui/MainWindow.cpp` |
| M3-3 | 胶片栏延迟加载 + 内存上限 | `setImages` 不再同步解码全部缩略图；仅可见行+滚动窗口异步加载（复用 M2-3 `thumbReadyImage` 通道）；`rawThumbs_` 改 LRU 上限（128 项）+ QPixmapCache 兜底 | `ui/FilmstripWidget.cpp` + `ui/MainWindow.cpp` |
| M3-4 | refreshFilmstrip 后台化（可选） | DB 查询移后台线程，快照 QVector 发回主线程；与 ImportWorker 并发写库注意 mutex 保护 | `ui/MainWindow.cpp` |

**执行顺序**：M3-1 可与 UI 并行；M3-2→M3-3→M3-4 在 M2 后串行推进。

### 验收标准

- [ ] headless 全绿；`listCollectionMemberships` 与 N+1 循环结果**一致性对比用例**通过。
- [ ] 手动 GUI（500 张目录）：过滤结果立即出占位图，滚动时缩略图平滑流入。
- [ ] `rawThumbs_` 内存有界：滚动全程堆内存不随滚动线性增长（LRU 上限 128 生效，可用插入计数/容量断言或 GUI 内存观测）。
- [ ] 改评分/色标/收藏：**不闪烁、不重排**（增量更新生效）。
- [ ] `setImages` 主线程耗时 **<50ms**（500 张规模，计时日志或探针断言）。
- [ ] M3-4（若启用）：刷新期间 UI 不冻结；并发导入不出现数据错乱。

### 状态快照（2026-09-03 源码核对）

`listCollectionMemberships` 已在 CatalogDatabase.h 声明；FilmstripWidget 含 `rawThumbs_` LRU（含 LRU 顺序注释与逐出逻辑）与 badge 缓存。M3-2/M3-4 未核实。→ M3 主体在码，增量收窄与后台化待确认。

---

## 5. M4 — 引擎进阶（像素近似，必须在 M1 后）

**前置**：M1 完成（共享 Pipeline.cpp）。容差依据见第 1 节矩阵。

### 实施路径

| 步骤 | 任务 | 关键动作 | 归属 |
|---|---|---|---|
| M4-1 | 清晰度降采样模糊 | 清晰度分支 luma 降到 1/4 线性尺寸（内存传输降 16 倍），sigma 同比例缩放后 3-box 模糊，双线性上采样回全分辨率再相减；纹理（sigma 2.5 小）保持全分辨率 | `engine/Pipeline.cpp` |
| M4-2 | HSL bitset 激活通道 | `applyAdjustments` 预计算 8 位 activeMask；`applyHSLPerChannel` 只迭代置位通道（替代每像素扫 8 通道） | `engine/Pipeline.cpp` |
| M4-3 | SIMD 分批 Pass1（可选） | 曝光/对比度/曲线/白平衡等 SIMD 友好阶段抽 8 像素批量循环（MSVC intrinsics）；非友好阶段保持标量；`LRC_ENABLE_SIMD` 默认关 | `engine/Pipeline.cpp` |

**执行顺序**：同一写者按 4-1→4-2→4-3 串行。

### 验收标准

- [ ] headless 全绿。
- [ ] M4-1 容差回归：合成图 + clarity，降采样路径 vs 全分辨率 3-box 路径，**逐通道最大差 <0.008 或 PSNR>40dB**；用例同时覆盖 sigma 边界（极小/极大清晰度值）。
- [ ] M4-2：bitset 路径与旧路径**字节相等**（现有 HSL 测试全绿即满足大半，另加激活通道组合用例）。
- [ ] M4-3：开关开启时容差回归通过（沿用 Δ<0.008 / PSNR>40dB）；**默认关**时行为与基线一致。
- [ ] 计时冒烟：清晰度渲染阶段耗时**降 ≥40%**。

### 状态快照（2026-09-03 源码核对）

Pipeline.cpp 已含 `LRC_ENABLE_SIMD` 条件编译块（含批量循环区段）→ M4-3 框架在码。M4-1/M4-2 未核实。注意：竞品优化计划（见同目录另一份 spec）的 P2-SIMD 与本任务相邻，边界以 perf M4-3 为准，扩展点（gaussianBlur AVX2 等）归竞品计划。

---

## 6. M5 — 数据/IO 与可选架构改动

### 实施路径

| 步骤 | 任务 | 关键动作 | 归属 |
|---|---|---|---|
| M5-1 | 导出非缓存路径 | `ExportEngine::exportJpeg/exportPng/exportTiff8` 由 `renderFull`（会 insertCache 全尺寸 U8、挤掉预览 LRU）改走 `renderFullDisplay`（不缓存）；输出字节不变 | `engine/ExportEngine.cpp` |
| M5-2 | 缩略图 JPEG + 磁盘上限 | `ensure()` 渲染路径 PPM→JPEG；缓存键加版本后缀（`@v2`）使旧缓存失效；启动/写入时按 mtime 做 LRU 清理，磁盘上限（如 1GB）+ 最大条目数 | `catalog/ThumbnailCache.cpp` |
| M5-3 | 导入 fullres 懒生成 | `ImportWorker::importPaths` 移除 `ensureFullRes`，导入只建缩略图；1:1 首次进入走 `PreviewWorker::loadFullRes` 的"无缓存→解码并回写"兜底；`fullres_path` 仅作提示；可加低优先级后台回填开关 | `ui/ImportWorker.cpp` + `catalog/ThumbnailCache.cpp` |
| M5-4 | 锁拆分（可选） | `ensureFullRes/writeFullRes` 不再跨 decode+render+write 全程持 `mutex_`；改短锁检查 + 锁外长操作 + 回插（per-key in-flight 去重）；`getMemory/findFullRes` 不被阻塞 | `catalog/ThumbnailCache.cpp` |
| M5-5 | HDR 分块（可选） | `HdrMerge::merge` 按行带 tiling 流式合并（每块只驻留 2 帧行带），替代全帧 F32 同时驻留（3×24MP≈864MB） | `engine/HdrMerge.cpp` |
| M5-6 | FolderMonitor 离主线程（可选） | `watchFolderPoll` 从 UI 线程迁 QThread 或 QFileSystemWatcher 原生监听；保留轮询回退 | `catalog/FolderMonitor.cpp` + `ui/MainWindow.cpp` |
| M5-7 | 1:1 全分辨率调整缓存（可选） | `PreviewWorker` 保留最近 1-2 份非默认调整的全分辨率 U8，undo/redo 往返不重渲染 | `ui/PreviewWorker.cpp` |

**执行顺序**：M5-2→M5-3→M5-4 同文件串行；M5-1、M5-5、M5-7 与主线并行；M5-6 在 M3 之后（共享 MainWindow.cpp）。

### 验收标准

- [ ] M5-1：新增断言——导出前后 `pipeline.cacheBytes()` **不增**、预览 LRU 无逐出；`TestExportEngine` 全绿（输出字节不变）。
- [ ] M5-2：缩略图落盘为 `.jpg` 且体积较 PPM **降 ≥60%**；写入超过上限时清理到 <1GB（清理用例：构造超限缓存目录启动后断言）；旧 PPM/无版本键缓存被忽略不加载。
- [ ] M5-3：导入 300 张不再触发全分辨率解码（用导入计时或解码计数断言验证）；首次 1:1 进入稍慢但可用，且回写后第二次即走缓存。
- [ ] M5-4：`getMemory` 在 `ensureFullRes` 进行中**即时返回**（无阻塞用例：并发线程计时/断言）；并发同 key ensure 只算一次（in-flight 去重断言）。
- [ ] M5-5：3×24MP 输入峰值内存 **≤1.5GB**；分块输出与整帧输出**逐字节一致**。
- [ ] M5-6：拖入文件自动导入时 UI 不阻塞（手动验收）；轮询回退路径仍可工作。
- [ ] M5-7：undo→redo 往返命中缓存不重渲染（渲染计数断言）。

### 状态快照（2026-09-03 源码核对）

已落地/在码：`ExportEngine` 三个导出函数均走 `renderFullDisplay`（M5-1）；ThumbnailCache 含 `@v2` 键、`.jpg` 落盘路径及旧 PPM 失效注释（M5-2 主体）；`ensureFullRes` API 存在。未核实：M5-3 导入路径是否已摘除 ensureFullRes、M5-4 锁拆分、M5-5 tiling、M5-6 线程化、M5-7 缓存。→ M5 前半在码，后半（多为可选项）待实施。

---

## 7. 验证方式（每批次通用）

```bat
:: 引擎+目录 headless（主回归，快）
cmake -S lightroom-clone-cpp -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure

:: 全量 GUI + 测试
cmake -S lightroom-clone-cpp -B build_gui -G Ninja -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j
:: GUI 测试直接运行 exe（中文路径下 ctest 不稳）
build_gui\tests\lrc_tests.exe
```

**最终验收门槛**（全部 M1–M5 收口时）：

- [ ] 现有全部用例 + 各里程碑新增用例全绿。
- [ ] GUI 手动验收清单逐项通过：直方图节流生效、胶片栏 500 张流畅、导入提速、1:1 调整流畅、导出后预览仍命中缓存。
- [ ] 所有可选项开关处于文档化的默认状态；`LRC_ENABLE_SIMD` 默认 OFF。

---

## 8. 编码约定（强制）

- 中文注释行必须以 ASCII 结尾（MSVC GBK 吞换行）。
- 新增 `.cpp` 必须登记 `tests/CMakeLists.txt` / 对应 CMakeLists；优先 header-only 规避登记。
- 可选改动必须有独立开关，失败可回退旧路径。
- 遵循 AGENTS.md 第 7 节注释规范：公开 API 用 Doxygen 风格，线程安全/缓存键/状态转换必注，英文代码注释。
- 引擎/目录层改动优先 headless 验证；UI 层手动验收。
