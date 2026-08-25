# Goal: 实现虚拟副本（Virtual Copies）

> 原 `docs/archive/PHASE_3.md` 中 §1 独立成 Goal。数据库层改动最大（`edits` 表改复合主键），
> 奠定 `variant_id` 基础，先于图库线其他 Phase 落地。

## 前置依赖

- 无 Phase 级依赖。依赖现有 `catalog/CatalogDatabase` 的 `edits` / `history` / `photos` 表结构，
  `FilmstripWidget` / `NavigatorWidget` 分组逻辑。

## 目标

允许同一张物理 RAW 文件创建多个逻辑副本，各自拥有独立的参数堆叠、编辑历史与快照，不额外占用磁盘空间。

### 1. 数据模型

- 扩展 `catalog/CatalogDatabase` 的 `edits` 表：
  - 当前主键假设为 `photo_id`（或隐含单条记录）。改为复合主键：`photo_id` + `variant_id`（INTEGER，默认 0 表示主副本/母版）。
  - 新增 `variants` 表（或复用 `edits` 表）：`photo_id`, `variant_id`, `name`（TEXT，如 "黑白版"）, `is_master`（BOOL，仅一条为 true）, `created_at`。
  - `edits` 表 JSON 列现在按 `(photo_id, variant_id)` 存储独立参数堆叠。
  - `history` 表（编辑历史快照）同样扩展 `variant_id`。
- **虚拟副本创建**：
  - 右键菜单/照片菜单「创建虚拟副本」：复制当前 `variant_id` 的全部参数堆叠到新的 `variant_id = max+1`，`name` 默认为 "副本 1/2/3"。
  - 不复制 RAW 文件，不复制缩略图（共享同一张 `thumbnail` / `standard_preview` / `full_preview`）。
  - 删除虚拟副本：仅从数据库删除该 variant 的 edits/history 记录，不影响物理文件与其他 variant。
  - 删除母版：若存在虚拟副本，禁止直接删除母版（或提示将某一副本提升为母版）。

### 2. UI 交互

- **Filmstrip**：虚拟副本显示与母版相同缩略图，但角标区分（如页码角标或叠加数字 badge）。
- **Navigator**：左栏「目录图片库」下新增「母版照片」与「所有虚拟副本」分组，或仅在照片列表中平铺显示。
- **切换**：Filmstrip 点击虚拟副本时，中央预览区加载同一张 RAW 但应用该 variant 的参数堆叠。
- **重命名**：右键虚拟副本可重命名（如 "高对比黑白"）。
- **导出**：导出对话框可选择导出母版或任意虚拟副本，行为与单张照片一致。

### 3. XMP 与兼容性

- 写入 XMP sidecar 时，默认写入母版（variant_id=0）的参数。
- 虚拟副本的参数不写入 sidecar（LRC 也不写虚拟副本到 XMP，仅 catalog 内）。
- 导入含虚拟副本的 LRC catalog（若未来支持）预留接口。

### 4. 测试

- 创建 3 个虚拟副本，各自独立调整曝光/HSL，验证数据库中各 variant 参数隔离。
- 删除虚拟副本，验证母版与其他副本不受影响。
- 重启应用，验证各 variant 自动恢复。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。variant 参数堆叠通过纯数据结构传递。
2. **非破坏性原则**：虚拟副本不修改 RAW 文件，仅新增逻辑记录。
3. **数据库事务**：虚拟副本创建使用 SQLite 事务保证原子性。
4. **性能**：虚拟副本切换延迟 < 100ms（复用 Standard Preview，仅参数堆叠切换）。
5. **测试覆盖**：新增 Catch2 测试用例，覆盖虚拟副本的创建/删除/参数隔离/重启恢复。

## 验收标准

1. 对一张照片创建 2 个虚拟副本（"彩色版" / "黑白版"），分别调整曝光/HSL，预览各自独立。
2. Filmstrip 中虚拟副本角标区分于母版，缩略图相同。
3. 删除其中一个虚拟副本，母版与另一个副本不受影响。
4. 重启应用，各 variant 自动恢复。
5. 导出对话框可选择导出母版或指定副本。
6. 写入 XMP sidecar，仅包含母版参数。
7. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- 虚拟副本的堆叠/展开（Stack）：仅平铺显示 + 角标区分。
- LRC catalog 导入含虚拟副本数据：仅预留接口。

## 参考

- 现有 `CatalogDatabase` 的 `edits` / `history` / `photos` 表结构。
- 现有 `Collection` / `Folder` 的 Navigator 分组逻辑。
- Lightroom Classic 行为：虚拟副本共享物理文件。

## 实现提示

1. **variant_id 兼容性**：现有 `edits` 表若无 variant_id 列，迁移策略为：新增列并默认 0，所有现有记录视为母版。确保 `SELECT` 语句默认 `variant_id = 0` 以兼容旧代码路径。
2. **共享预览**：虚拟副本共享同一张 `thumbnail` / `standard_preview` / `full_preview`，切换仅切换参数堆叠，可复用 PHASE_2 的预览缓存。
