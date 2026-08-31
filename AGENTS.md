# AGENTS.md

这是 Silver Halide 的仓库级 AI 指南。该项目是一个以 C++20 + Qt 6 为核心的 RAW/摄影工作流桌面程序，代码主入口在 [lightroom-clone-cpp/CMakeLists.txt](lightroom-clone-cpp/CMakeLists.txt)。

## 1. 关键事实

- 源码根目录： [lightroom-clone-cpp](lightroom-clone-cpp)
- 工作区根目录： [d:\Desktop\SilverHalide-src](.)
- 活跃构建树位于工作区根： [build_gui](build_gui) 和 [build_headless](build_headless)
- 旧的 VS 产物树 [lightroom-clone-cpp/build_gui](lightroom-clone-cpp/build_gui) 是历史残留，打包/调试/自动化不要再指向它
- Qt 安装路径： [qt/6.8.3/msvc2022_64](qt/6.8.3/msvc2022_64)
- ONNX Runtime 预置目录： [onnxruntime/onnxruntime-win-x64-1.23.2](onnxruntime/onnxruntime-win-x64-1.23.2)
- 主要文档： [lightroom-clone-cpp/README.md](lightroom-clone-cpp/README.md), [lightroom-clone-cpp/docs/ARCHITECTURE.md](lightroom-clone-cpp/docs/ARCHITECTURE.md), [lightroom-clone-cpp/CLAUDE.md](lightroom-clone-cpp/CLAUDE.md)

## 2. 真实可用的构建与测试命令

在 PowerShell / cmd 下，先确保使用 x64 MSVC 环境：

- GUI 全量构建：
  - [build_x64.bat](build_x64.bat)
  - 等价手写： `cmake -S lightroom-clone-cpp -B build_gui -G Ninja -DCMAKE_PREFIX_PATH=qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release`
  - 然后： `cmake --build build_gui -j`

- Headless / 引擎测试构建：
  - `cmake -S lightroom-clone-cpp -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF`
  - `cmake --build build_headless -j`
  - `ctest --test-dir build_headless --output-on-failure`

- GUI 测试：
  - 直接运行 `build_gui/tests/lrc_tests.exe`；不要把 GUI 测试当成稳定的 ctest 入口，仓库文档中已记录中文路径/进程管理在某些情况下会出现假卡死
  - 也可参考 [lightroom-clone-cpp/CLAUDE.md](lightroom-clone-cpp/CLAUDE.md) 或脚本 [run_app.ps1](run_app.ps1)

- 运行应用：
  - `qt/6.8.3/msvc2022_64/bin/windeployqt.exe --release --no-translations --compiler-runtime build_gui/SilverHalide.exe`
  - `build_gui/SilverHalide.exe`

## 3. 架构边界

- 引擎与目录层必须保持 Qt-free：
  - [lightroom-clone-cpp/src/engine](lightroom-clone-cpp/src/engine)
  - [lightroom-clone-cpp/src/catalog](lightroom-clone-cpp/src/catalog)
  - [lightroom-clone-cpp/src/utils](lightroom-clone-cpp/src/utils)
- UI 代码位于：
  - [lightroom-clone-cpp/src/ui](lightroom-clone-cpp/src/ui)
- 测试代码位于：
  - [lightroom-clone-cpp/tests](lightroom-clone-cpp/tests)

约定：
- `engine/` 与 `catalog/` 不应引入 Qt 头/依赖；它们需要能在 headless 构建中编译和测试。
- 新增功能若是展示层，优先放在 UI；若是处理 RAW/颜色/非破坏性编辑逻辑，应落到 engine/
- 新增单测优先放到 [lightroom-clone-cpp/tests](lightroom-clone-cpp/tests) 并在 [lightroom-clone-cpp/tests/CMakeLists.txt](lightroom-clone-cpp/tests/CMakeLists.txt) 中注册

## 4. 代码与构建约定

- CMake 版本：3.25+
- 标准：C++20，关闭扩展语言
- 生成器：默认使用 Ninja，尤其是当前工作区中所用的根构建树
- 构建选项：
  - `LRC_BUILD_TESTS=ON`
  - `LRC_ENABLE_QT=ON`
  - `LRC_ENABLE_AI=ON`（仅在 ONNX Runtime 可用时启用）
  - `LRC_ENABLE_QT=OFF` 用于无头构建
  - `LRC_PROXY=https://ghfast.top/` 可用于国内镜像/代理场景
- 本仓库对依赖的处理首选：已安装 package / vendored third_party / FetchContent；不要在不必要时改动依赖获取方式

## 5. 重要坑位

- 不要编辑生成目录中的源码或汇编产物；如 [build_gui](build_gui) 和 [build_headless](build_headless) 都是 CMake/Ninja 产物目录
- 不要把打包路径指向 [lightroom-clone-cpp/build_gui](lightroom-clone-cpp/build_gui) 这类历史 VS 旧树
- 若需要做发布打包，先遵循 [lightroom-clone-cpp/packaging/installer.nsi](lightroom-clone-cpp/packaging/installer.nsi) 的说明，并将发布文件暂存到 `build_gui/install_stage`
- 代码改动应优先以 headless 构建和测试作为快速反馈，GUI 相关改动再补 GUI 构建或直接跑对应 exe

## 6. 任务时的工作方式

- 先定位属于 engine / catalog / ui / tests 的对应目录，再动代码
- 优先用最小、可验证的改动完成问题修复；不要为了“更通用”而扩散重构
- 如果新增行为涉及图像处理或色彩管线，尽量补充 Catch2 测试，不要只修 UI 表现
- 若修改构建或依赖，更新相关说明文档；本仓库的 README / CLAUDE / docs 已出现过时表述，写代码前先核对时效

## 7. 注释规范（新增，强制遵守）

本仓库的注释需要同时满足“工程可读性”和“长期维护性”两个目标。所有新增或重写代码，必须遵循以下规则：

### 7.1 目标与原则

- 注释用于说明“设计意图、边界条件、接口契约、性能/线程假设”，而不是重复代码本身。
- 公开接口（header / public method）必须比实现细节更完整；调用方应能从注释里理解输入、输出、限制与失败语义。
- 不要为了“写注释”而写废话；不要重复显然代码。
- 对复杂算法、奇怪边界、缓存键、线程安全、状态转换、性能热点，必须补充说明。
- 历史阶段标记（如 `PHASE_X`、`M1/M2`、`Sx`）只能用于任务跟踪，不能混入长期 API 注释；长期维护的注释必须描述稳定的设计语义。

### 7.2 统一风格

- 代码注释优先使用英文，文档说明优先使用中文。
- 公开 API 与关键类型优先采用 Doxygen 风格：`/** ... */` + `@brief` + `@param` + `@return`。
- 类注释必须解释职责、适用范围和关键约束。
- 函数注释必须说明：
  - 作用（what）
  - 输入约束（preconditions / ranges / units）
  - 返回语义（return / failure behavior）
  - 关键约束（threading / ownership / caches / side effects）
  - 必要时附加 `@note` / `@warning`

### 7.3 注释模板（建议用法）

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

### 7.4 必须注明的场景

以下情况必须额外补充注释：

- 线程安全/线程边界
- 生命周期/所有权
- 缓存键与缓存策略
- 状态机转换（例如：idle → rendering → committed）
- 重要性能热点（重构时也要保留解释）
- 非显而易见的数学、色彩转换或像素处理逻辑
- 边界条件、异常分支和失败回退

### 7.5 禁止事项

- 不要用单行“无意义注释”覆盖明显代码，如 `// increment i`。
- 不要把历史任务状态写进长期 API 注释。
- 不要在第三方代码目录中按项目规范进行重写；第三方库注释是上游代码的一部分，不代表项目规范。
- 不要写出“会过期但没人维护”的注释，避免伪装成设计说明。

### 7.6 审查门禁

在提交或合并时，检查者应至少确认以下几点：

- 公开 API 是否具备必要注释
- 关键功能是否说明了输入/输出/失败语义
- 线程安全、缓存、状态转换、边界条件是否清楚
- 是否存在历史任务标签污染长期注释
- 说明是否与当前代码状态一致，不出现“文档说法与实现不符”

以上约定来自当前 CMake 入口和实际代码状态；更多实现与历史背景请参阅 [lightroom-clone-cpp/README.md](lightroom-clone-cpp/README.md) 与 [lightroom-clone-cpp/docs/ARCHITECTURE.md](lightroom-clone-cpp/docs/ARCHITECTURE.md)。
