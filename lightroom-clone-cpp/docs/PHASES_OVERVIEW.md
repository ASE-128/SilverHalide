# Phase 顺序与依赖总览

> 本文件是 `docs/PHASE_1.md` – `docs/PHASE_15.md` 的索引：说明各 Phase 的编号、内容、
> 前置依赖与推荐执行顺序。原始未拆分文档归档于 `docs/archive/`
> （`PHASE_2.md` / `PHASE_3.md` / `PHASE_4.md` / `SPEEDUP_GOAL.md`）。

## 背景

原 `SPEEDUP_GOAL.md` 与 `PHASE_2/3/4.md` 各自打包了多个相互独立的 GOAL。按依赖拓扑
拆分为 14 个独立 GOAL 文档，重新编号 `PHASE_2` – `PHASE_15`（`PHASE_1` 为已完成的
Detail 面板，保持不变）。编号顺序即推荐的执行顺序——每个 Phase 的「前置依赖」小节
列出其必须在此之前完成的前置。

## 编号总览

| Phase | 标题 | 来源 | 前置依赖 |
|---|---|---|---|
| 1 | Detail 面板（锐化 + 手动降噪） | 已完成（v2.x） | — |
| **2** | 预览分辨率渲染 + 标准预览 + 增量管线 | SPEEDUP_GOAL | — |
| 3 | 裁剪后晕影（Post-Crop Vignette） | 原 PHASE_2 §3 | 2 |
| 4 | 颜色/明亮度范围蒙版 | 原 PHASE_2 §1 | 2 |
| 5 | 污点去除与修复画笔 | 原 PHASE_2 §2 | 2 |
| 6 | 虚拟副本 | 原 PHASE_3 §1 | — |
| 7 | Before/After 与参考视图 | 原 PHASE_3 §2 | 2 |
| 8 | 关键字与元数据 | 原 PHASE_3 §3 | — |
| 9 | 智能收藏夹 | 原 PHASE_3 §4 | 8 |
| 10 | 全景合成 | 原 PHASE_3 §5 | — |
| 11 | AI 推理基础设施（ONNX Runtime） | 原 PHASE_4 §0 | — |
| 12 | AI 降噪 | 原 PHASE_4 §1 | 11 |
| 13 | AI 蒙版（主体/天空/背景） | 原 PHASE_4 §2 | 11 + 4 |
| 14 | 重复检测 | 原 PHASE_4 §3 | 11 |
| 15 | 软打样 | 原 PHASE_4 §4 | — |

## 依赖关系图

```
PHASE_2 (性能地基) ──硬依赖──┬─→ PHASE_3 (晕影)
      │                      ├─→ PHASE_4 (范围蒙版) ──硬依赖──→ PHASE_13 (AI 蒙版)
      │                      ├─→ PHASE_5 (污点修复)
      │                      └─→ PHASE_7 (Before/After)
      │
      ├─ PHASE_6 (虚拟副本) ── 独立
      ├─ PHASE_8 (关键字) ──→ PHASE_9 (智能收藏夹)
      ├─ PHASE_10 (全景合成) ── 独立
      │
      └─ PHASE_11 (AI 基础设施) ──┬─→ PHASE_12 (AI 降噪)
                                   ├─→ PHASE_14 (重复检测)
                                   └─→ PHASE_13 (AI 蒙版，另需 PHASE_4)
         PHASE_15 (软打样) ── 完全独立
```

### 硬依赖（必须先完成）

| 依赖 | 理由 |
|---|---|
| **PHASE_2 → PHASE_3/4/5/7** | 这些功能的渲染插入点与性能验收都以 PHASE_2 定型的 Stage A–F 阶段划分、预览分辨率渲染与增量缓存为前提 |
| **PHASE_4 → PHASE_13** | AI 蒙版并入 PHASE_4 扩展后的 `Mask` 结构与组合权重逻辑 |

### 可并行（软依赖）

- **PHASE_6 / 8 / 10 / 11 / 15** 无 Phase 级前置，可独立开工（PHASE_15 软打样最独立，
  只用 `ColorManager` / lcms2）。
- **PHASE_12 / 14** 仅依赖 PHASE_11，可与 PHASE_2–5 线并行。
- 图库线（6–10）与渲染线（2–5）互相独立；AI 线（11–15）大部分与图库线独立。

## 推荐执行顺序（三条并行线）

```
线A（渲染主线）：PHASE_2 → PHASE_3 → PHASE_4 → PHASE_5
线B（图库线）  ：PHASE_6 → PHASE_7 → PHASE_8 → PHASE_9 → PHASE_10
                 （PHASE_7 需等线A的 PHASE_2 完成；PHASE_9 需等 PHASE_8）
线C（AI 线）   ：PHASE_11 → PHASE_12 / PHASE_14 → PHASE_13
                 （PHASE_13 另需等线A的 PHASE_4；PHASE_15 可随时插队）
```

关键策略：**先跑通 PHASE_2**（性能地基），随后三条线并行推进。

## 冲突 / 协调点（跨 Phase 共享文件，需串行或分支隔离）

1. **`CatalogDatabase` 一文件多写者**：PHASE_2（previews 加 preview_type）、PHASE_6
   （edits 复合主键 + variants）、PHASE_8（keywords/photo_keywords）、PHASE_9
   （smart_collections）、PHASE_14（photo_features）均改同一类——按「一文件一写者」串行。
2. **`XmpProfile` 两处扩展**：PHASE_4（crs 蒙版/污点/晕影字段）、PHASE_8（dc: 命名空间
   关键字/标题/版权）——同文件，需协调。
3. **`Adjustments` 哈希设计**：PHASE_2 的按 Stage 分组哈希须在 PHASE_4/5 新增字段前定型，
   否则增量缓存失效。
4. **全景合成依赖**：PHASE_10 若走 OpenCV 需先解决项目 `LRC_USE_OPENCV=OFF` 的引入方式。

## 归档说明

- `docs/archive/SPEEDUP_GOAL.md` — 原始性能目标文档（完整保留）。
- `docs/archive/PHASE_2.md` / `PHASE_3.md` / `PHASE_4.md` — 原始三大块未拆分文档。
- 各拆分文档头部标注了来源（如「原 `docs/archive/PHASE_2.md` 中 §1 独立成 Goal」）。
