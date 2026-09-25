# Milestone LR M1：移除 Lightroom 读取兼容

> Status: DRAFT
> Milestone: LR M1
> Last updated: 2026-09-24
>
> 本里程碑落实 [ADR-0001](../../../../adr/0001-drop-lightroom-compat.md) 与
> [04-drop-lr-compat.md](../../04-drop-lr-compat.md) 中 **读取侧（Read）** 的移除，
> 是 [00-ROADMAP.md](../../00-ROADMAP.md) Phase 4「自成体系（LR 移除落地）」在读方向上的拆解节点。
> 纯文档，不改任何代码。

## 1. 结论声明

与 ADR-0001 保持一致：**Lightroom 兼容的读取能力整体移除**。本应用不再识别任何 LR
数据产物作为输入契约——不导入 LR 预设，不解析 crs 字段映射，不读取 LR sidecar。
产品数据契约由自有 `.lrp` 预设与自有 sidecar / 目录体系（`.shcat` 目录即库）自洽演进
（见 master 定位 [optimization-positioning-spec.md](../../../optimization-positioning-spec.md) 的
In scope / Assumptions：旧 LR 产物不再识别）。

## 2. 移除范围（读取侧）

本里程碑负责 `04-drop-lr-compat.md` §2.2「读取侧（移除）」的落地：

- LR 预设 `.xmp` / 目录 `.lrcat` 的**导入**。
- crs 字段映射**解析**（crs: 属性 → `Adjustments`）。
- LR sidecar **读取**（`.xmp` 读取并并入编辑堆栈）。

## 3. 移除点清单（读取侧落点）

> 依据 `04-drop-lr-compat.md` §3 的移除点清单按「读/写职责」拆分。下表仅列该项目
> **读方向**的子职责；各文件承载的**写方向**职责由同一份配对里程碑
> [M2-remove-write.md](M2-remove-write.md) 承接，两者并集覆盖整表，不冲突、不重复计算。

| 文件 | 读方向子职责（本里程碑移除） | 备注 |
|------|-------------------|------|
| `src/engine/Xmp.h` / `Xmp.cpp` | XmpDocument 的**解析/读取**路径：读入 XMP/RDF，读取 dc:/lr: 元数据、收集 crs 命名空间的读分支 | 同一文件的序列化/写出分支归 M2 |
| `src/engine/XmpSidecar.h` / `XmpSidecar.cpp` | `.xmp` sidecar 的**读取**：`readFromSidecar` / `readFromEmbedded` | `writeToSidecar` 归 M2 |
| `src/utils/XmpProfile.h` / `XmpProfile.cpp` | crs 映射层的**读**方向：`fromCrsAttrs`（crs → `Adjustments`）、SpotCorrections 读入解析 | `toCrsAttrs` 归 M2 |
| `src/ui/MainWindow.cpp`（importPreset ~2633、sidecar 读 ~2723） | LR XMP 预设**导入**分支、sidecar **读取**入口与 `.xmp` 过滤器（读方向 UI） | exportPreset / sidecar 写槽归 M2 |
| `tests/TestXmpDocument.cpp` | XmpDocument crs 解析、dc:/lr: 元数据读取的测试 | — |
| `tests/TestXmpProfile.cpp` | XmpProfile `fromCrsAttrs` 读方向映射测试 | — |
| `tests/TestXmpSidecar.cpp` | XmpSidecar `readFromSidecar` 读取测试 | — |

配套动作：上述 `TestXmp*` 在 `tests/CMakeLists.txt` 中的注册一并移除（与 M2 共享；
M1 先落地读取类用例的删除点）。

## 4. 保留（明确不做）

以下均**不属 LR 兼容读取**，M1 不触碰：

- 本应用自有 `.lrp` 预设（JSON，经 `src/utils/ProfileIO`）的 **loadPreset / fromJson** 读取。
- 本应用自有目录体系 `.shcat`（目录即库）与历史 `.lrcat → .shcat` 文件名迁移
  （`src/catalog/CatalogDatabase`、`src/app/Application.cpp` 的 `.lrcat` 路径换名，非 LR 格式）。
- 自有 sidecar 契约：仅移 LR XMP sidecar 读取，不删自有 sidecar 读入逻辑。
- `src/utils/PresetLibrary` 的 `.lrp` 预设库读取（经 ProfileIO，非 XMP）；`TestPresetLibrary.cpp`
  中 `destPath .xmp` 仅为任意扩展名测试，内容仍是 JSON，予以保留。

## 5. 关联文档与约束

- Master 定位：[optimization-positioning-spec.md](../../../optimization-positioning-spec.md)
  （In scope / Assumptions：旧 LR 产物不再识别）。
- 决策：[ADR-0001](../../../../adr/0001-drop-lightroom-compat.md)（读+写均移除）。
- 落点子：父文档 [04-drop-lr-compat.md](../../04-drop-lr-compat.md) §2.2 / §3 表格。
- 路线图：[00-ROADMAP.md](../../00-ROADMAP.md) Phase 4 与方向级 AC-3 / AC-4。
- 配对里程碑：[M2-remove-write.md](M2-remove-write.md)（写方向并集）。

## 6. Acceptance criteria（二值、可验证）

- **AC-M1-1**：M1 读取侧代码清点后，`src/` 中不再存在可将 LR `.xmp` / `.lrcat`、crs、
  LR sidecar 读入编辑栈的调用链（按 §3 文件清单可 grep 到 0 处读方向命中）。
- **AC-M1-2**：headless 全量编译 + ctest 全绿（`build_headless`）；移除读取类用例后
  `TestPresetLibrary` / `TestCatalogDatabase` 等保留用例仍全绿，无针对 LR 读取的用例残存。
- **AC-M1-3**：自有 `.lrp` 预设写→读往返自洽（ProfileIO / PresetLibrary 走 JSON，不依赖任何
  被移除的 XMP/crs 读取骨架）。
- **AC-M1-4**：`.shcat` / 自有 sidecar / `.lrp` 契约可被本应用正常读取，功能不受移除影响。

## 7. 风险与回退

- 与 04 文档一致：读完移除后旧 LR 预设/目录/sidecar 无法再被识别；一次性迁移/引导方案
  （04 §5）/[spec Open questions](../../../optimization-positioning-spec.md) 需产品决策先行。
- 回退：删除/改造的关键文件保留在 git 历史，必要时 revert 重建并重新验证 XMP/crs 匹配
  与 golden sample 测试（见 04 §6）。