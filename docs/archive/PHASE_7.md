# Goal: 实现前后对比视图（Before/After）与参考视图（Reference View）

> 原 `docs/archive/PHASE_3.md` 中 §2 独立成 Goal。Before/After 两路渲染的性能
> （分屏延迟增加 < 50ms）依赖增量渲染缓存，故排在地基（PHASE_2）之后。

## 前置依赖

- **PHASE_2**：增量渲染缓存。Before 与 After 的差异仅在于 Stage A 参数时，
  增量缓存可极大加速（原 PHASE_3 明确引用"Phase 1.5"即本依赖）。
- 现有 `PreviewWorker` 后台线程与 LRU 缓存机制。

## 目标

### 1. Before/After 对比

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

### 2. 参考视图（Reference View）

- **概念**：在中央预览区同时显示两张照片：左侧为参考图（可来自图库任意照片），右侧为当前编辑照片，方便风格匹配。
- **UI 实现**：
  - 照片菜单/工具栏「参考视图」：点击后弹出迷你 Filmstrip 或 Navigator，用户选择参考照片。
  - `ImageViewport` 左右分屏，左侧加载参考照片的渲染结果（应用其当前参数堆叠），右侧加载当前照片。
  - 参考照片不可编辑（只读），但可缩放/平移独立（或锁定与当前照片同步缩放）。
- **渲染路径**：
  - 参考照片使用独立的 `PreviewWorker` 请求（或复用主 `PreviewWorker` 的线程池），从 `CatalogDatabase` 读取其参数堆叠与 Standard Preview，渲染为与当前视图同尺寸的 QImage。
  - 参考视图不触发数据库写入。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。两路渲染参数通过纯数据结构传递。
2. **非破坏性原则**：Before/After、参考视图均不修改 RAW 文件。
3. **线程安全**：Before 路渲染在后台线程完成，不阻塞主 UI。
4. **测试覆盖**：新增 Catch2 测试用例，覆盖 Before/After 零调整状态渲染与当前状态像素差异。
5. **性能**：Before/After 分屏渲染延迟增加 < 50ms。

## 验收标准

1. 对一张已应用曝光 -1.0 的照片，按 `\` 键切换 Before/After，Before 显示原始曝光，After 显示调整后。
2. 左右分屏模式下，中央分割线可拖拽，两路画面清晰。
3. 单图切换模式（`\`）瞬间切换，无闪烁。
4. 参考视图选择图库另一张照片，左侧显示参考图，右侧显示当前照片，可独立缩放。
5. Before/After 同时渲染时，操作流畅度与单路渲染差异不明显（< 50ms）。
6. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- 编辑历史快照时间轴的 Before/After 对比（本次仅「零调整」或复位快照）。
- 参考视图与当前照片联动编辑。

## 参考

- 现有 `PreviewWorker` 的并发渲染机制。
- PHASE_2 的增量渲染缓存与 Standard Preview。

## 实现提示

1. **Before 路无需重新解码 RAW**：直接从 Standard Preview + 默认 `Adjustments()` 渲染。若 `Pipeline` 的增量渲染已实现，Before 与 After 的差异仅在于 Stage A 参数，增量缓存可极大加速。
2. **双 ViewportPane**：Before/After 模式内部分为两个 `ViewportPane`，各自持有独立的 `QImage`，共享缩放状态（或独立）。
