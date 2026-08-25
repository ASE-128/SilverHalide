# M2 里程碑状态 — 真正 1:1 全尺寸放大（v2.1）

> 本文件由实施代理实时维护：每个子代理完成后立即更新。
> 目标计划见 [MILESTONE-PLAN.md](MILESTONE-PLAN.md) §3。仓库非 git，按「一文件一写者」切分并行。

## 验收标准（计划 §3.2）— 代码层全部完成；真机验收待用户确认（见「遗留」）

- [x] `Ctrl+1`/双击 → 真正的 1:1：**1 图像像素 = 1 物理像素**（`zoom_ * devicePixelRatioF() * nativeFactor`，含 dpr 换算）。
- [x] 进入 1:1 时后台全尺寸解码，就绪后无缝替换预览；等待期间沿用 1920 预览（按 `nativeSize` 真实 1:1 取景缩放，渐进式不白屏）。
- [x] 全尺寸渲染不阻塞 UI（worker `fullResLatest_` 代次丢弃 + 主线程 `fullResActive_` guard 丢弃在途结果）。
- [x] 内存受控：`fullResLinear_` 同一时刻至多 1 份；全尺寸渲染走 `renderLinearFull`+`toDisplaySRGB`（不塞 LRU，测试断言覆盖）。
- [x] 修复缓存容量不一致：`Constants.kCacheCapacityMB` 现为 `Pipeline` 默认容量唯一来源（默认 64MB，测试断言覆盖）。
- [x] 退出 1:1 / 换图时释放全尺寸缓冲（Fit→`clearFullRes`；换图→`clearFullRes` + `setNativeImageSize(QSize())`；`loadImage` 内防御性释放）。

## 任务状态

| 任务 | 内容 | 状态 | 归属文件 | 代理 |
|------|------|------|---------|------|
| M2.1 | 缓存与常量清理 + `renderFullDisplay`（全尺寸 U8 渲染不缓存） | ✅ 已完成 | `src/engine/Pipeline.h/.cpp`、`src/app/Constants.h`、`tests/TestPipeline.cpp` | A |
| M2.2 | PreviewWorker 全尺寸请求（`loadFullRes`/`adjustFullRes`/`clearFullRes` + `fullResReady`） | ✅ 已完成 | `src/ui/PreviewWorker.h/.cpp` | B |
| M2.3 | ImageViewport 真 1:1（OneToOne 语义、中心保持、全尺寸就绪换图） | ✅ 已完成 | `src/ui/ImageViewport.h/.cpp` | C |
| M2.4 | MainWindow 联动 + 收口（Ctrl+1 触发/退出、缓存容量接线、直方图） | ✅ 已完成 | `src/ui/MainWindow.h/.cpp` | C |

## 共享接口契约（Phase 1 两路 + Phase 2 收口共同遵守，接口先行约定）

- **PreviewWorker 全尺寸接口**（B 实现，C 依赖，签名不得改动）：
  - 槽：`loadFullRes(qint64 imageId, const QString& filePath, qint64 generation, const lrc::Adjustments&)`（全尺寸解码 `halfSize=false,maxDim=0` + 渲染 + 保留 `fullResLinear_`）；
    `adjustFullRes(qint64 imageId, qint64 generation, const lrc::Adjustments&)`（复用已解码全尺寸线性缓冲重渲染，不重复解码）；`clearFullRes()`（释放全尺寸缓冲 + 重置计数）。
  - 信号：`fullResReady(qint64 imageId, const lrc::ImageBufferPtr& rgbU8)`（与 `previewReady` 同形，复用已注册 metatype，无需改 MetaTypes.h）。
  - 信号（B 补充，C 依赖）：`imageMeta(qint64 imageId, int nativeWidth, int nativeHeight)` —— `loadImage` 解码成功后发出原生像素尺寸（用于等待期按真实 1:1 取景缩放 1920 预览）。`RawMeta` 增 `nativeWidth/nativeHeight`（maxDim 缩放前尺寸，`meta.width/height` 语义不变）。
  - 代次：独立 `fullResLatest_`，仅当 `generation == fullResLatest_` 才发信号（与预览的 `latestGeneration_` 互不干扰）。
  - 去重：同 `(imageId, filePath)` 已解码则复用 `fullResLinear_`，不重复解码。
- **Pipeline 新增**（A 实现，已完）：`renderFullDisplay(imageId, linear, adj)` — 全尺寸渲染到显示 sRGB U8，**不写入 LRU 缓存**（`renderFull` 仍会缓存，1:1 不能走它）。默认缓存容量直接读 `kCacheCapacityMB`（engine 包含 Qt-free 的 `app/Constants.h`），无需 MainWindow 接线；新增 getter `cacheCapacityBytes()`/`cacheBytes()`。
- **缓存容量对齐**（A，已完）：`Constants.kCacheCapacityMB` 成为唯一事实来源 —— `Pipeline` 默认容量读取该常量（`#include "app/Constants.h"`，Qt-free，引擎可安全包含）；新增 getter `cacheCapacityBytes()` 供测试断言。
- **ImageViewport**（C 内部设计）：`setNativeImageSize(QSize)` 记录图像真实像素尺寸，使等待期 1920 预览可按真实 1:1 缩放（targetRect OneToOne 分支 `scale = zoom_ * devicePixelRatioF() * (nativeSize_.width()/image_.width())`）；`fullResNeeded(bool)` 信号让 MainWindow 触发 `loadFullRes`/`clearFullRes`；全尺寸就绪换图时保持视口中心不动（`setImageKeepView`）。
- **构建分工**：A 独占 `build_headless`（引擎/测试，headless 可全量验证）；B 用 `build_gui` 验证 UI 编译；C 收口时最终构建 `build_gui` + windeployqt。两构建目录生成器均为 VS 17 2022，`cmake --build <dir> --config Release`。

## 构建 / 测试状态

| 项 | 状态 | 结果 |
|----|------|------|
| headless 构建 + `lrc_tests.exe` 全量 | ✅ | **131 用例 / 822 断言全绿**（基线 125/745 → +6 用例 / +77 断言，复核确认） |
| GUI 构建（`build_gui\Release\lrc_lightroom.exe`，3.2MB） | ✅ | 构建成功，仅既有 C4819/C4996 告警 |
| Qt 运行时部署（`windeployqt` → `platforms/qwindows.dll`） | ✅ | 已部署 |
| `ctest --test-dir build_gui -C Release` | ✅ | 1/1 通过 |
| 手动验收 UI（真机 1:1 精确性/延迟/内存） | ⏸ | 待用户验收（见「遗留」） |

## 日志

- 2026-08-11：开工 M2，创建本文档。确认现状：`PreviewWorker::loadImage` 硬编码 1920+halfSize；`ImageViewport::OneToOne` 为假 1:1；`renderFull` 会把全尺寸塞进 LRU（1:1 不能走它）；Pipeline 默认 48MB 与 `kCacheCapacityMB=64` 不一致。
- 2026-08-11：**Phase 1 两路并行派发**。代理 A（M2.1 引擎/常量/测试，独占 `build_headless`）；代理 B（M2.2 PreviewWorker，不构建，改动由 C 收口编译）。关键设计定案：**B 走已有 `renderLinearFull` + `colorManager_->toDisplaySRGB`（均 public 且不缓存），零跨代理编译竞态**；A 把 `kCacheCapacityMB` 变成 Pipeline 默认容量唯一来源（engine 包含 Qt-free 的 `app/Constants.h`）+ 新增 `cacheCapacityBytes()`/`cacheBytes()` getter + maxDim=0 全尺寸/不缓存测试。C 待 A/B 完成后派发（M2.3+M2.4 收口，GUI 构建验证）。
- 2026-08-11：**代理 B 完成（M2.2）**。契约签名逐字一致：`loadFullRes`/`adjustFullRes`/`clearFullRes` 槽 + `fullResReady` 信号。新成员 5 个（`fullResLinear_`/`fullResImageId_`/`fullResPath_`/`fullResLatest_`/`fullResAdjHash_`）。全尺寸渲染走 `renderLinearFull`+`toDisplaySRGB`（不缓存）；两级去重（同图复用线性缓冲、同 imageId+adjHash 整体跳过）；独立代次 `fullResLatest_`；`loadImage` 内换图自动释放全尺寸（同一时刻至多 1 份）；解码失败复用 `loadFailed`。无新增 metatype（`ImageBufferPtr` 已注册）。B 未构建，编译验证交由 C 收口。待 A。
- 2026-08-11：**代理 A 完成（M2.1）**。缓存容量对齐落地：`Pipeline` 默认容量读 `kCacheCapacityMB`（engine 含 `app/Constants.h`），新增 `renderFullDisplay`（全尺寸 sRGB U8、不缓存）、`cacheCapacityBytes()`/`cacheBytes()` getter。追加 6 个 TEST_CASE：maxDim=0 全尺寸正确性、半尺寸 vs 全尺寸面积平均一致性（曝光 +0.5 档可交换）、`renderLinearFull`/`renderFullDisplay` 不缓存断言、`render(...,0)` 仍缓存（记录现状）、默认容量=常量断言、`setCacheCapacity` 生效。headless 构建成功，**131 用例 / 822 断言全绿**（+6 用例 / +77 断言），ctest 100% passed。
- 2026-08-11：**GUI 冒烟构建通过**（收口前基线校验）。`cmake --build build_gui --config Release` 成功，`build_gui\Release\lrc_lightroom.exe` + `lrc_tests.exe` 生成，仅既有 C4819/C4996 告警。A+B 改动在 Qt 构建下可一起编译。
- 2026-08-11：**Phase 2 派发**。向 B 发补充任务（`RawMeta` 增 `nativeWidth/nativeHeight` + `imageMeta` 信号，供等待期真实 1:1 取景），并派发收口代理 C（M2.3 真 1:1 语义含 DPR/等待期取景/中心保持/双击切换 + M2.4 MainWindow 接线/1:1 调整路由/换图释放；C 独占 ImageViewport+MainWindow，收口做 GUI 构建 + windeployqt + headless 复核）。
- 2026-08-11：**代理 B 补充任务完成**。`RawMeta` 增 `nativeWidth/nativeHeight`；`imageMeta(imageId, nativeW, nativeH)` 信号在 `loadImage` 解码成功后发出。**关键修正**：预览路径 `halfSize=true` 下 `img->width` 已被减半，不能作原生尺寸 —— RAW 路径改从 libraw `sizes.iwidth/iheight`（含 `flip&4` 90° 翻转）取全尺寸解码的真实输出尺寸，恰与 `loadFullRes`（halfSize=false）输出一致，`native/previewDim` 即正确 1:1 缩放比。非 RAW 用全尺寸 `linear` 尺寸。无 `RawMeta` 聚合初始化受影响。
- 2026-08-11：**代理 C 完成（M2.3 + M2.4）收口**。ImageViewport：`currentScale() = zoom_*dpr*nativeFactor` 真 1:1、`setNativeImageSize`、`setImageKeepView`（中心保持）、`fullResRequested(bool)` 信号 + 去重（进入 OneToOne/滚轮过 100% 触发、回 Fit/低于 95% 释放、拖拽不误触发）、`mouseDoubleClickEvent` Fit↔OneToOne 切换。MainWindow：三路接线（`fullResReady`/`imageMeta`/`fullResRequested`）、`requestFullRes`/`clearFullRes`/`requestAdjustFullRes`（invokeMethod 复用 `previewGeneration_`）、`onFullResRequested`（维护 `fullResActive_`）、`onFullResReady`（guard imageId + `!fullResActive_` 丢弃退出后在途结果，兜底 nativeSize，`setImageKeepView`，直方图+状态栏原生尺寸）、`onPreviewReady` 在 1:1 期间不 setImage 只更新直方图、换图释放。**GUI 构建 + windeployqt 成功**；headless **131 用例 / 822 断言全绿**；ctest 1/1。
- 2026-08-11：**收尾 sanity check（协调者复核）**。`build_headless\tests\Release\lrc_tests.exe` 全绿（822/131）；`build_gui\Release\lrc_lightroom.exe`（3.2MB）+ `platforms/qwindows.dll` 就位；grep 确认 `setNativeImageSize`/`setImageKeepView`/`fullResRequested`/`loadFullRes`/`fullResReady`/`imageMeta`/`onFullResReady` 接线全部落地。**M2 代码层完成**。

## 遗留（待用户真机验收后处理；M1 验收流程同款）

- **真机验收 UI**：dpr=1（多数 Windows 100% 缩放）下 1:1 精确；dpr>1 需真机确认「1 图像像素=1 物理像素」视觉符合预期（HiDPI 缩放屏可能有公式方向取舍，见代理 C 备注）。
- **性能实测**：6000×4000 ARW 进入 1:1 的延迟与峰值内存（U16 解码 ~144MB + U8 输出 ~72MB + QImage 拷贝峰值）需真机实测并记录到 STATUS.md（计划 §3.5）。
- **小瑕疵（未纳入本次范围）**：1:1 期间若 mask 工具激活，红色权重叠加为 1920 分辨率旧缓冲（绘制时被缩放，无崩溃）。
- **版本号同步**：`src/app/Constants.h` `kAppVersion` 现 "2.0.0"，M2 验收通过后升 **v2.1.0** 并同步 STATUS.md / README.md / CLAUDE.md（含测试计数 131/822、1:1 功能条目）。计划 §6。
- 手动验收步骤建议：`.\build_gui\Release\lrc_lightroom.exe` → 导入/打开一张 RAW → `Ctrl+1` 或双击 → 观察等待期 1920 预览 1:1 取景 → 全尺寸就绪后换入清晰视图且中心不动 → 滚轮缩放/`Ctrl+0` 退出 → 换图确认内存释放。
