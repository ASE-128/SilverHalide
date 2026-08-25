# Goal: 实现软打样（Soft Proofing）

> 原 `docs/archive/PHASE_4.md` 中 §4 独立成 Goal。唯一不依赖推理基础设施的 Phase——
> 只用现有 `ColorManager`（lcms2）与 ICC 配置文件，是最独立的收尾功能。

## 前置依赖

- 无 Phase 级依赖。依赖现有 `ColorManager`（lcms2 封装，`loadDisplayProfile` / transform 机制）。

## 目标

利用现有 `ColorManager`（lcms2）实现印刷预览与色域警告。

### 1. 功能

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

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`SoftProofing` 为纯 C++ lcms2 封装。
2. **非破坏性原则**：软打样仅预览，不写入文件。
3. **线程安全**：软打样 transform 复用现有 `shared_mutex` 保护（与 `loadDisplayProfile` 重建互斥）。
4. **测试覆盖**：加载测试 ICC，验证超色域像素被正确标记。

## 文件清单（需修改/新增）

### 修改

- `src/engine/ColorManager.h` / `.cpp` — 新增软打样变换创建接口。
- `src/ui/ImageViewport.h` / `.cpp` — 支持软打样模式与色域警告红色叠加。
- `src/ui/MainWindow.h` / `.cpp` — 新增「软打样设置」菜单。

### 新增

- `src/engine/SoftProofing.h` / `.cpp` — lcms2 软打样与色域警告封装（纯 C++）。
- `src/ui/SoftProofingDialog.h` / `.cpp` — ICC 配置文件选择与意图设置对话框。
- `tests/TestSoftProofing.cpp` — 色域警告标记验证。

## 验收标准

1. 选择一张高饱和照片，加载 CMYK 测试 ICC（如 `CoatedFOGRA39.icc`）。
2. 开启软打样，预览区显示模拟印刷后的暗淡版本（色域压缩可见）。
3. 按 Shift+G，超色域区域以红色叠加高亮。
4. 关闭软打样，立即恢复标准 sRGB 预览。
5. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- 印刷配置文件管理与远程打样服务：仅本地 ICC 文件选择。

## 参考

- lcms2 软打样：`cmsCreateProofingTransform`, `cmsFLAGS_GAMUTCHECK`
- 现有 `ColorManager` 的 `D65→ProPhoto` 矩阵与 `toDisplaySRGB` 管线。

## 实现提示

1. **GAMUTCHECK 复用 lcms2**：用 `cmsFLAGS_GAMUTCHECK` + `cmsSetAlarmCodes` 把超色域像素标记为告警色，在预览叠加层显示，不改动管线数据。
2. **设置持久化**：软打样是应用级设置，存 `QSettings` 而非 `Adjustments`，避免进入参数哈希与 XMP 往返。
