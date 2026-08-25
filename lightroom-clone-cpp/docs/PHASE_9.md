# Goal: 实现智能收藏夹（Smart Collections）

> 原 `docs/archive/PHASE_3.md` 中 §4 独立成 Goal。规则字段包含 `keyword`，依赖
> PHASE_8 的关键字数据，故排在关键字之后。

## 前置依赖

- **PHASE_8**：规则谓词的 `keyword` 字段依赖关键字表与检索能力；`rating`/`label`/`flag`/`camera`
  等字段依赖现有照片元数据。
- 现有 `Collection` / `Folder` 的 Navigator 分组逻辑。

## 目标

允许用户创建基于规则的动态集合，照片自动进出，无需手动维护。

### 1. 数据模型

- 新增 `smart_collections` 表：`id` (PK), `name`, `predicate_json`（规则 JSON）, `match_type`（ALL/ANY）。
- 规则谓词（Predicate）结构：
  ```json
  {
    "match": "all",
    "rules": [
      {"field": "rating", "op": ">=", "value": 4},
      {"field": "label", "op": "==", "value": "red"},
      {"field": "camera", "op": "==", "value": "ILCE-7CM2"},
      {"field": "keyword", "op": "contains", "value": "风光"},
      {"field": "date", "op": "between", "value": ["2026-01-01", "2026-12-31"]},
      {"field": "file_type", "op": "==", "value": "ARW"}
    ]
  }
  ```

- **支持的字段**：rating, label, flag, camera, lens, keyword, date_taken, file_type, folder_path, has_adjustments（是否有非默认编辑）。
- **支持的运算符**：==, !=, <, <=, >, >=, contains, starts_with, in, between。
- **成员计算**：
  - 创建/编辑 Smart Collection 时，立即执行一次全表扫描匹配，结果存入 `smart_collection_photos` 关联表（或虚拟表/视图）。
  - 照片增删改（评分/标签/关键字变更）时，增量更新受影响的 Smart Collection 成员。若实现复杂，可接受在图库变更后延迟刷新（如切换视图时重新计算）。
  - 首次实现可允许「手动刷新」按钮，自动实时更新标记为优化项。

### 2. UI

- Navigator 左栏「收藏夹」分组下方新增「智能收藏夹」子分组。
- 右键「新建智能收藏夹」：弹出对话框，规则构建器（下拉框选字段/运算符/值，可添加多行规则，选 ALL/ANY）。
- 规则构建器预设：「5 星照片」「未关键字」「最近 30 天」「A7C II 照片」。
- 点击 Smart Collection，Filmstrip 显示匹配成员。

### 3. 测试

- 创建规则「rating >= 4」，给一张照片从 3 星改为 4 星，验证自动出现在集合中。
- 创建复合规则「rating >= 4 AND keyword contains 风光」，验证交集正确。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。谓词规则以 JSON 纯数据传递。
2. **线程安全**：Smart Collection 的重新计算在后台线程完成，不阻塞主 UI。
3. **数据库事务**：Smart Collection 刷新使用 SQLite 事务保证原子性。
4. **测试覆盖**：新增 Catch2 测试用例，覆盖规则解析/匹配/成员增减/复合规则。

## 验收标准

1. 创建「5 星照片」规则，Filmstrip 显示全部 5 星照片。
2. 将一张 3 星照片改为 4 星（或刷新），自动出现在「rating >= 4」集合中。
3. 创建复合规则「rating >= 4 AND keyword contains 风光」，验证交集正确。
4. 修改/删除规则，成员实时或延迟刷新正确。
5. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- 规则的实时增量监听（每次图库变更自动刷新）：首次可接受「打开集合时计算」或「手动刷新」按钮。

## 参考

- 现有 `Collection` / `Folder` 的 Navigator 分组逻辑。
- Lightroom Classic 行为：Smart Collection 规则与 LRC 的「所有/任意」一致。

## 实现提示

1. **实时性取舍**：首次实现可接受「打开集合时计算」而非「每次图库变更实时增量更新」。在 `NavigatorWidget` 点击 Smart Collection 时执行查询，避免监听所有照片变更的复杂性。
2. **谓词求值**：用 `utils/Json` 解析 `predicate_json`，实现字段取值 + 运算符分派的谓词求值器，便于单测。
