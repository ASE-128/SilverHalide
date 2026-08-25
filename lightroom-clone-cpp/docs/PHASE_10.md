# Goal: 实现全景合成（Panorama Merge）

> 原 `docs/archive/PHASE_3.md` 中 §5 独立成 Goal。独立大功能，可与图库线其他 Phase 并行
> （PHASE_6/8/9 之外）；输出路径依赖现有 `CatalogDatabase::addPhoto()` 与 DNG/TIFF 写入能力。

## 前置依赖

- 无 Phase 级硬依赖。依赖现有 `RawDecoder` / `ImageBuffer`（ProPhoto RGB F32）、
  `ImageIO`（TIFF/DNG 写入）、`catalog/CatalogDatabase::addPhoto()`、`utils/Parallel` 线程池。
- 需评估 OpenCV 依赖（项目当前 `LRC_USE_OPENCV=OFF`）。

## 目标

将多张水平/垂直重叠的照片拼接为全景图，输出 DNG 并入目录。

### 1. 工作流程

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

### 2. 架构约束

- 全景合成是**破坏性输出**（生成新文件），但合成过程本身不修改源照片。
- 合成在**后台线程**执行，对话框显示进度条，UI 不阻塞。
- 若依赖 OpenCV，需评估是否引入 vcpkg 依赖。若保持纯 C++ 自研，可使用：
  - 降采样图像的 **互相关（Cross-correlation）** 找重叠偏移。
  - 简单 **柱面投影**：`x' = f × arctan((x - cx) / f) + cx`，`y' = f × (y - cy) / sqrt((x-cx)² + f²) + cy`，其中 `f` 为焦距估算值。
  - **羽化融合**：重叠区按距中心距离线性加权。

### 3. UI

- 合成对话框：Qt 模态对话框，显示进度条、取消按钮。
- 合成完成后，新照片自动选中并载入预览。

## 架构约束（必须遵守）

1. **engine/ 目录禁止包含任何 Qt 头文件**。拼接算法在 engine 层，UI 在 ui/ 层。
2. **非破坏性原则**：全景合成生成新文件，但保留源文件不变。
3. **线程安全**：全景合成在后台线程执行，不阻塞主 UI。
4. **数据库事务**：合成结果入目录与 `panorama_sources` 写入使用 SQLite 事务。
5. **测试覆盖**：新增 Catch2 测试用例，覆盖两张图拼接后的输出尺寸/重叠区无接缝（视觉验证或像素差异阈值）。
6. **性能**：全景合成（5 张 20MP）后台完成时间 < 30 秒（ThinkPad X390 级硬件）。

## 验收标准

1. 多选 3 张水平重叠的风景照，执行「全景合成…」。
2. 合成过程中进度条推进，UI 可操作，不卡死。
3. 合成完成生成 DNG（或 TIFF）文件，自动入目录并选中载入预览。
4. 拼接结果重叠区无明显接缝，透视畸变可接受。
5. `panorama_sources` 表记录正确。
6. `ctest --test-dir build --output-on-failure` 全绿（原有 272 用例不挂 + 新增测试通过）。

## 排除项（不在本次范围）

- **自动曝光融合**：要求输入照片曝光一致，不处理曝光差异混合（HDR 全景后续迭代）。
- **多频段融合**：首次可用羽化加权平均，多频段标记为 TODO。
- **非圆柱/非透视投影**：首次支持圆柱或透视之一，球面标记为 TODO。

## 参考

- 现有 `CatalogDatabase::addPhoto()`、`ImageIO`（DNG/TIFF 写入）、`ImageBuffer` 缩放/裁剪。
- 现有 HDR 合成（v1.4）的融合实现可参考。
- Lightroom Classic 行为：全景合成输出 DNG。

## 实现提示

1. **算法选型**：若项目已依赖 OpenCV（或可通过 vcpkg 引入），优先使用 OpenCV Stitcher 管线（`cv::Stitcher`），可大幅缩短开发周期。若必须纯自研，先用相位相关做平移对齐 + 柱面投影 + 羽化融合，覆盖 90% 风光接片场景。
2. **降低分辨率加速**：对齐阶段用降采样图，最后仅在输出阶段用全分辨率拼接。
