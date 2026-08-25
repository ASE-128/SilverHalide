# Goal: 实现 AI 降噪（AI Denoise）

> 原 `docs/archive/PHASE_4.md` 中 §1 独立成 Goal。依赖 PHASE_11 的推理基础设施，
> 渲染插入依赖 PHASE_2 的管线。

## 前置依赖

- **PHASE_11**：ONNX Runtime + `ModelManager` + `AiInferenceEngine`，含 `LRC_ENABLE_AI` 编译开关。
- **PHASE_2**：AI 降噪的预览/分块渲染与性能目标复用预览分辨率管线与 Stage 划分。

## 目标

在 Detail 面板中新增「AI 降噪」按钮，一键去除高 ISO 彩色/亮度噪点，效果优于手动滑块。

### 1. 模型选择

- **推荐模型**：使用开源轻量级 U-Net 或 CBDNet 的 ONNX 导出版本。
  - 候选 1：**DnCNN-BSD68**（去噪经典网络，参数量小，BSD68 训练，ONNX 社区已有导出）。
  - 候选 2：**FFDNet**（快速灵活，支持不同噪声水平输入）。
  - 候选 3：**NAFNet**（NTIRE 2021 冠军，效果极佳，有官方 ONNX 导出脚本）。
- **输入输出**：模型输入为 **sRGB 8bit/16bit 或 归一化 F32 的 RGB 图像**（因开源降噪模型多在 sRGB 训练）。若 RAW 域降噪模型难找，可接受在 **去马赛克后的 ProPhoto RGB → 转 sRGB → 降噪 → 转回 ProPhoto** 的管线中执行。
- **处理流程**：
  1. 用户在 Detail 面板点击「AI 降噪」按钮。
  2. 若模型未下载，弹出下载对话框。
  3. 后台线程（`PreviewWorker` 或专用 `AiWorker`）将当前预览分辨率的图像（或全分辨率，若用户处于 1:1）转为模型输入格式（如 224×224 或全图分块）。
  4. 由于降噪模型通常要求固定输入尺寸或全图推理，对于高分辨率：
     - 若模型支持任意尺寸（如全卷积网络），直接输入预览尺寸。
     - 若模型固定尺寸，采用**重叠分块（tiling）**：将图像切分为 256×256 或 512×512 的块，重叠 32px，分别推理后融合重叠区（线性羽化）。
  5. 推理结果替换原图像像素（作为非破坏性调整层，存储 `aiDenoiseEnabled: bool` 与 `aiDenoiseStrength: 0-100`）。
  6. 预览更新，用户可调整「强度」滑块混合原图与降噪结果。

### 2. 数据模型

- `Adjustments` 新增：
  ```cpp
  struct AiDenoise {
      bool enabled = false;
      float strength = 100.0f;  // 0-100，混合系数
      // 不存储模型版本，由应用自动匹配最新下载模型
  };
  ```

- 管线位置：在手动降噪之后、锐化之前（或作为 Detail 面板的最底层操作）。若降噪模型在 sRGB 空间工作，则在 `toDisplaySRGB` 之后、显示之前执行（仅影响预览与导出，不进入 ProPhoto 管线）。

### 3. UI

- Detail 面板新增「AI 降噪」分组：
  - 按钮「应用 AI 降噪」（首次点击触发模型下载 + 推理）。
  - 滑块「强度」：0–100，默认 100。控制降噪结果与原图的混合比例。
  - 复选框「仅亮度降噪」/「亮度+颜色」（若模型支持，否则仅一个模式）。
  - 进度指示：推理时显示「处理中…」旋转图标，后台完成。

### 4. 性能与回退

- 预览分辨率下（如 2048px 长边），推理时间目标 < 3 秒（CPU，ThinkPad X390）。
- 全分辨率 60MP 导出时，允许分块推理，后台完成，显示进度条。
- 若 ONNX Runtime 未启用编译（`LRC_ENABLE_AI=OFF`），AI 降噪按钮隐藏。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`AiDenoiseProcessor` 为纯 C++ 分块/融合逻辑。
2. **非破坏性原则**：AI 降噪作为参数层，不修改 RAW。
3. **线程安全**：推理在后台线程（`PreviewWorker` 或专用 `AiWorker`）执行，主 UI 不阻塞。
4. **测试覆盖**：输入纯噪点图，验证输出平滑（像素方差下降）。
5. **性能**：预览更新延迟（缓存降噪结果后仅混合）< 200ms。

## 文件清单（需修改/新增）

### 修改

- `src/engine/Pipeline.h` / `.cpp` — 在管线中插入 AI 降噪层（若启用）。
- `src/engine/Adjustments.h` / `.cpp` — 新增 `AiDenoise` 参数结构。
- `src/ui/DevelopPanel.h` / `.cpp` — 新增 AI 降噪按钮/强度滑块。

### 新增

- `src/engine/AiDenoiseProcessor.h` / `.cpp` — AI 降噪分块/重叠/融合逻辑（纯 C++）。
- `tests/TestAiDenoise.cpp` — 降噪效果像素级验证。

## 验收标准

1. 导入 ISO 12800 的 A7C II ARW，打开 Detail 面板。
2. 点击「AI 降噪」，3 秒内预览更新，噪点显著减少，边缘保持清晰。
3. 调整「强度」滑块从 100 → 50，预览实时混合原图与降噪结果，延迟 < 200ms（缓存降噪结果后仅需混合）。
4. 导出 JPEG，AI 降噪效果正确 baked。
5. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- **AI 超分辨率（Super-Resolution）**：如 Real-ESRGAN 放大，本次仅降噪。
- **RAW 域降噪模型**：若找不到现成模型，接受 sRGB 域降噪再转回 ProPhoto 的方案。

## 参考

- 现有手动降噪（PHASE_1 的 Detail 面板）作为对照基准。
- ONNX Runtime C++ API 文档：https://onnxruntime.ai/docs/api/c/

## 实现提示

1. **分块推理是关键**：高分辨率图像（60MP）无法一次性送入内存，AI 降噪必须实现 512×512 重叠分块（overlap 64px）+ 羽化融合，否则输出有接缝。
2. **强度混合**：缓存降噪结果后，「强度」滑块仅做一次逐像素混合（`out = src*(1-s) + denoised*s`），保证实时性。
