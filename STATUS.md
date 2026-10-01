# 项目状态 — v1.0.0 正式版 · Silver Halide

> **版本号说明**：v1.0.0 是 Silver Halide 的首个正式发布版本。
> 下文记录的 v2.x 版本号均为内部开发里程碑（dev milestones），仅用于团队跟踪开发进度，
> 从未对外发布；不要把它们当作已发布的对外版本。所有内部 2.x 里程碑的工作已经收敛到 v1.0.0 正式版中。
>
> **当前发布**：`dist/Silver Halide Setup 1.0.0.exe`（Inno Setup 打包）。
> 构建输出：`build_gui/SilverHalide.exe`（Ninja + MSVC 2022 x64 生成器）。
> 构建后需运行 `windeployqt` 部署 Qt 运行时。
>
> **打包（Inno Setup）**：exe 为 GUI 子系统（`WIN32`，无控制台）。发布载荷暂存于
> `build_gui/install_stage/`（exe + Qt DLL + plugins + VC++ 运行时，不含 .lib），
> 由 `packaging/installer.iss` 用 `ISCC.exe` 编译出 `dist/Silver Halide Setup 1.0.0.exe`。
> 见 `packaging/installer.iss` 头部注释。

## UI 优化四条 + GUI 测试运行库部署（2026-10-01）

前置提交：`618be00`（UI 与样式，12 文件）、`6806bc2`（AGENTS.md 记录 kimi-cu 验收流程），
两者已推送。本节的 GUI 测试部署与 STATUS 更新随本提交一并落地。

### 1. 画布背景改为中性灰（`src/ui/ImageViewport.cpp`）

`paintEvent` 与 `paintCompare` 此前对**整块画布**无条件调用 `paintCheckerboard()`，棋盘格原本
只该用于标示透明区域。现在新增 `paintCanvasBackground()`：仅当图像带 alpha 通道时才画棋盘格，
普通不透明照片铺 `#333333` 中性灰（比窗口底色 `#2b2b2b` 略亮，图像边界更清楚）。实测：竖幅图在
“适合窗口”下左右留白为纯色，无格纹。

### 2. 界面状态持久化（`src/ui/MainWindow.*`、`src/ui/DevelopPanel.cpp`）

此前只有 `catalogPath` 一个键落盘，窗口几何、分栏比例、分区折叠状态每次启动都重置。现在：

- 窗口几何与两条分栏状态写入 QSettings 的 `ui/windowGeometry`、`ui/mainSplitterState`、
  `ui/centerSplitterState`；在 `closeEvent` 保存，析构函数再兜一次。
- 11 个调色分区的折叠状态按**稳定 ASCII 键**存取（`develop/section/<key>`，如 `basic`、
  `toneCurve`），与翻译无关；面板内容高约 3000px，此前全部分区默认展开。
- 存的状态不可用时（Qt 会对 blob 做版本校验）回落到 `buildUi()` 里的默认尺寸，不产生空布局。

实测往返：折叠“基本”并拖宽面板 → 关闭（注册表出现 `develop\section\basic=false` 与三条几何 blob）
→ 重启后折叠状态与宽度均保持。

### 3. 主题颜色收敛（`resources/styles/dark.qss`）

新增 `QLabel[role="subTitle"|"subLabel"|"hint"]` 与 `[role="hint"][state="ok"]` 规则。`DevelopPanel`
的内联 `setStyleSheet` 由 16 处降至 6 处；余下 6 处是**数据驱动色板**（显示取样/拾取到的颜色），
背景属于用户数据而非主题，已加注释说明；`SoftProofingDialog` 的 2 处同理。动态状态用
`setProperty` + `repolish()`（Qt 只在 polish 时解析属性选择器）。

### 4. 中英混排与 i18n 收口

- 数组型显示名统一为 `QT_TR_NOOP(...)` + 使用处 `tr(...)`：`CurveEditor` 音区名、
  `FilmstripWidget` 色标名、`MainWindow` 旗标/色标名、`SmartCollectionsDialog` 三张表。
- 未包裹的显示中文字面量清零（剩余 65 处是传给内部已调 `tr()` 的辅助函数的参数，属正确写法）。
- **两个整块英文对话框改中文**：软打样、重复项检测结果（ICC 配置文件名、渲染意图等专有名词
  按惯例保留/单独处理）。视口空状态、直方图提示同样改中文。
- **顺带修复两个真 bug**：(a) `ExportDialog::browse()` 用 `QString::fromLatin1` 解码 UTF-8
  中文字面量，保存对话框的“保存类型”显示乱码（已改为 `tr()`，实机确认显示 `图像 (*.jpg)`）；
  (b) `CurveEditor::paintEvent` 里局部变量 `tr` 遮蔽 `QObject::tr()`，重命名为 `topRight`。

### 5. GUI 测试缺 DLL（`tests/CMakeLists.txt`）

`build_gui/tests/` 内**没有任何 Qt DLL**，且 Qt bin 不在 PATH；Windows 不搜索父目录，因此直接运行
`lrc_qt_tests.exe` 会弹缺 DLL 对话框并停住，外观与“假卡死”一致（CPU 占用为 0）。

修法：给 `lrc_qt_tests` 增加 POST_BUILD `windeployqt`（`--release --no-translations
--compiler-runtime`），工具位置从 `Qt6::qmake` 推导；找不到时降级为警告。实测部署出
`platforms/qwindows.dll` 等，exe 可正常启动退出。

### 6. 验证与已知缺口

- `cmake --build build_gui` 通过；headless `build_gui/tests/lrc_tests.exe` **480 用例 /
  878871 断言全绿**。
- `lrc_qt_tests.exe` 退出码 0，且测试体确实执行（运行期间 `%TEMP%\lrc_qt_tests` 被创建）。
- kimi-cu 实机验收：画布纯色、折叠/分栏跨重启保持、原生保存对话框过滤器中文正确、空状态中文。
- **缺口（待后续处理）**：该套件的 QTest 报告在本机取不到——stderr 正常但 stdout 无输出，
  `-o -,txt` 亦为空；改用 `-o file,txt` 只能拿到最后一个测试类，因为 `main()` 把同一份
  `argc/argv` 传给全部 13 次 `QTest::qExec`，每个类都会重新截断同一文件。因此本次没有取得
  该套件的用例级计数，不能沿用旧的“12 类 / 48 用例全绿”说法。

## 1a 热路径验收工具化：埋点 + worker 级计时 + 运行手册（2026-09-28，工作区未提交）

- **埋点**：`src/ui` 新增 `[PERF1A]` 计时日志。t0 = `MainWindow::requestFullRes()` 请求下发，t7 =
  `MainWindow::onFullResReady()` 帧呈现（端到端判定口径）；worker 侧 `PreviewWorker::loadFullRes()`
  打出 `hit=earlyReturn|embedded|disk|memU8`、`decode=done`、`render=done`、`worker=done`（归因用）。
  埋点零副作用，不改变控制流、缓存写入条件与 `fullResReady` 的发出顺序；engine/catalog 未引入 Qt。
- **测试**：`tests/lrc_qt_tests.cpp` 新增 `FullResHotPathTest`（3 用例）——分别计量目录内嵌预览与
  legacy 磁盘缓存两条直读路径的请求→出帧耗时，验证同图同状态重复请求走 `earlyReturn`（无渲染、无帧，
  不计入锚点判定），并在 `clearFullRes()` 后确认可重新出帧。
- **文档**：新增 [docs/spec/optimization/pillars/01-pa/1a-hot-path-runbook.md](docs/spec/optimization/pillars/01-pa/1a-hot-path-runbook.md)
  （样本与机器、构建命令、埋点位置表、冷态重置、测量步骤、产出物、边界）；
  [M1-zero-wait.md](docs/spec/optimization/pillars/01-pa/M1-zero-wait.md) 补 2026-09-28 实测小节。
- **验证（Release，本机）**：`build_gui/tests/lrc_qt_tests.exe` **12 类 / 48 用例全绿**（exit 0）；
  `build_gui/SilverHalide.exe` 重新编译链接通过；headless
  `build_headless/tests/Release/lrc_tests.exe "[performance]"` **4 用例 / 24 断言全绿**
  （B-2 精帧 min=98.0ms / avg=100.4ms，B-1 引擎段 623.7ms，D-2 回落 688.2MB 可重复）。
- **状态**：代码与文档已落地并自测通过；**B-1 锚点仍为真机待验**——本次数字只覆盖 PreviewWorker
  管道且夹具为 256×192 合成图，不构成 50MP 锚点判定。

## 元类型重复声明 + 软打样三项遗留 + profile 字节可复现（2026-09-26，工作区未提交）

### 1. `ImportMode` 元类型重复声明（C2766 隐患，已清除）

`ui/MetaTypes.h` 与 `ui/ImportWorker.h` 各自 `Q_DECLARE_METATYPE(lrc::ImportMode)`。该宏展开为
`QMetaTypeId<T>` 的**显式特化**，同一翻译单元内二次声明即 MSVC C2766，任何同时包含两个头的 TU
直接编译失败（此前 `tests/lrc_qt_tests.cpp` 是靠「不包含 MetaTypes.h + 本地重复声明
`SoftProofParams`」绕开的）。

- **修法**：`ui/MetaTypes.h` 成为**唯一声明点**（补 `@warning` 说明）；`ui/ImportWorker.h` 改为包含它，
  并留注释禁止在别处再声明；`lrc_qt_tests.cpp` 删掉本地声明。
- **过程中暴露的更深依赖**：声明搬进 MetaTypes.h 后，AUTOMOC 的聚合 TU（`mocs_compilation.cpp`
  把所有 `moc_*.cpp` 合成一个 TU）里 `moc_DevelopPanel.cpp` 先**隐式实例化**
  `QMetaTypeId<lrc::Adjustments>`，之后才看到特化 → C2908 + C2766。根因是「把引擎类型放进信号/槽的
  头没有包含元类型声明头」，此前只是靠 AUTOMOC 的排序恰好没触发（改一行包含顺序就可能炸）。
  现按规则给 8 个真正在信号/槽里携带这些类型的头补 `#include "ui/MetaTypes.h"`：
  `DevelopPanel` / `MainWindow` / `PreviewWorker` / `PanoramaWorker` / `PanoramaDialog` /
  `SnapshotDialog` / `PresetDialog` / `SoftProofingDialog`（`HistogramWidget::computeFromBuffer`
  是私有非槽方法，moc 不可见，故未加）。

### 2. 软打样三项遗留（全部落地）

- **(a) 报警色只作用于色域报告 PNG**：lcms 的 out-of-gamut 报警码存在 **context** 的 client chunk 里、
  **每次 `cmsDoTransform` 按 `p->ContextID` 读取**（`cmsxform.c:296/453`），所以改色无需重建变换。
  现引擎新增 `SoftProofing::setGamutAlarmCodes()/gamutAlarmCodes()`，并把整个实例改用**私有 lcms
  context**（`cmsCreateContext` + 全部 `...THR` 变体）——既让颜色真正生效，也去掉了旧构造函数
  `cmsSetAlarmCodes()` 写**进程全局**状态的副作用（那会重染同进程任何变换，并与并发变换构成数据竞争）。
  `gamutMask()` 的判定改为匹配**当前配置色**：此前硬编码纯红，用户一换色掩膜就整体变空（静默失效）。
  worker 在 arm 时把对话框的颜色推进引擎，预览叠加层与报告 PNG 从此一致。
- **(b) 1:1 全尺寸视图不叠加打样**：1:1 帧改由 worker 自己打样——`loadFullRes`/`adjustFullRes` 在 arm 时
  用 `proofLinear()` 取代 `toDisplaySRGB()`，`emitFullResCoarse` 同理。打样帧**不写** U8 缓存、
  目录内嵌全尺寸预览与旧磁盘缓存（那些是未打样产物），并且帧去重键混入**打样状态盐**
  （intent / gamut 开关 / 报警色；关闭时为 0）→ 同一次调整下切换打样或换色也会重新渲染。
- **(c) 拖动中的粗帧不打样**：`adjustPreviewCoarse` 在 arm 时经新入口
  `Pipeline::renderLinearBounded()`（= `renderInternalLinear`，与显示路径**同一 stage**，不做 sRGB
  转换、不入显示 LRU）取线性缓冲再打样后发出；未 arm 时路径与行为完全不变。
- 新增 `PreviewWorker::setProofState()` 新槽，MainWindow 在参数变化时 queued 下发；原来「预览帧渲染完成
  后再补一次 requestProof」的机制保留（保证精修帧一致），但粗帧不再依赖它。

### 3. `exportProfileBytes()` 跨进程字节不稳定（已修，导出可复现）

- **取证**（临时探针把三份预置落盘，跨进程 + 间隔 3s 逐字节对比）：三份预置**只差 1 个字节，偏移
  35** —— ICC 头 `dateTimeNumber` 的「秒」字段，即 lcms 用挂钟时间戳生成 profile；profile ID
  （偏移 84）无变化 ⇒ 内存构造的 profile 未计算 MD5 ID，固定日期不会造成「ID 与内容不一致」。
- **修法**：`exportProfileBytes()` 序列化后把偏移 24..35 的 6 个大端 uInt16 固定为
  `2020-01-01 00:00:00`。修复后同样两次跨进程运行，三份预置**差异 0 字节**；export 因此在固定设置下
  字节可复现（校验和、去重、字节金标准测试都成立）。

**测试登记**：

- `tests/TestColorManager.cpp`：`exportProfileBytes is byte-reproducible`（两次调用逐字节相同、声明长度
  自洽、时间戳为固定值、`acsp` 未被改坏）。
- `tests/TestSoftProofing.cpp`：`the alarm colour drives both the proof frame and mask`（默认纯红 → 换绿后
  帧与掩膜同步跟随、掩膜不再变空）、`the alarm colour is per-instance, not process-global`（两个实例交错
  使用各保持自己的颜色，证明私有 context）。
- `tests/TestPipeline.cpp`：`renderLinearBounded stays linear F32 at a bounded size`（F32、尺寸上限、
  不修改源缓冲，且 `toDisplaySRGB(该缓冲) == render()` 逐像素一致 ±1，即「就是显示路径所转换的那一级」）。
- `tests/lrc_qt_tests.cpp`：`SoftProofRenderPathsTest` 端到端驱动真实 worker——未 arm 的粗帧无报警色、
  arm 后粗帧带报警色；arm 时 1:1 帧带报警色、关闭后同一次调整重新渲染回无报警色。报警色取**品红**，
  fixture 里不存在该颜色，所以「这帧有没有打样」是精确的像素计数而不是统计判断。

**反向验证**：临时让 `proofArmed()` 恒为 false 后重建，两个新 Qt 用例按预期转红
（`the coarse drag frame did not carry the soft proof` / `the 1:1 full-res frame did not carry the soft proof`），
确认测试不是空过。

**验证（Release，本机）**：GUI 全量构建通过；`lrc_qt_tests.exe` **11 个测试类全绿**；headless 全量
**480 用例 / 878886 断言全部通过**。

## R-08：JPEG 嵌入 ICC 丢 SOS 标记（缺陷修复，2026-09-26，工作区未提交）

`TestRawDecoder.cpp:556`（embedded ICC JPEG 用例）长期 flaky 的根因**不是阈值**，而是写入端产出
了**结构非法的 JPEG**；顺带修掉同一函数里第二处互操作缺陷。

- **根因（写入侧丢 SOS 标记）**：`ImageIO.cpp` 的 `parseJpegSegments()` 命中 SOS 分支时先 `i += 2`
  再记 `pixelStart`，偏移指向 SOS 的 length 字段而**不是 `FF DA` 标记本身**；`embedIccJpeg()`
  从该偏移原样回贴尾部，于是每次带 ICC 写出的 JPEG 都**缺 SOS 标记**。
  - 严格解码器（GDI+ / Qt 的 libjpeg）直接拒收：`Invalid JPEG file structure: missing SOS marker`。
  - stb 过于宽容：把熵数据当头部继续扫，输出缓冲只写了一半，**未写到的字节是堆上残留**，
    同一份字节流每次解码结果都不同 → 该用例的 ΔG 在 0.005~0.825 间随机跳，时过时不过。
  - 读取路径 `extractIccFromJpeg()` 不使用该偏移，所以 **ICC 提取始终正确**，签名类断言全绿，
    缺陷因此长期隐身（`TestCompetitiveP1` 只断言 `contains("ICC_PROFILE")`，从不解码）。
- **产品影响**：`ExportEngine::exportJpeg()` 只要 `IccChoice != None`（sRGB/AdobeRGB/DisplayP3）
  就会导出**外部打不开**的 JPEG（默认 `None` 路径不受影响，PNG iCCP / TIFF tag 34675 路径正确）。
- **修复**：`parseJpegSegments()` 改为在跳过标记**之前**记录 `markerStart`，SOS/EOI 分支写
  `pixelStart = markerStart`（尾段含标记本身）；未找到时哨兵值由 `0` 改为 `jpg.size()`（不再回贴
  整份文件），并补齐函数契约注释与 `@warning`。
- **同函数第二处缺陷（由新增 Qt 用例发现）**：`jpegIccMarkers()` 原先按 **255 字节**切分 ICC。
  JPEG 段长是 16 位，APP2 载荷上限 65533、减去 14 字节 ICC 头即 **65519 字节/段**；255 字节切法
  把 588 字节的 sRGB profile 切成 3 段（尾段仅 78 字节），而 Qt 的 JPEG 读取器**会静默丢弃
  载荷 ≤146 字节的 ICC_PROFILE 段**（`qjpeghandler.cpp` 中 `data_length > 128 + 4 + 14`），
  于是 Qt 只拿到 510/588 字节，`QColorSpace::fromIccProfile` 报 `failed size sanity 2` 并**整个
  忽略该 profile** → Qt 系消费者会把我们的 wide-gamut 导出当 sRGB 处理。
  现改为按 **65519 字节**切分（与 Qt / ImageMagick 一致，<64 kB 的 profile 永远是单段），
  超过单段时按 `ceil(size/count)` **均分**，保证不出现接近该阈值的小尾段；超过 255 段
  （seq/count 为单字节）无法表示时**丢弃 profile**，而不是写出 count 与实际段数不符的坏文件。
- **测试登记（均先复现后修复）**：
  - `tests/TestImageIO.cpp` 新增 4 用例：ICC JPEG 的 SOS 结构校验（独立实现的段游走，不复用引擎
    解析器）＋ 解码回读像素＋双次解码一致性；无 ICC 路径字节等价于 `encodeJpeg()` 且结构有效；
    70 kB profile 均分多段且 `loadSourceImage().icc` 逐字节相等；>255 段（16.7 MB）profile 被
    丢弃但 JPEG 仍有效可解码。
  - `tests/lrc_qt_tests.cpp` 新增 `IccJpegDecodabilityTest` 2 用例：导出 JPEG 必须被 Qt 的
    **独立解码器**完整读出（尺寸、平铺色块像素、`colorSpace().isValid()` 且 profile 长度一致）；
    反向对照——把同一文件删掉 `FF DA` 后 Qt 必须拒收（证明判定器对该缺陷敏感）。
- **反向验证**：临时还原 `pixelStart = i` 后重建，`REQUIRE(sos != npos)` 与解码回读像素断言
  **双双转红**（`bytes=1226 sos=npos`），确认新测试确为该缺陷的守卫；固定后 `[raw][io]` 连跑
  **20/20 全绿**（修复前同一用例约半数失败）。跨进程交叉验证：GDI+ 从修复前的文件读出完整
  588 字节 profile，段头实测 `271/271/94 seq=1/3,2/3,3/3` + `SOS@0x4C1`；修复后单段 602 字节，
  Qt 报 `cs valid: true cs profile size: 588`。

**验证（Release，本机）**：headless 全量 **476 用例 / 878791 断言全部通过**（此前基线 469 用例 /
878717 断言，唯一失败即本次修复的 R-08 JPEG 用例）；GUI 全量构建通过；`lrc_qt_tests.exe`
**10 个测试类全绿**（新增 `IccJpegDecodabilityTest`，且 `qt.gui.icc` 警告消失）。

## UI 层接线修复 + 两个孤儿模块接入（2026-09-26，工作区未提交）

对 `src/ui` 做了一次「信号声明 ↔ emit ↔ connect」三方对账与对话框入口追踪，修复下列接线缺陷；
**未删除任何既有功能**，两个此前完全不可达的模块（软打样、查重）改为补入口接入。

- **颜色范围「取样」按钮空转（真缺陷）**：`DevelopPanel` 的独立颜色范围取样按钮 emit 的
  `colorRangeSamplerRequested(bool)` 在全仓库没有任何接收者，点击无反应，只有 `C` 快捷键能激活
  viewport 取样器。现补 `connect(... → ImageViewport::setColorRangeSamplerActive)`；新增
  `DevelopPanel::setColorRangeSamplerChecked(bool)`（信号阻断式镜像），并在 `C`/`W` 快捷键路径
  回写面板按钮态。
- **取样器互斥不完整**：`W`/`C` 之前只改面板按钮（信号被阻断，viewport 标志未复位），会导致
  两个 sampler 同时 armed、按钮与 viewport 状态漂移。现两条路径显式复位
  `setSamplerActive/setPointColorSamplerActive/setColorRangeSamplerActive/setLuminanceSamplerActive`
  并同步面板按钮，顺带让此前定义但无人调用的 `setRangeSubSamplerChecked()` 真正投入使用。
- **`lensfunEnabledChanged` 死信号**：`DevelopPanel.h` 声明后既无 emit 也无 connect。现由 Lensfun
  开关 emit，`MainWindow::onLensfunEnabledChanged` 负责状态栏反馈（持久化仍走
  `adjustmentsCommitted`，不重复提交）。
- **软打样（视图 → 软打样…）接入**：此前 `engine/SoftProofing` + `ui/SoftProofingDialog` 完整实现
  但全仓库无构造点。现补菜单项（无模式对话框）、`SoftProofing::loadProofProfileFromMemory()`
  （内置 sRGB / Adobe RGB / Display P3 预设以内存 ICC 加载，无需随包分发 .icc）、
  `PreviewWorker::requestProof()`（渲染调整后线性 ProPhoto → 打样到 sRGB U8，关闭时无损回落到普通
  预览）与 `PreviewWorker::requestGamutReport()`（按报警色输出 U8 色域掩膜，主线程写 PNG）。
  CMYK 打样预设（Fogra39 / SWOP）本仓库无 ICC 文件，不能解析时给出明确状态栏提示而非静默失败。
- **查重（照片 → 查找重复项…）接入**：此前 `app/DuplicateDetector` + `CatalogDatabase` 的
  `photo_features` / stack API + `ui/DuplicateResultsDialog` 完整但无入口。现补菜单项，
  `QtConcurrent` 后台扫描（读目录内嵌缩略图，不做 RAW 解码），复用 `getPhotoFeatures` 作为缓存种子，
  新算出的 phash/embedding 经 `storePhotoFeatures` 写回；结果进入结果对话框，`groupStacked` 刷新
  胶片栏并提示，`focusImagesRequested` 跳转到首个成员。析构前 join 扫描，避免 use-after-free。
- **头文件命名空间污染**：`ui/SoftProofingDialog.h` 把 `class ColorManager;` 前置声明写在全局命名空间，
  任何同时 `using namespace lrc` 的 TU 包含它后无限定名 `ColorManager` 会解析到全局不完整类型
  （实测让 `lrc_qt_tests.cpp` 既有代码编译失败）。现移入 `namespace lrc`。

**测试登记**：`tests/TestSoftProofing.cpp` 新增 3 用例（内存 ICC 加载并出图、三种内置预设逐一打样、
空字节清空与垃圾字节安全失败）；`tests/lrc_qt_tests.cpp` 新增 `ColorRangeSamplerTest`（按钮 emit /
两种镜像静默）、`SoftProofingDialogSmokeTest`（paramsChanged 契约 + 启用状态）、
`DuplicateResultsDialogSmokeTest`（无目录渲染分组）；`tests/CMakeLists.txt` 纳入
`SoftProofingDialog.cpp` / `DuplicateResultsDialog.cpp`；`DevelopPanel` 三个取样按钮补稳定 objectName。

**验证（Release，本机）**：GUI 全量构建通过（`SilverHalide.exe` + `lrc_qt_tests.exe`）；
`lrc_qt_tests.exe` 9 个测试类全绿（含新增 3 类，Totals 全 0 failed）；GUI 启动冒烟
（启动后 10s 存活 → WM_CLOSE）EXIT=0；headless 全量 **469 用例 / 878717 断言，468 通过**，
唯一失败为既有 flaky `TestRawDecoder.cpp:556`（R-08 embedded-ICC：两次运行 Δ=0.0073 / 0.0121，
均低于阈值 0.02），与本次改动路径无关——**该缺陷已在上一小节定位并修复**。

## P-C：移除 Lightroom 兼容（2026-09-26，工作区未提交）

按 [ADR-0001](docs/adr/0001-drop-lightroom-compat.md) 与
[docs/spec/optimization/04-drop-lr-compat.md](docs/spec/optimization/04-drop-lr-compat.md)
落地 P-C 部分：彻底移除 Lightroom 的读/写兼容路径，产品数据契约以自有 `.lrp` 预设与 `.shcat` 目录自洽。

- **删除代码**：`src/engine/Xmp.h/.cpp`、`src/engine/XmpSidecar.h/.cpp`、
  `src/utils/XmpProfile.h/.cpp`、`tests/TestXmp*.cpp`、`docs/XMP-MAPPING.md`、
  `third_party/pugixml`（仅用于 XMP/crs）。
- **CMake**：`LRC_ENGINE_SOURCES` / `LRC_UTILS_SOURCES` 移除上述源文件；
  移除 `pugixml` 静态目标与链接。
- **UI 入口清理**：`MainWindow.cpp` 删除「读取 sidecar / 写入 sidecar」菜单与槽函数；
  「导入预设…」「导出预设…」仅保留本应用 `.lrp` 格式（移除 `.xmp` / crs 分支）。
- **测试迁移**：`tests/test_postcrop_vignette.cpp` 中原 XMP shape 字段往返用例改为
  `ProfileIO::toJson/fromJson` JSON 往返，保持断言覆盖。
- **文档更新**：`README.md`、`docs/ARCHITECTURE.md` 移除 XMP 与 Lightroom 兼容描述，
  明确数据契约自成体系。

**验证**：headless 全量构建 + ctest 全绿（469 用例 / 878718 断言，一次预存 flaky 独立重跑后通过）；
GUI 全量构建通过（`SilverHalide.exe`、`lrc_qt_tests.exe` 均生成），`run_qt_tests.bat` 退出码 0。

## 导入静默失败 / 视图跳变 / AI 模型无提示 三连修（2026-09-25，工作区未提交）

用户报障三例非致命运行时问题，定位结论：均为**逻辑性错误 + 失败路径静默**，无结构性病灶，未重构。
修复 + 回归测试全部落地，`lrc_qt_tests.exe` 全绿，GUI 全量构建通过。

- **症状 1 导入 RAW 不显示（根因修复）**：`ImportWorker::importPaths` Stage 1 的
  `QtConcurrent` 任务守卫 `if (abort_.load() || !f.ok)` 中 `f.ok` 初值为 `false`，
  条件恒真 → **每次导入全部文件被静默跳过**（`ok=false` + 空 warn 在 Stage 3 按
  "跳过不报告"消费），表现为侧栏出现文件夹、状态栏无成功无报错、点文件夹无反应。
  引入提交 `461300d`（2026-08-31 三段式导入重构），此后所有导入均为静默 0 张。
  修复：守卫只判 `abort_`。
- **症状 1 伴随的静默失败防线**（防再犯，非根因）：`abort_` 改为一次性、
  `importPaths` 入口复位（残留 abort 不再吞掉下一次导入）；三段式主体包异常防火墙
  （引擎异常 → 回滚 + `importWarning` + `finishedImport(0)`，不再挂死 "正在导入…"）；
  `MainWindow::startImport` 检查 `invokeMethod` 返回值，调度失败状态栏报错。
- **症状 3 蒙版绘图手感（视图跳变修复）**：最终帧路径 `onPreviewReady` 此前对任何
  预览帧都走 `viewport_->setImage()`（内部 `resetView()`），蒙版拖动中每个几何提交
  都触发最终渲染 → 视图在手势中途缩放归 1/平移归零。修复：新增
  `lastPreviewImageId_` 分流——新图首帧仍重置视图（原行为），同图重渲染只换像素
  不碰缩放/平移；两处清空视口路径（删图、空列表）同步重置该标记。
  线性渐变"没有实时渲染蒙版范围"的一部分即视图重置把 overlay 锚点打掉；可视化
  增强（线性渐变覆盖带、径向羽化过程显示）待实机体验后再定。
- **症状 2 AI 功能全部无响应（有声化）**：模型本体不随安装包分发、需首次下载，
  缺失时引擎全线静默回退（降噪 no-op / 蒙版启发式近似 / 预分级确定性显著性）。
  `DevelopPanel` 新增 `aiModelUnavailable` 信号 + 使用点探测：AI 降噪启用与
  AI 蒙版四按钮（主体/天空/背景/点选）在模型缺失时状态栏可执行提示
  （指路「设置 → 模型管理」），蒙版仍创建但明示"近似结果"。引擎层守卫
  （`hasModel`/`runSamPoint`）静态核查完整；"调用时直接崩溃"未复现，待模型
  下载后端到端验证。
- **测试**：`tests/lrc_qt_tests.cpp` 新增三个测试类 6 用例——
  `ImportWorkerTest`（残留 abort 不吞导入 + 导入落库含双预览）、
  `ViewportViewStabilityTest`（同图刷新保缩放 / 新图重置）、
  `AiModelAvailabilityTest`（无引擎时点 AI 按钮/降噪的提示内容与兜底行为）；
  `tests/CMakeLists.txt` 相应纳入 `ImportWorker/ImageViewport/DevelopPanel`
  等 6 个 UI 源并链接 `Qt6::Concurrent`。仓库根新增 `run_qt_tests.bat`
  （本机 System32 缺 UCRT 转发 DLL，测试 exe 需 vcvars + Qt bin 环境，脚本已封装）。

## P-A 性能极致 · 引擎层 headless 量化验收（2026-09-25，工作区未提交）

**本轮内容**：P-A（M1 1:1 零等待 / M2 编辑手感 / M3 内存极致 / M4 大目录）原有底层能力已基本落地
（1:1 磁盘直读、coarse-then-refine、全尺寸单缓冲+回落、worker 线程直方图、256MB LRU），缺的
是**可二值验证的量化验收**。本次新增引擎层 headless 验收，产出可复现实测记录；真机锚点登记待真机。

- **新增 [tests/TestPerfAnchors.cpp](file:///d:/Desktop/SilverHalide-src/tests/TestPerfAnchors.cpp)**（4 用例 `[performance][pa]`，D-1/D-2 另带 `[memory]`）：合成 8000×6000 F32 缓冲
  推过引擎渲染路径 + Win32 `PagefileUsage` 私有提交采样；[tests/CMakeLists.txt](file:///d:/Desktop/SilverHalide-src/tests/CMakeLists.txt) 注册源文件并在 WIN32 下链 `psapi`。
- **实测（Release，本机沙箱）**：
  - B-2 1920 精帧（`renderPreview` 1920×1440，代表性主滑块×3 取稳态）：**min ≈99ms / avg ≈100ms**（引擎段）。
  - B-1 全尺寸渲染引擎段（`renderFullDisplay`，线性→U8，不含解码/磁盘直读）：**≈514ms**。
  - D-2 切图回落：作用域释放后 **回落 ≈688MB，两轮 688.0/688.0MB，可重复 ✓**（私有提交口径）。
  - D-1 单图峰值：headless 无 UI 编辑流程不可复现，**标真机待验**（累计进程 working-set 峰值 ~1319MB 含历史用例残留，非单图口径）。
- **登记**：pillars/01-pa 的 [M1](file:///d:/Desktop/SilverHalide-src/docs/spec/optimization/pillars/01-pa/M1-zero-wait.md) /
  [M2](file:///d:/Desktop/SilverHalide-src/docs/spec/optimization/pillars/01-pa/M2-edit-feel.md) /
  [M3](file:///d:/Desktop/SilverHalide-src/docs/spec/optimization/pillars/01-pa/M3-memory.md) 各补「验收状态」实测段。
- **真机待验**：B-1 热≤175ms/冷≤1500ms、B-2 松手→精帧≤80ms、B-3 主线程≤16ms/帧、D-1 单图峰值≤1.2GB —— 均需独立 GPU + 真实 50MP RAW，待 GPU 环境实测。
- **验证**：headless 全量 **482 用例转绿**（含新增 4 条 PA 用例）；仅有已知 TestRawDecoder R-08 embedded-ICC 运行间非确定（单独跑 `[raw][io]` 全过）。

## P3 竞品对齐优化·体验打磨（2026-09-21，工作区未提交）

竞品规格 [competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md) 第 4 节 P3 四项全部落地并 headless 验收；P1/P2/P3/T 已全部实现，spec 内对应验收标准已勾选。四项代码均在工作区，未 commit（按用户要求先不提交/推送）。

- **P3-1 直方图线性化**：`Pipeline::computeHistogramFromLinear()`（全遍历无抽样；F32/U16 经 sRGB OETF LUT 编码，U8 直接回退）+ `renderPreview` 可选 `linearDisplayOut` 交出调整后线性 F32 显示缓冲；`PreviewWorker` 新增 `histogramReady` 信号在 **worker 线程** 统计（主线程零开销）；`HistogramWidget` 8bit 回退去掉 4M 抽样。`TestHistogram.cpp` 8 用例 / 3090 断言，含 4096×2048 大图全遍历（旧抽样只计一半）。
- **P3-2 事件驱动文件夹监视**：`FolderMonitor` 新增 `ReadDirectoryChangesW`（异步 OVERLAPPED）事件线程 + 合并唤醒，差异/去抖复用 `poll()`（两路径语义一致）；`MainWindow` 优先事件驱动、失败回退 2s QTimer 轮询。`TestFolderWatcher.cpp` 7 用例（轮询回退 + 秒级事件驱动）。**本次修复两处健壮性 bug**：事件线程异常退出时 wake 一次以恢复轮询回退；D3D11 blur 负尺寸先检查再 resize。
- **P3-3 编辑预设管理**：`PresetLibrary`（Qt-free，`%APPDATA%/Silver Halide/presets`）保存/列表/加载/删除/导出，复用 ProfileIO 原子写；`PresetDialog`（命名保存/应用/删除/导出分享）+ 文件菜单「管理预设…」。`TestPresetLibrary.cpp` 5 用例 + `lrc_qt_tests` PresetDialogSmokeTest。
- **P3-4 GPU 像素管线后端抽象**：`SpatialFilterBackend`（CPU/GPU 双后端）+ D3D11 DirectCompute 后端（内嵌 HLSL，`LRC_ENABLE_D3D` 默认 OFF）+ SEH 保护的设备/微计算探测，GPU 不可用自动回退 CPU；texture/sharpness 两路径改走后端。`TestSpatialBackend.cpp` 13 用例，CPU vs 参考分块 Δ<0.008。GPU 提速数据留待真机（RTX A4000）验收——沙箱无可用 GPU。

**验证**：headless 构建通过；P3 相关组（`[spatial][histogram][presetlib][p3-2]`）全绿。全量 headless 仅两处既有失败、均非 P3 引入：
- `TestAiInference.cpp:149` 空引擎 SIGSEGV（0xC0000005）——Debug 构建输出目录未部署 ORT 1.23 DLL，按 DLL 搜索顺序加载了系统 `C:\Windows\System32\onnxruntime.dll`（ORT 1.17.1，报 API version 23 unavailable）。Release 目录有 1.23 故不崩。**环境问题，非代码 bug**（spec 此前归因"沙箱 D3D12"不准确，已修正认知）。
- `TestRawDecoder.cpp:556` R-08 embedded-ICC 断言——Debug 下真实 Δ（~0.001–0.019）与阈值 0.02 过近，运行间非确定；单独跑 `[raw][io]` 全过。Release 全量通过。属测试阈值质量问题，非功能 bug。

## MobileSAM 点击式蒙版（2026-09-21，已提交）

- **P2-3（AI 蒙版扩展）点击交互全链路落地并提交**：MobileSAM 两阶段分割（`AiInference::runSamPoint` — encoder 生成 embedding + decoder 按点/noIou 输出）接入 `AiMaskGenerator` 新增 `AiMaskTarget::Prompt`；`Adjustments::AiMaskParams` 加 `std::vector<SamPoint> points`，`Mask::hash/geometryHash` 覆盖（缓存键随点变化）。UI 层 `ImageViewport` 点击加点（前景绿/背景红 + 十字标记）、`DevelopPanel`「AI 点选」按钮 + 前景/背景切换 + 清除点 + 羽化滑杆、`MainWindow` 撤销接线。
- **布局修正（实测真实模型）**：`mobilesam-encoder` 输入为 **NHWC `[H,W,3]`**（非 manifest 原声明的 NCHW），已恢复引擎 NHWC 布局并同步修正 manifest/测试声明；decoder 为单掩码候选，iou/mask 候选轴按末维/倒数第三维稳健取用。
- **验证**：headless `[ai]` 35 用例 / 284 断言（含真实模型 runSamPoint + Prompt 端到端）、全量 **454 用例 / 752,822 断言** 全绿。
- **R-08 预存失败修复**：测试用纯绿断言 G 通道（sRGB/AdobeRGB 红原色相同、绿原色才不同），区分 embedded ICC 应用，全量转绿后并入提交。
- Commit `0ac13bb` + `4520933` 已推送 `SilverHalide/main`。

## 当前任务状态（2026-09-12）

- **当前进行中的任务**：[竞品对齐优化实施规格](docs/spec/competitive-optimization-implementation-spec.md) +
  [代码业务可用性审查](docs/archive/code-business-risk-review-2026-09-06.md)（R-01 ~ R-16）+
  [P-C Lightroom 兼容移除](docs/spec/optimization/04-drop-lr-compat.md)（本批次已落地，待构建验证）。
- **总体状态**：竞品规格 P1/P2/P3/T 均已实现（spec 内对应验收标准已勾选，含 2026-09-19 起的 P1-3 端到端与 Qt 真实导出验收、P3-4 GPU 后端）；**P-C（Lightroom 兼容移除）已按 ADR-0001 落地代码、测试与文档**。GPU 真机量化（P1-1 DirectML ≥2x、P3-4 提速）与部分 P1 数值矩阵断言待补（见 spec 各批次验收标准注释）。风险审查项已逐条修复，详见下。
- **风险审查修复进展（工作区未提交）**：R-01（DCP 注入解码/渲染）~ R-16 均已在引擎/目录/UI 层落地并登记回归测试；R-10（标准预览缓存色彩版本）、R-11（内存导入保持 ICC）、R-16（删除清理外部缓存）为最近一批，2026-09-12 验证通过。
- **验证（2026-09-12，headless）**：当前构建树 `cmake --build build_headless -j` 通过；`ctest --test-dir build_headless` 全绿，**444 用例 / 752,709 断言** 全部通过。期间修复一处 JPEG APP2 ICC 段长度 off-by-2 的写入错误（`ImageIO::jpegIccMarkers`），否则内嵌 ICC 无法被 `extractIccFromJpeg` 恢复，会同时影响 R-08/R-10 路径。
- **P1-3 批量重命名（2026-09-19 端到端已验收）**：`src/utils/RenameTemplate.h` 实现模板渲染（`{date}/{index[:W]}/{base}`）与冲突唯一路径；导入端 `ImportDialog` 加重命名复选+模板+新文件名预览，`ImportWorker::importPaths` 按选择顺序+源文件修改日期在 Stage1 重命名（`renameMutex_` 串行化查重，失败回退）；导出端 `ExportDialog` 加批量重命名复选+模板+母版/副本预览，启用时 `MainWindow` 走 `PreviewWorker::batchExport` 为母版+所有虚拟副本按模板重命名输出。headless 端到端集成用例（批处理后文件名唯一、DB `file_path` 镜像一致）+ Qt `batchExport` 真实导出断言已验收；GUI 手动验收未做。
- **DCP 相机配置文件（工作区未提交，风险项 R-01 已修复）**：DevelopPanel 相机配置文件选择框选择后，DCP 的 ColorMatrix1/2 与 ProfileToneCurve 在解码阶段注入并进入渲染管线（R-01），不再只是读取文件、未应用到色彩计算。
- **Lensfun（工作区未提交）**：已声明 `lensfun:x64-windows`、接入 Qt-free 查询 adapter 和 RAW 镜头元数据；UI 仅声明几何畸变支持（移除未实际处理的暗角/色差承诺，R-05）；vcpkg 安装仍受历史中断影响，数据库查询验证待完成。
- **历史性能资料**：M1-M5 实施规格和四阶段加速报告位于 `docs/archive/`，仅作回溯依据，不作为当前进行中任务清单。

## Catalog 持久化层重构与无模型打包（2026-09-14，工作区未提交）

本轮对 `src/catalog` 持久化层做了整体升级，并完成 UI「目录即库」装配与无模型安装包。涉及的新增/修改类如下。

**引擎/目录层（Qt-free）**：

- `CatalogDatabase`（.h/.cpp，改动最大）：引入**版本化迁移**框架 `applyMigrations/migrateSqlVersion/migrateV1-4/migrateEnsureColumn/migrateEnsureColumnOn`，以 `PRAGMA user_version` + `schema_migrations` 表注册版本，每次迁移在事务内执行、失败整体回滚并关库（不再部分应用）。新增/改造：
  - **预览外置**：previews 由 `data BLOB` 改为磁盘文件 + `path` 列；`previewPathFor/    previewFilePath/storePreviewImpl/finishPreviewExport` 写入 `<previewDir>/<id低3位hex>/<id>_<kind>.shcat`，迁移按可续跑方式把存量 BLOB 导成文件后删行瘦库。
  - **文件指纹**：images 增 `file_size/file_mtime` 列 + `setFingerprint/detectModifiedImages/addImage(fileSize,fileMtime)`，支撑外部修改检测与导入幂等。
  - **清单完整性**：统一 `applyPragmas`（foreign_keys=ON、WAL、synchronous=NORMAL、busy_timeout=5000）；表转 STRICT + CHECK；file_path 唯一索引改大小写不敏感；补 `collection_images`/`panorama_sources` 反向索引。
  - **单连接并发**：移除双连接 import 系列，改单连接 + busy_timeout 队列。
  - **检索能力**：FTS5 外置内容 + `images_search`（COALESCE 处理 NULL 关键字）；phash 分段 `phash_seg` 表粗筛 `scanPhashCandidates`；接入 sqlite-vec `vec0` 64 维 `images_vec`，`annByEmbedding/rebuildSearchIndexes` 支持降级。
  - **生命周期**：edit_history 增 `kind` 列（auto/snapshot），`enableAutoSnapshotRetention` 每图自动历史保留 20 条。

- `CatalogDatabase` 公开 API 补充注释（Doxygen `@brief/@param/@return`），引擎/目录层保持 Qt-free。

**UI/装配层**：

- `src/app/Constants.h`：`kDefaultCatalogFile` → `Default.shcat`；新增 `kPreviewSideDir=previews.shcat`、`kCatalogDbSuffix=.shcat`。
- `src/app/Application.{h,cpp}`：`defaultCatalogPath()` 改返回**目录**（`AppData/…/Default`，目录即库），保留 legacy `.lrcat`/`.shcat` 单文件迁移（旧文件存在且新目录未建时返回旧文件）；新增 `catalogDbFile()`（目录→`<dir>/<stem>.shcat`，文件→原样）、`catalogPreviewDir()`（目录→`<dir>/previews.shcat`）。
- `src/ui/MainWindow.cpp`：CatalogDatabase 构造传入 dbFile+previewDir；`newCatalog` 选目录、`openCatalog` 接受 `.shcat` 目录与 legacy `.lrcat`；libraryRoot 目录在目录内、legacy 文件在文件旁。移除用户可见 lr 名称：备份 `backup.lrcat`→`.shcat`；预设对话框仅保留本应用 `.lrp` 预设（Lightroom / XMP 兼容路径已移除）。
- `src/catalog/CatalogDatabase.cpp` `open()`：新建目录前先 `create_directories(parent)`，目录不存在时自动创建而非报错。
- `src/ui/ImportWorker.cpp`：改单连接批量事务（`addImage` 带指纹 / `storePreview` / `beginTransaction`）；修复 `fileFingerprint` chrono 模板 `duration` 类型错误（否则 GUI 无法编译）。

**测试**：[TestCatalogDatabase.cpp](file:///d:/Desktop/SilverHalide-src/tests/TestCatalogDatabase.cpp) 新增 3 用例——文件指纹外改检测、重复 phash + 64 维 embedding ANN 索引（goal 6）、自动编辑历史保留 20 条且命名快照不受限。

**打包**：新增 [packaging/installer-nomodels.iss](file:///d:/Desktop/SilverHalide-src/packaging/installer-nomodels.iss)（无模型 Inno Setup 变体：**链接并打包 onnxruntime.dll，仅不含 models/\* 模型权重文件**；保留 lensfun.dll+glib 链与全部 DCP profiles）。补齐 [install_stage](file:///d:/Desktop/SilverHalide-src/build_gui/install_stage) 的 lensfun 运行库链（`lensfun.dll`+`glib-2.0-0.dll`+`iconv-2.dll`+`intl-8.dll`+`pcre2-8.dll`），顺带解决「找不到 lensfun.dll」问题；产物 `dist/Silver Halide Setup 1.0.0 (No Models).exe`（实测静默安装：无 models、有 onnxruntime.dll/lensfun/DCP）。

**验证**：GUI 编译通过；headless catalog **62 用例 / 2106 断言** 全绿。全量 headless 仅为 `TestRawDecoder.cc` off-by-2 ICC 前置问题及既有 pipeline/颜色路径失败（与本次无关）。

***

# 内部开发历史（2.x dev milestones，仅作历史记录）

以下章节为开发期里程碑记录，版本号不代表对外发布版本。保留这些记录以便后续维护时
追溯功能落地时序与决策上下文。

## v2.12.0 (2026-08-18) M5–M10：污点去除 / 虚拟副本 / Before-After / 关键字元数据 / 智能收藏夹 / 全景合成

**范围**：`docs/PHASE_5` – `docs/PHASE_10` 六个功能里程碑全部落地。实施方式：**多子代理并行三波**（Wave1 引擎/目录/XMP 深层实现 5 路并行 + Wave2 UI 层 7 路并行 + Wave3 MainWindow 集成），主代理统一编译验收。文件域互不相交（一文件一写者）。

- **M5 污点去除**（PHASE\_5）：`Adjustments::SpotRemoval`（Clone/Heal，归一化坐标）+ Pipeline 空间阶段后渲染（双线性采样、Heal 边缘亮度匹配 + 羽化）+ ProfileIO JSON 往返 + XMP `crs:SpotCorrections` 往返；DevelopPanel 污点面板（模式/半径/可视化）+ ImageViewport 交互（十字准星、Alt 设源、Delete/右键删除、1:1 坐标映射）+ 工具栏激活。

- **M6 虚拟副本**（PHASE\_6）：CatalogDatabase `edits`/`edit_history` 复合主键 `(image_id, variant_id)` 迁移（旧库重建为 variant 0 母版）+ `variants` 表与方法；Filmstrip 折角序号角标（母版像素级不变）+ Navigator 变体分组 + ExportDialog 副本选择 + 「创建虚拟副本」菜单。

- **M7 Before/After 与参考视图**（PHASE\_7）：PreviewWorker 成对双渲染 `requestBeforeAfter`（默认参数 before + 当前 after，同代不互斥、相同堆叠只渲染一次）+ `requestReference`；ImageViewport 五种比较模式（Off/左右/上下/单图/参考，可拖拽分割线、参考锁定同步缩放）+ 工具栏互斥组 + `\` 快捷键。

- **M8 关键字与元数据**（PHASE\_8）：CatalogDatabase `keywords`（自引用层级）/`photo_keywords` + `images` 元数据列（title/caption/copyright/creator/date\_taken/camera/lens）；XMP `dc:subject`/`lr:hierarchicalSubject`/`dc:title/description/rights/creator` 往返；DevelopPanel 元数据面板（标题/说明/版权/创作者 + 关键字 chip）+ 工具栏搜索框 + Navigator 关键字分组聚合计数。

- **M9 智能收藏夹**（PHASE\_9）：`smart_collections` + `smart_collection_photos` + 独立谓词求值器 `evaluatePredicate`（rating/label/flag/keyword/date/file\_type/has\_adjustments 等字段 × ==/!=/\</=/>/>=/contains/starts\_with/in/between）+ `refreshSmartCollection` 事务重算；Navigator 智能收藏夹子分组 + `SmartCollectionsDialog` 规则构建器（字段/运算符/值编辑器 + ALL/ANY）。

- **M10 全景合成**（PHASE\_10）：纯 C++ `PanoramaStitcher`（无 OpenCV：降采样空间域 NCC 平移对齐 + 细网格 + 小偏移平局裁决、柱面投影查表、行带羽化融合、autoCrop）+ `PanoramaDialog`/`PanoramaWorker`（QThread + 原子取消 + 进度）+ 「全景合成…」菜单（内联多选源图 → 写 DNG → `addImage` 入目录 → `recordPanoramaSources`）。

**验证**：headless **353 用例 / 745,722 断言**、GUI **354 用例 / 745,882 断言** 全绿，GUI 冒烟 `SMOKE_EXIT=0`。

**关键决策/坑**：

- **sqlite 语句复用**：本工程 sqlite amalgamation 在 `SQLITE_DONE` 后直接 re-bind 返回 `SQLITE_MISUSE`（无自动 reset）——`Stmt::step()` 改为「返回 DONE/ERROR 后立即 `sqlite3_reset`」，多行 INSERT 循环才能每行落库。

- **GBK 编码**：headless 无 `/utf-8`，引擎/目录文件的中文串字面量奇数字节会吃闭合引号——CatalogDatabase 标签名改 UTF-8 hex 转义（`"\xE7\xBA\xA2"` 等）；UI 文件因 Qt AUTOMOC 带 `-utf-8` 无碍。

- **Qt 6.8 元类型**：`Q_DECLARE_METATYPE` 在 Q\_OBJECT 头内与 `qmetatype.h` 冲突——全景信号改为不传 `PanoramaOptions`（getter 暴露），彻底规避。

- **全景对齐**：粗网格步长漏精确偏移 + 平滑图 y 向 NCC 歧义 → 细网格 `s1=max(1,maxDim/64)` + 近平局小偏移优先。

- **已知局限**：全景合成无标准预览时在 UI 线程解码（旧目录会短暂卡顿）；EditHistoryDialog 仅显示母版历史；智能收藏夹刷新为阻塞式全表（超大库建议后台化）。

**手动验收清单**：污点工具画/拖/删 + Ctrl+Z 单步撤销；创建虚拟副本独立编辑并导出；`\` 前后对比分屏；参考视图；搜索框/关键字/智能收藏夹过滤互斥；元数据编辑后 Navigator 计数刷新；多选 2+ 张全景合成出 DNG 并选中。

## v2.11.0 (2026-08-12) 默认目录机制 + 目录内嵌预览

**默认目录机制**：命名「Default」默认目录（`Default.lrcat`），未新建/打开目录时导入进 Default；
窗口标题显示当前目录文件名；「目录」菜单新增「切换到默认目录」；旧 `catalog.lrcat` 自动迁移.

**目录内嵌预览**：新增 `previews` 表（`image_id, kind, data BLOB, width, height`，复合主键 +
`ON DELETE CASCADE`）；导入时经 `renderThumbJpeg`/`renderFullResJpeg` 渲染缩略图（320px JPEG）+
1:1 全分辨率 JPEG 并 `storePreview` 写入目录文件；PreviewWorker 1:1 默认调整下从目录 BLOB 直读；
Filmstrip/MainWindow 从 BLOB 解码缩略图；外部磁盘缓存降级为旧目录回退.

**版本号**：`Constants.h` 已升至 2.11.0.

**测试登记**：`TestCatalogDatabase`（previews 往返/upsert/cascade）、`TestThumbnailCache`
（renderThumbJpeg/renderFullResJpeg 内存 JPEG 尺寸往返）.

**2026-08-14 验证确认**：全量 **245 用例 / 274234 断言** 全绿（GUI 与 headless 双构建一致）。

**2026-08-14 AUDIT 修复验收（8 严重 + 20 中等全部落地）**：7 路审计报告 `docs/archive/AUDIT-2026-08-14.md` 的 8S+20M 经并行子代理修复完毕，Phase 2 集成 MainWindow（S1 关窗延迟关闭/循环等待/阻塞兜底、M10 备份/优化移 std::thread、M11 画笔 500ms debounce、M12 删除后选相邻图、PreviewWorker coarse 接线）+ `tests/TestImageIO.cpp` 登记 + `addImageImpl` UPDATE 升级路径漏绑 `?1` 回归修复。**验收**：headless 与 GUI 双端 **272 用例 / 274373 断言全绿、EXIT=0**；GUI 退出冒烟（启动后 WM\_CLOSE）EXIT=0 不崩溃，S1 退出修复验证通过。GUI 构建期间修复 1 处树独有编译错误（`FilmstripWidget.cpp:242` Qt 6.8 无单参 `QIcon` 构造，改 `QIcon(),QString()`）。
此前 headless `Release/lrc_tests.exe` 为 08-12 旧二进制（当时 `TestCatalogDatabase` 查询计划
用例尚未修复），其 2 处失败（backup / query-plan）经 `cmake --build build_headless --config
Release --target lrc_tests` 重建为最新源码后消失。目标「默认目录机制 + 目录内嵌预览」验证闭环。

**2026-08-13 收尾同步**：修复 `TestCatalogDatabase` 查询计划用例重复拼接 `EXPLAIN QUERY PLAN`
前缀导致全量 ctest 中止的 bug；版本统一为 2.11.0（Constants.h / CMakeLists / app.rc /
installer.nsi / README / CLAUDE）；headless + Ninja GUI + VS Release 构建通过；重建安装包
`dist/Silver Halide Setup 2.11.0.exe`。

**2026-08-13 更名 Silver Halide**：产品脱离 LRC 重制。`kAppName` → "Silver Halide"、
`kAppOrganization` → "ASE-128"（AppData 路径变为 `%APPDATA%/ASE-128/Silver Halide`）；
`defaultCatalogPath()` 增加旧身份（`%APPDATA%/LRC/LRC Clone Flash`）的
Default.lrcat / catalog.lrcat 回退迁移，旧照片不孤立；exe 输出更名 `SilverHalide.exe`
（CMake 目标 `lrc_lightroom` 保留，OUTPUT\_NAME 覆盖）；About/窗口标题/文件对话框同步；
安装包 → `dist/Silver Halide Setup 2.11.0.exe`；README/CLAUDE/AGENTS/ARCHITECTURE 同步；
旧 v2.3.1 及更早历史章节保留原名。测试按用户要求未运行。

**2026-08-13 修复导入文件筛选器**：`ImportDialog::browse()` 的筛选器把扩展名拼成
`.arw .dng …`（缺 `*` 通配符），Qt 按字面模式匹配导致「图像」筛选下看不到文件；
改为 `*.arw *.dng …` 标准通配符模式。其余对话框（导出/打开目录/导入预设）本就正确。

## v2.10.0 (2026-08-12) 缓存机制优化：导入预生成缩略图 + 1:1 缓存

导入图片时同步预生成缩略图 + 1:1 全分辨率缓存到磁盘，路径记入目录，进入 1:1 视图免重复全分辨率解码：

- **导入期预生成缩略图 + 1:1 缓存（性能）**：`ImportWorker` 导入时一并生成缩略图与 1:1 全分辨率
  缓存到磁盘（`ThumbnailCache::ensureFullRes`/`findFullRes`/`writeFullRes`，默认调整参数下以
  1:1 显示 sRGB JPEG 写入 `<cacheDir>/thumbnails/fullres/`），路径经
  `CatalogDatabase::setFullResPath` 记入目录（`fullres_path` 列）.

- **1:1 免重复解码直读磁盘（性能）**：`PreviewWorker` 进入 1:1 视图且当前为默认调整时，直接从
  磁盘缓存加载 1:1 JPEG，无需重复全分辨率解码，大幅提速；非默认调整时惰性解码，保证正确性.

- **测试登记**：新增 `tests/TestThumbnailCache.cpp`，覆盖 1:1 缓存 ensureFullRes 构建/幂等、
  findFullRes 命中、writeFullRes 写回与 loadImage 尺寸往返.

## v2.9.0 (2026-08-12) UI 修复收官：8 严重 + 15 中等

依据 `docs/archive/UI-AUDIT.md`（5 路并行静态审查，去重后 8 严重 + 15 中等 + 38 轻微），7 路并行
子代理完成本轮 UI 修复，主代理构建验收通过。修复按数据安全 / 核心功能失效 / 稳定性分组：

- **键盘微调滑块即时提交（数据安全）**：QSlider 键盘操作（方向键 / PageUp / Home / End）此前
  只刷新预览、不触发 `adjustmentsCommitted` → 目录不保存、撤销栈不压栈，切图即丢。现键盘
  微调同样走提交（目录保存 + 撤销栈压栈），不再丢数据。

- **refreshFilmstrip 阻断 selectionChanged 重放（数据安全）**：列表 `clear()` 后重选当前图
  必重放 `selectionChanged → onImageSelected`，后者无条件 `undoStack_.clear()` + 强制重解码。
  现以 **QSignalBlocker** 挂住重放——撤销栈不再被清空、不再强制重解码。

- **恢复编辑历史只写 1 条快照（数据安全）**：`onRestore()` 内 `saveAdjustments()` 与对话框
  Accepted 分支 `commitAdjustments()` 各追加一条 → 每次恢复膨胀 2 条。现移除对话框内
  `saveAdjustments`，恢复只写 1 条。

- **胶片栏缩略图从磁盘加载（核心功能）**：`setImages` 改从 `Entry::thumbnailPath` 磁盘加载
  （替代空 `QIcon()` 占位块），`refreshIcon` 以原始图基底重画角标——占位块永不重载与角标
  残留一并修复。

- **「已拒绝」旗标筛选（核心功能）**：SQL 置 `includeRejected`，`flag=-1` 不再恒空；被拒
  照片在胶片栏可见/可清除。

- **滚轮放大方向判定（核心功能）**：snap-back 只作用于缩小方向；Fit 比例 ≥ \~1.0（小图/截图）
  时滚轮放大不再被立即弹回，且正常请求全尺寸。

- **1:1 换图立即请求全尺寸（核心功能）**：换图时复位全尺寸请求状态，新图不再停在下采样
  预览的模糊态。

- **退出竞态修复（稳定性）**：ImportWorker / PreviewWorker 加 `std::atomic` abort 标志，
  `stopWorkers` 先 `requestAbort` 再 `quit()` + `wait`，批量导入/长解码关窗不再访问已析构的
  `catalog_/pipeline_`（杜绝 use-after-free）。

- **蒙版提交与选择**：蒙版「反转」勾选与不透明度补齐提交路径（切图不再丢）；蒙版下拉
  「取消选择」不再被 QComboBox 重填自动回选第 0 项破坏；`syncRow` 越界 UI/引擎统一 clamp。

- **色轮与曲线拖拽**：色轮 600ms 提交定时器换图时停止，不再压多余撤销条目；曲线拖拽走
  coarse 节流（CurveEditor 新增 `dragStarted`/`dragEnded` 信号），1:1 下不再持续全分辨率重渲染。

- **事件与路径**：文件夹筛选用边界匹配（`C:/Photos` 不再误含 `C:/Photoshop/…`）；eventFilter
  焦点白名单补 **QSlider** 并检查 `activePopupWidget()`；导出默认目录改当前照片目录；导出/导入
  对话框空路径不再静默 accept；HDR 导出校验 `.dng` 后缀。

- **样式与其他**：dark.qss 补 **QPushButton:disabled** / **QLineEdit** / **QRadioButton** 深色规则；
  导航树重建后恢复选中态；ImportWorker 批处理补 SQLite 事务；PreviewWorker 旧图
  `loadFailed`/`imageMeta` 补代次守卫。

测试全绿：216 test cases / 273018 assertions（GUI + headless，Release）

## v2.7.0 (2026-08-12) M4 Phase2 集成收口：直观 UI

- **顶部 QToolBar**：新增 `buildToolbar()`（构造函数调用，`addToolBar(Qt::TopToolBarArea, …)`）。
  三个 `checkable` 的 QToolButton 组成**视图模式互斥组**（适合窗口 / 适合宽度 / 1:1，tooltip 注明
  快捷键 **Ctrl+0 / Ctrl+Alt+0 / Ctrl+1**）；其后为非 checkable 的**放大 / 缩小**按钮
  （直接 `toolBar->addAction(text, viewport_, &ImageViewport::zoomIn/zoomOut)`）；分隔符后是
  **蒙版工具互斥组**（线性 / 径向 / 画笔）。视图互斥组与蒙版互斥组为**两个独立** **`QButtonGroup`**
  （各自 `setExclusive(true)`），互不干扰。

- **双向同步**：`ImageViewport::zoomModeChanged` → `MainWindow::onZoomModeChanged`
  （`QSignalBlocker` 挂住视图按钮组，把对应按钮 `setChecked(true)`，初始按 `zoomMode()` 同步一次）；
  `DevelopPanel::maskToolActivated` → `MainWindow::onMaskToolActivated`
  （None 时先 `setExclusive(false)` 逐按钮清空再恢复 `setExclusive(true)`，全程 blocker）。
  面板为源、工具栏为镜像（单向，此槽内不再反向调用 `selectMaskTool`/`setZoomMode`）；工具栏点击走
  `setZoomMode`/`selectMaskTool`，其回发信号幂等同步勾选，无环。

- **dark.qss 补齐**：QToolBar / QToolButton(:checked) / QPushButton:checked / QComboBox /
  QCheckBox 深色样式，纯 CSS 无图片（Phase 1 已交付）。

- 版本号 **v2.7.0**（`src/app/Constants.h` `kAppVersion`）。

## v2.8.0 (2026-08-12) M5 Phase2 集成收口：预览更新速率 coarse-then-refine

- **coarse-then-refine 预览更新速率**：DevelopPanel 拖动滑块期间经 **30ms coarse 节流**
  emit `adjustmentsChangedCoarse`；MainWindow `onAdjustmentsChangedCoarse` 路由到
  `requestAdjustCoarse` → `PreviewWorker::adjustPreviewCoarse`（复用现有
  `latestGeneration_` 代次守卫，渲染 **maxDim=480** 低分辨率帧并 emit
  `previewReadyCoarse`），`onPreviewReadyCoarse` 用 `viewport_->setImageKeepView`
  即时换图（保锚点、不重置缩放）。松手/停顿后仍走原有 **50ms refine 停顿路径**
  （`adjustmentsChanged` → 1920 预览），先低清后高清，拖动反馈更快。

- **1:1 下的行为**：coarse 取代的是 1920 预览层（非全尺寸），因此 `fullResActive_`（1:1）时
  coarse 仍更新预览层、不被像 refine 那样抑制；coarse 路径**不触发**
  `requestAdjustFullRes()`——全尺寸刷新仍由 refine 路径在松手/停顿后统一触发。

- **测试登记**：TestPipeline 新增 **2 用例**——`render(…,480)` 尺寸断言 +
  `kCoarseMaxDim=480` 与 0（全尺寸）**缓存键独立**（Pipeline 缓存键含 maxDim）。

- 版本号 **v2.8.0**（`src/app/Constants.h` `kAppVersion`）。

## v2.6.0 (2026-08-12) M3 Phase2 集成收口：蒙版引擎级性能

- **几何哈希去重**：`Mask::geometryHash()` 只哈希 type + 几何（dab 序列 / 线性端点与 feather /
  径向半径与 radial invert）+ `invert`；拖动局部调整滑块（曝光/对比度/饱和度等）不改几何哈希 →
  权重场命中缓存，不再整场重建。

- **`Pipeline::brushWeightField`** **权重场缓存**：COW `shared_ptr<const vector<float>>`，缓存键 =
  `geometryHash + w + h`，\~128MB LRU；按 **O(∑dab 面积)** 光栅化（取代原 O(P×D) 逐像素），
  UI 线程与 worker 线程共用（COW 保证线程安全）。`applyLocalAdjustments` 已改从 field 读权重；
  `maskWeight` 保留（供测试参考）。

- **ImageViewport 叠加层**：`rebuildMaskOverlay` 用 `geometryHash` 去重（几何未变不重绘）+
  长边 ≤480px 降分辨率，拖动局部滑块时蒙版叠加不再全分辨率重建。

- **dab 合并**：仅当指针在 widget 空间移动 ≥4px（manhattan）**且**图像空间归一化间距 ≥0.6×radius
  才追加新 dab，大画笔自动并笔，减少冗余 dab。

- **DevelopPanel 软度滑块**：蒙版节新增「软度」滑块（0..10000 默认 5000）→
  `onMaskSoftnessChanged(int)` 即时 emit `maskSoftnessChanged(value/10000.0)`；
  `activateMaskTool`/`onMaskSelected` 在 Brush 分支与 `maskBrushSizeChanged` 同路径同步发射；
  MainWindow 接线 `maskSoftnessChanged → ImageViewport::setMaskBrushSoftness`
  （钳制 0..1，只影响后续新 dab）。

- **测试登记**：新增 `TestMaskRaster`（**6 用例**：brushWeightField 采样一致性/非 Brush 类型光栅化/
  缓存命中与失效/geometryHash 仅几何+invert/500-dab 冒烟（<2000ms）/applyLocalAdjustments 逐位一致）；
  全量预期 **211 用例**（基线 205 用例 + 6 用例，断言数以实际构建为准）。

- 版本号 **v2.6.0**（`src/app/Constants.h` `kAppVersion`）。

## v2.5.0 (2026-08-12) M2 集成收口：缩放/平移体验

- **光标锚定滚轮缩放**：滚轮以光标所在图像点为锚缩放（放大/缩小锚点不漂移），由新纯函数
  `ViewportMath::anchorPan`（`utils/ViewportMath.h`，逆解 target-rect 映射）计算缩放后的平移偏移。

- **平移**：空格 + 左键拖拽、中键拖拽、方向键平移（放大后移动视野）。

- **适合宽度视图（FitWidth）**：`ZoomMode` 新增 `FitWidth` 并与 Fit 归入 `isFitLike()` 统一走自动
  适配路径；视图菜单新增「适合宽度」（快捷键 **Ctrl+Alt+0**，与「适合窗口」Ctrl+0 区分）。

- **拖动进 1:1 请求全尺寸**：点击-拖拽进入 1:1 视图时请求全尺寸缓冲，退出 1:1 时释放。

- **Z 键记忆最后适合模式**：`toggleOneToOne()` 进入 1:1 前记住 `lastFitMode_`（Fit/FitWidth），
  从 1:1 返回上次适合模式（裸键 Z，替换原 OneToOne↔Fit 硬编码切换）。

- **测试登记**：新增 `TestViewportMath`（**9 用例 / 50 断言**，纯几何 anchorPan 覆盖中心锚定/
  恒等缩放/放大/缩小/边角/视图小于图像/非整数坐标/极小图像）；全量 **205 用例 / 243402 断言**
  （基线 196 用例 / 243352 断言 + 9 用例 / 50 断言）。

## v2.4.0 (2026-08-11) M1 集成收口：快捷键体系 + 撤销/重做 + 白平衡吸管

- **全局快捷键体系（M1）**：MainWindow 安装应用级 `eventFilter` 处理裸键，修饰键组合走
  `QShortcut`；spin box / line edit / combo 等文本输入控件在焦点白名单内不被劫持。

- **裸键**（无 Ctrl/Alt 修饰，可带 Shift）：`Z` 1:1/适合窗口切换、`Tab` 收起/展开左右侧栏、
  `F` 全屏、`G/D/E` 聚焦图库/修改/预览、`L` 灯光循环（正常 ↔ 仅预览）、`Home/End` 首张/末张、
  `[`/`]` 减小/增大蒙版画笔、`K` 画笔 / `M` 径向 / `Shift+M` 线性渐变蒙版工具、
  `O` 蒙版红色叠加开关、`W` 白平衡吸管开关、`+`/`=`/`-` 曝光 ±0.33 EV。

- **修饰键组合**（`QShortcut`）：`Ctrl+Z` 撤销、`Ctrl+Shift+Z` 重做、`Ctrl+←`/`Ctrl+→` 上一张/下一张、
  `Ctrl+Shift+R` 复位（走 `resetAll` → 一次提交）。

- **撤销/重做（UndoStack）**：`lrc::UndoStack<Adjustments>`（50 步上限）。所有用户编辑提交路径统一走
  `commitAdjustments`（滑块释放 / spin 编辑完成 / 自动调整 / 复位 / 编辑历史恢复 / 预设导入 /
  sidecar 读取 / 颜色取样提交），每次压入一步；`applyAdjustmentState` 用于撤销/重做回放（不压栈）。
  换图清栈并以加载的初始状态为栈底。

- **蒙版一笔一步**：笔画首次几何变更压入笔画前快照，`maskEditFinished` 压入笔画后状态，中间几何更新
  不单独入栈 —— 一次撤销恰好回到笔画前。

- **白平衡吸管（W）**：取样像素 sRGB→线性 ProPhoto，`Pipeline::estimateWhiteBalance` 估计色温/色调偏移
  并叠加到当前调整（钳制 ±100），取色后吸管保持激活可连续取；与点选吸管互斥。

- **测试登记**：新增 `TestUndoStack`（10 用例/65 断言）+ TestPipeline `estimateWhiteBalance` 4 用例；
  全量 **196 用例 / 243352 断言** 全绿（本机已实测构建验证）。

## v2.3.1 (2026-08-11) 打包发布

- **应用更名**：`kAppName` 由 "Lightroom Clone" → **"LRC Clone Flash"**；`app.rc` 新增
  VERSIONINFO（rc 编译加 `/utf-8`），exe 与安装包元数据写入产品名 **LRC Clone Flash**、
  公司 **ASE-128**、版权 **© ASE-128 2026**；About 对话框补版权行。

- **无控制台（GUI 子系统）**：`add_executable(lrc_lightroom WIN32 ...)` + 链接
  `Qt6::EntryPointPrivate`（qtmain 转发 WinMain→main），exe PE 子系统由 CONSOLE(3)→WINDOWS(2)，
  启动不再弹出黑色控制台窗口。

- **NSIS 安装包**：新增 `packaging/installer.nsi`（MUI2 中文界面，UTF-8 BOM）、
  `packaging/welcome.bmp`（由 `packaging/make_bmp.py` 生成，因 NSIS Contrib 缺默认图）。
  发布载荷暂存 `build_gui/install_stage/`（exe + Qt DLL + 插件 + VC++ 运行时，不含 .lib），
  编译产物 **`dist/LRC Clone Flash Setup 2.3.0.exe`**（约 17MB），安装到
  `Program Files\LRC Clone Flash`，带开始菜单/桌面快捷方式、卸载器与 Add/Remove Programs 注册。

- **清理**：移除项目根目录散落 .obj 与损坏的旧 zip（非合法 zip）、父目录临时日志/脚本。

- **文档**：README/STATUS/CLAUDE.md/ARCHITECTURE 更新为新应用名、无控制台与打包流程；
  `run_app.bat` 修正为指向 `build_gui/Release/lrc_lightroom.exe`。

## v2.3 (2026-08-11) 性能优化（子代理并行实施）

- **快速高斯模糊**：`gaussianBlur` 大 sigma（>4.0，如清晰度 ≈min(w,h)/50、蒙版局部清晰度、DetailMask 可视化）改走 **3 趟滑动窗口 box blur 逼近**（O(1)/像素），小 sigma（纹理 2.5 / 锐化 ≤4）保留原精确分离卷积逐位不变。实测 1920×1080、sigma=38：**1260ms → 14.4ms（\~87x）**，box 与精确逐像素平均绝对差 \~0.0015（视觉不可见）。全尺寸 6000×4000 清晰度由约数秒降到数十毫秒。

- **HSL / 点颜色去重**：`applyHSLReconstruct` 改为接收预计算 HSL，消除 `applyHSLPerChannel`/`applyColorTarget` 内双重 rgbToHSL；ColorTarget 参考色（refHue/refSat/refLuma）提升出逐像素循环，每 target 每像素省 1-2 次 HSL 转换。

- **中性参数快速路径**：`applyAdjustments` 全部参数中性时直接返回（恒等），跳过每像素 \~40% 死操作，初始预览打开更快。

- **`renderInternalLinear`** **消除 U16 中间拷贝**：无需 resize 且非 F32 输入直接 `linear.toF32()`，省一次全尺寸 U16 memcpy。

- **`toDisplaySRGB`** **消除冗余 F32 深拷贝**：输入已是 F32 时直接引用调用方缓冲（`cmsDoTransform` 只读），省 24.9MB/帧拷贝。

- **`applyMatrixU16`** **就地 + 并行**：相机矩阵应用不再分配第二份全尺寸 U16（24MP 省 \~144MB）且走 `parallelFor`，解码提速 \~30-40%。

- **`ImageBuffer::toF32()`** **并行化**：逐行转换改按行分块并行。

- **`toQImage`** **零拷贝**：QImage 经 cleanup 函数持有 `ImageBufferPtr` 直接引用 U8 缓冲，主线程省 6.2MB memcpy/帧（已核实 setImage/直方图路径全部只读）。

## v2.2 (2026-08-11)

- **XMP 兼容 / Lightroom 预设互导（M3）**：新增 pugixml 驱动的 XMP 解析器（`engine/Xmp`）、
  crs↔Adjustments 字段映射（`utils/XmpProfile`）与 sidecar 读写（`engine/XmpSidecar`），
  UI 侧「文件 → 导入/导出预设…」「照片 → 读取/写入 sidecar」四个动作已接线。

- **`.xmp`** **sidecar 读写**：sidecar 与图像同名（`img.arw` → `img.xmp`），写侧直接生成
  Lightroom 兼容的 XMP 文档，读侧解析并回填参数堆栈。

- **预设导入/导出**：`.xmp` 为 Lightroom 预设（XMP 描述 + crs 属性），`.lrp` 为本应用预设
  （ProfileIO JSON，字段无损完整往返）。

- **字段映射覆盖**：基本（曝光/对比/高光/阴影/白/黑/色温/色调/饱和）、细节（锐化量/半径/细节/蒙版）、
  效果（纹理/清晰度/去朦胧/鲜艳度）、颗粒（量/大小/粗糙度）、色调曲线（ToneCurvePV2012 点数组 →
  分区曲线）、HSL 24 项（8 通道 × 色相/饱和/明亮）、色调分离（高光/阴影/平衡）。

- **互导细节**：未知 crs 属性静默忽略（容错）；`Temperature` 以 mired 锚点 6500K 换算，
  `Tint` 按 ×1.5 换算；导出时数值取最短十进制形式。

- **v1 取舍**：蒙版 / 点颜色 / 畸变不互导 XMP（`.lrp` 完整往返）；RAW 内嵌 XMP 读取 deferred（返回 false）。

## v2.0 (2026-08-11)

- **标签颜色**：7 色标签（红/橙/黄/绿/青/蓝/紫）可设/清除，持久化到 `images.label_color`；
  Filmstrip 缩略图右下角色标角标 + 右键「色标」子菜单（点击已选色即清除）+ 快捷键
  （`6-9` → 色 1-4，`Ctrl+6-8` → 色 5-7，`Ctrl+Shift+0` → 清除）；Navigator 第 3 个筛选下拉
  按色标过滤（复用 `ImageFilter.labelColor`）。

- **收藏夹集合**：新增 `collections`/`collection_images` 两表（`ON DELETE CASCADE` 级联），
  创建/重命名/删除集合、成员增删（重复添加幂等）；Navigator「收藏夹」顶层分组点击过滤胶片栏；
  「照片 → 管理收藏夹…」打开 `CollectionsDialog`（CRUD + 添加/移除成员）；收藏归属在
  Filmstrip 缩略图左上角显示角标。

## v1.9 (2026-08-11)

- **受管库导入**：导入对话框提供「添加 / 复制到目录 / 移动」三种策略，配合「导入时转换为
  DNG」。复制/移动落地到按日期组织的受管库 `<library_root>/<yyyy>/<yyyy-mm-dd>/`，
  文件名冲突自动追加 `-2`、`-3` 后缀；移动为「复制后删除源文件」。无目标目录时复制/移动
  自动降级为添加，绝不丢文件（`LibraryManager`、`ImportWorker::importPaths`）。

- **「目录」菜单**：新建目录 / 打开目录 / 备份目录 / 优化目录。目录路径经 `QSettings`
  （键 `catalogPath`）持久化，新建/打开后自动重启应用切换目录；备份走 SQLite `backup`
  API（先 WAL checkpoint），优化走 `VACUUM`。

- **编辑历史快照**：每次 `saveAdjustments` 自动写入 `edit_history` 表；照片菜单「编辑历史…」
  打开 `EditHistoryDialog`，可查看每张照片的全部快照并恢复（恢复会写回 edits 并重新渲染预览）。

- **导航「目录图片库」分组**：`refreshFilmstrip` 扫描受管图片（`is_managed`），按库根前缀收集
  相对子文件夹（如 `2026/2026-08-11`）去重排序后展示；点击库子项按 `/` 拼接绝对路径过滤胶片栏。

- **`is_managed`** **标记**：`images` 表新增该列，复制/移动导入为 1，添加导入为 0；重新导入同一文件
  不会翻转既有标记（导入幂等）。

## 构建状态

- **生成器**: Visual Studio 17 2022 (x64)，`cmake --build build_gui --config Release`

- **输出**: `build_gui/Release/SilverHalide.exe`, `build_gui/tests/Release/lrc_tests.exe`

- **Qt 部署**: 构建后 `..\qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations .\build_gui\Release\SilverHalide.exe`（Qt 安装在项目上一级的 `qt/` 下；插件以扁平布局部署到 exe 目录，如 `platforms/qwindows.dll`）

## 测试结果（v2.10.0，2026-08-12）

v2.5.0 基线：`All tests passed (243402 assertions in 205 test cases)`。
v2.6.0 登记 `TestMaskRaster`（6 用例）后全量预期 **211 用例**；确切断言数以主代理构建后的
`ctest --test-dir build_gui --output-on-failure` 输出为准。
v2.8.0 登记 TestPipeline 2 用例（`render(…,480)` 尺寸断言 + 480/0 缓存键独立）后全量预期
**213 用例**；确切断言数以主代理构建后的 `ctest --test-dir build_gui --output-on-failure` 输出为准。
v2.9.0 本轮 UI 修复后全量 **216 用例 / 273018 断言** 全绿（GUI 与 headless 双构建一致）；
新增 3 用例均为追加到既有测试文件：TestCatalogDatabase 的 rejected 筛选、
TestViewportMath 的 wheelShouldSnapBack 与 imagePointInBounds。
v2.10.0 新增 TestThumbnailCache（1:1 缓存构建/幂等/写回）与 TestCatalogDatabase fullres 往返后全量 **218 用例 / 273043 断言** 全绿（GUI 与 headless 双构建一致）。

### 测试覆盖

| 测试文件                | 覆盖内容                                                                                                                            |
| ------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| TestAdjustments     | 默认值、复位、相等性、哈希、HSL、色调分离、蒙版                                                                                                       |
| TestPipeline        | 曝光/对比度/高光/阴影/白黑/色温/HSL/色调分离/清晰度/颗粒/蒙版局部/可视化叠加                                                                                   |
| TestImageBuffer     | U16/U8/F32 读写、裁剪、缩放、格式转换                                                                                                        |
| TestColorManager    | D65→ProPhoto 矩阵、相机矩阵、中性灰渲染、sRGB 往返                                                                                              |
| TestRawDecoder      | 最小 DNG 验证与元数据读取                                                                                                                 |
| TestCatalogDatabase | 打开、添加、编辑、事务、评分、文件夹、备份、设置、编辑历史快照                                                                                                 |
| TestLibraryManager  | 受管库目标路径、日期子目录、重名后缀、Copy/Move 落地、自库内文件不再复制                                                                                       |
| TestExportEngine    | JPEG/PNG/TIFF-8/16/DNG 导出、PPM、PNG 读取往返                                                                                          |
| TestJson            | 标量、对象、美化输出、转义、拒绝畸形输入、指数                                                                                                         |
| TestMathUtils       | clamp、stopsToGain、smoothstep、hashCombine、UTF-8 路径                                                                               |
| TestMasks           | 线性/径向/画笔蒙版权重、局部调整、**JSON 往返（含布尔 invert/enabled）**                                                                               |
| TestMaskRaster      | **蒙版引擎级性能**：brushWeightField 光栅化采样一致性、非 Brush 类型、缓存命中/失效、geometryHash 仅几何+invert、500-dab 冒烟（<2000ms）、applyLocalAdjustments 逐位一致 |
| TestThumbnailCache  | 1:1 全分辨率缓存：ensureFullRes 构建/幂等、findFullRes 命中、writeFullRes 写回、loadImage 尺寸往返                                                    |
| TestHdrMerge        | 曝光包围合成、EV 估计                                                                                                                    |
| TestProfileIO       | 预设 JSON 完整往返、version/未知键容错、savePreset/loadPreset、ColorTarget/Mask 哈希                                                            |
| TestUndoStack       | UndoStack 压栈/撤销/重做、redo 分支清空、limit 截断、clear、setLimit 动态生效                                                                       |
| TestViewportMath    | 光标锚定缩放平移的 anchorPan 纯函数：中心锚定、恒等/放大/缩小、边角、视图小于图像、非整数坐标、极小图像                                                                      |

## 管线调整顺序

```
曝光 → 对比度 → 高光 → 阴影 → 白色/黑色 → 色调曲线 → 白平衡 → 去朦胧 → 鲜艳度
→ HSL (各通道) → 色调分离 → 点颜色 (ColorTarget) → [空间阶段: 纹理/清晰度/锐化/畸变/颗粒/晕影/蒙版局部] → sRGB gamma
```

## 运行方式

```powershell
cmake --build build_gui --config Release
..\qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations .\build_gui\Release\SilverHalide.exe
.\build_gui\Release\SilverHalide.exe
```

## 已实现功能（对照 Lightroom Classic 固有功能）

### ✅ 已完成

| 功能                                                                                                                                                                                             | 状态              |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------- |
| 自动调整（曝光/对比度/鲜艳度建议）                                                                                                                                                                             | ✅ 引擎 + UI「自动」按钮 |
| 基本面板（色温/色调/曝光/对比/高光/阴影/白/黑）                                                                                                                                                                    | ✅               |
| 色调曲线（Catmull-Rom 拖拽）                                                                                                                                                                           | ✅               |
| 细节（锐化量/半径/细节/蒙版）+ Alt+边缘蒙版叠加                                                                                                                                                                   | ✅               |
| 颜色（HSL 8 通道 × 色相/饱和/明亮）+ 点选吸管                                                                                                                                                                  | ✅               |
| 色调分离（高光/阴影 + 平衡 + 可视化色轮）                                                                                                                                                                       | ✅               |
| 效果（纹理/清晰度/去朦胧/鲜艳度/颗粒）                                                                                                                                                                          | ✅               |
| 镜头校正（几何畸变 + 晕影，非配置文件依赖）                                                                                                                                                                        | ✅               |
| Alt+调整可视化（裁剪/细节/边缘蒙版叠加）                                                                                                                                                                        | ✅ 已接线           |
| **多格式导出（JPEG/PNG/TIFF-8/16/DNG 对话框）**                                                                                                                                                          | ✅ v1.4 新增       |
| **照片管理 UI（评分 1-5 / 旗标 P X U / 删除 / 筛选）**                                                                                                                                                       | ✅ v1.4 新增       |
| **HDR 合成菜单（选 2+ 张→合并→存 DNG→入目录）**                                                                                                                                                              | ✅ v1.4 新增       |
| PNG/TIFF 读取 + 多 RAW 格式（libraw）                                                                                                                                                                 | ✅               |
| DNG 读取/写出                                                                                                                                                                                      | ✅               |
| 目录管理（SQLite `.shcat` 目录即库、评分/旗标/标签/备份/VACUUM）                                                                                                                                              | ✅               |
| 蒙版引擎（线性/径向/画笔 + 局部调整）                                                                                                                                                                          | ✅ 引擎层           |
| **UI 蒙版编辑（线性/径向/画笔绘制 + 红色权重叠加 + 羽化/不透明度/反转 + 局部调整滑块）**                                                                                                                                         | ✅ v1.5 新增       |
| 点颜色引擎（ColorTarget 色相/饱和/明亮/范围/平滑/强度）                                                                                                                                                           | ✅ 引擎层，已序列化      |
| **点颜色 UI（取参考色吸管 + 多目标添加/删除 + 启用 + 色相/饱和/明亮/范围/平滑/强度滑块）**                                                                                                                                       | ✅ v1.6 新增       |
| **Sony 全系 RAW 导入（.arw/.sr2/.srf/.sr3）+ 导入对话框「导入时转换为 DNG」**                                                                                                                                     | ✅ v1.7 新增       |
| **导入所有导出格式（JPEG/PNG/TIFF + RAW/DNG）**：对话框/文件夹监视/预览/导出全链路，非 RAW 走 stb\_image + 内建 TIFF 读取器 → 线性 ProPhoto                                                                                        | ✅ v1.8 新增       |
| **LRC 式导入（添加/复制/移动 + 按日期受管库）**                                                                                                                                                                 | ✅ v1.9 新增       |
| **「目录」菜单（新建/打开/备份/优化，QSettings 持久化 + 重启切库）**                                                                                                                                                   | ✅ v1.9 新增       |
| **编辑历史快照（edit\_history 表 + EditHistoryDialog 恢复）**                                                                                                                                             | ✅ v1.9 新增       |
| **导航「目录图片库」分组（受管子文件夹浏览 + 过滤）**                                                                                                                                                                 | ✅ v1.9 新增       |
| **标签颜色（7 色可设/清除 + Filmstrip 角标 + 右键菜单/快捷键 + Navigator 色标筛选）**                                                                                                                                  | ✅ v2.0 新增       |
| **收藏夹集合（collections/collection\_images 表 + Navigator 分组过滤 + CollectionsDialog + 角标）**                                                                                                          | ✅ v2.0 新增       |
| ~~XMP sidecar 读写（.xmp 与图像同名 + Lightroom 兼容 XMP 文档生成/解析）~~（按 ADR-0001 已移除）                                                                                                                            | ❌ 已移除         |
| **本应用预设导入/导出（文件菜单：.lrp 格式）**                                                                                                                                                                    | ✅ v2.2 (M3) 新增  |
| **预设序列化加固（TestProfileIO：完整往返/version 与未知键容错/save-load/哈希）**                                                                                                                                    | ✅ v2.2 (M3) 新增  |
| **M1 快捷键体系（全局裸键 + Ctrl 组合）+ 撤销/重做（UndoStack，滑块一步/蒙版一笔一步）+ 白平衡吸管（W）**                                                                                                                           | ✅ v2.4.0 新增     |
| **M2 缩放/平移体验（光标锚定滚轮缩放 + 空格/中键/方向键平移 + 适合宽度视图 Ctrl+Alt+0 + 拖动进 1:1 请求全尺寸 + Z 键记忆最后适合模式）**                                                                                                       | ✅ v2.5.0 新增     |
| **M3 蒙版引擎级性能 Phase2（geometryHash 去重 + brushWeightField COW 权重场缓存 + 叠加层降分辨率 + dab 并笔 + 软度滑块接线 + TestMaskRaster）**                                                                               | ✅ v2.6.0 新增     |
| **M4 直观 UI（顶部 QToolBar：视图模式互斥组适合窗口/适合宽度/1:1 + 放大缩小 + 蒙版工具互斥组线性/径向/画笔；zoomModeChanged + maskToolActivated 双向同步、面板为源工具栏镜像；dark.qss 补齐 QToolBar/QToolButton:checked/QComboBox/QCheckBox 纯 CSS）**  | ✅ v2.7.0 新增     |
| **M5 预览更新速率（coarse-then-refine：DevelopPanel 拖动 30ms coarse 节流 + PreviewWorker adjustPreviewCoarse 480 渲染 + MainWindow 路由；refine 维持 50ms 停顿路径；1:1 下 coarse 仍更新预览层、不触发全尺寸刷新；TestPipeline 2 用例）** | ✅ v2.8.0 新增     |
| **P1-3 批量重命名（`RenameTemplate` 模板 `{date}/{index[:W]}/{base}` + 冲突唯一路径；导入端 ImportDialog 复选/模板/预览 + ImportWorker Stage1 按选择顺序重命名；导出端 ExportDialog 批量重命名 + MainWindow 走 batchExport 导出母版+所有虚拟副本）** | ✅ headless 端到端 + Qt `batchExport` 真实导出断言 / 对话框 smoke 已验收（GUI 手动验收待做） |

### 🔄 进行中

- **优化方向（对标 Lightroom Classic · 自成体系）**：定位 master 见
  [docs/spec/optimization-positioning-spec.md](docs/spec/optimization-positioning-spec.md)，
  阶段路线图见 [00-ROADMAP.md](docs/spec/optimization/00-ROADMAP.md)。三大支柱优先级
  **P-A 性能极致 > P-B 离线 AI > P-D 稳定**；ADR-0001 弃 LR 兼容（读+写），数据契约 `.lrp`/`.shcat` 自洽。
  当前 **P-A（性能极致）引擎层 headless 验收已落地并提交（2026-09-25）**：Milestone 节点见
  [pillars/01-pa/](docs/spec/optimization/pillars/01-pa/)（M1 1:1 零等待 / M2 编辑手感 / M3 内存极致 / M4 大目录），
  **引擎层 headless 验收已落地（2026-09-25）**：新增 `tests/TestPerfAnchors.cpp`（合成 8000×6000 F32：
  B-2 1920 精帧 min≈99ms、B-1 全尺寸渲染引擎段≈514ms、D-2 回落≈688MB 可重复），登记 M1/M2/M3 实测记录；
  真机锚点（热/冷 1:1、真机精帧、主线程帧、单图 ≤1.2GB 峰值）登记 **真机待验**，headless 全量 482 用例转绿。
  其余支柱节点见 [00-ROADMAP 索引](docs/spec/optimization/00-ROADMAP.md)。P-A 量化锚点：B-1 热 50MP 1:1 ≤175ms、
  B-2 松手→精帧 ≤80ms、B-3 主线程 ≤16ms/帧、D-1 峰值 ≤1.2GB、D-2 切图回落 ≤400MB；真机锚点待 GPU 环境实测。

- **P-B（离线 AI）实施落地（2026-09-25，工作区未提交）**：spec 见
  [docs/spec/optimization/02-PB-offline-ai.md](docs/spec/optimization/02-PB-offline-ai.md)，
  Milestone 节点见
  [pillars/02-pb/](docs/spec/optimization/pillars/02-pb/)（M1 自动出片 / M2 批量预分级）。
  本轮进入 **P-B（离线 AI）实施**：B-1 本地 AI 自动出片辅助（曝光/对比/WB/裁剪建议 + 1 键应用到参数堆栈，
  非生成式、走非破坏性路径）+ B-2 导入批量 AI 预分级（后台跑既有模型去噪/主体/质量分 + 胶片栏标志）。
  验收基调**能跑 + headless `[ai]` 门控单测绿**，不设吞吐/耗时性能门槛；复用 `ModelManager` manifest + SHA256、
  `AiInference`（DirectML → CPU 回退）。B-3/B-4 为 optional，不作承诺。
  实施进度（引擎/目录/UI 层已落地）：`Pipeline::autoDevelop`（曝光/对比/WB + 内容感知裁剪包围盒，
  裁剪仅作建议、不写参数堆栈）+ `PregradeAnalyzer`（rmbg2-subject 主体分割，缺模型走确定性中心显著性兜底）+
  `CatalogDatabase` V5 迁移（`pregrade_has_subject/score/auto` 三列）+ `setPregrade/getPregrade`；headless 门控单测
  `[autodevelop]` 9 例、`[pregrade]` 7 例、catalog 预分级读回全绿。UI：ImportWorker 导入完成后再后台串行对每张图
  解码标准预览跑预分级 → `setPregrade` 落库 + `pregradeReady` 信号 → Filmstrip 打「AI」角标（有主体青色/无灰）；
  DevelopPanel「自动」按钮沿用既有路径，merge 了 `autoDevelop` 的 contrast 建议进参数堆栈（可撤销/进历史）。
  裁剪字段因管线当前无实际 crop 渲染，按计划偏差仅作建议值（不扩散重构）。GUI 构建链接通过。

- **竞品对齐优化**：当前任务文件为
  [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md)。
  P1/P2/P3/T 均已实现并验收（spec 内验收标准已勾选）。P1-1「CPU vs DirectML 逐通道容差 + ≥2x 耗时」
  与 P3-4 GPU 提速数据需 GPU（RTX A4000）实测（本机未测；provider 回退已覆盖）。其余待补的量化验收
  见 spec 各批次的「验收状态快照」。

- **性能 M1-M5**：相关实现和报告已归档，部分量化验收仍需单独复核；不要将归档文档中的历史测试数字直接当作当前基线。

## 已修复的预存缺陷（v1.5）

- `ImageViewport.cpp` 蒙版工具代码自上次构建后未编译：`BrushDab` 未限定 `lrc::`（2 处）+ 缺 `<algorithm>/<cmath>`，已修。

- `DevelopPanel::MaskRow` 声明无 `decimals` 成员（构造时误写）—— 已移除。

## 已修复的预存缺陷（v1.6）

- **GBK/UTF-8 注释编码导致 MSVC 编译失败**：新增的中文注释行若以汉字/中文标点结尾，其 UTF-8 末字节是 GBK 前导字节（0x81–0xBF），MSVC 在代码页 936 下会把该悬挂字节与换行配对、吞掉换行，从而把下一行（如函数签名）注释掉。修复方式：新注释行一律以 ASCII 字符结尾（如 `(sampled colour).`）。今后写注释请遵循此规则。

## 已修复的预存缺陷（v1.7）

- **`DngConverter.cpp`** **引用** **`imgdata.color.filters`，本工程 LibRaw（vendored 0.22.0 / 0.21.3）中该字段在** **`imgdata.idata.filters`** → 该文件此前在 `build_gui` 中从未编译成功（被 .vcxproj 掩盖），本次重编译即报 C2039。已改为 `idata.filters`（两个 LibRaw 树都正确）。

- **TestDngConverter 的合成 DNG 缺少 CalibrationIlluminant2/ColorMatrix2**：本工程 LibRaw 只有在一对 illuminant+colormatrix 都齐备时才从 DNG 标签填 `cam_xyz`，因此该测试的 `cam_xyz` 恒为零、ColorMatrix1 断言结构性地必然失败。已补全成对标签。

- **TestDngConverter 的** **`readTiffField()`** **只认 SHORT/LONG/BYTE 内联标签**：`ColorMatrix1`（SRATIONAL，count 9）永远返回 -1，`hasTag(50721)` 无法通过。已补 `type==10` 分支（返回偏移量表示存在）。

- **CFA 断言过严**：LibRaw 会把 CFA 绿色规范化为 G2（码 3），输出 `{0,1,3,2}` 而非输入 `{0,1,1,2}`，两者都是合法 RGGB。断言改为语义检查（R 在 (0,0)，G/G2 在绿色位，B 在 (1,1)）。

## 已修复的预存缺陷（v1.8）

- **`ImageIO.cpp`** **TIFF 写入器 IFD 索引错位**：`writeTiffRaw()` 用 `ifd[4]`/`ifd[8]` 回填条带偏移，但 index 4 实际是 tag 262（Photometric），tag 273（StripOffsets）在 index 5。结果是所有导出的 TIFF/DNG 都写成了 Photometric=像素偏移、StripOffsets=0 的畸形文件，stb\_image/libraw 都无法读取，即「导出格式无法再导入」。已改为 `ifd[5]`（273）回填，并新增 IFD 解析回归测试（Photometric==2、StripOffsets>0、ByteCounts==w*3*bps/8\*h）。

- **stb\_image（vendored）不支持 TIFF**：`loadImage()` 的注释声称支持 TIFF，实际 stb 只能读 PNG/JPEG/BMP。已新增内建 TIFF 读取器（未压缩、chunky、8/16 位、RGB/RGBA、LE/BE、多条带、Predictor 2），并在 `loadImage()` 中按 TIFF 魔数分发。

## 已知取舍

- 非 RAW 导入（JPEG/PNG/TIFF）经 sRGB → 线性 ProPhoto 转换，16 位 TIFF 导入时量化为 8 位

- TIFF 读取器仅支持未压缩 RGB（应用自身导出格式及常见工具产物）；压缩/CMYK/浮点 TIFF 视为不支持

- 调整顺序固定，无图层系统；编辑历史为每次提交的参数快照（线性，无分支/时间线）

- 1:1 视图使用预览缓存（最长边 1920），非全尺寸

- 文件夹监视用 OS 事件驱动（`ReadDirectoryChangesW`），不可用时回退 2s 轮询

- 直方图统计调整后线性 F32 显示缓冲（全遍历，无抽样）；8bit 预览仅在无线性数据时回退

- 仅验证了 MSVC 2022 构建

- 点颜色色相重建为简化实现，与 Lightroom 精确行为可能存在差异

- HDR 合并按 frame0 为参考曝光，无手动 EV 校正

<br />
