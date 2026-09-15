# 架构文档

> 当前任务状态入口：[竞品对齐优化实施规格](spec/competitive-optimization-implementation-spec.md)。该规格目前处于待实施状态；本文件描述稳定架构约束，不代表其中的 P1/P2/P3/T 功能已经完成。

## 1. 分层与依赖规则

```
┌─────────────────────────────────────────────────────────────┐
│  ui/   (Qt6 Widgets)      MainWindow · 自绘控件 · 2 个 worker │
│        ── 通过信号槽与引擎/目录层交互，禁止直接操作像素       │
├─────────────────────────────────────────────────────────────┤
│  catalog/  (纯 C++，无 Qt)   CatalogDatabase · ThumbnailCache│
│             · FolderMonitor · LibraryManager  ── 依赖 engine │
├─────────────────────────────────────────────────────────────┤
│  engine/   (纯 C++，无 Qt)   RawDecoder · ImageBuffer ·      │
│             Pipeline · Adjustments · ColorManager · Export   │
│             Engine · ImageIO · HdrMerge · DngConverter ·     │
│             ZoneCurve · Xmp · XmpSidecar · ImageFormats ·    │
│             LensfunProfile · DcpProfile                     │
├─────────────────────────────────────────────────────────────┤
│  utils/    Json · ProfileIO · XmpProfile · MathUtils ·      │
│             Parallel · UndoStack · ViewportMath             │
└─────────────────────────────────────────────────────────────┘
```

- `engine/` 与 `catalog/` 的头文件**禁止包含任何 Qt 头文件**，因此整个管线可在
  无 Qt 的环境下编译、单测（`ctest`）。
- 跨层数据用引擎自有的 `ImageBuffer`（U16/F32/U8 RGB 交错布局）传递；UI 层在
  `toQImage()` 处一次性拷贝为 `QImage`。

## 2. 非破坏性编辑管线（核心）

### 2.1 数据流

```
ARW ──RawDecoder──▶ 线性 ProPhoto RGB (U16, 相机矩阵手动应用)
                       │  decode 仅一次，之后不再访问原始文件
                       ▼
              ImageBuffer (全尺寸，360MB 级)
                       │  resize() 面积平均降到预览尺寸
                       ▼
   Pipeline.render(imageId, linear, Adjustments, maxDim)
     applyAdjustments(线性光 gamma=1.0):
       1.  曝光       v *= 2^EV
       2.  对比度     围绕中灰 0.18 的线性增益
       3.  高光       平滑权重（Hermite），影响 >0.35 的区域
       4.  阴影       平滑权重（Hermite），影响 <0.35 的区域
       5.  白色/黑色  白点/黑点（clipping/crushing）
       6.  色调曲线   分区点曲线（高光/亮色调/暗色调/阴影，Fritsch–Carlson 单调三次）
       7.  白平衡     色温/色调围绕 as-shot WB 缩放 R/G/B
       8.  去朦胧     大气去雾
       9.  鲜艳度     保护肤色的饱和度提升
       10. HSL       各通道独立调整（8 色相范围 × 色相偏移/饱和度/明亮度）
       11. 色调分离   高光/阴影独立着色，平衡滑块控制过渡中点
       12. 点颜色     ColorTarget：吸管取参考色，按色相/饱和接近度加权调整
     applySpatialAdjustments(线性光): 纹理/清晰度/锐化/畸变/颗粒/晕影/蒙版局部
     ColorManager.toDisplaySRGB(): lcms 浮点 ProPhoto→sRGB，含gamma
                       ▼
              U8 sRGB 预览 (1920 最长边) ──LRU 缓存──▶ QImage + 直方图
```

### 2.2 关键设计

- **全线性运算**：调整只在线性光下进行，sRGB gamma 仅在最后一步由 lcms 施加，
  避免反复 gamma 造成条带。
- **预览尺寸**：解码时用 libraw `half_size` + 面积平均降到 1920 最长边；拖动滑块走
  **coarse-then-refine**（30ms 节流先出 480px 粗帧，停顿/松手后 1920 精帧）；全尺寸
  渲染只在导出 / 1:1 放大时执行。
- **缓存**：`PipelineCacheKey = hash(imageId, adjustments.hash(), maxDim)`，
  256MB LRU（maxDim 参与键，粗/精/全尺寸三档天然分离）。滑块拖动命中缓存则零重算。
- **1:1 全尺寸**：`PreviewWorker::loadFullRes/adjustFullRes` 走
  `halfSize=false, maxDim=0` 解码，`renderLinearFull` + `toDisplaySRGB` 渲染（**不塞
  LRU**），同一时刻至多 1 份全尺寸缓冲，退出 1:1 / 换图即释放；默认调整下优先从目录
  内嵌预览 / 磁盘 1:1 缓存直读，免重复解码。
- **并发**：`PreviewWorker` 单线程串行处理预览请求（请求按序到达，最新结果
  必然最后到达），generation 计数丢弃过时结果。`ColorManager` / `Pipeline` /
  `CatalogDatabase` / `ThumbnailCache` 均内部加锁，可跨线程共享。

### 2.3 镜头与相机配置文件集成状态

- `RawMeta` 从 LibRaw 传递相机厂商/型号以及 `LensMake`/`Lens`，供 Qt-free
  `LensfunProfile` 查询器使用。
- CMake/vcpkg 已声明 Lensfun；未安装时查询器保持安全空结果，现有 RAW 管线不变。
- `DcpProfileReader` 已识别 Adobe 的 `IIRC` DCP TIFF 变体，并读取 profile 名称、
  相机型号、ColorMatrix1/2 与 ProfileToneCurve 长度；已用
  `resources/Camera/Canon EOS 1000D/Canon EOS 1000D Camera Faithful.dcp` 回归。
  矩阵/曲线实际应用、HSV/look LUT 和 UI 选择仍待完成。

### 2.4 色彩管理

- 工作空间：**线性 ProPhoto RGB (D50)**。
- `RawDecoder`：`output_color=0` 取相机线性 RGB → 手动 `cam_xyz`(相机→XYZ D65)
  → Bradford 适配 D65→D50 → XYZ→ProPhoto 矩阵（均见 `ColorManager`）。
- 显示/导出：lcms 浮点变换（`cmsFLAGS_FLOATSHAPER`），目标 sRGB（可替换为显示
  器 ICC）。
- 白平衡：解码时应用相机 as-shot WB（`cam_mul`）；开发面板色温/色调是相对偏移。

## 3. 目录与持久化

- `.lrcat` 为 SQLite 单文件（WAL），表：
  - `images(id, file_path UNIQUE, import_date, rating, flag, label_color, thumbnail_path,
    fullres_path, is_managed)` —— `is_managed` 标记受管库复制/移动导入
  - `edits(image_id PK, adjustment_json)` —— 参数堆叠的 JSON 序列化
  - `folders(id, path UNIQUE, parent_id)`、`collections(id, name, sort_order)` +
    `collection_images(collection_id, image_id, 复合主键)`（收藏夹，级联删除）
  - `edit_history(image_id, ts, adjustment_json)` —— 每次提交的参数快照（撤销/恢复来源）
  - `previews(image_id, kind, data BLOB, width, height, 复合主键)` —— **目录内嵌预览**
    （320px 缩略图 + 1:1 JPEG），导入时预生成，默认调整下 1:1 直读
  - `settings(key, value)` —— 如 `library_root`
- 批量导入在 `BEGIN IMMEDIATE` 事务内逐条写入；`file_path` 唯一约束使重复导入幂等；
  begin/commit/rollback 返回值均检查，杜绝事务悬挂。
- **导入/导出批量重命名（P1-3）**：`utils/RenameTemplate.h` 提供 Qt-free 的
  `renderRenameTemplate`（`{date}`/`{index[:W]}`/`{base}`，大小写不敏感）与
  `uniqueRenamePath`（冲突自动 `-N` 后缀）。导入端在 `ImportWorker` Stage1 落盘后
  按选择顺序（1 起）+ 源文件修改日期渲染并重命名，`renameMutex_` 串行化查重+重命名
  保证同批唯一；导出端启用重命名时 `MainWindow` 走 `PreviewWorker::batchExport`，
  为母版+所有虚拟副本按模板在输出目录下生成唯一文件名。
- 缩略图/1:1 缓存：优先目录内嵌 `previews` 表 BLOB，其次磁盘
  `<AppData>/ASE-128/Silver Halide/cache/thumbnails/`（`QStandardPaths::AppDataLocation`，
  即 `<APPDATA>/<organizationName>/<applicationName>`），文件名 = FNV-1a 稳定哈希；
  写入走临时文件 + 原子改名，崩溃不留截断文件。

## 4. 线程模型

| 线程 | 职责 | 与 UI 通信 |
|------|------|-----------|
| 主线程 | 布局、事件、直方图、DB 读 | — |
| `PreviewWorker` (QThread) | 解码 + 管线渲染（1920 精 / 480 粗 / 全尺寸 1:1）+ 导出 | `previewReady`/`previewReadyCoarse`/`fullResReady`(imageId, ImageBufferPtr) 队列信号 |
| `ImportWorker` (QThread) | 事务批导入（含 Stage1 模板重命名）+ 缩略图/1:1 预生成 + 后台解码回填 | `thumbReadyImage`/`progress`/`finishedImport` 队列信号 |

UI 通过 `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` 下发请求，跨线程
自定义类型（`ImageBufferPtr`、`Adjustments`）已在 `main()` 中注册元类型；两 worker
均带 `std::atomic` abort 标志，退出时先 requestAbort 再 quit + wait，杜绝 use-after-free。

## 5. 已知取舍

- 调整顺序固定（曝光→对比度→高光→阴影→白/黑→曲线→白平衡→去朦胧→鲜艳度→HSL→色调分离→点颜色→空间阶段），
  参数堆叠是单层 JSON，无图层/快照；编辑历史为每次提交的线性快照，无分支/时间线。
- 1:1 为真全尺寸：`PreviewWorker` 后台全尺寸解码，同一时刻 1 份缓冲、不塞 LRU。
- 非 RAW 导入（JPEG/PNG/TIFF）经 sRGB → 线性 ProPhoto 转换；16 位 TIFF 导入时量化为 8 位；
  TIFF 读取器仅支持未压缩 RGB（应用自身导出格式及常见工具产物）。
- 文件夹监视用 2s 轮询（`std::filesystem`），非 OS 事件驱动。
- 直方图直接统计预览 8bit，每 4M 像素抽样一次。
- 蒙版局部调整的画笔权重场（`Pipeline::brushWeightField`）以 COW `shared_ptr` 缓存，键 =
  `geometryHash + w + h`，拖动局部调整滑块不重建权重场。

## 6. 目录结构

```
./                            # 源码根 = 工作区根
├── CMakeLists.txt          # FetchContent + 可选 vcpkg toolchain；MSVC 全局 /EHsc
├── vcpkg.json              # vcpkg 依赖清单
├── src/
│   ├── main.cpp            # 入口：注册元类型、应用启动
│   ├── app/                # Application、Constants（版本/缓存容量/应用名）
│   ├── engine/             # RawDecoder ImageBuffer Pipeline Adjustments
│   │                       # ColorManager ExportEngine ImageIO ImageFormats
│   │                       # HdrMerge DngConverter ZoneCurve Xmp XmpSidecar
│   ├── catalog/            # CatalogDatabase ThumbnailCache FolderMonitor LibraryManager
│   ├── ui/                 # MainWindow + 自绘控件（CurveEditor/Histogram/Filmstrip/
│   │                       # Navigator/DevelopPanel/ImageViewport/ColorWheel/PanelSection）
│   │                       # + 5 对话框 + ImportWorker/PreviewWorker + MetaTypes
│   └── utils/              # ProfileIO XmpProfile Json MathUtils Parallel
│                           # UndoStack(头文件) ViewportMath(头文件)
├── tests/                  # Catch2 单测（无 Qt，25 个 Test*.cpp）
├── resources/styles/       # 暗色主题 QSS（dark.qss）
├── resources/icons/        # 应用/ exe 图标（make_icons.py 生成）
├── packaging/              # NSIS 安装脚本 installer.nsi + welcome.bmp/make_bmp.py
├── docs/                   # 架构、XMP 映射、当前任务规格；历史归档在 archive/
└── third_party/            # vendored 依赖（libraw/lcms2/sqlite3/Catch2/pugixml/stb）
```
