# 迁移说明（MIGRATION）

本文件指导把 Silver Halide 项目迁移到**另一台设备（Windows 11 x64）**并交给另一个智能体继续开发。

## 1. 迁移包内容

压缩包包含（源码 + 文档 + 脚本 + vendored 依赖）：

```
SilverHalide-src/  (解压目录名)
├── lightroom-clone-cpp/          ← 项目根（核心）
│   ├── CMakeLists.txt  src/  tests/  resources/  packaging/
│   ├── third_party/              ← ★ vendored 依赖（libraw/lcms2/sqlite/stb/pugixml/Catch2），必须保留
│   ├── docs/                     ← 架构/里程碑/状态 + 新增：DEVELOPMENT_EXPERIENCE.md、MIGRATION.md、plans/
│   ├── CLAUDE.md  README.md  STATUS.md  AGENTS.md  .gitignore  vcpkg.json
│   └── harness/Parallel.cpp  harness/main.cpp  harness/build*.bat   (调试工具源码)
├── setup_environment.ps1         ← ★ 一键环境安装脚本（Windows 11，见 §3）
├── build_x64.bat  _build_gui4.bat  _deploy.bat  _run_gui_tests.bat  …  (构建/测试/部署脚本)
└── AGENTS.md  lightroom-clone-cpp.md  run_app.ps1 …
```

### 刻意未包含（需在目标设备重建/重装）
| 目录/文件 | 原因 | 重建方式 |
|---|---|---|
| `qt/6.8.3/msvc2022_64/`（2G） | Qt 安装目录 | §3 步骤 4（aqtinstall） |
| `build_gui/` `build_headless/` `build_headless_m3/` | 构建产物 | §4 重建 |
| `lightroom-clone-cpp/build_gui|build_headless|build_asan|dist/` | 废弃旧树 + 打包输出 | §4/§5 重建 |
| 各 `.obj` / `.log` / harness 产物（.exe/.pdb/.ilk） | 编译中间产物 | 重建时生成 |

## 2. 目标设备环境要求（Windows 11 x64）

| 依赖 | 版本 | 用途 | 安装方式（推荐） |
|---|---|---|---|
| **Visual Studio 2022** | Community | MSVC 工具链（含 CMake/Ninja 组件） | §3 步骤 1 |
| **Windows 11** | x64 | 系统（winget 内置可用） | 已就绪 |
| **CMake** | ≥ 3.25 | 构建 | §3 步骤 2 |
| **Ninja** | 最新 | 生成器 | §3 步骤 2 |
| **Qt** | **6.8.3**（msvc2022_64 预编译） | GUI 框架 | §3 步骤 4（aqtinstall） |
| Python 3.x（可选） | ≥ 3.9 | `resources/tools/make_icons.py`、`packaging/make_bmp.py` | §3 步骤 3 |
| NSIS 3（可选） | 最新 | 打包安装器 | §3 步骤 5 |

可选：vcpkg（本项目 CMake 已内置 vendored 依赖策略，**不装也能构建**；`vcpkg.json` 仅为记录依赖清单）。

> **兼容性说明**：本项目在 Windows 10 19045 开发并验收，目标 Windows 11 无已知不兼容点
> （纯 C++20 + Qt6，无系统版本相关 API 依赖）。若首次构建报错，优先核对 §4 的检查清单。

## 3. 环境安装（Windows 11，约 10–50 分钟）

### 方式 A：一键脚本（推荐）

随包提供 `setup_environment.ps1`。**以管理员身份**打开 PowerShell / Terminal：

```powershell
# 在解压目录内，管理员 PowerShell：
powershell -ExecutionPolicy Bypass -File setup_environment.ps1
```

脚本依次执行：VS2022（C++ 工作负载）→ CMake + Ninja → Python（可选）→ Qt 6.8.3 → NSIS（可选）。
VS2022 是耗时大头（10–40 分钟，可能需要重启）；Qt 及以下很快。

### 方式 B：手动逐步（便于分步执行/排查）

```powershell
# 1) Visual Studio 2022 Community + 「使用 C++ 的桌面开发」工作负载
#    也可用 VS Installer 图形界面勾选 "Desktop development with C++"（含 MSVC x64、CMake、Windows SDK）
winget install --id Microsoft.VisualStudio.2022.Community --silent `
  --accept-source-agreements --accept-package-agreements `
  --override "--wait --quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"

# 2) CMake + Ninja
winget install --id Kitware.CMake --silent --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja --silent --accept-source-agreements --accept-package-agreements

# 3) Python（可选：图标/位图生成脚本用）
winget install --id Python.Python.3.12 --silent --accept-source-agreements --accept-package-agreements

# 4) Qt 6.8.3 MSVC 2022 x64 预编译（aqtinstall 官方工具）
python -m pip install --upgrade aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O qt
#    产出 .\qt\6.8.3\msvc2022_64\（含 bin\windeployqt.exe），
#    与后续 cmake 的 -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 一致（相对解压根）。

# 5) NSIS（可选：打包安装器）
winget install --id NSIS.NSIS --silent --accept-source-agreements --accept-package-agreements
```

### 安装后验证

```bat
:: 新开一个终端验证（PATH 需含新装的工具；VS 装完可能要先重启）
cmake --version        :: >= 3.25
ninja --version        :: 有版本号即可
cl                      :: 显示 MSVC 帮助/版本（x64 开发者命令行里）
dir qt\6.8.3\msvc2022_64\bin\qmake.exe   :: Qt 就位
```

> 安装路径约定：Qt 装在**解压根**的 `qt/` 下（与迁移前目录结构一致）。若装到别处，
> 后续 cmake 命令把 `-DCMAKE_PREFIX_PATH` 指到对应路径即可。

## 4. 首次构建（目标设备）

```bat
:: 0) 打开「x64 Native Tools Command Prompt for VS 2022」（开始菜单）
::    （或先 call vcvars64.bat；本项目未设 /utf-8，仓库已按「中文注释以 ASCII 结尾」规范书写，可直接编译）

:: 1) 完整构建 GUI + 测试（内联 vcvars64）→ build_gui\SilverHalide.exe
.\build_x64.bat

:: 2) 无头构建（引擎 + 目录测试）→ 主回归，最快
cmake -S lightroom-clone-cpp -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j4
ctest --test-dir build_headless --output-on-failure

:: 3) GUI 层测试直跑 exe（中文路径下 ctest 不稳）
.\_run_gui_tests.bat
.\_run_smoke.ps1
```

**首次构建若失败，先核对**（详见 `docs/DEVELOPMENT_EXPERIENCE.md` §4）：
- `CMAKE_CXX_FLAGS_RELEASE` 含 `/O2 /Ob2 /DNDEBUG`；
- 用的是 x64 工具链（`file build_gui/SilverHalide.exe` 为 `PE32+ x86-64`）；
- `build_x64.bat` 用 `%~dp0` 定位、未硬编码中文路径；
- 网络受限时依赖下载用代理（但 vendored 依赖已内置，通常无需网络）。

## 5. 打包（NSIS，可选）

```bat
:: 部署 Qt 运行时到 build_gui
qt/6.8.3/msvc2022_64/bin/windeployqt.exe --release --no-translations --compiler-runtime build_gui/SilverHalide.exe
:: 手动拷贝 VC redist（windeployqt 不保证）：
::   C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT\*.dll → build_gui\
:: 暂存发布载荷到 build_gui\install_stage\（exe + *.dll + 7 个插件目录）
:: 编译安装包（NSIS 装好后在 PATH 中，或指定 makensis.exe 路径）
makensis packaging\installer.nsi   →  dist\Silver Halide Setup <ver>.exe
```

## 6. 接手后的第一件事（给接手的智能体）

1. **必读**：`lightroom-clone-cpp/CLAUDE.md`（构建/架构/编码约定）+ `docs/DEVELOPMENT_EXPERIENCE.md`
   （经验与坑）+ `docs/ARCHITECTURE.md`。
2. **当前状态**：`STATUS.md` 顶部 + `docs/PHASE_1_STATUS.md`。测试基线 277 用例（PHASE_1 后）。
3. **进行中工作**：性能优化 **M4/M5 未启动**，计划见 `docs/plans/perf-m1-m5-plan.md`。
4. **协作纪律**：批处理编译（攒批再构建）、主代理只分发不写码（一文件一写者）、STATUS.md 实时同步
   ——见 `docs/DEVELOPMENT_EXPERIENCE.md` §7。
5. **验证**：按 §4 跑一遍构建与测试确认环境健康，再开始改代码。

## 7. 附：原环境路径备忘（Windows 10 开发机，新设备可忽略，仅参考）

- 原工作区：`C:\Users\tmlde\Desktop\dev\lrc仿制`
- 原 NSIS：`C:\Users\tmlde\Desktop\dev\nsis\makensis.exe`
- 原 Claude 计划：`C:\Users\tmlde\.claude\plans\`（关键计划已随迁到 `docs/plans/`）
- 原记忆目录：`C:\Users\tmlde\.claude\projects\...\memory\`（反馈/经验已提炼进 `docs/DEVELOPMENT_EXPERIENCE.md`）
