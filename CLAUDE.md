# CLAUDE.md

此文件是 Claude Code 在此仓库中工作的当前指导文档，重点说明真实构建入口、架构边界和最新状态来源。

## 当前状态来源

最新的项目状态与核对材料在：

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md)（当前进行中的竞品优化任务）
- [STATUS.md](STATUS.md)
- [docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)
- [docs/archive/Speedup_Four_Stages_Report.md](docs/archive/Speedup_Four_Stages_Report.md)

历史的 PHASE 1–15、旧 speedup 状态、旧审计报告已归档到 [docs/archive](docs/archive)。

## 构建命令

> 源码根目录 = 工作区根目录 D:\Desktop\SilverHalide-src，活跃构建树位于根目录的 build_gui / build_headless。
> 旧的 lightroom-clone-cpp/build_gui 是历史残留（源码已上提至根目录后不存在），不应当作为当前打包、调试或自动化入口。

### GUI 构建

```bat
cmake -S . -B build_gui -G Ninja ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build_gui -j
```

包装脚本：

```bat
.\build_x64.bat
```

### 无头构建与测试

```bat
cmake -S . -B build_headless -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure
```

### 运行 GUI

```powershell
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build_gui\SilverHalide.exe
.\build_gui\SilverHalide.exe
```

## 架构边界

- engine / catalog / utils 必须保持 Qt-free
- UI 代码位于 src/ui
- 测试代码位于 tests

核心布局：

```text
src/
├── engine/
├── catalog/
├── ui/
├── app/
├── utils/
└── main.cpp
```

要求：

- 新增 RAW / 色彩 / 非破坏性编辑逻辑优先落在 engine
- 展示层逻辑优先落在 ui
- 新增单测优先放置于 tests 并注册到 tests/CMakeLists.txt

## 2026-09-05 当前实现概览

这次已落地的关键成果：

- AI 模型分发与 UI 管理：下载 / 解压 / SHA256 校验 / ONNX Runtime 推理 / 缓存目录管理
- NAFNet ZIP / external-data 兼容修复
- SHA256 余数块回归修复与单元测试
- Standard Preview v2 / preview pipeline 复用优化
- 导入 / 预览 / 修改 / 导出四阶段加速报告已完成并记录在 [docs/archive/Speedup_Four_Stages_Report.md](docs/archive/Speedup_Four_Stages_Report.md)
- 当前进行中的实施任务为 [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md)；P1/P2/P3/T 尚待按规格实施和验收
- 历史 PHASE 1–15 与旧 speedup 状态归入 [docs/archive](docs/archive)

## 关键文档

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)
- [docs/archive/Speedup_Four_Stages_Report.md](docs/archive/Speedup_Four_Stages_Report.md)
- [docs/spec/competitive-optimization-implementation-spec.md](docs/spec/competitive-optimization-implementation-spec.md)
- [docs/archive](docs/archive)

## 约定

- 默认使用 Ninja，尤其是根目录下的 build_gui / build_headless
- 若修改构建或依赖，优先同步 README / CLAUDE / docs 中的真实状态说明
- 不要把旧 VS 构建树或历史阶段文档当作当前真实状态的来源
- 优先用 headless 构建与测试作为快速反馈，UI 改动再补 GUI 验证
