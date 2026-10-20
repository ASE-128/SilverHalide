# 现代 C++ 采用度与效率审计

- 状态：已实施（基线见 [modern-cpp-adoption-baseline.md](modern-cpp-adoption-baseline.md)，结果与遗留见 [modern-cpp-adoption-report.md](modern-cpp-adoption-report.md)）
- 基线：`main` @ `1d6a624`（2026-10-05），审计日期 2026-10-19
- 范围：`src/` + `tests/`，182 个 C++ 源文件，约 56,900 行
- 相关文档：[ARCHITECTURE.md](../ARCHITECTURE.md)、[competitive-optimization-implementation-spec.md](competitive-optimization-implementation-spec.md)
- 产物目标：把「标准版本是 C++20、用法仍以 C++11/14 为主」的差距收敛到「所有权与拷贝先现代化，语法糖后补」

---

## 0. TL;DR

1. **总体结论**：项目对现代 C++ 的采用是「配置到位、设施偏科」。C++20 已开启且关闭扩展语言，可读性设施（`std::optional`、`std::clamp`、`std::filesystem`、`std::bit_cast`、默认 `operator==`）用得不错；但真正能省成本的那批设施（`std::variant`、`std::string_view`、`std::span`、结构化绑定、`if constexpr`、概念、`from_chars/to_chars`）**全部为 0 使用**。
2. **效率瓶颈不在语法糖，在所有权**：渲染管线每推进一级 stage 就整幅深拷贝一次 F32 工作缓冲。1920 宽预览每帧约 4 × 29.5 MB ≈ 118 MB memcpy；全尺寸 4608×3464 每级约 191 MB。这是全仓收益最高的改进点，也是最「现代 C++」（移动语义 + 不可变共享）的改造点。
3. **不建议**在本工程引入 modules、coroutines、`std::execution::par`、`<format>`。收益为零，风险为负。

---

## 1. 结论与判断依据

### 1.1 一句话结论

> 工程用手写的资源池、线程池、增量缓存和 SSE2 手工向量化，换来了「已经很快」；但标准库里零成本的效率设施基本没用，导致本该由语言/库省下的拷贝与分配仍在发生。

### 1.2 做得好的部分（应保留）

| 方向 | 证据 | 说明 |
| --- | --- | --- |
| 标准配置 | `CMakeLists.txt:4-6` | C++20、`CXX_STANDARD_REQUIRED ON`、`CXX_EXTENSIONS OFF` |
| 可读性设施 | `std::optional` 79、`std::clamp` 180、`std::filesystem` 775、`std::bit_cast` 11、`starts_with` 5 | 说明作者并不排斥新标准 |
| RAII 雏形 | `src/engine/RenderScratch.h` | `ScratchBuffer` 守卫 + thread_local 池，稳态渲染零分配 |
| 并发基础 | `src/utils/Parallel.h` + `atomic` 19 / `mutex` 71 | 自建线程池，含 UAF 防护注释（提交 `751965d` 刚修过完成态 UAF） |
| 值语义 | `operator==(...) = default` 多处（`Adjustments.h`） | C++20 默认比较已落地 |
| 测试资产 | 484 个 `TEST_CASE`；`tests/TestPerfAnchors.cpp` | 重构的安全网已经足够厚 |

### 1.3 明显空白

- **零使用**：`std::variant`、`std::string_view`、`std::span`、结构化绑定、`if constexpr`、`concept/requires`（7 处命中全是英文注释）、`std::ranges`/`std::execution`、`std::from_chars`/`std::to_chars`、`[[nodiscard]]`、modules、coroutines、`std::jthread`/`stop_token`。
- **偏低**：`std::move` 约 105 处 / 5.7 万行；`std::forward` 0 处；算法替代手写循环几乎不做（`std::sort` 7、`std::transform` 2、`accumulate` 1，像素主循环全部手写）。
- **反例**：`ColorManager` 仍持 7 个 `void*` 裸句柄 + 析构里手写 `cmsCloseProfile`（`ColorManager.h:120-128`、`ColorManager.cpp:73-89`），构造中途抛异常即泄漏。

---

## 2. 统计口径

统计对象为 `src` 与 `tests` 下全部 `*.cpp/*.h/*.hpp`，按“出现次数”计（非文件数）。复现命令见附录 A。

| 指标 | 数值 | 指标 | 数值 |
| --- | --- | --- | --- |
| 源文件 / 行数 | 182 / ~56,900 | `std::optional` | 79 |
| `std::move` | ~105 | `std::variant` | 0 |
| `std::forward` | 0 | `std::string_view` | 0 |
| `make_shared` / `make_unique` | 83 / 28 | `std::span` | 0 |
| `shared_ptr` / `unique_ptr` | 96 / 25 | 结构化绑定 | 0 |
| `constexpr` | 181（多为常量表） | `if constexpr` | 0 |
| `noexcept` | 8 | 概念 / requires | 0 |
| `[[nodiscard]]` | 0 | `std::ranges` | 0 |
| `reinterpret_cast` | 215 | `from_chars`/`to_chars` | 0 / 0 |
| 范围 for | 321 | `std::sort` / `transform` | 7 / 2 |
| `std::bit_cast` | 11 | `std::thread` / `jthread` | 20 / 0 |
| `const std::string&` 只读参数 | 201 | `TEST_CASE` | 484 |

> 注：全仓 524 处 `new` 绝大多数是 Qt 控件构造（父对象接管所有权，符合 Qt 惯例），**不列为缺陷**，也不建议改成智能指针。

---

## 3. 改进清单

优先级按「预期收益 ÷ 改动风险」排序。P0 = 直接作用于每帧渲染路径；P1 = 结构性收益；P2 = 长期正确性/一致性。

### IM-1（P0）消除渲染 stage 的整缓冲深拷贝

- **位置**：`src/engine/Pipeline.cpp:3009`、`3066`；同源模式见 `:3188`、`:3332`、`:3519-3522`、`:3637-3645`；`src/engine/ImageBuffer.h`
- **现状**

```cpp
// Pipeline.cpp:3065-3066
// The retained node is immutable once cached; the next stage needs
// its own writable copy.
input = std::make_shared<ImageBuffer>(*renderState_.stages[i]);
```

  为了保持已缓存阶段不可变，每推进一级就整幅复制一次；stage 0..3 共 4 次。
  1920×1280 F32：`1920*1280*12 = 29.5 MB` → 每帧约 118 MB；4608×3464 时每级 191.5 MB。
  另外 `renderInternalLinear`（`:3634-3645`）对「已是 F32 且同尺寸」的输入也整幅复制，注释已明说原因是 `applyAdjustments` 原地编辑。
- **建议**

  1. 渲染状态持有**两块**可写工作缓冲轮转使用（ping-pong），而不是每级新分配/复制；
  2. 保留阶段升级为 `std::shared_ptr<const ImageBuffer>`，只读共享，天然不可变；
  3. `ImageBuffer` 增加 `clone()` 与 `withSameLayout()` 两个显式语义，禁止隐式拷贝构造出现在热路径（可用 `[[deprecated]]` 或代码评审约定，不建议直接 `= delete`，会破坏 `tests/TestImageBuffer.cpp`）；
  4. 对「调用方缓冲必须不被修改」的 API，改为显式 `borrow`/`takeOwnership` 两种入口，而不是靠拷贝保证。
- **改造前后**

```cpp
// before
input = std::make_shared<ImageBuffer>(*renderState_.stages[i]);

// after
retainStage(i, std::move(input));                       // shared_ptr<const ImageBuffer>
input = workingPool_.acquire(nw, nh, PixelFormat::F32); // 两块轮转，复用不清零
applyStageB(adjustments, mutableFloats(*input), pixelCount);
```
- **预期收益**：预览/导出路径 memcpy 量下降约 60–80%，峰值 RSS 与内存带宽同步下降；滑块拖动帧时间更稳。
- **风险**：生命周期管理是唯一难点（`input` 不能与任何保留阶段别名）。**不改任何浮点运算顺序**，因此不触碰 bit-exact 约束。
- **验收**：`tests/test_incremental_pipeline.cpp` 的 stage 复用断言全过；`TestPipeline` 输出逐位一致；`TestPerfAnchors` 的 render 时间下降并记录。

---

### IM-2（P0）用 `std::span` 取代 `(ptr, w, h)` 三元组与逐像素 `switch`

- **位置**：`src/engine/ImageBuffer.h`（`pixel/setPixel` 声明）、`src/engine/ImageBuffer.cpp:96-128`、`src/engine/HdrMerge.cpp:61-63`
- **现状**：全仓 215 处 `reinterpret_cast`；接口以裸指针 + 宽高传参；`ImageBuffer::pixel()` 每次调用做 4 次边界判断，内部再按 `PixelFormat` 走一次 `switch`。`HdrMerge` 每像素调用 3 次 = 每像素 12 次分支。
- **建议**：新增类型化只读/可写视图，一次校验、循环内零分支；`pixel()/setPixel()` 保留给冷路径与测试。

```cpp
// ImageBuffer.h（新增，不改既有存储布局）
std::span<const float> asFloatSpan() const;   // precondition: format() == F32
std::span<float> asMutableFloatSpan();        // precondition: format() == F32
```

```cpp
// before — HdrMerge.cpp:61-63
acc += buf.pixel(sx, sy, 0) * 0.2126 +
       buf.pixel(sx, sy, 1) * 0.7152 +
       buf.pixel(sx, sy, 2) * 0.0722;

// after
const std::span<const float> px = buf.asFloatSpan();
for (size_t i = 0; i + 2 < px.size(); i += 3)
    acc += px[i] * 0.2126f + px[i + 1] * 0.7152f + px[i + 2] * 0.0722f;
```
- **预期收益**：热循环去掉每像素十余个分支，可被 MSVC 自动向量化；同时把「缓冲 + 长度」绑成单一对象，是 IM-1 双缓冲改造的前置条件。
- **风险**：视图不得越过底层缓冲生命周期，需在 Doxygen 注释里写清 precondition（AGENTS.md 第 8 节要求）。
- **验收**：`TestImageBuffer`、`TestHdrMerge`、`TestPipeline` 全过；`HdrMerge` 输出逐位一致。

---

### IM-3（P0）`parallelFor` 的内联快路径 + 参数转发

- **位置**：`src/utils/Parallel.h:44-46`、`src/utils/Parallel.cpp:44-45`、`212-215`
- **现状**：API 边界是 `const std::function<void(size_t,size_t)>&`，每次调用构造一次类型擦除对象；`n <= grain`、已在池线程（嵌套）这两条内联路径也被擦除挡住，无法内联。
- **建议（低风险版）**：把快路径提到头文件内联，只有真正需要投递任务时才擦除一次；**不要**重写整个 `Pool`（`751965d` 刚修完完成态 UAF，重写风险大于收益）。

```cpp
// Parallel.h
namespace detail {
bool onPoolThread() noexcept;   // 原 Parallel.cpp 里的 tInPool
void parallelForErased(size_t n, size_t grain,
                       const std::function<void(size_t, size_t)>& fn);
}

template <class Fn>
void parallelFor(size_t n, size_t grain, Fn&& fn) {
    if (n == 0) return;
    const size_t g = std::max<size_t>(1, grain);
    if (n <= g || detail::onPoolThread()) { fn(0, n); return; }   // 零成本路径
    detail::parallelForErased(n, g, std::function<void(size_t, size_t)>(
                                     std::forward<Fn>(fn)));
}
```
- **预期收益**：小图片/尾部 chunk 与嵌套调用完全不再分配；渲染一次拖动会调用几十次 `parallelFor`，累积可观。
- **风险**：`std::function` 仍存在于跨线程路径（合理，队列必须擦除）；不得改变 chunk 切分，否则破坏逐位一致。
- **验收**：`TestParallel` 全过（含嵌套与异常路径）；`TestPipeline` 逐位一致。

---

### IM-4（P0）JSON 层：`std::variant` + `string_view` + `from_chars/to_chars`

- **位置**：`src/utils/Json.h:119-127`、`src/utils/Json.cpp:325-340`、`:393`、`:438`
- **现状**：
  - `Value` 同时内置 `std::string` + `std::vector<Value>` + `std::map<std::string, Value>`，每个 bool/数字都背着三个容器成员（对象 ≥ 100 字节）；
  - 每个数字先 `substr` 出一个 `std::string` 再 `strtod`/`strtoll`，并用 `errno` 判错（受 locale 影响）；
  - 序列化用 `ostringstream + setprecision(17)`，每层缩进构造一次临时 `std::string`。
- **建议**：`std::variant<std::monostate, bool, long long, double, std::string, std::vector<Value>, Object>`；解析对 `std::string_view` 直接 `from_chars`；序列化 `to_chars` 写入预分配 `std::string`。

```cpp
// before
const std::string token = text_.substr(start, pos_ - start);
const double v = std::strtod(token.c_str(), &end);

// after
const std::string_view token = text_.substr(start, pos_ - start);
double v = 0.0;
const auto [ptr, ec] = std::from_chars(
    token.data(), token.data() + token.size(), v);
if (ec != std::errc{}) return std::nullopt;
```
- **预期收益**：预置/调整栈反序列化不再有逐值堆分配；`Value` 从「全成员常驻」收敛到 variant 最大成员；顺带消除 locale 隐患。
- **风险**：`Value` 的 `members()/items()/asString()` 等访问器语义必须保持不变；`TestJson`、`TestProfileIO`、`TestPresetLibrary` 是安全网。
- **验收**：JSON 往返逐字节一致（含 `precision(17)` 的浮点格式，注意 `to_chars` 默认最短表示会改变输出，需显式用`std::chars_format::general` + 比较语义而非字符串，或保留原有格式化策略）。

> ⚠️ 该条有一个隐藏坑：`to_chars` 的最短往返表示与 `setprecision(17)` 输出不同。若仓库里有「序列化文本逐字节比对」的测试，必须同步调整期望值，或仅替换解析侧。

---

### IM-5（P1）`std::string_view` 化只读字符串参数

- **位置**：`src/utils/Json.cpp`（`operator[]`/`contains`）、`src/utils/ProfileIO.cpp`、`src/catalog/CatalogDatabase.cpp`
- **现状**：201 处 `const std::string&` 只读参数；配合 `std::map<std::string, ...>` 时，用字面量查表会构造临时 `std::string`。
- **建议**：先做 Json + ProfileIO（调用最频繁），再做 CatalogDatabase；容器换成带透明比较的形式。

```cpp
// before
bool Value::contains(const std::string& key) const;
std::map<std::string, Value> obj_;

// after
bool Value::contains(std::string_view key) const;
std::map<std::string, Value, std::less<>> obj_;   // 字面量键零分配
```
- **预期收益**：查表与参数传递去掉临时分配；接口表达力更好。
- **风险**：`string_view` 不得指向临时对象；调用点若持有临时 `std::string` 需确认生命周期。**不要**一次性全仓替换（201 处），按模块分批。
- **验收**：按模块跑对应测试；全量 headless 测试回归。

---

### IM-6（P1）`Mask` 判别式联合 → `std::variant`；`Adjustments` 跨线程改共享只读

- **位置**：`src/engine/Adjustments.h:160-230`、`src/ui/PreviewWorker.h:146`、`:151`
- **现状**：`Mask` 同时内联 `LinearGradientMask`、`RadialGradientMask`、`std::vector<BrushDab>`、`ColorRangeMask`、`AiMaskParams` 及 11 个局部调整 `double`；`Adjustments` 内含 `std::vector<Mask>`，通过队列信号以 `const Adjustments&` 传递时每次都要深拷贝整个数组。
- **建议**：

```cpp
// before
struct Mask {
    MaskType type = MaskType::None;
    LinearGradientMask linear;
    RadialGradientMask radial;
    std::vector<BrushDab> brush;
    ColorRangeMask color;
    AiMaskParams ai;
    /* ...11 个局部调整字段... */
};

// after
using MaskShape = std::variant<LinearGradientMask, RadialGradientMask,
                               BrushStroke, ColorRangeMask, AiMaskParams>;
struct Mask {
    MaskShape shape;          // 只有一种几何体常驻
    /* 公共的局部调整字段保持扁平 */
};
```

  跨线程传递改为 `std::shared_ptr<const Adjustments>`（UI 侧构造一次、worker 只读）。
- **预期收益**：`Adjustments` 体积与拷贝成本显著下降；每次预览请求少一次含 vector 的深拷贝。
- **风险**：`Mask` 被 UI（`DevelopPanel`）、序列化（`ProfileIO`）、管线三处消费，改动面大；`shared_ptr<const Adjustments>` 需在 `main()` 注册元类型，否则队列信号运行期失败。建议先用 `std::shared_ptr<const Adjustments>`（低风险），`Mask` 的 variant 化单独立项。
- **验收**：`TestMasks`、`TestAdjustments`、`TestProfileIO`、`lrc_qt_tests` 全过；预置文件往返一致。

---

### IM-7（P1）`ColorManager` 的 lcms 句柄 RAII 化

- **位置**：`src/engine/ColorManager.h:120-128`、`src/engine/ColorManager.cpp:41-109`
- **现状**：7 个 `void*`（`cmsHPROFILE`/`cmsHTRANSFORM`）+ 析构里手写 `if (h) cmsCloseProfile(h)`；构造函数里 `cmsCreateRGBProfile` 失败或抛异常会泄漏已建句柄；`display_` 与 `srgb_` 存在共享指针别名，析构顺序靠人工记忆。
- **建议**：

```cpp
// before
void* prophoto_ = nullptr;               // cmsHPROFILE
~ColorManager() { if (prophoto_) cmsCloseProfile(prophoto_); }

// after
struct CmsProfileDeleter { void operator()(void* h) const noexcept; };
using CmsProfile = std::unique_ptr<void, CmsProfileDeleter>;
CmsProfile prophoto_;      // 构造中途失败自动回收，无手写 delete
```
- **预期收益**：异常安全 + 析构顺序由成员声明顺序显式决定；删掉 15 行手工清理代码。
- **风险**：`display_ == srgb_` 的别名关系要保留（不可双重释放）——RAII 化时应让 `display_` 持 `shared_ptr` 或显式 `release()` 处理别名。
- **验收**：`TestColorManager`、`TestSoftProofing`、`TestExportEngine` 全过；连续 `loadDisplayProfile` 无泄漏（可用 ASan 或计数）。

---

### IM-8（P1）SIMD 策略与编译开关对齐

- **位置**：`src/engine/Pipeline.cpp:1064-1130`、`CMakeLists.txt`（`LRC_ENABLE_SIMD` 默认 OFF，开启时加 `/arch:AVX2`）
- **现状**：注释与实现是 SSE2（`_mm_*`、8 像素/24 float 批次），而编译开关是 AVX2；默认构建下这段代码根本不参与，且 `/arch:AVX2` 下的自动向量化可能已经追平手工 SSE2。
- **建议**：先做**测量**再决定：(a) 默认构建 baseline；(b) `LRC_ENABLE_SIMD=ON`；(c) 删除手工 SSE2、仅靠 `/arch:AVX2` 自动向量化。三者对比 `TestPerfAnchors` 的时间，用数据决定保留还是删除。
- **预期收益**：要么拿回一部分性能，要么删掉一大段难维护的 intrinsic 代码。
- **风险**：任何重排都会破坏 bit-exact 约定，必须逐位比对 `TestPipeline`；SIMD 默认 OFF 的既定策略不得擅自更改。

---

### IM-9（P2）`noexcept` 与 `[[nodiscard]]` 补齐

- **位置**：全仓（`noexcept` 仅 8 处，`[[nodiscard]]` 0 处）
- **建议**：优先给三类 API：返回 `std::optional`/`shared_ptr`/`bool` 状态的；`ImageBuffer::sizeBytes/width/height` 等访问器；移动构造与移动赋值。
- **收益**：正确性（忽略返回值会被编译器提示）+ 容器扩容路径少一次异常表开销。
- **风险**：`noexcept` 误标会导致 `std::terminate`，只标真正不抛的函数。

---

### IM-10（P2）`std::jthread` + `std::stop_token`（范围受限）

- **位置**：`src/catalog/FolderMonitor.cpp`（唯一的原生 `std::thread` 使用者）
- **现状**：`FolderMonitor.h:160` 用 `std::atomic<bool> stopRequested_` 手工停机。
- **建议**：仅在该文件改用 `std::jthread` + `stop_token`，`request_stop()`/`get_stop_token()` 替代自建原子标志。
- **收益**：停机路径标准化，析构自动 join。
- **风险**：Qt 的 `QThread` 体系（`PreviewWorker`/`ImportWorker`）**不要**改，混用会打乱既有 abort 语义与 `main()` 的元类型设计。

---

### IM-11（P2）`constexpr`/`consteval` 覆盖纯数学与常量表

- **位置**：`src/utils/MathUtils.h:23,29,36,42,45`（`lerp`/`smoothstep`/`hashCombine`/`stopsToGain`/`sliderToScale` 目前是 `inline`）、`src/engine/ColorManager.cpp:20-31`（矩阵已是 `constexpr`）、`kColorRangeCenters`
- **建议**：把纯函数标 `constexpr`（顺带 `noexcept`），让 kernel 表与 LUT 可在编译期生成。
- **收益**：当前主要是表达力与可测试性；对运行时性能影响有限（`-O2` 已内联）。
- **风险**：低。注意不要为了 `constexpr` 改数学实现细节。

---

### 明确不做

| 候选 | 不做的理由 |
| --- | --- |
| C++20 modules | 单 exe + 内部静态库 + Qt6 + FetchContent 混编，模块化会破坏现有 CMake 流程，收益仅编译期 |
| coroutines | 现有 IO 走 Qt 事件循环 / 回调，渲染是同步 CPU 密集，协程无处发力 |
| `std::execution::par` | MSVC STL 并行算法支持面窄，且嵌套语义与自建池冲突（`Parallel.h` 已处理嵌套死锁） |
| `<format>` | 非 Windows/headless 构建兼容性未验证，收益可由 `to_chars` 覆盖 |

---

## 4. 风险与前置条件

1. **工具链**：MSVC 19.4x + Ninja + `/std:c++20` 已满足 `span`/`variant`/`jthread`/`bit_cast`/`from_chars<double>`。最低版本假设写进 CMake 注释，避免有人用旧 VS 打开。
2. **逐位一致是硬约束**：管线注释明确要求标量路径 bit-identical（`Pipeline.cpp:1056-1072`），`tests/TestPipeline.cpp`、`tests/test_incremental_pipeline.cpp` 会抓。**本轮只改所有权/接口，不改浮点运算顺序**；SIMD 默认 OFF 的约定保留。
3. **ABI 与持久化**：单可执行体，外部 ABI 影响小；但 `ImageBuffer` 的字节布局一旦变动，会影响磁盘预览缓存 `previews.shcat` 与 `tests/TestImageBuffer.cpp`。IM-1/IM-2 明确**不改布局**，只加视图与所有权语义；`PixelFormat` 枚举值不可重排。
4. **Qt 元类型**：`Adjustments` 若改为 `shared_ptr<const Adjustments>`，必须同步在 `main()` 调用 `qRegisterMetaType`，否则队列连接在运行期静默失败。
5. **线程池回归**：`Parallel.cpp` 的完成态 UAF 刚在 `751965d` 修复，改动必须保留 `ForState` 的 shared_ptr 所有权模型与异常路径的等待逻辑；`TestParallel` 是最低门槛。
6. **测试覆盖**：484 个 `TEST_CASE` 是本次改造的最大资产；`tests/TestPerfAnchors.cpp` 提供可复现的 headless 基线。每个任务前后各跑一次 `build_headless\lrc_tests.exe`，并保留 `[performance]` 输出。
7. **注释与编码**：`engine/`、`catalog/` 的中文注释行**必须以 ASCII 字符结尾**（GBK 代码页会吞下一行，见 CLAUDE.md）；新增公开 API 按 AGENTS.md 第 8 节写 Doxygen 注释（作用/前置条件/返回值/线程与所有权假设）。
8. **视图生命周期**：`span`/`string_view` 不得越过底层缓冲；`shared_ptr<const ImageBuffer>` 必须真的不投递可写别名，否则从「快」变成「错」。

---

## 5. 分阶段落地

| 阶段 | 内容 | 预估 | 门禁 |
| --- | --- | --- | --- |
| 0 基线 | headless 构建；记录 `[performance]`/`[pipeline]`/`[incremental]` 数字与内存峰值 | 0.5 天 | 基线数字入库；`git status` 干净（当前有 `packaging/installer.iss` 等无关改动，需先隔离） |
| 1 视图层 | IM-2：`ImageBuffer` 加 `span` 视图；`HdrMerge` 去逐像素访问器 | 2–3 天 | 484 用例全过；输出逐位一致 |
| 2 所有权 | IM-1：Pipeline 双缓冲 + 不可变保留阶段；顺带处理 `renderInternalLinear` 的同尺寸复制 | 约 1 周 | incremental 测试全过；perf anchor 下降 |
| 3 设施 | IM-3（Parallel 快路径）+ IM-4（Json） | 约 1 周 | 全量测试；JSON 往返语义一致 |
| 4 铺开 | IM-5/6/7/8/9/10/11，每项独立 PR | 持续 | 每项各自跑全量 + 对应专项测试 |

**顺序理由**：IM-2 是所有后续改造的类型基础；IM-1 是唯一能立刻量到收益的项；IM-3/IM-4 改动局部、风险低，适合在 IM-1 之后并行；IM-6 面最大，放最后。

---

## 6. 任务分解（供 Agent Teams 使用）

下表是建议的共享任务板条目。写入范围尽量互斥；有依赖的用 `blocked_by` 串起来。

| ID | 任务 | 写入范围 | 依赖 | 验收 |
| --- | --- | --- | --- | --- |
| T0 | 基线与快照：构建 headless、跑三组测试、记录 perf 数字 | 无代码改动（仅记录到 `docs/spec/`） | — | 数字可复现 |
| T1 | IM-2 `ImageBuffer` span 视图 | `src/engine/ImageBuffer.h/.cpp`、`tests/TestImageBuffer.cpp` | T0 | 专项测试 + 全量 |
| T2 | IM-2 消费方：`HdrMerge`（可并入 T1，但写范围不同） | `src/engine/HdrMerge.cpp`、`tests/TestHdrMerge.cpp` | T1 | 逐位一致 |
| T3 | IM-1 Pipeline 双缓冲 | `src/engine/Pipeline.cpp/.h` | T1 | incremental + perf |
| T4 | IM-3 Parallel 快路径 | `src/utils/Parallel.h/.cpp`、`tests/TestParallel.cpp` | — | 并行/嵌套/异常用例 |
| T5 | IM-4 Json variant + from_chars | `src/utils/Json.h/.cpp`、`tests/TestJson.cpp` | — | 往返一致 |
| T6 | IM-5 string_view（先 Json/ProfileIO） | `src/utils/ProfileIO.*`、`src/utils/Json.*` | T5 | 模块测试 |
| T7 | IM-7 ColorManager RAII | `src/engine/ColorManager.h/.cpp` | — | 泄漏与专项测试 |
| T8 | IM-8 SIMD 测量与决策 | `src/engine/Pipeline.cpp` | T3 | 三组对比数据 + 结论 |

并发建议：`{T1+T2}`、`T4`、`T5`、`T7` 可同时进行（写范围完全不重叠）；`T3` 依赖 T1；`T6` 依赖 T5；`T8` 必须在 T3 之后（同文件冲突）。`T8` 只产出测量与建议，除非数据支持，否则**不擅自删除** intrinsic。

---

## 7. 附录 A：统计复现

在仓库根目录用 PowerShell 执行（只读，不会写入）：

```powershell
$files = (Get-ChildItem -Path src,tests -Recurse -File -Include *.cpp,*.h,*.hpp).FullName
function Cnt($pat) {
  $n = 0
  foreach ($f in $files) {
    $m = Select-String -LiteralPath $f -Pattern $pat -AllMatches -ErrorAction SilentlyContinue
    foreach ($x in $m) { $n += $x.Matches.Count }
  }
  return $n
}
"move="    + (Cnt 'std::move')
"forward=" + (Cnt 'std::forward')
"optional=" + (Cnt 'std::optional')
"variant=" + (Cnt 'std::variant')
"sv="      + (Cnt 'string_view')
"span="    + (Cnt 'std::span')
"ifcx="    + (Cnt 'if constexpr')
"ranges="  + (Cnt 'std::ranges')
"reinterp=" + (Cnt 'reinterpret_cast')
"nodiscard=" + (Cnt '\[\[nodiscard\]\]')
```

规模与测试：

```powershell
(Get-ChildItem -Path src,tests -Recurse -File -Include *.cpp,*.h,*.hpp).Count
(Select-String -Path (Get-ChildItem tests -Recurse -File -Include *.cpp).FullName -Pattern 'TEST_CASE' | Measure-Object).Count
```

> 注意：不要对本机 `C:\Users\<user>` 或 `node_modules` 做递归搜索（存在目录 junction 回环，会表现为卡死）。统计必须限定 `src`/`tests` 这类显式目录。

---

## 8. 附录 B：验收清单（每个任务收尾自查）

- [ ] 只改了声明范围内的文件，没有顺手重构无关代码
- [ ] `cmake --build build_headless -j` 通过
- [ ] `build_headless\lrc_tests.exe` 全绿（484 用例）
- [ ] 涉及渲染输出时，逐位比对通过（`TestPipeline` / `test_incremental_pipeline`）
- [ ] 新增/修改的公开 API 有 Doxygen 注释，说明前置条件、失败语义、线程与所有权
- [ ] `engine/`、`catalog/` 的中文注释行以 ASCII 结尾
- [ ] 涉及 perf 的改动，附上改动前后 `TestPerfAnchors` 的数字
- [ ] 未引入 modules / coroutines / `std::execution::par` / `<format>`
