# Silver Halide（lightroom-clone-cpp）

使用 **C++20 + Qt6 Widgets** 构建的数字摄影桌面应用，聚焦 RAW 解码与非破坏性编辑。
核心架构目标是一条**非破坏性编辑管线**：原始像素永不修改，所有调整以参数堆叠（Parameter Stack）的形式
保存，预览 / 导出时在**线性 ProPhoto RGB 工作空间**中实时渲染。

**版本 2.11.0 — 默认目录机制 + 目录内嵌预览** ✅ | 测试全绿（272 用例 / 274373 断言，2026-08-14 验收）

**Silver Halide** · © ASE-128 2026

## 功能

- 三栏布局：左导航栏 / 中央预览 + 底部胶片栏 / 右开发面板，暗色主题（QSS）
- 导入 ARW 原始文件（批量导入走事务 + 后台线程，缩略图异步生成，不阻塞 UI）
- 非破坏性调整：曝光、对比度、高光、阴影、白/黑、色温、色调、饱和度、纹理/清晰度/去朦胧/鲜艳度、颗粒 + 色调曲线
- 调整顺序：**曝光 → 对比度 → 高光 → 阴影 → 白/黑 → 色调曲线 → 白平衡 → 去朦胧 → 鲜艳度 → HSL（各通道）→ 色调分离 → 点颜色 → 空间阶段（纹理/清晰度/锐化/畸变/颗粒/晕影/蒙版局部）→ sRGB**
- HSL 各通道独立调整：8 个色相范围（红/橙/黄/绿/青/蓝/紫/品红），每通道色相偏移、饱和度、明亮度
- 色调分离：高光/阴影独立着色 + 可视化色轮
- 蒙版局部调整：线性/径向/画笔绘制 + 红色权重叠加 + 羽化/不透明度/反转 + 局部滑块（v1.5）
- 点颜色（ColorTarget）：取参考色吸管 + 多目标 色相/饱和/明亮/范围/平滑/强度（v1.6）
- 照片管理：评分 1–5 / 旗标 / 删除 / 筛选（v1.4）
- 多格式导出对话框：JPEG/PNG/TIFF-8/16/DNG（v1.4）
- HDR 合成：多张曝光包围 → 合并 DNG → 入目录（v1.4）
- 实时预览：50ms 防抖 + 后台渲染线程 + 参数哈希 LRU 缓存，预览延迟 < 200ms
- RGB 直方图实时跟随预览（线性 / 对数刻度，双击切换）
- 色调曲线：分区点曲线，高光/亮色调/暗色调/阴影四句柄拖动（黑白端点固定、纵向保持单调、双击复位该区），绘制与渲染共用单调三次插值（v2.1.0）
- 编辑参数保存至 `.lrcat` 目录数据库（SQLite WAL），重启后自动恢复
- 文件夹监视：被监视文件夹新增 RAW 自动导入
- 受管库导入对话框：添加 / 复制到目录 / 移动 + 按日期受管库 `<root>/<yyyy>/<yyyy-mm-dd>`（v1.9）
- 「目录」菜单：新建/打开/备份/优化目录，路径经 QSettings 持久化，切换后自动重启（v1.9）
- 编辑历史快照：每次保存自动记录，照片菜单「编辑历史…」可查看并恢复（v1.9）
- 导航面板「目录图片库」分组：浏览受管库的子文件夹并过滤胶片栏（v1.9）
- 标签颜色：7 色可设/清除 + Filmstrip 角标 + 右键菜单/快捷键 + Navigator 色标筛选（v2.0）
- 收藏夹集合：创建/重命名/删除 + 成员增删（幂等）+ Navigator 分组过滤 + CollectionsDialog（v2.0）
- XMP sidecar 读写：照片菜单「读取/写入 sidecar」，`.xmp` 与图像同名，写侧生成 Lightroom 兼容 XMP 文档（v2.2.0）
- 预设导入/导出：文件菜单「导入/导出预设…」，`.xmp` Lightroom 预设 / `.lrp` 本应用预设（v2.2.0）
- crs 字段映射：基本/细节/效果/颗粒/曲线 PV2012 点数组/HSL 24/色调分离，未知字段容错，Temperature mired 6500K、Tint ×1.5（v2.2.0）
- 性能优化：快速高斯（大 sigma 走 3-box 滑动窗口，清晰度实测 ~87x）、HSL/点色 rgbToHSL 去重、中性参数快速路径、相机矩阵就地+并行解码、`toF32()` 并行、`toDisplaySRGB`/`toQImage` 去冗余拷贝（v2.3.0）
- 发布打包：**GUI 子系统（无控制台窗口）** + **NSIS 安装包**（`dist/Silver Halide Setup 2.11.0.exe`，含 Qt/VC 运行时与全部插件，一键安装/卸载，见「打包（NSIS）」）（v2.3.1 起）
- 默认目录机制：未新建/打开目录时导入进「Default」默认目录（`Default.lrcat`），旧 `catalog.lrcat` 自动迁移，「目录」菜单可随时切换回默认目录（v2.11.0）
- 目录内嵌预览：`previews` 表把 320px 缩略图与 1:1 JPEG 存进目录文件，导入时预生成，默认调整下 1:1 直读免重复解码（v2.11.0）

## 架构

```
src/
├── engine/   纯 C++（禁止 Qt 头文件）：RawDecoder、ImageBuffer、Pipeline、
│             Adjustments、ColorManager(lcms2)、ExportEngine、ImageIO、
│             Xmp（pugixml XMP 解析器）、XmpSidecar
├── catalog/  纯 C++：CatalogDatabase(sqlite3)、ThumbnailCache、FolderMonitor
├── ui/       Qt6 层：MainWindow + 7 个自绘控件 + ImportWorker/PreviewWorker
├── app/      Application 入口、常量
└── utils/    ProfileIO、XmpProfile、MathUtils、Json（自研最小 JSON 解析器）
```

详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 构建

依赖：CMake ≥ 3.25、支持 C++20 的编译器（MSVC 2022 / Clang 17 / GCC 13）、Qt 6.x。

> **本项目实际布局**（Windows）：使用 **Ninja 生成器**（完整构建脚本见父目录 `build_x64.bat`），
> GUI exe 输出到 `build_gui/SilverHalide.exe`（`lrc_tests.exe` 在 `build_gui/tests/`），无 Release 层，
> 增量构建用 `cmake --build build_gui -j`。`build_gui` 位于仓库**父目录**（`../build_gui`）；
> 仓库根目录下残留的 VS 旧树 `build_gui/`（`build_gui/Release/`）已废弃，打包勿指向该树。
> 下面的通用命令以 `build` 目录为例，与具体生成器无关。打包发布见「打包（NSIS）」。

### Windows 快速开始（使用已安装的 Qt）

```bat
cmake -S lightroom-clone-cpp -B build -G Ninja ^
      -DCMAKE_PREFIX_PATH=C:\path\to\qt\6.8.3\msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 使用 vcpkg

```bat
git clone https://github.com/microsoft/vcpkg
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install
cmake -S . -B build -G Ninja ^
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake ^
      -DCMAKE_PREFIX_PATH=<Qt 安装目录>
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 使用 FetchContent + 国内代理

```bat
pip install cmake ninja aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtbase
cmake -S . -B build -G Ninja -DLRC_PROXY=https://ghfast.top/ ^
      -DCMAKE_PREFIX_PATH=%CD%\6.8.3\msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 运行

```powershell
# 设置 Qt 平台插件路径（根据实际 Qt 安装位置调整）
$env:QT_QPA_PLATFORM_PLUGIN_PATH="C:\path\to\qt\6.8.3\msvc2022_64\plugins\platforms"
.\build\SilverHalide.exe
```

本项目（Ninja 生成器，exe 在 `build_gui/SilverHalide.exe`，无 Release 层）部署 Qt 运行时后直接运行（无控制台窗口）：

```powershell
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime .\build_gui\SilverHalide.exe
.\build_gui\SilverHalide.exe
```

## 打包（NSIS）

Windows 安装包（`dist/Silver Halide Setup 2.11.0.exe`），一键安装到
`Program Files\Silver Halide`，含 Qt / VC++ 运行时与全部插件，带开始菜单与桌面快捷方式。

```bat
:: 1) 重建 GUI exe 并部署运行时到 build_gui/（Ninja 布局，无 Release 层）
cmake --build build_gui -j
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build_gui/SilverHalide.exe
::    （再把 VS redist 的 vcruntime140*/msvcp140* 等 DLL 复制进 build_gui/）

:: 2) 暂存发布载荷（exe + Qt DLL + 插件子目录 + VC 运行时，不含 .lib）到 build_gui/install_stage/
::    （installer.nsi 的 STAGE_DIR 指向父目录的活跃 Ninja 树 build_gui/install_stage）

:: 3) 编译 NSIS 安装包
C:\Users\tmlde\Desktop\dev\nsis\makensis.exe packaging\installer.nsi
```

- 脚本：`packaging/installer.nsi`（MUI2 中文界面，UTF-8 BOM）；欢迎页位图
  `packaging/welcome.bmp` 由 `packaging/make_bmp.py` 生成（NSIS 自带 Contrib 缺默认图）。
- 应用为 **GUI 子系统**（`WIN32` + `Qt6::EntryPointPrivate`），启动不弹出控制台窗口。
- 应用名 **Silver Halide**，版权 **© ASE-128 2026**（写入 exe VERSIONINFO 与安装包元数据）。

## 使用

1. `文件 → 导入文件夹…` 选择存放 ARW 的目录（会进入监视列表）
2. 底部胶片栏点击缩略图载入图片
3. 右侧面板拖动滑块，预览与直方图实时更新；松开滑块后写入目录数据库
4. `文件 → 导出 JPEG…` 全尺寸渲染导出

## 测试

引擎与目录层为纯 C++，可用 Catch2 独立验证（无需 Qt）：

```bat
ctest --test-dir build --output-on-failure
# 或直接运行
.\build\tests\lrc_tests.exe
```

### 测试结果（v2.11.0）

Catch2 v3 套件（引擎/目录层，无需 Qt）。2026-08-14 AUDIT 验收 headless + GUI 双端
**272 用例 / 274373 断言**全绿、EXIT=0。最终计数以
`ctest --test-dir build --output-on-failure` 输出为准。

测试覆盖：

- **Adjustments**: 默认值、复位、相等性、哈希、HSL、色调分离、蒙版
- **Pipeline**: 曝光/对比度/高光/阴影/白黑/色温/HSL/色调分离/清晰度/颗粒/蒙版局部/可视化叠加
- **ImageBuffer**: U16/U8/F32 读写、裁剪、缩放、格式转换
- **ColorManager**: D65→ProPhoto 矩阵、相机矩阵、中性灰渲染、sRGB 往返
- **RawDecoder**: 最小 DNG 验证与元数据读取
- **CatalogDatabase**: 打开、添加、编辑、事务、评分、文件夹、备份
- **ExportEngine**: JPEG/PNG/TIFF-8/16/DNG 导出、PPM、PNG 读取往返
- **Json**: 标量、对象、美化输出、转义、拒绝畸形输入、指数
- **MathUtils**: clamp、stopsToGain、smoothstep、hashCombine、UTF-8 路径
- **Masks**: 线性/径向/画笔蒙版权重、局部调整、JSON 往返（含布尔 invert/enabled）
- **HdrMerge**: 曝光包围合成、EV 估计
- **ZoneCurve**: 四分区曲线布局、自由点量化、单调插值（线性精确/过控制点/无过冲）
- **XmpDocument**: XMP 解析（命名空间、rdf:Description、crs 属性提取、畸形 XML 容错）
- **XmpProfile**: crs↔Adjustments 映射（往返全组、未知键容错、越界钳制、金色值）
- **XmpSidecar**: sidecar 路径、写后读往返、缺失/损坏/空 XML、deferred 内嵌读取
- **ProfileIO**: 预设 JSON 完整往返、version/未知键容错、savePreset/loadPreset、ColorTarget/Mask 哈希

## 验收标准

| #   | 标准                       | 实现位置                                                       |
| --- | ------------------------ | ---------------------------------------------------------- |
| 1   | 导入 A7C II ARW，正确白平衡 / 曝光 | `engine/RawDecoder`（`output_color=0` 线性 + 相机矩阵 → ProPhoto） |
| 2   | DevelopPanel 七项调整        | `ui/DevelopPanel` + `engine/Pipeline::applyAdjustments()`  |
| 3   | 拖动延迟 < 200ms（1920×1080）  | 50ms 防抖 + 后台 PreviewWorker + LRU 缓存                        |
| 4   | 直方图实时跟随                  | `ui/HistogramWidget` 读取预览 8bit 数据，每 4M 像素抽样                |
| 5   | 导出 8bit sRGB JPEG        | `engine/ExportEngine` + `engine/ImageIO`（stb_image_write）  |
| 6   | 编辑参数持久化 + 重启恢复           | `catalog/CatalogDatabase.edits` 表（JSON 参数堆叠）               |
| 7   | 暗色三栏布局                   | `ui/MainWindow`（QSplitter）+ `resources/styles/dark.qss`    |
| 8   | 1000 张导入不阻塞 UI           | `ui/ImportWorker` 后台线程 + 事务批量写入                            |

## 项目结构

```
lightroom-clone-cpp/
├── CMakeLists.txt          # FetchContent 依赖管理
├── vcpkg.json              # vcpkg 依赖清单
├── src/
│   ├── main.cpp            # 入口，注册元类型
│   ├── app/                # Application、Constants
│   ├── engine/             # 纯 C++ 核心引擎（无 Qt）
│   │   ├── RawDecoder      # libraw 封装，输出线性 ProPhoto RGB
│   │   ├── ImageBuffer     # U16/F32/U8 RGB 像素缓冲区
│   │   ├── Pipeline        # 非破坏性编辑管线 + LRU 缓存
│   │   ├── Adjustments     # 参数堆叠结构体
│   │   ├── ColorManager    # lcms2 色彩管理（ProPhoto ↔ sRGB）
│   │   ├── ExportEngine    # 全尺寸 JPEG 导出
│   │   ├── ImageIO         # stb_image_write JPEG/PPM 编码
│   │   ├── ZoneCurve       # 分区点曲线布局 + 单调插值
│   │   ├── Xmp            # pugixml XMP 解析器
│   │   └── XmpSidecar     # .xmp sidecar 读写
│   ├── catalog/            # 纯 C++ 目录层（无 Qt）
│   │   ├── CatalogDatabase # SQLite 目录 .lrcat
│   │   ├── ThumbnailCache  # 磁盘 + 内存 LRU 缩略图
│   │   └── FolderMonitor   # 文件系统轮询监视
│   ├── ui/                 # Qt6 界面层
│   │   ├── MainWindow      # 三栏主窗口
│   │   ├── DevelopPanel    # 右栏参数面板
│   │   ├── HistogramWidget # 自绘 RGB 直方图
│   │   ├── ImageViewport   # 中央预览区（缩放/拖拽）
│   │   ├── FilmstripWidget # 底部胶片栏
│   │   ├── NavigatorWidget # 左栏导航
│   │   ├── CurveEditor     # 色调曲线编辑
│   │   ├── ImportWorker    # 后台导入线程
│   │   └── PreviewWorker   # 后台预览/导出线程
│   └── utils/              # 工具库
│       ├── Json            # 自研最小 JSON 解析器
│       ├── ProfileIO       # 调整参数序列化（.lrp 预设）
│       ├── XmpProfile      # crs↔Adjustments 字段映射
│       └── MathUtils       # 数学辅助函数
├── tests/                  # Catch2 单元测试（25 个 Test*.cpp）
├── resources/
│   ├── styles/dark.qss     # 暗色主题
│   └── resources.qrc       # Qt 资源文件
├── docs/
│   └── ARCHITECTURE.md     # 架构文档
├── packaging/              # NSIS 安装脚本（installer.nsi）+ 欢迎页位图生成（make_bmp.py）
├── dist/                   # 安装包输出（Silver Halide Setup 2.11.0.exe）
└── third_party/            # 依赖源码（libraw、lcms2、sqlite3、Catch2、stb）
```

## 路线图

- [ ] 真正 1:1 放大（按需全尺寸解码渲染）
- [x] HSL 各通道独立调整、色调分离（v1.1.0）
- [x] 多格式导出 / 照片管理（评分/旗标/筛选）/ HDR 合成（v1.4.0）
- [x] 蒙版局部调整（线性/径向/画笔 + 局部滑块，v1.5.0）
- [x] 点颜色 ColorTarget（吸管取参考色 + 滑块，v1.6.0）
- [x] 受管库导入（添加/复制/移动 + 按日期受管库）与目录菜单（v1.9.0）
- [x] 编辑历史快照 + 恢复（v1.9.0）
- [x] 照片管理：标签颜色、收藏夹集合（v2.0.0）
- [x] 色调曲线：分区点曲线（v2.1.0）
- [x] XMP 兼容 / Lightroom 预设互导（v2.2.0）
- [x] Windows NSIS 安装包 + 无控制台发布（v2.3.1）
- [x] 默认目录机制 + 目录内嵌预览（v2.11.0）
- [ ] macOS / Linux 构建验证
