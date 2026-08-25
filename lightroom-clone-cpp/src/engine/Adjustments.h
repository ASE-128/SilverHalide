#pragma once

// Non-destructive adjustment parameters. This is the "parameter stack" of the
// pipeline: it never mutates the decoded raw buffer, only describes how to
// render it. Adjustments are applied in linear light in a fixed order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lrc {

// A point on the tonal response curve. Coordinates are normalized to [0,1]
// against the linear signal before gamma encoding.
struct ToneCurvePoint {
    float x = 0.0f;
    float y = 0.0f;

    bool operator==(const ToneCurvePoint&) const = default;
};

// Eight colour ranges used by HSL individual channel controls,
// matching Lightroom's convention.
enum class ColorRange : uint8_t {
    Red = 0,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple,
    Magenta,
    Count  // sentinel — must be last
};

// Per-channel HSL correction. Each channel stores a relative offset for
// hue (degrees, -180..+180), saturation (-100..+100), and luminance (-100..+100).
struct HSLChannel {
    double hue = 0.0;        // -180..+180  (hue shift in degrees)
    double saturation = 0.0; // -100..+100
    double luminance = 0.0;  // -100..+100

    bool operator==(const HSLChannel& o) const = default;
};

// Split toning: separate colour tinting for highlights and shadows.
struct SplitTone {
    double highlightHue = 0.0;        // 0..360  (hue angle)
    double highlightSaturation = 0.0; // 0..100
    double shadowHue = 0.0;          // 0..360
    double shadowSaturation = 0.0;   // 0..100
    double balance = 0.0;            // -100..+100  (shifts the midpoint)

    bool operator==(const SplitTone& o) const = default;
};

// --- Local (masked) adjustments ----------------------------------------------
// Lightroom-style masks: a mask defines a per-pixel weight in [0,1] and a set of
// local adjustments that are applied only where the weight is non-zero. Multiple
// masks stack on top of the globally-adjusted image.

enum class MaskType : uint8_t {
    None = 0,
    LinearGradient,
    RadialGradient,
    Brush,
    ColorRange,
};

// Linear gradient mask. The masked region is the infinite strip between the two
// parallel lines through `start` and `end`; `feather` softens both strip edges.
// Coordinates are normalized to [0,1] of the image.
struct LinearGradientMask {
    float startX = 0.0f;
    float startY = 0.0f;
    float endX = 1.0f;
    float endY = 0.0f;
    float feather = 0.5f; // 0..1  softness of the strip edges

    bool operator==(const LinearGradientMask&) const = default;
};

// Radial gradient mask: an elliptical region. Coordinates normalized to [0,1].
struct RadialGradientMask {
    float centerX = 0.5f;
    float centerY = 0.5f;
    float radiusX = 0.4f;
    float radiusY = 0.4f;
    float feather = 0.5f;  // 0..1  softness of the elliptical edge
    bool invert = false;

    bool operator==(const RadialGradientMask&) const = default;
};

// A single brush dab: a soft-edged circle. Coordinates normalized to [0,1];
// `radius` is normalized against the shorter image dimension.
struct BrushDab {
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.1f;
    float softness = 0.5f; // 0..1

    bool operator==(const BrushDab&) const = default;
};

// Colour-range mask: a pixel weight based on HSL distance to a sampled
// reference colour (eyedropper). hueTolerance in degrees [0,180]; the
// tolerance/softness fields all default to a "moderate band" preset.
struct ColorRangeMask {
    bool hasReference = false;   // false -> mask is inert, skipped in render
    float refR = 0.0f;           // reference colour, linear RGB [0,1]
    float refG = 0.0f;
    float refB = 0.0f;
    double hueTolerance = 40.0;  // degrees [0,180]
    double satTolerance = 0.5;   // [0,1]
    double lumaTolerance = 0.5;  // [0,1]
    double softness = 0.5;       // [0,1]

    bool operator==(const ColorRangeMask&) const = default;
};

// A local-adjustment mask: weight = maskWeight * opacity, optionally inverted.
// The local adjustments mirror a subset of the global stack.
struct Mask {
    MaskType type = MaskType::None;
    LinearGradientMask linear;
    RadialGradientMask radial;
    std::vector<BrushDab> brush;
    ColorRangeMask color;        // only meaningful when type == ColorRange
    bool invert = false;
    double opacity = 1.0; // 0..1

    // Local adjustment values (Lightroom-like ranges, same semantics as the
    // global fields of Adjustments).
    double exposure = 0.0;    // stops, -5..+5
    double contrast = 0.0;    // -100..+100
    double highlights = 0.0;  // -100..+100
    double shadows = 0.0;     // -100..+100
    double whites = 0.0;      // -100..+100
    double blacks = 0.0;      // -100..+100
    double clarity = 0.0;     // -100..+100
    double dehaze = 0.0;      // -100..+100
    double saturation = 0.0;  // -100..+100
    double temperature = 0.0; // -100..+100
    double tint = 0.0;        // -100..+100

    // --- Sub-mask: colour range (PHASE_4) ------------------------------------
    // A pixel-content modifier layered on top of a *geometry* mask (linear /
    // radial / brush). Independent from the standalone MaskType::ColorRange
    // mask above: that type's weight comes solely from `color`, while these
    // fields modulate the geometry weight (multiply for intersection, max for
    // union -- see `maskUnion`). The reference colour is linear ProPhoto RGB.
    bool useColorRange = false;
    float colorRefR = 0.0f;            // reference colour, linear ProPhoto RGB
    float colorRefG = 0.0f;
    float colorRefB = 0.0f;
    float colorRange = 50.0f;          // 0-100 tolerance (larger = selects more)
    float colorSmoothness = 50.0f;     // 0-100 band-edge softness

    // --- Sub-mask: luminance range (PHASE_4) ---------------------------------
    // Relative-luminance modifier layered on top of the geometry mask. The
    // reference is the Rec.709 luma L = 0.2126R + 0.7152G + 0.0722B of a
    // sampled pixel, stored in percent (0-100).
    bool useLuminance = false;
    float luminanceRef = 50.0f;        // 0-100 relative-luminance reference
    float luminanceRange = 50.0f;      // 0-100 tolerance
    float luminanceSmoothness = 50.0f; // 0-100 band-edge softness

    // Combination mode for the sub-mask modifiers (default intersection):
    //   false -> weight = geometry x colour x luminance (multiply)
    //   true  -> weight = max(geometry, colour, luminance) (union)
    bool maskUnion = false;

    bool operator==(const Mask& other) const;
    size_t hash() const;

    // Narrower hash covering only the fields that shape the weight field
    // (type + geometry + invert + the sub-mask modifiers). Opacity and the
    // local-adjustment values are excluded, so dragging a local slider never
    // invalidates a cached weight field. Used as the cache key by
    // Pipeline::brushWeightField and by the viewport's overlay rebuild check.
    size_t geometryHash() const;

    bool hasEffect() const {
        return exposure != 0.0 || contrast != 0.0 || highlights != 0.0 ||
               shadows != 0.0 || whites != 0.0 || blacks != 0.0 ||
               clarity != 0.0 || dehaze != 0.0 || saturation != 0.0 ||
               temperature != 0.0 || tint != 0.0 ||
               useColorRange || useLuminance;
    }
};

// --- Spot removal / healing brush (PHASE_5) -----------------------------------
// A spot copies a circular region from a source point onto a target point:
//   Clone  -- direct copy of the source pixels (the Lightroom Clone stamp).
//   Heal   -- source texture scaled so its rim brightness matches the target
//             rim, with a feathered edge (a simplified Poisson-style blend).
// All coordinates are normalized to [0,1] of the image. `radius` is normalized
// against the longer image edge, so the same parameter set keeps the same
// relative spot size at any render resolution (1920px preview and full-size
// export share the same applyAdjustments/applySpatialAdjustments path).
enum class SpotType : uint8_t {
    Clone = 0,
    Heal = 1,
};

struct SpotRemoval {
    SpotType type = SpotType::Heal;
    float sourceX = 0.0f;   // source point, normalized [0,1]
    float sourceY = 0.0f;
    float targetX = 0.0f;   // target point, normalized [0,1]
    float targetY = 0.0f;
    float radius = 0.1f;    // radius, normalized against the longer edge
    bool visible = true;

    bool operator==(const SpotRemoval&) const = default;
};

// Point colour target: sample a colour from the image (reference linear RGB),
// then apply hue/saturation/luminance adjustments to pixels within `range` of
// that colour. `smoothness` controls the falloff softness at the band edge.
struct ColorTarget {
    bool enabled = false;
    float refR = 0.0f; // reference colour, linear RGB [0,1]
    float refG = 0.0f;
    float refB = 0.0f;
    double hueShift = 0.0;   // degrees, -180..+180
    double saturation = 0.0; // -100..+100
    double luminance = 0.0;  // -100..+100
    double range = 50.0;     // 0..100  colour-range width
    double smoothness = 50.0; // 0..100  falloff softness
    double amount = 100.0;   // 0..100  overall strength

    bool operator==(const ColorTarget&) const = default;
    size_t hash() const;

    bool hasEffect() const {
        return hueShift != 0.0 || saturation != 0.0 || luminance != 0.0;
    }
};

struct Adjustments {
    // --- develop panel parameters (LR-like ranges) -----------------------
    double exposure = 0.0;    // stops, -5..+5
    double contrast = 0.0;    // -100..+100
    double highlights = 0.0;  // -100..+100
    double shadows = 0.0;     // -100..+100
    double whites = 0.0;      // -100..+100  white point (clipping)
    double blacks = 0.0;      // -100..+100  black point (crushing)
    double temperature = 0.0; // -100..+100 (warm/cool offset around as-shot)
    double tint = 0.0;        // -100..+100 (green/magenta offset)
    double saturation = 0.0;  // -100..+100

    // --- effects (LR "effects" panel) ------------------------------------
    double texture = 0.0;   // -100..+100  small-radius local contrast
    double clarity = 0.0;   // -100..+100  mid-radius local contrast
    double dehaze = 0.0;    // -100..+100  atmospheric haze removal
    double vibrance = 0.0;  // -100..+100  saturation that protects skin tones

    // --- detail (LR "detail" panel) --------------------------------------
    double sharpness = 0.0;         // 0..150   unsharp-mask amount
    double sharpnessRadius = 1.0;   // 0.5..3.0 px  unsharp radius
    double sharpnessDetail = 0.0;   // 0..100   halo / fine-detail suppression
    double sharpnessMasking = 0.0;  // 0..100   only sharpen edges above threshold

    // --- detail: noise reduction (LR "detail" panel) ---------------------
    double luminance = 0.0;                 // 0..100   luma NR strength
    double luminanceDetail = 50.0;          // 0..100   luma detail preservation
    double luminanceContrast = 0.0;         // 0..100   restore local contrast after NR
    double colorNoiseReduction = 0.0;       // 0..100   chroma NR strength (neutral baseline)
    double colorNoiseReductionSmoothness = 50.0; // 0..100  chroma NR smoothness

    // --- lens corrections (profile-independent geometric model) ----------
    double distortion = 0.0;  // -100..+100  radial barrel/pincushion
    double vignette = 0.0;    // -100..+100  post-crop vignette amount (corner shading)

    // --- effects: post-crop vignette shape (LR "effects" panel) ----------
    double vignetteMidpoint = 50.0;   // 0..100    distance where the falloff is centred
    double vignetteRoundness = 0.0;   // -100..+100  rectangle (-100) to circle (+100)
    double vignetteFeather = 50.0;    // 0..100    falloff transition width
    double vignetteHighlights = 0.0;  // 0..100    protect bright pixels from darkening
    int vignetteStyle = 2;            // 0=PaintOverlay, 1=ColorPriority, 2=HighlightPriority (only 2 implemented)

    // --- effects: film grain ---------------------------------------------
    double grain = 0.0;            // 0..100  film-grain amount
    double grainSize = 25.0;       // 0..100  grain coarseness
    double grainRoughness = 50.0;  // 0..100  grain texture roughness

    // Optional parametric tone curve; empty means identity.
    std::vector<ToneCurvePoint> curve;

    // Per-channel HSL adjustments (one per ColorRange).
    // Default-constructed entries are all zero (no adjustment).
    std::array<HSLChannel, static_cast<size_t>(ColorRange::Count)> hsl;

    // Split toning (zeroed = no effect).
    SplitTone splitTone;

    // Masked local adjustments, applied spatially after the global stack.
    // A Mask with an empty geometry or zero opacity is ignored.
    std::vector<Mask> masks;

    // Point-colour targets, applied per-pixel after HSL (step 6.5).
    std::vector<ColorTarget> colorTargets;

    // Spot removal / healing brush corrections, applied after the spatial
    // stages (texture/clarity/sharpening/NR/distortion/grain/vignette/masks)
    // and before the final sRGB conversion, in array order (later spots paint
    // over earlier ones). Empty means no spot correction (the pipeline takes
    // the fast path and stays bit-exact).
    std::vector<SpotRemoval> spotRemovals;

    bool operator==(const Adjustments& other) const;
    bool operator!=(const Adjustments& other) const { return !(*this == other); }

    void reset();
    size_t hash() const;

    // Per-stage hashes for the incremental render cache (PHASE_2). Each covers
    // exactly the parameters that influence one pipeline stage, so the cache can
    // detect which stage changed without hashing the whole stack:
    //   A: basic tonal (exposure/contrast/highlights/shadows/whites/blacks)
    //   B: tone curve
    //   C: white balance / dehaze / vibrance
    //   D: HSL / global saturation / split toning / point colour
    //   E: spatial (texture/clarity/sharpening/NR/distortion/grain/vignette/
    //      masks/spot removals)
    //   F: sRGB output -- no adjustment parameters, returns a constant.
    size_t hashStageA() const;
    size_t hashStageB() const;
    size_t hashStageC() const;
    size_t hashStageD() const;
    size_t hashStageE() const;
    static size_t hashStageF();

    static Adjustments defaults();
};

} // namespace lrc
