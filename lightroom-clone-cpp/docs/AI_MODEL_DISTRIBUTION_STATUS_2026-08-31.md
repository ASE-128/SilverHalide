# AI 模型分发与 UI 管理 — 实现变更总结

> 日期：2026-08-31
> 范围：将开源 ONNX 模型接入 AI 降噪 / 蒙版模块，覆盖模型选型、清单、下载 /
> 解压 / 校验、打包、UI 管理面板。对应设计文档 `.claude/artifacts/designs/ai-model-distribution.md`
> 与 `docs/PHASE_11.md` 的落地实现。

## 概述

本对话围绕「下载现有开源模型 → 集成到 AI 降噪 / 蒙版模块 → 引入打包」展开，最终产出：
端到端可用的模型分发链路（manifest 驱动 → 后台下载 → ZIP 自动解压 → SHA256 校验
→ ONNX Runtime 推理），以及一个供用户管理模型（下载 / 导入 / 打开缓存目录）的
设置面板。

- 许可证合规：优先 MIT / Apache-2.0，排除 NC（非商业）许可模型（如 BRIA RMBG-2）。
- 混合分发：小模型内置（test_add_2x2 测试桩）+ 大模型按需下载到
  `%LOCALAPPDATA%/Silver Halide/models/`。
- 离线可用：安装包捆绑 ONNX Runtime DLL 与 manifest.json，模型本体用户首次下载。

## 变更文件清单（按层）

### 引擎层

- `src/engine/AiInference.cpp` / `.h` — ONNX Runtime 推理封装，修正与 ONNX Runtime
  1.23.2 不兼容的 API 调用，保证加载 / 推理链路可用。
- `src/utils/Sha256.h` / `.cpp` — **新增**。从 `ModelManager` 内联实现中抽出，提供
  `sha256Buffer(data, len)` 与 `sha256File(path)`，使无头测试可调用。

### 应用层

- `src/app/ModelManager.h` / `.cpp` — 模型分发核心：
  - 新增静态方法 `extractOnnxFromZip(zipPath, targetPath, errMsg)`，基于 Qt 私有
    `QZipReader` 实现磁盘式解压；处理 Qualcomm NAFNet 的 external-data 模式：
    将 ZIP 内全部文件扁平到缓存目录，最大 `.onnx` 重命名为 manifest 指定文件名，
    `.data` 文件保留在同目录。
  - 修复 `sha256File` 缓冲区索引错误（对非 64 字节倍数、末块有余数的文件计算错误，
    导致 `isModelReady` 在后续运行返回 false）；实现迁移至 `utils/Sha256`。

### UI 层

- `src/ui/ModelManagementWidget.h` / `.cpp` — **新增**。设置对话框，列出 manifest 中
  所有模型并显示就绪状态（SHA 校验通过 / 未下载 / 许可证受限 / 下载中 / 错误），
  提供：下载（异步 + 进度条 + 取消）、导入本地 `.onnx` / `.zip`、打开缓存目录。
  内置测试桩（`internalOnly`）对终端用户隐藏。关闭对话框时显式取消在途下载。
- `src/ui/MainWindow.cpp` — 在 `buildMenus` 的「视图」与「帮助」之间新增「设置」
  菜单，含「模型管理…」项，弹出 `ModelManagementWidget::exec()`；并补
  `#include "ui/ModelManagementWidget.h"`。

### 资源与打包

- `resources/models/manifest.json` — 对齐真实模型信息：
  - `rmbg2-subject` → BiRefNet_lite-ONNX（主体分割，MIT），真实 URL / SHA256 / size。
  - `nafnet-denoise` → Qualcomm NAFNet-DeNoise（ZIP 分发，含 .data），实测 SHA256
    `b5451aea…`，标记「已跑通」。
  - `u2net-sky` → 因许可证问题留空 URL（许可证受限），由 `heuristicFallback` 兜底。
- `packaging/installer.nsi` — 捆绑 `manifest.json`，处理模型目录的安装 / 卸载，
  保证离线可用。

### 测试

- `tests/TestSha256.cpp` — **新增**，7 个回归用例（空输入、'abc'、100×'a'、
  跨块、文件校验等），防止 SHA256 余数块 bug 复现。
- `tests/nafnet_smoke.cpp` — NAFNet 端到端冒烟测试：下载 → ZIP 解压 → 加载 → 推理。

### 构建

- `CMakeLists.txt` — 将 `src/ui/ModelManagementWidget.cpp` 加入 `LRC_UI_SOURCES`
  （`lrc_lightroom` 目标）；`TestSha256` 注册进 `lrc_tests` 目标。

## 关键技术修复

1. **NAFNet ZIP 缺 `.data`**：Qualcomm NAFNet 以 ZIP 分发（model.onnx + metadata.json
   + 外部 `.data`）。原解压只取 `.onnx` 导致 ONNX Runtime 加载失败。修复为扁平提取
   全部文件，保留 `.data` 于同目录。
2. **SHA256 余数块错误**：原实现对「文件长度非 64 字节倍数且末块有余数」的情形算错
   哈希，导致下载后 `isModelReady` 永远 false。重构 `sha256_update` 缓冲区索引计算
   并加回归测试。
3. **`ModelManagementWidget` 占位 / 破损代码**（本轮修复）：
   - `onImportRequested` 中畸形的 `#if defined(_WIN32)` ZIP 提取块（含 `...` 占位与
     不匹配括号）→ 改为提取 `srcPath` 一次、ZIP / `.onnx` 两路干净复用。
   - `statusFor` 中无意义的 `activeHandles_.contains(0)` 死占位 `if` → 删除，仅保留
     磁盘 / manifest 状态判定。
   - `rebuildActionButtons` 中的空 `manifestIdx` 循环与重复 id 解析 → 改为在
     `refreshTable` 一次性把 model id 写入 `nameItem` 的 `Qt::UserRole`，按 id 解析。
   - 许可证列 `tr("?").toStdString()` 绕回 QString 的多余往返 → 简化。

## 构建与测试验证

- **无头构建 + 测试**：`ctest --test-dir build_headless --output-on-failure` →
  100% 通过（含 7 个新增 SHA256 用例，套件总计 413 例），未对 ModelManager /
  AiInference 层造成回归。
- **GUI 构建**：在 VS 2022 Developer PowerShell（`Enter-VsDevShell -Arch amd64`）
  下 `cmake --build build_gui -j --target lrc_lightroom` 通过：
  - MOC 对 `ModelManagementWidget` 的 `Q_OBJECT` 头跑通。
  - `ModelManagementWidget.cpp` 零错误零警告；`MainWindow.cpp` 仅有既有菜单代码的
    C4996 弃用警告（非本次引入）。
  - `SilverHalide.exe` 链接成功。
- **NAFNet 冒烟**：下载 → 解压 → 加载 → 推理全链路跑通。

## 待办 / 后续

- 运行时冒烟：启动 GUI，走 设置 → 模型管理…，确认表格 / 状态色 / 下载进度 / 「打开
  缓存目录」交互。
- 模型版本升级提示：manifest `version` 变更时提示用户重新下载。
- 缓存删除功能：允许从缓存目录删除已下载模型。
- U²Net-sky 替换为可商用许可的 sky 分割模型，填补 `heuristicFallback` 之外的空缺。
