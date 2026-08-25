# XMP 映射表 — Lightroom crs: 属性 ↔ Adjustments（M3.0）

> 本文件是 M3 里程碑的设计门交付物（MILESTONE-PLAN §4.3 M3.0）。
> 定义「Lightroom XMP `crs:` 命名空间属性 ↔ `lrc::Adjustments` 字段」的逐字段双向映射：
> 名称 / 单位 / 比例 / 默认 / 导出与导入规则。**实现（XmpProfile）必须逐条遵循本表。**
>
> - `crs:` 命名空间 URI：`http://ns.adobe.com/camera-raw-settings/1.0/`
> - XMP 载体：`rdf:RDF` 内一个 `rdf:Description` 元素，crs 属性直接作为该元素的 XML 属性。
> - 本工程自定义 JSON 预设（`.lrp`，`ProfileIO`）不受影响，继续作为完整无损格式。

---

## 1. 总体规则（实现必读）

1. **字段名**：crs 属性名不带 `crs:` 前缀，如 `Exposure2012`。
2. **导出**：仅写出**非默认值**字段（默认见映射表「默认」列）。全默认的 Adjustments 导出为空属性集（合法，LR 忽略）。
3. **导入**：缺失字段 = 保持默认；**未知 crs 属性一律跳过，不报错**（含蒙版 `crs:Mask*`、通道曲线 `crs:ToneCurvePV2012Red/Green/Blue`、参数曲线 `crs:Parametric*`、点颜色 `crs:ColorAdjustment*` 等 v1 不处理的字段）。
4. **数值解析**：`std::strtod` 式宽松解析（容忍 `+` 号、前导/尾随空白）；解析失败按默认处理。
5. **数值钳制**：导入值钳制到映射表「范围」列（与 Adjustments 字段既有域一致），防御 LR 预设越界。
6. **数字格式化（导出）**：用 `%.6g` 语义（至多 6 位有效数字、去尾零、整数无小数点），保证往返可解析。正数不加强制 `+`（LR 可读）。
7. **浮点比较**：测试用语义相等（近似容差），不比较字符串逐字节；LR 可能在往返中改写格式。
8. **映射层接口**：`XmpProfile::toCrsAttrs(const Adjustments&) -> std::map<std::string,std::string>`；
   `XmpProfile::fromCrsAttrs(const std::map<std::string,std::string>&) -> Adjustments`。不依赖 XML 解析器。

---

## 2. 基本面板（Basic）

| Adjustments 字段 | 默认 | crs 属性 | LR 量纲 / 范围 | 比例 | 导出条件 |
|-----------------|------|---------|---------------|------|---------|
| `exposure` | 0.0 | `Exposure2012` | 档（stops），约 -5..+5 | 1:1 | ≠ 0 |
| `contrast` | 0.0 | `Contrast2012` | -100..+100 | 1:1 | ≠ 0 |
| `highlights` | 0.0 | `Highlights2012` | -100..+100 | 1:1 | ≠ 0 |
| `shadows` | 0.0 | `Shadows2012` | -100..+100 | 1:1 | ≠ 0 |
| `whites` | 0.0 | `Whites2012` | -100..+100 | 1:1 | ≠ 0 |
| `blacks` | 0.0 | `Blacks2012` | -100..+100 | 1:1 | ≠ 0 |
| `temperature` | 0.0 | `Temperature` | 绝对开尔文 ~2000..50000 | mired 公式，见 §6.1 | ≠ 0（相对偏移） |
| `tint` | 0.0 | `Tint` | -150..+150 | ×1.5，见 §6.2 | ≠ 0 |
| `saturation` | 0.0 | `Saturation` | -100..+100 | 1:1 | ≠ 0 |

## 3. 细节 / 效果 / 颗粒 / 镜头

| Adjustments 字段 | 默认 | crs 属性 | LR 量纲 / 范围 | 比例 | 导出条件 |
|-----------------|------|---------|---------------|------|---------|
| `sharpness` | 0.0 | `SharpenAmount` | 0..150 | 1:1 | ≠ 0 |
| `sharpnessRadius` | 1.0 | `SharpenRadius` | 0.5..3.0 | 1:1 | ≠ 1.0 |
| `sharpnessDetail` | 0.0 | `SharpenDetail` | 0..100 | 1:1 | ≠ 0 |
| `sharpnessMasking` | 0.0 | `SharpenMasking` | 0..100 | 1:1 | ≠ 0 |
| `texture` | 0.0 | `Texture` | -100..+100 | 1:1 | ≠ 0 |
| `clarity` | 0.0 | `Clarity2012` | -100..+100 | 1:1 | ≠ 0 |
| `dehaze` | 0.0 | `Dehaze` | -100..+100 | 1:1 | ≠ 0 |
| `vibrance` | 0.0 | `Vibrance` | -100..+100 | 1:1 | ≠ 0 |
| `vignette` | 0.0 | `PostCropVignetteAmount` | -100..+100 | 1:1（近似语义） | ≠ 0 |
| `distortion` | 0.0 | — | LR 无标准手动畸变 crs 属性 | — | **不互导**（见 §7） |
| `grain` | 0.0 | `GrainAmount` | 0..100 | 1:1 | ≠ 0 |
| `grainSize` | 25.0 | `GrainSize` | 0..100 | 1:1 | ≠ 25.0 |
| `grainRoughness` | 50.0 | `GrainFrequency` | 0..100 | 1:1（仅改名） | ≠ 50.0 |

## 4. 色调曲线（Tone Curve，PV2012 点数组）

| Adjustments 字段 | 默认 | crs 属性 | 说明 |
|-----------------|------|---------|------|
| `curve`（`std::vector<ToneCurvePoint>`，[0,1] 归一化） | 空 = 恒等 | `ToneCurvePV2012` | 0..255 整数坐标点数组，逗号分隔，如 `0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255` |
| — | — | `ToneCurveName2012` | 仅导出：恒等 → `"Linear"`，否则 → `"Custom"`（导入忽略此键） |

**导出算法**：
1. 若 `curve` 为空（恒等）→ 写 `ToneCurveName2012="Linear"`，省略 `ToneCurvePV2012`。
2. 否则对 `curve` 每个点：`x_int = lround(p.x * 255)`，`y_int = lround(p.y * 255)`，钳制到 [0,255]；
   若首点 x ≠ 0 前补 `0, y0`、末点 x ≠ 255 后补 `255, 255`；按 `"x0,y0, x1,y1, …"` 写出，并写 `ToneCurveName2012="Custom"`。

**导入算法**：
1. 解析 `ToneCurvePV2012` 为 (x,y) 对（容忍空格/换行）。点数为 0 或解析失败 → 保持空曲线。
2. 归一化为 [0,1] `ToneCurvePoint` 列表。
3. 若与恒等默认（如 `0,0,32,32,64,64,128,128,192,192,255,255`）等价 → 保持空曲线。
4. 否则 `zonecurve::fromPoints(points)`（量化到四分区锚点，已有工具，复用）赋给 `curve`。

> 说明：本工程曲线是「四分区锚点 + 固定黑白端点」的 6 点表示；导出其自身采样点既满足 LR 点数组合法性，也在本应用内近乎无损往返。恒等曲线导出为 LR 线性默认的写法由 LR 惯例决定。

## 5. HSL 与色调分离

### 5.1 HSL（24 个属性，全量）

`hsl[i]` 为 `HSLChannel{hue, saturation, luminance}`，`ColorRange` 顺序：
**Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta**（与 LR 色相名一致）。

| Adjustments 字段 | 默认 | crs 属性模板 | LR 范围 | 比例 | 导出条件 |
|-----------------|------|-------------|---------|------|---------|
| `hsl[i].hue` | 0.0 | `<Color>Hue`（如 `RedHue`） | -100..+100 | 1:1 | ≠ 0 |
| `hsl[i].saturation` | 0.0 | `<Color>Saturation`（如 `RedSaturation`） | -100..+100 | 1:1 | ≠ 0 |
| `hsl[i].luminance` | 0.0 | `<Color>Luminance`（如 `RedLuminance`） | -100..+100 | 1:1 | ≠ 0 |

即：`RedHue/RedSaturation/RedLuminance, OrangeHue/…, Yellow…, Green…, Aqua…, Blue…, Purple…, Magenta…`。

### 5.2 色调分离（Split Toning）

| Adjustments 字段 | 默认 | crs 属性 | LR 范围 | 比例 | 导出条件 |
|-----------------|------|---------|---------|------|---------|
| `splitTone.highlightHue` | 0.0 | `SplitToningHighlightHue` | 0..360 | 1:1 | ≠ 0 |
| `splitTone.highlightSaturation` | 0.0 | `SplitToningHighlightSaturation` | 0..100 | 1:1 | ≠ 0 |
| `splitTone.shadowHue` | 0.0 | `SplitToningShadowHue` | 0..360 | 1:1 | ≠ 0 |
| `splitTone.shadowSaturation` | 0.0 | `SplitToningShadowSaturation` | 0..100 | 1:1 | ≠ 0 |
| `splitTone.balance` | 0.0 | `SplitToningBalance` | -100..+100 | 1:1 | ≠ 0 |

---

## 6. 映射公式详述

### 6.1 白平衡温度：相对偏移 ↔ 绝对开尔文（mired 线性模型）

本工程 `temperature` 是 **-100..+100 相对偏移**（0 = 中性，正 = 暖），LR `crs:Temperature` 是**绝对开尔文**（正 = 暖，即更低的 K 值）。取中性锚点 6500 K，用 **mired 线性**近似（贴近 LR 温控手感）：

```
mired0 = 1e6 / 6500 ≈ 153.846        // 6500K 的 mired 值
导出：kelvin = 1e6 / (mired0 + temperature * 1.30)
       crs:Temperature = round(kelvin)      // 取整开尔文
导入：mired = 1e6 / clamp(kelvin, 2000, 50000)
       temperature = clamp((mired - mired0) / 1.30, -100, +100)
```

校验：
- t=+100 → mired=283.85 → 3523 K（暖）✓；t=-100 → mired=23.85 → 41933 K（冷）✓；t=0 → 6500 K ✓。
- kelvin=4000 → mired=250 → t=+74.0；kelvin=10000 → mired=100 → t=-41.4。
- 往返（取整误差）：4000 K → t≈+73.96 → 回导出 → 1e6/(153.846+96.15)=4000.07 → round=4000 ✓。

**取舍**：锚点固定 6500 K，未耦合 LR「As Shot」原白平衡；LR 预设若基于不同基线白平衡导入会有系统偏差，属 v1 已知近似（见 §7）。

### 6.2 色调：Tint 量纲换算

LR `crs:Tint` 范围 **-150..+150**（正 = 品红，负 = 绿）；本工程 `tint` 为 -100..+100。

```
导出：crs:Tint = tint * 1.5        // 例：tint=+20 → "+30"
导入：tint = clamp(crs:Tint / 1.5, -100, +100)
```

### 6.3 曲线：见 §4。`lround` 取整保证 0..255 整数；`zonecurve::fromPoints` 复用既有量化工具。

---

## 7. v1 取舍（不互导项，文档化）

| 项 | 处理 | 理由 |
|----|------|------|
| **蒙版 / 局部调整**（`masks`） | XMP 导入忽略 `crs:Mask*`/`crs:MaskGroupBasedCorrections`；导出不写。`.lrp` JSON 中完整往返 | 计划 §4.5 明确「不做双向互导」；LR 蒙版 XML 结构复杂（rdf:li 嵌套 + 形状几何） |
| **点颜色**（`colorTargets`） | 不互导 XMP | LR `crs:ColorAdjustment*` 是固定 8 色目标的元组编码，本工程支持任意参考色，语义不对齐；v1 跳过 |
| **通道色调曲线** | 不互导 `crs:ToneCurvePV2012Red/Green/Blue` | 本工程仅单 RGB 曲线 |
| **参数曲线** | 不互导 `crs:Parametric*` | 本工程为点曲线表示；PV2012 点数组已覆盖 |
| **手动畸变** `distortion` | 不互导 | LR 无标准手动畸变 crs 属性（仅配置文件驱动的 `LensProfile*`） |
| **RAW 内嵌 XMP** | 读取列为可选（`XmpSidecar::readFromEmbedded`，libraw `xmpdata`）；不做则文档标注 deferred | sidecar + 预设是验收核心；内嵌读取增加 libraw 元数据路径 |

---

## 8. Golden 样例（测试契约）

### 8.1 导入样例（`rdf:Description` 片段，XmpDocument 应提取出右侧 7 个非默认属性）

```xml
<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 5.6-c140">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    xmlns:exif="http://ns.adobe.com/exif/1.0/"
    crs:Exposure2012="+0.50"
    crs:Contrast2012="+25"
    crs:Highlights2012="-40"
    crs:Temperature="4000"
    crs:Tint="+10"
    crs:Clarity2012="+30"
    crs:ToneCurvePV2012="0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255"
    exif:UnknownTag="shouldBeIgnored"/>
 </rdf:RDF>
</x:xmpmeta>
```

导入期望（`fromCrsAttrs` → `Adjustments`）：
- `exposure=0.5`，`contrast=25`，`highlights=-40`，`clarity=30`
- `temperature` = 由 4000 K 反推 ≈ +74（容差 ±1.0）
- `tint = 10/1.5 ≈ 6.667`（容差 1e-3）
- `curve` 非空（`zonecurve::fromPoints` 量化后的 6 点）
- `exif:UnknownTag` 与缺失字段被忽略/保持默认

### 8.2 导出样例（`Adjustments` → crs 属性，`.xmp` 结构合法）

给定 `exposure=0.5, temperature→≈+74, tint=20, clarity=30, hsl[Red].saturation=10, splitTone.highlightSaturation=25`，导出应产生 `rdf:Description` 携带对应 `crs:` 属性，且：
- 结构为 `x:xmpmeta > rdf:RDF > rdf:Description`（含 `xmlns:crs` 声明，rdf:about 可空）。
- 属性名拼写与上表一致；数值可 `strtod` 往返。
- 未知字段（masks/colorTargets/distortion）**不出现**在导出中。

### 8.3 测试清单（TestXmpProfile / TestXmpDocument）

1. **导出→导入往返**：构造 Adjustments（覆盖各分组），`toCrsAttrs` → `fromCrsAttrs`，逐字段断言（容差 1e-6；温度容差 ±1.0 因取整）。
2. **XmpDocument 往返**：`serialize(map)` → `parse(text)` 得回等价 map；golden 样例解析出正确键值。
3. **未知字段容错**：含 `crs:Mask*`/`crs:ToneCurvePV2012Red`/`crs:ColorAdjustmentRed`/未知命名空间 → 不崩溃、不改变已识别字段。
4. **缺省字段**：空/缺属性 → 全部默认；`ToneCurvePV2012` 为恒等 → `curve` 为空。
5. **越界钳制**：`crs:Exposure2012="99"` → 钳制到范围。
6. **格式化**：`%.6g` 往返（`0.5`、`25`、`4000` 等）。
7. **golden 结构**：导出文本可由第二个 XMP 解析器（测试内再解析一次）成功解析，crs 属性齐全。
