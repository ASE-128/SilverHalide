# 四阶段加速实施计划（归档）

> 来源： [.trae/documents/speedup_four_stages_plan.md](../../.trae/documents/speedup_four_stages_plan.md)
> 状态：APPROVED（Critic 第 1 轮通过，保留意见已吸收）
> 归档日期：2026-08-31
>
> 本文档保留了最初的实现计划与验收标准，用于对照最终的执行结果和历史变更。

## 1. Requirements summary

用户要一份按最终用户操作路径（Import / Preview / Edit / Export）组织的性能加速方向 + 方法规划。它不是对现有 `docs/plans/perf-m1-m5-plan.md` 的重复，而是把 M 批次里的细项重新串入四条业务路径，补足每条路径的 Key Bottleneck、优先级、验收指标，并补上此前没覆盖的加速方向（批量导入并行解码、预览阶段细粒度复用、编辑端渲染合并、导出流水线化等）。

## 2. Acceptance criteria

- AC-1（导入）：200 RAW 导入批量化，首屏可点击时间 ≤ 基线 50%；UI 不阻塞主线程。
- AC-2（预览切换）：若有 Standard preview，first paint ≤ 25ms；若无，coarse 480p 首帧 ≤ 120ms + 1920 refine ≤ 350ms。
- AC-3（编辑滑块）：拖动 exposure/contrast/HSL 等全局滑块单帧 1920p ≤ 20ms。
- AC-4（导出）：单张 60MP RAW 的端到端导出 ≤ 基线 70%；批导出 20 张总耗时 ≤ 单张 × 20 × 0.8。
- AC-5（回归）：`ctest --test-dir build_headless --output-on-failure` 全通过；GUI 层不引入崩溃。

## 3. RALPLAN-DR summary

### 通用原则

1. 最小代码 + 外科手术式改动
2. 用户感知优先于平均峰值
3. 每条路径 ≥ 2 个 viable options
4. 跨模块缝合点优先
5. 全部改动可测

### 路径 1：导入

- Option A：批导入流水线 = 复制/DNG 转并行 + 解码+缩略 N-线程 + 顺序入库
- Option B：按文件粒度整体并行 + DB 乐观重试
- 推荐：Option A

### 路径 2：预览

- Option A：Standard preview 强绑定 + 增量缓存 Stage 复用 + 粗/精帧共享
- Option B：多图片 Decode/Working buffer 预取池
- 推荐：Option A

### 路径 3：编辑

- Option A：全局滑块走 Stage 分组 hash 细粒度复用 + 空间卷积 scratch 池 + 蒙版 weight 缓存
- Option B：coarse 帧走 SIMD 友好的近似实现
- 推荐：Option A

### 路径 4：导出

- Option A：renderFullDisplay + 编码并行 + 批导出双流水线
- Option B：导出走半尺寸预览代理 + 全分辨率按需低优先级队列
- 推荐：Option A

## 4. Implementation steps

1. 记录基线运行时间与 ctest 结果。
2. 改造 ImportWorker 的三阶段流水线。
3. 增强 PreviewWorker 的 Standard preview 优先分支。
4. 优化 Pipeline 的增量复用和 RenderScratch。
5. 改造导出链路和批导出。
6. 运行 headless + GUI 回归验证。

## 5. Risks & mitigations

- ImportWorker 三阶段队列 backpressure 卡死：使用 bounded queue + drop/回退。
- Standard preview 旧 blob 兼容：未知版本返回 nullopt 并回到 raw decode 路径。
- RenderScratch thread_local 池嵌套时膨胀：对 `Parallel.h` 的嵌套策略做保护。
- Pipeline hash 对不上：强制更新 Hash 与测试断言。
- 导出超内存：队列限流和错误聚合。

## 6. Verification steps

```bat
. "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build build_headless -j
ctest --test-dir build_headless --output-on-failure
```

## 7. 结果结论

该计划最终已被落地为：
- AI 模型分发与 UI 管理
- Standard Preview / Pipeline 增量缓存 / 导入导出通路优化
- 四阶段加速报告已归档于 [Speedup_Four_Stages_Report.md](../Speedup_Four_Stages_Report.md)

相关最终实现状态见：
- [AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md](../AI_MODEL_DISTRIBUTION_STATUS_2026-08-31.md)
- [Speedup_Four_Stages_Report.md](../Speedup_Four_Stages_Report.md)
