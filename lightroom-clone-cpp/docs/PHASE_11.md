# Goal: 搭建 AI 推理基础设施（ONNX Runtime + ModelManager + AiInferenceEngine）

> 原 `docs/archive/PHASE_4.md` 中 §0 独立成 Goal，并承载 PHASE_4 的公共约束（可选编译、
> 模型不入版本控制）。是 PHASE_12（AI 降噪）、PHASE_13（AI 蒙版）、PHASE_14（重复检测）
> 的共同前提——先跑通端到端推理，再逐个接入具体 AI 功能。

## 前置依赖

- 无 Phase 级硬依赖（构建系统层面引入 ONNX Runtime 不影响已有功能）。
- 渲染插入点约定来自 PHASE_2（Stage E 等）；本 Phase 本身只搭推理后端，不接渲染。

## 项目背景与核心约束

Silver Halide 使用 C++20 + Qt6 Widgets，非破坏性编辑管线，RAW 像素永不修改，
预览/导出在线性 ProPhoto RGB 工作空间实时渲染。v2.11.0，272 用例 / 274373 断言全绿。

- **不自研训练模型**：不涉及模型训练、数据集构建、PyTorch/TensorFlow 训练管线。
- **仅使用开源预训练权重**：所有 AI 功能基于社区已发布的 ONNX 格式模型，运行时自动下载到本地缓存。
- **推理框架**：引入 **ONNX Runtime**（C++ API）作为统一推理后端，支持 CPU/GPU/DirectML。

## 目标

### 1. 依赖引入

- 通过 **vcpkg** 或 **FetchContent** 引入 ONNX Runtime：
  - vcpkg: `onnxruntime-gpu` 或 `onnxruntime`（CPU 版优先，GPU 版标记为可选）。
  - 若 vcpkg 包不完整，允许使用官方预编译二进制（Microsoft 提供 x64/ARM64 的 C/C++ 发行包），通过 CMake `ExternalProject_Add` 或 `find_library` 集成。
- 更新 `CMakeLists.txt` 与 `vcpkg.json`，确保 CI/本地构建均可通过。

### 2. 模型管理器（ModelManager）

- 位置：`src/app/ModelManager.h` / `.cpp`（允许使用 Qt 进行网络下载）或 `src/utils/`。
- **职责**：
  - 维护模型清单（manifest）：JSON 文件（`resources/models/manifest.json` 或内嵌 C++ 结构）记录每个模型的：
    - `id`（如 "ai-denoise-raw", "mobilesam", "u2net-sky"）
    - `name`, `description`
    - `download_url`（HuggingFace / GitHub Releases 直链，或国内镜像如 hf-mirror.com）
    - `filename`, `sha256`, `size_bytes`, `version`
    - `input_shape`, `output_shape`, `input_format`（如 RGB/F32/224×224）
  - **本地缓存路径**：
    - Windows: `%LOCALAPPDATA%/Silver Halide/models/`
    - macOS/Linux: `~/.local/share/SilverHalide/models/`（或 `~/.silverhalide/models/`）
  - **下载逻辑**：
    - 应用启动时或首次使用某 AI 功能时，检查模型文件是否存在且 SHA256 匹配。
    - 缺失/损坏时，通过 `QNetworkAccessManager`（或 `libcurl` / `wininet` 纯 C++）后台下载，显示进度对话框。
    - 支持断点续传（HTTP Range）或至少支持取消重试。
    - 下载完成后校验 SHA256，失败则删除重下。
  - **版本更新**：manifest 中 `version` 升级后，自动重新下载新模型。
- **离线回退**：模型缺失且网络不可用时，AI 功能按钮置灰，提示「模型未下载」，不影响其他功能。

### 3. 推理封装（AiInferenceEngine）

- 位置：`src/engine/AiInference.h` / `.cpp`（**纯 C++，禁止 Qt 头文件**）。
- **职责**：
  - 封装 ONNX Runtime C API 或 C++ API：
    - `Ort::Env`, `Ort::Session`, `Ort::MemoryInfo`, `Ort::Value` 的生命周期管理。
    - 输入：`ImageBuffer`（ProPhoto RGB F32 / 线性）或 `std::vector<float>`。
    - 输出：`ImageBuffer` 或 `std::vector<float>` / mask 数组。
  - 提供统一接口：
    ```cpp
    class AiInferenceEngine {
    public:
        bool loadModel(const std::filesystem::path& modelPath);
        std::vector<float> run(const std::vector<float>& input,
                               const std::vector<int64_t>& inputShape);
        // 或针对图像的特化接口
        ImageBuffer runImage(const ImageBuffer& input,
                             const std::vector<int64_t>& inputShape,
                             const std::vector<int64_t>& outputShape);
    };
    ```
  - **线程安全**：ONNX Runtime 的 `Session::Run` 支持多线程，但单 Session 非线程安全。使用 `thread_local` Session 或 Session 池。
  - **内存池**：对于固定输入尺寸的模型（如分割），预分配 `Ort::Value` 避免每次推理重新分配。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`AiInferenceEngine` 是纯 C++ ONNX Runtime 封装，模型下载器在 `app/` 或 `ui/`。
2. **非破坏性原则**：AI 功能均作为参数层，不修改 RAW。
3. **线程安全**：所有 ONNX 推理在后台线程执行，主 UI 不阻塞。`Ort::Session` 的线程安全通过 Session 池或互斥锁保证。
4. **模型文件不入版本控制**：`models/` 目录加入 `.gitignore`，仅保留 `manifest.json` 与 SHA256 校验文件在仓库中。
5. **可选编译**：所有 AI 功能通过 CMake 选项 `LRC_ENABLE_AI`（默认 ON，但允许 OFF）控制。OFF 时：
   - 不链接 ONNX Runtime。
   - UI 中 AI 按钮隐藏。
   - 所有 AI 相关代码通过 `#ifdef LRC_ENABLE_AI` 或编译期接口存根隔离。
6. **测试覆盖**：
   - `ModelManager`：manifest 解析、SHA256 校验、路径生成。
   - `AiInferenceEngine`：ONNX 模型加载、输入输出形状匹配、内存管理（Valgrind/AddressSanitizer 无泄漏）。
   - 新增测试包含一个「极简 ONNX 模型」（如 2×2 输入的 Add 网络），用于无模型文件时的 CI 回归测试。
7. **隐私与合规**：所有模型为开源可商用许可证（MIT/Apache-2.0），manifest 中注明许可证。不调用云端 API，推理完全本地。

## 文件清单（需修改/新增）

### 修改

- `CMakeLists.txt` / `vcpkg.json` — 添加 ONNX Runtime 依赖与 `LRC_ENABLE_AI` 选项。

### 新增

- `src/engine/AiInference.h` / `.cpp` — ONNX Runtime 推理引擎封装（纯 C++，无 Qt）。
- `src/app/ModelManager.h` / `.cpp` — 模型清单解析、下载、校验、缓存路径管理（可用 QtNetwork）。
- `resources/models/manifest.json` — 模型清单（ID/URL/SHA256/版本/许可证）。
- `tests/TestAiInference.cpp` — ONNX 引擎单元测试（使用极简测试 ONNX 模型）。

## 验收标准

1. **ONNX Runtime 集成**：
   - 构建时 `LRC_ENABLE_AI=ON`，CMake 成功找到并链接 ONNX Runtime。
   - 构建时 `LRC_ENABLE_AI=OFF`，项目编译通过，AI 按钮不出现，二进制体积不增加。
2. **模型下载**：
   - 首次点击 AI 功能，若模型缺失，弹出下载对话框，显示进度条与速度。
   - 下载完成后自动校验 SHA256，失败提示重试。
   - 模型缓存到本地应用数据目录，重启后无需重复下载。
3. **推理链路**：
   - 用极简测试 ONNX 模型（Add 网络）跑通「加载 → 推理 → 输出形状校验」端到端。
   - 连续推理内存不泄漏（AddressSanitizer 无报错）。
4. **测试**：
   - `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 推荐开源模型清单（manifest.json 示例）

```json
{
  "models": [
    {
      "id": "nafnet-denoise",
      "name": "NAFNet Denoise",
      "description": "General image denoising",
      "url": "https://github.com/.../nafnet-denoise.onnx",
      "filename": "nafnet-denoise.onnx",
      "sha256": "a1b2c3...",
      "size": 5242880,
      "input_shape": [1, 3, -1, -1],
      "license": "Apache-2.0"
    },
    {
      "id": "rmbg2-subject",
      "name": "RMBG-2 Subject Segmentation",
      "description": "Foreground/background segmentation",
      "url": "https://huggingface.co/briaai/RMBG-2-Studio/resolve/main/model.onnx",
      "filename": "rmbg2.onnx",
      "sha256": "d4e5f6...",
      "size": 8912896,
      "input_shape": [1, 3, 1024, 1024],
      "license": "Apache-2.0"
    },
    {
      "id": "u2net-sky",
      "name": "U²Net Sky Segmentation",
      "description": "Sky detection for landscape photos",
      "url": "https://github.com/.../u2net-sky.onnx",
      "filename": "u2net-sky.onnx",
      "sha256": "g7h8i9...",
      "input_shape": [1, 3, 320, 320],
      "license": "MIT"
    },
    {
      "id": "mobilenetv3-embed",
      "name": "MobileNetV3 Feature Extractor",
      "description": "Duplicate detection embedding",
      "url": "https://github.com/.../mobilenetv3-embed.onnx",
      "filename": "mobilenetv3-embed.onnx",
      "sha256": "j0k1l2...",
      "size": 4194304,
      "input_shape": [1, 3, 224, 224],
      "output_shape": [1, 1280],
      "license": "MIT"
    }
  ]
}
```

> 实际模型 URL 与 SHA256 需在实施时从 HuggingFace / GitHub Releases 获取准确值。若国内访问困难，manifest 中可配置镜像源（`mirror_url`）。

## 参考

- ONNX Runtime C++ API 文档：https://onnxruntime.ai/docs/api/c/
- 现有 `PreviewWorker` 的后台线程机制。

## 实现提示

1. **先搭建基础设施**：先实现 `ModelManager` + `AiInferenceEngine` + 极简测试 ONNX 模型跑通端到端，再逐个接入具体 AI 功能。
2. **ONNX 模型来源**：若找不到现成 ONNX，可用 PyTorch 官方模型 + `torch.onnx.export()` 自行导出，将导出脚本与原始权重链接记录在 `docs/MODELS.md` 中（不入主仓库）。
