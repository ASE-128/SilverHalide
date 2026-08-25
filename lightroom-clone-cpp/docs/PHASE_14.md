# Goal: 实现重复检测（Duplicate Detection）

> 原 `docs/archive/PHASE_4.md` 中 §3 独立成 Goal。依赖 PHASE_11 推理基础设施做深度特征
> 确认；pHash 预筛选本身为纯算法。

## 前置依赖

- **PHASE_11**：ONNX Runtime + 模型下载（MobileNetV3 特征提取模型）。
- 现有 `previews` 表的 320px thumbnail（pHash 输入源）。

## 目标

自动发现图库中视觉重复或高度相似的照片，支持自动堆叠。

### 1. 算法策略：双阶段检测

- **阶段 1：感知哈希（pHash）快速预筛选**（纯算法，无 AI）：
  - 对每张图片的 thumbnail（320px）计算 64-bit pHash（DCT 低频）。
  - 汉明距离 < 10 视为候选重复组。
- **阶段 2：深度特征精确确认**（AI）：
  - 使用轻量 CNN（如 **MobileNetV3-Small** 或 **ResNet18**）的 ONNX 导出，提取 512-dim 或 128-dim 嵌入向量。
  - 对阶段 1 的候选组计算余弦相似度，> 0.92 确认为重复。
  - 模型输入：224×224 RGB，输出：特征向量。

### 2. 工作流程

- **手动扫描**：图库菜单「查找重复照片…」：
  - 对话框：选择扫描范围（整个目录/所选文件夹），相似度阈值。
  - 后台线程遍历所有照片，计算 pHash 与深度特征（首次计算后缓存到数据库）。
  - 结果对话框：分组显示重复照片（如连拍 3 张几乎相同），用户可选择「堆叠」「删除副本」「取消」。
- **自动堆叠**：可选勾选「自动堆叠重复照片」，将确认重复的照片在 Filmstrip 中折叠为一组（与 LRC 的 Stack 行为一致）。
- **数据库缓存**：
  - 新增 `photo_features` 表：`photo_id`, `phash` (TEXT/INT64), `embedding` (BLOB, float32 数组), `updated_at`。
  - 照片导入时自动计算并缓存。
  - 照片编辑后（如大幅裁剪/调色）pHash 可能变化，可设置「仅对未编辑的 RAW 启用检测」或定期重新计算。

### 3. UI

- `NavigatorWidget` 或 `MainWindow` 菜单：「图库 → 查找重复照片」。
- 结果对话框：左右对比显示候选重复组，标记最佳保留项（如最高评分、最大文件）。
- Filmstrip 中重复堆叠显示角标数字（如 "3" 表示堆叠 3 张）。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。`DuplicateDetector` 为纯 C++（pHash + 深度特征）。
2. **非破坏性原则**：检测不修改任何照片文件；「删除副本」需用户确认。
3. **线程安全**：扫描与推理在后台线程执行，主 UI 不阻塞。
4. **测试覆盖**：构造 3 组已知重复/非重复测试图，验证召回率 > 95%、误报率 < 5%。
5. **性能**：首次扫描结果持久化到 `photo_features`，后续增量更新仅处理新增/修改照片。

## 文件清单（需修改/新增）

### 修改

- `src/catalog/CatalogDatabase.h` / `.cpp` — 新增 `photo_features` 表（pHash/embedding）。
- `src/ui/MainWindow.h` / `.cpp` — 新增「查找重复」菜单。

### 新增

- `src/app/DuplicateDetector.h` / `.cpp` — pHash + 深度特征重复检测引擎（纯 C++）。
- `src/ui/DuplicateResultsDialog.h` / `.cpp` — 重复照片结果展示与操作对话框。
- `tests/TestDuplicateDetection.cpp` — 重复检测准确率验证。

## 验收标准

1. 图库中导入 10 张连拍几乎相同的照片 + 10 张完全不同的照片。
2. 执行「查找重复照片」，正确识别出连拍组为重复，其他照片无误报。
3. 选择「自动堆叠」，Filmstrip 中连拍组折叠为 1 个堆叠，角标显示数量。
4. 展开堆叠可查看成员，删除其中一张后堆叠自动更新。
5. 首次扫描后，重启应用再次扫描走缓存，不重复计算特征。
6. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- 视频相似性检测：仅静态图像。

## 参考

- Lightroom Classic 行为：重复检测后按 Stack 折叠，角标显示数量。

## 实现提示

1. **embedding 缓存**：首次扫描 10000 张照片可能耗时数分钟，结果必须持久化到 `photo_features` 表，后续增量更新仅处理新增/修改照片。
2. **两阶段顺序**：先用 pHash 粗筛缩小候选集（避免对全部照片做 CNN 推理），再对候选组做深度特征确认，平衡准确率与耗时。
