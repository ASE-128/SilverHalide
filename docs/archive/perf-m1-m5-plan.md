# LRC Clone 性能优化与运行时流畅化实施计划

## Context(背景与目标)

项目(LRC Clone Flash, C++20 + Qt6, v2.10.0, 218 测试全绿)功能已完备,但运行时存在四类开销:

1. **渲染热路径分配**: `gaussianBlur`/`lumaImage` 每次调用堆分配 2–3 个 w×h 的 `std::vector<float>`,全分辨率(6000×4000)每帧最多分配释放 ~864MB;luma 在纹理/清晰度/锐化/局部蒙版/可视化间重复计算 4 次。
2. **主线程 UI 卡顿**: 胶片栏 `setImages` 同步磁盘解码全部缩略图 + `rawThumbs_` 无上限(QImage 500 张约 1GB);`refreshFilmstrip` 主线程同步 SQLite 查询(N+1);coarse 帧(约 33Hz)每次全量重算直方图 + 状态栏;角标合成每项重建 QPixmap。
3. **导入/IO 阻塞**: 导入时对每张图全分辨率解码+渲染+JPEG 写(300 张 15–30 分钟);ThumbnailCache 单 `std::mutex` 跨 decode+render+write 全程持锁阻塞内存缓存;DB 缺索引(`import_date` 等排序全表扫描);FolderMonitor 在 UI 线程全扫目录。
4. **缓存/内存失控**: 磁盘缩略图无上限无清理、存 PPM(3×JPEG 体积);导出 `renderFull()` 把全尺寸 U8 塞进预览 LRU 挤掉预览;HDR 合并多帧全分辨率 F32 同时驻留(峰值 >2GB)。

**目标**: 不动架构前提下,优先消灭上述四类问题;清晰度降采样、SIMD 等**允许极小像素容差**(用户已确认);高收益低风险项先行,高风险架构改动标记可选、独立开关、可回滚。

**范围决策(用户已确认)**: 覆盖完整 M1–M5;允许极小像素容差(附逐通道 Δ/PSNR 回归)。

---

## 0. 里程碑总览与依赖

| 批次 | 主题 | 核心热点 | 风险 | 主要改动层 |
|---|---|---|---|---|
| **M1** | 引擎热路径分配消除 | 高斯暂存器池、luma 共享、蒙版 weight 复用、toDisplaySRGB thread_local | 低(纯重构, bit-exact) | engine |
| **M2** | UI 节流 + 目录查询提速 | 直方图/状态栏节流、角标合成缓存、onThumbReady 线程解码、异步持久化、DB 索引 + 预编译语句 | 低 | ui + catalog |
| **M3** | 胶片栏与主窗口刷新 | 胶片栏延迟加载 + 内存上限 + 增量更新;refreshFilmstrip N+1→2 查询、合并 navigator 调用 | 中(UI 状态机) | ui + catalog |
| **M4** | 引擎进阶(像素近似) | 清晰度降采样模糊、HSL bitset、SIMD 分批 Pass1 | 中 | engine |
| **M5** | 数据/IO 与可选架构 | 导出非缓存、缩略图 JPEG+磁盘上限、导入 fullres 懒生成、锁拆分、HDR 分块、FolderMonitor 线程化 | 中–高 | engine/catalog/ui |

**依赖链**(同文件一写者串行):
- `Pipeline.cpp` 被 M1 与 M4 共享 → **M1 先于 M4**。
- `MainWindow.cpp` 被 M2 与 M3 共享 → **M2 先于 M3**(节流先行,胶片栏增量在后)。
- `ThumbnailCache.cpp` 内 M5 顺序: **5.2(格式/容量)→ 5.3(懒生成)→ 5.4(锁拆分)**。
- `CatalogDatabase.cpp` 的 M2 预编译语句先于 M3 的新查询 → **M2→M3**。
- M1(engine) 与 M2(catalog/UI) 无文件交集,可整体并行。

---

## 1. 像素精确性矩阵(回归依据)

| 任务 | 输出像素 | 回归要求 |
|---|---|---|
| M1 全部(暂存器池/luma 共享/蒙版 weight/thread_local) | **bit-exact** | 218 全绿 + 新增字节级对比用例 |
| M2/M3 全部(节流/缓存/线程解码/索引) | **不改**(仅显示/持久化层) | 目录行为测试 + 手动 GUI |
| M4-1 清晰度降采样 | **改变像素(近似)** | 逐通道 Δ<0.008 或 PSNR>40dB |
| M4-2 HSL bitset | bit-exact | 现有 HSL 测试全绿 |
| M4-3 SIMD 分批 | 可能微小浮点差 | 容差回归;默认 `LRC_ENABLE_SIMD` 关闭 |
| M5-1 导出非缓存 | bit-exact | TestExportEngine 全绿 |
| M5-2 缩略图 PPM→JPEG | 文件格式变(JPEG 有损) | 视觉容差 + 头测 |
| M5-5 HDR 分块 | 分块 bit-exact | 分块 vs 整帧对比 |

---

## M1 — 引擎热路径分配消除(bit-exact)

**目标**: 消除渲染热路径每次调用的 vector 堆分配,合并重复 luma 计算。覆盖探查报告引擎热点前 4 项 + 蒙版 weight。

| 任务 | 内容 | 归属文件 | 可测 |
|---|---|---|---|
| M1-1 | **高斯暂存器池**: `gaussianBlur`(Pipeline.cpp:401)从返回 vector 改为写调用者输出缓冲,内部 `tmp/tmpA/tmpB/out` 走 `thread_local` 池(新 header-only `src/engine/RenderScratch.h` 免登记 CMakeLists);同步改调用点(applySpatialAdjustments 纹理/清晰度/锐化、applyLocalAdjustments、visualizeOverlay)与测试钩子 `pipelineGaussianBlurTest`。**全分辨率每帧 864MB 分配→0** | Pipeline.cpp + RenderScratch.h | ✅ |
| M1-2 | **luma 共享**: `lumaImage`(485)改输出参数;`applySpatialAdjustments` 内纹理+清晰度+锐化共用一次 luma(现 1177 行锐化重复算);调用次数 4→1~2 | Pipeline.cpp | ✅ |
| M1-3 | **蒙版 weight 复用**: `applyLocalAdjustments` 的 `std::vector<float> weight(n)`(805)去掉,融合循环内 `field[i]*opacity` 就地乘入 | Pipeline.cpp | ✅ |
| M1-4 | **toDisplaySRGB thread_local**: `ColorManager.cpp` 169-171 每块 `std::vector<float> tmp` 改 thread_local 池 | ColorManager.cpp | ✅ |
| ~~M1-splitTone~~ | **(验证后降级)**: `hueToRGB`(289)在 inline per-pixel 函数内,但 hCol/sCol(305-306)依赖整帧不变的 hue,编译器很可能已 CSE 提升 → 收益存疑,列为低优先可选,不占主线 | Pipeline.cpp | ✅ |

**并行切分**: Pipeline.cpp 由同一写者按 1→2→3 串行;ColorManager.cpp(1-4)由第二写者并行。

**验收**: headless 218 全绿;新增 TestPipeline: 记录 golden 输出后连续渲染 N 次断言逐字节相等;纹理+清晰度+锐化组合渲染字节对比;`TestColorManager` 全绿;计时冒烟 1920px 渲染耗时降 ≥30%。

---

## M2 — UI 节流 + 目录查询提速

**目标**: 消除主线程高频廉价工作堆积与 slider 释放热路径的 DB 写。

| 任务 | 内容 | 归属文件 | 可测 |
|---|---|---|---|
| M2-1 | **直方图/状态栏节流**: `onPreviewReadyCoarse` 只更新 viewport(必做),直方图+状态栏按 ~150ms 节流合并(节流+最后一帧必达),仅 refine/最终帧全量更新 | MainWindow.cpp | ❌ |
| M2-2 | **角标合成缓存**: `makeBadgedIcon` 结果按 `(imageId, rating, flag, labelColor, inCollection, thumb版本)` 存 `QPixmapCache`(限 ~64 项) | FilmstripWidget.cpp | ❌ |
| M2-3 | **onThumbReady 线程解码**: `ImportWorker` 在自身线程 `QImage::fromPath` 后经新信号 `thumbReadyImage(int, QImage)` 发送(元类型注册),主线程不再磁盘解码(现 1353) | ImportWorker.cpp + MainWindow.cpp | ❌ |
| M2-4 | **commitAdjustments 异步持久化**: `saveAdjustments` 的 JSON 序列化 + 两次 DB 写移出 slider 释放热路径,合并定时器(如 500ms)始终持久化最新值;`onAdjustmentsCommitted` 仍即时更新 UI | MainWindow.cpp | ❌ |
| M2-5 | **DB 索引**: `open()` 追加 `CREATE INDEX IF NOT EXISTS idx_images_rating / idx_images_flag / idx_images_import_date / idx_images_label_color` | CatalogDatabase.cpp | ✅ |
| M2-6 | **预编译语句缓存**: `CatalogDatabase` 内 `std::unordered_map<std::string, sqlite3_stmt*>` + 现有 recursive_mutex 缓存每条 SQL 的 stmt,复用代替每次 prepare+finalize(按参数类型重绑) | CatalogDatabase.cpp | ✅ |

**并行切分**: MainWindow.cpp(2-1/2-3/2-4)一个写者;FilmstripWidget.cpp(2-2)并行;CatalogDatabase.cpp(2-5/2-6)第三写者并行。

**验收**: headless 全绿;新增 TestCatalogDatabase: `EXPLAIN QUERY PLAN` 显示走 `SEARCH` 非 `SCAN`,且结果与迁移前一致;连续大量 saveAdjustments/listImages 验证 stmt 复用不串数据。手动 GUI: 拖曝光滑块直方图更新 ≤7Hz,状态栏不闪,滑块释放主线程无卡顿。

---

## M3 — 胶片栏与主窗口刷新(500 张规模)

**目标**: 画册刷新不阻塞主线程、内存有界、变更不触发全量重建。

| 任务 | 内容 | 归属文件 | 可测 |
|---|---|---|---|
| M3-1 | **listCollectionMemberships 单查询**: 新增一条 SQL JOIN 返回 imageId→collectionIds 映射,替换 refreshFilmstrip 每 collection 一次 listImagesInCollection 的 N+1 | CatalogDatabase.cpp | ✅ |
| M3-2 | **refreshFilmstrip 重建收窄**: `onSetLabelColor`/`onCollectionToggle`/`onFiltersChanged`(现全量 refreshFilmstrip)改增量更新 `records_`+`filmstrip_->setRating/setFlag/setLabelColor`;仅 filter 变化才全量;保留全量兜底 | MainWindow.cpp | ❌ |
| M3-3 | **胶片栏延迟加载 + 内存上限**: `setImages`(FilmstripWidget.cpp:129)不再同步解码全部缩略图;改为仅可见行+滚动窗口异步加载(复用 M2-3 的 thumbReadyImage 通道);`rawThumbs_` 改 LRU 上限(128 项)+ QPixmapCache 兜底 | FilmstripWidget.cpp + MainWindow.cpp | ❌ |
| M3-4 | **refreshFilmstrip 后台化(可选)**: DB 查询移到后台线程,快照 QVector<Entry> 发回主线程,主线程只 setImages;注意与 ImportWorker 并发写库(mutex 已保护) | MainWindow.cpp | ❌ |

**并行切分**: M3-1 属 catalog 可与 ui 并行;M3-2/3-3/3-4 在 **M2 完成后**动 MainWindow.cpp,与 FilmstripWidget.cpp 写者并行(仅经既有接口对接)。

**验收**: headless 全绿;memberships 单查询与 N+1 结果一致性对比。手动 GUI: 500 张过滤结果立即出占位图,滚动平滑流入;`rawThumbs_` 内存有界(堆不随滚动线性增长);改评分/色标/收藏不闪烁不重排;`setImages` 主线程耗时 <50ms。

---

## M4 — 引擎进阶(像素近似,须在 M1 后)

**目标**: 清晰度大 sigma 模糊降到 1/4 分辨率执行;HSL 只遍历激活通道;可选 SIMD 化基础 Pass。

| 任务 | 内容 | 归属文件 | 可测 |
|---|---|---|---|
| M4-1 | **清晰度降采样模糊**: 清晰度分支(1149-1168)luma 降到 1/4 线性尺寸,sigma 同比例缩放后 3-box 模糊,双线性上采样回全分辨率再减;纹理(sigma 2.5 小)保持全分辨率。内存传输降 16 倍 | Pipeline.cpp | ✅ |
| M4-2 | **HSL bitset 激活通道**: `applyAdjustments` 预计算 8 位 activeMask,`applyHSLPerChannel` 只迭代置位通道(现每像素扫 8 通道) | Pipeline.cpp | ✅ |
| M4-3 | **SIMD 分批 Pass1(可选)**: 曝光/对比度/曲线/白平衡等 SIMD 友好阶段抽到 8 像素批量循环(MSVC intrinsics);非友好阶段保持标量;`LRC_ENABLE_SIMD` 开关默认关 | Pipeline.cpp | ✅ |

**并行切分**: 同文件 → 同一写者按 1→2→3 串行(在 M1 完成后)。

**验收**: headless 全绿;新增容差回归(合成图 + clarity,降采样 vs 全分辨率 3-box 逐通道最大差 <0.008 或 PSNR>40dB);HSL bitset 与旧路径字节相等;清晰度渲染阶段耗时降 ≥40%。

---

## M5 — 数据/IO 与可选架构改动

**目标**: 修复导出污染预览 LRU、缩略图 PPM 过大/磁盘无上限、导入期 fullres 阻塞、ThumbnailCache 长锁、HDR 峰值内存、FolderMonitor 主线程轮询。

| 任务 | 内容 | 归属文件 | 可测 |
|---|---|---|---|
| M5-1 | **导出非缓存路径**: `ExportEngine::exportJpeg/exportPng/exportTiff8` 由 `renderFull`(会 insertCache 全尺寸 U8 挤掉预览)改走 `renderFullDisplay`(不缓存);输出字节不变 | ExportEngine.cpp | ✅ |
| M5-2 | **缩略图 JPEG + 磁盘上限**: `ensure()` 渲染路径改写 JPEG(现 PPM);缓存键加版本后缀(如 `@v2`)使旧缓存失效;启动/写入时按 mtime 做 LRU 清理,磁盘上限(如 1GB)与最大条目数 | ThumbnailCache.cpp | ✅ |
| M5-3 | **导入 fullres 懒生成**: `ImportWorker::importPaths`(141-143)移除 `ensureFullRes` 只建缩略图;1:1 首次进入 `PreviewWorker::loadFullRes` 已有「无缓存→解码并回写」兜底(109-141);`fullres_path` 仅作提示;可加低优先级后台回填可选开关 | ImportWorker.cpp + ThumbnailCache.cpp | ✅ |
| M5-4 | **ThumbnailCache 锁拆分(可选)**: `ensureFullRes/writeFullRes` 现跨 decode+render+write 全程持 `mutex_`;改短锁检查 + 锁外长操作 + 回插(per-key in-flight 去重);`getMemory/findFullRes` 不再被阻塞 | ThumbnailCache.cpp | ✅ |
| M5-5 | **HDR 分块(可选)**: `HdrMerge::merge` 全帧 F32 同时驻留(3×24MP≈864MB);按行带 tiling 流式合并(每块只驻留 2 帧行带),分块与整帧结果对比 | HdrMerge.cpp | ✅ |
| M5-6 | **FolderMonitor 离主线程(可选)**: `watchFolderPoll` UI 线程 directory_iterator 全扫;迁 QThread 或 QFileSystemWatcher 原生监听;保留轮询回退 | FolderMonitor.cpp + MainWindow.cpp | ❌ |
| M5-7 | **1:1 全分辨率调整缓存(可选)**: `PreviewWorker` 保留最近 1-2 份非默认调整的全分辨率 U8,undo/redo 往返不重渲染 | PreviewWorker.cpp | ❌ |

**并行切分**: M5-1(ExportEngine)、M5-2(ThumbnailCache,先于 5-3/5-4)、M5-5(HdrMerge)、M5-7(PreviewWorker)四写者并行;M5-3 依赖 5-2(同文件顺序);M5-4 依赖 5-2;M5-6 触碰 MainWindow.cpp → 在 M3 之后。

**验收**: M5-1 新增断言导出后 `pipeline.cacheBytes()` 不增、预览 LRU 未被逐出;M5-2 缩略图为 .jpg 且体积降 ≥60%、超上限清理到 <1GB、旧缓存忽略;M5-3 导入 300 张不再全分辨率解码,首次 1:1 首帧稍慢可用;M5-4 `getMemory` 在 ensureFullRes 进行中即时返回;M5-5 峰值内存 ≤1.5GB(3×24MP)且输出一致;M5-6 拖入文件自动导入不阻塞 UI。

---

## 2. 并行写者调度速查

| 文件 | M1 | M2 | M3 | M4 | M5 |
|---|---|---|---|---|---|
| `engine/Pipeline.cpp` | 写者A(1-1..1-3) | — | — | 写者A(继M1) | — |
| `engine/ColorManager.cpp` | 写者B(1-4) | — | — | — | — |
| `ui/MainWindow.cpp` | — | 写者C | 写者C(继M2) | — | 写者C(继M3,5-6) |
| `ui/FilmstripWidget.cpp` | — | 写者D(2-2) | 写者D(3-3) | — | — |
| `ui/ImportWorker.cpp` | — | 写者C(2-3) | — | — | 写者E(5-3) |
| `ui/PreviewWorker.cpp` | — | — | — | — | 写者F(5-7) |
| `catalog/CatalogDatabase.cpp` | — | 写者G(2-5,2-6) | 写者G(继M2,3-1) | — | — |
| `catalog/ThumbnailCache.cpp` | — | — | — | — | 写者H(5-2→5-3→5-4) |
| `engine/ExportEngine.cpp` | — | — | — | — | 写者I(5-1) |
| `engine/HdrMerge.cpp` | — | — | — | — | 写者J(5-5) |
| `catalog/FolderMonitor.cpp` | — | — | — | — | 写者K(5-6) |

**主代理职责**: 只管理分发不修改源码;批次内多写者并行,同文件串行;每批构建+测试验收后再进下一批。

---

## 3. 验证方式(每批次通用)

```bat
:: 引擎+目录 headless(主回归,快)
cmake -S lightroom-clone-cpp -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure

:: 全量 GUI+测试
cmake -S lightroom-clone-cpp -B build_gui -G Ninja -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j
ctest --test-dir build_gui --output-on-failure
.\build_gui\Release\lrc_lightroom.exe   :: 手动 GUI 验收
```

最终目标: 218 现有用例全绿 + 各里程碑新增用例全绿;GUI 手动验收清单(直方图节流、胶片栏 500 张流畅、导入提速、1:1 调整流畅度、导出后预览仍命中缓存)。

## 4. 编码约定提醒

- **中文注释行必须以 ASCII 结尾**(MSVC GBK 吞换行,见记忆 feedback-gbk-comment-encoding)。
- 新增 `.cpp` 必须登记 CMakeLists(本计划尽量用 header-only `RenderScratch.h` 规避)。
- 所有 M5 可选改动加编译/运行开关,失败可回退旧路径。
- 引擎/目录层改动优先 headless 验证;UI 层手动验收。
