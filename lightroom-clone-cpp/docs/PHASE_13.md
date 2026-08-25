# Goal: 实现 AI 蒙版（Select Subject / Sky / Background）

> 原 `docs/archive/PHASE_4.md` 中 §2 独立成 Goal。AI 生成的 mask 并入 PHASE_4 扩展后的
> 蒙版体系，且依赖 PHASE_11 推理基础设施——本 Phase 是 AI 线中依赖链最长的。

## 前置依赖

- **PHASE_11**：ONNX Runtime + `ModelManager` + `AiInferenceEngine`。
- **PHASE_4**：颜色/明亮度范围蒙版扩展后的 `Mask` 结构体与组合权重逻辑——`AiMask`
  作为新蒙版类型并入同一体系，与现有局部调整滑块结合。

## 目标

在现有蒙版面板中新增「选择主体」「选择天空」「选择背景」按钮，自动生成像素级蒙版。

### 1. 模型选择

- **主体分割（Subject / Foreground）**：
  - **RMBG-2-Studio**（BRIA AI，开源可商用，ONNX 格式，专门优化前景/背景分离）。
  - 或 **U²Net**（显著性检测，轻量，ONNX 社区成熟）。
  - 或 **MobileSAM**（用点提示框选主体）。
- **天空分割（Sky）**：
  - **U²Net** 在天空数据集微调的版本，或 **SegFormer-B0** 的 ADE20K 导出（ONNX）。
  - 简化方案：使用通用语义分割模型（如 **PP-LiteSeg** ONNX），取 sky 类别通道。
- **背景分割**：主体蒙版取反即可。

### 2. 处理流程

1. 用户点击「选择主体」：
   - 模型输入：当前预览图像（sRGB，归一化，尺寸通常为 320×320 或 512×512，模型决定）。
   - 推理输出：单通道 mask（0.0–1.0），尺寸与输入一致。
   - 将 mask 上采样/插值到当前预览/全分辨率尺寸。
   - 转换为现有 `Mask` 体系的**画笔蒙版权重图**（或直接作为新的 `AiMask` 类型）。
   - 在预览区以红色叠加显示生成结果，用户可进一步用画笔增减（与现有蒙版局部调整结合）。
2. **天空**：同理，输出天空 mask。
3. **背景**：主体 mask 取反。

### 3. 与现有蒙版体系集成

- 新增蒙版类型 `AiMask`（或复用现有几何蒙版，将 AI 输出作为初始权重图）。
- `AiMask` 参数：
  ```cpp
  struct AiMask {
      enum Target { Subject, Sky, Background };
      Target target;
      float threshold = 0.5f;  // mask 二值化阈值，用户可微调
      float feather = 0.0f;    // 额外羽化
      bool invert = false;
  };
  ```
- AI 蒙版可与现有局部调整滑块（曝光/清晰等）结合，行为与几何蒙版一致；也可与
  PHASE_4 的颜色/明亮度范围蒙版组合（几何 × 颜色 × 明亮度 × AI）。

### 4. UI

- 蒙版面板（`DevelopPanel` 的 Mask 分组）新增三个按钮：
  - 「选择主体 🤖」「选择天空 ☁️」「选择背景 🖼️」。
- 点击后后台推理，预览区显示红色权重叠加。
- 提供「阈值」滑块微调边缘（0–1），以及「羽化」滑块柔化边缘。
- 用户可继续用「橡皮擦」画笔在 AI 蒙版上局部修正。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`AiMaskGenerator` 为纯 C++（输入图像 → 调用模型 → 输出 mask）。
2. **非破坏性原则**：AI 蒙版作为参数层，不修改 RAW。
3. **线程安全**：推理在后台线程执行，主 UI 不阻塞。`Ort::Session` 线程安全通过 Session 池或互斥锁保证。
4. **测试覆盖**：输入标准测试图（如纯色背景+主体），验证 mask 与主体区域 IoU > 0.85。
5. **模型文件不入版本控制**：见 PHASE_11 公共约束。

## 文件清单（需修改/新增）

### 修改

- `src/engine/Pipeline.h` / `.cpp` — 在管线中接入 AI 蒙版权重生成。
- `src/engine/Adjustments.h` / `.cpp` — 新增 `AiMask` 参数结构。
- `src/ui/DevelopPanel.h` / `.cpp` — 新增 AI 蒙版按钮。

### 新增

- `src/engine/AiMaskGenerator.h` / `.cpp` — AI 蒙版生成（输入图像 → 调用模型 → 输出 mask，纯 C++）。
- `tests/TestAiMask.cpp` — 蒙版生成 IoU 验证。

## 验收标准

1. 导入一张人物+天空照片。
2. 蒙版面板点击「选择主体」，2 秒内红色叠加显示人物轮廓，边缘准确（头发丝允许轻微瑕疵）。
3. 点击「选择天空」，天空区域被选中。
4. 在 AI 蒙版上应用「曝光 -1.0」，仅天空变暗，人物不受影响。
5. 用画笔在 AI 蒙版上局部擦除/增加，与现有蒙版体系无缝结合。
6. 连续对 5 张照片使用 AI 蒙版，进程内存不泄漏（AddressSanitizer 无报错）。
7. 后台推理时，UI 仍可浏览图库、切换照片。
8. 模型推理异常（损坏 ONNX），捕获异常并弹窗「模型推理失败」，不崩溃。
9. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- **生成式填充（Generative Fill）**：需 Stable Diffusion/Inpainting，计算量过大，本次不包含。
- **自动主体识别后自动应用调整**（如 Auto Tone）：仅生成蒙版，不自动调参。

## 参考

- PHASE_4 的蒙版组合权重逻辑（几何 × 颜色 × 明亮度）。
- 现有 `Mask` 体系的权重叠加与局部调整逻辑。

## 实现提示

1. **AI 蒙版分辨率**：分割模型通常输出 320×320 或 512×512 mask，需用双线性插值上采样到预览分辨率，边缘用现有 `feather` 滑块二次羽化。
2. **mask 持久化**：AI 生成的是像素权重，存储时以 `AiMask` 参数 + 目标类型为主，权重图按需重生成（或缓存），避免在 Adjustments 里塞大数组。
