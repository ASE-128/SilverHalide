# Goal: 实现 Develop 模块 Detail 面板（锐化 + 手动降噪）

## 当前状态

- 已有右侧面板：Basic（曝光/对比/高光/阴影/白黑/色温/色调/饱和/纹理/清晰度/去朦胧/鲜艳度/颗粒）、HSL、色调分离、色调曲线、点颜色、蒙版局部调整、效果（颗粒/晕影）。
- **缺失**：Detail 面板（锐化四滑块 + 手动降噪五滑块），这是与 Lightroom Classic 差距最大的功能断层。
- 管线调整顺序（engine/Pipeline.cpp）：曝光 → 对比度 → 高光 → 阴影 → 白/黑 → 色调曲线 → 白平衡 → 去朦胧 → 鲜艳度 → HSL → 色调分离 → 点颜色 → 空间阶段（纹理/清晰度/锐化/畸变/颗粒/晕影/蒙版局部）→ sRGB。
- 空间阶段已有纹理/清晰度/颗粒/晕影/蒙版局部，**需在空间阶段插入锐化与降噪**。

## 目标

在 DevelopPanel 右侧新增 **Detail** 面板，实现与 Lightroom Classic 行为一致的锐化与手动降噪功能，所有参数非破坏性持久化到目录数据库，支持 XMP sidecar 往返。

### 1. 锐化（Sharpening）四滑块

- **数量（Amount）**：0–150，默认 40。控制 USM 锐化强度。
- **半径（Radius）**：0.5–3.0，默认 1.0。控制高斯模糊核半径（USM 的模糊半径）。
- **细节（Detail）**：0–100，默认 25。控制高频增益保留量（0=仅低频边缘，100=全频段）。
- **蒙版（Masking）**：0–100，默认 0。基于边缘检测生成蒙版，按住 Alt/Option 键预览黑白蒙版（与 LRC 行为一致：白色=完全锐化，黑色=不锐化）。

技术实现：

- 使用 Unsharp Mask 算法：原图 - 高斯模糊（半径）= 差值，差值 × 数量加到原图。
- **细节**滑块：将差值分解为低频/高频，细节控制高频混合比例。建议通过多尺度分解（如拉普拉斯金字塔或简单的高低频分离）实现。
- **蒙版**滑块：基于 Sobel 或 Laplacian 边缘强度图生成灰度蒙版，阈值由 Masking 控制（0=全白，100=仅最强边缘）。Alt 预览时渲染为灰度图叠加。
- 在 Pipeline 的「空间阶段」、清晰度之后、颗粒之前执行。

### 2. 手动降噪（Noise Reduction）五滑块

- **明亮度（Luminance）**：0–100，默认 0。亮度通道降噪强度。
- **细节（Luminance Detail）**：0–100，默认 50。亮度降噪中的细节保留。
- **对比度（Luminance Contrast）**：0–100，默认 0。控制降噪后局部对比度恢复。
- **颜色（Color）**：0–100，默认 25。色度通道降噪强度（针对彩色噪点）。
- **平滑（Smoothness）**：0–100，默认 50。色度降噪的平滑度/保真度权衡。

技术实现：

- **色度降噪**：将图像转换到 Lab 或 YCbCr 空间，对 a/b 或 Cb/Cr 通道应用双边滤波或基于局部方差的高斯/中值混合滤波。颜色滑块控制滤波强度，平滑度控制空间/颜色域 sigma 比例。
- **亮度降噪**：对 L 通道应用边缘保持滤波（如双边滤波、非局部均值简化版或基于局部方差的自适应高斯）。细节滑块控制滤波核的细节保留（高频增益），对比度滑块在降噪后加回局部对比度（可通过 unsharp mask 或局部直方图均衡简化实现）。
- 在锐化之前执行（先降噪后锐化，避免放大噪点）。

### 3. 数据模型与持久化

- 在 `engine/Adjustments.h` 的 `Adjustments` 结构体新增 `Detail` 子结构体，包含上述 9 个字段（锐化4 + 降噪5）。
- 实现默认值、相等性判断、参数哈希（用于 LRU 缓存）。
- 在 `catalog/CatalogDatabase.cpp` 的 edits 表 JSON 序列化中新增 detail 字段。
- 在 `engine/XmpProfile.cpp` 中映射 XMP crs 字段（crs:Sharpness, crs:SharpenRadius, crs:SharpenDetail, crs:SharpenEdgeMasking, crs:LuminanceSmoothing, crs:ColorNoiseReduction, crs:ColorNoiseReductionDetail, crs:ColorNoiseReductionSmoothness 等），实现与 Lightroom 的 XMP sidecar 往返兼容。
- 未知/越界字段容错，与现有 XmpProfile 行为一致。

### 4. UI 面板

- 在 `ui/DevelopPanel.cpp` 新增 `DetailPanel` 子面板（或直接在 DevelopPanel 内新增分组）。
- 暗色主题 QSS 兼容，滑块风格与现有 Basic 面板一致（自定义 QSlider + QLabel 数值）。
- 支持 Alt/Option 键按住时预览锐化蒙版（黑白图）。需与 `ui/ImageViewport` 协调，在预览渲染路径中支持「蒙版预览模式」。
- 滑块释放后 50ms 防抖写入数据库（与现有 Basic 面板行为一致）。

### 5. 管线集成

- 修改 `engine/Pipeline.cpp` 的 `applyAdjustments()`：
  1. 在空间阶段，颗粒/晕影之前插入降噪（若明亮度&gt;0 或 颜色&gt;0）。
  2. 降噪之后插入锐化（若数量&gt;0）。
  3. 若蒙版&gt;0，锐化前生成边缘蒙版，锐化时逐像素按蒙版权重混合（原图 + 锐化效果 × 蒙版权重）。
  4. 若参数全为默认值（中性参数），走快速路径（跳过该阶段），与现有 HSL/其他中性优化一致。
- 预览渲染：利用现有 `PreviewWorker` 后台线程 + LRU 缓存，确保滑块拖动延迟 &lt; 200ms。
- 直方图实时跟随（现有机制，无需改动，但需确保 Detail 调整后的 8bit 预览数据正确生成）。

## 架构约束（必须遵守）

1. **测试覆盖**：新增测试用例到 `tests/`（Catch2），覆盖：
   - Adjustments 默认值/相等性/哈希
   - Pipeline 锐化/降噪各滑块独立与组合效果
   - XmpProfile Detail 字段往返映射
   - 中性参数快速路径
2. **性能**：高分辨率图像（如 60MP）的 Detail 处理需在后台线程完成，UI 不阻塞。大 sigma 高斯模糊复用现有的快速高斯实现（3-box 滑动窗口）。

## 文件清单（需修改/新增）

### 修改

- `src/engine/Adjustments.h` / `.cpp` — 新增 Detail 结构体
- `src/engine/Pipeline.h` / `.cpp` — 集成降噪与锐化阶段
- `src/engine/XmpProfile.cpp` — XMP 字段映射
- `src/catalog/CatalogDatabase.cpp` — JSON 序列化
- `src/ui/DevelopPanel.cpp` / `.h` — 新增 Detail UI 分组
- `src/ui/HistogramWidget.cpp` — 确保 Detail 调整后直方图正确（通常无需改动，但需验证）

### 新增

- `src/engine/DetailProcessor.h` / `.cpp` — 锐化与降噪算法实现（纯 C++，无 Qt）
- `tests/TestDetailAdjustments.cpp` — 单元测试
- `tests/TestPipelineDetail.cpp` — 管线集成测试

## 验收标准

1. 调整「数量/半径/细节」滑块，预览实时更新，边缘变清晰，延迟 &lt; 200ms。
2. 调整「明亮度/颜色」滑块，彩色噪点与亮度噪点减少，细节滑块保留纹理。
3. 按住 Alt 键，预览区显示锐化蒙版黑白图（边缘白、平坦区黑），松开 Alt 恢复彩色预览。
4. 导出 JPEG，Detail 调整正确 baked 到输出图像。
5. 重启应用，Detail 参数从 `.lrcat` 自动恢复。
6. 写入 XMP sidecar，用 Lightroom Classic 打开，Detail 滑块数值一致。
7. 从 Lightroom 导出的含 Detail 的 XMP 导入本应用，滑块数值一致。
8. `ctest --test-dir build --output-on-failure` 全绿（新增测试通过，原有 272 用例不挂）。
9. 中性参数（全默认值）时，PreviewWorker 性能与调整前持平（快速路径生效）。

## 排除项（不在本次范围）

- AI 降噪（需 ONNX Runtime，本次仅手动算法）
- 镜头校正 / 变换 / 校准面板（后续迭代）
- 裁剪后晕影（已有基础晕影，本次不改动）
- 蒙版局部锐化（蒙版体系已有，但 Detail 是全局调整，局部版后续迭代）

## 参考

- 现有清晰度实现（`engine/Pipeline.cpp` 空间阶段，快速高斯 3-box）可作为性能参考。
- 现有 `Mask` 的权重混合逻辑可参考蒙版预览实现。
- Lightroom Classic 行为：降噪始终先于锐化；蒙版基于边缘强度；Alt 预览为灰度。
