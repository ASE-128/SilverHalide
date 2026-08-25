#include "utils/XmpProfile.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/ZoneCurve.h"
#include "utils/MathUtils.h"

namespace lrc {
namespace {

// Colour names in ColorRange order (Red, Orange, Yellow, Green, Aqua, Blue,
// Purple, Magenta) - they double as the Lightroom HSL attribute suffixes.
const char* const kColorNames[] = {
    "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};

// White-balance anchor: 6500 K expressed in mired (micro-reciprocal degrees).
// Lightroom's crs:Temperature is absolute kelvin while Adjustments::temperature
// is a -100..+100 relative offset around this anchor (see mapping doc 6.1).
constexpr double kMired0 = 1e6 / 6500.0;
constexpr double kTempScale = 1.30;  // mired per offset unit
constexpr double kTintScale = 1.5;   // LR Tint is on a -150..+150 scale

// Formats a double with the shortest round-trip representation (chars_format::
// general), e.g. 25.0 -> "25", 0.5 -> "0.5".  Parsing this back with strtod
// reproduces the value exactly.
std::string formatDouble(double v) {
    char buf[128];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v,
                                   std::chars_format::general);
    return std::string(buf, res.ptr);
}

// Lenient strtod-style parse: tolerates a leading '+', surrounding whitespace
// and trailing junk.  Returns false only when no number was consumed at all.
bool parseDouble(const std::string& s, double& out) {
    if (s.empty())
        return false;
    const char* start = s.c_str();
    char* end = nullptr;
    out = std::strtod(start, &end);
    return end != start;
}

// Writes a scalar field only when it differs from its default.
void putScalar(std::map<std::string, std::string>& out, const std::string& key,
               double value, double def) {
    if (value != def)
        out[key] = formatDouble(value);
}

// Exports the tone curve.  An empty (identity) curve writes only
// ToneCurveName2012="Linear"; a non-empty curve scales the points to 0..255,
// pads the black/white endpoints as needed and writes ToneCurvePV2012 plus
// ToneCurveName2012="Custom" (mapping doc section 4).
void exportCurve(std::map<std::string, std::string>& out,
                 const std::vector<ToneCurvePoint>& curve) {
    if (curve.empty()) {
        out["ToneCurveName2012"] = "Linear";
        return;
    }
    out["ToneCurveName2012"] = "Custom";
    std::string s;
    auto append = [&s](long x, long y) {
        if (!s.empty())
            s += ", ";
        s += std::to_string(x);
        s += ",";
        s += std::to_string(y);
    };
    const size_t n = curve.size();
    for (size_t i = 0; i < n; ++i) {
        const long xi = std::clamp<long>(std::lround(curve[i].x * 255.0), 0, 255);
        const long yi = std::clamp<long>(std::lround(curve[i].y * 255.0), 0, 255);
        if (i == 0 && xi != 0)
            append(0, yi);  // pad start so the curve begins at x == 0
        append(xi, yi);
        if (i == n - 1 && xi != 255)
            append(255, 255);  // pad end so the curve reaches x == 255
    }
    out["ToneCurvePV2012"] = s;
}

// Splits ToneCurvePV2012 on commas/whitespace into (x, y) pairs, clamps each
// value to [0,255] and normalizes to [0,1].  Returns an empty vector on any
// parse failure or when the token count is odd.
std::vector<ToneCurvePoint> parseCurvePV2012(const std::string& s) {
    std::vector<double> nums;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ',' ||
                         std::isspace(static_cast<unsigned char>(s[i]))))
            ++i;
        if (i >= n)
            break;
        const size_t start = i;
        while (i < n && s[i] != ',' &&
               !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        double v = 0.0;
        if (!parseDouble(s.substr(start, i - start), v))
            return {};  // parse failure -> keep empty curve
        nums.push_back(v);
    }
    if (nums.empty() || nums.size() % 2 != 0)
        return {};
    std::vector<ToneCurvePoint> points;
    points.reserve(nums.size() / 2);
    for (size_t j = 0; j + 1 < nums.size(); j += 2) {
        const double x = std::clamp(nums[j], 0.0, 255.0) / 255.0;
        const double y = std::clamp(nums[j + 1], 0.0, 255.0) / 255.0;
        points.push_back({static_cast<float>(x), static_cast<float>(y)});
    }
    return points;
}

// True when every normalized point satisfies y == x (the linear identity), so
// an "S-curve-less" PV2012 list imports back to an empty curve.
bool isIdentityCurve(const std::vector<ToneCurvePoint>& points) {
    for (const auto& p : points) {
        if (std::abs(static_cast<double>(p.y) - static_cast<double>(p.x)) > 1e-6)
            return false;
    }
    return true;
}

}  // namespace

std::map<std::string, std::string> XmpProfile::toCrsAttrs(
    const Adjustments& adj) {
    std::map<std::string, std::string> out;

    // Basic panel (1:1 scalars).
    putScalar(out, "Exposure2012", adj.exposure, 0.0);
    putScalar(out, "Contrast2012", adj.contrast, 0.0);
    putScalar(out, "Highlights2012", adj.highlights, 0.0);
    putScalar(out, "Shadows2012", adj.shadows, 0.0);
    putScalar(out, "Whites2012", adj.whites, 0.0);
    putScalar(out, "Blacks2012", adj.blacks, 0.0);
    putScalar(out, "Saturation", adj.saturation, 0.0);

    // White balance: relative offset <-> absolute kelvin via a mired-linear
    // model anchored at 6500 K (mapping doc 6.1).
    if (adj.temperature != 0.0) {
        const double kelvin = 1e6 / (kMired0 + adj.temperature * kTempScale);
        out["Temperature"] = std::to_string(std::lround(kelvin));
    }
    // Tint: LR uses a -150..+150 scale, we use -100..+100 (mapping doc 6.2).
    if (adj.tint != 0.0) {
        out["Tint"] = formatDouble(adj.tint * kTintScale);
    }

    // Effects / detail / vignette / grain.
    putScalar(out, "Texture", adj.texture, 0.0);
    putScalar(out, "Clarity2012", adj.clarity, 0.0);
    putScalar(out, "Dehaze", adj.dehaze, 0.0);
    putScalar(out, "Vibrance", adj.vibrance, 0.0);

    putScalar(out, "SharpenAmount", adj.sharpness, 0.0);
    putScalar(out, "SharpenRadius", adj.sharpnessRadius, 1.0);
    putScalar(out, "SharpenDetail", adj.sharpnessDetail, 0.0);
    putScalar(out, "SharpenMasking", adj.sharpnessMasking, 0.0);

    // Noise reduction (LR detail panel).
    putScalar(out, "LuminanceSmoothing", adj.luminance, 0.0);
    putScalar(out, "LuminanceNoiseReductionDetail", adj.luminanceDetail, 50.0);
    putScalar(out, "LuminanceNoiseReductionContrast", adj.luminanceContrast, 0.0);
    putScalar(out, "ColorNoiseReduction", adj.colorNoiseReduction, 0.0);
    putScalar(out, "ColorNoiseReductionSmoothness",
              adj.colorNoiseReductionSmoothness, 50.0);

    putScalar(out, "PostCropVignetteAmount", adj.vignette, 0.0);
    putScalar(out, "PostCropVignetteMidpoint", adj.vignetteMidpoint, 50.0);
    putScalar(out, "PostCropVignetteRoundness", adj.vignetteRoundness, 0.0);
    putScalar(out, "PostCropVignetteFeather", adj.vignetteFeather, 50.0);
    putScalar(out, "PostCropVignetteHighlightContrast",
              adj.vignetteHighlights, 0.0);

    putScalar(out, "GrainAmount", adj.grain, 0.0);
    putScalar(out, "GrainSize", adj.grainSize, 25.0);
    putScalar(out, "GrainFrequency", adj.grainRoughness, 50.0);

    // Tone curve (section 4).
    exportCurve(out, adj.curve);

    // HSL: 24 attributes in ColorRange order.
    for (size_t i = 0; i < static_cast<size_t>(ColorRange::Count); ++i) {
        const HSLChannel& ch = adj.hsl[i];
        const std::string color(kColorNames[i]);
        putScalar(out, color + "Hue", ch.hue, 0.0);
        putScalar(out, color + "Saturation", ch.saturation, 0.0);
        putScalar(out, color + "Luminance", ch.luminance, 0.0);
    }

    // Split toning.
    putScalar(out, "SplitToningHighlightHue", adj.splitTone.highlightHue, 0.0);
    putScalar(out, "SplitToningHighlightSaturation",
              adj.splitTone.highlightSaturation, 0.0);
    putScalar(out, "SplitToningShadowHue", adj.splitTone.shadowHue, 0.0);
    putScalar(out, "SplitToningShadowSaturation",
              adj.splitTone.shadowSaturation, 0.0);
    putScalar(out, "SplitToningBalance", adj.splitTone.balance, 0.0);

    return out;
}

Adjustments XmpProfile::fromCrsAttrs(
    const std::map<std::string, std::string>& attrs) {
    Adjustments a = Adjustments::defaults();

    // Reads a key if present AND parseable; on any failure the field keeps its
    // default because we only assign inside the guarded branch.
    auto parse = [&](const std::string& key, double& out) -> bool {
        const auto it = attrs.find(key);
        if (it == attrs.end())
            return false;
        return parseDouble(it->second, out);
    };

    double v = 0.0;

    // Basic panel (1:1 scalars).
    if (parse("Exposure2012", v))
        a.exposure = clamp(v, -5.0, 5.0);
    if (parse("Contrast2012", v))
        a.contrast = clamp(v, -100.0, 100.0);
    if (parse("Highlights2012", v))
        a.highlights = clamp(v, -100.0, 100.0);
    if (parse("Shadows2012", v))
        a.shadows = clamp(v, -100.0, 100.0);
    if (parse("Whites2012", v))
        a.whites = clamp(v, -100.0, 100.0);
    if (parse("Blacks2012", v))
        a.blacks = clamp(v, -100.0, 100.0);
    if (parse("Saturation", v))
        a.saturation = clamp(v, -100.0, 100.0);

    // White balance (6.1): kelvin is clamped to the LR absolute range first.
    if (parse("Temperature", v)) {
        const double kelvin = clamp(v, 2000.0, 50000.0);
        a.temperature = clamp((1e6 / kelvin - kMired0) / kTempScale, -100.0, 100.0);
    }
    // Tint (6.2).
    if (parse("Tint", v))
        a.tint = clamp(v / kTintScale, -100.0, 100.0);

    // Effects / detail / vignette / grain.
    if (parse("Texture", v))
        a.texture = clamp(v, -100.0, 100.0);
    if (parse("Clarity2012", v))
        a.clarity = clamp(v, -100.0, 100.0);
    if (parse("Dehaze", v))
        a.dehaze = clamp(v, -100.0, 100.0);
    if (parse("Vibrance", v))
        a.vibrance = clamp(v, -100.0, 100.0);

    if (parse("SharpenAmount", v))
        a.sharpness = clamp(v, 0.0, 150.0);
    if (parse("SharpenRadius", v))
        a.sharpnessRadius = clamp(v, 0.5, 3.0);
    if (parse("SharpenDetail", v))
        a.sharpnessDetail = clamp(v, 0.0, 100.0);
    if (parse("SharpenMasking", v))
        a.sharpnessMasking = clamp(v, 0.0, 100.0);

    if (parse("LuminanceSmoothing", v))
        a.luminance = clamp(v, 0.0, 100.0);
    if (parse("LuminanceNoiseReductionDetail", v))
        a.luminanceDetail = clamp(v, 0.0, 100.0);
    if (parse("LuminanceNoiseReductionContrast", v))
        a.luminanceContrast = clamp(v, 0.0, 100.0);
    if (parse("ColorNoiseReduction", v))
        a.colorNoiseReduction = clamp(v, 0.0, 100.0);
    if (parse("ColorNoiseReductionSmoothness", v))
        a.colorNoiseReductionSmoothness = clamp(v, 0.0, 100.0);

    if (parse("PostCropVignetteAmount", v))
        a.vignette = clamp(v, -100.0, 100.0);
    if (parse("PostCropVignetteMidpoint", v))
        a.vignetteMidpoint = clamp(v, 0.0, 100.0);
    if (parse("PostCropVignetteRoundness", v))
        a.vignetteRoundness = clamp(v, -100.0, 100.0);
    if (parse("PostCropVignetteFeather", v))
        a.vignetteFeather = clamp(v, 0.0, 100.0);
    if (parse("PostCropVignetteHighlightContrast", v))
        a.vignetteHighlights = clamp(v, 0.0, 100.0);

    if (parse("GrainAmount", v))
        a.grain = clamp(v, 0.0, 100.0);
    if (parse("GrainSize", v))
        a.grainSize = clamp(v, 0.0, 100.0);
    if (parse("GrainFrequency", v))
        a.grainRoughness = clamp(v, 0.0, 100.0);

    // Tone curve (section 4): identity import stays empty, otherwise quantize
    // through the zone handles.
    const auto cit = attrs.find("ToneCurvePV2012");
    if (cit != attrs.end()) {
        const auto points = parseCurvePV2012(cit->second);
        if (!points.empty() && !isIdentityCurve(points))
            a.curve = zonecurve::fromPoints(points);
    }

    // HSL (5.1): 24 attributes in ColorRange order.
    for (size_t i = 0; i < static_cast<size_t>(ColorRange::Count); ++i) {
        const std::string color(kColorNames[i]);
        if (parse(color + "Hue", v))
            a.hsl[i].hue = clamp(v, -100.0, 100.0);
        if (parse(color + "Saturation", v))
            a.hsl[i].saturation = clamp(v, -100.0, 100.0);
        if (parse(color + "Luminance", v))
            a.hsl[i].luminance = clamp(v, -100.0, 100.0);
    }

    // Split toning (5.2).
    if (parse("SplitToningHighlightHue", v))
        a.splitTone.highlightHue = clamp(v, 0.0, 360.0);
    if (parse("SplitToningHighlightSaturation", v))
        a.splitTone.highlightSaturation = clamp(v, 0.0, 100.0);
    if (parse("SplitToningShadowHue", v))
        a.splitTone.shadowHue = clamp(v, 0.0, 360.0);
    if (parse("SplitToningShadowSaturation", v))
        a.splitTone.shadowSaturation = clamp(v, 0.0, 100.0);
    if (parse("SplitToningBalance", v))
        a.splitTone.balance = clamp(v, -100.0, 100.0);

    return a;
}

void XmpProfile::toSpotCorrectionXml(
    const std::vector<SpotRemoval>& spots,
    std::vector<XmpDocument::SpotCorrectionXml>& out) {
    out.clear();
    out.reserve(spots.size());
    for (const SpotRemoval& s : spots) {
        XmpDocument::SpotCorrectionXml x;
        // Type: the only two Lightroom correction modes.  Unknown values are
        // never written; import defaults unknown values to Clone.
        x.type = s.type == SpotType::Heal ? "Heal" : "Clone";
        x.source = "Source";      // manual source (no auto source in v1)
        x.amount = "100";         // opacity fixed at 100% (PHASE_5)
        x.active = s.visible ? "true" : "false";
        // Geometry is normalized 0..1 (the engine's SpotRemoval unit):
        // source/target coordinates are relative to the image, radius is
        // normalized against the longer edge (see Adjustments.h).  The mask
        // circle is centred on the target point; the source point is carried
        // by the custom SourceX/Y keys because Lightroom's per-spot XMP has no
        // direct source offset.
        x.centerX = formatDouble(s.targetX);
        x.centerY = formatDouble(s.targetY);
        x.radius = formatDouble(s.radius);
        x.sourceX = formatDouble(s.sourceX);
        x.sourceY = formatDouble(s.sourceY);
        out.push_back(std::move(x));
    }
}

std::vector<SpotRemoval> XmpProfile::fromSpotCorrectionXml(
    const std::vector<XmpDocument::SpotCorrectionXml>& xmlSpots) {
    std::vector<SpotRemoval> out;
    out.reserve(xmlSpots.size());
    for (const XmpDocument::SpotCorrectionXml& x : xmlSpots) {
        SpotRemoval s;
        // Type is tolerant: anything that is not exactly "Heal" maps to Clone
        // (Lightroom's default) instead of failing the whole import.
        s.type = x.type == "Heal" ? SpotType::Heal : SpotType::Clone;
        // Active defaults to true; only an explicit "false"/"0" turns it off.
        s.visible = !(x.active == "false" || x.active == "False" ||
                      x.active == "0");
        double v = 0.0;
        if (parseDouble(x.centerX, v))
            s.targetX = static_cast<float>(clamp(v, 0.0, 1.0));
        if (parseDouble(x.centerY, v))
            s.targetY = static_cast<float>(clamp(v, 0.0, 1.0));
        if (parseDouble(x.radius, v))
            s.radius = static_cast<float>(clamp(v, 0.0, 1.0));
        if (parseDouble(x.sourceX, v))
            s.sourceX = static_cast<float>(clamp(v, 0.0, 1.0));
        if (parseDouble(x.sourceY, v))
            s.sourceY = static_cast<float>(clamp(v, 0.0, 1.0));
        out.push_back(s);
    }
    return out;
}

}  // namespace lrc
