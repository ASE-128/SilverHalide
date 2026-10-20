# Silver Halide

**Version 1.0.0** · © ASE-128 2026 · MIT License

Silver Halide 是一个基于 C++20 + Qt 6 的 RAW 摄影工作流桌面应用，聚焦 RAW 解码、非破坏性编辑以及高性能预览 / 导出链路。本仓库即为 1.0.0 正式版的源代码与构建产物；此前的 2.x 版本号均为内部开发里程碑，仅用于团队跟踪进度，未对外发布。

## 功能概览

> **定位**：Silver Halide 对标 Lightroom Classic 功能密度，但以纯 C++ 高性能（大图零等待、常驻轻量）与本地离线 AI（隐私）为特色，数据契约自成体系、不兼容 Lightroom（[ADR-0001](docs/adr/0001-drop-lightroom-compat.md)）。

- **RAW 解码与线性工作流**：libraw 后端、相机颜色矩阵、线性 ProPhoto RGB (D50) 工作空间，sRGB 伽马由 lcms 在管线末端一次性应用

- **镜头/相机配置文件**：DCP 相机配置已完整接入渲染管线——按白平衡色温在双校准光源间插值颜色矩阵、应用色相/饱和度映射（ProfileHueSatMapData，三线性插值）与色调曲线，UI 可选择相机配置；Lensfun 镜头校正为可选 vcpkg 依赖，尚未接入

- **非破坏性编辑管线**：参数堆栈 + 缓存 + 预览 / 导出一致；曝光、对比度、高光 / 阴影、白平衡、曲线、HSL、色调分离、去朦胧、纹理 / 清晰度 / 锐化、畸变、颗粒、晕影、点颜色

- **局部调整**：污点去除（Clone / Heal，归一化坐标 + 羽化 + 边缘亮度匹配）、蒙版局部调整

- **目录与检索**：嵌入式 SQLite 目录（`Default.shcat` 目录即库），预览外置磁盘；虚拟副本、关键字层级、智能收藏夹（多谓词规则）、元数据编辑；数据契约以自有 `.lrp` 预设与 `.shcat` 目录自洽，不兼容 Lightroom

- **视图与比较**：胶片栏、主预览、Before / After 分屏、参考视图、1:1 全尺寸

- **导出**：JPEG / TIFF / PNG，按虚拟副本独立导出；批量导出（母版+所有虚拟副本）时可用模板重命名输出文件（`{date}/{index[:W]}/{base}`，冲突自动加序号）

- **导入**：添加 / 复制到目录 / 移动，按日期组织受管库；支持导入时按模板重命名（`{date}/{index[:W]}/{base}`），新文件名可预览

- **全景合成**：纯 C++ 实现（无 OpenCV 依赖），降采样空间域 NCC 对齐 + 柱面投影 + 行带羽化融合

- **AI 组件**：ONNX Runtime 后端，NAFNet 去噪、BiRefNet 主体分割、U²Net 天空近似蒙版；模型按需下载、SHA-256 校验、UI 管理。U²Net 是通用显著性模型，并非天空专用模型，天空结果不可用时仍回退到内置启发式蒙版

- **性能**：导入 / 预览 / 编辑 / 导出四段路径均有增量加速；coarse-then-refine 双路径预览；sqlite 语句复用；预览管线的逐级增量缓存以只读共享持有保留阶段（不再每级整幅深拷贝），1920 精帧单帧约 −17%~−33%（同机 A/B）

## 系统要求

- Windows 10 / 11 x64

- Visual C++ Redistributable（安装包内置）

- 至少 4 GB 可用磁盘空间（含 AI 模型缓存）

- 可选：网络连接用于首次下载 AI 模型

## 安装

下载 `Silver Halide Setup 1.0.0.exe`，双击运行安装程序。安装向导会显示 MIT 许可证（含中文参考译文），接受协议后选择安装目录即可完成。

安装程序已包含：

- `SilverHalide.exe` 主程序

- Qt 6 运行时 DLL 与插件

- ONNX Runtime

- VC++ 运行库安装器（按需自动安装）

- 应用图标

NAFNet 和 BiRefNet 模型文件随安装包一起分发，位于 `<安装目录>\models\`。U²Net 天空近似模型约 176 MB，不随安装包捆绑；可在「设置 → 模型管理」中按需下载，文件会保存到 `%LOCALAPPDATA%\Silver Halide\models\` 并进行 SHA-256 校验。

## 从源码构建

### 依赖

- CMake ≥ 3.25

- C++20 编译器（MSVC 2022 推荐）

- Ninja（推荐生成器）

- Qt 6.8.x（MSVC 2022 x64 包），路径形如 `qt/6.8.3/msvc2022_64`

- ONNX Runtime 1.23+（可选，启用 AI 推理）

- vcpkg（可选 Lensfun）：`C:\Users\tmlde\vcpkg`，安装目标为 `lensfun:x64-windows`

### GUI 构建

```bat
cmake -S . -B build_gui -G Ninja ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j
```

或直接使用 `build_x64.bat`。

### 无头构建与测试

```bat
cmake -S . -B build_headless -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure
```

> GPU/像素密集型阶段的可选 SIMD 快速路径（`gaussianBlur` AVX2 等）默认关闭，行为与标量基线一致。
> 需要启用时加 `-DLRC_ENABLE_SIMD=ON`（MSVC 自动加 `/arch:AVX2`）；结果与标量路径容差相近（Δ<0.008）而非逐位一致。

### 运行 GUI

```powershell
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime .\build_gui\SilverHalide.exe
.\build_gui\SilverHalide.exe
```

### 打包发布

```powershell
# 暂存 release 载荷到 build_gui/install_stage/
# 然后用 Inno Setup 编译 installer.iss
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" packaging\installer.iss
# 输出：dist\Silver Halide Setup 1.0.0.exe
```

## 架构

三层严格分层：

```
src/
├── engine/     # 纯 C++，Qt-free：RAW 解码、管线、颜色、缓存
├── catalog/    # 纯 C++，Qt-free：SQLite 目录、预览、检索
├── utils/      # 纯 C++，共享工具
├── app/        # 应用层（Constants、ModelManager 等）
└── ui/         # Qt 6 Widgets：MainWindow、DevelopPanel、Filmstrip 等
```

约定：`engine/` 与 `catalog/` 不得引入 Qt 头或依赖，必须能在 headless 构建中编译与测试。新增功能若是展示层优先放在 `ui/`；若是 RAW / 颜色 / 非破坏性编辑逻辑应落到 `engine/`。

## 文档索引

- 架构：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)

- AI 模型分发状态：[docs/archive/AI\_MODEL\_DISTRIBUTION\_STATUS\_2026-08-31.md](docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)

- 四阶段加速报告：[docs/archive/Speedup\_Four\_Stages\_Report.md](docs/archive/Speedup_Four_Stages_Report.md)

- 现代 C++ 审计与实施：[docs/spec/modern-cpp-adoption-audit.md](docs/spec/modern-cpp-adoption-audit.md)、[基线](docs/spec/modern-cpp-adoption-baseline.md)、[实施报告](docs/spec/modern-cpp-adoption-report.md)

- 开发经验与迁移：[docs/DEVELOPMENT\_EXPERIENCE.md](docs/DEVELOPMENT_EXPERIENCE.md)、[docs/MIGRATION.md](docs/archive/MIGRATION.md)

- 数据契约：[ADR-0001](docs/adr/0001-drop-lightroom-compat.md) 已移除 Lightroom 兼容读/写路径，产品以自有 `.lrp` 预设与 `.shcat` 目录自洽

- 内部开发历史归档（2.x dev 阶段）：[docs/archive](docs/archive)

## 仓库结构

```
.
├── AGENTS.md                  # AI 协作指南
├── build_x64.bat              # GUI 一键构建
├── build_gui/                 # Ninja GUI 构建树
├── build_headless/            # Ninja 无头构建树
├── CMakeLists.txt             # 源码根 = 工作区根
├── README.md                  # 本文件
├── CLAUDE.md                  # 构建 / 架构 / 编码约定
├── src/
├── tests/
├── resources/
│   ├── icons/app.ico
│   └── models/manifest.json
├── docs/
├── packaging/
│   ├── installer.iss          # Inno Setup 脚本（推荐）
│   ├── installer.nsi          # NSIS 脚本（历史参考）
│   ├── LICENSE.txt            # MIT 许可证（含中文参考译文）
│   └── lang/ChineseSimplified.isl
├── qt/                        # Qt 6.8.3 安装
└── onnxruntime/               # ONNX Runtime 安装
```

## 许可证

本项目基于 MIT License 发布，版权 © ASE-128 2026。完整许可证文本见 [packaging/LICENSE.txt](packaging/LICENSE.txt)，包含英文原版与开放原子开源基金会（OpenAtom Foundation）2023 年发布的中文参考译文（仅作参考之用，以英文原版为准）。

## 致谢

- libraw、lcms、sqlite、ONNX Runtime、Qt 等开源项目

- 开放原子开源基金会 MIT 许可证中文译本评审组（北京大学周明辉教授、华为开源法律专家沈芬律师等）

