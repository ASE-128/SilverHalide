# 开发经验总结（供迁移接手）

> 本文档由原开发环境（2026-08-17）整理，目标是让**另一台设备上的智能体**无需翻阅全部历史即可安全接手
> Silver Halide（`lightroom-clone-cpp`）。它是 CLAUDE.md（构建/架构/编码约定）的**经验补充**——
> 重点关注「踩过的坑」与「如何高效工作」，而非重复架构说明。
>
> 迁移配套文件：`docs/archive/MIGRATION.md`（目标环境重建步骤）、`docs/spec/competitive-optimization-implementation-spec.md`（当前进行中的竞品优化任务）。

## 1. 项目快照（当前状态）

| 项 | 值 |
|---|---|
| 应用名 | **Silver Halide**（`src/app/Constants.h` 的 `kAppName`），版权 © ASE-128 2026 |
| 版本 | **v1.0.0**（`Constants.h` / `CMakeLists.txt` / `app.rc` / `installer.iss` / `installer.nsi` 五处同步；先前的 2.x 版本号均为内部开发 dev 里程碑，未对外发布） |
| 工作空间 | 线性 **ProPhoto RGB (D50)**；所有调整在线性光下运算，sRGB 伽马最后由 lcms 应用一次 |
| 测试基线 | headless + GUI 双端 **272 用例 / 274373 断言**全绿（AUDIT-2026-08-14 验收）；PHASE_1 降噪后为 **277 用例 / 274420 断言** |
| 架构 | 三层：`engine/`（纯 C++ 无 Qt）+ `catalog/`（纯 C++）+ `ui/`（Qt6 Widgets），`utils/` 共享 |
| 调整顺序 | 曝光 → 对比度 → 高光 → 阴影 → 白/黑 → 色调曲线 → 白平衡 → 去朦胧 → 鲜艳度 → HSL → 色调分离 → 点颜色 → 空间阶段（纹理/清晰度/锐化/畸变/颗粒/晕影/蒙版局部）→ sRGB |
| 进行中工作 | **竞品对齐优化规格待实施**（详见 `docs/spec/competitive-optimization-implementation-spec.md`）；性能 M1-M5 为历史实施记录，部分量化验收仍需单独复核 |

测试断言计数有两个口径：272 是 AUDIT 验收基线，277 是 PHASE_1 降噪（5 新用例）之后的数。写 STATUS.md 时以实际跑出的为准。

## 2. 目录结构速览（哪些是源码、哪些可丢弃）

```
./                            ← 工作区根 = 源码根（源码已从 lightroom-clone-cpp/ 上提至此处）
├── build_gui/                  ← 活跃 GUI 构建树（Ninja，可重新生成，不迁移）
├── build_headless/             ← 活跃无头构建树（Ninja，可重新生成，不迁移）
├── build_headless_m3/          ← 历史残留构建树（不迁移）
├── qt/6.8.3/msvc2022_64/       ← Qt 6.8.3 安装（2G，目标设备需重新安装，不迁移）
├── *.bat / *.ps1               ← 构建/测试/部署包装脚本（迁移）
├── CMakeLists.txt              ← 主构建脚本（迁移）
├── src/                        ← 全部源码（迁移）
├── tests/                      ← Catch2 测试（迁移）
├── docs/                       ← 架构/里程碑/状态文档（迁移；含本次新增经验文档）
├── third_party/                ← ★ vendored 依赖源码（libraw/lcms2/sqlite/stb/pugixml/Catch2，迁移）
├── resources/                  ← 图标、QSS、app.rc（迁移）
├── packaging/                  ← NSIS 安装脚本（迁移）
├── dist/                       ← 打包输出（可重新生成，不迁移）
└── harness/                    ← 早期调试工具（保留 Parallel.cpp/main.cpp/build*.bat，产物不迁移）
```

> **重要**：仓库内曾经的 `lightroom-clone-cpp/build_gui`（带 `Release/` 层）是 VS 生成器的**废弃旧树**，源码上提后已不存在；
> 活跃构建树是工作区根的 `build_gui`（Ninja，无 Release 层）。打包/调试时切勿重建或指向旧树。
> 早期规范草稿（如 `lightroom-clone-cpp.md`）含过时内容（如 output_color=0），仅供参考。

### vendored `third_party` 必须保留
CMake 的依赖策略是「优先 vendored、其次 FetchContent」。虽然 CMake 也能下载依赖，但：
- 本项目 libraw 有**针对性配置**（`output_color=4` 路径，见 §5.1），FetchContent 版行为未经本项目验证；
- 中国网络下 FetchContent 需 `-DLRC_PROXY=https://ghfast.top/` 代理，下载慢且易断。

因此**迁移包必须包含 `third_party/`**，不要为了省体积删它（38M）。

## 3. 构建 / 测试 / 打包命令（新设备复现用）

> 全部命令须在 **x64 MSVC 环境**（`vcvars64.bat`）下执行。详细说明见 CLAUDE.md 与 MIGRATION.md。

```bat
:: ① 完整配置 + 构建 GUI（内联 vcvars64.bat）→ build_gui\SilverHalide.exe
.\build_x64.bat

:: ② 增量重建 GUI（-j4 并行；-j8 内存不足会批量失败）
.\_build_gui4.bat

:: ③ 无头构建（仅引擎 + 目录测试，无 Qt）
cmake -S . -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j

:: ④ 跑测试（GUI 层建议直跑 exe，中文路径下 ctest 假卡死，见 §4.7）
.\_run_gui_tests.bat     :: 跑 build_gui\tests\lrc_tests.exe，输出 GUI_TESTS_EXIT
.\_run_smoke.ps1         :: GUI 启动冒烟：等主窗口句柄 → WM_CLOSE → 退出码 0
ctest --test-dir build_headless --output-on-failure   :: headless 层 ctest 稳定

:: ⑤ 部署 Qt 运行时（关键：解决「Qt 平台插件缺失」错误）
qt/6.8.3/msvc2022_64/bin/windeployqt.exe --release --no-translations --compiler-runtime build_gui/SilverHalide.exe

:: ⑥ 打包 NSIS 安装包 → dist\Silver Halide Setup <ver>.exe
C:\Users\tmlde\Desktop\dev\nsis\makensis.exe packaging\installer.nsi
```

### 使用代理（中国网络，仅 FetchContent 需要）
```bat
cmake -S . -B build_gui -G Ninja -DLRC_PROXY=https://ghfast.top/ ^
      -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release
```

## 4. 构建 / 运行期「坑」清单（最易踩，务必先读）

### 4.1 MSVC 必须用 x64 工具链
配置必须 `call vcvars64.bat`。若用默认 `Hostx86/x86` cl.exe 配置，会得到 32 位 lib，无法链接 64 位 Qt
（报 DLL/插件错误）。检查产物位数：`file build_gui/SilverHalide.exe` → 应为 `PE32+ x86-64`。

### 4.2 `/EHsc` 必须全局开启
根 `CMakeLists.txt` MSVC 块已 `add_compile_options(/EHsc)`。缺失时 C4530，`try/catch` 不生效，
`utils/Parallel.cpp` 的异常守卫失效 → 渲染异常直接 terminate / 退出卡死。**不要删除这行**。

### 4.3 Release 构建须核对优化标志
`CMAKE_CXX_FLAGS_RELEASE` 应为 `/O2 /Ob2 /DNDEBUG`。若 CMakeCache 丢失/重配后为空，Release 会退化为
无优化——多线程测试暴露时序竞态、GUI 退出卡死、`--order lex` SIGSEGV（M3 恢复时踩过）。验收前核对：
`grep CMAKE_CXX_FLAGS_RELEASE build_gui/CMakeCache.txt`，为空则删缓存重配。

### 4.4 windeployqt 部署 + VC redist 手动拷贝
- 构建后**必须**跑 windeployqt，否则 exe 启动即「Qt 平台插件」/DLL not found 错误。
- windeployqt **不保证**拷贝 VC redist（msvcp140/vcruntime140 缺失）。需手动从
  `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT\*.dll`
  拷入 `build_gui\`。打包路径：先把 exe+DLL+插件（platforms/imageformats/iconengines/generic/
  networkinformation/styles/tls 等 7 个插件目录）暂存到 `build_gui\install_stage\`，再跑 makensis。

### 4.5 exe 位置：Ninja 无 Release 层
Ninja 生成器下 exe 直接位于构建根（`build_gui\SilverHalide.exe`），**不是** `build_gui\Release\`。
旧文档指向 `Release\` 是 VS 生成器时代遗留，勿照抄。

### 4.6 中文目录名 + .bat 编码陷阱
- 工作区根是 `C:\Users\tmlde\Desktop\dev\lrc仿制`。cmd 按 GBK(936) 读 `.bat`，UTF-8 的 `.bat` 里硬编码
  中文路径会乱码。**禁在 .bat 硬编码中文路径**；用 `%~dp0` 相对定位，或经环境变量传路径
  （`export LRC_ROOT="C:/Users/tmlde/Desktop/dev/lrc仿制"`，bat 内引用 `%LRC_ROOT%\...`）。
- Write 工具写的 `.bat` 是 LF 行尾，cmd 会误解析（尤其 `>nul` 重定向），LF 时先转 CRLF：
  `sed -i 's/$/\r/' script.bat`。LF 脚本可能静默跳过 vcvars64 → cl.exe 找不到头文件。
- 后台任务会被环境 killed，构建/测试须前台跑。

### 4.7 ctest 在中文路径下不稳 → 直跑 exe
中文路径下 ctest 的进程管理会假卡死/假 SIGSEGV。GUI 层测试**直接运行** `build_gui\tests\lrc_tests.exe`
（`_run_gui_tests.bat` 已配 PATH + QT_PLUGIN_PATH）。headless 层 ctest 稳定可用。

### 4.8 并行度 -j4
Ninja 构建用 `-j4`。`-j8` 内存不足会批量失败（GUI 目标含 Qt 链接）。

### 4.9 Qt 6.8 特有小坑
`QListWidgetItem` 无单参 `QIcon` 构造（`FilmstripWidget.cpp` 曾因此 GUI 独有编译错误），需
`QIcon(), QString()` 双参。headless 构建不编 UI，这类错误只在 GUI 构建暴露——**UI 改动必须双端构建验证**。

## 5. 关键架构决策与「勿再改回」的修复

### 5.1 RAW 颜色：libraw `output_color=4` + lcms D50 适配（勿回退！）
曾用 `output_color=0` + 手动 `cam_xyz` 矩阵转 ProPhoto，但 `cam_xyz` 是白平衡前矩阵、方向/白点与项目
假设（D65）不符 → RAW 荧光/彩虹伪彩色（2026-08-15 修复）。现路径：
- `RawDecoder.cpp` 用 `output_color=4`（libraw 官方线性 ProPhoto，D65 白点）；
- `ColorManager::fromDcrawProPhoto()` 经 lcms2 把 libraw ProPhoto 白点重新锚定到工作空间 **D50** ProPhoto
  （`dcrawToProPhoto_` transform）。
- **经验**：RAW 颜色问题一律走 libraw 官方输出 + lcms 白点适配，**不要再手动拼 cam_xyz**。

### 5.2 线程模型与 Parallel
- 全局惰性线程池 `utils/Parallel`：`parallelFor(n, grain, fn)` 分块并行；嵌套安全（池线程内退化为串行）；
  `LRC_THREADS` 覆盖线程数。**核心渲染/解码阶段**都走它。
- `ColorManager` 的 lcms transform 以 `cmsFLAGS_NOCACHE` 构建，`shared_mutex` 保护：转换可并发
  （shared lock），`loadDisplayProfile` 重建时独占。
- 跨线程数据经 `ImageBufferPtr`（shared_ptr），注册为 Qt 元类型。

### 5.3 缓存设计
- 预览 LRU：键 `(imageId, adjHash, maxDim)`，256MB，约 40 张 1080p。
- coarse/refine 双槽：拖动 30ms 节流 480px（`adjustPreviewCoarse`）+ 停顿 50ms 1920px，共用代次守卫丢弃旧结果。
- 缩略图/1:1 磁盘缓存 + 目录内嵌 `previews` 表（320px JPEG + 1:1 JPEG BLOB）。

### 5.4 其他历史修复（避免重复踩）
- 高光/阴影曾声明但未被 `applyAdjustments` 应用（静默忽略）——改动参数结构必须核对管线全链路。
- `ImageIO::writeTiffRaw` IFD 回填索引错位（`ifd[4]` 实为 tag 262）→ 导出 TIFF/DNG 全畸形，已修 `ifd[5]`。
- 退出竞态：`stopWorkers` 先 `requestAbort` 再 `quit()+wait`，worker 加 `std::atomic` abort 标志，防 UAF。
- 蒙版画笔权重场 COW 缓存（`Pipeline::brushWeightField`，键 geometryHash+w+h，~128MB LRU），拖局部滑块不重建。

## 6. 当前任务：竞品对齐优化规格

当前任务文件为 **`docs/spec/competitive-optimization-implementation-spec.md`**。它定义 P1/P2/P3 三波优化和 T 技术债的实施路径、依赖关系及验收标准。

- **当前状态**：P1、P2、P3、T 尚未按该规格正式启动。现有 AI 天空蒙版、编辑历史、SIMD 条件编译等只能作为基础能力，不能替代规格中的完整实现和验收。
- **首批范围**：P1-1 DirectML、P1-2 导出 ICC、P1-3 批量重命名；实施后必须补齐 headless/GUI 测试与性能或行为证据。
- **后续批次**：P1-5 快照系统、P2 核心能力、P3 体验打磨和 T 技术债按规格中的依赖链推进。
- **历史依据**：性能 M1-M5 实施规格和四阶段加速报告位于 `docs/archive/`，仅用于回溯，不作为当前进行中任务清单。

## 7. 工作方式偏好（原环境确认的协作纪律）

这些是用户明确要求的工作方式，新设备接手的智能体应延续：

1. **批处理编译**：不要每改一个文件/每个阶段就编译。攒一批相关修改（整个 feature 或 2–3 个 feature）后
   **统一编译一次**。分阶段大任务（如 PHASE 系列）在「整个部分完成后再编译」，子代理只做静态自查。
2. **主代理只分发不写码**：实施阶段主代理只做分发、构建、跑测试、验收、更新文档；所有源码改动由子代理
   （按「一文件一写者」切分）完成。构建失败 → 定位后重新下发对应子代理修复。
3. **STATUS.md 保持同步**：每完成一个连贯批次就更新 `STATUS.md`（测试数、构建状态、
   新特性、验收标准）。文档曾长期滞后于现实，务必避免。
4. 内存式进度（`docs/Speedup_Mx_STATUS.md`、`docs/PHASE_1_STATUS.md`）是主代理维护的持久化载体，实时更新。

## 8. 编码规范速查（易忘项）

- 命名空间 `lrc`；engine/catalog 文件 `#pragma once` 且**禁止包含 Qt 头文件**。
- MSVC：引擎 `/W4 /wd4100 /wd4244 /wd4996`，应用/测试 `/W3`；`NOMINMAX`、`_CRT_SECURE_NO_WARNINGS`；
  `LRC_ENGINE_SOURCES` 加 `/bigobj`。
- **中文注释行必须以 ASCII 结尾**（`. ` `,` `)` 等）：工程未设 `/utf-8`，MSVC 按 GBK 读源码，行末汉字/
  全角标点的 UTF-8 末字节（0x81–0xBF）被当 GBK 前导字节与换行配对 → 吞掉换行、把下一行代码注释掉，
  报 `C2530 references must be initialized` 之类费解错误。写完中文注释逐行自查是否以 ASCII 结尾。
- 新增 `.cpp` 必须在 CMakeLists（`LRC_ENGINE_SOURCES` 等）或 `tests/CMakeLists.txt` 登记。
- 测试文件命名 `Test<类>.cpp`，用 Catch2 v3 `Catch2::Catch2WithMain`，不得依赖 Qt。

## 9. 验证清单（新设备首次构建后跑一遍）

1. headless 全量：`ctest --test-dir build_headless --output-on-failure` → 全绿。
2. GUI 构建：`.\_build_gui4.bat` → `build_gui\SilverHalide.exe` 生成。
3. GUI 测试：`.\_run_gui_tests.bat` → `GUI_TESTS_EXIT=0`，用例数与 headless 一致。
4. 冒烟：`.\_run_smoke.ps1` → 启动、等窗口、WM_CLOSE、退出码 0。
5. 打包冒烟（可选）：windeployqt + makensis → 安装包可生成。
6. 核对 `CMAKE_CXX_FLAGS_RELEASE` 含 `/O2 /Ob2 /DNDEBUG`（§4.3）。
