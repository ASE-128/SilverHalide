# AGENTS.md

这是 Silver Halide 的仓库级 AI 指南。该项目是一个以 C++20 + Qt 6 为核心的 RAW/摄影工作流桌面程序，代码主入口在 [CMakeLists.txt](CMakeLists.txt)。

## 1. 关键事实

- 源码根目录 = 工作区根目录： [d:\Desktop\SilverHalide-src](.)
- 活跃构建树位于工作区根： [build_gui](build_gui)（含 AI）与 [build_headless](build_headless)；`build_gui_noai`、`build_headless_ai` 是 AI 开关对照树，`build_mask`、`build_t1`…`build_t9` 是历史试验树，不要视作当前构建入口
- 旧的 VS 产物树（曾位于源子目录 `lightroom-clone-cpp/build_gui`）是历史残留，已随源码上提清理，打包/调试/自动化不要再建此类旧树
- Qt 安装路径： [qt/6.8.3/msvc2022_64](qt/6.8.3/msvc2022_64)
- ONNX Runtime 预置目录： [onnxruntime/onnxruntime-win-x64-1.23.2](onnxruntime/onnxruntime-win-x64-1.23.2)
- 主要文档： [README.md](README.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), [CLAUDE.md](CLAUDE.md), [STATUS.md](STATUS.md)

## 2. 确认进行中任务和状态

每次开始工作前，先从实时状态判断任务是否仍在进行；不要仅凭历史里程碑文档推断当前状态：

1. 执行 `git status --short --branch` 和 `git log -n 10 --date=iso --format="%h %ad %s"`，记录当前分支、未提交文件和最近提交。
2. 检查 `docs/plans/` 中未归档的计划（当前无未归档计划，目录为空或不存在即视为无）；其中标记为 completed 的计划不应当重新执行。
3. 将 `docs/archive/` 视为历史证据，不把其中的 PHASE、M 里程碑或旧测试数字直接当作当前任务。
4. 若 Git、构建/测试结果和文档冲突，以可复现的 Git 状态与实测结果为准，并在回复中明确冲突和日期。
5. 在报告任务状态时区分 `已提交`、`工作区未提交`、`代码已落地但未验收`、`待实施`，不要把“有代码框架”写成“已完成”。

当前核对快照：分支 `main`。2026-11-17 已提交 no-AI 构建修复（AI 条件编译 `#ifdef` → `#if`，涉及 `src/engine/Pipeline.cpp`、`src/ui/DevelopPanel.cpp/.h`、`src/ui/MainWindow.cpp`）与文档收口（本文件、`README.md`、`STATUS.md`、三份 P-A 里程碑文档、`docs/DEVELOPMENT_EXPERIENCE.md` §11）；2026-11-18 完成文档与遗留收口 + 功能补强（预分级角标/智能筛选、蒙版高亮修复、B-2 预分级落库修复）与 kimi-cu 真机验收。最终 HEAD 以 `git log -n 1` 为准。此前记录的现代 C++ 改造（IM-1 ~ IM-11）、D3D11 与性能诊断、AI 模型懒加载、Lightroom 工作流均已提交，验收数字与遗留见 [docs/spec/modern-cpp-adoption-report.md](docs/spec/modern-cpp-adoption-report.md) 与 [STATUS.md](STATUS.md)。`docs/plans/` 当前无未归档计划（lensfun 计划已归档到 `docs/archive/`）。`dist/` 现有 `Silver Halide Setup 1.0.0.exe`（含 AI，约 980.9 MB）与 `Silver Halide Setup 1.0.0 (No AI).exe`（约 406.8 MB），旧记录里“缺 AI 安装包”的遗留项已解决。`build_headless`（`LRC_ENABLE_AI=OFF`）2026-11-17 本次实测：重跑 `_fix_msvc_deps.ps1` 后 `Pipeline.cpp.obj` 为 `#deps 14 (VALID)`，全量 clean 重建 304 步成功，`ctest` 1/1 Passed，524 test cases / 979,265 assertions 全通过（519 / 979,236 为 2026-10-26 的旧记录，两者不合并）。性能锚点的逐条真机状态见 [STATUS.md 的「性能锚点实测状态」唯一表](STATUS.md)，不再以“M1-M5 全部待验”概括。注意每次 configure 后仍需重跑 `_fix_msvc_deps.ps1`；`LRC_ENABLE_AI` 恒被定义为 0/1，必须用 `#if`（见 `docs/DEVELOPMENT_EXPERIENCE.md` §11）。开始新任务前必须重新执行上述检查，不能把本快照当作永久状态。

状态参考： [STATUS.md](STATUS.md)、[docs/archive/perf-m1-m5-implementation-spec.md](docs/archive/perf-m1-m5-implementation-spec.md)、[docs/archive/Speedup_Four_Stages_Report.md](docs/archive/Speedup_Four_Stages_Report.md)、[docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](docs/archive/AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)。

## 3. 真实可用的构建与测试命令

在 PowerShell / cmd 下，先确保使用 x64 MSVC 环境：

- GUI 全量构建：
  - [build_x64.bat](build_x64.bat)
  - 等价手写： `cmake -S . -B build_gui -G Ninja -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_AI=ON -DONNXRUNTIME_ROOT=<绝对路径>\onnxruntime\onnxruntime-win-x64-1.23.2`
  - 然后： `cmake --build build_gui -j`
  - 漏传 `ONNXRUNTIME_ROOT` 时 CMake 只打印一行 STATUS，然后 `FORCE` 把 `LRC_ENABLE_AI` 改成 `OFF`，会静默得到无 AI 构建

- Headless / 引擎测试构建：
  - `cmake -S . -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF`
  - `cmake --build build_headless -j`
  - `ctest --test-dir build_headless --output-on-failure`

- 单测/分组测试（Catch2 标签，可执行文件在 build_headless\tests\ 下）：
  - `.\build_headless\tests\lrc_tests.exe "[pipeline]"`、`"[incremental]"`、`"[performance][pa]"` 等标签可只跑相关用例；全量仍用 `ctest`

- GUI / Qt 测试：
  - `build_gui/tests/lrc_qt_tests.exe` 是 Qt 层测试（真实导出、对话框 smoke），用 [run_qt_tests.bat](run_qt_tests.bat) 跑，脚本已注入 vcvars 与 Qt bin 的 PATH
  - `build_gui/tests/lrc_tests.exe` 是同一套 Catch2 单测；不要把测试当成稳定的 ctest 入口，仓库文档中已记录中文路径/进程管理在某些情况下会出现假卡死
  - 也可参考 [CLAUDE.md](CLAUDE.md) 或脚本 [run_app.ps1](run_app.ps1)

- 运行应用：
  - `qt/6.8.3/msvc2022_64/bin/windeployqt.exe --release --no-translations --compiler-runtime build_gui/SilverHalide.exe`
  - `build_gui/SilverHalide.exe`

- GUI 操作验收：
  - 改动 UI 行为后，除编译和无头测试外，必须用 kimi-cu 计算机操作 MCP 真实跑一遍受影响的交互，步骤见第 9 节

## 4. 架构边界

- 引擎与目录层必须保持 Qt-free：
  - [src/engine](src/engine)
  - [src/catalog](src/catalog)
  - [src/utils](src/utils)
- UI 代码位于：
  - [src/ui](src/ui)
- 测试代码位于：
  - [tests](tests)

约定：
- `engine/` 与 `catalog/` 不应引入 Qt 头/依赖；它们需要能在 headless 构建中编译和测试。
- 新增功能若是展示层，优先放在 UI；若是处理 RAW/颜色/非破坏性编辑逻辑，应落到 engine/
- 新增单测优先放到 [tests](tests) 并在 [tests/CMakeLists.txt](tests/CMakeLists.txt) 中注册

## 5. 代码与构建约定

- CMake 版本：3.25+
- 标准：C++20，关闭扩展语言
- 生成器：默认使用 Ninja，尤其是当前工作区中所用的根构建树
- 构建选项（均来自 CMakeLists.txt 的 `option()`）：
  - `LRC_BUILD_TESTS=ON`（默认 ON）
  - `LRC_ENABLE_QT=ON`（默认 ON；`OFF` 即无头构建）
  - `LRC_ENABLE_AI=ON`（默认 ON，但找不到 ONNX Runtime 会被强制改 OFF；需 `-DONNXRUNTIME_ROOT=<解压根目录>` 或 vcpkg）
  - `LRC_ENABLE_D3D=ON`（默认 ON；Windows 专有，其他平台强制 OFF，运行失败自动回退 CPU）
  - `LRC_ENABLE_SIMD=OFF`（默认 OFF；开启需 AVX2，与标量路径逐位一致）
  - `LRC_ENABLE_LENSFUN=ON`（默认 ON，镜头校正配置）
  - `LRC_USE_OPENCV=OFF`（默认 OFF，重依赖）
  - `LRC_PROXY=https://ghfast.top/` 可用于国内镜像/代理场景
- 本仓库对依赖的处理首选：已安装 package / vendored third_party / FetchContent；不要在不必要时改动依赖获取方式

## 6. 重要坑位

- 不要编辑生成目录中的源码或汇编产物；如 [build_gui](build_gui) 和 [build_headless](build_headless) 都是 CMake/Ninja 产物目录
- 不要把打包路径指向 `lightroom-clone-cpp/build_gui` 这类历史 VS 旧树（源码上提后已不存在）
- 若需要做发布打包，按 [packaging/installer.iss](packaging/installer.iss)（Inno Setup，主入口）头部注释暂存载荷到 `build_gui/install_stage` 后编译；[packaging/installer.nsi](packaging/installer.nsi) 仅为历史参考
- **Ninja + 中文版 MSVC 的 `/showIncludes` 前缀会被 CMake 错误转码**，导致 `CMakeFiles/rules.ninja` 里的 `msvc_deps_prefix` 匹配不上、所有 `.obj` 记录 `#deps 0`：改头文件不触发重编，增量构建会静默使用陈旧 obj（曾导致 Json 布局变更后 SIGSEGV）。每次 configure 后必须先跑 `build_headless\_fix_msvc_deps.ps1 -BuildDir <dir>`，并用 `ninja -C <dir> -t deps <obj>` 确认 `#deps > 0`；改头文件后的验证结论以 clean 重建为准（详见 [docs/DEVELOPMENT_EXPERIENCE.md](docs/DEVELOPMENT_EXPERIENCE.md) §10）
- 代码改动应优先以 headless 构建和测试作为快速反馈，GUI 相关改动再补 GUI 构建或直接跑对应 exe

## 7. 任务时的工作方式

- 先定位属于 engine / catalog / ui / tests 的对应目录，再动代码
- 优先用最小、可验证的改动完成问题修复；不要为了“更通用”而扩散重构
- 如果新增行为涉及图像处理或色彩管线，尽量补充 Catch2 测试，不要只修 UI 表现
- 若修改构建或依赖，更新相关说明文档；本仓库的 README / CLAUDE / docs 已出现过时表述，写代码前先核对时效

## 8. 注释规范（新增，强制遵守）

本仓库的注释需要同时满足“工程可读性”和“长期维护性”两个目标。所有新增或重写代码，必须遵循以下规则：

### 8.1 目标与原则

- 注释用于说明“设计意图、边界条件、接口契约、性能/线程假设”，而不是重复代码本身。
- 公开接口（header / public method）必须比实现细节更完整；调用方应能从注释里理解输入、输出、限制与失败语义。
- 不要为了“写注释”而写废话；不要重复显然代码。
- 对复杂算法、奇怪边界、缓存键、线程安全、状态转换、性能热点，必须补充说明。
- 历史阶段标记（如 `PHASE_X`、`M1/M2`、`Sx`）只能用于任务跟踪，不能混入长期 API 注释；长期维护的注释必须描述稳定的设计语义。

### 8.2 统一风格

- **硬性编码坑**：工程未设 `/utf-8`，MSVC 在 GBK(936) 代码页下读源码。`engine/`、`catalog/`、`utils/` 里中文注释行的行末必须是 ASCII 字符（`.`、`,`、`)` 等）；行末汉字或全角标点的 UTF-8 尾字节（0x81–0xBF）会被当成 GBK 前导字节与换行配对，吞掉换行并把下一行代码注释掉，报 `C2530` 之类费解错误。写完中文注释逐行自查（详见 [docs/DEVELOPMENT_EXPERIENCE.md](docs/DEVELOPMENT_EXPERIENCE.md) §8）。
- 代码注释优先使用英文，文档说明优先使用中文。
- 公开 API 与关键类型优先采用 Doxygen 风格：`/** ... */` + `@brief` + `@param` + `@return`。
- 类注释必须解释职责、适用范围和关键约束。
- 函数注释必须说明：
  - 作用（what）
  - 输入约束（preconditions / ranges / units）
  - 返回语义（return / failure behavior）
  - 关键约束（threading / ownership / caches / side effects）
  - 必要时附加 `@note` / `@warning`

### 8.3 注释模板（建议用法）

```cpp
/**
 * @brief Renders a preview from a linear ProPhoto source buffer.
 *
 * The pipeline is intentionally non-destructive: input buffers are never
 * mutated in place. The output is display-ready sRGB data sized to the
 * requested viewport.
 *
 * @param input Linear RGB buffer in ProPhoto color space.
 * @param size Target render size; 0 means full resolution.
 * @param hashKey Stable cache key for the current adjustment stack.
 * @return Rendered preview image, or nullptr on invalid input.
 *
 * @note This path assumes the input buffer is already normalized and not
 *       aliased with the original RAW buffer. Thread safety is per-image-id.
 */
```

### 8.4 必须注明的场景

以下情况必须额外补充注释：

- 线程安全/线程边界
- 生命周期/所有权
- 缓存键与缓存策略
- 状态机转换（例如：idle → rendering → committed）
- 重要性能热点（重构时也要保留解释）
- 非显而易见的数学、色彩转换或像素处理逻辑
- 边界条件、异常分支和失败回退

### 8.5 禁止事项

- 不要用单行“无意义注释”覆盖明显代码，如 `// increment i`。
- 不要把历史任务状态写进长期 API 注释。
- 不要在第三方代码目录中按项目规范进行重写；第三方库注释是上游代码的一部分，不代表项目规范。
- 不要写出“会过期但没人维护”的注释，避免伪装成设计说明。

### 8.6 审查门禁

在提交或合并时，检查者应至少确认以下几点：

- 公开 API 是否具备必要注释
- 关键功能是否说明了输入/输出/失败语义
- 线程安全、缓存、状态转换、边界条件是否清楚
- 是否存在历史任务标签污染长期注释
- 说明是否与当前代码状态一致，不出现“文档说法与实现不符”

以上约定来自当前 CMake 入口和实际代码状态；更多实现与历史背景请参阅 [README.md](README.md) 与 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 9. GUI 操作验收（kimi-cu 计算机操作）

本机可用的 kimi-cu MCP 能驱动真实鼠标与键盘。凡是改动 UI 行为，除编译和无头测试外，都应按下列步骤做一次真实的端到端操作，并把观察到的结果（截图、状态栏文案、落盘文件）作为验收证据，不要只报告“编译通过”。

### 9.1 常规步骤

1. 构建：导入 x64 MSVC 环境后执行 `cmake --build build_gui -j`；若无改动会输出 `ninja: no work to do`，此时既有 `build_gui/SilverHalide.exe` 即对应当前源码。首次布局需要先跑 windeployqt 补齐 Qt 运行库，判据是 `build_gui/platforms/qwindows.dll` 存在。
2. 启动：用 `launch_app` 指向 `build_gui/SilverHalide.exe`；窗口出现可能要等一下，先 `list_apps` 确认进程与窗口。
3. 定位窗口：不要用 `app` 参数按名字查找。本工作区的终端标题里含 SilverHalide-src，会误命中 WindowsTerminal；正确做法是 `list_apps` 取到 pid，再用 `pid` 或 `window_id` 观察。
4. 采样：`get_app_state` 用 `mode: image` 做视觉判断、用 `mode: ax` 取控件索引，两者都会返回 snapshot_id，后续动作必须带上它。界面内容变化后要重新采样，不要复用旧索引或旧坐标。
5. 操作：优先使用 AX 索引（`click` / `set_value` / `perform_secondary_action`）；弹窗、自绘控件和无障碍树缺失时才退化为坐标点击。
6. 复验：每个关键动作后重新 `get_app_state` 确认状态真的变了。状态栏文案、图片计数、列表项变化是最省事的判据。

### 9.2 标准测试图（本机现成）

- `G:\2026\2026-01-02\_1090017.RW2`：RAW，全分辨率 4608×3464，横幅
- `G:\2026\2026-06-26\_DSC0112.ARW`：RAW，竖幅
- `G:\2026\2026-03-09\_DSC0015-HDR.jpg`：JPEG，带文字水印

这组图覆盖了 RAW 与 JPEG 两条解码路径，以及横竖两种版式。

### 9.3 建议的例行路径

按“导入 → 逐张渲染 → 自动 → 导出 → 元数据”跑一遍，覆盖读、显示、编辑、写盘四条链路：

1. 文件 → 导入文件…，选“复制到目录”，在原生文件框的文件名栏一次填入三个带引号的完整路径，确认对话框显示“已选择 3 个文件”后导入。
2. 依次点选缩略图，确认状态栏给出文件名与像素尺寸、直方图有数据；RAW 与 JPEG 都要看，避免只验证一种解码器。
3. 在 RAW 上点“自动”，检查滑杆数值变化与状态栏的“编辑已保存”。
4. 文件 → 导出…，JPEG + 原始尺寸，落盘后用 System.Drawing 或等价手段确认导出尺寸是原始分辨率而不是预览尺寸。
5. 切到“元数据”页写入一个关键字并回车，确认左侧“关键字列表”同步出现计数。

### 9.4 注意事项

- 目录库默认落在 `%LOCALAPPDATA%\Temp\shcat_layout_test\Library\...`，导出也默认写回该库目录。验收完成后可整体删除这个临时库，不要把测试产物写进仓库。
- 原生文件对话框的输入必须确认 `type_text` 返回的 `verification.status` 为 `matched` 才算成功；`typed` 的字符数不能作为依据，详见全局笔记的 kimi-cu 小节。
- 收尾可以调用 `turn_ended` 隐藏覆盖层并清除 Esc 取消状态。
