# UI 全面检查报告 — LRC Clone Flash v2.8.0

> **检查日期**: 2026-08-12
> **基线**: v2.8.0 全绿 — `All tests passed (272994 assertions in 213 test cases)`
> **检查方式**: 5 路只读静态审查子代理并行(文件集互不相交),按八维度逐文件过:
> 信号/槽接线、生命周期/内存、线程安全、事件处理、状态一致性、空指针/越界、QSS 完整性、LRC 交互惯例。
> 本报告为检查交付物,**未修改任何代码**;修复需另立任务、由子代理实施。
>
> 分路:① 集成层(MainWindow/Constants/Application/main) ② 开发面板(DevelopPanel/CurveEditor/ColorWheel/PanelSection)
> ③ 视口与线程(ImageViewport/PreviewWorker/Histogram) ④ 浏览面板(Navigator/Filmstrip/ImportWorker)
> ⑤ 对话框与样式(5 对话框 + dark.qss)

**汇总:66 条发现(9 严重 / 19 中等 / 38 轻微),其中 4 条被 ≥2 路独立确认。**

---

## 🔴 严重(8 条,去重后,会导致功能失效 / 数据丢失 / 崩溃)

### 1. 键盘微调滑块不持久化,切图即丢数据
`src/ui/DevelopPanel.cpp:579` + `:553-576`
`adjustmentsCommitted` 只挂在 `sliderPressed/sliderReleased` 与 spin `editingFinished` 上;QSlider 键盘操作(方向键/PageUp/Home/End)只触发 `valueChanged` → 仅刷新预览。滑块获得焦点后用方向键微调 → 预览变了但**目录不保存、撤销栈不压栈**,切图/退出即丢。覆盖蒙版/HSL/色调分离/点颜色全部行。**唯一真正丢数据的发现。**

### 2. "已拒绝"旗标筛选恒空,被拒照片永久不可见
`src/ui/MainWindow.cpp:1613` + `src/catalog/CatalogDatabase.cpp:347` ⚠️ **两路独立确认**
`refreshFilmstrip` 设 `filter.flag = -1` 时未置 `includeRejected=true`,SQL 变 `flag = -1 AND flag != -1` → 恒空;默认视图也因 `includeRejected=false` 排除被拒照片。照片被 `X` 标记后胶片栏无任何入口可见/清除该旗标,"已拒绝"下拉形同虚设。

### 3. 退出竞态:关窗时 worker 访问已析构成员
`src/ui/MainWindow.cpp:607` + `src/ui/ImportWorker.cpp:49-138` ⚠️ **三路独立确认**
`stopWorkers()` 仅 `quit()` + `wait(3000)` 且忽略返回值,`importPaths`/`loadFullRes` 无取消/中断机制;批量导入或全尺寸解码超过 3 秒时关窗,worker 线程仍引用即将析构的 `catalog_/pipeline_` → **use-after-free / "QThread destroyed while running"** 崩溃。

### 4. 每次 `refreshFilmstrip()` 重放 selectionChanged,清空撤销历史 + 强制重解码
`src/ui/MainWindow.cpp:1675` + `src/ui/FilmstripWidget.cpp:189` ⚠️ **两路独立确认**
`refreshFilmstrip` 末尾 `selectImage(currentImageId_)` 在列表 `clear()` 后必触发 `currentRowChanged → selectionChanged → onImageSelected`,后者无条件 `undoStack_.clear()` + `requestPreview`(`loadImage` 不检查 `imageId==loadedImageId_`,每次都重新 decode)。改色标/收藏夹增删/导入完成/筛选变更都会触发 → 当前图撤销历史被静默抹掉,并伴随一次 RAW 全量解码。

### 5. 胶片栏缩略图每次刷新后清成占位块且永不重载
`src/ui/FilmstripWidget.cpp:129-145` + `:147-156`
`setImages` 用**空 `QIcon()`** 建项,完全不用 `Entry::thumbnailPath`;全工程唯一的 `setThumbnail` 调用点 `onThumbReady` 只由 ImportWorker 导入期间触发。打开目录/切换筛选/收藏夹增删/删除照片后全为占位图,且导入期间新图不进列表导致 `thumbReady` 打不中 `idToRow_`。**胶片栏核心功能失效。**

### 6. 恢复编辑历史追加两条相同快照
`src/ui/EditHistoryDialog.cpp:74` + `src/ui/MainWindow.cpp:841`
`onRestore()` 内 `saveAdjustments()`(内部 `saveEditSnapshot()` 追加一次)+ `showEditHistory()` 的 Accepted 分支 `commitAdjustments()` 再追加一次 → 每次恢复列表确定性膨胀 2 条相同记录。

### 7. 滚轮 snap-back 未判缩放方向,滚轮放大静默失效
`src/ui/ImageViewport.cpp:610`
`cur <= fitWheelScale() * 1.05` 未判断滚动方向。当 fit 比例 ≥ ~1.0(原生图 ≤1.05×视口,4K 屏 + ≤4000px 图/小图/截图常见),Fit 模式滚轮向上 → 切 1:1 → `cur` 立即 ≤ fit×1.05 → **弹回 Fit**。滚轮放大完全无效,且全程不发全尺寸请求。Ctrl+1/双击可绕过,但滚轮是主缩放输入。

### 8. 1:1 模式下换图,全尺寸永不重新请求,新图永久模糊
`src/ui/ImageViewport.cpp:63` + `src/ui/MainWindow.cpp:736`
`onImageSelected` 不重置 zoomMode_;`setImage` 把 `fullResRequested_` 静默置 false 且 `resetView()` 不动 zoomMode_ → 视口停在 OneToOne 显示下采样预览,无任何路径再触发全尺寸请求,直到手动 Ctrl+1。

---

## 🟡 中等(15 条,去重后,状态不一致 / 潜在问题)

| 文件 | 问题 |
|------|------|
| `src/ui/DevelopPanel.cpp:856` | 蒙版"反转"勾选从不发 `adjustmentsCommitted`,切图即丢 |
| `src/ui/DevelopPanel.cpp:1012-1051` | 蒙版下拉"取消选择"被 QComboBox 清空重填自动回选第 0 项破坏,切图后"工具已解除但蒙版 1 选中" |
| `src/ui/DevelopPanel.cpp:1354` | `syncRow` 预设越界值 UI clamp 但 `current_` 保留越界值,面板与引擎显示不一致 |
| `src/ui/DevelopPanel.cpp:264` | 色轮 600ms 提交定时器换图时未停止,压多余撤销条目 |
| `src/ui/DevelopPanel.cpp:148` | 曲线拖拽不走 30ms coarse 节流,1:1 下持续全分辨率重渲染(性能) |
| `src/ui/FilmstripWidget.cpp:213` | 清除角标后旧角标残留在图标上(`refreshIcon` 以含旧角标的图标为基底) |
| `src/ui/NavigatorWidget.cpp:88-194` | 每次 `refreshFilmstrip` 重建导航树,文件夹/集合选中态丢失,树显示与真实筛选脱节 |
| `src/ui/MainWindow.cpp:1675` | 当前图被筛选掉时 `currentImageId_=-1` 但 viewport/DevelopPanel 仍显示旧图,拖动滑块静默无渲染 |
| `src/ui/MainWindow.cpp:1652` | 文件夹筛选用裸前缀匹配,`C:/Photos` 会误含 `C:/Photoshop/…` |
| `src/ui/MainWindow.cpp:976` | 全局 eventFilter 焦点白名单漏 `QSlider`,键盘 Home/End/`[`/`]`/`+`/`-` 被全局逻辑截走 |
| `src/ui/MainWindow.cpp:964` | eventFilter 不检查 `activePopupWidget()`,右键菜单/下拉弹出时裸键仍触发全局快捷键 |
| `src/ui/MainWindow.cpp:704`/`664` | 导出/导入对话框空路径/未选文件静默 accept,主窗口无提示返回 |
| `resources/styles/dark.qss` | QPushButton 缺 `:disabled`(禁用按钮外观与启用无异);QLineEdit/QRadioButton 完全无深色规则(对话框核心输入框走原生浅灰) |
| `src/ui/ImageViewport.cpp:594` | `angleDelta().y()==0`(水平滚动)时 factor=1.0 仍切 1:1 触发全尺寸解码 |
| `src/ui/ImageViewport.cpp:647` | 空白视口点击误入 1:1 + 触发解码,letterbox 点击可能把图推出屏幕 |

---

## 🟢 轻微(38 条,精选)

- **死代码**:`zoomChanged` 信号全工程无 connect(缩放百分比从未显示)、`ImageViewport` 的 Key_O/Key_W 处理器被 MainWindow eventFilter 先吞、`HistogramWidget::computeFromBuffer`、`NavigatorWidget::showAll`、`MainWindow::gestureActive_`
- **快捷键**:缺 Ctrl+Y 重做(LR 惯例)、Tab 被全局劫持无法焦点遍历、吸管互斥单向(关 HSL 不恢复点颜色)
- `ImportWorker` 头注释称"带 SQLite 事务"但从未 `beginTransaction/commitTransaction`(与文档不符 + 批量导入性能)
- 导出路径默认落家目录而非当前照片目录;HDR 输出不校验 `.dng` 后缀;`ExportDialog::onFormatChanged` 会覆盖用户清空路径
- 滑块无双击复位/右键复位(LR 惯例);`PanelSection::setExpanded` 死代码、节展开状态未持久化
- `PreviewWorker`:`loadFailed`/`imageMeta` emit 无代次守卫,旧图解码失败误报"加载失败";coarse 后随 refine 造成短暂清晰度抖动(M5 固有)
- **性能**:`paintCheckerboard` 每次 paint ~8 万次 `fillRect`(4K);`rebuildMaskOverlay` 笔画中每次全量重建权重场;`setAdjustments` 硬编码 `8` 而非 `ColorRange::Count`;`resetAll` 重复调用 `resetToLinear`

---

## ✅ 值得肯定

- **线程安全整体良好**:PreviewWorker 的 shared_ptr 经信号/槽跨线程传递正确(ImageBuffer 不可变 + 零拷贝 QImage 持活),`latestGeneration_`/`fullResLatest_` 单 worker 线程串行访问无竞态,coarse/refine 因 FIFO 不乱序覆盖;`brushWeightField` 全局缓存有互斥锁。
- 五路均确认 M1–M5 的粗/精预览、蒙版缓存、双向同步、撤销栈主体接线正确。

---

## 建议修复批次

| 优先级 | 内容 | 涉及 |
|--------|------|------|
| **P0 数据安全** | #1 键盘滑块持久化、#4 撤销栈被清、#6 双快照 | DevelopPanel / MainWindow / EditHistoryDialog |
| **P0 核心功能失效** | #5 缩略图、#2 已拒绝筛选、#7 滚轮放大、#8 1:1 换图 | FilmstripWidget / MainWindow / CatalogDatabase / ImageViewport |
| **P1 稳定性** | #3 退出竞态(worker 加中断检查 + stopWorkers 兜底) | MainWindow / ImportWorker / PreviewWorker |

修复遵循执行纪律:每批一个集成子代理收口,主代理构建验证;中文注释行以 ASCII 结尾。
