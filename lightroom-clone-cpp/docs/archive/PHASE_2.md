# Goal: 实现预览分辨率渲染 + 标准预览缓存 + 增量渲染管线

> **本 Phase 是全项目性能地基**。后续所有 Phase（蒙版/修复/图库/AI）的渲染插入点与
> 性能验收都以本 Phase 定型的 Stage A–F 阶段划分与增量缓存为前提。
> 原文档 `docs/archive/SPEEDUP_GOAL.md` 已归档至 `docs/archive/`。

## 前置依赖

- 无 Phase 级依赖。依赖现有 `PreviewWorker`（50ms 防抖 + LRU 缓存）、`previews` 表
  （320px thumbnail + 1:1 JPEG）、`utils/Parallel` 线程池，均为已交付现状。

## 当前性能瓶颈

当前 `PreviewWorker` 在滑块调整时，对全分辨率 RAW（如 60MP）执行完整管线：`RawDecoder → CameraMatrix → ProPhoto RGB F32 → 全部 Adjustments → toDisplaySRGB → QImage`。虽然已有 LRU 缓存（参数哈希），但缓存未命中时延迟高达数百毫秒，且缓存命中率随调整组合爆炸而降低。

Lightroom Classic 的「无时延」核心在于三条策略：

1. **导入时预生成标准预览**：RAW 解码 + 默认渲染后缓存为 2048px 长边位图，Develop 模块直接从此缓存开始。
2. **预览分辨率渲染**：滑块调整时只处理屏幕适配后的像素数（如 200-300 万），而非 6000 万全像素。
3. **增量渲染状态**：管线拆分为阶段节点，只重算参数变化后的阶段，前面结果复用内存缓存。

本 Goal 要求同时实现这三条策略。

## 目标

### 1. 导入时预生成标准预览缓存（Standard Preview）

在现有 `previews` 表（320px thumbnail + 1:1 JPEG）基础上，新增 **Standard Preview** 类型。

- **生成时机**：`ImportWorker` / `CatalogDatabase::addPhoto()` 导入 RAW 完成后，后台线程生成。
- **内容**：RAW 解码 → 相机矩阵 → 线性 ProPhoto RGB → 应用「零调整默认参数」（曝光 0/对比 0/曲线线性/白平衡 As Shot/其余默认）→ 缩放到 **2048px 长边**（保持比例，短边自适应）→ 存储为 **ProPhoto RGB F32 无损**（或 half-float / 16bit 有损压缩以控制大小）。
- **存储**：
  - 扩展 `catalog/CatalogDatabase` 的 `previews` 表：新增 `preview_type` 字段（TEXT，取值 'thumbnail', 'standard', 'full'），或新增独立 `standard_previews` 表。
  - 标准预览 blob 存储在数据库内（与现有 thumbnail/full 一致），或允许存储为外部文件（如 `<catalog>_previews/<photo_id>_std.dng` / `.f32`），数据库只存路径。优先内部 blob 以保持与现有架构一致。
  - 提供 `CatalogDatabase::getStandardPreview(photoId)` 返回 `std::vector<uint8_t>` 或 `ImageBuffer`。
- **回退**：若标准预览缺失（如旧目录、导入中断），`PreviewWorker` 自动回退到从 RAW 全量解码生成（与当前行为一致），并在后台异步补生成标准预览。

### 2. 预览分辨率渲染管线（Preview-Resolution Pipeline）

重构 `PreviewWorker` 与 `Pipeline`，使 Develop 模块的预览渲染**始终基于屏幕所需分辨率**，而非全分辨率。

- **分辨率决策逻辑**：
  - `ImageViewport` 在每次缩放/窗口尺寸变化时，计算当前视图所需的**逻辑渲染尺寸**（viewport 的 widget 尺寸 × devicePixelRatio，或考虑缩放级别的适配尺寸）。
  - `PreviewWorker` 接收该逻辑渲染尺寸 `targetSize`。
  - **Fit/Fill 视图**（zoom ≤ 1:1）：使用 **Standard Preview**（2048px 长边）作为输入源，将其缩放到 `targetSize`（不超过 2048px），然后执行 `Pipeline` 调整。
  - **1:1 放大视图**（zoom > 1:1）：使用 **1:1 Preview / Full Resolution** 作为输入源，仅渲染可见区域（或整图，若内存允许），然后执行 `Pipeline` 调整。
  - **过渡策略**：1:1 与 Fit 之间（如 2:1 放大）优先使用 1:1 Preview 降采样，而非 Standard Preview 上采样，避免模糊。
- **Pipeline 适配**：
  - `engine/Pipeline` 新增 `renderPreview(const ImageBuffer& source, const Adjustments& adj, const QSize& targetSize)` 接口。
  - `source` 已经是适配 `targetSize` 的 `ImageBuffer`（ProPhoto RGB F32）。
  - 管线内所有空间操作（清晰度/锐化/降噪/颗粒）的核半径、sigma 等参数需按**相对于全分辨率的比例缩放**，确保预览与最终导出视觉效果一致。例如：清晰度 sigma 在 2048px 预览上应为全分辨率的 `(preview_long_edge / full_long_edge)` 倍。
  - 非破坏性原则不变：`source` 只读，输出新 `ImageBuffer`。
- **直方图同步**：
  - `HistogramWidget` 当前从预览 8bit 数据抽样。若预览分辨率降低，直方图统计仍应准确。
  - 要求：预览分辨率渲染后的 8bit 数据与全分辨率渲染后的直方图分布**统计一致**（允许轻微偏差，但不应出现明显截断或偏移）。若偏差过大，可选择在后台异步用全分辨率 F32 数据更新直方图（LRC 行为）。

### 3. 增量渲染状态缓存（Incremental Render State）

在 `Pipeline` 内部实现阶段节点缓存，避免每次滑块调整都重跑整条管线。

- **管线阶段划分**（与现有调整顺序一致）：
  - **Stage A**: 基础光影（曝光 → 对比度 → 高光 → 阴影 → 白/黑）
  - **Stage B**: 色调曲线
  - **Stage C**: 白平衡 → 去朦胧 → 鲜艳度
  - **Stage D**: HSL → 色调分离 → 点颜色
  - **Stage E**: 空间阶段（纹理/清晰度/锐化/降噪/畸变/颗粒/晕影/蒙版局部）
  - **Stage F**: sRGB 输出（色彩空间转换 + Gamma + 8bit 量化）
- **节点缓存机制**：
  - `Pipeline` 内部维护 `RenderState` 结构，包含每个 Stage 的输出 `ImageBuffer` 缓存指针（`std::shared_ptr<ImageBuffer>` 或 `std::optional<ImageBuffer>`）。
  - 每次渲染前，比较 `currentAdjustments` 与 `lastAdjustments` 的各阶段参数哈希：
    - 若 Stage A 参数未变，复用 `cache.afterStageA`。
    - 若 Stage B 参数未变，但 Stage A 变了，重算 Stage A→B，复用 Stage C/D/E/F 缓存（如果它们也未变）。
    - 实际实现：找到**第一个参数发生变化的 Stage**，从该 Stage 开始重新执行到 Stage F，之前的 Stage 直接复用缓存输出作为输入。
  - 缓存失效：当 `source` 图像变化（换照片、换预览分辨率、1:1 区域变化）时，全部缓存清空。
- **内存管理**：
  - 每个 Stage 缓存一张 `targetSize` 分辨率的 F32 ProPhoto 图像。以 2048×1365×3×4 ≈ 33MB 计，6 个 Stage 约 200MB。实际可优化为只缓存**关键节点**（如 Stage A、Stage D、Stage E 输出），而非全部。
  - 要求：提供内存上限控制（如最大缓存 150MB），超限时按 LRU 淘汰非必要节点（至少保留最后一步输出用于增量比较）。

### 4. UI 与线程协调

- **`ImageViewport`**: 新增 `viewportResolutionChanged(QSize targetSize)` 信号，在 resize/zoom 时发射。`PreviewWorker` 订阅此信号，中断当前渲染并重新调度。
- **`PreviewWorker`**:
  - 接收 `targetSize` 后，优先从 `CatalogDatabase` 加载 Standard Preview 到内存。
  - 若 Standard Preview 加载中（异步 I/O），先显示低分辨率 thumbnail 或上次预览，加载完成后切换。
  - 滑块拖动时，使用**预览分辨率 + 增量缓存**快速生成新帧，目标延迟 < 50ms（1920×1080 viewport）。
  - 鼠标释放后，后台线程可异步发起一次**全分辨率渲染**更新 1:1 Preview 缓存（如果用户处于 1:1 视图），但不得阻塞 UI。
- **防抖与中断**：保持现有 50ms 防抖。新调整到达时，若当前 `PreviewWorker` 正在渲染旧请求，应安全中断（检查原子标志位）并启动新请求。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`targetSize` 在 engine 层使用纯 C++ 结构（如 `struct Size { int width, height; }`）传递，ui/ 层负责转换。
2. **非破坏性原则**：`Pipeline` 的输入 `ImageBuffer` 永远只读，所有缓存节点也是不可变（const）复用。
3. **线程安全**：`RenderState` 缓存与 `PreviewWorker` 同在后台线程，但主线程可能读取最后渲染结果。使用原子标志或双缓冲确保无数据竞争。
4. **色彩空间**：Standard Preview 为线性 ProPhoto RGB F32。预览分辨率渲染仍在线性 ProPhoto 中执行，最后 Stage F 转 sRGB。
5. **测试覆盖**：新增测试用例到 `tests/`（Catch2），覆盖：
   - Standard Preview 生成：导入后数据库可读取，分辨率正确，色彩空间正确。
   - Pipeline 预览分辨率渲染：同一组参数在 2048px 与全分辨率下输出视觉一致（允许 < 1% 像素差异 due to 缩放滤波）。
   - 增量渲染：连续调整不同 Stage 的参数，验证缓存命中与未命中的正确性（通过性能计时或内部状态探针）。
   - 分辨率比例缩放：清晰度/锐化核半径在预览分辨率下正确缩放。
6. **性能基准**：提供 `tests/benchmark_preview_latency.cpp`（或集成到现有测试），测量：
   - 仅调整曝光（Stage A）的预览延迟：目标 < 20ms（1920×1080）。
   - 仅调整清晰度（Stage E）的预览延迟：目标 < 80ms。
   - 全参数随机调整的首次延迟：目标 < 150ms。
   - 对比基准：记录优化前全分辨率渲染的延迟作为参考。

## 验收标准

1. 导入 10 张 A7C II ARW（60MP），后台自动完成 Standard Preview 生成，数据库 `previews` 表（或对应表）可查询到 2048px 长边的标准预览记录。
2. 打开任意照片进入 Develop 模块，Fit 视图下拖动「曝光」滑块，预览更新延迟 < 50ms（ThinkPad X390 或同级硬件，1920×1080 viewport）。
3. 拖动「清晰度」滑块，预览更新延迟 < 100ms。
4. 连续快速拖动多个滑块（曝光→对比→高光），预览保持流畅不卡顿，无闪烁。
5. 放大到 1:1 视图，预览自动切换到全分辨率渲染，细节清晰，无马赛克。
6. 缩小回 Fit 视图，自动回到 Standard Preview 路径，响应迅速。
7. 同一张照片，仅调整 Stage A 参数后，再调整 Stage D 参数，验证通过内部日志或测试探针：Stage A 未重新执行，从 Stage D 开始重算。
8. 导出 JPEG（全分辨率），与旧版本（全分辨率管线）对比，像素差异 < 1%（预览分辨率渲染不应影响最终导出质量）。
9. 标准预览缺失时（手动删除数据库记录测试），自动回退到 RAW 解码，UI 不崩溃，后台自动补生成。
10. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。
11. 内存占用：打开 10 张照片连续编辑，进程内存增长可控（增量缓存总量不超过 200MB，或提供配置项）。

## 参考

- 现有 `Pipeline::applyAdjustments()` 的调整顺序与阶段划分。
- 现有 `PreviewWorker` 的 50ms 防抖 + LRU 缓存机制。
- 现有 `previews` 表的 320px thumbnail + 1:1 JPEG 存储模式。
- Lightroom Classic 行为：Standard Preview 为 DNG 兼容的线性 RGB；预览时按屏幕分辨率处理；滑块释放后才写数据库。

## 实现提示

1. **先实现 Standard Preview 生成与存储**，再改 PreviewWorker 使用它，最后加入增量缓存。分步验证，不要一次全改。
2. **核半径比例缩放**是关键细节：清晰度/锐化/降噪的 sigma 在预览分辨率下必须按比例缩小，否则预览会过锐/过糊。公式：`preview_sigma = full_sigma * (preview_long_edge / full_long_edge)`。
3. **增量缓存的哈希比较**：`Adjustments` 结构体可按 Stage 分组计算局部哈希（如 `hashBasic()`, `hashHsl()`, `hashSpatial()`），避免每次全量哈希。
4. **线程安全注意**：`PreviewWorker` 可能在中途被新请求中断。`RenderState` 缓存应使用 `std::shared_ptr<const ImageBuffer>` 共享不可变数据，避免拷贝。
