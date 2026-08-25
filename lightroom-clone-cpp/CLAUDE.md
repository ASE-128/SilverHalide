# CLAUDE.md

此文件为 Claude Code (claude.ai/code) 在此代码库中工作时提供指导。

## 构建命令

> **工作区根目录**是仓库上一级（`D:\Desktop\SilverHalide-src`，2026-08-18 迁移至此），活跃构建树为根目录
> `build_gui/`（Ninja 布局，`SilverHalide.exe` 与 `tests/lrc_tests.exe` 直接在 `build_gui/` 下，
> 无 Release 层）。仓库内 `lightroom-clone-cpp/build_gui/`（VS 旧树，带 `Release/` 层）与
> `build_asan` 为废弃残留，打包勿指向该树。
>
> 常用包装脚本（根目录，均须 x64 MSVC 环境）：

```bat
:: 完整配置 + 构建 GUI（内联 vcvars64.bat）→ build_gui\SilverHalide.exe
.\build_x64.bat

:: 增量重建 GUI（-j4 并行；-j8 内存不足会批量失败）
.\_build_gui4.bat

:: 无头构建（仅引擎 + 目录测试，无 Qt）
cmake -S lightroom-clone-cpp -B build_headless -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j

:: 运行测试（GUI 层建议直跑 exe，见下）
.\_run_gui_tests.bat      :: 跑 build_gui\tests\lrc_tests.exe，输出 GUI_TESTS_EXIT
.\_run_smoke.ps1          :: GUI 启动冒烟：等主窗口句柄 → WM_CLOSE → 退出码 0
.\run_app.ps1             :: 启动 GUI 并捕获 stdout/stderr 到 app_*.log
```

**等价手写命令**：

```bat
:: 完整构建 (GUI + 测试)
cmake -S lightroom-clone-cpp -B build_gui -G Ninja ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j

:: 无头构建 (仅引擎 + 目录测试，无 Qt)
cmake -S lightroom-clone-cpp -B build_headless -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j

:: 运行测试（headless 层用 ctest 稳定）
ctest --test-dir build_headless --output-on-failure
:: GUI 层测试：直跑 exe（中文路径下 ctest 进程管理不稳会假卡死）
.\build_gui\tests\lrc_tests.exe
:: 或一键：.\_run_gui_tests.bat（自动加 PATH/QT_PLUGIN_PATH）

:: 使用代理 (中国网络)
cmake -S lightroom-clone-cpp -B build_gui -G Ninja ^
      -DLRC_PROXY=https://ghfast.top/ ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
```

```powershell
# 部署 Qt 运行时（关键：解决“Qt 平台插件缺失”错误）
# 将 Qt6*.dll、plugins/platforms/qwindows.dll 等复制到 exe 所在目录
qt/6.8.3/msvc2022_64/bin/windeployqt.exe --release --no-translations --compiler-runtime build_gui/SilverHalide.exe

# 运行 GUI 应用（exe 在 build_gui/，Ninja 生成器布局，无 Release 层）
.\build_gui\SilverHalide.exe
```

> **注意**：配置必须使用 **x64 MSVC 工具链**（先 `call vcvars64.bat`）。若用默认的
> `Hostx86/x86` cl.exe 配置，会得到 32 位 lib，无法链接 64 位 Qt（报 DLL/插件错误）。
> 完整脚本见父目录 `build_x64.bat`。

## 打包（NSIS）

`lrc_lightroom` 目标（输出 `SilverHalide.exe`）用 `WIN32` + `Qt6::EntryPointPrivate` 链接为 **GUI 子系统**（无控制台）。
发布时把 exe + Qt DLL + plugins + VC++ 运行时暂存到 `build_gui/install_stage/`（不含 .lib），
再编译安装包：

```bat
:: 1) 重建 + 部署（把 windeployqt 复制的 DLL/插件、以及 VC redist 的
::    vcruntime140/msvcp140 等 DLL 放入 build_gui/，Ninja 布局无 Release 层）
cmake --build build_gui -j

:: 2) 暂存发布载荷（脚本见 packaging/，可手工复制）
::    build_gui/*.dll + 插件子目录 -> build_gui/install_stage/
::    （installer.nsi 的 STAGE_DIR 指向父目录活跃 Ninja 树 build_gui/install_stage）

:: 3) 编译 NSIS 安装包 -> dist\Silver Halide Setup 2.11.0.exe
C:\Users\tmlde\Desktop\dev\nsis\makensis.exe packaging\installer.nsi
```

> 安装脚本 `packaging/installer.nsi`（UTF-8 BOM）需要自定义欢迎页位图
> `packaging/welcome.bmp`（NSIS 自带 Contrib 缺图），由 `packaging/make_bmp.py` 生成。

## 架构

**三层 C++20 桌面应用程序** (Qt6-free 引擎 + 目录，Qt6 Widgets UI):

### 第一层: `engine/` — 纯 C++，无 Qt 头文件
- `RawDecoder` — 统一解码：libraw 解码 RAW/DNG → 线性 ProPhoto RGB (U16)（`cam_mul` + `cam_xyz` 相机校准）；JPEG/PNG/TIFF 经 `ImageIO::loadImage` + `ColorManager::srgbToLinearProPhoto` → F32。`setColorManager()` 由 GUI 注入一次，扩展名分发见 `ImageFormats`
- `ImageBuffer` — RGB 交错 U16/F32/U8，面积平均缩放，裁剪，F32 转换
- `Pipeline` — 非破坏性编辑：曝光 → 对比度 → 高光 → 阴影 → 色调曲线 → 白平衡 → HSL (各通道) → 色调分离。逐像素阶段经 `utils/Parallel` 线程池按行/块并行。LRU 缓存键为 `(imageId, adjHash, maxDim)`。静态 `applyAdjustments()` 用于直接使用
- `Adjustments` — 参数堆叠结构 (7 个基本参数 + 色调曲线 + **HSLChannel[8]** + **SplitTone**)，`hash()` 用于缓存键。`ColorRange` 枚举 (红/橙/黄/绿/青/蓝/紫/品红)
- `ColorManager` — lcms2 封装：ProPhoto↔sRGB 转换，相机矩阵，D65↔D50 色适应。transform 以 `cmsFLAGS_NOCACHE` 构建，`shared_mutex` 保护——转换块可并发（shared lock），`loadDisplayProfile` 重建时独占
- `ExportEngine` — 通过 stb_image_write 进行全尺寸 JPEG 导出
- `ImageIO` — stb_image_write JPEG/PNG 编码 + 内建 TIFF/DNG 写入；解码 PNG/JPEG/BMP 走 stb_image，TIFF 走内建读取器（stb 不支持 TIFF）；`loadImage()` 返回 8bit sRGB
- `Xmp` — pugixml XMP 解析器：命名空间/rdf:Description 提取 crs 属性，畸形文档容错
- `XmpSidecar` — `.xmp` sidecar 读写（与图像同名），写侧生成 Lightroom 兼容 XMP 文档；RAW 内嵌 XMP 读取 deferred

### 第二层: `catalog/` — 纯 C++，无 Qt 头文件
- `CatalogDatabase` — SQLite WAL `.lrcat` 文件，包含 `images`, `edits` (JSON 参数堆叠), `folders` 表
- `ThumbnailCache` — FNV-1a 哈希文件名，嵌入式 JPEG 然后备用 PPM
- `FolderMonitor` — 2秒文件系统轮询用于自动导入

### 第三层: `ui/` — Qt6 Widgets
- `MainWindow` — 3列 QSplitter 布局，深色 QSS 主题
- `DevelopPanel` — 调整滑块，50ms 防抖 → PreviewWorker
- `HistogramWidget` — 来自预览 8bit 数据的 RGB 实时直方图，线性/对数切换
- `ImageViewport` — 带缩放/平移的中央预览
- `CurveEditor` — LRC 风格分区点曲线：高光/亮色调/暗色调/阴影四分区句柄在固定黑白端点间拖动，纵向保持单调、横向限在本区带内；双击句柄复位该区。绘制与管线共用 `zonecurve::interpolate`（Fritsch–Carlson 单调三次，无过冲、恒等曲线严格为直线）
- `FilmstripWidget` — 底部缩略图条
- `NavigatorWidget` — 左侧面板
- `ImportWorker` (QThread) — 带 SQLite 事务的批处理导入
- `PreviewWorker` (QThread) — 解码 + 渲染管线，代次计数丢弃陈旧结果

### `utils/` — 在引擎和目录之间共享
- `Json` — 自包含的最小 JSON 解析器/序列化器，用于参数堆叠
- `ProfileIO` — 调整序列化（.lrp 预设，字段无损完整往返）
- `XmpProfile` — crs↔Adjustments 字段映射（基本/细节/效果/颗粒/曲线 PV2012 点数组/HSL 24/色调分离），Temperature mired 6500K、Tint ×1.5，未知字段容错
- `MathUtils` — clamp, stopsToGain, smoothstep, hashCombine, UTF-8 路径助手
- `Parallel` — 全局惰性线程池 + `parallelFor(n, grain, fn)` 分块并行：嵌套安全（池线程内退化为串行）、`LRC_THREADS` 环境变量覆盖线程数。用于引擎各渲染阶段（逐像素循环、高斯模糊 pass、lcms 转换块）

### 线程模型
- 主线程：UI 事件、布局、直方图渲染、DB 读取
- `PreviewWorker` (QThread)：解码 + 管线渲染 + 导出，带代次计数的串行排队请求
- `ImportWorker` (QThread)：批处理导入 + 缩略图生成
- 跨线程数据通过 `ImageBufferPtr` (shared_ptr) 传输，注册为 Qt 元类型
- `Pipeline` 渲染阶段经全局线程池多核并行（`utils/Parallel`，线程数 = 硬件并发，`LRC_THREADS` 可覆盖）；嵌套 `parallelFor` 安全退化，无死锁
- `ColorManager`/`Pipeline`/`CatalogDatabase`/`ThumbnailCache` 内部锁保护，可安全跨线程共享；lcms 转换经 `shared_mutex` 并发

### 关键设计决策
- 所有调整在线性光线下进行 (伽马 1.0)，sRGB 伽马在最后由 lcms 应用一次
- 预览采用 1920 最长边，通过 libraw `half_size` + 面积平均缩小
- 缓存: `PipelineCacheKey = hash(imageId, adj.hash(), maxDim)`，256MB LRU（约 40 张 1080p 预览）
- 相机管线: `output_color=0` → 手动 `cam_xyz` → Bradford D65→D50 → ProPhoto 矩阵
- 调整顺序固定，v1 版本中无图层/快照
- `engine/` 和 `catalog/` 从不包含 Qt 头文件 — 可在无 Qt 环境下测试
- HSL 调整：8 个颜色范围 (红/橙/黄/绿/青/蓝/紫/品红)，每个都带有色相/饱和度/亮度控制
- 色调分离：高光/阴影着色与平衡控制

## 构建系统

- CMake 3.25+, C++20, Ninja 生成器
- 依赖项通过 FetchContent (通过 `LRC_PROXY` 支持代理) 或在 `third_party/` 下供应商提供
- Qt 6.8.3 (msvc2022_64)，通过 aqtinstall 或 vcpkg 安装
- 选项: `LRC_BUILD_TESTS=ON`, `LRC_ENABLE_QT=ON`, `LRC_USE_OPENCV=OFF`
- vcpkg.json 列出所有依赖项 (qtbase, libraw, lcms, sqlite3, opencv4, catch2)
- MSVC: 根 `CMakeLists.txt` 已全局 `add_compile_options(/EHsc)`（异常必需，`utils/Parallel.cpp` 的
  异常守卫依赖它；缺失时 C4530，try/catch 不生效）
- Release 构建须核对 `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`——CMakeCache 丢失会让 Release
  退化为无优化，多线程测试会暴露时序竞态 / 退出卡死（见 `docs/Speedup_M3_STATUS.md`）

## 应用图标

- 运行图标：`resources/icons/app_256.png`，经 `resources/resources.qrc` 注册，`Application` 构造时 `setWindowIcon`
- exe 图标：`resources/icons/app.ico`，通过 `resources/app.rc`（相对路径）由 CMake 嵌入 Windows exe
- 绘制脚本：`resources/tools/make_icons.py`（PIL/numpy，4x 超采样渲染后缩放）。修改设计后重跑 `python resources/tools/make_icons.py` 重新生成全部尺寸
- CMake 会 `configure_file` 将 `app.ico` 复制到构建目录，确保 rc.exe 无论按 `.rc` 所在目录还是按构建目录解析都能找到图标

## 当前状态 (v2.11.0)

应用名：**Silver Halide**（`src/app/Constants.h` 的 `kAppName`），版权 **© ASE-128 2026**
（exe VERSIONINFO / About 对话框 / NSIS 安装包元数据）。`lrc_lightroom` 目标以 `WIN32` +
`Qt6::EntryPointPrivate` 链接为 GUI 子系统，**启动无控制台窗口**。安装包见
`packaging/installer.nsi` → `dist/Silver Halide Setup 2.11.0.exe`。

应用程序支持：
- 带固定调整顺序的非破坏性编辑：曝光 → 对比度 → 高光 → 阴影 → 白/黑 → 色调曲线 → 白平衡 → 去朦胧 → 鲜艳度 → HSL → 色调分离 → 点颜色 → 空间阶段（纹理/清晰度/锐化/畸变/颗粒/晕影/蒙版局部）→ sRGB
- 测试全绿（Catch2 v3，引擎/目录层无 Qt；2026-08-14 AUDIT 验收 headless + GUI 双端 **272 用例 / 274373 断言**）
- 色调曲线：分区点曲线（高光/亮色调/暗色调/阴影四句柄拖动，黑白端点固定，双击句柄复位该区）
- HSL 8 通道 × 色相/饱和/明亮 + 点选吸管
- 高光/阴影着色的色调分离 + 可视化色轮
- 蒙版局部调整 UI（线性/径向/画笔绘制 + 红色权重叠加 + 羽化/不透明度/反转，v1.5）
- 点颜色 ColorTarget UI（吸管取参考色 + 多目标滑块，v1.6）
- 标签颜色（7 色可设/清除 + Filmstrip 角标 + 右键菜单/快捷键 + Navigator 筛选，v2.0）
- 收藏夹集合（collections/collection_images 表 + Navigator 分组过滤 + CollectionsDialog，v2.0）
- XMP sidecar 读写（照片菜单「读取/写入 sidecar」，.xmp 与图像同名，写侧生成 Lightroom 兼容 XMP 文档，v2.2）
- 预设导入/导出（文件菜单「导入/导出预设…」）：.xmp Lightroom 预设 / .lrp 本应用预设（v2.2）
- crs 字段映射覆盖（基本/细节/效果/颗粒/曲线 PV2012 点数组/HSL 24/色调分离），未知字段容错，Temperature mired 6500K、Tint ×1.5（v2.2）
- 多格式导出（JPEG/PNG/TIFF/DNG）/ 照片管理（评分/旗标/筛选）/ HDR 合成（v1.4）
- <200ms 延迟的实时预览（50ms 防抖 + LRU 缓存 + 渲染阶段多线程 CPU 并行，1920px 预览实测 ~1.8x 提速）
- **性能优化（v2.3）**：快速高斯（大 sigma 走 3-box 滑动窗口，清晰度 sigma≈38 实测 ~87x）、HSL/点色 rgbToHSL 去重与参考色提升、中性参数快速路径、`applyMatrixU16` 就地+并行解码、`toF32()` 并行、`toDisplaySRGB` 去冗余拷贝、`toQImage` 零拷贝
- **默认目录机制（v2.11）**：Default.lrcat + 旧 catalog.lrcat 自动迁移 + 「切换到默认目录」
- **目录内嵌预览（v2.11）**：previews 表（320px 缩略图 + 1:1 JPEG），导入时预生成，默认调整 1:1 直读
- 3面板深色主题 UI (导航/预览/开发)
- 带 SQLite 持久化的目录数据库（.lrcat）
- 带后台缩略图生成的批处理导入
- **性能优化主线（M1–M5 计划）**：M1–M3 已完成并验收（`docs/Speedup_M1/M2/M3_STATUS.md`）；
  **M4 引擎进阶（清晰度降采样/HSL bitset/SIMD）与 M5 数据/IO 优化（导出非缓存、缩略图 JPEG+容量、导入 fullres 懒生成、HDR 分块、1:1 全分辨率缓存）均已完成并验收（2026-08-18，多子代理并行实施，双端 310 用例全绿）**；
  M5-6（FolderMonitor 离主线程）为可选项未实施。计划文件 `docs/plans/perf-m1-m5-plan.md`
- **AUDIT-2026-08-14**：7 路审计的 8 严重 + 20 中等全部修复，双端 272 用例全绿、GUI 退出冒烟通过
  （报告已归档 `docs/archive/AUDIT-2026-08-14.md`）

## 编码约定

- 命名空间: `lrc`
- 所有引擎/目录文件: `#pragma once`，无 Qt 包含
- ImageBuffer 是 RGB 交错 (3 个通道)，标准化到 [0,1] 供浮点访问
- MSVC: `/W4 /wd4100 /wd4244 /wd4996`, `NOMINMAX`, `_CRT_SECURE_NO_WARNINGS`
- Qt 目标: `CMAKE_AUTOMOC ON`, `CMAKE_AUTORCC ON`
- 测试使用 Catch2 v3 与 `Catch2::Catch2WithMain`
- 跨线程类型 (ImageBufferPtr, Adjustments) 在 `main()` 中通过 `qRegisterMetaType` 注册
- **中文注释行必须以 ASCII 结尾**（如 `// 点颜色参考色取样 (sampled colour).`）：工程未设 `/utf-8`，MSVC 按 GBK 代码页读取时，行末汉字/全角标点的 UTF-8 末字节（0x81–0xBF）会被当作 GBK 前导字节与换行配对，吞掉换行把下一行注释掉，导致费解的解析错误。参见记忆 [[feedback-gbk-comment-encoding]]。
