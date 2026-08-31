# Silver Halide

Silver Halide 是一个基于 C++20 + Qt 6 的 RAW / 摄影工作流桌面应用，聚焦 RAW 解码、非破坏性编辑以及高性能预览/导出链路。

## 当前状态

当前仓库的真实状态应以以下文档为准：

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](docs/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)
- [docs/Speedup_Four_Stages_Report.md](docs/Speedup_Four_Stages_Report.md)

这些文档反映了 2026-08-31 之后的最新实现状态。历史的 PHASE 1–15、旧 speedup 里程碑、审计报告已归档到 [docs/archive](docs/archive)。

## 关键能力

- RAW 解码与线性工作流：libraw、相机矩阵、ProPhoto / sRGB 转换
- 非破坏性编辑管线：参数堆栈、缓存、预览 / 导出一致性
- Qt 6 界面层：导航、胶片栏、主预览、开发面板
- 模型分发与 AI 组件：ONNX Runtime 下载、解压、校验、UI 管理
- 性能优化：按导入 / 预览 / 编辑 / 导出四段路径进行增量加速

## 实际构建入口

本仓库在 Windows 上的有效生成树位于工作区根目录：

- [build_gui](build_gui)
- [build_headless](build_headless)

请勿再把旧的 [lightroom-clone-cpp/build_gui](lightroom-clone-cpp/build_gui) 当作当前打包或调试入口。

### GUI 构建

```bat
cmake -S lightroom-clone-cpp -B build_gui -G Ninja ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j
```

或直接使用：

```bat
.\build_x64.bat
```

### 无头构建与测试

```bat
cmake -S lightroom-clone-cpp -B build_headless -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure
```

### 运行 GUI

```powershell
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime .\build_gui\SilverHalide.exe
.\build_gui\SilverHalide.exe
```

## 结构说明

```text
.
├── AGENTS.md
├── build_gui/
├── build_headless/
├── build_x64.bat
├── lightroom-clone-cpp/
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── CLAUDE.md
│   ├── src/
│   ├── tests/
│   ├── docs/
│   │   ├── ARCHITECTURE.md
│   │   ├── AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md
│   │   ├── Speedup_Four_Stages_Report.md
│   │   └── archive/
│   └── packaging/
├── qt/
├── onnxruntime/
└── ...
```

## 文档索引

- 架构：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- AI 模型分发状态：[docs/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](docs/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)
- 四阶段加速报告：[docs/Speedup_Four_Stages_Report.md](docs/Speedup_Four_Stages_Report.md)
- 历史归档：[docs/archive](docs/archive)

## 重要约定

- engine / catalog / utils 应保持 Qt-free
- UI 代码位于 src/ui
- 测试代码位于 tests
- 历史 PHASE 文档和旧 speedup 记录已归档，不再作为当前主状态的唯一来源
- 优先用 headless 构建与测试作为快速反馈，GUI 改动再补 GUI 验证

## 许可证与声明

Silver Halide 由 ASE-128 维护，版权声明以当前源代码和安装包中的版本信息为准。
