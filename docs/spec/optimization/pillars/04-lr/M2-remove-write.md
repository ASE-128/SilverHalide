# Milestone LR M2：移除 Lightroom 写入兼容

> Status: DRAFT
> Milestone: LR M2
> Last updated: 2026-09-24
>
> 本里程碑落实 [ADR-0001](../../adr/0001-drop-lightroom-compat.md) 与
> [04-drop-lr-compat.md](../04-drop-lr-compat.md) 中 **写入侧（Write）** 的移除，
> 是 [00-ROADMAP.md](../00-ROADMAP.md) Phase 4「自成体系（LR 移除落地）」在写方向上的拆解节点。
> 纯文档，不改任何代码。

## 1. 结论声明

与 ADR-0001 保持一致：**Lightroom 兼容的写入能力整体移除**。本应用不再向任何 LR
数据产物**写出**——不导出 LR 预设，不向 crs 命名空间写属性，不写 `.xmp` sidecar。
产品数据契约以自有 `.lrp` 预设与自有 sidecar / 目录体系（`.shcat`）自洽输出
（见 master 定位 [optimization-positioning-spec.md](../../optimization-positioning-spec.md)）。

## 2. 移除范围（写入侧）

本里程碑负责 `04-drop-lr-compat.md` §2.1「写入侧（移除）」的落地：

- Lightroom 预设**导出**（XMP 预设 `*.xmp` 写出）。
- LR 兼容 XMP / sidecar **写出**（`.xmp` sidecar 写盘）。
- crs 命名空间**写出**（`http://ns.adobe.com/camera-raw-settings/1.0/`）。

## 3. 移除点清单（写入侧落点）

> 依据 `04-drop-lr-compat.md` §3 的移除点清单按「读/写职责」拆分。下表仅列该项目
> **写方向**的子职责；各文件承载的**读方向**职责由同一份配对里程碑
> [M1-remove-read.md](M1-remove-read.md) 承接，两者并集覆盖整表，不冲突、不重复计算。

| 文件 | 写方向子职责（本里程碑移除） | 备注 |
|------|-------------------|------|
| `src/engine/Xmp.h` / `Xmp.cpp` | XmpDocument 的**序列化/写出**路径：crs: 命名空间收集与序列化、`crs:SpotCorrections` 写出、dc:/lr: 元数据写出 | 同一文件的解析/读取分支归 M1 |
| `src/engine/XmpSidecar.h` / `XmpSidecar.cpp` | `.xmp` sidecar 的**写入**：`writeToSidecar`（`.xmp` 写盘） | `readFromSidecar`/`readFromEmbedded` 归 M1 |
| `src/utils/XmpProfile.h` / `XmpProfile.cpp` | crs 映射层的**写**方向：`toCrsAttrs`（`Adjustments` → crs）、SpotCorrections 双向写出 | `fromCrsAttrs` 归 M1 |
| `src/ui/MainWindow.cpp`（exportPreset ~2673、sidecar 写 ~2741） | LR XMP 预设**导出**分支、sidecar **写入**入口与 `.xmp` 过滤器（写方向 UI） | importPreset / sidecar 读槽归 M1 |
| `tests/TestXmpDocument.cpp` | XmpDocument crs 序列化、dc:/lr: 元数据写出的测试 | — |
| `tests/TestXmpProfile.cpp` | XmpProfile `toCrsAttrs` 写方向映射测试 | — |
| `tests/TestXmpSidecar.cpp` | XmpSidecar `writeToSidecar` 写入测试 | — |

配套动作：上述 `TestXmp*` 在 `tests/CMakeLists.txt` 中的注册一并移除（与 M1 共享；
M2 再落地写出类用例的删除点）。

## 4. 保留（明确不做）

以下均**不属 LR 兼容写入**，M2 不触碰：

- 本应用自有 `.lrp` 预设导出（JSON，经 `src/utils/ProfileIO`）的 **savePreset / toJson** 写出。
- 本应用自有目录体系 `.shcat`（目录即库）的写出与持久化，与历史 `.lrcat → .shcat`
  文件名迁移（`src/catalog/CatalogDatabase`、`src/app/Application.cpp`，非 LR 格式）。
- 自有 sidecar 契约：仅移 LR XMP sidecar 写出，不删自有 sidecar 写入逻辑。
- `src/utils/PresetLibrary` 的 `.lrp` 预设库写出（经 ProfileIO，非 XMP）；`TestPresetLibrary.cpp`
  中 `destPath .xmp` 仅为任意扩展名测试，内容仍是 JSON，予以保留。

## 5. 关联文档与约束

- Master 定位：[optimization-positioning-spec.md](../../optimization-positioning-spec.md)
  （In scope / Assumptions；Out of scope：不再新增 LR 对齐投入，不以"能否与 LR 互导"为验收标准）。
- 决策：[ADR-0001](../../adr/0001-drop-lightroom-compat.md)（读+写均移除）。
- 落点子：父文档 [04-drop-lr-compat.md](../04-drop-lr-compat.md) §2.1 / §3 表格。
- 路线图：[00-ROADMAP.md](../00-ROADMAP.md) Phase 4 与方向级 AC-3 / AC-4。
- 配对里程碑：[M1-remove-read.md](M1-remove-read.md)（读方向并集）。

## 6. Acceptance criteria（二值、可验证）

- **AC-M2-1**：M2 写入侧代码清点后，`src/` 中不再存在可写出 LR XMP 预设、crs 命名空间、
  `.xmp` sidecar 的调用链（按 §3 文件清单可 grep 到 0 处写方向命中）。
- **AC-M2-2**：headless 全量编译 + ctest 全绿（`build_headless`）；移除写出类用例后
  `TestPresetLibrary` / `TestCatalogDatabase` 等保留用例仍全绿，无针对 LR 写出的用例残存。
- **AC-M2-3**：自有 `.lrp` 预设写→读往返自洽（ProfileIO / PresetLibrary 走 JSON，不依赖任何
  被移除的 XMP/crs 写出骨架）。
- **AC-M2-4**：`.shcat` / 自有 sidecar / `.lrp` 契约可被本应用正常写出，功能不受移除影响。

## 7. 风险与回退

- 与 04 文档一致：写侧移除后不再生成 LR 可导入的预设文件；一次性与 LR 的互导能力彻底关闭，
  需在 README/ARCHITECTURE 更新为"自成体系、不兼容 LR"，防止未来误加回（04 §6、AC-4）。
- 回退：删除/改造的关键文件保留在 git 历史，必要时 revert 重建并重新验证 XMP/crs 匹配
  与 golden sample 测试（见 04 §6）。