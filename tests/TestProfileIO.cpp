#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/ZoneCurve.h"
#include "utils/ProfileIO.h"

using namespace Catch;
using namespace lrc;

namespace {

// Writes `text` to `path`, returning true on success.
bool writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

// Deletes the file on destruction (also when an assertion fails mid-test).
struct ScopedRemove {
    explicit ScopedRemove(std::filesystem::path p) : path(std::move(p)) {}
    ~ScopedRemove() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::filesystem::path path;
};

bool nearEq(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) <= tol;
}

// Point-wise curve comparison.  ProfileIO stores the curve points at full
// double precision, so the round-trip is exact up to float storage width.
bool curveApprox(const std::vector<ToneCurvePoint>& a,
                 const std::vector<ToneCurvePoint>& b, double tol = 1e-6) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(static_cast<double>(a[i].x) - b[i].x) > tol ||
            std::abs(static_cast<double>(a[i].y) - b[i].y) > tol)
            return false;
    }
    return true;
}

bool brushDabEq(const BrushDab& a, const BrushDab& b, double tol) {
    return nearEq(a.x, b.x, tol) && nearEq(a.y, b.y, tol) &&
           nearEq(a.radius, b.radius, tol) && nearEq(a.softness, b.softness, tol);
}

// Field-by-field comparison of a Mask, mirroring every field ProfileIO writes.
bool maskEq(const Mask& a, const Mask& b, double tol = 1e-6) {
    if (a.type != b.type || a.invert != b.invert)
        return false;
    if (!nearEq(a.opacity, b.opacity, tol))
        return false;
    if (!nearEq(a.exposure, b.exposure, tol) ||
        !nearEq(a.contrast, b.contrast, tol) ||
        !nearEq(a.highlights, b.highlights, tol) ||
        !nearEq(a.shadows, b.shadows, tol) ||
        !nearEq(a.whites, b.whites, tol) ||
        !nearEq(a.blacks, b.blacks, tol) ||
        !nearEq(a.clarity, b.clarity, tol) ||
        !nearEq(a.dehaze, b.dehaze, tol) ||
        !nearEq(a.saturation, b.saturation, tol) ||
        !nearEq(a.temperature, b.temperature, tol) ||
        !nearEq(a.tint, b.tint, tol))
        return false;
    if (!nearEq(a.linear().startX, b.linear().startX, tol) ||
        !nearEq(a.linear().startY, b.linear().startY, tol) ||
        !nearEq(a.linear().endX, b.linear().endX, tol) ||
        !nearEq(a.linear().endY, b.linear().endY, tol) ||
        !nearEq(a.linear().feather, b.linear().feather, tol))
        return false;
    if (!nearEq(a.radial().centerX, b.radial().centerX, tol) ||
        !nearEq(a.radial().centerY, b.radial().centerY, tol) ||
        !nearEq(a.radial().radiusX, b.radial().radiusX, tol) ||
        !nearEq(a.radial().radiusY, b.radial().radiusY, tol) ||
        !nearEq(a.radial().feather, b.radial().feather, tol) ||
        a.radial().invert != b.radial().invert)
        return false;
    if (a.brush().size() != b.brush().size())
        return false;
    for (size_t i = 0; i < a.brush().size(); ++i)
        if (!brushDabEq(a.brush()[i], b.brush()[i], tol))
            return false;
    return true;
}

// Field-by-field comparison of a ColorTarget, mirroring every field written.
bool colorTargetEq(const ColorTarget& a, const ColorTarget& b,
                   double tol = 1e-6) {
    if (a.enabled != b.enabled)
        return false;
    return nearEq(a.refR, b.refR, tol) && nearEq(a.refG, b.refG, tol) &&
           nearEq(a.refB, b.refB, tol) && nearEq(a.hueShift, b.hueShift, tol) &&
           nearEq(a.saturation, b.saturation, tol) &&
           nearEq(a.luminance, b.luminance, tol) &&
           nearEq(a.range, b.range, tol) &&
           nearEq(a.smoothness, b.smoothness, tol) &&
           nearEq(a.amount, b.amount, tol);
}

// Field-by-field comparison of a SpotRemoval, mirroring every field written.
bool spotRemovalEq(const SpotRemoval& a, const SpotRemoval& b,
                   double tol = 1e-6) {
    if (a.type != b.type || a.visible != b.visible)
        return false;
    return nearEq(a.sourceX, b.sourceX, tol) &&
           nearEq(a.sourceY, b.sourceY, tol) &&
           nearEq(a.targetX, b.targetX, tol) &&
           nearEq(a.targetY, b.targetY, tol) &&
           nearEq(a.radius, b.radius, tol);
}

}  // namespace

TEST_CASE("ProfileIO: full JSON round-trip covers every group", "[profileio]") {
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
    // Lens corrections.
    a.distortion = 15.0;
    a.vignette = -20.0;
    // Grain (all three fields, including non-default size/roughness).
    a.grain = 15.0;
    a.grainSize = 40.0;
    a.grainRoughness = 30.0;
    // Tone curve.
    a.curve = zonecurve::fromOutputs(0.2f, 0.4f, 0.6f, 0.8f);
    // All 8 HSL channels with some non-zero values.
    for (int ci = 0; ci < 8; ++ci) {
        a.hsl[ci].hue = -3.0 + ci * 2.0;
        a.hsl[ci].saturation = 4.0 * ci;
        a.hsl[ci].luminance = -2.0 * ci;
    }
    // Split toning.
    a.splitTone.highlightHue = 30.0;
    a.splitTone.highlightSaturation = 25.0;
    a.splitTone.shadowHue = 220.0;
    a.splitTone.shadowSaturation = 40.0;
    a.splitTone.balance = 12.0;

    // One linear, one radial and one brush mask, all with local adjustments.
    Mask linear;
    linear.type = MaskType::LinearGradient;
    linear.invert = false;
    linear.opacity = 0.8;
    linear.exposure = 0.4;
    linear.contrast = 10.0;
    linear.highlights = -5.0;
    linear.shadows = 7.0;
    linear.whites = 3.0;
    linear.blacks = -2.0;
    linear.clarity = 12.0;
    linear.dehaze = 4.0;
    linear.saturation = 8.0;
    linear.temperature = 6.0;
    linear.tint = -3.0;
    linear.linear().startX = 0.1f;
    linear.linear().startY = 0.2f;
    linear.linear().endX = 0.8f;
    linear.linear().endY = 0.6f;
    linear.linear().feather = 0.3f;
    a.masks.push_back(linear);

    Mask radial;
    radial.type = MaskType::RadialGradient;
    radial.opacity = 0.6;
    radial.exposure = -0.3;
    radial.radial().centerX = 0.5f;
    radial.radial().centerY = 0.4f;
    radial.radial().radiusX = 0.3f;
    radial.radial().radiusY = 0.2f;
    radial.radial().feather = 0.6f;
    radial.radial().invert = true;
    a.masks.push_back(radial);

    Mask brush;
    brush.type = MaskType::Brush;
    brush.opacity = 0.5;
    brush.saturation = 15.0;
    brush.brush().push_back(BrushDab{0.2f, 0.3f, 0.1f, 0.5f});
    brush.brush().push_back(BrushDab{0.7f, 0.8f, 0.15f, 0.4f});
    a.masks.push_back(brush);

    // Two point-colour targets.
    ColorTarget ct1;
    ct1.enabled = true;
    ct1.refR = 0.3f;
    ct1.refG = 0.5f;
    ct1.refB = 0.7f;
    ct1.hueShift = 15.0;
    ct1.saturation = -20.0;
    ct1.luminance = 10.0;
    ct1.range = 40.0;
    ct1.smoothness = 60.0;
    ct1.amount = 80.0;
    a.colorTargets.push_back(ct1);

    ColorTarget ct2;
    ct2.enabled = false;
    ct2.refR = 0.9f;
    ct2.refG = 0.1f;
    ct2.refB = 0.4f;
    ct2.hueShift = -8.0;
    ct2.saturation = 5.0;
    ct2.luminance = -3.0;
    ct2.range = 70.0;
    ct2.smoothness = 30.0;
    ct2.amount = 100.0;
    a.colorTargets.push_back(ct2);

    const json::Value json = ProfileIO::toJson(a);
    const Adjustments r = ProfileIO::fromJson(json);

    // --- Scalar groups, field by field (Approx 1e-6). ----------------------
    REQUIRE(r.exposure == Approx(a.exposure).margin(1e-6));
    REQUIRE(r.contrast == Approx(a.contrast).margin(1e-6));
    REQUIRE(r.highlights == Approx(a.highlights).margin(1e-6));
    REQUIRE(r.shadows == Approx(a.shadows).margin(1e-6));
    REQUIRE(r.whites == Approx(a.whites).margin(1e-6));
    REQUIRE(r.blacks == Approx(a.blacks).margin(1e-6));
    REQUIRE(r.temperature == Approx(a.temperature).margin(1e-6));
    REQUIRE(r.tint == Approx(a.tint).margin(1e-6));
    REQUIRE(r.saturation == Approx(a.saturation).margin(1e-6));

    REQUIRE(r.texture == Approx(a.texture).margin(1e-6));
    REQUIRE(r.clarity == Approx(a.clarity).margin(1e-6));
    REQUIRE(r.dehaze == Approx(a.dehaze).margin(1e-6));
    REQUIRE(r.vibrance == Approx(a.vibrance).margin(1e-6));

    REQUIRE(r.sharpness == Approx(a.sharpness).margin(1e-6));
    REQUIRE(r.sharpnessRadius == Approx(a.sharpnessRadius).margin(1e-6));
    REQUIRE(r.sharpnessDetail == Approx(a.sharpnessDetail).margin(1e-6));
    REQUIRE(r.sharpnessMasking == Approx(a.sharpnessMasking).margin(1e-6));

    REQUIRE(r.luminance == Approx(a.luminance).margin(1e-6));
    REQUIRE(r.luminanceDetail == Approx(a.luminanceDetail).margin(1e-6));
    REQUIRE(r.luminanceContrast == Approx(a.luminanceContrast).margin(1e-6));
    REQUIRE(r.colorNoiseReduction == Approx(a.colorNoiseReduction).margin(1e-6));
    REQUIRE(r.colorNoiseReductionSmoothness ==
            Approx(a.colorNoiseReductionSmoothness).margin(1e-6));

    REQUIRE(r.distortion == Approx(a.distortion).margin(1e-6));
    REQUIRE(r.vignette == Approx(a.vignette).margin(1e-6));

    REQUIRE(r.grain == Approx(a.grain).margin(1e-6));
    REQUIRE(r.grainSize == Approx(a.grainSize).margin(1e-6));
    REQUIRE(r.grainRoughness == Approx(a.grainRoughness).margin(1e-6));

    REQUIRE(curveApprox(r.curve, a.curve, 1e-6));

    for (int ci = 0; ci < 8; ++ci) {
        const auto& lhs = r.hsl[ci];
        const auto& rhs = a.hsl[ci];
        REQUIRE(lhs.hue == Approx(rhs.hue).margin(1e-6));
        REQUIRE(lhs.saturation == Approx(rhs.saturation).margin(1e-6));
        REQUIRE(lhs.luminance == Approx(rhs.luminance).margin(1e-6));
    }

    REQUIRE(r.splitTone.highlightHue == Approx(a.splitTone.highlightHue).margin(1e-6));
    REQUIRE(r.splitTone.highlightSaturation ==
            Approx(a.splitTone.highlightSaturation).margin(1e-6));
    REQUIRE(r.splitTone.shadowHue == Approx(a.splitTone.shadowHue).margin(1e-6));
    REQUIRE(r.splitTone.shadowSaturation ==
            Approx(a.splitTone.shadowSaturation).margin(1e-6));
    REQUIRE(r.splitTone.balance == Approx(a.splitTone.balance).margin(1e-6));

    // --- Masks / colour targets, field by field. ---------------------------
    REQUIRE(r.masks.size() == 3);
    REQUIRE(maskEq(r.masks[0], linear));
    REQUIRE(maskEq(r.masks[1], radial));
    REQUIRE(maskEq(r.masks[2], brush));

    REQUIRE(r.colorTargets.size() == 2);
    REQUIRE(colorTargetEq(r.colorTargets[0], ct1));
    REQUIRE(colorTargetEq(r.colorTargets[1], ct2));

    // Exact round-trip: JSON stores full double precision, so operator== holds.
    REQUIRE(r == a);
}

TEST_CASE("ProfileIO: SpotRemoval round-trip and missing-key tolerance",
          "[profileio]") {
    // A Clone (visible) and a Heal (hidden) survive a toJson -> fromJson
    // round-trip field by field.
    Adjustments a;
    SpotRemoval clone;
    clone.type = SpotType::Clone;
    clone.sourceX = 0.2f;
    clone.sourceY = 0.3f;
    clone.targetX = 0.7f;
    clone.targetY = 0.6f;
    clone.radius = 0.15f;
    clone.visible = true;
    a.spotRemovals.push_back(clone);

    SpotRemoval heal;
    heal.type = SpotType::Heal;
    heal.sourceX = 0.1f;
    heal.sourceY = 0.9f;
    heal.targetX = 0.4f;
    heal.targetY = 0.4f;
    heal.radius = 0.2f;
    heal.visible = false;
    a.spotRemovals.push_back(heal);

    const json::Value roundTrip = ProfileIO::toJson(a);
    const Adjustments r = ProfileIO::fromJson(roundTrip);

    REQUIRE(r.spotRemovals.size() == 2);
    REQUIRE(spotRemovalEq(r.spotRemovals[0], clone));
    REQUIRE(spotRemovalEq(r.spotRemovals[1], heal));
    REQUIRE(r == a);

    // A JSON payload without the spotRemovals key keeps the list empty and the
    // rest of the fields intact (backward compatibility with old presets).
    const auto legacy = json::Value::parse(R"({"exposure":0.5,"clarity":10})");
    REQUIRE(legacy.has_value());
    const Adjustments legacyA = ProfileIO::fromJson(*legacy);
    REQUIRE(legacyA.spotRemovals.empty());
    REQUIRE(legacyA.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(legacyA.clarity == Approx(10.0).margin(1e-6));

    // A default object also yields an empty spotRemovals (already covered by
    // "empty/default object yields defaults", asserted here for locality).
    REQUIRE(ProfileIO::fromJson(json::Value::object()) == Adjustments::defaults());
}

TEST_CASE("ProfileIO: version key tolerance", "[profileio]") {
    // A large unknown version number still parses and applies the fields.
    const auto v999 = json::Value::parse(R"({"version":999,"exposure":0.5,"contrast":25})");
    REQUIRE(v999.has_value());
    const auto a = ProfileIO::fromJson(*v999);
    REQUIRE(a.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(a.contrast == Approx(25.0).margin(1e-6));

    // No version key at all also parses.
    const auto noVersion = json::Value::parse(R"({"exposure":-0.75})");
    REQUIRE(noVersion.has_value());
    REQUIRE(ProfileIO::fromJson(*noVersion).exposure == Approx(-0.75).margin(1e-6));

    // A non-numeric version (string) must not crash; fields still apply.
    const auto badVersion = json::Value::parse(R"({"version":"abc","clarity":10})");
    REQUIRE(badVersion.has_value());
    REQUIRE(ProfileIO::fromJson(*badVersion).clarity == Approx(10.0).margin(1e-6));
}

TEST_CASE("ProfileIO: unknown keys are ignored, known fields still apply",
          "[profileio]") {
    const auto v = json::Value::parse(
        R"({"bogus":123,"colorTargets2":[],"exposure":0.7,"shadows":-15})");
    REQUIRE(v.has_value());
    const auto a = ProfileIO::fromJson(*v);
    REQUIRE(a.exposure == Approx(0.7).margin(1e-6));
    REQUIRE(a.shadows == Approx(-15.0).margin(1e-6));
    REQUIRE(a.colorTargets.empty());
    REQUIRE(a.masks.empty());
    REQUIRE(a.curve.empty());
}

TEST_CASE("ProfileIO: empty/default object yields defaults", "[profileio]") {
    // An empty JSON object keeps every field at its default.
    REQUIRE(ProfileIO::fromJson(json::Value::object()) == Adjustments::defaults());
    // A non-object (null) value also yields defaults instead of crashing.
    REQUIRE(ProfileIO::fromJson(json::Value{}) == Adjustments::defaults());
}

TEST_CASE("ProfileIO: savePreset/loadPreset file round-trip", "[profileio]") {
    const auto dir = std::filesystem::temp_directory_path();

    Adjustments a;
    a.exposure = 0.5;
    a.contrast = 25.0;
    a.curve = zonecurve::fromOutputs(0.2f, 0.4f, 0.6f, 0.8f);
    a.hsl[static_cast<size_t>(ColorRange::Green)].hue = 8.0;
    a.splitTone.shadowSaturation = 30.0;
    a.masks.push_back(Mask{});  // a default mask round-trips too

    const auto path = dir / "lrc_profileio_roundtrip.lrp";
    ScopedRemove cleanup(path);

    REQUIRE(ProfileIO::savePreset(path, a));
    REQUIRE(std::filesystem::exists(path));

    const auto loaded = ProfileIO::loadPreset(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->exposure == Approx(0.5).margin(1e-6));
    REQUIRE(loaded->contrast == Approx(25.0).margin(1e-6));
    REQUIRE(curveApprox(loaded->curve, a.curve));
    REQUIRE(loaded->hsl[static_cast<size_t>(ColorRange::Green)].hue ==
            Approx(8.0).margin(1e-6));
    REQUIRE(loaded->splitTone.shadowSaturation == Approx(30.0).margin(1e-6));
    REQUIRE(loaded->masks.size() == 1);
    REQUIRE(*loaded == a);
}

TEST_CASE("ProfileIO: loadPreset returns nullopt for a corrupted file",
          "[profileio]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto path = dir / "lrc_profileio_corrupt.lrp";
    ScopedRemove cleanup(path);
    REQUIRE(writeFile(path, "this is not json {"));

    REQUIRE_FALSE(ProfileIO::loadPreset(path).has_value());
}

TEST_CASE("ProfileIO: ColorTarget hashing distinguishes field differences",
          "[profileio]") {
    ColorTarget a;
    a.enabled = true;
    a.refR = 0.3f;
    a.refG = 0.5f;
    a.refB = 0.7f;
    a.hueShift = 15.0;
    a.saturation = -20.0;
    a.luminance = 10.0;
    a.range = 40.0;
    a.smoothness = 60.0;
    a.amount = 80.0;

    // Two equal targets hash identically.
    ColorTarget equal = a;
    REQUIRE(a.hash() == equal.hash());

    // Differing only in refR, range or amount must change the hash.
    ColorTarget diffRef = a;
    diffRef.refR = 0.9f;
    REQUIRE(a.hash() != diffRef.hash());

    ColorTarget diffRange = a;
    diffRange.range = 90.0;
    REQUIRE(a.hash() != diffRange.hash());

    ColorTarget diffAmount = a;
    diffAmount.amount = 20.0;
    REQUIRE(a.hash() != diffAmount.hash());
}

TEST_CASE("ProfileIO: Mask hashing distinguishes geometry differences",
          "[profileio]") {
    Mask a;
    a.type = MaskType::LinearGradient;
    a.linear().startX = 0.1f;
    a.linear().startY = 0.2f;
    a.linear().endX = 0.8f;
    a.linear().endY = 0.6f;
    a.linear().feather = 0.3f;
    a.opacity = 0.8;
    a.exposure = 0.4;
    a.clarity = 12.0;

    // Two equal masks hash identically.
    Mask equal = a;
    REQUIRE(a.hash() == equal.hash());

    // Differing geometry (start point / feather) must change the hash.
    Mask diffStart = a;
    diffStart.linear().startX = 0.5f;
    REQUIRE(a.hash() != diffStart.hash());

    Mask diffFeather = a;
    diffFeather.linear().feather = 0.9f;
    REQUIRE(a.hash() != diffFeather.hash());

    // A brush dab difference also changes the hash.
    Mask brushA;
    brushA.type = MaskType::Brush;
    brushA.brush().push_back(BrushDab{0.2f, 0.3f, 0.1f, 0.5f});
    Mask brushB = brushA;
    brushB.brush()[0].x = 0.9f;
    REQUIRE(brushA.hash() != brushB.hash());
}

TEST_CASE("ProfileIO: Adjustments::hash incorporates masks and colour targets",
          "[profileio]") {
    Adjustments base;
    base.exposure = 0.5;

    // Adding only a colour target must change the stack hash.
    Adjustments withTarget = base;
    ColorTarget ct;
    ct.enabled = true;
    ct.refR = 0.2f;
    withTarget.colorTargets.push_back(ct);
    REQUIRE(base.hash() != withTarget.hash());

    // Adding only a mask must change the stack hash.
    Adjustments withMask = base;
    Mask m;
    m.type = MaskType::RadialGradient;
    m.opacity = 0.7;
    withMask.masks.push_back(m);
    REQUIRE(base.hash() != withMask.hash());

    // The reverse check: identical stacks hash identically.
    Adjustments a1 = base;
    a1.colorTargets.push_back(ct);
    Adjustments a2 = base;
    a2.colorTargets.push_back(ct);
    REQUIRE(a1.hash() == a2.hash());
}
