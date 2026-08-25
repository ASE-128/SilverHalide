# Goal: 实现颜色范围（Color Range）与明亮度范围（Luminance Range）蒙版

> 原 `docs/archive/PHASE_2.md` 中 §1 独立成 Goal。扩展现有几何蒙版体系，是后续
> PHASE_13（AI 蒙版）集成到蒙版体系的必要前置——AI 生成的 mask 将并入本 Phase
> 扩展后的 Mask 结构体与组合权重逻辑。

## 前置依赖

- **PHASE_2**：蒙版权重图的生成/缓存与预览叠加依赖 PHASE_2 的渲染管线与性能框架；
  权重图需在后台线程生成，UI 不阻塞。
- 现有几何蒙版体系（`engine/Adjustments.h` 中 `Mask` 结构体，`ui/DevelopPanel` 蒙版分组，
  `Pipeline` 空间阶段的蒙版局部调整应用逻辑）。

## 目标

在现有蒙版体系上扩展两种基于像素内容的范围蒙版，可与现有几何蒙版组合使用。

### 1. 颜色范围蒙版（ColorRangeMask）

- **参数**：
  - `referenceColor`: ProPhoto RGB F32 参考色（通过预览区吸管工具选取）。
  - `range`: 0–100，默认 50。控制色相/饱和度容差范围（越大选中越多）。
  - `smoothness`: 0–100，默认 50。控制选区边缘羽化程度（高斯模糊或平滑过渡）。
- **权重生成逻辑**：
  - 将预览/渲染图像的每个像素与 `referenceColor` 在 ProPhoto RGB 空间计算欧氏距离（或转 HSV 后计算色相环距离 + 饱和度差异的加权和）。
  - `range` 映射为距离阈值：距离 < threshold 则权重 1，距离 > threshold×(1+falloff) 则权重 0，中间 smoothstep 过渡。
  - `smoothness` 对生成的二值/渐变权重图做高斯模糊（sigma 与 smoothness 成正比），柔化边缘。
- **与几何蒙版组合**：
  - 提供「交集（Multiply）」与「并集（Add/Max）」两种组合模式，默认交集。
  - 最终蒙版权重 = `geometricMaskWeight × colorRangeWeight`（交集模式）。
  - 现有局部调整滑块应用于此组合后的权重图。

### 2. 明亮度范围蒙版（LuminanceRangeMask）

- **参数**：
  - `range`: 0–100，默认 50。基于像素的 L*（Lab）或 Y（YCbCr/相对亮度）值与参考亮度的容差。
  - `smoothness`: 0–100，默认 50。边缘羽化。
- **权重生成逻辑**：
  - 计算图像每个像素的相对亮度 `L = 0.2126×R + 0.7152×G + 0.0722×B`（ProPhoto RGB 线性空间，或转 Lab 取 L*）。
  - 用户通过吸管在预览区选取参考亮度 `L_ref`。
  - `range` 控制 `|L - L_ref|` 的接受阈值，超出阈值范围按 smoothstep 衰减至 0。
  - `smoothness` 对权重图高斯模糊。
- **组合**：与几何蒙版、颜色范围蒙版可三级组合（几何 × 颜色 × 明亮度）。

### 3. UI 交互

- 在 `DevelopPanel` 的蒙版分组中，每个蒙版条目（线性/径向/画笔）下方新增「范围」子面板。
- 提供「颜色范围」与「明亮度范围」两个复选框/开关，开启后显示对应滑块。
- **吸管工具**：点击后在 `ImageViewport` 中鼠标变为吸管，点击预览区任意像素，自动提取该点 ProPhoto RGB 值作为 `referenceColor` 或亮度作为 `L_ref`。吸管需支持 1:1 放大下精确选取。
- **范围可视化**：按住某个修饰键（如 Shift）时，预览区以红色叠加显示当前蒙版的权重图（与现有红色权重叠加一致，但需支持组合后的最终权重）。

### 4. 数据模型

- 在 `engine/Adjustments.h` 的 `Mask` 结构体（或对应类型）中新增：
  ```cpp
  struct ColorRange {
      float r, g, b;      // referenceColor in ProPhoto RGB
      float range;        // 0-100
      float smoothness;   // 0-100
      bool enabled;
  };
  struct LuminanceRange {
      float referenceL;   // 0-100 (L* or relative luminance %)
      float range;        // 0-100
      float smoothness;   // 0-100
      bool enabled;
  };
  ```

- 蒙版结构体包含 `ColorRange colorRange` 与 `LuminanceRange luminanceRange`。
- 实现 JSON 序列化、相等性判断、哈希。
- **哈希注意**：本 Phase 新增字段必须并入 PHASE_2 定型的按 Stage 分组哈希（蒙版属于空间阶段/Stage E），否则增量缓存失效。
- XMP 映射：映射到 `crs:ColorRangeMask` / `crs:ColorRangeMaskRange` / `crs:ColorRangeMaskSmoothness` 及 `crs:LuminanceRangeMask` 等 Lightroom 兼容字段（若 LRC 使用内部格式，则参照其 sidecar 样本实现往返）。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。吸管等 UI 交互逻辑在 ui/ 层，通过纯数据结构传递坐标/参数。
2. **非破坏性原则**：蒙版权重作为参数叠加，不修改原始像素。
3. **线程安全**：`PreviewWorker` 在后台线程运行。`Adjustments` 按值传递或拷贝。
4. **色彩空间**：所有像素级操作（蒙版权重计算）在线性 ProPhoto RGB F32 空间执行。
5. **测试覆盖**：新增 Catch2 测试用例，覆盖：
   - ColorRangeMask / LuminanceRangeMask 权重计算（已知参考色/亮度的期望权重图）。
   - 蒙版组合逻辑（几何 × 颜色 × 明亮度的乘法/加法）。
   - XmpProfile 新增字段往返映射。
   - 中性参数快速路径（无蒙版范围时跳过）。
6. **性能**：
   - 颜色/明亮度范围蒙版的权重图生成需在后台线程完成，UI 不阻塞。
   - 权重图仅在参考色/亮度或图像内容变化时重新计算；仅调 range/smoothness 时可基于已计算的原始距离图快速重新映射，避免全图重算。

## 验收标准

1. **颜色范围蒙版**：
   - 导入一张蓝天+白云的照片，创建径向蒙版覆盖整图。
   - 开启「颜色范围」，吸管点击蓝天，调整 range 使仅蓝天被选中（白云排除）。
   - 拖动「曝光」局部滑块，仅蓝天变暗，白云不受影响。
   - 红色权重叠加（Shift 键）显示选中区域与蓝天轮廓吻合。
2. **明亮度范围蒙版**：
   - 同一张照片，创建画笔蒙版覆盖整图。
   - 开启「明亮度范围」，吸管点击暗部山脉。
   - 调整 range 使仅山脉被选中，天空高光排除。
   - 拖动「清晰度」局部滑块，仅山脉纹理增强。
3. **蒙版组合**：
   - 同时启用几何蒙版 + 颜色范围 + 明亮度范围，验证最终效果为三者交集。
   - 切换为并集模式（若实现），验证行为正确。
4. **性能**：
   - 含颜色范围蒙版的局部调整，预览延迟 < 200ms（1920×1080 viewport）。
5. **测试**：
   - `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- **AI 蒙版**（主体/天空/背景自动识别）：需深度学习模型，由 **PHASE_13** 承接，本 Phase 仅基于像素值的规则蒙版。
- **蒙版「并集」以外的布尔运算**（差集/对称差）：仅支持交集与并集两种模式。

## 参考

- 现有蒙版体系：`engine/Adjustments.h` 中 `Mask` 结构体，`ui/DevelopPanel` 蒙版分组，`Pipeline` 空间阶段的蒙版局部调整应用逻辑。
- Lightroom Classic 行为：Color Range / Luminance Range 基于吸管选取。

## 实现提示

1. **分步验证**：先实现纯算法（权重生成 + 组合逻辑，可在 engine 层独立测试），再接入 UI 吸管与红色叠加。
2. **权重缓存**：仅调整 range/smoothness 时，可基于已计算的原始距离图快速重新映射阈值与平滑，避免全图重算。
