#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "engine/ZoneCurve.h"
#include "engine/Xmp.h"
#include "utils/XmpProfile.h"

using namespace Catch;
using namespace lrc;

namespace {

// Point-wise curve comparison.  Import quantizes through zonecurve::fromPoints
// after lround'ing to 0..255, so a small tolerance absorbs that rounding.
bool curveApprox(const std::vector<ToneCurvePoint>& a,
                 const std::vector<ToneCurvePoint>& b, double tol = 1e-2) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<double>(a[i].x) - b[i].x) > tol ||
            std::abs(static_cast<double>(a[i].y) - b[i].y) > tol)
            return false;
    }
    return true;
}

}  // namespace

TEST_CASE("XmpProfile: export -> import round-trips every group", "[xmpprofile]") {
    Adjustments a;
    // Basic panel.
    a.exposure = 0.5;
    a.contrast = 25.0;
    a.highlights = -40.0;
    a.shadows = 20.0;
    a.whites = 15.0;
    a.blacks = -10.0;
    a.temperature = 74.0;
    a.tint = 20.0;
    a.saturation = 30.0;
    // Effects.
    a.texture = 12.0;
    a.clarity = 30.0;
    a.dehaze = -8.0;
    a.vibrance = 5.0;
    // Detail.
    a.sharpness = 40.0;
    a.sharpnessRadius = 1.5;
    a.sharpnessDetail = 25.0;
    a.sharpnessMasking = 30.0;
    // Detail: noise reduction.
    a.luminance = 45.0;
    a.luminanceDetail = 65.0;
    a.luminanceContrast = 20.0;
    a.colorNoiseReduction = 35.0;
    a.colorNoiseReductionSmoothness = 70.0;
    // Vignette.
    a.vignette = -20.0;
    // Grain.
    a.grain = 15.0;
    a.grainSize = 40.0;
    a.grainRoughness = 30.0;
    // Tone curve.
    a.curve = zonecurve::fromOutputs(0.2f, 0.4f, 0.6f, 0.8f);
    // HSL (a few channels, including the colour-range order).
    a.hsl[static_cast<size_t>(ColorRange::Red)].hue = 10.0;
    a.hsl[static_cast<size_t>(ColorRange::Red)].saturation = -15.0;
    a.hsl[static_cast<size_t>(ColorRange::Blue)].luminance = 20.0;
    a.hsl[static_cast<size_t>(ColorRange::Magenta)].hue = -5.0;
    // Split toning.
    a.splitTone.highlightHue = 30.0;
    a.splitTone.highlightSaturation = 25.0;
    a.splitTone.shadowHue = 220.0;
    a.splitTone.shadowSaturation = 40.0;
    a.splitTone.balance = 12.0;

    const auto attrs = XmpProfile::toCrsAttrs(a);
    const Adjustments r = XmpProfile::fromCrsAttrs(attrs);

    // Scalar fields (1e-6 absolute tolerance).
    REQUIRE(r.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(r.contrast == Approx(25.0).margin(1e-6));
    REQUIRE(r.highlights == Approx(-40.0).margin(1e-6));
    REQUIRE(r.shadows == Approx(20.0).margin(1e-6));
    REQUIRE(r.whites == Approx(15.0).margin(1e-6));
    REQUIRE(r.blacks == Approx(-10.0).margin(1e-6));
    REQUIRE(r.tint == Approx(20.0).margin(1e-6));
    REQUIRE(r.saturation == Approx(30.0).margin(1e-6));

    REQUIRE(r.texture == Approx(12.0).margin(1e-6));
    REQUIRE(r.clarity == Approx(30.0).margin(1e-6));
    REQUIRE(r.dehaze == Approx(-8.0).margin(1e-6));
    REQUIRE(r.vibrance == Approx(5.0).margin(1e-6));

    REQUIRE(r.sharpness == Approx(40.0).margin(1e-6));
    REQUIRE(r.sharpnessRadius == Approx(1.5).margin(1e-6));
    REQUIRE(r.sharpnessDetail == Approx(25.0).margin(1e-6));
    REQUIRE(r.sharpnessMasking == Approx(30.0).margin(1e-6));

    REQUIRE(r.luminance == Approx(45.0).margin(1e-6));
    REQUIRE(r.luminanceDetail == Approx(65.0).margin(1e-6));
    REQUIRE(r.luminanceContrast == Approx(20.0).margin(1e-6));
    REQUIRE(r.colorNoiseReduction == Approx(35.0).margin(1e-6));
    REQUIRE(r.colorNoiseReductionSmoothness == Approx(70.0).margin(1e-6));

    // Vignette (amount + shape + style).
    REQUIRE(r.vignette == Approx(-20.0).margin(1e-6));
    REQUIRE(r.vignetteMidpoint == Approx(50.0).margin(1e-6));
    REQUIRE(r.vignetteRoundness == Approx(0.0).margin(1e-6));
    REQUIRE(r.vignetteFeather == Approx(50.0).margin(1e-6));
    REQUIRE(r.vignetteHighlights == Approx(0.0).margin(1e-6));
    REQUIRE(r.vignetteStyle == 2);   // Highlight Priority default

    REQUIRE(r.grain == Approx(15.0).margin(1e-6));
    REQUIRE(r.grainSize == Approx(40.0).margin(1e-6));
    REQUIRE(r.grainRoughness == Approx(30.0).margin(1e-6));

    // Temperature round-trips through an integer kelvin, allow +/-1.0.
    REQUIRE(std::abs(r.temperature - 74.0) <= 1.0);

    REQUIRE(curveApprox(r.curve, a.curve));

    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Red)].hue ==
            Approx(10.0).margin(1e-6));
    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Red)].saturation ==
            Approx(-15.0).margin(1e-6));
    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Blue)].luminance ==
            Approx(20.0).margin(1e-6));
    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Magenta)].hue ==
            Approx(-5.0).margin(1e-6));
    // Untouched channels stay at default.
    REQUIRE(r.hsl[0].luminance == 0.0);
    REQUIRE(r.hsl[1].hue == 0.0);

    REQUIRE(r.splitTone.highlightHue == Approx(30.0).margin(1e-6));
    REQUIRE(r.splitTone.highlightSaturation == Approx(25.0).margin(1e-6));
    REQUIRE(r.splitTone.shadowHue == Approx(220.0).margin(1e-6));
    REQUIRE(r.splitTone.shadowSaturation == Approx(40.0).margin(1e-6));
    REQUIRE(r.splitTone.balance == Approx(12.0).margin(1e-6));

    // Non-interoperable fields (masks / colour targets / distortion) are
    // never emitted by export.
    REQUIRE(attrs.find("Distortion") == attrs.end());
    REQUIRE(attrs.find("PostCropVignetteAmount") != attrs.end());
}

TEST_CASE("XmpProfile: unknown crs keys are skipped without crashing",
          "[xmpprofile]") {
    std::map<std::string, std::string> attrs;
    attrs["Exposure2012"] = "+0.50";
    attrs["Contrast2012"] = "+25";
    // v1-unsupported crs attributes (mask, channel curve, parametric, point
    // colour) plus a made-up key and a foreign namespace tag.
    attrs["MaskGroupBasedCorrections"] = "rdf:li structure";
    attrs["ToneCurvePV2012Red"] = "0,0, 255,255";
    attrs["ColorAdjustmentRed"] = "0,0,0, 0";
    attrs["ParametricHighlights"] = "0";
    attrs["TotallyUnknownKey"] = "123";
    attrs["exif:UnknownTag"] = "shouldBeIgnored";

    const Adjustments r = XmpProfile::fromCrsAttrs(attrs);
    REQUIRE(r.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(r.contrast == Approx(25.0).margin(1e-6));
    // Recognized-but-unlisted keys must not leak into any field.
    REQUIRE(r.curve.empty());
    REQUIRE(r.highlights == 0.0);
    REQUIRE(r.temperature == 0.0);
    REQUIRE(r.hsl[0].hue == 0.0);
    REQUIRE(r.splitTone.highlightSaturation == 0.0);
}

TEST_CASE("XmpProfile: empty or missing map yields all defaults", "[xmpprofile]") {
    REQUIRE(XmpProfile::fromCrsAttrs({}) == Adjustments::defaults());

    std::map<std::string, std::string> onlyUnknown;
    onlyUnknown["Bogus"] = "1";
    onlyUnknown["crs:MaskWhat"] = "2";
    REQUIRE(XmpProfile::fromCrsAttrs(onlyUnknown) == Adjustments::defaults());
}

TEST_CASE("XmpProfile: out-of-range values are clamped on import", "[xmpprofile]") {
    std::map<std::string, std::string> attrs;
    attrs["Exposure2012"] = "99";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).exposure == Approx(5.0).margin(1e-6));

    attrs.clear();
    attrs["Highlights2012"] = "-500";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).highlights ==
            Approx(-100.0).margin(1e-6));

    attrs.clear();
    attrs["SharpenAmount"] = "999";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).sharpness ==
            Approx(150.0).margin(1e-6));

    attrs.clear();
    attrs["SharpenRadius"] = "100";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).sharpnessRadius ==
            Approx(3.0).margin(1e-6));

    attrs.clear();
    attrs["Tint"] = "500";  // 500 / 1.5 = 333.3 -> clamped
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).tint == Approx(100.0).margin(1e-6));

    attrs.clear();
    attrs["GrainSize"] = "-5";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).grainSize == Approx(0.0).margin(1e-6));

    attrs.clear();
    attrs["Temperature"] = "60000";  // clamp to 50000 K -> coldest offset
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).temperature ==
            Approx(-100.0).margin(1.0));

    attrs.clear();
    attrs["Temperature"] = "1000";  // clamp to 2000 K -> warmest offset
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).temperature ==
            Approx(100.0).margin(1.0));

    attrs.clear();
    attrs["SplitToningHighlightHue"] = "500";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).splitTone.highlightHue ==
            Approx(360.0).margin(1e-6));

    attrs.clear();
    attrs["RedSaturation"] = "999";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).hsl[0].saturation ==
            Approx(100.0).margin(1e-6));

    attrs.clear();
    attrs["LuminanceSmoothing"] = "200";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).luminance ==
            Approx(100.0).margin(1e-6));

    attrs.clear();
    attrs["ColorNoiseReduction"] = "-5";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).colorNoiseReduction ==
            Approx(0.0).margin(1e-6));

    // Unparseable values keep the field default instead of crashing.
    attrs.clear();
    attrs["Exposure2012"] = "not-a-number";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).exposure == 0.0);
}

TEST_CASE("XmpProfile: tone-curve import (identity empty, S-curve quantized)",
          "[xmpprofile]") {
    std::map<std::string, std::string> attrs;

    // Lightroom's linear default is identity -> stays empty.
    attrs["ToneCurvePV2012"] = "0,0, 32,32, 64,64, 128,128, 192,192, 255,255";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).curve.empty());

    // A two-point identity is also linear.
    attrs["ToneCurvePV2012"] = "0, 0, 255, 255";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).curve.empty());

    // The golden S-curve becomes a non-empty 6-point zone curve.
    attrs["ToneCurvePV2012"] =
        "0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255";
    const Adjustments r = XmpProfile::fromCrsAttrs(attrs);
    REQUIRE(!r.curve.empty());
    REQUIRE(r.curve.size() == zonecurve::kZoneCount + 2);
    REQUIRE(r.curve.front().x == Approx(0.0));
    REQUIRE(r.curve.back().x == Approx(1.0));

    // Malformed curve strings import as an empty (identity) curve.
    attrs["ToneCurvePV2012"] = "";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).curve.empty());
    attrs["ToneCurvePV2012"] = "0,0, 128";  // odd token count
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).curve.empty());
    attrs["ToneCurvePV2012"] = "abc";
    REQUIRE(XmpProfile::fromCrsAttrs(attrs).curve.empty());
}

TEST_CASE("XmpProfile: golden import values from XMP-MAPPING 8.1", "[xmpprofile]") {
    std::map<std::string, std::string> attrs;
    attrs["Exposure2012"] = "+0.50";
    attrs["Contrast2012"] = "+25";
    attrs["Highlights2012"] = "-40";
    attrs["Temperature"] = "4000";
    attrs["Tint"] = "+10";
    attrs["Clarity2012"] = "+30";
    attrs["ToneCurvePV2012"] =
        "0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255";
    attrs["exif:UnknownTag"] = "shouldBeIgnored";

    const Adjustments r = XmpProfile::fromCrsAttrs(attrs);
    REQUIRE(r.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(r.contrast == Approx(25.0).margin(1e-6));
    REQUIRE(r.highlights == Approx(-40.0).margin(1e-6));
    REQUIRE(r.clarity == Approx(30.0).margin(1e-6));
    REQUIRE(std::abs(r.temperature - 74.0) <= 1.0);       // 4000 K -> +73.96
    REQUIRE(r.tint == Approx(6.667).margin(1e-3));         // 10 / 1.5
    REQUIRE(!r.curve.empty());
    // Everything not listed stays default.
    REQUIRE(r.shadows == 0.0);
    REQUIRE(r.whites == 0.0);
    REQUIRE(r.vibrance == 0.0);
}

TEST_CASE("XmpProfile: export formatting and non-interoperable fields",
          "[xmpprofile]") {
    Adjustments a;
    a.exposure = 0.5;
    a.temperature = 74.0;
    a.tint = 20.0;
    a.clarity = 30.0;
    a.hsl[static_cast<size_t>(ColorRange::Red)].saturation = 10.0;
    a.splitTone.highlightSaturation = 25.0;
    a.distortion = 30.0;  // no standard crs attribute -> must not be emitted
    a.masks.push_back(Mask{});
    a.colorTargets.push_back(ColorTarget{});

    const auto m = XmpProfile::toCrsAttrs(a);
    REQUIRE(m.at("Exposure2012") == "0.5");          // shortest to_chars form
    REQUIRE(m.at("Clarity2012") == "30");             // whole doubles, no ".0"
    REQUIRE(m.at("Tint") == "30");                    // 20 * 1.5, no forced '+'
    REQUIRE(m.at("RedSaturation") == "10");
    REQUIRE(m.at("SplitToningHighlightSaturation") == "25");
    REQUIRE(m.at("ToneCurveName2012") == "Linear");   // always emitted

    // Masks, colour targets and distortion never surface in the export.
    REQUIRE(m.count("PostCropVignetteAmount") == 0);
    REQUIRE(m.count("Distortion") == 0);
    for (const auto& kv : m)
        REQUIRE(kv.first.rfind("Mask", 0) != 0);

    // Temperature is exported as an integer kelvin string.
    Adjustments b;
    b.temperature = 74.0;
    const auto mt = XmpProfile::toCrsAttrs(b);
    const double kelvin = std::strtod(mt.at("Temperature").c_str(), nullptr);
    REQUIRE(kelvin > 3000.0);
    REQUIRE(kelvin < 5000.0);

    // A fully-default Adjustments exports only the always-on ToneCurveName2012.
    const auto md = XmpProfile::toCrsAttrs(Adjustments::defaults());
    REQUIRE(md.size() == 1);
    REQUIRE(md.at("ToneCurveName2012") == "Linear");
}

TEST_CASE("XmpProfile: spot corrections round-trip via crs:SpotCorrections",
          "[xmpprofile]") {
    Adjustments a;
    SpotRemoval clone;
    clone.type = SpotType::Clone;
    clone.sourceX = 0.10f;
    clone.sourceY = 0.20f;
    clone.targetX = 0.30f;
    clone.targetY = 0.40f;
    clone.radius = 0.05f;
    clone.visible = true;
    a.spotRemovals.push_back(clone);

    SpotRemoval heal;
    heal.type = SpotType::Heal;
    heal.sourceX = 0.60f;
    heal.sourceY = 0.70f;
    heal.targetX = 0.80f;
    heal.targetY = 0.90f;
    heal.radius = 0.10f;
    heal.visible = false;
    a.spotRemovals.push_back(heal);

    // Mapping entry point: Adjustments -> string-valued XML representation.
    std::vector<XmpDocument::SpotCorrectionXml> xml;
    XmpProfile::toSpotCorrectionXml(a.spotRemovals, xml);
    REQUIRE(xml.size() == 2);
    REQUIRE(xml[0].type == "Clone");
    REQUIRE(xml[0].active == "true");
    REQUIRE(xml[1].type == "Heal");
    REQUIRE(xml[1].active == "false");

    // The XML layer must survive a full serialize -> parse round trip.
    const auto attrs = XmpProfile::toCrsAttrs(a);
    const std::string doc = XmpDocument::serializeFull(attrs, "", nullptr, &xml);
    std::vector<XmpDocument::SpotCorrectionXml> xml2;
    REQUIRE(XmpDocument::parseSpotCorrections(doc, xml2));
    REQUIRE(xml2.size() == 2);

    // Back to Adjustments; every field must round-trip (1e-6 tolerance).
    const auto r = XmpProfile::fromSpotCorrectionXml(xml2);
    REQUIRE(r.size() == 2);
    REQUIRE(r[0].type == SpotType::Clone);
    REQUIRE(r[0].sourceX == Approx(0.10).margin(1e-6));
    REQUIRE(r[0].sourceY == Approx(0.20).margin(1e-6));
    REQUIRE(r[0].targetX == Approx(0.30).margin(1e-6));
    REQUIRE(r[0].targetY == Approx(0.40).margin(1e-6));
    REQUIRE(r[0].radius == Approx(0.05).margin(1e-6));
    REQUIRE(r[0].visible);
    REQUIRE(r[1].type == SpotType::Heal);
    REQUIRE(r[1].sourceX == Approx(0.60).margin(1e-6));
    REQUIRE(r[1].targetX == Approx(0.80).margin(1e-6));
    REQUIRE(r[1].targetY == Approx(0.90).margin(1e-6));
    REQUIRE(r[1].radius == Approx(0.10).margin(1e-6));
    REQUIRE_FALSE(r[1].visible);
}

TEST_CASE("XmpProfile: unknown spot CorrectionType is tolerated", "[xmpprofile]") {
    std::vector<XmpDocument::SpotCorrectionXml> xml;
    XmpDocument::SpotCorrectionXml x;
    x.type = "MysteryMode";
    x.active = "true";
    x.centerX = "0.5";
    x.centerY = "0.5";
    x.radius = "0.1";
    xml.push_back(x);

    // Unknown type must not fail the import; it defaults to Clone and the
    // geometry is still parsed.
    const auto r = XmpProfile::fromSpotCorrectionXml(xml);
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].type == SpotType::Clone);
    REQUIRE(r[0].targetX == Approx(0.5).margin(1e-6));
    REQUIRE(r[0].targetY == Approx(0.5).margin(1e-6));
    REQUIRE(r[0].radius == Approx(0.1).margin(1e-6));
    REQUIRE(r[0].visible);

    // Explicit inactive value maps to visible=false.
    xml[0].active = "false";
    const auto r2 = XmpProfile::fromSpotCorrectionXml(xml);
    REQUIRE_FALSE(r2[0].visible);
}

TEST_CASE("XmpProfile: empty spot list emits no SpotCorrections element",
          "[xmpprofile]") {
    std::vector<XmpDocument::SpotCorrectionXml> xml;
    XmpProfile::toSpotCorrectionXml({}, xml);
    REQUIRE(xml.empty());

    const std::map<std::string, std::string> attrs = {
        {"ToneCurveName2012", "Linear"}};
    const std::string doc = XmpDocument::serializeFull(attrs, "", nullptr, &xml);
    REQUIRE(doc.find("SpotCorrections") == std::string::npos);
}

TEST_CASE("XmpProfile: PHASE_3 vignette style round-trips (Paint/Color/Highlight)",
          "[xmpprofile][phase3]") {
    for (int style = 0; style < 3; ++style) {
        Adjustments a;
        a.vignette = -20.0;
        a.vignetteMidpoint = 60.0;
        a.vignetteRoundness = 20.0;
        a.vignetteFeather = 70.0;
        a.vignetteHighlights = 10.0;
        a.vignetteStyle = style;

        const auto attrs = XmpProfile::toCrsAttrs(a);
        // Always-on fields exist:
        REQUIRE(attrs.count("PostCropVignetteAmount") == 1);
        REQUIRE(attrs.count("PostCropVignetteMidpoint") == 1);
        // Style 2 (Highlight Priority) is the default — it is omitted so
        // fully-default Adjustments keep a 1-key export. Styles 0 and 1 are
        // written explicitly because they diverge from the default.
        if (style == 0) {
            REQUIRE(attrs.count("PostCropVignetteStyle") == 1);
            REQUIRE(attrs.at("PostCropVignetteStyle") == "Paint Overlay");
        }
        if (style == 1) {
            REQUIRE(attrs.count("PostCropVignetteStyle") == 1);
            REQUIRE(attrs.at("PostCropVignetteStyle") == "Color Priority");
        }
        if (style == 2) {
            REQUIRE(attrs.count("PostCropVignetteStyle") == 0);
        }

        // Unknown style string (from a future LR) maps back to Highlight Priority (2).
        {
            auto copy = attrs;
            copy["PostCropVignetteStyle"] = "DeepSpace 9000";
            const Adjustments r = XmpProfile::fromCrsAttrs(copy);
            REQUIRE(r.vignetteStyle == 2);
        }

        // Real round-trip: all shape + style values preserved.
        const Adjustments r = XmpProfile::fromCrsAttrs(attrs);
        REQUIRE(r.vignetteStyle == style);
        REQUIRE(r.vignette == Approx(-20.0).margin(1e-6));
        REQUIRE(r.vignetteMidpoint == Approx(60.0).margin(1e-6));
        REQUIRE(r.vignetteRoundness == Approx(20.0).margin(1e-6));
        REQUIRE(r.vignetteFeather == Approx(70.0).margin(1e-6));
        REQUIRE(r.vignetteHighlights == Approx(10.0).margin(1e-6));
    }
}

TEST_CASE("XmpProfile: PHASE_4 standalone ColorRange mask round-trips",
          "[xmpprofile][phase4]") {
    Adjustments a;
    Mask crm;
    crm.type = MaskType::ColorRange;
    crm.color.hasReference = true;
    crm.color.refR = 0.75f;
    crm.color.refG = 0.25f;
    crm.color.refB = 0.10f;
    crm.color.hueTolerance = 60.0;
    crm.color.satTolerance = 0.6;
    crm.color.lumaTolerance = 0.4;
    crm.color.softness = 0.7;
    a.masks.push_back(crm);

    const auto attrs = XmpProfile::toCrsAttrs(a);
    REQUIRE(attrs.count("SH_ColorRangeMask_HasRef") == 1);
    REQUIRE(attrs.at("SH_ColorRangeMask_HasRef") == "true");
    // Floats: check values round-trip numerically rather than string-match,
    // because to_chars(double(static_cast<float>(x))) may preserve tail bits
    // of the IEEE-754 approximation (0.1f -> "0.10000000149...").
    auto asDouble = [](const std::string& s) { return std::strtod(s.c_str(), nullptr); };
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_RefR")) == Approx(0.75).margin(1e-4));
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_RefG")) == Approx(0.25).margin(1e-4));
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_RefB")) == Approx(0.10).margin(1e-4));
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_HueTol")) == Approx(60.0).margin(1e-4));
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_SatTol")) == Approx(0.6).margin(1e-4));
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_LumaTol")) == Approx(0.4).margin(1e-4));
    REQUIRE(asDouble(attrs.at("SH_ColorRangeMask_Softness")) == Approx(0.7).margin(1e-4));

    const Adjustments back = XmpProfile::fromCrsAttrs(attrs);
    REQUIRE(back.masks.size() >= 1);
    bool found = false;
    for (const Mask& m : back.masks) {
        if (m.type != MaskType::ColorRange) continue;
        found = true;
        REQUIRE(m.color.hasReference);
        REQUIRE(m.color.refR == Approx(0.75).margin(1e-6));
        REQUIRE(m.color.refG == Approx(0.25).margin(1e-6));
        REQUIRE(m.color.refB == Approx(0.10).margin(1e-6));
        REQUIRE(m.color.hueTolerance == Approx(60.0).margin(1e-6));
        REQUIRE(m.color.satTolerance == Approx(0.6).margin(1e-6));
        REQUIRE(m.color.lumaTolerance == Approx(0.4).margin(1e-6));
        REQUIRE(m.color.softness == Approx(0.7).margin(1e-6));
    }
    REQUIRE(found);
}

TEST_CASE("XmpProfile: PHASE_4 geometric mask sub-range modifiers round-trip",
          "[xmpprofile][phase4]") {
    Adjustments a;

    // Mask #0: linear gradient with both colour and luminance sub-masks,
    // Intersect mode (default).
    Mask g0;
    g0.type = MaskType::LinearGradient;
    g0.useColorRange = true;
    g0.colorRefR = 0.2f; g0.colorRefG = 0.6f; g0.colorRefB = 0.9f;
    g0.colorRange = 40.0f;
    g0.colorSmoothness = 55.0f;
    g0.useLuminance = true;
    g0.luminanceRef = 70.0f;
    g0.luminanceRange = 25.0f;
    g0.luminanceSmoothness = 60.0f;
    g0.maskUnion = false;
    a.masks.push_back(g0);

    // Mask #1: radial gradient with colour-only sub-mask, Union mode.
    Mask g1;
    g1.type = MaskType::RadialGradient;
    g1.useColorRange = true;
    g1.colorRefR = 0.95f; g1.colorRefG = 0.80f; g1.colorRefB = 0.10f;
    g1.colorRange = 80.0f;
    g1.colorSmoothness = 40.0f;
    g1.useLuminance = false;
    g1.maskUnion = true;
    a.masks.push_back(g1);

    const auto attrs = XmpProfile::toCrsAttrs(a);
    REQUIRE(attrs.count("SH_Mask0_UseColor") == 1);
    REQUIRE(attrs.at("SH_Mask0_UseColor") == "true");
    REQUIRE(attrs.at("SH_Mask0_UseLuma") == "true");
    REQUIRE(attrs.at("SH_Mask0_Combine") == "Intersect");
    REQUIRE(attrs.at("SH_Mask0_ColorRange") == "40");
    REQUIRE(attrs.at("SH_Mask0_LumaRange") == "25");
    REQUIRE(attrs.count("SH_Mask1_UseColor") == 1);
    REQUIRE(attrs.count("SH_Mask1_UseLuma") == 0);  // UseLuma=false -> not emitted
    REQUIRE(attrs.at("SH_Mask1_Combine") == "Union");
    REQUIRE(attrs.at("SH_Mask1_ColorRange") == "80");

    const Adjustments r = XmpProfile::fromCrsAttrs(attrs);
    REQUIRE(r.masks.size() >= 2);

    // Geometry types are intentionally not restored by the importer yet
    // (only range-modifier fields round-trip), but the field count and
    // values must match.
    const Mask& m0 = r.masks[0];
    REQUIRE(m0.useColorRange);
    REQUIRE(m0.useLuminance);
    REQUIRE_FALSE(m0.maskUnion);
    REQUIRE(m0.colorRefR == Approx(0.2).margin(1e-6));
    REQUIRE(m0.colorRefG == Approx(0.6).margin(1e-6));
    REQUIRE(m0.colorRefB == Approx(0.9).margin(1e-6));
    REQUIRE(m0.colorRange == Approx(40.0).margin(1e-6));
    REQUIRE(m0.colorSmoothness == Approx(55.0).margin(1e-6));
    REQUIRE(m0.luminanceRef == Approx(70.0).margin(1e-6));
    REQUIRE(m0.luminanceRange == Approx(25.0).margin(1e-6));
    REQUIRE(m0.luminanceSmoothness == Approx(60.0).margin(1e-6));

    const Mask& m1 = r.masks[1];
    REQUIRE(m1.useColorRange);
    REQUIRE_FALSE(m1.useLuminance);
    REQUIRE(m1.maskUnion);
    REQUIRE(m1.colorRefR == Approx(0.95).margin(1e-6));
    REQUIRE(m1.colorRefG == Approx(0.80).margin(1e-6));
    REQUIRE(m1.colorRefB == Approx(0.10).margin(1e-6));
    REQUIRE(m1.colorRange == Approx(80.0).margin(1e-6));
    REQUIRE(m1.colorSmoothness == Approx(40.0).margin(1e-6));
}

TEST_CASE("XmpProfile: PHASE_4 mask range out-of-range imports clamp",
          "[xmpprofile][phase4]") {
    std::map<std::string, std::string> attrs;
    attrs["SH_Mask0_UseColor"] = "true";
    attrs["SH_Mask0_ColorRefR"] = "100";      // > 1.0 -> clamp to 1.0
    attrs["SH_Mask0_ColorRange"] = "-500";    // clamp to 0
    attrs["SH_Mask0_ColorSmooth"] = "9999";   // clamp to 100
    attrs["SH_Mask0_UseLuma"] = "true";
    attrs["SH_Mask0_LumaRef"] = "500";        // clamp to 100
    attrs["SH_Mask0_LumaRange"] = "-5";       // clamp to 0
    attrs["SH_Mask0_LumaSmooth"] = "300";     // clamp to 100
    attrs["SH_Mask0_Combine"] = "Intersect";

    const Adjustments r = XmpProfile::fromCrsAttrs(attrs);
    REQUIRE(r.masks.size() >= 1);
    const Mask& m = r.masks[0];
    REQUIRE(m.useColorRange);
    REQUIRE(m.useLuminance);
    REQUIRE_FALSE(m.maskUnion);
    REQUIRE(m.colorRefR == Approx(1.0).margin(1e-6));
    REQUIRE(m.colorRange == Approx(0.0).margin(1e-6));
    REQUIRE(m.colorSmoothness == Approx(100.0).margin(1e-6));
    REQUIRE(m.luminanceRef == Approx(100.0).margin(1e-6));
    REQUIRE(m.luminanceRange == Approx(0.0).margin(1e-6));
    REQUIRE(m.luminanceSmoothness == Approx(100.0).margin(1e-6));
}
