# M3 里程碑状态 — XMP 兼容 / Lightroom 预设互导（v2.2）

> 本文件由协调者实时维护：每个子代理完成、每个阶段收口后立即更新。
> 目标计划见 [MILESTONE-PLAN.md](MILESTONE-PLAN.md) §4。仓库非 git，按「一文件一写者」切分并行。

## 验收标准（计划 §4.2）— 全部完成（含真机验收确认，2026-08-11）

- [x] 读取标准 Lightroom XMP sidecar（`rdf:RDF` + `crs:` 命名空间），映射为 `Adjustments` 并应用到当前照片（`XmpDocument::parse` + `XmpProfile::fromCrsAttrs` + 照片菜单「读取 sidecar」）。
- [x] 把当前调整导出为 `.xmp` 预设（crs: 属性），可在 Lightroom 中识别（结构合法、字段名/比例正确；golden 结构测试 + 往返测试覆盖；**真机 LR 打开验证待用户**）。
- [x] 导入 `.xmp` 预设对话框 + 「存储为预设」（文件菜单「导入/导出预设…」，`.xmp`/`.lrp` 双格式；sidecar 与目录 edits 双向同步经 `saveAdjustments` 落库）。
- [x] 对未知/缺失 crs: 字段容错（跳过，不报错；`TestXmpProfile` 覆盖）；数值比例与 Lightroom 约定一致（Exposure2012 档、Contrast2012 -100..+100、Temperature mired、Tint ×1.5 等）。
- [x] 测试：映射往返、sidecar 读写、golden `.xmp` 样例、未知字段容错（`TestXmpProfile`/`TestXmpSidecar`/`TestXmpDocument` 全绿）。

## 任务状态

| 任务 | 内容 | 状态 | 归属文件 | 代理 |
|------|------|------|---------|------|
| M3.0 | 设计门：`docs/XMP-MAPPING.md` 映射表 + pugixml 选型 + 接口契约 | ✅ 已完成 | `docs/XMP-MAPPING.md` | 协调者 |
| M3.1 | XmpDocument 解析器 + pugixml 集成 | ✅ 已完成 | `src/engine/Xmp.h/.cpp`、`third_party/pugixml/`、`tests/TestXmpDocument.cpp` | A |
| M3.2 | XmpProfile 映射层（crs ↔ Adjustments） | ✅ 已完成 | `src/utils/XmpProfile.h/.cpp`、`tests/TestXmpProfile.cpp` | B |
| M3.3 | sidecar I/O | ✅ 已完成 | `src/engine/XmpSidecar.h/.cpp`、`tests/TestXmpSidecar.cpp` | C |
| M3.4 | UI 菜单与对话框 | ✅ 已完成 | `src/ui/MainWindow.h/.cpp`（4 槽，无新对话框） | D |
| M3.5 | 收口：全量测试 + TestProfileIO 补强 + 文档版本同步 | ✅ 已完成 | `tests/TestProfileIO.cpp`、`STATUS.md`、`README.md`、`CLAUDE.md`、`Constants.h` | E |

## 接口契约（Phase 1 两路并行共同遵守，接口先行约定）

### 设计总则（协调者定案）
- **XML 选型**：vendored **pugixml v1.14**（`third_party/pugixml/pugixml.{hpp,cpp}`，已通过 ghfast.top 代理下载就位，MIT）。避免自写 XML 的边界坑。
- **解耦关键**：映射层 **不依赖** XML 解析器。XmpProfile 只操作
  `std::map<std::string,std::string>`（crs 属性名→字符串值）；XmpDocument 负责
  map ↔ RDF/XML 文本的双向转换。A/B 可零耦合并行，B 用独立 harness 验证。

### XmpDocument（代理 A 实现，B/C/D 不依赖）
- `src/engine/Xmp.h`（Qt-free，`#pragma once`）：
  - `class XmpDocument`：`static bool parse(const std::string& xmlText, std::map<std::string,std::string>& outCrsAttrs)`；
    `static std::string serialize(const std::map<std::string,std::string>& crsAttrs, const std::string& rdfAbout = "")`。
  - 仅收集 **crs 命名空间**（`http://ns.adobe.com/camera-raw-settings/1.0/`）的属性，属性名**不带前缀**（如 `Exposure2012`）。
  - 容错：畸形 XML 返回 false；无 crs 属性返回 false；未知命名空间属性忽略。
  - 也可暴露 `XmpSidecar` 所需：内嵌 XMP 提取由 C 处理（libraw `xmpdata` 已是文本）。

### XmpProfile（代理 B 实现，C/D 依赖）
- `src/engine/XmpProfile.h`（Qt-free）：`static std::map<std::string,std::string> toCrsAttrs(const Adjustments&)`；
  `static Adjustments fromCrsAttrs(const std::map<std::string,std::string>&)`。
- 逐字段映射、比例、钳制、容错规则**全部遵循 `docs/XMP-MAPPING.md`**（见下）。

### XmpSidecar（代理 C 实现，D 依赖）
- `src/engine/XmpSidecar.h`：`static bool readFromSidecar(const std::filesystem::path& imagePath, Adjustments& out)`；
  `static bool writeToSidecar(const std::filesystem::path& imagePath, const Adjustments&)`；
  `static std::filesystem::path sidecarPathFor(const std::filesystem::path& imagePath)`（换扩展名为 `.xmp`）。
- 内嵌 RAW XMP 读取：v1 列为可选项，若实现放 `readFromEmbedded()`（读 libraw `xmpdata`），否则文档标注 deferred。

### UI 动作（代理 D 实现）
- 文件 → 导入预设…（`.xmp`/`.lrp`）/ 导出预设…；照片 → 读取 sidecar / 写入 sidecar。
- 应用预设后走既有 `saveAdjustments` + 重渲染链路。

## 构建 / 测试状态

| 项 | 状态 | 结果 |
|----|------|------|
| 基线 headless `lrc_tests.exe`（M3 开工前） | ✅ | **139 用例 / 1658 断言全绿** |
| pugixml v1.14 vendored（ghfast.top 代理） | ✅ | `third_party/pugixml/pugixml.{hpp,cpp}`（54KB/347KB，MIT） |
| headless 构建（含全部 XMP 层 + 4 个新测试文件） | ✅ | **170 用例 / 1931 断言全绿**（协调者复核） |
| GUI 构建 + windeployqt | ✅ | `build_gui\Release\lrc_lightroom.exe`（3.4MB）+ `platforms/qwindows.dll`，仅既有 C4819/C4996 告警 |

## 日志

- 2026-08-11：开工 M3，创建本文档。确认现状：`ProfileIO` 为扁平 camelCase JSON（`version:5` 未校验）；`Adjustments` 字段域已掌握；`src/` 无任何 XMP/EXIF 代码；libraw `xmpdata` 未读取。**XML 选型定案：vendored pugixml v1.14**（直连 raw.githubusercontent.com 被墙，ghfast.top 代理 HTTP 200 下载成功）。**架构定案：XmpProfile 只依赖 `std::map<string,string>`，与 XML 解析器完全解耦** → M3.1/M3.2 可零耦合并行，B 用独立 harness（Adjustments.cpp+MathUtils.cpp+XmpProfile.cpp）验证，不碰共享 `build_headless`。
- 2026-08-11：基线复核 —— `build_headless\tests\Release\lrc_tests.exe` **139 用例 / 1658 断言全绿**（与 M2 收口一致）。`Adjustments.cpp` 依赖仅 `Adjustments.h` + `MathUtils.h` + `<bit>/<cstring>`，独立 harness 可行。
- 2026-08-11：**M3.0 设计门完成**。产出 `docs/XMP-MAPPING.md`（逐字段映射表 + mired 温度公式 §6.1 + tint×1.5 §6.2 + ToneCurvePV2012 点数组算法 §4 + golden 样例 §8 + 测试契约）。**XML 选型落地**：pugixml v1.14 vendored（`third_party/pugixml/`，已下载）。**共享文件预接线**（协调者独占写入，避免并行冲突）：契约头文件 + stub 实现 `src/engine/Xmp.h/.cpp`、`src/utils/XmpProfile.h/.cpp`、`src/engine/XmpSidecar.h/.cpp`、`tests/TestXmp{Document,Profile,Sidecar}.cpp`；CMakeLists 预登记 pugixml lib + 3 个新引擎/工具源 + 3 个新测试。
- 2026-08-11：**Phase 1 两路并行派发**。代理 A ← M3.1（`XmpDocument` + pugixml，**独占 `build_headless` 构建**，自验 + 补 `TestXmpDocument.cpp`）；代理 B ← M3.2（`XmpProfile` 映射层，**用独立 harness `~/.claude/jobs/ba0734c7/tmp/xmp_profile_harness` 验证，不碰共享构建**，补 `TestXmpProfile.cpp`）。接口契约：XmpProfile 只依赖 `std::map<string,string>`。
- 2026-08-11：**代理 A 完成（M3.1）**。`Xmp.cpp`：pugixml 实现 `parse`（递归整树遍历，local name `Description` 的节点收 `crs:` 前缀属性去前缀入库；畸形 XML / 无 crs 属性均返回 false）+ `serialize`（`x:xmpmeta>rdf:RDF>rdf:Description` + `xmlns:crs` + 可选 `rdf:about`，`StringWriter` 收集缩进输出）。`Xmp.h` 未改（契约签名原样）。**补 `third_party/pugixml/pugiconfig.hpp`**（pugixml.hpp 内 include 它，缺则 C1083；标准 v1.14 默认内容，无行为改变）。`TestXmpDocument.cpp` 6 用例 / 40 断言（golden 样例、往返、畸形 false、无 crs false、序列化可再解析、多 Description 合并）。headless 构建成功，**145 用例 / 1698 断言全绿**（基线 139/1658 + 6/40）。无新告警（既有 C4819 属 GBK 编码旧账）。**契约偏差**：crs 属性按字面前缀匹配（非命名空间 URI 解析），Lightroom 恒用 `crs:`，符合预期。
- 2026-08-11：**代理 B 完成（M3.2）**。`XmpProfile.cpp`：全量映射 —— 基本/细节/效果/颗粒/镜头/曲线(§4)/HSL 24/色调分离 5；`toCrsAttrs` 用 `std::to_chars` 最短往返格式、仅非默认值、恒写 `ToneCurveName2012`；`fromCrsAttrs` 容错未知键、`strtod` 宽松解析、全字段钳制、曲线经 `zonecurve::fromPoints` 量化、恒等曲线→空。`TestXmpProfile.cpp` 7 用例 / 93 断言（往返、未知键容错、缺省、越界钳制、曲线导入、golden §8.1、导出格式）。harness **92 项检查全过**（独立构建，未碰 `build_headless`）。**解析的歧义**：① 数字格式遵 `to_chars` 最短往返（非 doc 的 %.6g）；② 恒等曲线 = 逐点 y==x（容差 1e-6）；③ PV2012 奇数 token → 空曲线；④ HSL hue 钳 ±100；⑤ **修 harness CMakeLists include 路径 bug**（`${SRC}/..`→`${SRC}`）；⑥ Blue=index 5。`XmpProfile.h` 未改。
- 2026-08-11：**Phase 2 两路并行派发**。代理 C ← M3.3（`XmpSidecar` sidecar 读写，`readFromEmbedded` v1 deferred 返回 false，独占 `build_headless` 并先复核 M3.1+M3.2 集成基线 ≈152 用例 / 1791 断言）；代理 D ← M3.4（MainWindow 菜单：文件→导入/导出预设 `.xmp/.lrp`、照片→读取/写入 sidecar；独占 `build_gui`）。
- 2026-08-11：**代理 C 完成（M3.3）**。基线复核：**152 用例 / 1791 断言全绿**（与预期一致，M3.1+M3.2 集成无问题）。`XmpSidecar.cpp`：`sidecarPathFor` 用 `replace_extension(".xmp")`（目录/基名保留，多扩展名只换最后一个）；`readFromSidecar` 缺失/打开失败/`badbit`/parse 失败均返回 false，**失败路径不触碰 `out`**（唯一成功路径赋值；用 `in.bad()` 而非 `!good()` 避免 EOF 误判）；`writeToSidecar` 二进制截断写入、返回流状态；`readFromEmbedded` 精确注释 `// v1: embedded RAW XMP not read (deferred).` 返回 false。`TestXmpSidecar.cpp` 9 用例 / 42 断言（路径 3 例、往返、缺失/损坏/无 crs 三态 false+out 不动、默认集可写、RAII 临时清理）。最终 headless **161 用例 / 1833 断言全绿**（+9 用例 / +42 断言）。无新告警。
- 2026-08-11：**代理 D 完成（M3.4）**。`MainWindow.h` 新增 4 槽 `importPreset`/`exportPreset`/`readSidecar`/`writeSidecar`；`buildMenus()` 接线：文件→「导入预设…/导出预设…」（转换 DNG 之后、退出分隔线前）、照片→「读取 sidecar/写入 sidecar」（管理收藏夹之后）。`.xmp` 走 `XmpDocument::parse`+`XmpProfile::fromCrsAttrs`、`.lrp` 走 `ProfileIO`；应用链路复用编辑历史恢复模式（`currentAdjustments_`→`setAdjustments`→`requestAdjust`→`saveAdjustments`）；无照片 guard；导出默认名=当前图基名+`.xmp`。用既有 `toStdPath()`（Windows 宽字符）而非 `toStdString`。`build_gui` 构建成功（exit 0），新动作用非弃用 `addAction` 重载，**零新增告警**（仅既有 9 处 C4996）。**注**：D 构建时 `XmpSidecar` 尚为 stub（并行时序），磁盘上现为 C 真实实现，M3.5 重建即正确链接。无 GUI 运行时测试。
- 2026-08-11：**Phase 3 派发（M3.5 收口）**。集成代理 E：① 新建 `tests/TestProfileIO.cpp`（version 键容错、完整 JSON 往返、未知键容错、ColorTarget/Masks 哈希、savePreset/loadPreset 文件往返）并在 `tests/CMakeLists.txt` 登记；若发现 `Adjustments::hash()` 漏 colorTargets/masks 则修复（M3.5 范围）；② 版本号 `Constants.h`+`CMakeLists` → **v2.2.0**；③ headless 全量构建测试 + GUI 重建 + windeployqt；④ 同步 STATUS.md/README.md/CLAUDE.md（版本/功能/测试计数）；⑤ sanity check。
- 2026-08-11：**代理 E 完成（M3.5 收口）**。`TestProfileIO.cpp` 9 用例 / 98 断言（完整 JSON 往返、version 键容错含 `"version":"abc"`、未知键容错、空/默认对象、savePreset/loadPreset 文件往返、ColorTarget/Masks 哈希）。**发现：`Adjustments::hash()` 已含 `masks.size()+m.hash()` 与 `colorTargets.size()+ct.hash()`，无需修复**（补充断言覆盖）。版本号 `Constants.h`+`CMakeLists` → **v2.2.0**。headless 全量 **170 用例 / 1931 断言全绿**（161/1833 + 9/98）；GUI 构建成功链接真实 XmpSidecar，windeployqt 部署（仅 dxcompiler/VCINSTALLDIR 非致命提示）；sanity check 通过（exe 3.4MB + `platforms/qwindows.dll` + 4 槽接线逐行核对）。文档同步：STATUS.md（标题 v2.2 + v2.2 章节 + 测试覆盖 4 行 + M4 进行中 + 已完成表 4 行）、README.md（版本/功能/结构树/18 测试文件）、CLAUDE.md（当前状态 v2.2.0 + 170/1931 + XMP 架构条目），构建命令/编码约定/GBK 段未动。
- 2026-08-11：**协调者最终复核（M3 完成）**。`lrc_tests.exe` **170 用例 / 1931 断言全绿**；`kAppVersion="2.2.0"`；MainWindow 4 槽接线（8 处引用）；STATUS.md 标题 v2.2；新文件 6 个就位。**M3 里程碑代码层完成**。
- 2026-08-11：**真机验收通过（用户确认）**。导出的 `.xmp` 可在 Lightroom 中识别、导入真实预设参数核对无误，sidecar 读写路径符合预期。**M3 里程碑正式关闭**。

## 遗留

- ~~**真机验收 UI**~~：✅ 已完成（2026-08-11 用户确认）—— 导入真实 Lightroom `.xmp` 预设参数核对、导出 `.xmp` 在 Lightroom 中可识别、sidecar 读写路径均验证通过。
- **映射语义**：Temperature 绝对 Kelvin ↔ 相对偏移用固定锚点 6500K mired 线性公式（XMP-MAPPING.md §6.1），与真实 LR「As Shot」基线白平衡存在系统偏差，属 v1 取舍。
- **点颜色 / 蒙版 / 畸变**：v1 不互导 XMP（XMP-MAPPING.md §7），在 `.lrp` JSON 中完整往返。
- **内嵌 RAW XMP 读取**：`XmpSidecar::readFromEmbedded` 返回 false（deferred，注释标注），后续版本可接 libraw `xmpdata`。
- **版本号已同步**：v2.2.0（Constants.h / CMakeLists / STATUS / README / CLAUDE 已一致）。
