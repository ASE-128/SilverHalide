#include "utils/ProfileIO.h"

#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace lrc {

namespace {

// Reads a whole file into a string. Returns nullopt on failure.
std::optional<std::string> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Writes `text` to `path` atomically: the content is first written to a
// temporary file in the same directory, then renamed over the target once it
// is fully flushed. A crash mid-write leaves only the temp file, never a
// truncated target that callers might trust via exists(). The temp file is
// removed on any failure. On MSVC std::filesystem::rename uses
// MoveFileExW(MOVEFILE_REPLACE_EXISTING), so an existing target is replaced;
// if that is not supported, a remove-then-rename fallback is attempted.
bool writeFileAtomic(const std::filesystem::path& path,
                     const std::string& text) {
    std::filesystem::path tmpPath = path;
    tmpPath += ".tmp";

    {
        std::ofstream os(tmpPath, std::ios::binary | std::ios::trunc);
        if (!os)
            return false;
        os.write(text.data(), static_cast<std::streamsize>(text.size()));
        os.flush();
        if (!os) {
            os.close();
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        // The target could not be replaced directly (e.g. an exotic filesystem
        // without rename-over-existing): drop the old file, then retry.
        std::error_code rmEc;
        std::filesystem::remove(path, rmEc);
        if (!rmEc)
            std::filesystem::rename(tmpPath, path, ec);
    }
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
}

} // namespace

json::Value ProfileIO::toJson(const Adjustments& a) {
    json::Value root = json::Value::object();
    root["version"] = 6;
    root["exposure"] = a.exposure;
    root["contrast"] = a.contrast;
    root["highlights"] = a.highlights;
    root["shadows"] = a.shadows;
    root["whites"] = a.whites;
    root["blacks"] = a.blacks;
    root["temperature"] = a.temperature;
    root["tint"] = a.tint;
    root["saturation"] = a.saturation;
    root["texture"] = a.texture;
    root["clarity"] = a.clarity;
    root["dehaze"] = a.dehaze;
    root["vibrance"] = a.vibrance;
    root["sharpness"] = a.sharpness;
    root["sharpnessRadius"] = a.sharpnessRadius;
    root["sharpnessDetail"] = a.sharpnessDetail;
    root["sharpnessMasking"] = a.sharpnessMasking;
    root["luminance"] = a.luminance;
    root["luminanceDetail"] = a.luminanceDetail;
    root["luminanceContrast"] = a.luminanceContrast;
    root["colorNoiseReduction"] = a.colorNoiseReduction;
    root["colorNoiseReductionSmoothness"] = a.colorNoiseReductionSmoothness;
    root["distortion"] = a.distortion;
    root["vignette"] = a.vignette;
    root["lensfunEnabled"] = a.lensfunEnabled;
    root["lensfunCameraMaker"] = a.lensfunCameraMaker;
    root["lensfunCameraModel"] = a.lensfunCameraModel;
    root["lensfunLensMaker"] = a.lensfunLensMaker;
    root["lensfunLensModel"] = a.lensfunLensModel;
    root["lensfunFocalLengthMm"] = a.lensfunFocalLengthMm;
    root["lensfunAperture"] = a.lensfunAperture;
    root["lensfunCropFactor"] = a.lensfunCropFactor;
    root["vignetteMidpoint"] = a.vignetteMidpoint;
    root["vignetteRoundness"] = a.vignetteRoundness;
    root["vignetteFeather"] = a.vignetteFeather;
    root["vignetteHighlights"] = a.vignetteHighlights;
    root["vignetteStyle"] = a.vignetteStyle;
    root["grain"] = a.grain;
    root["grainSize"] = a.grainSize;
    root["grainRoughness"] = a.grainRoughness;

    json::Value curve = json::Value::array();
    for (const ToneCurvePoint& p : a.curve) {
        json::Value pt = json::Value::array();
        pt.push_back(json::Value(p.x));
        pt.push_back(json::Value(p.y));
        curve.push_back(std::move(pt));
    }
    root["curve"] = std::move(curve);

    // Per-channel HSL.
    static const char* kColorNames[8] = {
        "red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
    json::Value hslObj = json::Value::object();
    for (int ci = 0; ci < 8; ++ci) {
        const auto& ch = a.hsl[ci];
        if (ch.hue != 0.0 || ch.saturation != 0.0 || ch.luminance != 0.0) {
            json::Value chObj = json::Value::object();
            chObj["hue"] = ch.hue;
            chObj["saturation"] = ch.saturation;
            chObj["luminance"] = ch.luminance;
            hslObj[kColorNames[ci]] = std::move(chObj);
        }
    }
    root["hsl"] = std::move(hslObj);

    // Split toning.
    json::Value st = json::Value::object();
    st["highlightHue"] = a.splitTone.highlightHue;
    st["highlightSaturation"] = a.splitTone.highlightSaturation;
    st["shadowHue"] = a.splitTone.shadowHue;
    st["shadowSaturation"] = a.splitTone.shadowSaturation;
    st["balance"] = a.splitTone.balance;
    root["splitTone"] = std::move(st);

    // Masked local adjustments.
    json::Value masks = json::Value::array();
    for (const Mask& m : a.masks) {
        json::Value mo = json::Value::object();
        mo["type"] = static_cast<int>(m.type);
        mo["invert"] = m.invert;
        mo["opacity"] = m.opacity;
        mo["exposure"] = m.exposure;
        mo["contrast"] = m.contrast;
        mo["highlights"] = m.highlights;
        mo["shadows"] = m.shadows;
        mo["whites"] = m.whites;
        mo["blacks"] = m.blacks;
        mo["clarity"] = m.clarity;
        mo["dehaze"] = m.dehaze;
        mo["saturation"] = m.saturation;
        mo["temperature"] = m.temperature;
        mo["tint"] = m.tint;
        // Sub-mask modifiers (colour / luminance range + combination mode).
        // Serialized unconditionally so a round-trip is lossless even for
        // geometry masks that do not use them.
        mo["useColorRange"] = m.useColorRange;
        mo["colorRefR"] = m.colorRefR;
        mo["colorRefG"] = m.colorRefG;
        mo["colorRefB"] = m.colorRefB;
        mo["colorRange"] = m.colorRange;
        mo["colorSmoothness"] = m.colorSmoothness;
        mo["useLuminance"] = m.useLuminance;
        mo["luminanceRef"] = m.luminanceRef;
        mo["luminanceRange"] = m.luminanceRange;
        mo["luminanceSmoothness"] = m.luminanceSmoothness;
        mo["maskUnion"] = m.maskUnion;
        if (m.type == MaskType::LinearGradient) {
            mo["startX"] = m.linear.startX;
            mo["startY"] = m.linear.startY;
            mo["endX"] = m.linear.endX;
            mo["endY"] = m.linear.endY;
            mo["feather"] = m.linear.feather;
        } else if (m.type == MaskType::RadialGradient) {
            mo["centerX"] = m.radial.centerX;
            mo["centerY"] = m.radial.centerY;
            mo["radiusX"] = m.radial.radiusX;
            mo["radiusY"] = m.radial.radiusY;
            mo["feather"] = m.radial.feather;
            mo["radialInvert"] = m.radial.invert;
        } else if (m.type == MaskType::Brush) {
            json::Value dabs = json::Value::array();
            for (const BrushDab& d : m.brush) {
                json::Value dObj = json::Value::array();
                dObj.push_back(json::Value(d.x));
                dObj.push_back(json::Value(d.y));
                dObj.push_back(json::Value(d.radius));
                dObj.push_back(json::Value(d.softness));
                dabs.push_back(std::move(dObj));
            }
            mo["brush"] = std::move(dabs);
        } else if (m.type == MaskType::ColorRange) {
            mo["hasReference"] = m.color.hasReference;
            mo["refR"] = m.color.refR;
            mo["refG"] = m.color.refG;
            mo["refB"] = m.color.refB;
            mo["hueTolerance"] = m.color.hueTolerance;
            mo["satTolerance"] = m.color.satTolerance;
            mo["lumaTolerance"] = m.color.lumaTolerance;
            mo["softness"] = m.color.softness;
        }
        masks.push_back(std::move(mo));
    }
    root["masks"] = std::move(masks);

    // Point-colour targets.
    json::Value targets = json::Value::array();
    for (const ColorTarget& ct : a.colorTargets) {
        json::Value tObj = json::Value::object();
        tObj["enabled"] = ct.enabled;
        tObj["refR"] = ct.refR;
        tObj["refG"] = ct.refG;
        tObj["refB"] = ct.refB;
        tObj["hueShift"] = ct.hueShift;
        tObj["saturation"] = ct.saturation;
        tObj["luminance"] = ct.luminance;
        tObj["range"] = ct.range;
        tObj["smoothness"] = ct.smoothness;
        tObj["amount"] = ct.amount;
        targets.push_back(std::move(tObj));
    }
    root["colorTargets"] = std::move(targets);

    // Spot removal / healing brush corrections. Serialized unconditionally so
    // a round-trip is lossless even for an empty list.
    json::Value spots = json::Value::array();
    for (const SpotRemoval& s : a.spotRemovals) {
        json::Value so = json::Value::object();
        so["type"] = static_cast<int>(s.type);
        so["sourceX"] = s.sourceX;
        so["sourceY"] = s.sourceY;
        so["targetX"] = s.targetX;
        so["targetY"] = s.targetY;
        so["radius"] = s.radius;
        so["visible"] = s.visible;
        spots.push_back(std::move(so));
    }
    root["spotRemovals"] = std::move(spots);

    // AI Denoise. Serialized unconditionally so a round-trip is lossless even
    // when denoise is disabled.
    json::Value aiDenoiseObj = json::Value::object();
    aiDenoiseObj["enabled"] = a.aiDenoise.enabled;
    aiDenoiseObj["strength"] = a.aiDenoise.strength;
    aiDenoiseObj["mode"] = static_cast<int>(a.aiDenoise.mode);
    aiDenoiseObj["modelVersion"] = a.aiDenoise.modelVersion;
    root["aiDenoise"] = std::move(aiDenoiseObj);

    // AI Mask parameters. Serialized only when the adjustments contain at least
    // one AiMask so a legacy preset loads unchanged.
    bool hasAiMask = false;
    for (const Mask& m : a.masks) {
        if (m.type == MaskType::AiMask) {
            hasAiMask = true;
            break;
        }
    }
    if (hasAiMask) {
        json::Value aiMaskArr = json::Value::array();
        for (const Mask& m : a.masks) {
            if (m.type != MaskType::AiMask)
                continue;
            json::Value ao = json::Value::object();
            ao["aiTarget"] = static_cast<int>(m.ai.target);
            ao["aiThreshold"] = m.ai.threshold;
            ao["aiFeather"] = m.ai.feather;
            ao["aiInvert"] = m.ai.invert;
            ao["aiModelInputSize"] = m.ai.modelInputSize;
            ao["aiCachedWeightVersion"] = m.ai.cachedWeightVersion;
            ao["aiModelIdOverride"] = m.ai.modelIdOverride;
            aiMaskArr.push_back(std::move(ao));
        }
        root["aiMasks"] = std::move(aiMaskArr);
    }

    return root;
}

Adjustments ProfileIO::fromJson(const json::Value& v) {
    Adjustments a;
    if (!v.isObject())
        return a;
    a.exposure = v["exposure"].asNumber(a.exposure);
    a.contrast = v["contrast"].asNumber(a.contrast);
    a.highlights = v["highlights"].asNumber(a.highlights);
    a.shadows = v["shadows"].asNumber(a.shadows);
    a.whites = v["whites"].asNumber(a.whites);
    a.blacks = v["blacks"].asNumber(a.blacks);
    a.temperature = v["temperature"].asNumber(a.temperature);
    a.tint = v["tint"].asNumber(a.tint);
    a.saturation = v["saturation"].asNumber(a.saturation);
    a.texture = v["texture"].asNumber(a.texture);
    a.clarity = v["clarity"].asNumber(a.clarity);
    a.dehaze = v["dehaze"].asNumber(a.dehaze);
    a.vibrance = v["vibrance"].asNumber(a.vibrance);
    a.sharpness = v["sharpness"].asNumber(a.sharpness);
    a.sharpnessRadius = v["sharpnessRadius"].asNumber(a.sharpnessRadius);
    a.sharpnessDetail = v["sharpnessDetail"].asNumber(a.sharpnessDetail);
    a.sharpnessMasking = v["sharpnessMasking"].asNumber(a.sharpnessMasking);
    a.luminance = v["luminance"].asNumber(a.luminance);
    a.luminanceDetail = v["luminanceDetail"].asNumber(a.luminanceDetail);
    a.luminanceContrast = v["luminanceContrast"].asNumber(a.luminanceContrast);
    a.colorNoiseReduction = v["colorNoiseReduction"].asNumber(a.colorNoiseReduction);
    a.colorNoiseReductionSmoothness =
        v["colorNoiseReductionSmoothness"].asNumber(a.colorNoiseReductionSmoothness);
    a.distortion = v["distortion"].asNumber(a.distortion);
    a.vignette = v["vignette"].asNumber(a.vignette);
    a.lensfunEnabled = v["lensfunEnabled"].asBool(a.lensfunEnabled);
    // json::Value::asString() takes no default argument, so missing keys
    // must preserve the existing (default) value explicitly.
    if (v.contains("lensfunCameraMaker"))
        a.lensfunCameraMaker = v["lensfunCameraMaker"].asString();
    if (v.contains("lensfunCameraModel"))
        a.lensfunCameraModel = v["lensfunCameraModel"].asString();
    if (v.contains("lensfunLensMaker"))
        a.lensfunLensMaker = v["lensfunLensMaker"].asString();
    if (v.contains("lensfunLensModel"))
        a.lensfunLensModel = v["lensfunLensModel"].asString();
    a.lensfunFocalLengthMm = static_cast<float>(
        v["lensfunFocalLengthMm"].asNumber(a.lensfunFocalLengthMm));
    a.lensfunAperture = static_cast<float>(
        v["lensfunAperture"].asNumber(a.lensfunAperture));
    a.lensfunCropFactor = static_cast<float>(
        v["lensfunCropFactor"].asNumber(a.lensfunCropFactor));
    a.vignetteMidpoint = v["vignetteMidpoint"].asNumber(a.vignetteMidpoint);
    a.vignetteRoundness = v["vignetteRoundness"].asNumber(a.vignetteRoundness);
    a.vignetteFeather = v["vignetteFeather"].asNumber(a.vignetteFeather);
    a.vignetteHighlights =
        v["vignetteHighlights"].asNumber(a.vignetteHighlights);
    a.vignetteStyle =
        std::clamp(v["vignetteStyle"].asInt(a.vignetteStyle), 0, 2);
    a.grain = v["grain"].asNumber(a.grain);
    a.grainSize = v["grainSize"].asNumber(a.grainSize);
    a.grainRoughness = v["grainRoughness"].asNumber(a.grainRoughness);
    if (v.contains("curve") && v["curve"].isArray()) {
        const auto& pts = v["curve"].items();
        a.curve.reserve(pts.size());
        for (const json::Value& pt : pts) {
            if (pt.isArray() && pt.size() == 2) {
                a.curve.push_back(ToneCurvePoint{
                    static_cast<float>(pt[0].asNumber(0.0)),
                    static_cast<float>(pt[1].asNumber(0.0))});
            }
        }
    }

    // Per-channel HSL.
    static const char* kColorNames[8] = {
        "red", "orange", "yellow", "green", "aqua", "blue", "purple", "magenta"};
    if (v.contains("hsl") && v["hsl"].isObject()) {
        const auto& hslObj = v["hsl"];
        for (int ci = 0; ci < 8; ++ci) {
            if (hslObj.contains(kColorNames[ci])) {
                const auto& chObj = hslObj[kColorNames[ci]];
                if (chObj.isObject()) {
                    a.hsl[ci].hue = chObj["hue"].asNumber(0.0);
                    a.hsl[ci].saturation = chObj["saturation"].asNumber(0.0);
                    a.hsl[ci].luminance = chObj["luminance"].asNumber(0.0);
                }
            }
        }
    }

    // Split toning.
    if (v.contains("splitTone") && v["splitTone"].isObject()) {
        const auto& st = v["splitTone"];
        a.splitTone.highlightHue = st["highlightHue"].asNumber(0.0);
        a.splitTone.highlightSaturation = st["highlightSaturation"].asNumber(0.0);
        a.splitTone.shadowHue = st["shadowHue"].asNumber(0.0);
        a.splitTone.shadowSaturation = st["shadowSaturation"].asNumber(0.0);
        a.splitTone.balance = st["balance"].asNumber(0.0);
    }

    // Masked local adjustments.
    if (v.contains("masks") && v["masks"].isArray()) {
        for (const json::Value& mo : v["masks"].items()) {
            if (!mo.isObject())
                continue;
            Mask m;
            // Cap at MaskType::AiMask (not ColorRange) so an AI mask type
            // survives the round-trip; a lower cap silently downgraded it to a
            // colour-range mask and lost the ai params.
            m.type = static_cast<MaskType>(
                std::clamp(mo["type"].asNumber(0.0), 0.0,
                           static_cast<double>(MaskType::AiMask)));
            // Bool values are stored as JSON booleans, so read with asBool.
            m.invert = mo["invert"].asBool(false);
            m.opacity = mo["opacity"].asNumber(1.0);
            m.exposure = mo["exposure"].asNumber(0.0);
            m.contrast = mo["contrast"].asNumber(0.0);
            m.highlights = mo["highlights"].asNumber(0.0);
            m.shadows = mo["shadows"].asNumber(0.0);
            m.whites = mo["whites"].asNumber(0.0);
            m.blacks = mo["blacks"].asNumber(0.0);
            m.clarity = mo["clarity"].asNumber(0.0);
            m.dehaze = mo["dehaze"].asNumber(0.0);
            m.saturation = mo["saturation"].asNumber(0.0);
            m.temperature = mo["temperature"].asNumber(0.0);
            m.tint = mo["tint"].asNumber(0.0);
            // Sub-mask modifiers. Missing keys keep their defaults, so older presets
            // load unchanged.
            m.useColorRange = mo["useColorRange"].asBool(false);
            m.colorRefR = static_cast<float>(mo["colorRefR"].asNumber(0.0));
            m.colorRefG = static_cast<float>(mo["colorRefG"].asNumber(0.0));
            m.colorRefB = static_cast<float>(mo["colorRefB"].asNumber(0.0));
            m.colorRange = static_cast<float>(mo["colorRange"].asNumber(50.0));
            m.colorSmoothness =
                static_cast<float>(mo["colorSmoothness"].asNumber(50.0));
            m.useLuminance = mo["useLuminance"].asBool(false);
            m.luminanceRef =
                static_cast<float>(mo["luminanceRef"].asNumber(50.0));
            m.luminanceRange =
                static_cast<float>(mo["luminanceRange"].asNumber(50.0));
            m.luminanceSmoothness =
                static_cast<float>(mo["luminanceSmoothness"].asNumber(50.0));
            m.maskUnion = mo["maskUnion"].asBool(false);
            if (m.type == MaskType::LinearGradient) {
                m.linear.startX = static_cast<float>(mo["startX"].asNumber(0.0));
                m.linear.startY = static_cast<float>(mo["startY"].asNumber(0.0));
                m.linear.endX = static_cast<float>(mo["endX"].asNumber(1.0));
                m.linear.endY = static_cast<float>(mo["endY"].asNumber(0.0));
                m.linear.feather = static_cast<float>(mo["feather"].asNumber(0.5));
            } else if (m.type == MaskType::RadialGradient) {
                m.radial.centerX = static_cast<float>(mo["centerX"].asNumber(0.5));
                m.radial.centerY = static_cast<float>(mo["centerY"].asNumber(0.5));
                m.radial.radiusX = static_cast<float>(mo["radiusX"].asNumber(0.4));
                m.radial.radiusY = static_cast<float>(mo["radiusY"].asNumber(0.4));
                m.radial.feather = static_cast<float>(mo["feather"].asNumber(0.5));
                m.radial.invert = mo["radialInvert"].asBool(false);
            } else if (m.type == MaskType::Brush) {
                if (mo.contains("brush") && mo["brush"].isArray()) {
                    for (const json::Value& dArr : mo["brush"].items()) {
                        if (dArr.isArray() && dArr.size() == 4) {
                            m.brush.push_back(BrushDab{
                                static_cast<float>(dArr[0].asNumber(0.0)),
                                static_cast<float>(dArr[1].asNumber(0.0)),
                                static_cast<float>(dArr[2].asNumber(0.1)),
                                static_cast<float>(dArr[3].asNumber(0.5))});
                        }
                    }
                }
            } else if (m.type == MaskType::ColorRange) {
                m.color.hasReference = mo["hasReference"].asBool(false);
                m.color.refR = static_cast<float>(mo["refR"].asNumber(0.0));
                m.color.refG = static_cast<float>(mo["refG"].asNumber(0.0));
                m.color.refB = static_cast<float>(mo["refB"].asNumber(0.0));
                m.color.hueTolerance = mo["hueTolerance"].asNumber(40.0);
                m.color.satTolerance = mo["satTolerance"].asNumber(0.5);
                m.color.lumaTolerance = mo["lumaTolerance"].asNumber(0.5);
                m.color.softness = mo["softness"].asNumber(0.5);
            }
            a.masks.push_back(std::move(m));
        }
    }

    // Point-colour targets.
    if (v.contains("colorTargets") && v["colorTargets"].isArray()) {
        for (const json::Value& tObj : v["colorTargets"].items()) {
            if (!tObj.isObject())
                continue;
            ColorTarget ct;
            ct.enabled = tObj["enabled"].asBool(false);
            ct.refR = static_cast<float>(tObj["refR"].asNumber(0.0));
            ct.refG = static_cast<float>(tObj["refG"].asNumber(0.0));
            ct.refB = static_cast<float>(tObj["refB"].asNumber(0.0));
            ct.hueShift = tObj["hueShift"].asNumber(0.0);
            ct.saturation = tObj["saturation"].asNumber(0.0);
            ct.luminance = tObj["luminance"].asNumber(0.0);
            ct.range = tObj["range"].asNumber(50.0);
            ct.smoothness = tObj["smoothness"].asNumber(50.0);
            ct.amount = tObj["amount"].asNumber(100.0);
            a.colorTargets.push_back(std::move(ct));
        }
    }

    // Spot removal / healing brush corrections. A missing key or a malformed
    // element keeps the default (empty list / per-field defaults).
    if (v.contains("spotRemovals") && v["spotRemovals"].isArray()) {
        for (const json::Value& so : v["spotRemovals"].items()) {
            if (!so.isObject())
                continue;
            SpotRemoval s;
            s.type = static_cast<SpotType>(
                std::clamp(so["type"].asNumber(0.0), 0.0, 1.0));
            s.sourceX = static_cast<float>(so["sourceX"].asNumber(0.0));
            s.sourceY = static_cast<float>(so["sourceY"].asNumber(0.0));
            s.targetX = static_cast<float>(so["targetX"].asNumber(0.0));
            s.targetY = static_cast<float>(so["targetY"].asNumber(0.0));
            s.radius = static_cast<float>(so["radius"].asNumber(0.1));
            s.visible = so["visible"].asBool(true);
            a.spotRemovals.push_back(std::move(s));
        }
    }

    // AI Denoise. Missing keys keep per-field defaults.
    if (v.contains("aiDenoise") && v["aiDenoise"].isObject()) {
        const auto& ad = v["aiDenoise"];
        a.aiDenoise.enabled = ad["enabled"].asBool(false);
        a.aiDenoise.strength = ad["strength"].asNumber(100.0);
        a.aiDenoise.mode = static_cast<AiDenoiseMode>(
            std::clamp(ad["mode"].asInt(0), 0, 1));
        a.aiDenoise.modelVersion = ad["modelVersion"].asInt(0);
    }

    // AI Mask parameters. Only applies when the adjustments list contains masks
    // of type AiMask; other masks are left untouched.
    if (v.contains("aiMasks") && v["aiMasks"].isArray()) {
        for (size_t i = 0; i < v["aiMasks"].size(); ++i) {
            const json::Value& ao = v["aiMasks"][i];
            if (!ao.isObject())
                continue;
            // Find the i-th AiMask in the existing list (masks may have been
            // pre-populated by the geometry loop above). We match by position
            // among AiMask entries to keep the round-trip stable.
            size_t targetIdx = 0;
            for (size_t j = 0; j < a.masks.size(); ++j) {
                if (a.masks[j].type == MaskType::AiMask) {
                    if (targetIdx == i) {
                        // Fill the AiMask params.
                        a.masks[j].ai.target = static_cast<AiMaskTarget>(
                            std::clamp(ao["aiTarget"].asInt(0), 0, 2));
                        a.masks[j].ai.threshold = ao["aiThreshold"].asNumber(0.5);
                        a.masks[j].ai.feather = ao["aiFeather"].asNumber(0.05);
                        a.masks[j].ai.invert = ao["aiInvert"].asBool(false);
                        a.masks[j].ai.modelInputSize = ao["aiModelInputSize"].asInt(1024);
                        a.masks[j].ai.cachedWeightVersion = ao["aiCachedWeightVersion"].asInt(0);
                        a.masks[j].ai.modelIdOverride = ao["aiModelIdOverride"].asString();
                        break;
                    }
                    ++targetIdx;
                }
            }
        }
    }

    return a;
}

bool ProfileIO::savePreset(const std::filesystem::path& path,
                           const Adjustments& adjustments) {
    const std::string text = toJson(adjustments).serialize(true);
    return writeFileAtomic(path, text);
}

std::optional<Adjustments> ProfileIO::loadPreset(
    const std::filesystem::path& path) {
    auto text = readAll(path);
    if (!text)
        return std::nullopt;
    auto value = json::Value::parse(*text);
    if (!value)
        return std::nullopt;
    return fromJson(*value);
}

} // namespace lrc
