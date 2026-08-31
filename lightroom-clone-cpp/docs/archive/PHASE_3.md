# Goal: 实现裁剪后晕影（Post-Crop Vignette）

> 原 `docs/archive/PHASE_2.md` 中 §3 独立成 Goal。在原 PHASE_2 三个功能中它最独立
> （不依赖蒙版体系、不涉及复杂 UI 交互），故排在最前。

## 前置依赖

- **PHASE_2**：晕影渲染插入「空间阶段（Stage E）、颗粒之后」，权重图缓存与预览实时更新
  依赖 PHASE_2 定型的阶段划分与渲染管线。若尚未实施 PHASE_2，至少需在现有
  `Pipeline` 的空间阶段内完成插入点。

## 目标

将现有晕影升级为裁剪后晕影，晕影中心始终跟随当前裁剪后的图像几何中心。

### 1. 参数

- `amount`: -100 到 +100，默认 0。负值暗角，正值亮角。
- `midpoint`: 0–100，默认 50。控制晕影过渡中点。
- `roundness`: -100 到 +100，默认 0。控制形状（-100 为矩形，+100 为圆形）。
- `feather`: 0–100，默认 50。边缘羽化。
- `highlights`: 0–100，默认 0。保护高光（暗角时减少高光衰减，模拟光学暗角行为）。
- `style`: Enum { Paint Overlay, Color Priority, Highlight Priority }。首次实现可仅支持 **Highlight Priority**（与 LRC 默认一致），其余标记为 TODO。

### 2. 算法实现

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

### 3. UI

- 在 `DevelopPanel` 的 Effects 面板中，将现有晕影扩展为上述五滑块（或新增独立分组）。
- 提供「样式」下拉框（首次仅 Highlight Priority）。
- 预览实时更新。

### 4. XMP 映射

- `crs:PostCropVignetteAmount`, `crs:PostCropVignetteMidpoint`, `crs:PostCropVignetteRoundness`, `crs:PostCropVignetteFeather`, `crs:PostCropVignetteHighlightContrast`（highlights）。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。晕影参数通过纯数据结构传递，UI 在 ui/ 层。
2. **非破坏性原则**：晕影作为参数叠加，不修改原始像素。`Pipeline` 输入只读。
3. **色彩空间**：晕影应用在线性 ProPhoto RGB F32 空间执行。
4. **测试覆盖**：新增 Catch2 测试用例，覆盖：
   - PostCropVignette 中心跟随裁剪矩形（改变裁剪后晕影中心偏移）。
   - XmpProfile 新增字段往返映射。
   - 中性参数快速路径（晕影为 0 时跳过）。
5. **性能**：
   - 晕影权重图可缓存（仅当裁剪矩形或晕影参数变化时重新生成）。
   - 晕影单独调整，预览延迟 < 50ms。

## 验收标准

1. 导入照片，应用 -50 amount 暗角。
2. 未裁剪时，暗角中心在图像中心。
3. 使用裁剪工具裁去左侧 30%，应用裁剪。
4. 暗角中心自动移至裁剪后图像的新几何中心，左侧无暗角残留。
5. 调整 midpoint/roundness/feather，预览实时更新，形状符合预期。
6. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- Paint Overlay / Color Priority 样式：仅实现 Highlight Priority，其余标记 TODO。
- 多频段/多层晕影：仅单组参数。

## 参考

- 现有晕影实现：`engine/Pipeline` 空间阶段或 Effects 中的晕影逻辑。
- Lightroom Classic 行为：Post-Crop Vignette 中心始终跟随裁剪后图像中心。

## 实现提示

1. **中心跟随裁剪**是核心验收点：需确保裁剪应用后，晕影中心读到的几何中心来自「当前裁剪后的图像坐标系」，而非原始图像中心。
2. **权重图缓存**：仅在裁剪矩形或晕影参数变化时重新生成权重图；仅调 `amount` 时可复用权重图直接缩放，避免全图重算。
