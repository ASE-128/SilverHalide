# Goal: 实现关键字与元数据管理

> 原 `docs/archive/PHASE_3.md` 中 §3 独立成 Goal。相对独立（数据库 schema + UI 面板），
> 是 PHASE_9（智能收藏夹）的字段来源之一。

## 前置依赖

- 无 Phase 级硬依赖。依赖现有 `catalog/CatalogDatabase`、`XmpProfile` / `XmpDocument`。

## 目标

实现照片的关键字标签与 IPTC 基础元数据，支持检索与 XMP 往返。

### 1. 数据模型

- 扩展 `catalog/CatalogDatabase`：
  - 新增 `keywords` 表：`id` (PK), `name` (TEXT UNIQUE), `parent_id` (自引用，支持层级关键字如 "风光 > 冰岛")。
  - 新增 `photo_keywords` 关联表：`photo_id`, `keyword_id`。
  - 扩展 `photos` 表（或新增 `metadata` 表）：`title`, `caption`, `copyright`, `creator`, `date_taken` 等基础 IPTC 字段。
- **关键字分配**：
  - 单张照片：右侧面板新增 `Metadata` 面板，含关键字输入框（逗号分隔或标签云）、标题、说明、版权。
  - 批量分配：图库视图多选照片，右键「添加关键字」。
  - 层级关键字：输入 "风光/冰岛/瀑布" 自动创建层级关系（或扁平化存储，UI 显示层级）。

### 2. 检索与筛选

- **Navigator 左栏**：新增「关键字列表」分组，按关键字聚合照片数量（如 "风光 (42)"）。点击关键字过滤 Filmstrip。
- **搜索栏**：顶部工具栏新增搜索框，输入关键字实时过滤图库。
- **筛选器**：与现有评分/标签筛选器叠加，支持 "5星 AND 风光 AND 2026"。

### 3. XMP 与导入

- **写入 XMP sidecar**：扩展 `XmpProfile` / `XmpDocument`，写入 `dc:subject`（关键字数组）、`dc:title`、`dc:description`、`dc:rights`、`dc:creator`。
- **读取 XMP sidecar / 导入时**：从 RAW 内嵌 XMP 或 sidecar 提取关键字与元数据，写入数据库。
- **与 LRC 兼容**：LRC 的关键字存储在 `lr:hierarchicalSubject` 和 `dc:subject`，两者均需支持。

### 4. UI

- `DevelopPanel` 或新增 `MetadataPanel`：标题（单行）、说明（多行）、版权（单行）、关键字（TagInput 风格，支持回车添加、点击删除）。
- Filmstrip 缩略图下方可显示标题（可选，设置项控制）。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。元数据通过纯数据结构传递。
2. **非破坏性原则**：关键字不修改 RAW 文件（除非写入 sidecar 时修改外部 XMP）。
3. **数据库事务**：关键字批量分配使用 SQLite 事务保证原子性。
4. **测试覆盖**：新增 Catch2 测试用例，覆盖关键字增删改/层级/搜索过滤/XMP 往返。
5. **性能**：关键字搜索在 10000 张照片库中 < 200ms。

## 验收标准

1. 对一张照片添加关键字 "风光/冰岛/瀑布"，自动创建三级层级。
2. Navigator 左栏显示关键字聚合数量，点击过滤 Filmstrip。
3. 搜索框输入关键字实时过滤图库。
4. 多选照片批量添加关键字。
5. 写入 XMP sidecar，用 Lightroom Classic 打开，关键字/标题/说明/版权一致。
6. 从 LRC 的 XMP 导入关键字，层级与关键字正确还原。
7. 10000 张照片库中关键字搜索 < 200ms。
8. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- 地理标签（GPS Map）：元数据仅到关键字/标题/版权层级。
- 人脸识别/AI 自动关键字：纯手动关键字。

## 参考

- 现有 `XmpProfile` 的 crs 字段映射，扩展 dc: 命名空间。
- Lightroom Classic 行为：关键字存储于 `lr:hierarchicalSubject` 与 `dc:subject`。

## 实现提示

1. **层级存储**：`keywords` 表自引用 `parent_id`，导入 "风光/冰岛/瀑布" 时自顶向下查找/创建。
2. **检索**：关键字过滤用 `photo_keywords` 关联表 JOIN，可加索引保证 10000 张量级 < 200ms。
