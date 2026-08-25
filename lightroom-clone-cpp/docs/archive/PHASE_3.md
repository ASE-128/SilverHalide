
# Goal: 实现图库效率与专业工作流（Phase 3）

## 当前状态
- **目录管理**：受管库导入、文件夹监视、评分/旗标/标签/筛选、收藏夹集合（手动）、编辑历史快照、XMP sidecar 读写。
- **图库视图**：三栏布局（左导航/中央预览/右面板），底部 Filmstrip，Navigator 支持文件夹/标签/收藏夹过滤。
- **缺失**：
  1. 无**虚拟副本**（Virtual Copies）：同一张 RAW 无法保存多套独立编辑（如彩色版/黑白版/胶片版）。
  2. 无**对比视图**：无法 Before/After 对比原图，也无法加载参考图匹配风格。
  3. 无**关键字与元数据管理**：无法通过关键字检索、无标题/说明/版权字段。
  4. 无**智能收藏夹**（Smart Collections）：无法按规则自动聚合照片。
  5. 无**全景合成**（Panorama Merge）：已有 HDR 合成，缺全景拼接。

## 目标

### 1. 虚拟副本（Virtual Copies）

允许同一张物理 RAW 文件创建多个逻辑副本，各自拥有独立的参数堆叠、编辑历史与快照，不额外占用磁盘空间。

#### 1.1 数据模型
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

#### 1.2 UI 交互
- **Filmstrip**：虚拟副本显示与母版相同缩略图，但角标区分（如页码角标或叠加数字 badge）。
- **Navigator**：左栏「目录图片库」下新增「母版照片」与「所有虚拟副本」分组，或仅在照片列表中平铺显示。
- **切换**：Filmstrip 点击虚拟副本时，中央预览区加载同一张 RAW 但应用该 variant 的参数堆叠。
- **重命名**：右键虚拟副本可重命名（如 "高对比黑白"）。
- **导出**：导出对话框可选择导出母版或任意虚拟副本，行为与单张照片一致。

#### 1.3 XMP 与兼容性
- 写入 XMP sidecar 时，默认写入母版（variant_id=0）的参数。
- 虚拟副本的参数不写入 sidecar（LRC 也不写虚拟副本到 XMP，仅 catalog 内）。
- 导入含虚拟副本的 LRC catalog（若未来支持）预留接口。

#### 1.4 测试
- 创建 3 个虚拟副本，各自独立调整曝光/HSL，验证数据库中各 variant 参数隔离。
- 删除虚拟副本，验证母版与其他副本不受影响。
- 重启应用，验证各 variant 自动恢复。

---

### 2. 前后对比视图（Before/After）与参考视图（Reference View）

#### 2.1 Before/After 对比
- **概念**：
  - **Before**：当前照片的「零调整」状态（所有参数堆叠为默认值，或上一次手动保存的「复位」快照）。
  - **After**：当前参数堆叠的渲染结果。
  - 与「编辑历史快照」不同：Before/After 是视图模式，不创建新数据。
- **UI 实现**：
  - 工具栏/快捷键（`\` 或 `Y`）切换 Before/After 模式。
  - 显示模式：
    - **左右分屏（Left/Right）**：左 Before，右 After，中央可拖拽分割线。
    - **上下分屏（Top/Bottom）**：上 Before，下 After。
    - **单图切换（Single Toggle）**：同一区域按 `\` 键瞬间切换 Before/After（类似 LRC 的 `\` 行为）。
  - 在 `ImageViewport` 中实现：正常模式下 `ImageViewport` 显示单张 QImage；Before/After 模式下内部分为两个 `ViewportPane`，各自持有独立的 `QImage`。
- **渲染路径**：
  - `PreviewWorker` 需要同时生成两路预览：
    - **After 路**：当前 `Adjustments` 正常渲染。
    - **Before 路**：使用「零调整」`Adjustments()`（或用户指定的历史快照点）作为参数，复用 Standard Preview 缓存直接渲染（跳过 RAW 解码）。
  - 两路预览共享同一输入源（Standard Preview / 1:1 Preview），仅 `Pipeline` 参数不同。
  - 若 Before 状态已存在于 `RenderState` 增量缓存中（如用户刚从复位状态调过来），直接复用。
  - 性能要求：Before/After 同时渲染的延迟增加 < 50ms（利用现有缓存与预览分辨率渲染）。

#### 2.2 参考视图（Reference View）
- **概念**：在中央预览区同时显示两张照片：左侧为参考图（可来自图库任意照片），右侧为当前编辑照片，方便风格匹配。
- **UI 实现**：
  - 照片菜单/工具栏「参考视图」：点击后弹出迷你 Filmstrip 或 Navigator，用户选择参考照片。
  - `ImageViewport` 左右分屏，左侧加载参考照片的渲染结果（应用其当前参数堆叠），右侧加载当前照片。
  - 参考照片不可编辑（只读），但可缩放/平移独立（或锁定与当前照片同步缩放）。
- **渲染路径**：
  - 参考照片使用独立的 `PreviewWorker` 请求（或复用主 `PreviewWorker` 的线程池），从 `CatalogDatabase` 读取其参数堆叠与 Standard Preview，渲染为与当前视图同尺寸的 QImage。
  - 参考视图不触发数据库写入。

---

### 3. 关键字与元数据管理

实现照片的关键字标签与 IPTC 基础元数据，支持检索与 XMP 往返。

#### 3.1 数据模型
- 扩展 `catalog/CatalogDatabase`：
  - 新增 `keywords` 表：`id` (PK), `name` (TEXT UNIQUE), `parent_id` (自引用，支持层级关键字如 "风光 > 冰岛")。
  - 新增 `photo_keywords` 关联表：`photo_id`, `keyword_id`。
  - 扩展 `photos` 表（或新增 `metadata` 表）：`title`, `caption`, `copyright`, `creator`, `date_taken` 等基础 IPTC 字段。
- **关键字分配**：
  - 单张照片：右侧面板新增 `Metadata` 面板，含关键字输入框（逗号分隔或标签云）、标题、说明、版权。
  - 批量分配：图库视图多选照片，右键「添加关键字」。
  - 层级关键字：输入 "风光/冰岛/瀑布" 自动创建层级关系（或扁平化存储，UI 显示层级）。

#### 3.2 检索与筛选
- **Navigator 左栏**：新增「关键字列表」分组，按关键字聚合照片数量（如 "风光 (42)"）。点击关键字过滤 Filmstrip。
- **搜索栏**：顶部工具栏新增搜索框，输入关键字实时过滤图库。
- **筛选器**：与现有评分/标签筛选器叠加，支持 "5星 AND 风光 AND 2026"。

#### 3.3 XMP 与导入
- **写入 XMP sidecar**：扩展 `XmpProfile` / `XmpDocument`，写入 `dc:subject`（关键字数组）、`dc:title`、`dc:description`、`dc:rights`、`dc:creator`。
- **读取 XMP sidecar / 导入时**：从 RAW 内嵌 XMP 或 sidecar 提取关键字与元数据，写入数据库。
- **与 LRC 兼容**：LRC 的关键字存储在 `lr:hierarchicalSubject` 和 `dc:subject`，两者均需支持。

#### 3.4 UI
- `DevelopPanel` 或新增 `MetadataPanel`：标题（单行）、说明（多行）、版权（单行）、关键字（TagInput 风格，支持回车添加、点击删除）。
- Filmstrip 缩略图下方可显示标题（可选，设置项控制）。

---

### 4. 智能收藏夹（Smart Collections）

允许用户创建基于规则的动态集合，照片自动进出，无需手动维护。

#### 4.1 数据模型
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

#### 4.2 UI

- Navigator 左栏「收藏夹」分组下方新增「智能收藏夹」子分组。
- 右键「新建智能收藏夹」：弹出对话框，规则构建器（下拉框选字段/运算符/值，可添加多行规则，选 ALL/ANY）。
- 规则构建器预设：「5 星照片」「未关键字」「最近 30 天」「A7C II 照片」。
- 点击 Smart Collection，Filmstrip 显示匹配成员。

#### 4.3 测试

- 创建规则「rating >= 4」，给一张照片从 3 星改为 4 星，验证自动出现在集合中。
- 创建复合规则「rating >= 4 AND keyword contains 风光」，验证交集正确。

---

### 5. 全景合成（Panorama Merge）

将多张水平/垂直重叠的照片拼接为全景图，输出 DNG 并入目录。

#### 5.1 工作流程

- **选择**：在 Filmstrip/Navigator 中多选 2–20 张照片（要求同焦距、同曝光或相近）。
- **触发**：照片菜单「全景合成…」。
- **对话框**：显示缩略图序列、投影模式（球面/圆柱/透视，首次实现可仅支持 **圆柱投影** 或 **透视投影**）、自动裁剪选项。
- **合成**：
  1. 读取每张照片的 Standard Preview 或 1:1 Preview（降低分辨率以加速对齐，可选）。
  2. **特征点匹配**：使用 OpenCV 或自研实现（ORB/SIFT 特征提取 + BFMatcher + RANSAC 估计单应性/仿射矩阵）。若引入 OpenCV 过重，可先用 **相位相关（Phase Correlation）** 或 **金字塔光流** 做平移对齐（假设纯平移/旋转，适合风光接片）。
  3. **变换与拼接**：根据匹配结果计算每张照片在全景画布上的位置。使用透视或圆柱投影将各图像 warp 到统一坐标系。
  4. **融合**：重叠区域使用 **多频段融合（Multi-band Blending）** 或简单 **羽化加权平均**（首次实现可用羽化，多频段标记为 TODO）。
  5. **输出**：生成一张大尺寸的 ProPhoto RGB F32 图像，封装为 **DNG**（嵌入拼接后的 RAW 元数据，或退而求其次输出 16bit TIFF/DNG）。输出文件存入受管库目录（如 `<root>/Panoramas/2026-08-15/`），并自动调用 `CatalogDatabase::addPhoto()` 入目录。
  6. **元数据**：记录源照片 ID 列表（`panorama_sources` 关联表）。

#### 5.2 架构约束

- 全景合成是**破坏性输出**（生成新文件），但合成过程本身不修改源照片。
- 合成在**后台线程**执行，对话框显示进度条，UI 不阻塞。
- 若依赖 OpenCV，需评估是否引入 vcpkg 依赖。若保持纯 C++ 自研，可使用：
  - 降采样图像的 **互相关（Cross-correlation）** 找重叠偏移。
  - 简单 **柱面投影**：`x' = f × arctan((x - cx) / f) + cx`，`y' = f × (y - cy) / sqrt((x-cx)² + f²) + cy`，其中 `f` 为焦距估算值。
  - **羽化融合**：重叠区按距中心距离线性加权。

#### 5.3 UI

- 合成对话框：Qt 模态对话框，显示进度条、取消按钮。
- 合成完成后，新照片自动选中并载入预览。

---

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。所有 UI 逻辑在 ui/ 层，通过纯数据结构传递。
2. **非破坏性原则**：虚拟副本、Before/After、参考视图、关键字均不修改 RAW 文件。全景合成生成新文件，但保留源文件不变。
3. **线程安全**：Smart Collection 的重新计算、全景合成、Before/After 的 Before 路渲染，均在后台线程完成，不阻塞主 UI。
4. **数据库事务**：虚拟副本创建、关键字批量分配、Smart Collection 刷新，使用 SQLite 事务保证原子性。
5. **测试覆盖**：新增 Catch2 测试用例，覆盖：
   - 虚拟副本：创建/删除/参数隔离/重启恢复。
   - Before/After：零调整状态渲染与当前状态像素差异。
   - 关键字：增删改/层级/搜索过滤/XMP 往返。
   - Smart Collection：规则解析/匹配/成员增减/复合规则。
   - 全景合成：两张图拼接后的输出尺寸/重叠区无接缝（视觉验证或像素差异阈值）。
6. **性能**：
   - 虚拟副本切换延迟 < 100ms（复用 Standard Preview，仅参数堆叠切换）。
   - Before/After 分屏渲染延迟增加 < 50ms。
   - 关键字搜索在 10000 张照片库中 < 200ms。
   - 全景合成（5 张 20MP）后台完成时间 < 30 秒（ThinkPad X390 级硬件）。

---

## 排除项（不在本次范围）

- **视频导出/管理**：仅静态图像。
- **联机拍摄（Tethered Capture）**：后续迭代。
- **地理标签（GPS Map）**：元数据仅到关键字/标题/版权层级。
- **人脸识别/AI 自动关键字**：纯手动关键字。
- **全景合成的自动曝光融合**：要求输入照片曝光一致，不处理曝光差异混合（HDR 全景后续迭代）。
- **多频段融合**：首次可用羽化加权平均，多频段标记为 TODO。
- **非圆柱/非透视投影**：首次支持圆柱或透视之一，球面标记为 TODO。

## 参考

- 现有 `CatalogDatabase` 的 `edits` / `history` / `photos` 表结构。
- 现有 `Collection` / `Folder` 的 Navigator 分组逻辑。
- 现有 `PreviewWorker` 的并发渲染机制。
- 现有 `XmpProfile` 的 crs 字段映射，扩展 dc: 命名空间。
- Lightroom Classic 行为：虚拟副本共享物理文件；Before/After 分屏可拖拽；Smart Collection 规则与 LRC 的「所有/任意」一致；全景合成输出 DNG。

## 实现提示

1. **分步推进顺序**：
   ① 虚拟副本（数据库层改动最大，先奠定 variant_id 基础）
   → ② Before/After（利用现有 PreviewWorker，改动 UI 层为主）
   → ③ 关键字管理（数据库 schema + UI 面板，相对独立）
   → ④ 智能收藏夹（依赖关键字与评分数据，最后实现）
   → ⑤ 全景合成（独立大功能，可并行于 ③④）。

2. **variant_id 兼容性**：现有 `edits` 表若无 variant_id 列，迁移策略为：新增列并默认 0，所有现有记录视为母版。确保 `SELECT` 语句默认 `variant_id = 0` 以兼容旧代码路径。

3. **Before/After 性能**：Before 路无需重新解码 RAW，直接从 Standard Preview + 默认 `Adjustments()` 渲染。若 `Pipeline` 已实现增量渲染（Phase 1.5），Before 与 After 的差异仅在于 Stage A 参数，增量缓存可极大加速。

4. **全景合成算法选型**：若项目已依赖 OpenCV（或可通过 vcpkg 引入），优先使用 OpenCV Stitcher 管线（`cv::Stitcher`），可大幅缩短开发周期。若必须纯自研，先用相位相关做平移对齐 + 柱面投影 + 羽化融合，覆盖 90% 风光接片场景。

5. **Smart Collection 实时性**：首次实现可接受「打开集合时计算」而非「每次图库变更实时增量更新」。在 `NavigatorWidget` 点击 Smart Collection 时执行查询，避免监听所有照片变更的复杂性。
