# 现代 C++ 改造 · T0 基线快照

- 记录日期：2026-10-19
- 分支 / 提交：`main` @ `1d6a624`（工作区另有无关改动，见 §1）
- 构建树：`build_headless`（Ninja + MSVC 19.44.35229.0，Release，`LRC_ENABLE_QT=OFF`，`LRC_ENABLE_SIMD=OFF`）
- 用途：为 [modern-cpp-adoption-audit.md](modern-cpp-adoption-audit.md) 的 IM-1 ~ IM-11 提供可复现的「改造前」数字与逐位一致 oracle。

## 1. 工作区隔离状态

T0 开始时 `git status --short --branch`：

```
## main...SilverHalide/main
 M packaging/installer.iss
?? .trae-html-share-packages/
?? architecture.html
?? docs/spec/modern-cpp-adoption-audit.md
?? ppt_media_raw/
?? promo_ppt/
```

`packaging/installer.iss` 与上述未跟踪目录/文件与本轮改造无关，全程不得修改、暂存或纳入交接 diff；所有构建产物写入 `build_*`（已被 `.gitignore` 的 `build_*/` 覆盖）。

## 2. 构建与全量测试基线

```bat
cmake -S . -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
cmake --build build_headless -j 14
build_headless\tests\lrc_tests.exe
```

| 项 | 基线值 |
| --- | --- |
| 全量用例 | **482 test cases / 878,877 assertions，全绿** |
| 全量耗时 | 8.6 s |
| `[pipeline]` | 65 test cases / 2,675 assertions，全绿 |
| `[incremental]` | 8 test cases / 375,061 assertions，全绿 |
| `[performance]` | 4 test cases / 24 assertions，全绿 |

> 注：审计文档写的是 484 用例，实测为 482（审计统计口径含标签重复计数）。后续任务以 482 为回归基线。

## 3. TestPerfAnchors 基线（3 次连跑，取中位数）

命令：`build_headless\tests\lrc_tests.exe "[performance]" -s`

| 锚点 | 第 1 次 | 第 2 次 | 第 3 次 | 基线取值（中位） |
| --- | --- | --- | --- | --- |
| B-2 1920 refine 单帧 min | 90.34 ms | 94.23 ms | 93.40 ms | **93.40 ms**（avg 95.14 / 96.82 ms） |
| B-1 50MP 全尺寸 engine leg | 551.36 ms | 697.74 ms | 686.51 ms | **686.51 ms** |
| D-1 50MP 全尺寸渲染 | 606.62 ms | 687.29 ms | 700.45 ms | **687.29 ms** |
| D-2 峰值工作集回落量 | 688.19 MB | 687.94 MB | 688.06 MB | **688.06 MB** |
| D-1 作用域结束 commit | 74.68 MB | 74.70 MB | 74.75 MB | **74.70 MB** |

> B-1/D-1 首次运行偏快（约 550-600 ms）是首次调用时的缓存/频率状态差异，属已知抖动；比较改造前后必须固定迭代次数与顺序，并用同一台中位数口径。B-2 抖动约 ±5%。

## 4. 逐位一致 oracle（改造前后必须逐行相同）

由于 `TestPipeline` / `test_incremental_pipeline` 是容差型断言，仅靠它们不足以证明「未改变浮点运算顺序」。T0 额外建立了一个只依赖公开引擎 API 的 hash oracle：

- 源码：[modern-cpp-bitexact-harness.cpp](modern-cpp-bitexact-harness.cpp)（编译产物与基线输出在 `build_headless/_bitexact/`）
- 编译（需 x64 MSVC 环境）：

```bat
cl /nologo /std:c++20 /O2 /EHsc /MD -Isrc -Ithird_party -Ithird_party/libraw ^
   build_headless\_bitexact\bitexact.cpp /Fo:build_headless\_bitexact\ ^
   /Fe:build_headless\_bitexact\bitexact.exe ^
   /link build_headless\lrc_engine.lib build_headless\sqlite3.lib ^
         build_headless\sqlite_vec.lib build_headless\third_party\lcms2\lcms2.lib ^
         build_headless\third_party\libraw\raw.lib ws2_32.lib user32.lib advapi32.lib shell32.lib ole32.lib
```

基线输出（`build_headless/_bitexact/golden_baseline.txt`）：

```
ib.src=bbdf38a76d344aaa
ib.resizeToF32=e9cf978b66c356b2
ib.resize=e9cf978b66c356b2
ib.toU16=227b310078019674
ib.crop=fcfc596d24511708
ib.setPixel=d15191590ac49344
ib.toF32fromU8=4c9cfa4548357a24
cm.toDisplaySRGB=fdd35724b46c9825
cm.linearToSRGB=b5552a6ffa6d9106
cm.srgbPixelToLinear=d0c15df1c928c9fa
pl.preview.a=54a1b3e9957a5d08
pl.preview.b=824922ae80242827
pl.preview.b.hit=824922ae80242827
pl.preview.a2=54a1b3e9957a5d08
pl.preview.a2.linear=bef3acaffa4c5fd1
pl.fullDisplay=8846129ac11456e5
pl.histogram=7764bd71f576b94a
hdr.estimateEv*1e6=00000000000e5d95
hdr.merge.band64=e8ddb763f4c2f251
hdr.merge.band4096=e8ddb763f4c2f251
hdr.merge.deghost=178ccfd267ff2304
par.fill=9103d9d978063a61
json.compact=313c9b087069bbe9
json.pretty=a28993cce9bbbd2b
json.roundtrip=313c9b087069bbe9
profile.serialize=d7361584293766ff
profile.roundtrip=d7361584293766ff
```

覆盖范围与对应任务：

| 前缀 | 覆盖 | 相关任务 |
| --- | --- | --- |
| `ib.*` | `ImageBuffer` resize/toU16/toF32/crop/setPixel/getPixel 路径 | T1 |
| `cm.*` | `ColorManager` 三条转换路径 | T7 |
| `pl.*` | `renderPreview`（含增量复用与缓存命中）、`renderFullDisplay`、直方图 | T1/T3 |
| `hdr.*` | `HdrMerge` 的 band 不变式与 deghost 输出 | T2 |
| `par.*` | `parallelFor` 分块结果确定性 | T4 |
| `json.*` / `profile.*` | JSON 序列化文本与 ProfileIO 往返 | T5/T6 |

**验收口径**：每项任务收尾必须重新编译该 oracle 并与本文件 §4 逐行 diff；任何一行变化即视为违反「只改所有权与接口、不改浮点运算顺序」红线，必须回退重做。JPEG/PNG 编码不在此 oracle 内（不在本轮改动范围）。

---

## 5. IM-8 SIMD 测量（T8 交付）

测量对象：`src/engine/Pipeline.cpp` 的手工 SSE2 stage A/C 批次（约 1064-1130）与 `LRC_ENABLE_SIMD` 编译开关（默认 OFF；ON 时加 `/arch:AVX2`）。

### 5.1 逐位一致

| 配置 | oracle 27 行 | 说明 |
| --- | --- | --- |
| 默认（SIMD OFF） | **27/27 一致** | 红线「默认 OFF 逐位一致」成立 |
| `LRC_ENABLE_SIMD=ON` | **22/27 一致，5 行 `pl.*` 不同** | `pl.preview.a/.b/.b.hit/.a2/.a2.linear`；`pl.fullDisplay` 与 `cm.*`/`ib.*`/`hdr.*`/`par.*`/`json.*`/`profile.*` 均一致 |
| ON + 旁路手工 SSE2 批次（c1） | 与上表 ON **完全相同** | 证明手工 SSE2 批次本身逐位一致，差异不来自它 |
| ON + 旁路 AVX2 高斯模糊（c2，诊断） | **27/27 回到基线** | 差异唯一来源 = `blurExactPassAVX2` 的 `_mm256_fmadd_ps`（FMA 重结合） |

结论：SIMD=ON 非逐位一致是 **P2-2 的预先存在设计取舍**（代码注释已写明 tolerance-identical，验收 Δ<0.008），不是本轮回归；`/arch:AVX2` 对普通标量循环的自动向量化是逐位安全的。

### 5.2 性能（a/b/c1 交错各 4 样本，cl.exe=0）

| 配置 | B-2 min 中位 | B-1 中位 | D-1 中位 |
| --- | --- | --- | --- |
| a 默认 OFF | 78.47 ms（极差 12.46） | 640.31 ms（极差 215.33） | 719.95 ms（极差 322.52） |
| b SIMD=ON | 75.35 ms（极差 18.74） | 583.39 ms（极差 256.38） | 691.87 ms（极差 210.35） |
| c1 ON + 旁路手工 SSE2 批次 | 70.57 ms（极差 19.29） | 574.65 ms（极差 271.15） | 619.38 ms（极差 313.85） |

所有两两中位差均**小于对应采样极差** → 无可测量差异；最优点组间差 ≤5%。（测量窗口内仍有其他 teammate 进程造成的尖峰，故按极差判定而不是按均值宣称加速。）

### 5.3 决策

1. **手工 SSE2 stage A/C 批次：维持现状**（保留代码，`LRC_ENABLE_SIMD` 默认 OFF）。理由：逐位安全（c1 与 b 哈希完全相同）+ SIMD=ON 下无可测量收益。
2. 若要降维护面，可单独立「维护性删除」任务（删掉那两段 intrinsic，等价 c1 配置）；依据本测量预期无性能变化。**本轮不删**。
3. 附带记录：SIMD=ON 的 AVX2 `gaussianBlur` FMA 是 SIMD 构建非逐位一致的唯一来源，且在本轮三个锚点上同样无可测量收益——后续 SIMD 策略评审可把它列为「FMA 换容差、无收益」候选。
4. `CMakeLists.txt` 未改动，`LRC_ENABLE_SIMD` 仍默认 OFF。

---

## 6. Mask 逐位 oracle（为 IM-6 的 `Mask` variant 化冻结的基线）

第二阶段「独立项」要把 `Mask`（`src/engine/Adjustments.h`）从「五个几何体常驻」改成 `std::variant`。审计要求这种改造不能改变任何可观察输出，但主 oracle（§4）不含 mask，因此单独冻结一份 mask 基线。

- 源码：[modern-cpp-mask-bitexact-harness.cpp](modern-cpp-mask-bitexact-harness.cpp)
- 覆盖：`Pipeline::maskWeight`（64×48 网格，5 种 mask）、`Pipeline::brushWeightField`、`Pipeline::combinedMaskWeight`（含 colour/luminance 子蒙版的乘积与 union 两种模式）、`Mask::hash()` + `Mask::geometryHash()`（缓存键）、`ProfileIO::toJson(...).serialize(true)`（序列化键与顺序）、以及 4 个 mask 叠加的 `renderPreview` 与 AI mask 的渲染/权重回退路径。
- 编译与运行：与 §4 相同的 cl 命令，把源文件换成 `modern-cpp-mask-bitexact-harness.cpp`（链接同一个 `lrc_engine.lib`）。
- 基线值（**改造前后必须逐行相同**；已重复运行确认确定性）：

```
mask.weight.linear=c3a7408b93e53c88
mask.brushfield.linear=3d71c6a4fc6df8e3
mask.combined.linear=c3a7408b93e53c88
mask.hash.linear=59f464c12ee6fc01
mask.profile.linear=2979c6a681970a85
mask.weight.radial=7febb9bd44c09bbd
mask.brushfield.radial=fb67c3ab414af5c4
mask.combined.radial=7febb9bd44c09bbd
mask.hash.radial=177f93f97a74bbc9
mask.profile.radial=b75322c4c849617c
mask.weight.brush=72d0958a04e8b413
mask.brushfield.brush=3af1d687100dfa0a
mask.combined.brush=72d0958a04e8b413
mask.hash.brush=f78de04828cd0e52
mask.profile.brush=5d6d542433145a33
mask.weight.color=4b404aa0a9cf4383
mask.brushfield.color=4b404aa0a9cf4383
mask.combined.color=4b404aa0a9cf4383
mask.hash.color=59d4ed864cb8faca
mask.profile.color=9b52dc7e4b9f2796
mask.weight.submask=32c7b0541d683c82
mask.brushfield.submask=ac8dcb6615f0d651
mask.combined.submask=cc0411fad23119d6
mask.hash.submask=962dd391e385c69f
mask.profile.submask=9185da8886de424c
mask.weight.ai=7aa9af6747dae383
mask.brushfield.ai=7aa9af6747dae383
mask.combined.ai=7aa9af6747dae383
mask.hash.ai=2be53bbe4d2c0ded
mask.profile.ai=1268cbd89453ded3
mask.render.localstack=7aa1410aaeaf7c22
mask.render.ai=ba04b6e7c005d275
mask.ai.weightfield=9312b00567b44a8f
mask.subweights=59f4016d6c20f72a
```

**验收口径**：`Mask` variant 化任务收尾时，除 §4 的 27 行外，本文件的 34 行也必须逐行一致；任何一行变化都说明权重公式、缓存键或序列化格式被意外改变。
