
# Goal: 集成 AI 推理能力与长期竞争力功能（Phase 4）

## 项目背景
Silver Halide 是一个使用 C++20 + Qt6 Widgets 构建的数字摄影桌面应用，聚焦 RAW 解码与非破坏性编辑。核心架构是一条非破坏性编辑管线：原始像素永不修改，所有调整以参数堆叠（Parameter Stack）形式保存在 SQLite 目录数据库（.lrcat）中，预览/导出时在线性 ProPhoto RGB 工作空间中实时渲染。

版本：v2.11.0 | 测试全绿（272 用例 / 274373 断言）

## 当前状态
- 已有完整的 CPU 渲染管线（Pipeline）、目录系统（CatalogDatabase）、色彩管理（lcms2）。
- 已有蒙版体系（几何蒙版 + Phase 2 的颜色/明亮度范围蒙版）。
- **缺失**：
  1. 无 AI 降噪：高 ISO RAW 只能依赖手动降噪滑块。
  2. 无 AI 蒙版：无法自动选择「主体/天空/背景」，蒙版全靠手画。
  3. 无重复检测：图库膨胀后无法发现视觉重复照片。
  4. 无软打样：印刷 workflow 中无法预览 ICC 色域映射结果。

## 核心约束
- **不自研训练模型**：本项目不涉足模型训练、数据集构建、PyTorch/TensorFlow 训练管线。
- **仅使用开源预训练权重**：所有 AI 功能基于社区已发布的 ONNX / ncnn / MNN 格式模型，运行时自动下载到本地缓存。
- **推理框架**：引入 **ONNX Runtime**（C++ API）作为统一推理后端。ONNX Runtime 支持 CPU/GPU/DirectML，且绝大多数 PyTorch 模型均可导出为 ONNX。

## 目标

### 0. 基础设施：模型下载管理与 ONNX Runtime 集成

在实现具体 AI 功能前，先搭建推理基础设施。

#### 0.1 依赖引入
- 通过 **vcpkg** 或 **FetchContent** 引入 ONNX Runtime：
  - vcpkg: `onnxruntime-gpu` 或 `onnxruntime`（CPU 版优先，GPU 版标记为可选）。
  - 若 vcpkg 包不完整，允许使用官方预编译二进制（Microsoft 提供 x64/ARM64 的 C/C++ 发行包），通过 CMake `ExternalProject_Add` 或 `find_library` 集成。
- 更新 `CMakeLists.txt` 与 `vcpkg.json`，确保 CI/本地构建均可通过。

#### 0.2 模型管理器（ModelManager）
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

#### 0.3 推理封装（AiInferenceEngine）
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

---

### 1. AI 降噪（AI Denoise）

在 Detail 面板中新增「AI 降噪」按钮，一键去除高 ISO 彩色/亮度噪点，效果优于手动滑块。

#### 1.1 模型选择
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

#### 1.2 数据模型
- `Adjustments` 新增：
  ```cpp
  struct AiDenoise {
      bool enabled = false;
      float strength = 100.0f;  // 0-100，混合系数
      // 不存储模型版本，由应用自动匹配最新下载模型
  };
```

- 管线位置：在手动降噪之后、锐化之前（或作为 Detail 面板的最底层操作）。若降噪模型在 sRGB 空间工作，则在 `toDisplaySRGB` 之后、显示之前执行（仅影响预览与导出，不进入 ProPhoto 管线）。

#### 1.3 UI

- Detail 面板新增「AI 降噪」分组：
  - 按钮「应用 AI 降噪」（首次点击触发模型下载 + 推理）。
  - 滑块「强度」：0–100，默认 100。控制降噪结果与原图的混合比例。
  - 复选框「仅亮度降噪」/「亮度+颜色」（若模型支持，否则仅一个模式）。
  - 进度指示：推理时显示「处理中…」旋转图标，后台完成。

#### 1.4 性能与回退

- 预览分辨率下（如 2048px 长边），推理时间目标 < 3 秒（CPU，ThinkPad X390）。
- 全分辨率 60MP 导出时，允许分块推理，后台完成，显示进度条。
- 若 ONNX Runtime 未启用编译（CMake 选项 `LRC_ENABLE_AI=OFF`），AI 降噪按钮隐藏。

---

### 2. AI 蒙版（Select Subject / Sky / Background）

在现有蒙版面板中新增「选择主体」「选择天空」「选择背景」按钮，自动生成像素级蒙版。

#### 2.1 模型选择

- **主体分割（Subject / Foreground）**：
  - **RMBG-2-Studio**（BRIA AI，开源可商用，ONNX 格式，专门优化前景/背景分离）。
  - 或 **U²Net**（显著性检测，轻量，ONNX 社区成熟）。
  - 或 **MobileSAM**（见下，用点提示框选主体）。
- **天空分割（Sky）**：
  - **U²Net** 在天空数据集微调的版本，或 **SegFormer-B0** 的 ADE20K 导出（ONNX）。
  - 简化方案：使用通用语义分割模型（如 **PP-LiteSeg** ONNX），取 sky 类别通道。
- **背景分割**：主体蒙版取反即可。

#### 2.2 处理流程

1. 用户点击「选择主体」：
   - 模型输入：当前预览图像（sRGB，归一化，尺寸通常为 320×320 或 512×512，模型决定）。
   - 推理输出：单通道 mask（0.0–1.0），尺寸与输入一致。
   - 将 mask 上采样/插值到当前预览/全分辨率尺寸。
   - 转换为现有 `Mask` 体系的**画笔蒙版权重图**（或直接作为新的 `AiMask` 类型）。
   - 在预览区以红色叠加显示生成结果，用户可进一步用画笔增减（与现有蒙版局部调整结合）。
2. **天空**：同理，输出天空 mask。
3. **背景**：主体 mask 取反。

#### 2.3 与现有蒙版体系集成

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
- AI 蒙版可与现有局部调整滑块（曝光/清晰等）结合，行为与几何蒙版一致。

#### 2.4 UI

- 蒙版面板（`DevelopPanel` 的 Mask 分组）新增三个按钮：
  - 「选择主体 🤖」「选择天空 ☁️」「选择背景 🖼️」。
- 点击后后台推理，预览区显示红色权重叠加。
- 提供「阈值」滑块微调边缘（0–1），以及「羽化」滑块柔化边缘。
- 用户可继续用「橡皮擦」画笔在 AI 蒙版上局部修正。

---

### 3. 重复检测（Duplicate Detection）

自动发现图库中视觉重复或高度相似的照片，支持自动堆叠。

#### 3.1 算法策略：双阶段检测

- **阶段 1：感知哈希（pHash）快速预筛选**（纯算法，无 AI）：
  - 对每张图片的 thumbnail（320px）计算 64-bit pHash（DCT 低频）。
  - 汉明距离 < 10 视为候选重复组。
- **阶段 2：深度特征精确确认**（AI）：
  - 使用轻量 CNN（如 **MobileNetV3-Small** 或 **ResNet18**）的 ONNX 导出，提取 512-dim 或 128-dim 嵌入向量。
  - 对阶段 1 的候选组计算余弦相似度，> 0.92 确认为重复。
  - 模型输入：224×224 RGB，输出：特征向量。

#### 3.2 工作流程

- **手动扫描**：图库菜单「查找重复照片…」：
  - 对话框：选择扫描范围（整个目录/所选文件夹），相似度阈值。
  - 后台线程遍历所有照片，计算 pHash 与深度特征（首次计算后缓存到数据库）。
  - 结果对话框：分组显示重复照片（如连拍 3 张几乎相同），用户可选择「堆叠」「删除副本」「取消」。
- **自动堆叠**：可选勾选「自动堆叠重复照片」，将确认重复的照片在 Filmstrip 中折叠为一组（与 LRC 的 Stack 行为一致）。
- **数据库缓存**：
  - 新增 `photo_features` 表：`photo_id`, `phash` (TEXT/INT64), `embedding` (BLOB, float32 数组), `updated_at`。
  - 照片导入时自动计算并缓存。
  - 照片编辑后（如大幅裁剪/调色）pHash 可能变化，可设置「仅对未编辑的 RAW 启用检测」或定期重新计算。

#### 3.3 UI

- `NavigatorWidget` 或 `MainWindow` 菜单：「图库 → 查找重复照片」。
- 结果对话框：左右对比显示候选重复组，标记最佳保留项（如最高评分、最大文件）。
- Filmstrip 中重复堆叠显示角标数字（如 "3" 表示堆叠 3 张）。

---

### 4. 软打样（Soft Proofing）

利用现有 `ColorManager`（lcms2）实现印刷预览与色域警告。

#### 4.1 功能

- **目的**：模拟照片在特定输出设备（如打印机、CMYK 印刷）上的色彩表现，提前发现超色域（Gamut Out-of-Gamut）区域。
- **实现**：
  - 用户从 `文件 → 软打样设置` 选择 ICC 配置文件（如 `ISO Coated v2 300% (ECI).icc` 或自定义 `.icc`/`.icm`）。
  - `ColorManager` 使用 lcms2 的 `cmsCreateProofingTransform`：
    - 输入：ProPhoto RGB（工作空间）。
    - 输出：sRGB（显示器）。
    - 模拟：所选 ICC 配置文件（如 CMYK 或特定 RGB 打印机）。
    - 意图：Perceptual / Relative Colorimetric（用户可选）。
  - 预览区显示模拟后的图像。
  - **色域警告（Gamut Warning）**：按 `Shift+G` 或勾选「显示色域警告」，超色域像素以红色/灰色叠加显示（lcms2 的 `cmsFLAGS_GAMUTCHECK` + `cmsSetAlarmCodes`）。
- **数据模型**：不存入 `Adjustments`（非照片编辑参数），作为应用级设置（`QSettings`）或临时状态。
- **UI**：
  - 预览区底部状态栏显示当前软打样配置文件名称。
  - 软打样开启时，直方图下方显示警告标识。

---

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`AiInferenceEngine` 是纯 C++ ONNX Runtime 封装，模型下载器在 `app/` 或 `ui/`。
2. **非破坏性原则**：AI 降噪/蒙版均作为参数层，不修改 RAW。软打样仅预览，不写入文件。
3. **线程安全**：所有 ONNX 推理在后台线程（`PreviewWorker` 或专用 `AiWorker`）执行，主 UI 不阻塞。`Ort::Session` 的线程安全通过 Session 池或互斥锁保证。
4. **模型文件不入版本控制**：`models/` 目录加入 `.gitignore`，仅保留 `manifest.json` 与 SHA256 校验文件在仓库中。
5. **可选编译**：所有 AI 功能通过 CMake 选项 `LRC_ENABLE_AI`（默认 ON，但允许 OFF）控制。OFF 时：
   - 不链接 ONNX Runtime。
   - UI 中 AI 按钮隐藏。
   - 所有 AI 相关代码通过 `#ifdef LRC_ENABLE_AI` 或编译期接口存根隔离。
6. **测试覆盖**：
   - `ModelManager`：manifest 解析、SHA256 校验、路径生成。
   - `AiInferenceEngine`：ONNX 模型加载、输入输出形状匹配、内存管理（Valgrind/AddressSanitizer 无泄漏）。
   - AI 降噪：输入纯噪点图，验证输出平滑（像素方差下降）。
   - AI 蒙版：输入标准测试图（如纯色背景+主体），验证 mask 与主体区域 IoU > 0.85。
   - 重复检测：构造 3 组已知重复/非重复测试图，验证召回率 > 95%、误报率 < 5%。
   - 软打样：加载测试 ICC，验证超色域像素被正确标记。
7. **隐私与合规**：所有模型为开源可商用许可证（MIT/Apache-2.0），manifest 中注明许可证。不调用云端 API，推理完全本地。

---

## 文件清单（需修改/新增）

### 修改

- `CMakeLists.txt` / `vcpkg.json` — 添加 ONNX Runtime 依赖与 `LRC_ENABLE_AI` 选项。
- `src/engine/Pipeline.h` / `.cpp` — 在管线中插入 AI 降噪层（若启用）与 AI 蒙版权重生成。
- `src/engine/Adjustments.h` / `.cpp` — 新增 `AiDenoise` 与 `AiMask` 参数结构。
- `src/engine/ColorManager.h` / `.cpp` — 新增软打样变换创建接口。
- `src/catalog/CatalogDatabase.h` / `.cpp` — 新增 `photo_features` 表（pHash/embedding）。
- `src/ui/DevelopPanel.h` / `.cpp` — 新增 AI 降噪按钮/强度滑块、AI 蒙版按钮。
- `src/ui/ImageViewport.h` / `.cpp` — 支持软打样模式与色域警告红色叠加。
- `src/ui/MainWindow.h` / `.cpp` — 新增「查找重复」「软打样设置」菜单。

### 新增

- `src/engine/AiInference.h` / `.cpp` — ONNX Runtime 推理引擎封装（纯 C++，无 Qt）。
- `src/engine/AiDenoiseProcessor.h` / `.cpp` — AI 降噪分块/重叠/融合逻辑（纯 C++）。
- `src/engine/AiMaskGenerator.h` / `.cpp` — AI 蒙版生成（输入图像 → 调用模型 → 输出 mask，纯 C++）。
- `src/engine/SoftProofing.h` / `.cpp` — lcms2 软打样与色域警告封装（纯 C++）。
- `src/app/ModelManager.h` / `.cpp` — 模型清单解析、下载、校验、缓存路径管理（可用 QtNetwork）。
- `src/app/DuplicateDetector.h` / `.cpp` — pHash + 深度特征重复检测引擎（纯 C++）。
- `src/ui/AiProgressDialog.h` / `.cpp` — 模型下载/推理进度对话框。
- `src/ui/DuplicateResultsDialog.h` / `.cpp` — 重复照片结果展示与操作对话框。
- `src/ui/SoftProofingDialog.h` / `.cpp` — ICC 配置文件选择与意图设置对话框。
- `resources/models/manifest.json` — 模型清单（ID/URL/SHA256/版本/许可证）。
- `tests/TestAiInference.cpp` — ONNX 引擎单元测试（使用极简测试 ONNX 模型）。
- `tests/TestAiDenoise.cpp` — 降噪效果像素级验证。
- `tests/TestAiMask.cpp` — 蒙版生成 IoU 验证。
- `tests/TestDuplicateDetection.cpp` — 重复检测准确率验证。
- `tests/TestSoftProofing.cpp` — 色域警告标记验证。

---

## 验收标准

1. **ONNX Runtime 集成**：
   
   - 构建时 `LRC_ENABLE_AI=ON`，CMake 成功找到并链接 ONNX Runtime。
   - 构建时 `LRC_ENABLE_AI=OFF`，项目编译通过，AI 按钮不出现，二进制体积不增加。

2. **模型下载**：
   
   - 首次点击「AI 降噪」或「选择主体」，若模型缺失，弹出下载对话框，显示进度条与速度。
   - 下载完成后自动校验 SHA256，失败提示重试。
   - 模型缓存到本地应用数据目录，重启后无需重复下载。

3. **AI 降噪**：
   
   - 导入 ISO 12800 的 A7C II ARW，打开 Detail 面板。
   - 点击「AI 降噪」，3 秒内预览更新，噪点显著减少，边缘保持清晰。
   - 调整「强度」滑块从 100 → 50，预览实时混合原图与降噪结果，延迟 < 200ms（缓存降噪结果后仅需混合）。
   - 导出 JPEG，AI 降噪效果正确 baked。

4. **AI 蒙版**：
   
   - 导入一张人物+天空照片。
   - 蒙版面板点击「选择主体」，2 秒内红色叠加显示人物轮廓，边缘准确（头发丝允许轻微瑕疵）。
   - 点击「选择天空」，天空区域被选中。
   - 在 AI 蒙版上应用「曝光 -1.0」，仅天空变暗，人物不受影响。
   - 用画笔在 AI 蒙版上局部擦除/增加，与现有蒙版体系无缝结合。

5. **重复检测**：
   
   - 图库中导入 10 张连拍几乎相同的照片 + 10 张完全不同的照片。
   - 执行「查找重复照片」，正确识别出连拍组为重复，其他照片无误报。
   - 选择「自动堆叠」，Filmstrip 中连拍组折叠为 1 个堆叠，角标显示数量。
   - 展开堆叠可查看成员，删除其中一张后堆叠自动更新。

6. **软打样**：
   
   - 选择一张高饱和照片，加载 CMYK 测试 ICC（如 `CoatedFOGRA39.icc`）。
   - 开启软打样，预览区显示模拟印刷后的暗淡版本（色域压缩可见）。
   - 按 Shift+G，超色域区域以红色叠加高亮。
   - 关闭软打样，立即恢复标准 sRGB 预览。

7. **性能与稳定性**：
   
   - 连续对 5 张照片使用 AI 蒙版，进程内存不泄漏（AddressSanitizer 无报错）。
   - 后台推理时，UI 仍可浏览图库、切换照片。
   - 模型推理异常（损坏 ONNX），捕获异常并弹窗「模型推理失败」，不崩溃。

8. **测试**：
   
   - `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。
   - 新增测试包含一个「极简 ONNX 模型」（如 2×2 输入的 Add 网络），用于无模型文件时的 CI 回归测试。

---

## 排除项（不在本次范围）

- **模型训练/微调**：不训练、不微调、不准备数据集。
- **生成式填充（Generative Fill）**：需 Stable Diffusion/Inpainting，计算量过大，本次不包含。
- **AI 超分辨率（Super-Resolution）**：如 Real-ESRGAN 放大，本次仅降噪。
- **视频 AI 功能**：仅静态图像。
- **云端推理**：所有推理本地执行，不上传用户照片。
- **NPU/GPU 专属加速**：ONNX Runtime CPU 执行优先，DirectML/CUDA 作为后续优化项。
- **自动主体识别后自动应用调整**（如 Auto Tone）：仅生成蒙版，不自动调参。

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
- lcms2 软打样：`cmsCreateProofingTransform`, `cmsFLAGS_GAMUTCHECK`
- 现有 `ColorManager` 的 `D65→ProPhoto` 矩阵与 `toDisplaySRGB` 管线。
- 现有 `Mask` 体系的权重叠加与局部调整逻辑。
- 现有 `PreviewWorker` 的后台线程与 LRU 缓存机制。

## 实现提示

1. **先搭建基础设施**：先实现 `ModelManager` + `AiInferenceEngine` + 极简测试 ONNX 模型跑通端到端，再逐个接入具体 AI 功能。
2. **分块推理是关键**：高分辨率图像（60MP）无法一次性送入显存/内存，AI 降噪必须实现 512×512 重叠分块（overlap 64px）+ 羽化融合，否则输出有接缝。
3. **AI 蒙版分辨率**：分割模型通常输出 320×320 或 512×512 mask，需用双线性插值上采样到预览分辨率，边缘用现有 `feather` 滑块二次羽化。
4. **重复检测的 embedding 缓存**：首次扫描 10000 张照片可能耗时数分钟，结果必须持久化到 `photo_features` 表，后续增量更新仅处理新增/修改照片。
5. **ONNX 模型来源**：若找不到现成 ONNX，可用 PyTorch 官方模型 + `torch.onnx.export()` 自行导出，将导出脚本与原始权重链接记录在 `docs/MODELS.md` 中（不入主仓库）。
