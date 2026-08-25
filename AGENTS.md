# 仓库指南（Repository Guidelines）

Silver Halide（`lightroom-clone-cpp`）是一个使用 C++20 + Qt 6 的数字摄影桌面应用，聚焦 RAW 解码与非破坏性编辑。本工作区包含项目源码、构建目录以及本地 Qt 安装。

## 项目结构与模块组织

- `lightroom-clone-cpp/` — 项目根目录：`CMakeLists.txt`、源码、测试与文档。
  - `src/engine/` — 纯 C++、不含 Qt 的核心层：`Pipeline`、`Adjustments`、`RawDecoder`、`ColorManager`、`ExportEngine` 及 XMP 相关。
  - `src/catalog/` — 纯 C++：`CatalogDatabase`（SQLite WAL）、`ThumbnailCache`、`FolderMonitor`。
  - `src/ui/` — Qt 6 Widgets 界面层：`MainWindow`、自定义控件与工作线程。
  - `src/utils/` — 共享工具：`Json`、`ProfileIO`、`MathUtils`、`Parallel`。
  - `tests/` — Catch2 单元测试（`Test*.cpp`）；`docs/` — 架构与里程碑文档；`packaging/` — NSIS 安装包；`resources/` — 图标、样式、Qt 资源；`third_party/` — 内置依赖。
- `build_gui/`、`build_headless/` — CMake/Ninja 生成的构建目录，请勿手工修改。
- `qt/` — 本地 Qt 6.8.3（msvc2022_64）安装。
- 根目录脚本：`build_x64.bat`、`_m1_build.bat`、`run_app.ps1`。

## 构建、测试与开发命令

在工作区根目录、x64 MSVC 环境（vcvars64）下执行：

- `.\build_x64.bat` — 配置并构建 Release GUI（输出 `build_gui\SilverHalide.exe`）。
- `.\_m1_build.bat` — 无头构建，使用 `-DLRC_ENABLE_QT=OFF`（仅引擎与目录层）。
- `cmake --build build_gui -j` — 增量重新构建。
- `ctest --test-dir build_gui --output-on-failure` — 运行完整 Catch2 测试套件。
- `.\run_app.ps1` — 启动 GUI 并捕获 stdout/stderr。
- 部署 Qt 运行库：`qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build_gui\Release\SilverHalide.exe`。
- 生成安装包：`makensis lightroom-clone-cpp\packaging\installer.nsi`。

常用 CMake 选项：`-DLRC_ENABLE_QT=OFF`（无头构建）、`-DLRC_BUILD_TESTS=OFF`、`-DLRC_PROXY=https://ghfast.top/`（依赖下载代理镜像）。

## 编码风格与命名约定

- 使用 C++20（关闭语言扩展），CMake >= 3.25。`engine/` 与 `catalog/` 禁止包含 Qt 头文件，以保证核心逻辑可无头构建与测试。
- 一个类对应一个文件，类名使用 PascalCase（如 `Adjustments.h` / `Adjustments.cpp`）；函数使用 `camelCase`；常量使用 `UPPER_CASE`。
- 测试文件命名为 `Test<类名>.cpp`；CMake 目标统一使用 `lrc_` 前缀（`lrc_engine`、`lrc_catalog`、`lrc_lightroom`、`lrc_tests`）。
- MSVC 下：引擎目标为 `/W4`，应用与测试为 `/W3`，保持无警告。仓库未配置 lint 工具，请与既有代码风格保持一致。

## 测试规范

- 测试使用 Catch2 v3，且不得依赖 Qt —— 核心逻辑应在无头构建中验证。
- 新增测试时创建 `Test*.cpp` 文件，并在 `tests/CMakeLists.txt` 的 `lrc_tests` 目标中注册。
- 测试行为而非实现细节，保持套件快速。改动 GUI 前，先运行 `ctest --test-dir build_headless --output-on-failure`。

## 提交与 Pull Request 规范

本工作区尚未纳入 Git。引入版本控制后，请使用 Conventional Commits 约定（`feat:`、`fix:`、`refactor:`、`test:`、`docs:`），保持提交聚焦；PR 需说明动机、关联相关 issue、附测试结果，UI 改动附截图。
