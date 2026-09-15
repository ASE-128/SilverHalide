# M1 里程碑状态 — 照片管理（标签颜色 + 收藏夹集合）v2.0

> 本文件由实施代理实时维护：每个子代理完成后立即更新。
> 目标计划见 [MILESTONE-PLAN.md](MILESTONE-PLAN.md) §2。仓库非 git，按「一文件一写者」切分并行。

## 验收标准（计划 §2.2）— ✅ 用户确认通过（2026-08-11）

- [x] **标签颜色**：任意照片可设/清除 7 色标签；Filmstrip 缩略图显示色标角标；Navigator 可按色标筛选；右键菜单子菜单 + 键盘快捷键；选择持久化到 `label_color`，重启恢复。
- [x] **收藏夹集合**：可创建/重命名/删除集合；可向集合添加/移除照片（重复添加幂等）；Navigator「收藏夹」顶层分组，点击集合过滤胶片栏；集合与成员持久化（`collections` + `collection_images` 两表），级联删除干净。

## 任务状态

| 任务 | 内容 | 状态 | 归属文件 | 代理 |
|------|------|------|---------|------|
| M1.1 | 目录层：`collections` 两表 + 8 方法 | ✅ 已完成 | `src/catalog/CatalogDatabase.h/.cpp`、`tests/TestCatalogDatabase.cpp` | A |
| M1.2 | Filmstrip 色标角标 + 收藏夹右键菜单 | ✅ 已完成 | `src/ui/FilmstripWidget.h/.cpp` | B |
| M1.3 | Navigator 色标筛选 + 收藏夹分组 | ✅ 已完成 | `src/ui/NavigatorWidget.h/.cpp` | C |
| M1.4 | CollectionsDialog 集合管理对话框 | ✅ 已完成 | `src/ui/CollectionsDialog.h/.cpp`（新）+ `CMakeLists.txt` 登记 | C |
| M1.5 | MainWindow 接线（Phase 2 收口） | ✅ 已完成 | `src/ui/MainWindow.h/.cpp` | D |

## 共享接口契约（Phase 1 三路 + Phase 2 收口共同遵守，接口先行约定）

- **`lrc::Collection`**：`{ int64_t id; std::string name; int sortOrder; }`，由代理 A 在 `CatalogDatabase.h` 定义。
- **Catalog 层**：`createCollection(name)→id`、`renameCollection(id,name)`、`deleteCollection(id)`（级联成员）、`listCollections()`（sort_order,id 升序）、`addToCollection(id,imageId)`（幂等）、`removeFromCollection(id,imageId)`、`isImageInCollection(id,imageId)`、`listImagesInCollection(id)→vector<ImageRecord>`。
- **Filmstrip**：`Entry` 增 `int labelColor`（0 无，1..7）与 `QVector<int> collectionIds`；方法 `setLabelColor(imageId,color)`、`setCollections(QVector<lrc::Collection>)`；信号 `labelColorRequested(int,int)`、`collectionRequested(int,int,bool add)`。
- **Navigator**：`filtersChanged(int,int,int)`（新增第 3 参 labelColorFilter）；方法 `setCollections(QVector<lrc::Collection>)`；信号 `collectionSelected(int collectionId)`。
- **CollectionsDialog**：`explicit CollectionsDialog(lrc::CatalogDatabase*, QWidget* = nullptr)`；信号 `collectionsChanged()`。
- **色标映射**：1 红 2 橙 3 黄 4 绿 5 青 6 蓝 7 紫。
- **键盘（Filmstrip）**：`6/7/8/9` → 色 1/2/3/4，`Ctrl+6/7/8` → 色 5/6/7，`Ctrl+Shift+0` → 清除色标(0)。

## 构建 / 测试状态

| 项 | 状态 | 结果 |
|----|------|------|
| headless 构建（`build_headless`，VS 生成器，Release） | ✅ | 代理 A 完成，构建成功无新增告警 |
| `lrc_tests.exe` 全量 | ✅ | **125 用例 / 745 断言全绿**（改动前 ≈116 用例 / 682 断言） |
| GUI 构建（`build_gui`，VS 生成器，Release） | ✅ | `build_gui\Release\lrc_lightroom.exe` 生成成功（3.1MB）；仅有既有 C4996/C4819 告警 |
| 手动验收 UI | ⏸ | 待用户验收（设色→角标→筛选→重启恢复；建集合→添加→过滤→重启保留） |

## 日志

- 2026-08-11：开工 M1 Phase 1，派发三路并行代理 A（catalog）/ B（filmstrip）/ C（navigator+collections）。
- 2026-08-11：代理 C 完成（M1.3 + M1.4）。`filtersChanged` 改为 3 参；Navigator 增 `setCollections`/`collectionSelected`（集合 id 存于 `Qt::UserRole+2`）；`CollectionsDialog(catalog*, parent)` 落地（CRUD + 成员增删 + `collectionsChanged`），`src/ui/CollectionsDialog.cpp` 已登记进 `LRC_UI_SOURCES`。待 A/B。
- 2026-08-11：代理 A 完成（M1.1）。`Collection` 结构 + 8 方法 + `collections`/`collection_images` 两表（FK 级联）落地；抽出 `readImageRecord` 共享映射（`listImages`/`findImageByPath` 行为不变）。**偏差**：`listImagesInCollection` 用显式列投影而非 `SELECT images.*`（避免 `ensureColumn` 迁移库列序差异，结果列完全一致）。headless 全量 **125 用例 / 745 断言全绿**。待 B。
- 2026-08-11：代理 B 完成（M1.2）。`Entry` 补 `labelColor`/`collectionIds`；`makeBadgedIcon` 绘右下角色标方块 + 左上角收藏指示；右键「色标」/「收藏夹」子菜单；快捷键 `6-9`→色1-4、`Ctrl+6-8`→色5-7、`Ctrl+Shift+0`→清除；`Entry` 新增字段带默认值，MainWindow 现聚合初始化仍可编译。
- 2026-08-11：**Phase 1 三路全部完成**。派发 Phase 2 收口代理 D（M1.5：MainWindow 接线 + headless 复核 + GUI 编译验证）。
- 2026-08-11：**代理 D 收口完成**。MainWindow 接线全部落地（色标/收藏夹信号、3 参筛选、`CollectionsDialog` 菜单 + `collectionsChanged` 刷新、集合筛选）。headless **125 用例 / 745 断言全绿**（无回归，复核确认）；GUI 构建成功。M1 **代码层完成**。
- 2026-08-11：**M1 验收通过（用户确认），里程碑标记完成**。版本号升至 **v2.0.0**（`Constants.h` `kAppVersion`，同步 STATUS.md / README.md / CLAUDE.md）；测试计数同步为 125 用例 / 745 断言；STATUS.md「进行中」→ M2 预告。

## 遗留（待 M1 手动验收通过后处理，计划 §6 贯穿性收尾）

- 版本号同步：`src/app/Constants.h` `kAppVersion` 与 STATUS.md 不一致，M1 验收后统一更新（含 CLAUDE.md/README/STATUS.md）。
- 测试计数同步：实际 **125 用例 / 745 断言**（CLAUDE.md/README 写 95/512 已过期）。
- ProfileIO 专项测试（`tests/TestProfileIO.cpp`）：M1 或 M2 顺带，为 M3 兜底。
- 导航统计可顺带显示已收藏数（M1 可选）。
