# 代码与业务可用性审查报告

审查日期：2026-09-06  
审查范围：`main` 分支提交 `0ac2f54` 及当前工作区代码  
审查目标：识别会导致核心业务无法正常实现、结果与界面承诺不一致、数据污染或运行时不稳定的问题。

## 一、结论摘要

当前无头核心构建和测试通过，但现有自动化测试主要覆盖引擎单元行为，未覆盖完整的导入 -> 元数据 -> 预览 -> 调整 -> 导出 GUI 链路。因此，测试全绿不能证明 DCP、ICC、快照和批量导出选项在产品流程中可用。

本次发现 7 项问题：

| 编号 | 严重度 | 问题 | 结论（2026-09-10 更新） |
| --- | --- | --- | --- |
| R-01 | 高 | DCP 选择未进入渲染管线 | 已修复：DCP 矩阵/曲线注入解码与渲染管线，含回归测试 |
| R-02 | 高 | 标准预览路径不发出相机元数据 | 已修复：标准预览分支补发 `cameraMeta` |
| R-03 | 高 | ICC 只嵌入标签，未转换像素 | 已修复：JPEG/PNG/TIFF-8/16 均做像素到目标 ICC 空间转换，含回归测试 |
| R-04 | 高 | 虚拟副本快照固定使用母版 variant | 已修复：SnapshotDialog 使用当前 `currentVariantId_` |
| R-05 | 中 | Lensfun 暗角和色差只显示支持，实际未处理 | 已修复：UI 仅声明畸变支持，移除死代码 |
| R-06 | 中 | AI 引擎错误状态存在并发数据竞争 | 已修复：错误状态改用专用 `errMutex_`，避免自死锁；TSAN 回归待补 |
| R-07 | 中 | DNG 导出静默忽略 ICC 和水印选项 | 已修复：DNG 不嵌显示 ICC（避免错贴标签），UI 对 DNG 禁用 ICC、TIFF-16/DNG 禁用水印，含回归测试 |

## 二、已执行验证

- `cmake --build build_headless -j`：通过，当前构建树无需重新编译。
- `ctest --test-dir build_headless --output-on-failure`：通过，`1/1` 测试目标通过，总耗时约 2.79 秒。
- GUI 构建/真实 GUI 流程：本次未作为稳定自动化门禁执行；现有工作区包含未提交 UI 改动，且仓库约定 GUI 测试不作为稳定的 ctest 入口。

## 三、问题明细

### R-01 DCP 选择不会改变实际渲染结果

**严重度：高**  
**状态：已修复（2026-09-10）：DCP 矩阵/色调曲线在解码阶段注入并在渲染管线应用；TestRawDecoder 新增 DCP 注入回归测试**

`MainWindow::onDcpProfileSelected()` 读取了 DCP 文件并更新状态文字，但只保存 `currentDcpProfilePath_`，没有将矩阵、曲线或 LUT 写入 `Adjustments`，`Pipeline` 也没有读取该 profile。状态文字明确显示“已读取矩阵/曲线（尚未应用到管线）”。

涉及位置：

- `src/ui/MainWindow.cpp` 的 `onDcpProfileSelected()`
- `src/engine/DcpProfile.cpp`
- `src/engine/Pipeline.cpp`

**业务影响**：用户选择相机 DCP 后，预览和导出仍使用原有相机配置，DCP 选择功能对最终图像没有效果。

**建议验证**：对同一 RAW 在未选择 DCP、选择 DCP 两种状态分别导出 PNG，比较像素或 hash；修复后两者应存在由 profile 引起的可解释差异。

**建议修复**：将 DCP 选择纳入可持久化的调整状态或明确的渲染上下文；在 preview、full-resolution、linear export 三条路径统一应用矩阵/曲线/LUT，并增加参考灰卡或已知输入的颜色回归测试。

### R-02 标准预览命中时跳过相机元数据

**严重度：高**  
**状态：已修复（2026-09-10）：standard preview 分支补发 `cameraMeta`，相机/镜头元数据随之可达**

`PreviewWorker::loadImage()` 在 `catalog_.hasStandardPreview(imageId)` 命中时加载预览并直接发出 `previewReady` 后返回。相机元数据 `cameraMeta` 只在 RAW fallback 分支中发出。`MainWindow::onCameraMeta()` 是刷新 DCP 列表和 Lensfun 匹配的入口。

涉及位置：

- `src/ui/PreviewWorker.cpp` 的 standard preview 分支和 RAW fallback 分支
- `src/ui/MainWindow.cpp` 的 `onCameraMeta()`

**业务影响**：正常导入且已有 standard preview 的图片，通常不会触发 DCP/Lensfun 自动匹配；镜头校正和相机配置列表可能不显示或使用旧状态。

**建议验证**：导入包含 EXIF 相机/镜头信息的 RAW，确保生成 standard preview 后重新加载；监听 `cameraMeta`，当前路径不会收到该信号。

**建议修复**：standard preview 元数据写入预览记录并随 `getStandardPreviewWithMeta()` 返回，或在该分支补充一次轻量 RAW 元数据读取；发出信号时继续使用 generation/imageId 守卫。

### R-03 ICC 导出只嵌入 profile，未转换像素

**严重度：高**  
**状态：已修复（2026-09-10）：`ColorManager::sRGBEncodedToProfile` 将像素经 lcms 转换到目标 ICC 空间后嵌入同名 profile；JPEG/PNG/TIFF-8/16 全路径接入，TestColorManager 新增像素一致性回归**

JPEG、PNG 和 TIFF 导出先通过 `Pipeline::renderFullDisplay()` 得到显示数据，该路径使用 `ColorManager::toDisplaySRGB()` 输出 sRGB 像素；随后 `resolveIccBytes(spec)` 仅将用户选择的 profile 写入文件。没有从 sRGB/工作空间转换到 Adobe RGB 或 Display P3 的像素变换。

涉及位置：

- `src/engine/ExportEngine.cpp` 的 `exportJpeg()`、`exportPng()`、`exportTiff()`
- `src/engine/ColorManager.cpp` 的 `toDisplaySRGB()`

**业务影响**：文件标记为 Adobe RGB 或 Display P3，但像素仍是 sRGB。支持 ICC 的软件会按错误色域解释像素，导致颜色偏移；这不是单纯的元数据问题。

**建议验证**：使用高饱和测试图分别导出 sRGB、Adobe RGB、Display P3，用 LittleCMS 转换到同一参考空间比较像素。当前实现主要只会改变嵌入标签。

**建议修复**：把目标 profile 传入实际的 LittleCMS transform，先完成像素转换再嵌入同一 profile；为 8-bit、16-bit 及自定义 ICC 分别增加像素和 profile 一致性测试。

### R-04 虚拟副本快照固定使用母版 variant

**严重度：高**  
**状态：已修复（2026-09-10）：`SnapshotDialog` 改为使用当前 `currentVariantId_` 构造，快照读写随当前 variant 隔离**

`MainWindow::onManageSnapshots()` 始终以 `variant=0` 构造 `SnapshotDialog`。因此在虚拟副本中打开快照时，列出、创建、读取和删除针对的是母版；应用回调却在 `currentVariantId_ != 0` 时将调整保存到当前虚拟副本。

涉及位置：

- `src/ui/MainWindow.cpp` 的 `onManageSnapshots()`
- `src/ui/SnapshotDialog.cpp`
- `src/catalog/CatalogDatabase.cpp` 的快照 API

**业务影响**：虚拟副本看到的快照与其自身调整不对应，应用母版快照还可能覆盖当前副本，导致编辑结果污染和用户误操作。

**建议验证**：分别给母版和虚拟副本设置明显不同的曝光，创建同名快照；在虚拟副本中管理并应用快照后检查两个 variant 的调整 JSON 和快照列表。

**建议修复**：快照对话框使用当前 `currentVariantId_`；快照 API 的所有读写都携带该 variant，并补充母版/副本隔离的数据库集成测试。

### R-05 Lensfun UI 显示暗角/色差支持，但管线只应用畸变

**严重度：中**  
**状态：已修复（2026-09-10）：UI 仅声明几何畸变支持，移除未使用的暗角实现（LensfunProfile、MainWindow 状态文案）**

`refreshLensfunProfile()` 将 `hasVignetting` 和 `hasChromaticAberration` 显示为“支持”。实际 `LensfunProfile::applyGeometry()` 仅以 `LF_MODIFY_DISTORTION` 初始化并调用 `ApplyGeometryDistortion()`；`Pipeline` 也只调用这个几何阶段，没有暗角或色差处理。

涉及位置：

- `src/ui/MainWindow.cpp` 的 Lensfun 状态文案
- `src/engine/LensfunProfile.cpp` 的 `applyGeometry()`
- `src/engine/Pipeline.cpp` 的 Lensfun 阶段

**业务影响**：用户会认为三类镜头校正均已生效，但实际只修正几何畸变，边缘亮度和色差保持不变。

**建议修复**：要么实现 Lensfun 的 vignetting/TCA 阶段并加入开关，要么按已实现能力分别展示状态，避免把 profile 能力误报为当前渲染能力。

### R-06 AI 错误状态存在并发数据竞争

**严重度：中**  
**状态：已修复（2026-09-10）：错误状态改用专用 `errMutex_` 读写（不与推理 `mutex_` 竞争），消除数据竞争并避免共享锁升级自死锁；并发 TSAN 回归待补**

`AiInferenceEngine::run()` 直接清空和写入 `lastError_`，而 `lastError()` 在 `mutex_` 的 shared lock 下读取。多个 worker 同时调用同一 engine 时，写入并未持有相同 mutex；`loadModel()` 也采用同一未统一保护的错误状态模式。

涉及位置：

- `src/engine/AiInference.cpp` 的 `run()`、`clearError()`、`setError()`
- `src/engine/AiInference.h` 的 `lastError_` 声明

**业务影响**：并发推理失败/成功时可能读到错配的错误信息；C++ 数据竞争属于未定义行为，可能造成不稳定甚至崩溃。

**建议验证**：在启用 ThreadSanitizer 的构建中并发循环调用有效模型和不存在的模型 ID；同时检查导入、预览和 AI 处理是否共享同一 engine 实例。

**建议修复**：所有错误状态读写使用同一互斥量，或将错误作为调用结果返回而不使用共享可变状态；保留每次调用独立的失败信息。

### R-07 DNG 导出静默忽略 ICC 和水印选项

**严重度：中**  
**状态：已修复（2026-09-10）：线性 DNG 不再嵌入显示 ICC（避免错贴标签），UI 对 DNG 禁用 ICC、对 TIFF-16/DNG 禁用水印；TestExportEngine 新增 DNG 不嵌 ICC 回归**

导出对话框对通用导出流程提供 ICC 和水印选项，但 `ExportEngine::exportDng()` 的签名没有 `ExportSpec`，直接把线性数据量化并调用 `ImageIO::writeDng()`。因此 DNG 路径不会读取 ICC 或水印设置；TIFF-16 也按接口注释明确不应用水印。

涉及位置：

- `src/engine/ExportEngine.cpp` 的 `exportDng()`
- `src/engine/ExportEngine.h` 的 DNG 接口和 `ExportSpec` 注释
- `src/ui/ExportDialog.cpp` 的通用导出选项

**业务影响**：用户选择 DNG 后，部分设置被静默丢弃，导出结果与对话框操作不一致。

**建议修复**：DNG 不支持的选项应在 UI 中按格式禁用并说明原因；若产品要求支持，则扩展 DNG writer 写入相应 profile/元数据，并明确水印是否允许进入线性归档格式。至少应加入“选项不可用时不会误导用户”的 GUI smoke 测试。

## 四、优先级建议

1. 先修复 R-01、R-02：它们使 DCP/Lensfun 这条核心业务链在正常 standard preview 流程中失效。
2. 修复 R-03：颜色空间标签与像素不一致会污染外部工作流，且问题不容易从界面察觉。
3. 修复 R-04：快照和虚拟副本涉及用户编辑数据，优先保证 variant 隔离。
4. 处理 R-05、R-07 的产品语义：实现能力或收窄 UI 声明，不能静默丢弃用户选择。
5. 用 TSAN 或显式并发测试确认并修复 R-06，避免将共享错误状态作为跨线程通信手段。

## 五、测试缺口

- 缺少导入 standard preview 命中时的 `cameraMeta` 信号测试。
- 缺少 DCP 选择前后预览/导出像素差异和颜色准确性测试。
- 缺少 ICC 目标空间像素转换与嵌入 profile 一致性测试。
- 缺少母版与虚拟副本的快照隔离、应用和删除集成测试。
- 缺少 DNG 格式与 ICC/水印选项的 GUI 行为测试。
- 缺少 Lensfun 暗角、色差能力的实际像素测试。
- 缺少 AI engine 多线程错误状态的 ThreadSanitizer 回归。

## 六、审查边界与状态说明

本报告没有修改业务代码，也没有覆盖当前工作区中用户未提交的 UI 文件改动。报告中的“已验证”表示代码调用链已经足以证明当前行为；并不表示每项都已通过真实照片端到端复现。GUI 构建和 GUI 流程未作为本次报告的通过条件，相关结论应在修复后用真实 RAW、虚拟副本和导出文件进行验收。

## 七、追加探查：导入与渲染层色彩空间

本节针对导入文件的 ICC/色域、位深和工作空间转换追加审查。检查范围覆盖 `ImageIO`、`RawDecoder`、`ColorManager`、标准预览生成和非 RAW 导入路径。

### R-08 非 RAW 导入忽略源文件 ICC，宽色域文件会被错误当作 sRGB

**严重度：高**  
**状态：已修复（2026-09-11）：`ImageIO::loadSourceImage` 提取嵌入 ICC（JPEG APP2 / PNG iCCP / TIFF 34675），`ColorManager::sourceToLinearProPhoto` 经 lcms 从源空间转换到线性 ProPhoto；TestRawDecoder 新增 JPEG/PNG ICC 回归**

`ImageIO::loadImage()` 使用 stb 或内置 TIFF reader 将 JPEG/PNG/TIFF 统一解码为 8-bit U8，但没有提取或返回嵌入的 ICC profile。内置 TIFF reader 只读取像素和基础 TIFF 参数，也没有处理 ICC tag 34675。随后 `RawDecoder::decodeNonRaw()` 无条件调用 `ColorManager::srgbToLinearProPhoto()`，该转换固定使用内置 sRGB profile。

涉及位置：

- `src/engine/ImageIO.h` 的 `loadImage()` 契约：固定声明输出为 display-ready 8-bit sRGB
- `src/engine/ImageIO.cpp` 的 `loadImage()`、`decodeStbFromMemory()` 和 TIFF reader
- `src/engine/RawDecoder.cpp` 的 `decodeNonRaw()`
- `src/engine/ColorManager.cpp` 的 `srgbToLinearProPhoto()`

**业务影响**：Adobe RGB、Display P3 或自定义 ICC 的照片会按 sRGB 解读。导入后颜色可能出现饱和度、色相和亮度偏差；导出时即使再次嵌入目标 ICC，也无法恢复已经错误转换的源数据。此前 R-03 关注“导出像素与导出标签不一致”，R-08 是相反方向的导入错误，两者会叠加放大跨软件工作流中的色彩偏差。

**建议验证**：制作同一组线性参考数据，分别编码为 sRGB、Adobe RGB 和 Display P3，并嵌入对应 ICC；导入后不做调整，再导出到统一的 sRGB 参考空间，比较 ΔE 或逐像素误差。当前实现会把三种源文件都走同一条 sRGB 输入转换路径。

**建议修复**：让图像解码结果携带源 ICC 或明确的源色彩空间；使用 LittleCMS 将源空间转换到线性 D50 ProPhoto，而不是固定调用 sRGB transform。没有 ICC 时才按明确约定回退到 sRGB，并在 UI/元数据中标记该回退。

### R-09 16-bit TIFF 导入在色彩转换前被截断为 8-bit

**严重度：中**  
**状态：已修复（2026-09-11）：TIFF/PNG 16-bit 源在 `loadSourceImage` 保留原生 U16，`sourceToLinearProPhoto` 以 TYPE_RGB_16 直接经 lcms 转换、不先降到 U8；TestRawDecoder 新增位深保留回归**

内置 TIFF reader 对 16-bit 样本先执行 `uint16 -> uint8` 的四舍五入归一化，返回 U8；`RawDecoder::decodeNonRaw()` 随后才调用 `srgbToLinearProPhoto()`。因此 16-bit TIFF 的额外精度在进入线性工作空间前已经丢失。

涉及位置：

- `src/engine/ImageIO.cpp` 的 TIFF 读取分支：16-bit `rv/gv/bv` 被压缩为 `r8/g8/b8`
- `src/engine/ImageIO.h` 的 `loadImage()` 返回类型和 8-bit 约定
- `src/engine/RawDecoder.cpp` 的 `decodeNonRaw()`

**业务影响**：高位深扫描、合成图和导出 TIFF 重新导入后出现量化台阶，尤其在暗部、渐变和后续大幅曝光调整时更明显。该路径也使“16-bit TIFF 导入后仍保持非破坏性高位深编辑”的预期无法成立。

**建议验证**：生成包含密集暗部渐变和相邻 16-bit 值的 TIFF，导入后检查工作缓冲的唯一级数和相邻差异；对比直接以 F32 读取的参考结果。当前 U8 中间层最多保留 256 个每通道离散值。

**建议修复**：让 TIFF reader 保留 U16/F32 数据和源色彩空间元数据；为 U16 sRGB 或源 ICC 增加对应的 LittleCMS 转换入口，最后再按工作空间需要转为 F32。不要在通用导入 API 内无条件降到 U8。

### R-10 导入后的标准预览和真实 RAW 渲染使用了不同的色彩元数据能力

**严重度：中**  
**状态：已由代码路径确认**

RAW 导入路径通过 libraw 的 `output_color=4` 生成线性 ProPhoto 数据，再由 `fromDcrawProPhoto()` 做 D65→D50 工作空间重锚定；非 RAW 路径则把输入直接视为 sRGB 后转换。标准预览生成器重新创建 `ColorManager` 并调用同一 `RawDecoder`，但预览 blob 只保存 F32 像素和尺寸，不保存源 ICC、相机元数据或色彩配置。

涉及位置：

- `src/catalog/StandardPreview.cpp` 的 `buildStandardPreview2()` 和 blob 编码
- `src/engine/RawDecoder.cpp` 的 RAW/non-RAW 分支
- `src/engine/ColorManager.cpp` 的 D65 ProPhoto 重锚定和 sRGB 输入转换

**业务影响**：standard preview 一旦生成，后续加载只恢复像素，不具备重新解释源 ICC 或补充色彩配置的能力；当导入文件是宽色域非 RAW，错误会被固化进目录缓存。若之后修正 ICC 处理而不升级/失效缓存版本，旧 preview 仍可能继续显示错误颜色。

**建议验证**：用带 Adobe RGB/Display P3 ICC 的非 RAW 文件生成 standard preview，删除源文件 ICC 后重新加载缓存，比较缓存预览与按正确 ICC 重新解码的结果；检查缓存命中时是否存在源色彩配置版本或校验值。

**建议修复**：标准预览缓存记录源色彩配置的稳定标识和编码版本；导入色彩转换策略变化时使旧 blob 失效。预览的元数据至少应包含源 ICC 的 hash、源空间类型和解码器版本，避免错误像素被永久复用。

### R-11 嵌入 ICC 的输入文件无法通过内存导入 API 保持色彩配置

**严重度：中**  
**状态：已修复（2026-09-11）：新增 `ImageIO::loadSourceImageFromMemory`，与 `loadSourceImage` 共用 `decodeSourceBytes`，返回 `SourceImage`（像素 + 内嵌 ICC + 原生位深）**

`ImageIO::loadImage(const uint8_t* data, size_t size)` 只返回 U8 `ImageBuffer`，调用 `decodeStbFromMemory()` 时不返回文件格式、ICC profile 或位深信息。`PreviewWorker` 会用该 API 解码缓存 blob/缩略图；因此任何依赖内存字节的导入或预览路径都无法执行源 ICC 到工作空间的正确转换。

涉及位置：

- `src/engine/ImageIO.h` 的内存 `loadImage()` 接口
- `src/engine/ImageIO.cpp` 的 `decodeStbFromMemory()` 和内存 `loadImage()`
- `src/ui/PreviewWorker.cpp` 的内存图像加载路径

**业务影响**：即使文件路径导入将来增加 ICC 处理，内存路径仍可能按默认 sRGB 解释同一份宽色域内容，造成文件加载、缓存加载和预览加载之间的颜色不一致。

**建议修复**：将解码结果从单一 `ImageBuffer` 升级为带 `PixelFormat`、源色彩空间/ICC、白点和元数据的结构；内存 API 也必须传递或恢复这些字段。缓存格式应把色彩配置纳入版本和校验，而不是只保存像素。

## 八、追加探查后的处理优先级

1. R-08 优先于一般 UI 色彩优化：导入时错误解释会污染后续所有调整、缓存和导出结果。
2. R-09 与 R-08 应共用一次导入数据模型改造，避免修复 ICC 后仍被 U8 中间层截断。
3. R-10、R-11 需要随解码结果/缓存格式一并处理，否则文件路径和缓存路径会继续产生不同颜色。
4. 修复后应重新生成 standard preview，并增加 sRGB、Adobe RGB、Display P3、无 ICC 四类输入的端到端回归样本。

## 九、追加探查：目录、预览、快照与图层状态

### R-12 AI 蒙版无法通过目录/快照 JSON 往返

**严重度：高**  
**状态：已修复（2026-09-10）：`ProfileIO` 序列化/反序列化完整 `ai` 对象，类型上限放宽至 `MaskType::AiMask`；TestMasks 新增往返回归**

`Adjustments::AiMaskParams` 包含目标类型、阈值、羽化、反转、模型输入尺寸、缓存版本和模型覆盖 ID，但 `ProfileIO::toJson()`/`fromJson()` 没有序列化或读取这些字段。更严重的是，`ProfileIO::fromJson()` 将蒙版类型限制在 `MaskType::ColorRange` 的最大值；`MaskType::AiMask` 的枚举值更大，因此任何保存后重新加载的 AI 蒙版都会被截断为颜色范围蒙版。

涉及位置：

- `src/engine/Adjustments.h` 的 `AiMaskParams` 和 `MaskType::AiMask`
- `src/utils/ProfileIO.cpp` 的蒙版序列化/反序列化
- `src/catalog/CatalogDatabase.cpp` 的调整、历史和虚拟副本 JSON 持久化路径

**业务影响**：AI 主体/天空/背景蒙版在保存编辑、创建快照、重启应用或创建虚拟副本后无法恢复为原来的 AI 蒙版。恢复后的图层可能变成颜色范围蒙版，或者退化为默认参数，导致局部调整结果改变。

**建议验证**：创建一个 `AiMask`，设置 Sky、非默认 threshold/feather/model override，执行 `ProfileIO::toJson()` -> `fromJson()`，断言整个 `Adjustments` 逐字段相等；再覆盖 `saveAdjustments`、快照恢复和应用重启后的加载。

**建议修复**：序列化完整的 `ai` 对象；反序列化时允许 `MaskType::AiMask`，对未知类型拒绝或显式降级并报告，而不是静默映射到另一个业务类型。缓存权重版本还应与模型版本/输入图像标识绑定，避免复用错误的 AI mask raster。

### R-13 创建虚拟副本时会丢失命名快照名称

**严重度：中**  
**状态：已修复（2026-09-10）：`createVirtualCopy()` 复制历史时显式复制 `name` 列；TestCatalogDatabase 新增命名快照回归**

`CatalogDatabase::createVirtualCopy()` 复制 `edit_history` 时只选择 `image_id`、`adjustment_json` 和 `created_at`，没有复制新增的 `name` 列。由于快照对话框只展示非空名称，母版中的命名快照复制到虚拟副本后会变成普通未命名历史，用户无法在副本中看到或管理这些快照。

涉及位置：

- `src/catalog/CatalogDatabase.cpp` 的 `createVirtualCopy()` 中 `cpHist` 插入语句
- `src/ui/SnapshotDialog.cpp` 的命名快照过滤逻辑

**业务影响**：创建虚拟副本不是母版编辑状态的完整复制；命名快照资产丢失，且用户无法从界面发现丢失原因。

**建议验证**：在母版创建带名称的快照，创建虚拟副本后调用 `listEditHistory(imageId, variantId)`，检查对应记录的 `name`；当前名称为空。

**建议修复**：复制历史时显式复制 `name`。同时增加带名称快照、未命名历史和多个 variant 的数据库回归测试。

### R-14 文件内容变化不会使目录记录和预览缓存失效

**严重度：高**  
**状态：已修复（2026-09-12）：`FolderMonitor` 记录文件 size/mtime，poll 检测到变化则报告 `modified` 事件；UI 回调对 modified 调用 `invalidatePreviews()` 使预览缓存失效**

`FolderMonitor` 只维护文件路径集合，只报告 added/removed，不比较文件大小、最后修改时间或内容 hash。对同一路径上的 RAW/JPEG/TIFF 进行外部覆盖、重新导出或替换时，监视器不会通知 UI；目录中的 `images` 记录、standard preview、thumbnail/full-res preview 和当前编辑视图仍可能继续使用旧内容。

涉及位置：

- `src/catalog/FolderMonitor.h/.cpp` 的 `known_` 路径集合和 `poll()` 差异检测
- `src/ui/MainWindow.cpp` 的文件夹监视回调
- `src/catalog/CatalogDatabase.cpp` 的 `previews` 表和 `storePreview()`/`removeImage()`

**业务影响**：用户替换原文件后，应用可能显示旧预览并继续基于旧 RAW 编辑；重新打开应用时如果 standard preview 命中，错误内容会被继续复用。外部修改还可能让导出结果与磁盘上的当前源文件不一致。

**建议验证**：导入一张照片并生成 standard preview，保持路径不变地替换文件内容，然后触发多次 `FolderMonitor::poll()`；当前不会产生任何变更事件，也不会删除或重建预览。

**建议修复**：为目录记录和缓存保存文件身份（size、mtime，必要时 hash）；监视器报告 modified 事件。收到 modified 后应原子地失效 previews/fullres 缓存、更新元数据并重新生成预览，或明确提示用户重新导入。缓存键不能只依赖 image ID 或路径。

### R-15 文件夹监视把暂时不可见的文件直接从目录删除

**严重度：中**  
**状态：已验证（当前 UI 回调行为）**

文件夹监视回调收到 `removed` 后，`MainWindow::importFolder()` 立即按路径查找记录并调用 `catalog_->removeImage(rec->id)`。这没有区分真实删除、短暂的网络/移动磁盘不可用、文件被其他程序占用导致的枚举失败，也没有保留“离线但仍在目录”的状态。

涉及位置：

- `src/catalog/FolderMonitor.cpp` 的目录扫描失败/空结果处理
- `src/ui/MainWindow.cpp` 的 `importFolder()` removed 回调
- `src/catalog/CatalogDatabase.cpp` 的 `removeImage()`

**业务影响**：一次临时目录不可访问或移动介质断开，就可能删除目录记录、编辑历史、虚拟副本、快照和嵌入预览；文件恢复后只能作为新照片重新导入，原有编辑关系无法自动恢复。

**建议验证**：导入外部目录中的照片，临时重命名目录或断开网络盘后触发一次 poll，再恢复目录；当前 removed 回调会删除对应 catalog 记录，且 `ON DELETE CASCADE` 会清理相关编辑数据和预览。

**建议修复**：将 removed 建模为 offline/missing 状态，连续多次确认后再提供显式“从目录移除”操作；扫描失败时不要把空目录视为全部文件被删除。真正删除记录前应确认文件系统操作成功并保留可恢复的 catalog 备份。

### R-16 目录删除依赖级联清理，但不清理外部磁盘缓存文件

**严重度：低**  
**状态：已修复（2026-09-11）：`removeImage()` 删除记录前先查询 `thumbnail_path`/`fullres_path`，数据库删除成功后安全删除对应外部缓存文件；缺失/过期文件不报错**

`removeImage()` 只删除 `images` 行，数据库内的 edits、history、variants、previews 等依赖外键级联清理；但 `thumbnail_path`、`fullres_path` 指向的外部缓存文件没有在删除前移除。`ThumbnailCache` 的磁盘清理是全局按容量/LRU 执行，不是按 catalog 删除即时回收。

涉及位置：

- `src/catalog/CatalogDatabase.cpp` 的 `removeImage()`
- `src/catalog/CatalogDatabase.h` 的 `thumbnailPath`/`fullresPath`
- `src/catalog/ThumbnailCache.cpp` 的磁盘缓存回收逻辑

**业务影响**：大量导入后反复从目录移除照片会留下孤儿缩略图和全尺寸缓存，占用磁盘；若缓存路径可复用或按不稳定规则生成，还可能出现旧缓存被误命中。

**建议验证**：导入并生成 thumbnail/full-res 缓存，记录文件数量和大小，调用 `removeImage()` 后检查对应路径是否仍存在；重复执行并观察缓存上限前的磁盘增长。

**建议修复**：删除目录记录时在事务前收集缓存路径，数据库删除成功后安全删除属于该记录的缓存文件；对共享缓存使用引用计数或内容寻址，避免误删其他记录正在使用的文件。

## 十、目录与图层问题的优先级

1. R-12 立即修复，否则 AI 图层无法作为可持久化编辑能力交付。
2. R-14、R-15 共同修复，先建立 modified/offline 状态，再决定何时真正删除目录记录。
3. R-13 修复虚拟副本的快照复制字段，并补齐 variant 隔离回归。
4. R-16 在缓存生命周期整理时处理，避免长期目录操作造成磁盘残留。