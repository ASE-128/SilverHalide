# 现代 C++ 改造 · 实施汇总报告

- 规格：[modern-cpp-adoption-audit.md](modern-cpp-adoption-audit.md)（IM-1 ~ IM-11 + §8 验收清单）
- 基线：[modern-cpp-adoption-baseline.md](modern-cpp-adoption-baseline.md)（T0：数字、perf 三点采样、27 行逐位 oracle）
- 实施：2026-10-19 ~ 2026-10-20，Agent Teams 7 名 teammate（第二阶段新增 mask-owner）+ 共享任务板 T0~T13、U1~U5
- 分支：`main` @ `1d6a624`；**48 个源码/测试文件修改 + 1 个删除**（`src/engine/Pipeline.cpp.orig`），**+3531 / -1485**；另有文档改动与 5 个新增规格文件（另有 1 个 T0 前就存在的无关改动 `packaging/installer.iss`，未触碰）
- 结论：**IM-1 ~ IM-11 全部落地并验收**，包括第二阶段补齐的独立项——`Mask` 的 `std::variant` 化（IM-6）、IM-6 剩余跨线程拷贝、IM-5 第二批（CatalogDatabase）、IM-9 第二批，以及 IM-8 的后续动作（删除手工 SSE2 批次）。IM-8 的 SIMD 结论仍为「默认 OFF、保留 `LRC_ENABLE_SIMD` 开关与 AVX2 自动向量化」。

---

## 1. 验收方法（为什么这次数字可信）

### 1.1 逐位一致 oracle（补齐 §8 的空白）

`TestPipeline` / `test_incremental_pipeline` 多数是容差断言，只能证明「没有明显回归」，不足以证明「浮点运算顺序未变」。因此 T0 额外建立了一个只依赖公开引擎 API 的 hash oracle：27 行覆盖 `ImageBuffer` 几何/格式、`ColorManager` 三条转换、`renderPreview` 增量与缓存命中、`renderFullDisplay`、直方图、`HdrMerge`（含 band 不变式）、`parallelFor` 分块确定性、JSON/ProfileIO 往返。

- 源码：[modern-cpp-bitexact-harness.cpp](modern-cpp-bitexact-harness.cpp)
- 基线输出：[modern-cpp-adoption-baseline.md](modern-cpp-adoption-baseline.md) §4
- **最终集成结果：27/27 行与基线逐字节相同**（用最终 `build_headless/lrc_engine.lib` 重编后 `Compare-Object` 为空）。

### 1.2 最终集成验证（Lead 亲自执行，安静机器、clean 重建）

| 项 | 结果 |
| --- | --- |
| `cmake --build build_headless --target clean` + 全量重建 | 298/298 目标成功，0 error C / LNK / FAILED |
| 全量 `lrc_tests.exe` | **510 test cases / 979,162 assertions 全绿**，9 s |
| `[pipeline]` | 65 cases / 2,675 assertions（与基线**计数完全相同**） |
| `[incremental]` | 8 cases / 375,061 assertions（与基线**计数完全相同**） |
| `[performance]` | 4 cases / 24 assertions |
| oracle 27 行 | 与基线**完全一致** |
| `build_gui`（GUI 侧改动） | clean 全量重建 359 步 0 error；`lrc_qt_tests` **16 个测试类 0 失败**（最终源码） |
| GUI 启动冒烟（kimi-cu 真机） | `SilverHalide.exe` 启动成功，窗口标题 `Silver Halide — 1.0.0 — shcat_layout_test`，左侧库树/中央画布/右侧编辑面板与色调曲线全部正常渲染，状态栏「就绪」；Alt+F4 正常退出，无残留进程 |
| 红线扫描 | `std::execution` / `co_await` / `co_return` / `import std` / `std::format` / `<format>` 均 0 命中；engine/catalog 无 Qt include；engine/catalog 新增行非 ASCII 结尾 0 处；`git diff --check` 干净 |

### 1.3 期间发现并修复的构建设施故障（重要，影响所有验证有效性）

本机（CMake 4.4.2 + Ninja 1.13.2 + 中文版 MSVC 19.44）存在：

> cl.exe 把 `/showIncludes` 的本地化前缀（`注意: 包含文件:`）以 UTF-8 写出，而 CMake 用 GBK 系统代码页解码再以 UTF-8 写出，`CMakeFiles/rules.ninja` 里的 `msvc_deps_prefix` 变成双重 mojibake。ninja 的 `deps = msvc` 解析器永远匹配不上 → **每个 .obj 记录 `#deps 0`** → 改头文件不触发依赖重编，增量构建静默使用陈旧 obj。

实证：`ninja -t deps ...Pipeline.cpp.obj` 返回 `#deps 0`，且 cl 的 include 行原样泄漏进构建日志；`Json.h` 改布局期间曾出现 `TestJson` SIGSEGV（陈旧 obj 与新布局混链）。

- 处置：写了 `build_headless/_fix_msvc_deps.ps1`，把 `rules.ninja` 的 `msvc_deps_prefix` 行重写为正确的 UTF-8 字节（`注意: 包含文件:`），deps 立即恢复（`Pipeline.cpp.obj #deps 14 VALID`）。
- 全队据此**对所有任务做 clean 重建后复验**；最终验收也是 clean 重建。
- 风险留给后续：**每次重新 configure 后都要重跑该脚本**（rules.ninja 会被重写）。该脚本在 `build_*`（已被 `.gitignore` 覆盖）内，未入库；建议后续把「deps 前缀修正」纳入项目脚本，或升级 CMake 后复核。

---

## 2. 逐项改动明细

> 每项均满足：`build_headless` clean 构建通过 + 全量测试全绿 + oracle 对应行零差异 + §8 自查。perf 数字见 §3。

### IM-1（P0）消除渲染 stage 的整幅深拷贝 — T3

- **文件**：`src/engine/Pipeline.h`（+66/−4）、`src/engine/Pipeline.cpp`（+492/−208）
- **before**：`renderState_.stages[6]` 为可变 `ImageBufferPtr`；每推进一级 stage 用 `std::make_shared<ImageBuffer>(*stages[i])` 整幅复制一次（1920 宽每级 ~29.5 MB，A→D 共 4 次；全尺寸 4608×3464 每级 ~191 MB）。`renderInternalLinear` 对「已是 F32 且同尺寸」的输入也整幅复制。
- **after**：
  - `RenderState` 拆为 `std::shared_ptr<const ImageBuffer> retained[4]`（A–D，const 类型即不可写保证）+ `ImageBufferPtr finalFrame`（stage F，保留「全命中返回同一对象」契约）；
  - 新增私有 `WorkBufferPool`（acquire/release/clear），`release` 在 `use_count()!=1` 时直接丢弃 → 池内可写缓冲**结构上不可能**与保留快照别名；
  - stage 函数改 `bool f(a, const float* src, float* dst, n)`，out-of-place 写入池化缓冲，`src==dst` 时与原 in-place 逐位等价；
  - 恒等 stage 零拷贝（`stageANeedsWork/B/C/D` 谓词与各 stage 体内 early-return 是同一份真值，返回 false 即别名共享）；
  - `renderInternalLinear` 改借用语义（stage A 兼作拷贝），调用方缓冲全程只读；
  - 顺带去掉「增量渲染/全命中时白跑一次全幅 area-average」的无谓工作。
- **验收**：oracle `pl.*` 6 行全一致；`[incremental]` 8 例（含 stage 复用、D/E 重启、淘汰回溯、内存预算）与 `[pipeline]` 65 例全过；perf 见 §3（B-2 −32.8%）。
- **取舍**：仍保留 1 次整幅拷贝（D→E，空间核是 in-place 实现且 D 必须保留）；稳态多 1 块 idle F32 缓冲（1920×1280 ≈ 29.5 MB），`clearIncrementalCache()` 释放，峰值缓冲数不变。

### IM-2（P0）`std::span` 取代 (ptr,w,h) 与逐像素 switch — T1 + T2

- **T1 文件**：`src/engine/ImageBuffer.h`、`ImageBuffer.cpp`、`tests/TestImageBuffer.cpp`
  - before：热循环只能 `pixel(x,y,c)`，每次 4 次边界判断 + 一次 `PixelFormat` switch，或用裸 `data()` + `reinterpret_cast`。
  - after：`std::span<const float> asFloatSpan() const` / `std::span<float> asMutableFloatSpan()`；仅 F32 且非空返回视图，否则**空 span、不抛异常**（不新增异常路径）；索引 `(y*width+x)*channels+c` 与 `pixel()`/`setPixel()` 完全一致；Doxygen 写明失效条件（reallocation/赋值/data() 变更）与线程要求。
  - **布局/枚举/拷贝语义零改动**。
- **T2 文件**：`src/engine/HdrMerge.cpp`（+81/−10）、`tests/TestHdrMerge.cpp`（+64）
  - before：`lumaSmall()` 每像素 3 次 `pixel()`（每像素 12 次分支）；`merge()` 每像素经 `px()` lambda 裸指针算术。
  - after：入口取一次 span，循环内 `p[0]*0.2126 + p[1]*0.7152 + p[2]*0.0722`（**double 累加、先 0.2126、顺序逐字未变**）；U16/U8 保留原路径（分支提到循环外）；`merge()` 每 band 取一次读视图，输出用 `asMutableFloatSpan()`。
  - **抓到并修复一个真实的逐位陷阱**：`lround` 使最后一个降采样格越过源边界（320×213 + maxDim=64 时 `y1=214 > 212`），旧 `pixel()` 越界返回 0 **但仍计入 `cnt`**。新路径只累加合法子矩形、保留原采样数并补一次 `acc += 0.0`，从而复现唯一可观测差异（−0.0 vs +0.0）。若不处理会直接改 hash。

### IM-3（P0）`parallelFor` 内联快路径 + 参数转发 — T4

- **文件**：`src/utils/Parallel.h`（+102）、`src/utils/Parallel.cpp`（+16）、`tests/TestParallel.cpp`（+97）
- **before**：API 边界是 `const std::function<void(size_t,size_t)>&`，每次调用都构造类型擦除对象；`n <= grain` 与「已在池线程」两条内联路径也被擦除挡住。
- **after**：`parallelFor` 改为头文件模板；`n == 0` 直接返回、`n <= grain` 或 `detail::onPoolThread()` 直接 `fn(0,n)` 内联（零 `std::function`、零入队、不启动池）；只有真正投递时擦除一次 → `detail::parallelForErased`。
- **线程池主体零改动**：`Pool`/`ForState`（含 751965d 的 UAF 修复）、异常路径两条 wait、worker 吞异常、cursor 分块全部原样；`src/utils/Parallel.cpp` 的 diff 只有「`tInPool` 暴露为 `detail::onPoolThread()`」「改名」「一行薄转发」「namespace 收尾」。
- **验收**：新增 5 例（n==0、单 chunk 在调用线程、模板 vs 显式 `std::function` vs 擦除函数三方一致、内联异常传播、抛异常后池仍可用）；`[parallel]` 11 cases / 330,018 assertions 全绿；oracle `par.fill` 不变。

### IM-4（P0）JSON：`std::variant` + from_chars / to_chars — T5

- **文件**：`src/utils/Json.h`、`src/utils/Json.cpp`、`tests/TestJson.cpp`
- **before**：`Value` 同时常驻 `string` + `vector<Value>` + `map<string,Value>`（对象 ≥ 96 B）；数字先 `substr` 出 `std::string` 再 `strtod/strtoll`（受 locale 影响，出错靠 `errno`）；序列化 `ostringstream + setprecision(17)`。
- **after**：`std::variant<std::monostate,bool,long long,double,std::string,Array,Object>`，**`sizeof(Value)` 96 → 40 B**；解析用非拥有 `string_view` + `from_chars`（超出 `long long` 回退 double，越界回退 `strtod` 保留 ±inf）；序列化写入预分配 `std::string`，整数 `to_chars`，**浮点刻意保留 `setprecision(17)`**（避免 `to_chars` 最短表示改变输出）；全部访问器语义不变。
- **验收**：`json.*` / `profile.*` 5 行 oracle 逐字节不变；额外做了「改动前源码 vs 改动后源码」序列化快照 `fc /b` 与 50 例解析语义对拍，逐字节相同；JSON 微基准 2 KB 文档 parse+serialize **148.24 → 111.52 µs/iter（≈1.33×）**（本地微基准，仅供量级参考）。
- **注意（写进遗留）**：浮点序列化格式被 oracle 钉死，后续不能改成 `to_chars` 最短表示。

### IM-5（P1）`string_view` 化只读字符串参数 — T6

- **文件**：`src/utils/Json.h`、`Json.cpp`、`src/utils/ProfileIO.cpp`、`tests/TestJson.cpp`（`ProfileIO.h` 未改：其公开 API 无字符串键参数）
- **before**：201 处 `const std::string&` 只读参数；字面量查表构造临时 `std::string`。
- **after**：`operator[]` / `contains` 键参数与 `parse()` 入参 → `std::string_view`；`Object = std::map<std::string, Value, std::less<>>` 透明比较；非 const `operator[]` 先 `find`、未命中才 `emplace(std::string(key))`（已存在键零分配，插入时拷贝键保证生命周期安全）；`ProfileIO.cpp` 的 `writeFileAtomic` 正文参数 `string_view`。
- **验收**：oracle 5 行不变；新增生命周期用例（临时 `std::string` / 临时 `string_view` 入参）。`CatalogDatabase` 的 string_view 化按审计列为后续批次，本轮未做。

### IM-6（P1）`Adjustments` 跨线程共享 + `Mask` 的 `std::variant` 化 — T11 / U1 / U2

- **文件**：`src/engine/Adjustments.h`（+18）、`src/ui/MetaTypes.h`（+4）、`src/main.cpp`（+3）、`src/ui/PreviewWorker.h`（44 行）、`PreviewWorker.cpp`（65 行）、`src/ui/MainWindow.h`（+28）、`MainWindow.cpp`（96 行）、`tests/lrc_qt_tests.cpp`（22 行）
- **before**：`std::vector<Mask>` 的 `Adjustments` 经队列信号以 `const Adjustments&` 传递，**每次请求深拷贝整个栈**（一次编辑可能派生 coarse/refine/1:1 多个请求 → 1+N 次深拷贝）。
- **after**：`using AdjustmentsPtr = std::shared_ptr<const Adjustments>`；12 个 worker slot 参数改为 `AdjustmentsPtr`，槽内判空直接 return（绝不用默认栈继续跑），worker 只读不保留指针；`MainWindow` 新增 `adjustmentsSnapshot()`（用 `Adjustments::operator==` 判断等价才复用，一次编辑的多次派发共享同一快照）；`MetaTypes.h` 单点 `Q_DECLARE_METATYPE` + `main()` `qRegisterMetaType`。
- **验收**：**GUI clean 全量重建 359 步 0 error**；`lrc_qt_tests` 全绿（16 个测试类），其中 `SoftProofRenderPathsTest{coarseDragFrameIsProofed,fullResFrameIsProofed}`、`FullResHotPathTest{...}`、新增的 `AdjustmentsSignalTest::panelPublishesSharedSnapshot`、`PresetDialogSmokeTest`/`DialogSmokeTest`（载荷断言）与 `BatchExportTest::batchExportWritesAllFormats` 真实走通 queued `invokeMethod`（匹配失败会 timeout，不会静默通过）→ 运行期证明 metatype 注册 + moc 归一化签名成立；headless 全量 510 例；主 oracle 27 行 + mask oracle 34 行不变。
- **收益**：一次编辑的深拷贝次数从 `1+N` 降为 `1`（剩余那 1 次是从 `DevelopPanel` 信号到 `currentAdjustments_`）。
- **U2 补齐（第二次阶段）**：`DevelopPanel`（4 个信号 / 57 处 emit）、`PresetDialog`、`SnapshotDialog` 的信号全部改 `AdjustmentsPtr`；`ExportItem::adjustments` 改共享快照（入队即冻结，null 计 per-item 失败而非静默默认栈）；`MainWindow::adoptPublishedAdjustments()` 值不等才拷贝并复用同一指针。**拷贝账**：一次编辑由「每信号 1 次 + 每次派发 1 次」降为「panel 1 次 + MainWindow 值不等时 1 次 + 队列 0 次」；批量导出由「每条目 1 次 + 入队再拷 1 次」降为「每条目 1 次 + 入队 0 次」。
- **U1 补齐（Mask variant）**：`MaskShape = std::variant<LinearGradientMask, RadialGradientMask, BrushGeometry, ColorRangeMask, AiMaskParams, monostate>`，**`sizeof(Mask)` 352 → 248 B（−29.5%）**；`type` 仍是权威选择器，const 访问器 `std::get_if` 未命中返回函数内 static const 默认实例（不抛、不竞争），非 const 访问器未命中先 `emplace<T>()` 再返回；管线消费与 ProfileIO 键名/键顺序/缺省值全部不变；monostate 表达 `MaskType::None`。
- **未做（遗留）**：彻底消除 panel→MainWindow 的最后一次拷贝需要把编辑状态改成 COW `shared_ptr`（约 100 个变更点），单独立项。

### IM-7（P1）`ColorManager` lcms 句柄 RAII 化 — T7

- **文件**：`src/engine/ColorManager.h`（+173）、`ColorManager.cpp`（164 行重写）
- **before**：7 个 `void*`（`cmsHPROFILE`/`cmsHTRANSFORM`）+ 析构里 ~15 行手写 ~`if (h) cmsCloseProfile(h)`；构造中途失败泄漏；`display_ == srgb_` 双 owner 别名靠人工记忆。
- **after**：`CmsProfileDeleter`/`CmsTransformDeleter`（`noexcept`，定义在 .cpp，头文件继续 lcms-free）+ `std::unique_ptr<void, Deleter>`；`display_ == srgb_` 的别名改为「所有权派生」——删除第二个裸指针，`activeDisplayProfile()` 返回 `ownedDisplay_ ? ownedDisplay_.get() : srgb_.get()`，**每个句柄只有一个 owner ⇒ 结构上不可能双释放**；`~ColorManager() = default`；`reset()` 接管保证「新建成功才销毁旧句柄」；3 条 per-call 路径的临时句柄也 RAII 化，去掉 3 处早退泄漏点；`shared_mutex` 加锁范围语义不变。
- **验收**：oracle `cm.*` 3 行不变；`[color]` 13 cases、`SoftProofing` 11 cases、`ExportEngine` 11 cases 全过；额外用 lcms 内存 handler 探针验证 200×`loadDisplayProfile`、100×ctor/dtor、50×export churn 后 **live 块恒为 0、allocs == frees（351,125/351,125）**，private bytes 与 OS handles 稳定。

### IM-8（P1）SIMD 策略与编译开关对齐 — T8（测量）+ U4（删除）

- **结论：默认 OFF 逐位一致；保留 `LRC_ENABLE_SIMD` 开关与 `/arch:AVX2` 自动向量化；U4 已按测量数据删除手工 SSE2 stage A/C 批次（287 行），A/C 恒走标量路径。**
- **逐位一致**：
  - 默认 OFF：oracle **27/27 一致**（红线成立）。
  - `LRC_ENABLE_SIMD=ON`：oracle 有 **5 行 `pl.*` 不一致**（`pl.preview.a/.b/.b.hit/.a2/.a2.linear`），其余 22 行一致。隔离实验（c1/c2）证明：差异**唯一来源**是 P2-2 的 `blurExactPassAVX2` 使用 `_mm256_fmadd_ps`（FMA 重结合），代码注释本身已写明 tolerance-identical（P2-2 验收 Δ<0.008）；旁路它后 27/27 回到基线。**手工 SSE2 stage A/C 批次与标量路径逐位一致**（旁路它后哈希与 SIMD=ON 完全相同），`/arch:AVX2` 自动向量化也是逐位安全的。
  - ⇒ SIMD=ON 非逐位一致是**预先存在的设计取舍，不是本轮回归**。
- **性能**（a 默认 OFF / b SIMD=ON / c1 SIMD=ON+旁路手工 SSE2 批次；三组交错各 4 样本，cl.exe=0）：所有两两中位差均**小于采样极差** → 无可测量差异；最优点组间差 ≤5%。
- **U4 后续动作（已执行）**：删除 `applyStageABasicSIMD` / `applyStageCBasicSIMD` / `stageA_pixelTail` / `stageC_pixelTail` 与两处 `LRC_ENABLE_SIMD` 分派（共 287 行），A/C 恒走标量循环。验收：默认 OFF oracle 27/27、mask oracle 34/34；`SIMD=ON` 重建后仍是与 T8 **逐字节相同**的那 5 行 `pl.*` 差异（无新增差异行、未变成 27/27）；同窗口 3 次 perf 与 T8 的 a/c1 差均小于采样极差，**无可测量回退**。

### IM-9（P2）`noexcept` / `[[nodiscard]]` 补齐 — T12 + U5

- **文件**：`src/engine/ImageBuffer.h/.cpp`、`src/utils/Json.h/.cpp`、`src/engine/ColorManager.h/.cpp`（`Parallel.h` 未改）
- **before**：`noexcept` 全仓 8 处、`[[nodiscard]]` 0 处。
- **after**：`ImageBuffer` 12 个访问器/span/pixel 加 `noexcept`、7 个返回新缓冲的工厂/变换加 `[[nodiscard]]`；`Json` 17 个查询访问器 `noexcept`、`parse()` `[[nodiscard]]`；`ColorManager` 2 个纯矩阵函数 `noexcept`、8 个返回缓冲/字节的 API `[[nodiscard]]`（并把被改声明升级为 Doxygen）。
- **刻意不加**：所有会分配/按契约抛异常的构造函数与转换 API；`Value::type()`（`std::visit` 在 valueless-by-exception 时会抛 `bad_variant_access`，标 `noexcept` 会 terminate）；`Value` 移动（编译期探针实测本机 MSVC STL 的 `std::map` 移动构造非 `noexcept`，强标会 terminate）；`threadCount/defaultGrain/parallelFor`（首次调用会构造线程池）。
- **验收**：编译期探针 `static_assert` 验证所有 `noexcept` 成立、8 个 `[[nodiscard]]` API 在被丢弃时触发 C4834（项目构建 C4834 = 0）；oracle 27 行不变。
- **U5 第二批**：`Pipeline.h/.cpp`（16 个 out-of-line `noexcept`、10 个 `[[nodiscard]]`）、`HdrMerge.h`（`Result::ok`、`estimateEv`）、`Adjustments.h/.cpp`（19 个 `noexcept`、Mask 访问器/hash）、`ProfileIO.h`（4 个 `[[nodiscard]]`）、`MathUtils.h`、`CatalogDatabase.h`（36 个查询 `[[nodiscard]]`、3 个无锁 inline `noexcept`）、`FolderMonitor.h/.cpp`。**刻意回退了 6 个取 `std::mutex` 的访问器 `noexcept`**（lock 允许抛 `std::system_error`，标 `noexcept` 会把理论异常变成 terminate）；tests/TestPipeline.cpp 的 5 处故意丢弃返回值改为 `(void)`，最终构建 **C4834 = 0**。

### IM-10（P2）`std::jthread` + `stop_token` — T9

- **文件**：`src/catalog/FolderMonitor.h/.cpp`、`tests/TestFolderWatcher.cpp`
- **before**：唯一原生 `std::thread` + `std::atomic<bool> stopRequested_` 手工停机。
- **after**：`std::jthread` + `eventThreadMain(std::stop_token, ...)`；`request_stop()` / `stop_requested()` 替代自建原子标志；析构自动 join。`ReadDirectoryChangesW` + 轮询回退、`abnormalExit` 唤醒、回调线程语义不变；公开 API 签名零变化。新增「watcher 阻塞时析构 <5 s 返回」用例。
- **验收**：`FolderMonitor*` 11 cases / 71 assertions 全绿；oracle 不受影响。Qt 的 `QThread` 体系（`PreviewWorker`/`ImportWorker`）未动。

### IM-11（P2）`constexpr` / `noexcept` 覆盖纯数学 — T10

- **文件**：`src/utils/MathUtils.h`、`tests/TestMathUtils.cpp`
- **before**：`lerp`/`smoothstep`/`hashCombine`/`stopsToGain`/`sliderToScale` 均为 `inline`。
- **after**：`lerp`/`smoothstep`/`hashCombine` → `constexpr noexcept`（**实现逐字符未变**）；`stopsToGain`/`sliderToScale` 仅加 `noexcept`（`std::pow` 在 C++20 非 constexpr，保留原实现以保逐位一致）；补 Doxygen。
- **验收**：`static_assert` 常量求值 + `noexcept(...)` 断言 + 「constexpr 与 volatile 强制运行期求值结果逐位相等」用例；`[math]` 7 cases 全绿；oracle 不变。

---

## 3. TestPerfAnchors：改动前后数字（权威 A/B）

方法：把 T0 基线二进制（`build_headless/_ab_base/lrc_tests_base.exe`，HEAD 源码，构建于任何改动之前）与最终二进制在同一台机器、**两侧交替执行 3 轮**（base→after→after→base→base→after），取样中位数；测量时 `cl.exe` 计数 = 0。

| 锚点 | before（HEAD） | after（最终） | 变化 |
| --- | --- | --- | --- |
| **B-2** 1920 refine 单帧 **min** | **102.64 ms** | **69.02 ms** | **−32.8%** |
| B-2 refine **avg** | 101.97 ms | 74.51 ms | −26.9% |
| **B-1** 50MP 全尺寸 engine leg | **531.53 ms** | **475.45 ms** | **−10.5%** |
| **D-1** 50MP 全尺寸渲染 | **532.10 ms** | **483.81 ms** | **−9.1%** |
| D-1 作用域结束 commit | 74.75 MB | 74.77 MB | ±0（无回退） |
| **D-2** 峰值工作集回落量 | 688.13 MB | 688.13 MB | ±0（无回退） |

原始样本：

- before：B-2 min 106.44 / 102.64 / 100.92；B-1 551.46 / 524.46 / 531.53；D-1 556.53 / 527.86 / 532.10
- after：B-2 min 69.57 / 69.02 / 66.09；B-1 479.84 / 475.45 / 473.39；D-1 483.81 / 484.21 / 482.73
- 稳定性：两侧组内极差 B-2 ≈ 3–6 ms，B-1/D-1 ≈ 5–8 ms；**改进幅度远大于抖动**（B-2 差 33.6 ms vs 极差 ≤6 ms）。

辅助证据（T3 实施者在 build_t3 内的同窗口 A/B，机器有 6 个并发构建、仅方向性）：B-2 min 中位 140.32 → 95.51 ms（−32%）、B-1 946.56 → 762.05 ms（−19.5%）、D-1 1055.04 → 796.69 ms（−24.5%），与上表方向与量级一致。

> ⚠️ 口径提醒：T0 首次记录（安静机、单跑）为 93.40 / 686.51 / 687.29 ms；同一 baseline 二进制在今天交替测量中是 102.64 / 531.53 / 532.10 ms —— 说明**绝对数字会随机器状态漂移（B-1/D-1 可达 ±25%）**，跨时段直接比较不可靠。上表是同窗口交替 A/B，是本轮唯一可归因的数字。

**归因**：B-2/B-1/D-1 同时下降与 IM-1 一致——每帧少 3 次整幅 F32 memcpy（1920 宽每帧 ≈ 88 MB，50MP 每帧 ≈ 575 MB），并去掉增量渲染/全命中时白跑的一次全幅 area-average。IM-3/IM-4/IM-7 的收益是分配/分支/异常安全层面，量级小于 IM-1，未单独做隔离 A/B。

---

## 4. 统计指标（附录 A 口径，只读 worktree 对拍）

| 指标 | 改造前 (HEAD 1d6a624) | 改造后 | 变化 |
| --- | --- | --- | --- |
| `std::variant` | 0 | 5 | +5 |
| `std::span` | 0 | 19 | +19 |
| `string_view` | 0 | 49 | +49 |
| `if constexpr` | 0 | 5 | +5 |
| `[[nodiscard]]` | 0 | 75 | +75 |
| `noexcept` | 8 | 146 | +138 |
| `std::jthread` | 0 | 8 | +8 |
| `std::from_chars` / `std::to_chars` | 0 / 0 | 3 / 2 | +3 / +2 |
| `std::less<>`（透明比较） | 0 | 2 | +2 |
| `std::move` / `std::forward` | 105 / 0 | 115 / 1 | +10 / +1 |
| `constexpr` | 181 | 200 | +19 |
| `reinterpret_cast` | 215 | 216 | +1（代码搬移） |
| `TEST_CASE` | 484 | 512（运行时 510） | +28 |
| 源文件 / 行数 | 182 / 62,682 | 182 / 64,728 | 0 / +2,046 |

口径说明：`string_view` 的 10/28 在测试文件；`noexcept` 的 6/76 是 `TestMathUtils` 的 `noexcept(...)` 编译期断言；`std::jthread` 8 次中约 5 次是注释文本；`std::variant`/`std::less<>` 各有 1 次在 Doxygen 注释里。即真实代码 token 少于此表，但量级与方向可信。

---

## 5. 任务板最终状态

| 任务 | 内容 | 状态 |
| --- | --- | --- |
| T0 | 基线与快照（headless 构建、三组测试、perf、27 行 oracle） | completed（Lead） |
| T1 | IM-2 `ImageBuffer` span 视图 | completed |
| T2 | IM-2 `HdrMerge` 消费方 | completed |
| T3 | IM-1 Pipeline 双缓冲 + 不可变保留阶段 | completed |
| T4 | IM-3 `parallelFor` 内联快路径 | completed |
| T5 | IM-4 Json variant + from_chars | completed |
| T6 | IM-5 Json/ProfileIO `string_view` | completed |
| T7 | IM-7 `ColorManager` RAII | completed |
| T8 | IM-8 SIMD 测量与决策 | completed（只测量，结论维持现状） |
| T9 | IM-10 `FolderMonitor` jthread | completed |
| T10 | IM-11 `MathUtils` constexpr/noexcept | completed |
| T11 | IM-6 `shared_ptr<const Adjustments>`（Mask variant 只做设计） | completed（低风险腿） |
| T12 | IM-9 noexcept/nodiscard | completed |
| T13 | 红线审查修复（FolderMonitor.h 注释 ASCII 结尾 + `<stop_token>`） | completed |
| U1 | IM-6 独立项：Mask → `std::variant`（引擎 + 序列化 + UI + 测试） | completed |
| U2 | IM-6 收尾：UI 信号改 `AdjustmentsPtr` + ExportItem 共享快照 | completed |
| U3 | IM-5 第二批：CatalogDatabase `string_view` 化 + 删除陈旧 `Pipeline.cpp.orig` | completed |
| U4 | IM-8 后续：删除手工 SSE2 stage A/C 批次（287 行） | completed |
| U5 | IM-9 第二批：noexcept/nodiscard 覆盖剩余引擎与工具头 | completed |

依赖关系按用户要求：T3←T1、T6←T5、T8←T3（另 T2←T1）；第二阶段 U2←U1、U4←U1、U5←U1+U2+U3。**写范围全程互斥**：最终 `git status` 的 49 个源码/测试改动文件 + 1 个删除（`Pipeline.cpp.orig`）+ 1 个文档改动，与各任务 `write_scopes` 并集一致。

---

## 6. 未完成项与遗留风险

### 6.1 未完成（阶段二结束后仅剩以下各项）

1. **IM-6 的「最后一次拷贝」（panel → MainWindow）**：U2 已把 4 个 DevelopPanel 信号 + PresetDialog/SnapshotDialog 的 `applied` 改成 `AdjustmentsPtr`，但 MainWindow 仍需保留一份**可编辑**工作副本（撤销/蒙版/污点/HSL 等约 100 个变更点直接改它），所以值不同时仍会发生 1 次拷贝。彻底消除需把编辑状态改成 COW `shared_ptr`，属更大重构。
2. **AI Prompt 点击数不回流**：U1 保留了基线既有行为——`DevelopPanel::updateMaskGeometry` 只同步 `type+invert+linear/radial/brush` 几何，不同步 `color/AI` 参数，因此 ImageViewport 里往 `maskDraft_.ai.points` 加的点不会经这条路回流到 `currentAdjustments_`。这是变体化之前的现状，**不是本轮引入**；若确认是 bug 建议单独立项。
3. **`Mask` variant 的语义收窄（预期内）**：同一 `Mask` 不能再同时持有两种非默认几何体；基线代码没有任何路径能造出这种状态（oracle + 全量测试证明无可观察差异）。若未来把「切换 mask 类型」实现成保留旧几何体，旧几何体会被丢弃——已在 `MaskShape`/`setType` 的 Doxygen 里写清。
4. **IM-9 未覆盖的其余 API**：`Json::Value::type()`（`std::visit` 在 valueless 时会抛）与 `Value` 移动（本机 MSVC STL 的 `std::map` 移动非 noexcept）因正确性原因刻意不加；取锁访问器也刻意回退了 noexcept。
5. **未做的人工 GUI 交互验收**：U2 改了 UI 传参管道与 57 处 emit，无可见 UI 行为变化；已做 build_gui clean 重建（359 步）+ 16 个测试类（含 queued invokeMethod 载荷断言）验证，但没有走完整「导入→逐张渲染→自动→导出→元数据」的人工路径。

### 6.2 遗留风险（按严重度）

1. **`ImageBuffer::create` 零初始化成本**：`renderInternalLinear` 借用路径仍需一次 `data_.resize` 零填充来分配目的缓冲——省掉一次 memcpy 但付一次 memset。要彻底避免需给 `ImageBuffer` 加「不初始化创建」接口（会动布局语义边界，未做）。
2. **`adjustmentsSnapshot()` 依赖 `operator==` 完整性**：未来给 `Adjustments` 加字段若忘记同步 `operator==`/`hash`，会退化为发送陈旧栈（当前已核对 `Adjustments::operator==` 显式比较全部字段，Mask/ColorTarget/AiDenoise/ToneCurvePoint/HSLChannel/SplitTone/SpotRemoval 均完整）。建议在 `Adjustments.h` 里把「新增字段必须同步 operator==/hash」写成硬约定。
3. **JSON 浮点序列化格式被 oracle 钉死**：不能改成 `to_chars` 最短表示，否则 `json.*`/`profile.*` 立即破。
4. **`LRC_ENABLE_SIMD=ON` 非逐位一致**（既有 P2-2 取舍，AVX2 blur 的 FMA）；默认 OFF 逐位一致。**任何人改 SIMD 构建都必须知道这一点**。
5. **Pipeline 稳态多 1 块 idle F32 工作缓冲**（1920×1280 ≈ 29.5 MB），`clearIncrementalCache()` 释放；峰值缓冲数不变。
6. **已清理**：`src/engine/Pipeline.cpp.orig`（git 已跟踪的 2026-08-18 旧副本，2784 行）已在 U3 中 `git rm`（引用检查：CMake 源列表/脚本均未引用，唯一提及是本报告）。
7. **GUI 验收深度有限**：T11 只改了跨线程传参的「管道」，无可见 UI 行为变化；已做 `build_gui` clean 重建 + `lrc_qt_tests`（含真实 queued `invokeMethod` 路径）+ kimi-cu 启动冒烟（窗口完整渲染、正常退出）。**未做**的是一条完整的「导入→逐张渲染→自动→导出→元数据」人工路径；若后续要改可见 UI 行为，按 AGENTS.md §9 仍需真机走一遍。
8. **构建设施**：`msvc_deps_prefix` mojibake 会让 ninja 依赖跟踪失效；重新 configure 后必须重跑 `build_headless/_fix_msvc_deps.ps1 -BuildDir <dir>`，否则增量构建会静默使用陈旧 obj（这是本轮最危险的坑，已写进 §1.3）。

---

## 7. 复现命令

```bat
:: 0) 修 deps 前缀（每次 configure 后都要跑）
powershell -NoProfile -ExecutionPolicy Bypass -File build_headless\_fix_msvc_deps.ps1 -BuildDir build_headless

:: 1) headless clean 构建 + 全量测试
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless --target clean
cmake --build build_headless -j 14
build_headless\tests\lrc_tests.exe

:: 2) 逐位 oracle（把 <DIR> 换成 build_headless）
cl /nologo /std:c++20 /O2 /EHsc /MD -Isrc -Ithird_party -Ithird_party/libraw ^
   build_headless\_bitexact\bitexact.cpp /Fo:<DIR>\_bitexact\ /Fe:<DIR>\_bitexact\bitexact.exe ^
   /link <DIR>\lrc_engine.lib <DIR>\sqlite3.lib <DIR>\sqlite_vec.lib ^
         <DIR>\third_party\lcms2\lcms2.lib <DIR>\third_party\libraw\raw.lib ^
         ws2_32.lib user32.lib advapi32.lib shell32.lib ole32.lib
<DIR>\_bitexact\bitexact.exe   :: 与 modern-cpp-adoption-baseline.md §4 逐行 diff

:: 3) GUI 侧（仅 IM-6 改动需要）
powershell -NoProfile -ExecutionPolicy Bypass -File build_headless\_fix_msvc_deps.ps1 -BuildDir build_gui
cmake --build build_gui -j 6
run_qt_tests.bat
```

---

## 8. 第二阶段：独立项与收尾（2026-10-20）

用户要求把本报告 §6 的遗留项做完。组织方式：**新增 1 名 teammate（mask-owner）负责最大的独立项**，其余复用已有 member；任务板 U1~U5，依赖 U2←U1、U4←U1、U5←U1+U2+U3（写范围互斥不变）。

### 8.1 第二阶段改动

| 任务 | 内容 | 关键数字 / 证据 |
| --- | --- | --- |
| U1 | **IM-6 独立项：`Mask` → `std::variant`**。`MaskShape = variant<LinearGradientMask, RadialGradientMask, BrushGeometry, ColorRangeMask, AiMaskParams, monostate>`；`type` 仍是权威选择器；const 访问器 `std::get_if`（未命中回 static 默认实例，不抛/不竞争），非 const 访问器未命中先 `emplace<T>()`；新增 `setType()/shapeMatchesType()/copyGeometryFrom()`；Pipeline 消费点、ProfileIO 键名/键序/缺省值、`hash()/geometryHash()/operator==` 全部保持 | `sizeof(Mask)` **352 → 248 B**；hash/== 的「可执行语句序列」与 HEAD 逐项一致（63/51/30 项）；mask oracle **34/34 逐行不变**；新增 4 个 `[variant]` 用例 |
| U2 | **IM-6 收尾**：DevelopPanel 4 个信号（57 处 emit）、PresetDialog/SnapshotDialog 的 `applied` 全部改 `AdjustmentsPtr`；`ExportItem::adjustments` 改共享快照（入队即冻结，null 计 per-item 失败）；`MainWindow::adoptPublishedAdjustments()` 值不等才拷贝并复用同一指针 | 一次编辑拷贝数 `1+N` → `1+1`；批量导出入队 0 次拷贝；新增/改写 4 个 Qt 用例（载荷断言 + 共享快照断言） |
| U3 | **IM-5 第二批**：`CatalogDatabase` 的 `findImageByPath/getSetting/searchImages` 改 `std::string_view`；新增 `bindText()`（`data()/size()` + `SQLITE_TRANSIENT`，**空 view 绑空串而非 NULL**）；顺带 `git rm` 陈旧 `src/engine/Pipeline.cpp.orig`（2784 行，无任何引用） | `getSetting("library_root")` 字面量调用从每次构造临时 string 变为零分配；新增字面量/临时/空 view 用例 |
| U4 | **IM-8 后续**：删除手工 SSE2 stage A/C 批次（`applyStageABasicSIMD`/`applyStageCBasicSIMD` + 两个 tail + 两处分派） | **287 行删除**；默认 OFF oracle 27/27、mask 34/34；`SIMD=ON` 仍是与 T8 **逐字节相同**的 5 行 `pl.*` 差异；perf 与 T8 的 a/c1 差全在采样极差内 |
| U5 | **IM-9 第二批**：`Pipeline.h/.cpp`（16 noexcept、10 nodiscard）、`HdrMerge.h`、`Adjustments.h/.cpp`（19 noexcept）、`ProfileIO.h`（4 nodiscard）、`MathUtils.h`、`CatalogDatabase.h`（36 nodiscard）、`FolderMonitor.h/.cpp` | 全仓 `noexcept` **8 → 146**、`[[nodiscard]]` **0 → 75**；**回退了 6 个取 `std::mutex` 访问器的 noexcept**（lock 允许抛，标 noexcept 会 terminate）；tests 的 5 处故意丢弃返回值改 `(void)`，最终 **C4834 = 0** |

### 8.2 第二阶段最终验收（Lead 亲自执行，clean 重建）

| 项 | 结果 |
| --- | --- |
| `build_headless` clean 全量重建 | 298/298 目标，**0 error**，**C4834 = 0** |
| 全量 `lrc_tests.exe` | **510 test cases / 979,162 assertions 全绿**，9 s |
| `[pipeline]` / `[incremental]` | 65 / 2,675、8 / 375,061（与基线计数**完全相同**） |
| 主 oracle（§4） | **27/27 行逐字节相同** |
| Mask oracle（§6） | **34/34 行逐字节相同** |
| `build_gui` | 与最终源码同步（U2 时 clean 359 步 0 error，后续增量 0 error；最终 `ninja: no work to do`） |
| `lrc_qt_tests` | **16 个测试类，0 failed**，exit=0 |
| 红线扫描 | forbidden features 0；engine/catalog 新增行非 ASCII 结尾 0；`git diff --check` 干净；无残留 `.orig`/临时文件 |

### 8.3 第二阶段 perf 复测（窗口 B：所有改动落地后，机器比第一阶段更繁忙）

同一 baseline 二进制（HEAD 源码）与最终二进制交替执行，取中位数：

| 锚点 | before（HEAD） | after（最终） | 变化 |
| --- | --- | --- | --- |
| **B-2** 1920 refine min | 93.40 ms | **77.25 ms** | **−17.3%** |
| B-1 50MP 全尺寸 engine leg | 677.64 ms | 678.19 ms | ±0 |
| D-1 50MP 全尺寸渲染 | 686.39 ms | 710.34 ms | +3.5%（分布重叠，见下） |

补充样本（4+4 交替）：base B-1 中位 686.2 / D-1 682.9（min 675.1）；after B-1 695.3 / D-1 703.2（min **659.1**，比所有 base 样本都快）。

**解读**：
- **B-2 的改进在窗口 A（−32.8%）与窗口 B（−17.3%）都稳定复现**，方向与「每帧少 3 次整幅 F32 memcpy」一致 —— 这是 IM-1 的主收益，也是交互路径（滑块拖动）最关心的指标。
- **B-1/D-1 不构成本轮回归，但收益不可复现**：窗口 A 曾测到 −10.5%/−9.1%，窗口 B 只有 ±0 ~ +3.5%（且 after 的最快样本比所有 base 样本更快）。关键证据是**同一个 base 二进制**在窗口 A 是 531/532 ms、在窗口 B 是 678/686 ms —— 机器状态漂移约 28%，这两条 50MP 内存带宽敏感路径已无法在当前噪声下分辨 <5% 的差异。
- 结论：**以 B-2 作为本轮可归因的性能证据；B-1/D-1 只声明「无可测量回退」。**

### 8.4 统计指标刷新

§4 的表已更新为第二阶段结束时的数字（`std::variant 0→5`、`string_view 0→49`、`[[nodiscard]] 0→75`、`noexcept 8→146`、行数 +2,046）。口径不变：按附录 A 的出现次数计，HEAD 与当前工作树都排除 `*.orig`。

---

## 9. 编译与打包（2026-10-20）

### 9.1 GUI Release 构建

```bat
cmake --build build_gui -j 8                      # Ninja / Release / MSVC 19.44 / Qt 6.8.3
qt\6.8.3\msvc2022_64\bin\windeployqt.exe --release --no-translations --compiler-runtime build_gui\SilverHalide.exe
```

- 构建成功（增量，源码与最终改动一致；此前 U2 时已做过 clean 359 步全量）。
- `windeployqt` 刷新了 `build_gui` 下的 Qt 运行库与插件；`SilverHalide.exe` = 4,820,480 B（2026-10-20 00:18）。
- `lrc_qt_tests`：16 个测试类 0 failed。

### 9.2 载荷暂存（`build_gui/install_stage`）

按 `packaging/installer.iss` 头部注释的约定刷新：

| 内容 | 说明 |
| --- | --- |
| `SilverHalide.exe` | 新 exe |
| Qt 运行库 + 插件 | Qt6Core/Gui/Widgets/Network/Svg/Concurrent、`platforms`/`imageformats`/`iconengines`/`generic`/`networkinformation`/`styles`/`tls`（`robocopy /MIR` 对齐，清掉陈旧插件） |
| ONNX Runtime | `onnxruntime.dll` + `onnxruntime_providers_shared.dll` |
| AI 模型 | `models/` 保留原有 6 个文件（`manifest.json`、nafnet、rmbg2、mobilesam×2），manifest 与 `resources/models` 哈希一致 |
| VC++ 运行库 | `vc_redist.x64.exe`（VS 14.44.35112） |
| DCP / Lensfun | 由 installer.iss 直接从 `build_gui/resources` 打包，不进 stage |

结果：**31 个文件 / 807.7 MB**。

### 9.3 安装包编译

```bat
cd packaging
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer.iss
```

- **Successful compile (19.9 s)**：LZMA2 ultra64 + solid + 多块并行。
- 产物：`dist/Silver Halide Setup 1.0.0.exe` —— **1,028,420,492 B（980.8 MiB）**
- `SHA256 = EE37045D09A1F3EF485A5D99DAEE64BD1CFB3B7748F9B6E6D58E00E65DDA4CDA`（见同目录 `.sha256`）
- 版本信息：ProductName=Silver Halide、FileVersion=1.0.0.0、CompanyName=ASE-128。
- 构建信息与载荷清单另见 `dist/BUILD-INFO.txt`。
- ISCC 的若干 `chinesesimplified` 消息未定义警告：Inno Setup 新版消息在中文语言包中缺翻译，自动回退英文，**不影响包正确性**。

### 9.4 打包冒烟

直接从 `build_gui/install_stage/SilverHalide.exe`（即安装包的实际载荷目录）启动：窗口标题 `Silver Halide — 1.0.0`，左侧库树 / 中央画布 / 右侧编辑面板与色调曲线全部正常渲染，状态栏「就绪」，Alt+F4 正常退出，无残留进程 → **载荷的 Qt DLL/插件/ONNX 依赖齐全**。

**未做**：真实安装到 `Program Files` 的端到端验证（会写注册表/安装目录）。`dist/` 与 `build_gui/` 均被 `.gitignore` 覆盖，未污染仓库。
