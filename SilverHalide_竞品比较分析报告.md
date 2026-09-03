# SilverHalide 与市面 RAW/摄影工作流软件比较分析报告

> 版本：v2.0（个人项目视角修订）  
> 日期：2026-09-03  
> 范围：功能、架构、性能、发展方向全面比较  
> **前提约束**：本项目为个人 vibecoding 项目，Windows 单平台为既定选择，不考虑跨平台移植

---

## 1. 执行摘要

SilverHalide（v1.0.0）是一款基于 C++20 + Qt 6 的 Windows 桌面 RAW 摄影工作流应用，聚焦 RAW 解码、非破坏性编辑与高性能预览/导出链路，采用 MIT 许可证开源。本文将其与市面上主流的商业与开源 RAW/摄影工作流软件进行多维度比较，明确当前差距与未来发展方向。

**评估前提调整（v2.0）：**

- v1.0 报告以"追赶成熟商业软件"为隐含目标，包含跨平台移植、人脸识别、云同步等适合团队项目的发展方向。
- v2.0 报告以"个人 vibecoding 项目"为前提重新校准：**不求功能对标，只求核心体验优秀**；不做跨平台，不做社区运营，不做重型基础设施。

**核心结论：**

- SilverHalide 在**架构分层设计**（engine/catalog/ui 严格分离、纯 C++ 核心可 headless 测试）上达到了专业级水准，406 个 headless 测试用例与 752,515 断言覆盖了核心管线。这一优势在个人项目中尤为难得——意味着后续加功能时始终有测试网兜底。
- 在**功能完备度**上，已具备 Lightroom-style 工作流的基础框架（RAW 解码、参数堆栈、局部调整、目录管理、导出），作为个人主力工具的核心骨架已经立住。
- 在**工程成熟度**上，性能优化（四阶段加速、增量渲染缓存、coarse-then-refine 预览）已达到较高水平，但缺少 GPU 加速与 SIMD 优化——这两个是"个人投入性价比最高"的改进方向，因为改动集中在 engine 层，且有现成测试验证 bit-exact 性。
- **定位调整**：不再以"成为 darktable/RawTherapee 的市场竞争者"为目标，而是**"Windows 平台上的个人现代化 RAW 工作流工具——自己用得爽，顺带开源"**。

---

## 2. SilverHalide 项目现状

### 2.1 基本信息

| 维度 | 详情 |
|------|------|
| **名称** | SilverHalide |
| **版本** | 1.0.0 |
| **许可证** | MIT License |
| **版权** | © ASE-128 2026 |
| **技术栈** | C++20 / Qt 6.8 / CMake / Ninja |
| **平台** | Windows 10/11 x64（既定，不扩展） |
| **依赖** | libraw, lcms2, sqlite3, ONNX Runtime, stb, pugixml |
| **测试覆盖** | 406 test cases / 752,515 assertions（headless） |

### 2.2 已实现功能

| 模块 | 功能 |
|------|------|
| **RAW 解码** | libraw 后端，支持 ARW/SR2/DNG 等，线性 ProPhoto RGB (D50) 工作空间 |
| **非破坏性编辑** | 曝光、对比度、高光/阴影、白/黑点、色调曲线、白平衡、去朦胧、鲜艳度、HSL（8 色域）、色调分离、点颜色 |
| **空间调整** | 纹理、清晰度、锐化、畸变、颗粒、晕影、蒙版局部调整 |
| **局部调整** | 污点去除（Clone/Heal）、线性/径向/画笔/色彩范围/AI 蒙版 |
| **AI 功能** | NAFNet 去噪（ONNX）、BiRefNet 主体分割蒙版（ONNX）、RMBG2 主体分割 |
| **目录管理** | SQLite 嵌入式目录（`.lrcat` 风格）、虚拟副本、关键字层级、智能收藏夹、XMP 往返 |
| **视图** | 胶片栏、主预览、Before/After 分屏、参考视图、1:1 全尺寸 |
| **导出** | JPEG / TIFF 8&16bit / PNG / DNG，批量导出，尺寸预设 |
| **合成** | 全景合成（纯 C++，NCC 对齐 + 柱面投影 + 羽化融合）、HDR 合成（曝光包围） |
| **性能** | 四阶段加速（导入/预览/修改/导出）、增量 Stage 复用、256MB LRU 缓存、coarse-then-refine 预览 |

### 2.3 架构亮点（对个人项目的意义）

- **三层严格分层**：`engine/`（纯 C++，Qt-free）/`catalog/`（纯 C++）/`ui/`（Qt6 Widgets）。对个人项目的价值：**每次改 engine 后跑一遍 headless ctest 就能确认没破坏管线**，不需要启动 GUI 验证。这是个人项目能持续演进的关键基础设施。
- **全线性工作流**：调整只在线性光（gamma=1.0）下进行，sRGB gamma 仅在最后由 lcms 一次性应用。避免了反复 gamma 造成的条带，色彩质量有底线保证。
- **缓存策略**：Pipeline LRU（256MB）+ 增量 Stage 缓存 + 画笔权重场 COW 缓存 + 晕影权重缓存。滑块拖动命中缓存时零重算，交互体验的关键。
- **固定调整顺序**：简化了实现和测试，对个人项目是正确的取舍——不需要 darktable 那种模块图的灵活性，换来的是管线行为的可预测性和 40 万级断言的回归保护。

---

## 3. 竞品全景图

> 注：竞品信息仅作功能参照系，不代表 SilverHalide 需要追赶的目标。

### 3.1 商业闭源软件

| 软件 | 厂商 | 定价模式 | 平台 | 核心定位 |
|------|------|----------|------|----------|
| **Adobe Lightroom Classic** | Adobe | 订阅制 ~$120/年 | Win/Mac | 行业标准，生态最完整 |
| **Capture One Pro** | Capture One | 订阅 $179/年 或 买断 $349 | Win/Mac | 专业级 RAW 处理与色彩编辑 |
| **DxO PhotoLab 9** | DxO | 买断 ~$229 | Win/Mac | 顶级降噪与镜头校正 |
| **ON1 Photo RAW 2026** | ON1 | 买断 $99-$169 或 订阅 | Win/Mac | Lightroom 替代品，AI 功能丰富 |
| **Luminar Neo** | Skylum | 买断 ~$149 或 订阅 | Win/Mac + Mobile | AI 驱动，易用性强 |

### 3.2 开源软件

| 软件 | 许可证 | 平台 | 核心定位 |
|------|--------|------|----------|
| **darktable 5.6** | GPL 3.0 | Win/Mac/Linux | 开源 Lightroom 替代品，功能最全面 |
| **RawTherapee** | GPL 3.0 | Win/Mac/Linux | 开源 RAW 处理器，色彩校正强大 |
| **digiKam** | GPL 2.0 | Win/Mac/Linux | 照片管理为主，编辑为辅 |

---

## 4. 多维度详细比较

### 4.1 RAW 解码与色彩科学

| 维度 | SilverHalide | Adobe LrC | Capture One | DxO PhotoLab | darktable | RawTherapee |
|------|-------------|-----------|-------------|--------------|-----------|-------------|
| **RAW 引擎** | libraw 0.22.0 | Adobe Camera Raw | 自研 | 自研（DeepPRIME） | 自研 | 自研 |
| **工作空间** | 线性 ProPhoto RGB (D50) | ProPhoto RGB (D50) | 自研 wide-gamut | 自研 wide-gamut | 线性 Rec.2020 / XYZ / Lab | 线性 ProPhoto / Lab |
| **色深** | U16 (RAW) / F32 (non-RAW) | U16 / F32 内部 | F32 内部 | F32 内部 | F32 / U16 | F32 / U16 |
| **白平衡** | As-shot + 相对偏移 | As-shot + 绝对色温/色调 | As-shot + 绝对 | As-shot + 绝对 | As-shot + 绝对 | As-shot + 绝对 |
| **相机配置文件** | 无（仅矩阵） | 大量官方 + 第三方 | 大量官方 + 自定义 ICC | DxO 光学模块 | 输入色彩配置文件 | DCP / ICC / 自定义 |
| **镜头校正** | 无（仅手动畸变） | 海量 Adobe 镜头配置文件 | 官方镜头配置文件 | 10万+ 相机/镜头组合的光学模块 | lensfun 数据库 | lensfun 数据库 |
| **去马赛克算法** | libraw 内置 | 自研 + AI Raw Details | 自研 | DeepPRIME XD3（AI） | 多种可选 | 多种可选 |

**分析：**
- 工作空间选择与 Lightroom 一致，是专业级选择，无需改动。
- **对个人项目最有价值的改进**：集成 lensfun 数据库（开源 C++，RawTherapee/darktable 都在用），一次集成永久受益，且能覆盖 90% 常见镜头的自动校正。
- 相机配置文件（DCP）支持是一个中等工作量的改进，可显著提升不同相机的色彩一致性。
- 去马赛克算法的多样化（darktable 提供 PPM/AMAZE/RCD 选择）对最终画质有影响，但实现成本高，个人项目优先级低。

### 4.2 非破坏性编辑管线

| 维度 | SilverHalide | Adobe LrC | Capture One | darktable | RawTherapee |
|------|-------------|-----------|-------------|-----------|-------------|
| **调整顺序** | 固定顺序 | 固定顺序 | 固定+部分可重排 | 模块顺序可自定义 | 固定顺序 |
| **图层/快照** | ❌ 无 | ✅ 快照 + 虚拟副本 | ✅ 图层 + 快照 | ✅ 快照 + 样式 | ✅ 处理配置文件 |
| **历史记录** | 线性快照（无分支） | 线性快照 | 线性历史 | 参数化历史 | 处理队列 |
| **局部调整** | 蒙版（6 种类型） | 蒙版（AI + 手绘 + 范围） | 图层蒙版 + 局部 | 蒙版（drawn + parametric + AI） | 局部调整 |
| **曲线编辑** | 分区点曲线 | 参数+点曲线 | 点曲线 + Luma 曲线 | 多种曲线 | 曲线 + LAB 曲线 |
| **HSL** | 8 色域独立调整 | 8 色域 | 高级色彩编辑器 | 色彩平衡 RGB | 色彩管理（Lab） |
| **色调分离** | 高光/阴影 + 平衡 | 高光/阴影/平衡 | 分割色调 | 分割色调 | 色调映射 |

**分析：**
- SilverHalide 的编辑参数覆盖度已接近 Lightroom 基础面板，日常修图的"武器库"是够用的。
- **缺少图层/快照系统**是日常使用中最容易感知的缺失——修一张图想尝试两种风格就得靠虚拟副本（当前已实现）来回切，或者手动记参数。已有 `edit_history` 表和虚拟副本机制，实现 Lightroom 式"快照"的增量工作量不大。
- darktable 的模块系统灵活性对个人项目是过度设计，SilverHalide 的固定顺序是正确取舍。

### 4.3 AI 功能

| 维度 | SilverHalide | Adobe LrC | ON1 Photo RAW | Luminar Neo | darktable 5.6 | DxO PhotoLab |
|------|-------------|-----------|---------------|-------------|---------------|--------------|
| **AI 降噪** | ✅ NAFNet（ONNX） | ✅ AI Denoise（GPU） | ✅ NoNoise AI | ❌（需插件） | ✅ Neural restore | ✅ DeepPRIME XD3（行业最强） |
| **AI 蒙版** | ✅ BiRefNet 主体分割 | ✅ 主体/天空/背景/对象 | ✅ Super Select AI | ✅ Mask AI + Sky AI | ✅ AI object mask（SAM，5.6） | ✅ AI Masks |
| **生成式 AI** | ❌ | ✅ Generative Remove | ✅ Generative Erase | ✅ GenErase/GenSwap | ❌ | ❌ |
| **AI 分辨率提升** | ❌ | ✅ Super Resolution | ✅ Resize AI | ❌（需插件） | ✅ upscale（RealPLKSR/BSRGAN） | ❌ |
| **AI 运行方式** | ONNX Runtime CPU | GPU / Apple Neural Engine | GPU / CPU | CPU | CPU / GPU（CUDA/ROCm/DirectML） | GPU / CPU |

**分析：**
- SilverHalide 的 AI 基础设施（ONNX Runtime + manifest 驱动 + SHA256 校验 + 按需下载）已经搭好，**加新 AI 功能的边际成本很低**——只需要找合适的开源 ONNX 模型、写 manifest 条目、加一个 processor。
- darktable 5.6 验证了"开源软件用社区模型做 AI 蒙版"的路径可行（SAM 点击式蒙版）。SilverHalide 引入 SAM 是天然的下一步。
- 生成式 AI（对象移除、画面扩展）对个人项目而言，模型大、推理慢、效果不稳定，且开源替代品（IOPaint / LaMa）已独立可用，建议不自己做。
- **关键瓶颈**：当前所有 AI 推理跑在 CPU 上。ONNX Runtime 支持 DirectML 执行提供者，Windows 上启用 GPU 加速是低成本的质变。

### 4.4 目录与资产管理 (DAM)

| 维度 | SilverHalide | Adobe LrC | darktable |
|------|-------------|-----------|-----------|
| **数据库** | SQLite（单文件 .lrcat） | SQLite（.lrcat） | SQLite |
| **文件夹监视** | 2s 轮询 | OS 事件驱动 | OS 事件驱动 |
| **智能收藏夹** | ✅ 多谓词规则 | ✅ | ✅（集合过滤器） |
| **重复检测** | ✅ pHash + 嵌入向量 | ✅（2025 新增） | ❌ |
| **人脸识别** | ❌ | ✅ | ❌ |
| **地理标签/地图** | ❌ | ✅ | ✅（OpenStreetMap） |
| **联机拍摄** | ❌ | ✅ | ✅（部分相机） |
| **批量重命名** | ❌ | ✅ | ✅ |

**分析：**
- 对个人项目而言，DAM 功能已经足够好。智能收藏夹 + 关键字层级 + 重复检测 + 虚拟副本，这些核心组织能力甚至超过部分商业软件。
- **人脸识別**：对个人项目不现实（需要持续维护人脸检测模型、处理大量误检）。且单机场景下，几千张照片用关键字和评分管理完全够用。
- **地理标签**：libraw 已能读取 GPS 坐标，但地图视图需要集成地图组件（QtLocation 或 Web 视图），工作量大、使用频率低，建议**仅做"读取并在元数据面板显示坐标"**，不做地图。
- **联机拍摄**：Windows 上 libgphoto2 支持有限，且个人项目很少需要，不建议投入。
- **批量重命名**：工作量小、实用性高，值得做。

### 4.5 性能与渲染

| 维度 | SilverHalide | Adobe LrC | Capture One | darktable | RawTherapee |
|------|-------------|-----------|-------------|-----------|-------------|
| **预览分辨率** | 1920px（最长边） | 可配置 | 可配置 | 可配置 | 可配置 |
| **GPU 加速** | ❌ | ✅ | ✅ | ✅（OpenCL） | ❌ |
| **SIMD 优化** | ❌（计划中） | ✅ | ✅ | ✅ | ✅ |
| **预览延迟** | 30ms 粗帧 + 50ms 精帧 | GPU 实时 | GPU 实时 | OpenCL 加速 | 较慢 |
| **增量渲染** | ✅ Stage 前缀复用 | ✅ | ✅ | ✅ 模块缓存 | ❌ |
| **导出并行** | ✅ TIFF strip 并行 | ✅ | ✅ | ✅ | ✅ |

**分析：**
- **GPU 加速是个人项目投入产出比最高的改进**。方案选择：
  - **DirectML**（推荐）：ONNX Runtime 原生支持，AI 推理直接受益；图像处理算子需手写 HLSL 计算着色器。
  - **OpenCL**：darktable 验证过，但微软已在 Windows 上弃用 OpenCL 支持，驱动质量参差。
  - **DirectCompute**：Windows 原生计算着色器，与 Qt/D3D 集成自然。
  - 个人建议：**AI 推理走 ONNX Runtime + DirectML（几乎零代码），像素管线走 SIMD（AVX2）优先**，GPU 像素管线作为长期项。
- SIMD（AVX2）优化是纯 engine 层改动，有 bit-exact 测试兜底，风险低收益高（逐像素算子 2-4x 加速）。
- 增量 Stage 复用已是亮点，保持即可。

### 4.6 导出与交付

| 维度 | SilverHalide | Adobe LrC | darktable | RawTherapee |
|------|-------------|-----------|-----------|-------------|
| **格式** | JPEG/PNG/TIFF8/TIFF16/DNG | JPEG/TIFF/PSD/DNG/PNG | JPEG/PNG/TIFF/WebP/AVIF | JPEG/PNG/TIFF/WebP |
| **色彩配置文件嵌入** | sRGB（硬编码） | 广泛支持 | 广泛支持 | 广泛支持 |
| **软打样** | ✅（lcms2） | ✅ | ✅ | ✅ |
| **打印模块** | ❌ | ✅ | ✅ | ✅ |
| **水印** | ❌ | ✅ | ✅ | ✅ |

**分析：**
- **色彩配置文件选择**是小改动大收益——ProfileIO 已有 ICC 读写能力，只需在导出对话框加一个下拉框。
- 水印（简单文字/PNG 叠加）工作量小，个人分享照片时实用。
- 打印模块对个人项目不值得投入（Windows 打印照片的标准路径很多）。

### 4.7 平台与生态

| 维度 | SilverHalide | Adobe LrC | darktable |
|------|-------------|-----------|-----------|
| **Windows** | ✅ | ✅ | ✅ |
| **macOS** | ❌（既定不扩展） | ✅ | ✅ |
| **Linux** | ❌（既定不扩展） | ❌ | ✅ |
| **插件体系** | ❌ | ✅ | ❌ |
| **社区/教程** | 极小 | 海量 | 活跃 |

**分析（个人项目视角）：**
- **Windows-only 不再是"缺陷"，而是合理范围控制**。macOS 移植需要 Apple 硬件做测试、处理 APFS 路径差异、验证 libraw 在各芯片上的表现——对个人项目是纯投入无回报。
- 无插件体系对个人项目不是瓶颈——自己的需求自己写，不存在"等社区插件"的场景。
- 社区/教程生态小对个人项目影响有限——用户就是作者本人。

---

## 5. 差距分析矩阵（个人项目视角）

### 5.1 核心差距（影响日常使用）

| 差距 | 严重程度 | 影响 | 改进性价比 |
|------|----------|------|-----------|
| **AI/像素处理全跑 CPU** | 🔴 高 | AI 降噪一张 45MP 图要数秒；高像素 RAW 预览延迟明显 | ★★★★★ DirectML 几乎零代码；SIMD 低风险 |
| **无镜头校正数据库** | 🔴 高 | 每张照片手动拉畸变滑块，效率低 | ★★★★★ lensfun 集成一次到位 |
| **无快照系统** | 🟡 中高 | 想对比两种调色风格只能靠虚拟副本，操作繁琐 | ★★★★ 已有 edit_history 基础，增量小 |
| **导出固定 sRGB** | 🟡 中 | 广色域显示器/印刷场景无法发挥 | ★★★★★ ProfileIO 已就绪，改动极小 |
| **无相机 DCP 支持** | 🟡 中 | 不同相机色彩还原差异大，无法精准校准 | ★★★ 中等工作量 |
| **无 AI 天空/对象蒙版** | 🟡 中 | 风光摄影高频需求，目前只有主体分割 | ★★★★ SAM 或专用分割模型 ONNX 现成 |

### 5.2 已明确不做的（个人项目范围控制）

| 方向 | 不做原因 |
|------|----------|
| ~~macOS / Linux 移植~~ | 用户明确排除；需 Apple 硬件测试；无回报 |
| ~~人脸识别~~ | 模型维护成本高；单机场景关键字+评分够用 |
| ~~地理标签地图~~ | 地图组件集成工作量大；使用频率低 |
| ~~联机拍摄~~ | Windows 上 libgphoto2 支持差；个人场景用不到 |
| ~~云同步 / 移动端~~ | 基础设施成本；个人项目无此需求 |
| ~~生成式 AI（对象移除/扩展）~~ | 模型大、推理慢、效果不稳；IOPaint/LaMa 等独立工具已够用 |
| ~~打印模块~~ | Windows 打印路径已多；个人照片打印场景少 |
| ~~插件体系 / SDK~~ | 社区不存在；自己的需求自己写 |

### 5.3 相对优势（个人项目语境下）

| 优势 | 对个人的意义 |
|------|-------------|
| **MIT 开源** | 无订阅费，永远可用，代码完全自己掌控 |
| **架构分层 + 406 测试** | 每次改完跑一遍 ctest 就放心，不需要 GUI 回归 |
| **构建简单** | 一条 `build_x64.bat` 出 Release，不依赖 vcpkg/复杂环境 |
| **AI 模型可替换** | 看到好的开源 ONNX 模型就能往里塞，manifest 驱动 |
| **没有厂商绑架** | 不怕 Adobe 改定价/关服务；数据在本地 SQLite 里，永远可导出 |

---

## 6. 未来发展方向与建议（个人项目校准版）

### 6.1 第一波：高性价比改进（随时可做，单项 1-4 周）

| 方向 | 具体建议 | 理由 |
|------|----------|------|
| **ONNX Runtime + DirectML** | 在 `AiInference` 初始化时检测 DirectML 执行提供者并启用 | 几乎零代码（ONNX Runtime 原生支持），AI 降噪/蒙版直接 GPU 加速数倍 |
| **导出色彩配置文件** | 导出对话框加 ICC 下拉（sRGB / Adobe RGB / Display P3 / 自定义） | ProfileIO 已有 ICC 能力，UI 加一个下拉即可 |
| **批量重命名** | 导入/导出对话框加 rename 模板（日期+序号+原文件名等） | 纯 catalog 层改动，正则模板即可 |
| **水印** | 导出时叠加文字或 PNG 图片（位置/透明度/大小可调） | engine 层像素叠加 + UI 一个小节 |
| **快照系统** | 在现有 `edit_history` 基础上加"命名快照"能力，UI 加快照面板 | catalog 层加 name 列，UI 列表即可；让虚拟副本回归"副本"本职 |

### 6.2 第二波：核心能力补齐（1-3 个月量级）

| 方向 | 具体建议 | 理由 |
|------|----------|------|
| **lensfun 镜头校正** | 集成 lensfun 库，导入时自动识别镜头并应用畸变/暗角/色差校正 | 一次集成永久受益；lensfun 有现成 CMake 目标，可 vendored |
| **SIMD 优化（AVX2）** | 对 `applyAdjustments` 逐像素循环和 `gaussianBlur` 引入 AVX2 intrinsics | engine 层改动，bit-exact 测试兜底；2-4x 加速，预览/导出都受益 |
| **AI 蒙版扩展** | 1) 引入 SAM（Segment Anything）ONNX 模型做点击式蒙版；2) 找开源天空分割模型（如 SkyAR/SwinUNet 的 ONNX 版）补天空蒙版 | manifest 加条目 + processor 类，AI 基建已就绪 |
| **相机 DCP 支持** | 解析 Adobe DCP 文件的色调曲线和查找表，作为相机配置文件应用 | 提升色彩还原准确性；dcp-tool 可参考 |

### 6.3 第三波：体验打磨（有闲心时）

| 方向 | 具体建议 | 理由 |
|------|----------|------|
| **直方图升级** | 当前统计预览 8bit + 每 4M 像素抽样；改用线性 F32 缓冲统计，显示 RGB 三通道直方图 | 小改动，修图时对曝光判断更准 |
| **OS 事件文件夹监视** | 用 `ReadDirectoryChangesW` 替换 2s 轮询 | Windows API 直接可用，响应即时 |
| **编辑预设** | 支持保存/加载调整参数为预设文件（JSON），可分享给未来的自己 | 参数堆栈本身就是 JSON，序列化即得 |
| **GPU 像素管线** | 将高斯模糊/锐化等空间滤波迁移到 DirectCompute 计算着色器 | 工作量大，放在 SIMD 之后；届时预览可真正做到 GPU 实时 |

### 6.4 技术债（顺手修）

| 方向 | 说明 |
|------|------|
| **flaky SIGSEGV in TestPipeline:704** | BufferPool thread_local 析构顺序问题，复现率 1/3；加显式 thread_local shutdown sequence |
| **TIFF rows-per-strip 自适应** | 100MP+ 大图时 `max(64, h/64)` 保持 strip 数 ~64 |
| **GUI 测试缺位** | 引入 Qt Test 对关键对话框做 smoke 测试（当前 GUI 层零自动化覆盖） |

---

## 7. 定位重述

```
                    功能深度
                       ▲
                       │
   Capture One     ●   │   ●  Lightroom Classic
   DxO PhotoLab    ●   │
                       │     ●  ON1 / Luminar Neo
   darktable       ●   │
   RawTherapee     ●   │
                       │
   ────────────────────┼───────────────────► 易用性
                       │
                       │  ○ SilverHalide（当前）
                       │     个人 Windows RAW 工具
                       │     · MIT 开源
                       │     · 免订阅
                       │     · 架构现代
                       │     · 核心管线测试完备
```

**个人项目的合理定位：**

> **"Windows 上自己日常修图用的主力工具——核心体验对标 Lightroom Classic 的 Develop 模块，AI 能力够用就好，不追求功能全覆盖。"**

与 darktable/RawTherapee 的关系不是"市场竞争者"，而是"同一生态位的不同选择"——如果有人恰好也用 Windows、喜欢 MIT 许可、看重代码可审计性，SilverHalide 是一个合理选项。但这不是项目的目标，只是顺带的副产品。

---

## 8. 结论

SilverHalide 作为个人 vibecoding 项目，v1.0.0 的完成度已经相当可观：核心管线稳定（406 测试）、AI 基建就绪（manifest + ONNX + DirectML 待启用）、性能优化有体系（四阶段加速）。**它不需要成为下一个 darktable，只需要成为"你自己最顺手的 Windows RAW 工具"。**

建议的执行顺序：

1. **立即**：启用 DirectML + 导出 ICC 选择 + 批量重命名（三项合计可能不到一周）
2. **1-3 个月内**：lensfun 集成 + AVX2 SIMD + 快照系统
3. **有空时**：SAM 点击蒙版 + DCP 支持 + 直方图升级
4. **明确不做**：跨平台、人脸识别、云同步、联机拍摄、生成式 AI、打印模块

这个范围对个人项目的精力投入是可持续的，每一项改进都能直接提升自己修图的体验，同时保持代码库的健康度。
