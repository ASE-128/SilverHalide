
# Goal: 实现局部精度与修复能力（Phase 2）

## 目标

### 1. 高级蒙版 — 颜色范围（Color Range）与明亮度范围（Luminance Range）

在现有蒙版体系上扩展两种基于像素内容的范围蒙版，可与现有几何蒙版组合使用。

#### 1.1 颜色范围蒙版（ColorRangeMask）
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

#### 1.2 明亮度范围蒙版（LuminanceRangeMask）
- **参数**：
  - `range`: 0–100，默认 50。基于像素的 L*（Lab）或 Y（YCbCr/相对亮度）值与参考亮度的容差。
  - `smoothness`: 0–100，默认 50。边缘羽化。
- **权重生成逻辑**：
  - 计算图像每个像素的相对亮度 `L = 0.2126×R + 0.7152×G + 0.0722×B`（ProPhoto RGB 线性空间，或转 Lab 取 L*）。
  - 用户通过吸管在预览区选取参考亮度 `L_ref`。
  - `range` 控制 `|L - L_ref|` 的接受阈值，超出阈值范围按 smoothstep 衰减至 0。
  - `smoothness` 对权重图高斯模糊。
- **组合**：与几何蒙版、颜色范围蒙版可三级组合（几何 × 颜色 × 明亮度）。

#### 1.3 UI 交互
- 在 `DevelopPanel` 的蒙版分组中，每个蒙版条目（线性/径向/画笔）下方新增「范围」子面板。
- 提供「颜色范围」与「明亮度范围」两个复选框/开关，开启后显示对应滑块。
- **吸管工具**：点击后在 `ImageViewport` 中鼠标变为吸管，点击预览区任意像素，自动提取该点 ProPhoto RGB 值作为 `referenceColor` 或亮度作为 `L_ref`。吸管需支持 1:1 放大下精确选取。
- **范围可视化**：按住某个修饰键（如 Shift）时，预览区以红色叠加显示当前蒙版的权重图（与现有红色权重叠加一致，但需支持组合后的最终权重）。

#### 1.4 数据模型
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
- XMP 映射：映射到 `crs:ColorRangeMask` / `crs:ColorRangeMaskRange` / `crs:ColorRangeMaskSmoothness` 及 `crs:LuminanceRangeMask` 等 Lightroom 兼容字段（若 LRC 使用内部格式，则参照其 sidecar 样本实现往返）。

---

### 2. 污点去除与修复画笔（Spot Removal / Healing Brush）

实现非破坏性的传感器灰尘/瑕疵修复工具。

#### 2.1 数据模型

- 在 `engine/Adjustments.h` 新增：
  
  ```cpp
  struct SpotRemoval {
      enum Type { Clone, Heal };
      Type type;
      float sourceX, sourceY;   // 源点坐标（归一化 0-1 或像素坐标，需定义标准）
      float targetX, targetY;   // 目标点坐标
      float radius;             // 画笔半径（像素，基于全分辨率）
      bool visible;
  };
  std::vector<SpotRemoval> spotRemovals;
  ```

- 坐标系统：使用**归一化坐标**（0.0–1.0 相对于全分辨率图像尺寸），确保裁剪/旋转后位置仍正确。或在渲染时根据当前裁剪/变换矩阵动态映射。

- 每个污点为一个圆形区域（半径 `radius`），UI 中显示为可拖拽的圆。

#### 2.2 算法实现

- **仿制（Clone）**：
  - 对目标圆形区域内的每个像素，从源点对应偏移位置直接复制像素值（ProPhoto RGB F32）。
  - 需考虑源点与目标点的光照一致性（简单复制即可，LRC 的 Clone 也是直接复制）。
- **修复（Heal）**：
  - 使用**泊松融合（Poisson Blending）**简化版，或**亮度混合 + 颜色匹配**：
    1. 从源区域提取纹理（高频）与颜色/亮度（低频）。
    2. 将源纹理叠加到目标区域，但保留目标区域的原始亮度梯度（或反之：保留源纹理，调整亮度/颜色以匹配目标边缘）。
    3. 简单实现：目标区域像素 = 源区域像素 × (目标边缘平均亮度 / 源边缘平均亮度)，使接缝处亮度连续。
    4. 边缘羽化：圆形边界内 `radius` 的 80%-100% 范围做 alpha 渐变，与底图混合。
  - 若泊松融合实现复杂，可先采用「纹理复制 + 边缘羽化 + 局部亮度匹配」的简化方案，后续迭代优化。
- **渲染时机**：
  - 在 `Pipeline` 的**空间阶段之后、最终 sRGB 转换之前**执行（ProPhoto RGB F32 空间）。
  - 遍历 `spotRemovals`，按顺序叠加（后画的覆盖先画的）。
  - 若 `spotRemovals` 为空，走快速路径跳过。

#### 2.3 UI 交互

- 在 `DevelopPanel` 新增 **Spot Removal** 面板（或作为工具栏按钮）。
- 提供「修复（Heal）」与「仿制（Clone）」两个模式按钮，以及「半径」滑块。
- **交互流程**（与 LRC 一致）：
  1. 用户选择工具后，在 `ImageViewport` 中鼠标变为十字准星。
  2. 在瑕疵处单击并拖动（或直接单击），创建一个圆形选区（目标点）。
  3. 自动或手动指定源点：LRC 行为是自动寻找相似纹理的源点，但**首次实现可要求用户按住 Alt/Option 单击指定源点**，或自动将源点置于目标点右侧固定偏移（简化交互）。
  4. 已创建的污点显示为两个相连的圆（目标圆 + 源圆），可拖拽移动位置。
  5. 按 Delete 或右键删除选中污点。
  6. 提供「可视化污点」按钮：预览区反相 + 高对比度显示，帮助发现灰尘点（临时切换显示模式，不改变参数）。
- **1:1 视图**：修复工具必须在 1:1 放大下工作，坐标映射需处理缩放与平移。

#### 2.4 XMP 映射

- 映射到 `crs:SpotCorrections` 数组，包含 `crs:CorrectionType`（Clone/Heal）、`crs:CorrectionSource`（Source/Auto）、`crs:CorrectionAmount`（不透明度，本次固定 100）、`crs:CorrectionMask`（圆形参数：CenterX/Y, Radius）。
- 未知字段容错，与现有 `XmpProfile` 行为一致。

---

### 3. 裁剪后晕影（Post-Crop Vignette）

将现有晕影升级为裁剪后晕影，晕影中心始终跟随当前裁剪后的图像几何中心。

#### 3.1 参数

- `amount`: -100 到 +100，默认 0。负值暗角，正值亮角。
- `midpoint`: 0–100，默认 50。控制晕影过渡中点。
- `roundness`: -100 到 +100，默认 0。控制形状（-100 为矩形，+100 为圆形）。
- `feather`: 0–100，默认 50。边缘羽化。
- `highlights`: 0–100，默认 0。保护高光（暗角时减少高光衰减，模拟光学暗角行为）。
- `style`: Enum { Paint Overlay, Color Priority, Highlight Priority }。首次实现可仅支持 **Highlight Priority**（与 LRC 默认一致），其余标记为 TODO。

#### 3.2 算法实现

- 在 `Pipeline` 的空间阶段、颗粒之后执行（或作为 Effects 的一部分）。
- **中心计算**：
  - 获取当前裁剪矩形的中心点（归一化坐标），而非原始图像中心。
  - 若未裁剪，则与原始中心一致。
- **权重图生成**：
  - 基于每个像素到裁剪中心的距离 `d`，以及裁剪后图像的长宽比，计算椭圆/圆距离。
  - `roundness` 控制椭圆程度：-100 时按矩形最大内接椭圆计算，+100 时按正圆计算，中间插值。
  - `midpoint` 控制 `d` 的缩放：midpoint 高则晕影范围小（仅边缘），midpoint 低则范围大（接近中心）。
  - `feather` 控制 smoothstep 过渡的陡度。
  - `amount` 控制最终像素的乘法因子（暗角：`(1 + amount/100 × weight)`，其中 weight 边缘大中心小）。
  - `highlights`：对高光区域（亮度 > 阈值）减少暗角强度，使用亮度掩码混合。
- **与现有晕影的关系**：
  - 若现有晕影是基于原始图像的，保留作为「Effects Vignette」，但新增「Post-Crop Vignette」作为独立参数组。
  - 或替换现有晕影为 Post-Crop 版本（推荐，因为 LRC 的晕影本质就是 Post-Crop）。

#### 3.3 UI

- 在 `DevelopPanel` 的 Effects 面板中，将现有晕影扩展为上述五滑块（或新增独立分组）。
- 提供「样式」下拉框（首次仅 Highlight Priority）。
- 预览实时更新。

#### 3.4 XMP 映射

- `crs:PostCropVignetteAmount`, `crs:PostCropVignetteMidpoint`, `crs:PostCropVignetteRoundness`, `crs:PostCropVignetteFeather`, `crs:PostCropVignetteHighlightContrast`（highlights）。

---

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。所有 UI 交互逻辑（吸管、鼠标拖拽、污点可视化）必须在 ui/ 层实现，通过纯数据结构传递坐标/参数。
2. **非破坏性原则**：`Pipeline` 的输入 ImageBuffer 永远只读。污点去除、晕影、蒙版权重均作为参数叠加，不修改原始像素。
3. **线程安全**：`PreviewWorker` 在后台线程运行。`Adjustments` 的读取与 `Pipeline` 渲染必须是线程安全的（`Adjustments` 按值传递或拷贝）。
4. **色彩空间**：所有像素级操作（蒙版权重计算、污点修复、晕影应用）在线性 ProPhoto RGB F32 空间执行。
5. **测试覆盖**：新增 Catch2 测试用例，覆盖：
   - ColorRangeMask / LuminanceRangeMask 权重计算（已知参考色/亮度的期望权重图）。
   - 蒙版组合逻辑（几何 × 颜色 × 明亮度的乘法/加法）。
   - SpotRemoval Clone/Heal 在测试图像上的像素验证（圆形区域内像素被正确替换/混合）。
   - PostCropVignette 中心跟随裁剪矩形（改变裁剪后晕影中心偏移）。
   - XmpProfile 新增字段往返映射。
   - 中性参数快速路径（无蒙版范围/无污点/晕影为 0 时跳过）。
6. **性能**：
   - 颜色/明亮度范围蒙版的权重图生成需在后台线程完成，UI 不阻塞。
   - 污点数量 ≤ 50 时，预览延迟增加 < 30ms。
   - 晕影权重图可缓存（仅当裁剪矩形或晕影参数变化时重新生成）。

---

## ## 验收标准

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

4. **污点去除（仿制）**：
   
   - 导入一张天空有灰尘点的 ARW，1:1 放大。
   - 选择仿制工具，半径 20px，在灰尘点单击创建目标圆。
   - 按住 Alt 在干净天空处单击指定源点。
   - 灰尘点被干净天空纹理完全覆盖，边界无硬边。

5. **污点去除（修复）**：
   
   - 同一张图，选择修复工具。
   - 在灰尘点创建目标圆，指定源点。
   - 修复后纹理与周围光照一致，无可见接缝。
   - 创建 20 个污点，拖动滑块时预览延迟增加 < 50ms。

6. **可视化污点**：
   
   - 点击「可视化污点」按钮，预览区切换为反相高对比度显示，灰尘点清晰可见。
   - 再次点击恢复正常预览。

7. **裁剪后晕影**：
   
   - 导入照片，应用 -50 amount 暗角。
   - 未裁剪时，暗角中心在图像中心。
   - 使用裁剪工具裁去左侧 30%，应用裁剪。
   - 暗角中心自动移至裁剪后图像的新几何中心，左侧无暗角残留。
   - 调整 midpoint/roundness/feather，预览实时更新，形状符合预期。

8. **XMP 往返**：
   
   - 编辑含颜色范围蒙版、3 个污点、裁剪后晕影的照片，写入 XMP sidecar。
   - 用 Lightroom Classic 打开，蒙版范围/污点位置/晕影参数一致（允许坐标系换算误差 < 2px）。
   - LRC 创建的同类编辑 XMP 导入本应用，参数一致。

9. **性能**：
   
   - 含颜色范围蒙版的局部调整，预览延迟 < 200ms（1920×1080 viewport）。
   - 含 10 个污点的照片，预览延迟 < 150ms。
   - 晕影单独调整，预览延迟 < 50ms。

10. **测试**：
    
    - `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

---

## 排除项（不在本次范围）

- **AI 蒙版**（主体/天空/对象自动识别）：需深度学习模型，本次仅基于像素值的规则蒙版。
- **内容识别填充（Content-Aware Fill）**：污点修复仅支持单点 Clone/Heal，不支持大面积复杂纹理生成。
- **红眼校正**：属于修复工具子集，本次不包含。
- **污点自动源点选择**（LRC 的自动寻找相似纹理）：首次实现要求用户手动指定源点（Alt+点击），自动选择标记为 TODO。
- **蒙版「并集」以外的布尔运算**（差集/对称差）：仅支持交集与并集两种模式。

## 参考

- 现有蒙版体系：`engine/Adjustments.h` 中 `Mask` 结构体，`ui/DevelopPanel` 蒙版分组，`Pipeline` 空间阶段的蒙版局部调整应用逻辑。
- 现有晕影实现：`engine/Pipeline` 空间阶段或 Effects 中的晕影逻辑。
- Lightroom Classic 行为：Color Range / Luminance Range 基于吸管选取；污点去除为圆形区域 Clone/Heal；Post-Crop Vignette 中心始终跟随裁剪后图像中心。

## 实现提示

1. **分步验证**：先实现 Post-Crop Vignette（相对独立），再实现 Color/Luminance Range（依赖现有蒙版体系），最后实现 Spot Removal（涉及 UI 交互最多）。
2. **坐标系统**：Spot Removal 使用归一化坐标（0.0–1.0）存储，渲染时根据当前全分辨率尺寸映射为像素坐标。若后续引入裁剪/旋转，需用变换矩阵映射，本次可先假设无几何变换（或直接使用当前裁剪后的坐标系）。
3. **泊松融合简化**：若完整实现过于复杂，Heal 可采用「源区域纹理 × 目标区域局部亮度均值 / 源区域局部亮度均值」的简化亮度匹配方案，配合边缘羽化，效果已接近 LRC 的 Heal。
4. **蒙版权重缓存**：颜色/明亮度范围权重图仅在参考色/亮度或图像内容变化时重新计算。若仅调整 range/smoothness，可基于已计算的原始距离图快速重新映射，避免全图重算。
