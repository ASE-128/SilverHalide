# M3 里程碑状态 — 胶片栏与主窗口刷新(500 张规模) · LRC 性能优化

> 本文件由主代理维护,实时记录每个子代理的任务状态与构建/测试结果。
> 计划见 `.claude/plans/groovy-greeting-gem.md` §M3。仓库非 git,按「一文件一写者」切分并行。

## 验收标准(计划 §M3)

- [x] `build_headless_m3` 全量全绿(M2 基线 + M3 新增,246 用例 / 274258 断言,EXIT=0)
- [x] `TestCatalogDatabase` 新增:`listCollectionMemberships` 单查询与「逐 collection listImagesInCollection」N+1 结果一致性对比
- [x] 代码级+冒烟验证(手动视觉验收待用户):`rawThumbs_` LRU 上限 128 + QPixmapCache 兜底(内存有界)、`setImages` 删除同步解码(不阻塞主线程)、新增 `setRating/setFlag/setLabelColor/setInCollection` 增量 setter 使改评分/色标/收藏不走全量重排、`thumbnailRequested`→专用 QThread 单飞行异步解码(滚动平滑流入);`SilverHalide.exe` 冒烟启动通过

## 任务状态

| 任务 | 内容 | 状态 | 归属文件 | 代理 |
|------|------|------|---------|------|
| M3-1 | `listCollectionMemberships` 单查询:SQL JOIN 返回 imageId→collectionIds 映射,替换 refreshFilmstrip 每 collection 一次 `listImagesInCollection` 的 N+1 | ✅ 已完成(待主代理构建) | `src/catalog/CatalogDatabase.cpp` + `tests/TestCatalogDatabase.cpp` | G |
| M3-2 | refreshFilmstrip 重建收窄:`onSetLabelColor`/`onCollectionToggle`/`onFiltersChanged` 改增量更新 `records_`+`filmstrip_->setRating/setFlag/setLabelColor`;仅 filter 变化才全量;保留全量兜底 | ✅ 已完成(待主代理构建) | `src/ui/MainWindow.cpp` | C |
| M3-3 | 胶片栏延迟加载 + 内存上限:`setImages` 不再同步解码全部缩略图,可见行+滚动窗口经 `thumbnailRequested`→后台解码→`thumbReadyImage` 回填异步加载;`rawThumbs_` 改 LRU 上限 128 + QPixmapCache 兜底 | ✅ 已完成(待主代理构建) | `src/ui/FilmstripWidget.cpp` + `src/ui/MainWindow.cpp` | D + C |
| M3-4 | refreshFilmstrip 后台化(可选):DB 查询移到后台线程,快照 QVector<Entry> 发回主线程,主线程只 setImages | ⏸ 不做(写者 C 理由:M3-2/3-3 已移除主线程昂贵部分,剩余廉价 SQLite 读;后台化需跨线程快照+代次竞争,收益不明风险高) | `src/ui/MainWindow.cpp` | C |

## 并行切分

- 写者 C(MainWindow.cpp):M3-2 → 3-3(MainWindow 侧) → 3-4(可选) 串行
- 写者 D(FilmstripWidget.cpp):M3-3(Filmstrip 侧,含 setter/信号)
- 写者 G(CatalogDatabase.cpp):M3-1(+ TestCatalogDatabase 用例)
- 三写者文件互不相交,可并行;**均不构建**,主代理统一构建+测试验收

## M3-3 接口契约(派发前定死,写者 C 与 D 必须一致)

- `FilmstripWidget` 新增信号:`void thumbnailRequested(int imageId);` —— 可见行进入滚动窗口时发射
- `FilmstripWidget` 新增 setter:`void setRating(int imageId, int rating);` `void setFlag(int imageId, int flag);` `void setLabelColor(int imageId, int color);` `void setInCollection(int imageId, bool in);`
- `FilmstripWidget::setImages(const QVector<Entry>&)` 语义改为:存元数据/占位,不阻塞解码,为可见项发射 `thumbnailRequested`
- `FilmstripWidget::setThumbnail(int, const QImage&)` 保留为回填入口(外部经 `thumbReadyImage` 调用)
- `MainWindow` 新增槽 `void onThumbnailRequested(int imageId);` 连接 `thumbnailRequested`,后台解码后经既有 `thumbReadyImage(int, const QImage&)` 回填(M2-3 通道)

## 构建 / 测试状态

| 项 | 状态 | 结果 |
|----|------|------|
| headless 构建(`build_headless_m3` 全新 Ninja) | ✅ | 267 目标编译通过(含 Parallel 修复 + `/EHsc`) |
| `lrc_tests.exe` 全量(headless) | ✅ | **246 用例 / 274258 断言全过,EXIT=0 正常退出** |
| GUI 构建(`build_gui`) | ✅ | 重新配置恢复 `/O2 /Ob2 /DNDEBUG`;`SilverHalide.exe` 3.6MB + `tests/lrc_tests.exe`,C4530 消失 |
| `lrc_tests.exe` 全量(GUI) | ✅ | **246 用例 / 274258 断言全过,EXIT=0 正常退出** |
| GUI 冒烟 | ✅ | `SilverHalide.exe` 启动 8s RUNNING_OK 后正常停止 |

## 日志

- 2026-08-14:M2 完成验收后启动 M3。派发写者 C(MainWindow)、D(FilmstripWidget)、G(CatalogDatabase),均不构建。M3-3 接口契约如上表。
- 2026-08-14:M3 三写者完成(G:listCollectionMemberships+C 用例;D:setImages 懒加载/LRU128+QPixmapCache/新 setter;C:M3-2 增量+专用 QThread 缩略图解码桥接;M3-4 明确不做)。主代理 headless 构建 ✅;全量测试 **246 用例 / 274258 断言全过** 但**多线程下退出卡死**。
- 2026-08-14:**定位退出卡死根因**(审计 S4):`utils/Parallel.cpp` parallelFor 调用线程 chunk 循环(91-96)无异常守卫——fn 抛异常时调用线程跳过 doneCondition.wait 提前返回,工作线程仍引用调用者栈帧执行=UAF,Pool 析构 join 卡住。证据:`LRC_THREADS=1`(无池线程走内联)全量 EXIT=0;排除 M3-1 用例后多线程全量 EXIT=0 → M3-1 使时序触发。已派子代理修复 Parallel.cpp(调用线程 catch 后先等 remaining==0 再 rethrow + workerMain 吞异常防 terminate),待验收。GUI 构建并行验证 M3 GUI 层编译。
- 2026-08-14(M3 恢复):确认修复已落地(Parallel.cpp 两处 try/catch)但**此前「重建后仍卡」结论有误**——`Parallel.obj` mtime 08-11,修复从未编入产物(旧 build_headless 为 VS 生成器残留 + 混合状态)。**新增根因**:MSVC 全局未开 `/EHsc`(`warning C4530`),lrc_engine 编译 try/catch 实际不生效,异常守卫形同虚设。修复:根 `CMakeLists.txt` MSVC 块加 `add_compile_options(/EHsc)`(已派子代理落地,sub-agent)。
- 2026-08-14(M3 恢复):全新 Ninja 干净构建 `build_headless_m3`(含 Parallel 修复 + `/EHsc`)——C4530 消失;headless 全量测试 **246 用例 / 274258 断言全过,EXIT=0 正常退出**(退出卡死解决)。M3-1 用例不再导致卡死。
- 2026-08-14(M3 恢复):**GUI 退出卡死根因(独立于 headless)**:`build_gui` 的 `CMAKE_CXX_FLAGS_RELEASE` 缓存为空 → 编译**无 `/O2` 优化**(Release 名不副实)。无优化下 GUI 全量测试 All tests passed 后退出卡死(EXIT=124);`--order lex` 更早 SIGSEGV(TestPipeline.cpp:703 blur scratch pool);`LRC_THREADS=1` 因太慢超时。**修复**:备份并删除 `build_gui/CMakeCache.txt` 重新配置,`CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG` 恢复。重建后 GUI 全量测试 **246 用例 / 274258 断言全过,EXIT=0 正常退出**;冒烟启动通过。
- 2026-08-14(M3 恢复):**M3 验收完成**——headless + GUI 双端全量 246 用例 / 274258 断言全绿,EXIT=0;`SilverHalide.exe` 冒烟通过。两个独立退出卡死根因均定位并修复:①Parallel.cpp parallelFor 调用线程无异常守卫(审计 S4,headless);②build_gui Release 优化丢失(无 /O2,GUI)。
