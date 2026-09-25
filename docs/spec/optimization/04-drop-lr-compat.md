# 移除 Lightroom 兼容专项细化

> Status: DRAFT
> Last updated: 2026-09-22
>
> 本专项落实 [ADR-0001](../../adr/0001-drop-lightroom-compat.md) 的读+写均移除决策，
> 是 docs/spec/optimization-positioning-spec.md（In scope / Assumptions）的执行落地文档。
> 本文纯文档，不改任何代码。

## 1. 结论声明

本专项落实 ADR-0001 的最终结论：**Lightroom 兼容的读与写均移除**。

- 写侧：不再向 Lightroom 的 crs 命名空间 / XMP sidecar / LR 预设导出任何数据。
- 读侧：不再识别 LR 预设 `.xmp` / `.lrcat` / LR sidecar / crs 字段映射。
- 产品数据契约由自有 `.lrp` 预设与自有 sidecar / 目录体系自洽；Master 文档
  [optimization-positioning-spec.md](../optimization-positioning-spec.md) 的 Assumption 已声明
  "旧 LR 产物不再识别"。

## 2. 移除范围

### 2.1 写入侧（移除）

- Lightroom 预设导出（XMP 预设 `*.xmp` 写出）。
- LR 兼容 XMP / sidecar 写出（`.xmp` sidecar 写盘）。
- crs 命名空间写出（`http://ns.adobe.com/camera-raw-settings/1.0/`）。

### 2.2 读取侧（移除）

- LR 预设 `.xmp` / 目录 `.lrcat` 的导入。
- crs 字段映射解析（crs: 属性 → `Adjustments`）。
- LR sidecar 读取（`.xmp` 读取并入编辑堆栈）。

### 2.3 保留 / 不再新增

- 本应用自有 `.lrp` 预设（JSON，经 `ProfileIO`）。
- 本应用自有 sidecar / 目录体系契约（`.shcat` 目录即库、`.lrp` 预设库）。
- 不再新增任何 LR 对齐投入；不再以"能否与 LR 互导"作为验收标准（对应 ADR 与
  positioning spec 的 P-C 移出支柱）。

## 3. 移除点清单（基于 `src/` 与 `tests/` 实据）

> 说明：以下 `.lrcat` 为 Silver Halide 自身历史目录扩展名（目录即库重构前），
> 属自有格式演进，非 LR 兼容路径，予以**保留**。

| 文件 | 职责(读/写) | 动作(移除/保留) |
|------|-------------|-----------------|
| `src/engine/Xmp.h` / `Xmp.cpp` | XMP/RDF 文档层：crs: 命名空间收集/序列化、`crs:SpotCorrections`、dc:/lr: 元数据（XmpDocument） | **移除**（读+写） |
| `src/engine/XmpSidecar.h` / `XmpSidecar.cpp` | `.xmp` sidecar 读/写（readFromSidecar / writeToSidecar / readFromEmbedded） | **移除**（读+写） |
| `src/utils/XmpProfile.h` / `XmpProfile.cpp` | crs 映射层：toCrsAttrs（写）/ fromCrsAttrs（读）、SpotCorrections 双向 | **移除**（读+写） |
| `src/ui/MainWindow.cpp`（importPreset ~2633、exportPreset ~2673、sidecar 读/写 ~2723/2741） | LR XMP 预设导入导出、sidecar 读/写 UI 入口（`.xmp` 过滤器） | **移除**（读+写） |
| `tests/TestXmpDocument.cpp` | XmpDocument crs 解析与序列化测试 | **移除**（读+写） |
| `tests/TestXmpProfile.cpp` | XmpProfile crs 双向映射测试 | **移除**（读+写） |
| `tests/TestXmpSidecar.cpp` | XmpSidecar 读/写测试 | **移除**（读+写） |
| `src/utils/ProfileIO.h` / `.cpp` | `.lrp` 预设 JSON 序列化（savePreset/loadPreset/toJson/fromJson），自有格式核心 | **保留** |
| `src/utils/PresetLibrary.h` / `.cpp` | `.lrp` 预设库管理（经 ProfileIO，非 XMP）；`TestPresetLibrary.cpp` 中 destPath `.xmp` 仅为任意扩展名测试，内容仍为 JSON | **保留** |
| `src/catalog/CatalogDatabase.h` / `.cpp` | 目录库 SQLite/资源迁移，配套 ProfileIO JSON；crs/XMP 不进入此处 | **保留**（自有 `.lrcat→.shcat` 历史演进） |
| `src/app/Application.cpp` / `.h`、`src/app/Constants.h` | 目录路径迁移：`Default.lrcat` / `catalog.lrcat` 均为自身历史目录文件名迁移，非 LR XMP 兼容 | **保留** |
| `src/ui/ImportDialog.*`、`src/ui/ImportWorker.h`、`src/catalog/LibraryManager.h`、`src/ui/DevelopPanel.h`、`PanelSection.h`、`CurveEditor.*`、`FilmstripWidget.*`、`Adjustments.h`、`Pipeline.h`、`SoftProofing.h`、`StandardPreview.h`、`DngConverter.h`、`ZoneCurve.h` | grep 命中仅为"Lightroom-style"命名/参数语义/UI 风格的文字引用（Add/Copy/Move、zone curve、色标、软打样覆盖色等），非数据兼容路径 | **保留** |

## 4. 改动影响面

- **engine**：`src/engine/Xmp.h/.cpp`、`XmpSidecar.h/.cpp` 移除，触及 read path 引用方（如 `MainWindow`），不影响 `Adjustments` / `Pipeline` / 颜色处理。
- **utils**：`src/utils/XmpProfile.h/.cpp` 移除；`ProfileIO` / `PresetLibrary` 保留，不受影响。
- **ui**：`MainWindow.cpp` 中 importPreset / exportPreset 的 `.xmp` 分支与 sidecar 读/写槽移除，保留 `/`.lrp` 分支与预设库入口。
- **catalog / app**：无 LR 数据契约，仅文字引用；`Application.cpp` 的 `.lrcat` 路径迁移保留。
- **tests**：`TestXmpDocument.cpp` / `TestXmpProfile.cpp` / `TestXmpSidecar.cpp` 及其在 `tests/CMakeLists.txt` 的注册一并移除；`TestPresetLibrary.cpp` / `TestCatalogDatabase.cpp` / `test_standard_preview.cpp` / `test_postcrop_vignette.cpp` 中的无关命中不受影响。

**回归要求**：headless 全量编译 + ctest 全绿（`build_headless`）；`.lrp` 预设写→读往返自洽（`TestPresetLibrary`/`ProfileIO` 仍绿）；`README` / `ARCHITECTURE` 更新为"自成体系、不兼容 LR"的定位表述（AC-4）。

## 5. "Open question：既有用户数据迁移"收敛决策

**推荐方案（建议采纳）：提供一次性转化/提示引导工具。**

- 在 1.x 提供一次性的自有格式迁移入口：读取旧 LR `.xmp` / LR 预设（仅提示，不长期支持），
  产出等价 `.lrp` 预设，并把可映射的 sidecar 调整并入 `.shcat` 目录。产出后即不保留 LR
  只读骨架。
- 对无法映射/损失的部分（如 crs:SpotCorrections 复杂蒙版、virtual copy 语义）给出明确
  "不支持/已忽略"清单，避免静默丢数据。
- 同时在 README 明确"不再兼容 LR 预设"，作为兜底提示层。

> **这是产品决策**：方案（一次性转化工具 / 仅文档提示 / 完全放弃迁移）需用户最终确认后
> 再落到 1.x 排期；本专项仅先落地"读+写移除"的代码骨架，迁移入口是否实现取决于该决策。

## 6. 风险与回退

- **一次性破坏性改动**：LR 读+写移除是不可逆的方向性删除；回退成本高（revert 后仍需
  重新验证 XMP/crs 匹配逻辑与所有 golden sample 测试）。
- **旧 LR 数据不可逆**：一旦移除读路径，既有 LR 预设 / sidecar / 目录将无法再被本应用
  识别；若迁移工具不落地，用户数据即处于只读失效状态——需在发版公告与 UI 提示中明确。
- **缓解**：迁移工具决策先行（见第 5 节）；README/ARCHITECTURE 防回加；删除的关键文件
  保留在 git 历史，便于必要时重建，不作为长期维护代码。