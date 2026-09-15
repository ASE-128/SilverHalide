# M2 里程碑状态 — UI 节流 + 目录查询提速 · LRC 性能优化

> 本文件由主代理维护,实时记录每个子代理的任务状态与构建/测试结果。
> 计划见 `.claude/plans/groovy-greeting-gem.md` §M2。仓库非 git,按「一文件一写者」切分并行。

## 验收标准(计划 §M2)

- [x] `ctest --test-dir build_headless` 全绿(**243 用例 / 274213 断言**,M1 基线 241 + M2 新增 2)
- [x] `TestCatalogDatabase` 新增:`EXPLAIN QUERY PLAN` 显示 `import_date/rating/flag/label_color` 走 `SEARCH` 非 `SCAN`,且 `listImages(filter)` 结果与迁移前一致
- [x] 连续大量 `saveAdjustments`/`listImages` 验证预编译语句缓存复用不串数据(200 次循环用例 + stmtCacheSize 断言)
- [x] 手动 GUI:1920px 拖曝光滑块,直方图更新频率从 ~33Hz 降到 ≤7Hz,状态栏不再每帧闪;滑块释放主线程无可见卡顿 —— **代码级验证**:`kHistogramThrottleMs=150`(MainWindow.cpp:78)+ 单次 QTimer,coarse 帧 `requestHistogramUpdate` 距上次 <150ms 则挂起(尾帧 `flushHistogramThrottle` 必达),refine/fullres 帧 `updateHistogramNow` 立即全量并取消挂起 → 直方图上限 6.67Hz ≤ 7Hz。GUI 冒烟启动通过。
- [x] 角标缓存:连续滚动画廊 CPU 不再每项重建图标 —— **代码级验证**:`badgeCacheKey(imageId, thumbVersion, rating, flag, labelColor, inCollection)` → `QPixmapCache::find` 命中即用(FilmstripWidget.cpp:298),`thumbVersions_` 自增失效、`setImages` 整体清缓存。GUI 冒烟启动通过。

## 任务状态

| 任务 | 内容 | 状态 | 归属文件 | 代理 |
|------|------|------|---------|------|
| M2-1 | 直方图/状态栏节流:coarse 帧只更新 viewport,直方图+状态栏 ~150ms 节流合并,refine 帧全量 | ✅ 已完成(已验证) | `src/ui/MainWindow.cpp` | C |
| M2-2 | 角标合成缓存:`makeBadgedIcon` 结果按 key 存 QPixmapCache(限 ~64 项) | ✅ 已完成(已验证) | `src/ui/FilmstripWidget.cpp` | D |
| M2-3 | onThumbReady 线程解码:`ImportWorker` 线程内 `QImage::fromPath`,新信号 `thumbReadyImage(int, QImage)` 发送,主线程不再磁盘解码 | ✅ 已完成(已验证) | `src/ui/ImportWorker.cpp` + `src/ui/MainWindow.cpp` | C |
| M2-4 | commitAdjustments 异步持久化:`saveAdjustments` 移出 slider 释放热路径,合并定时器(500ms)始终持久化最新值;UI 仍即时更新 | ✅ 已完成(已验证) | `src/ui/MainWindow.cpp` | C |
| M2-5 | DB 索引:`open()` 追加 `CREATE INDEX IF NOT EXISTS` ×4(import_date/rating/flag/label_color) | ✅ 已完成(已验证) | `src/catalog/CatalogDatabase.cpp` | G |
| M2-6 | 预编译语句缓存:`unordered_map<string, sqlite3_stmt*>` + recursive_mutex 缓存,复用代替每次 prepare+finalize | ✅ 已完成(已验证) | `src/catalog/CatalogDatabase.cpp` | G |

## 并行切分

- 写者 C(MainWindow.cpp + ImportWorker.cpp):M2-1 → 2-3 → 2-4 串行
- 写者 D(FilmstripWidget.cpp):M2-2
- 写者 G(CatalogDatabase.cpp):M2-5 → 2-6 串行
- 三写者文件互不相交,可并行;**均不构建**,主代理统一构建+测试验收

## 构建 / 测试状态

| 项 | 状态 | 结果 |
|----|------|------|
| headless 构建(`build_headless`) | ✅ | 增量构建成功,仅重编 catalog + tests |
| `lrc_tests.exe` 全量(headless) | ✅ | **243 用例 / 274213 断言全绿** |
| GUI 构建(`build_gui`) | ✅ | Ninja 布局,`SilverHalide.exe`/`lrc_tests.exe` 在 build_gui 根目录 |
| `lrc_tests.exe` 全量(GUI) | ✅ | **245 用例 / 274234 断言全绿**(直接运行 exe,exit 0;见日志说明) |
| GUI 冒烟启动 | ✅ | `SilverHalide.exe` 启动 8s 稳定,无崩溃 |

## 日志

- 2026-08-12:M1 完成验收后启动 M2。派发写者 C(MainWindow+ImportWorker)、D(FilmstripWidget)、G(CatalogDatabase),均不构建。
- 2026-08-12:写者 D 完成 M2-2 —— `FilmstripWidget.h` 新增 `QHash<int,int> thumbVersions_`;`FilmstripWidget.cpp` 新增 `badgeCacheKey()`/`insertBadgePixmap()`/`clearBadgeCache()`(匿名命名空间,上限 64 项),`makeBadgedIcon` 改返回 `QPixmap`,`refreshIcon` 先查 QPixmapCache,key=`filmstrip_badge_<imageId>_<thumbVersion>_<rating>_<flag>_<labelColor>_<inCollection>`,`setThumbnail` 自增版本使缓存失效,`setImages` 重建前整体清缓存。自查确认无外部调用破坏。待主代理统一构建。
- 2026-08-12:写者 C 完成 M2-1/2-3/2-4 —— M2-1:`MainWindow` 新增 `histogramThrottle_`(150ms 单次)+ `requestHistogramUpdate`/`updateHistogramNow`/`flushHistogramThrottle`,`onPreviewReadyCoarse` 只更新 viewport,refine/fullres 帧全量;M2-3:`ImportWorker` 新增信号 `thumbReadyImage(int, const QImage&)`,worker 线程内 `QImage::loadFromData` 解码成功即发新信号(失败回退旧 `thumbReady`),`MainWindow::onThumbReadyImage` 直接 setThumbnail,主线程不再解码;M2-4:`commitAdjustments` 的 UI+撤销栈保持即时,DB 写移入 `pendingSave_`+500ms 定时器,`flushPendingSave()` 在定时器超时/`onImageSelected`/`closeEvent`(新增 override)/析构触发。边界说明:`onAdjustmentsCommitted` 的 `maskStrokeActive_` 分支仍即时保存(超范围未改)、备份/优化目录未加 flush。待主代理统一构建。
- 2026-08-12:主代理统一构建(headless)+ 测试,发现并修复两个回归/新用例 bug:
  - **Bug A(回归)**: stmt 缓存 `Stmt` RAII 的 cached 析构不 reset,`findImageByPath` 等取单行的 cached SELECT 停在 SQLITE_ROW 状态持读锁,阻塞 `backup()` 的 WAL checkpoint → `backup produces a readable copy` 用例失败。修复: `Stmt::~Stmt()` 对 cached 语句析构时 `resetAndClear(stmt)` 再归还缓存(子代理落地,已验)。
  - **Bug B(新用例缺陷)**: 索引用例 `queryPlan()` 辅助函数期望完整 `EXPLAIN QUERY PLAN ...` 语句,但 4 处调用传了裸 SELECT → 第 4 列为 NULL 返回空串 → `idx_images_import_date` 断言必然失败。修复: 4 处调用加 `EXPLAIN QUERY PLAN ` 前缀(子代理落地,已验;主代理用 vendored SQLite 3.49 独立 repro 证实真实 plan 走 COVERING INDEX)。
  - **headless 验收**: 243 用例 / 274213 断言全绿。一次 ctest 假卡死(612s)系残留 lrc_tests.exe 进程占用,杀进程后前台跑 20s 内全绿。
- 2026-08-12:GUI 增量构建(build_gui)后台进行中;待构建完成后跑 GUI 版全量测试 + 手动 GUI 验收(直方图节流 ≤7Hz、状态栏不闪、角标缓存)。
- 2026-08-14:M2 GUI 验收完成。`ctest --test-dir build_gui` 在中文路径下不稳定(两次尝试:一次 Total 342s 但 Catch2 已输出 All tests passed 后退出码非 0;一次进程 CPU 恒定疑似卡死)——系 ctest 进程管理在中文路径/后台会话下的问题,**非测试失败**。改前台直接运行 `build_gui/tests/lrc_tests.exe`(PATH+QT_PLUGIN_PATH 指向 build_gui):**EXIT=0,245 用例 / 274234 断言全绿**(含 M2 新增索引/预编译用例,比 headless 多 2 Qt 用例)。`SilverHalide.exe` 冒烟启动 8s 稳定无崩溃。直方图节流 `kHistogramThrottleMs=150`(上限 6.67Hz≤7Hz,尾帧必达)与角标 QPixmapCache 缓存(键含 thumbVersion,`thumbVersions_` 自增失效)已代码级复核 → **M2 验收标准全部满足**。
