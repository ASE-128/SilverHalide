#pragma once

// Non-destructive adjustment parameters. This is the "parameter stack" of the
// pipeline: it never mutates the decoded raw buffer, only describes how to
// render it. Adjustments are applied in linear light in a fixed order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "engine/AiInference.h"  // SamPoint, SamMaskResult (for Prompt click masks)

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
    AiMask,                // AI-generated subject / sky / background
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

// --- AI Mask parameters ---------------------------------------------------
// Attached to a Mask whose `type == AiMask`.  The `target` selects which
// segmentation model to run; the resulting weight field is refined by the
// threshold/feather/invert knobs.  `cachedWeightVersion` increments whenever
// Pipeline should regenerate the weight from scratch (e.g. user switched
// target, or the underlying model updated on disk).

enum class AiMaskTarget : uint8_t {
    Subject = 0,
    Sky = 1,
    Background = 2,
    Prompt = 3,  // click-prompt (MobileSAM): mask follows SamPoint list
};

struct AiMaskParams {
    AiMaskTarget target = AiMaskTarget::Subject;
    float threshold = 0.5f;       // 0..1 soft knee at the mask boundary
    float feather = 0.05f;        // 0..1 blur radius relative to image diag
    bool invert = false;
    int modelInputSize = 1024;    // 320 / 512 / 1024 depending on the model
    int cachedWeightVersion = 0;  // bump to invalidate any rasterised weight
    // Optional model override — defaults come from AiMaskGenerator.
    std::string modelIdOverride;
    // Click prompts (only meaningful when target == Prompt). Normalized [0,1]
    // image coords; label 1 = foreground (include), 0 = background (exclude).
    // Pipeline feeds these to MobileSAM's decoder to rasterize the mask weight.
    std::vector<SamPoint> points;

    bool operator==(const AiMaskParams&) const = default;
};

// --- Mask geometry (discriminated union) ------------------------------------
// A mask owns exactly one geometry (linear / radial / brush stroke / colour
// range / AI parameters) plus the flat local-adjustment and sub-mask fields
// below. This used to be a "keep every geometry resident" struct, which made
// Mask ~350 bytes and allowed illegal combinations (e.g. a radial mask that
// still carried stale brush dabs). std::variant makes "one geometry at a
// time" structural while keeping the observable behaviour identical: an unused
// geometry reads as its default-constructed value, exactly like the old unused
// member did.

// Brush-stroke geometry: the ordered dab list of a MaskType::Brush mask.
// Wrapped in a struct instead of storing std::vector<BrushDab> directly so the
// variant alternative names the shape and can grow per-stroke metadata later.
struct BrushGeometry {
    std::vector<BrushDab> dabs;

    bool operator==(const BrushGeometry&) const = default;
};

/**
 * @brief The single resident geometry of a Mask.
 *
 * LinearGradientMask is deliberately the first alternative: a default-
 * constructed Mask therefore already owns a writable linear geometry, which
 * keeps the ubiquitous `Mask m; m.type = MaskType::LinearGradient;
 * m.linear().startX = ...;` sequence valid without extra initialisation.
 * std::monostate (last) is the explicit "no geometry" state selected by
 * MaskType::None.
 *
 * @note Prefer the Mask accessors (linear() / radial() / brush() / color() /
 *       ai()) or Mask::setType() over manipulating the variant directly: a
 *       mismatch between Mask::type and the resident alternative is harmless to
 *       read (the accessors fall back to that alternative's defaults) but
 *       switching the resident alternative drops the previous geometry.
 */
using MaskShape = std::variant<LinearGradientMask, RadialGradientMask,
                               BrushGeometry, ColorRangeMask, AiMaskParams,
                               std::monostate>;

// A local-adjustment mask: weight = maskWeight * opacity, optionally inverted.
// The local adjustments mirror a subset of the global stack.
struct Mask {
    // Authoritative selector. The UI, ProfileIO and Pipeline all dispatch on
    // this; it also decides which MaskShape alternative is the resident one.
    MaskType type = MaskType::None;

    // Storage for the single resident geometry (see MaskShape). Access it
    // through the accessors below instead of poking the variant directly.
    MaskShape shape;

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

    // --- Sub-mask: colour range -------------------------------------------------
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

    // --- Sub-mask: luminance range ---------------------------------------------
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

    // --- Geometry accessors ---------------------------------------------------
    // `type` is the authoritative selector and the accessors never change it.
    // Consumers must branch on `type` first; the accessors then make the
    // matching geometry readable/writable without a throw in any state.

    /**
     * @brief Resident linear-gradient geometry (mutable).
     *
     * Returns the resident LinearGradientMask when the variant holds one;
     * otherwise re-seats the variant to a default-constructed LinearGradientMask
     * and returns that instance, so a write always has a home. Does not change
     * Mask::type. Never throws and never touches shared state.
     */
    LinearGradientMask& linear() { return mutableShape<LinearGradientMask>(); }

    /**
     * @brief Resident linear-gradient geometry or its defaults (read-only).
     *
     * Returns the resident LinearGradientMask when the variant holds one;
     * otherwise a function-local immutable default instance. Safe from any
     * number of threads: a const Mask (e.g. shared through AdjustmentsPtr) is
     * never mutated, so concurrent readers only read the immutable default.
     */
    const LinearGradientMask& linear() const noexcept {
        return residentOrDefault<LinearGradientMask>();
    }

    /**
     * @brief Resident radial-gradient geometry (mutable); re-seated on demand.
     * @see linear() for the exact re-seating / thread-safety contract.
     */
    RadialGradientMask& radial() { return mutableShape<RadialGradientMask>(); }

    /**
     * @brief Resident radial-gradient geometry or its defaults (read-only).
     * @see linear() const for the thread-safety contract.
     */
    const RadialGradientMask& radial() const noexcept {
        return residentOrDefault<RadialGradientMask>();
    }

    /**
     * @brief Resident brush dabs (mutable); the BrushGeometry is re-seated to
     *        an empty stroke when the variant does not hold one.
     * @see linear() for the exact re-seating contract.
     */
    std::vector<BrushDab>& brush() { return mutableShape<BrushGeometry>().dabs; }

    /**
     * @brief Resident brush dabs, or an empty immutable list (read-only).
     * @see linear() const for the thread-safety contract.
     */
    const std::vector<BrushDab>& brush() const noexcept {
        return residentOrDefault<BrushGeometry>().dabs;
    }

    /**
     * @brief Resident colour-range geometry (mutable); re-seated on demand.
     * @see linear() for the exact re-seating / thread-safety contract.
     */
    ColorRangeMask& color() { return mutableShape<ColorRangeMask>(); }

    /**
     * @brief Resident colour-range geometry or its defaults (read-only).
     * @see linear() const for the thread-safety contract.
     */
    const ColorRangeMask& color() const noexcept {
        return residentOrDefault<ColorRangeMask>();
    }

    /**
     * @brief Resident AI-mask parameters (mutable); re-seated on demand.
     * @see linear() for the exact re-seating / thread-safety contract.
     */
    AiMaskParams& ai() { return mutableShape<AiMaskParams>(); }

    /**
     * @brief Resident AI-mask parameters or their defaults (read-only).
     * @see linear() const for the thread-safety contract.
     */
    const AiMaskParams& ai() const noexcept {
        return residentOrDefault<AiMaskParams>();
    }

    /**
     * @brief Sets Mask::type and re-seats the resident geometry to match it.
     *
     * The payload is kept when the variant already holds the requested
     * alternative; otherwise the alternative is default-constructed, which
     * discards the previously resident geometry (that geometry is unused for
     * the new type, matching the old "all geometries resident" behaviour for
     * everything the renderer reads).
     *
     * @param t New mask type (MaskType::None re-seats to std::monostate).
     */
    void setType(MaskType t);

    /**
     * @brief True when the resident alternative is the one `type` selects.
     *
     * Diagnostic / test helper; the accessors do not require this to hold.
     * A default-constructed Mask reports false because MaskShape intentionally
     * seeds a (unused) LinearGradientMask while `type` is MaskType::None.
     */
    bool shapeMatchesType() const noexcept;

    /**
     * @brief Copies type, invert and the linear/radial/brush geometry from
     *        `other`, leaving opacity, local adjustments and sub-mask
     *        modifiers untouched.
     *
     * Mirrors what the develop panel's viewport-draft sync used to do field by
     * field (type + linear + radial + brush + invert). The colour-range and AI
     * alternatives are deliberately not copied: they are edited through their
     * own paths, not through the viewport geometry draft.
     */
    void copyGeometryFrom(const Mask& other);

    bool operator==(const Mask& other) const;
    /** @brief Full-content hash (every field that can change the rendered result).
     *
     * @note noexcept: pure mixing of values already in memory; the const
     *       geometry accessors it reads never allocate or throw.
     */
    size_t hash() const noexcept;

    // Narrower hash covering only the fields that shape the weight field
    // (type + geometry + invert + the sub-mask modifiers). Opacity and the
    // local-adjustment values are excluded, so dragging a local slider never
    // invalidates a cached weight field. Used as the cache key by
    // Pipeline::brushWeightField and by the viewport's overlay rebuild check.
    /** @brief Geometry-only hash shaping the cached weight field.
     *
     * @note noexcept: same contract as hash(), over a strict subset of fields.
     */
    size_t geometryHash() const noexcept;

    bool hasEffect() const noexcept {
        if (opacity <= 0.0) return false;   // fully transparent: never contributes
        return exposure != 0.0 || contrast != 0.0 || highlights != 0.0 ||
               shadows != 0.0 || whites != 0.0 || blacks != 0.0 ||
               clarity != 0.0 || dehaze != 0.0 || saturation != 0.0 ||
               temperature != 0.0 || tint != 0.0 ||
               useColorRange || useLuminance ||
               (type == MaskType::AiMask);  // AI mask always contributes weight
    }

  private:
    // Mutable accessor: returns the resident alternative, re-seating the
    // variant to a default-constructed one on a miss so the caller can write.
    template <class T>
    T& mutableShape() {
        if (T* resident = std::get_if<T>(&shape))
            return *resident;
        return shape.emplace<T>();
    }

    // Const accessor: never mutates (a const Mask can be shared across
    // threads). On a miss it returns a function-local immutable default; the
    // magic static is initialised exactly once and is safe to read
    // concurrently.
    template <class T>
    const T& residentOrDefault() const {
        if (const T* resident = std::get_if<T>(&shape))
            return *resident;
        static const T kDefaultInstance{};
        return kDefaultInstance;
    }
};

// --- Spot removal / healing brush -------------------------------------------
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

// --- AI Denoise ------------------------------------------------------------
// Non-destructive AI denoise parameters.  The actual denoise buffer is NOT
// stored here — only the "what / how strong" metadata.  Pipeline computes
// the denoised pixels on demand (cached by `imageId + aiDenoise.version`)
// and the strength slider mixes between the original and the cached result.

enum class AiDenoiseMode : uint8_t {
    LuminanceAndColor = 0,
    LuminanceOnly = 1,
};

struct AiDenoise {
    bool enabled = false;
    float strength = 100.0f;       // 0..100, user-facing percentage
    AiDenoiseMode mode = AiDenoiseMode::LuminanceAndColor;
    int modelVersion = 0;         // bumped automatically on update

    bool operator==(const AiDenoise& o) const = default;
    bool hasEffect() const noexcept { return enabled && strength > 0.0f; }
};

// --- AI Mask parameters ---------------------------------------------------
// NOTE: the real definition lives above `struct Mask` — kept here as a
// navigational anchor only so the Doxygen group for AI features in this file
// reads consistently top-to-bottom.

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
    /** @brief Content hash used by the render cache keys.
     *
     * @note noexcept: pure scalar mixing, no allocation.
     */
    size_t hash() const noexcept;

    bool hasEffect() const noexcept {
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

    // Optional Lensfun geometric correction. The strings identify the matched
    // camera/lens profile; the numeric fields are the capture parameters used
    // by lfModifier. Disabled profiles leave the pixel pipeline unchanged.
    bool lensfunEnabled = false;
    std::string lensfunCameraMaker;
    std::string lensfunCameraModel;
    std::string lensfunLensMaker;
    std::string lensfunLensModel;
    float lensfunFocalLengthMm = 0.0f;
    float lensfunAperture = 0.0f;
    float lensfunCropFactor = 1.0f;

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

    // --- AI Denoise ------------------------------------------------------------
    AiDenoise aiDenoise;

    // HARD CONTRACT: every field of this struct must be compared by
    // operator== and mixed into hash() (plus the matching hashStageX() when the
    // field belongs to a stage). The UI's shared-snapshot publishing relies on
    // operator== being exhaustive: MainWindow::adjustmentsSnapshot() and
    // adoptPublishedAdjustments() reuse the published snapshot while it compares
    // equal, so a field that is missing here would make a changed stack look
    // unchanged and dispatch a STALE adjustment stack to the preview/export
    // worker. A new field with a default value must be added to operator== in the
    // same change.
    bool operator==(const Adjustments& other) const;
    bool operator!=(const Adjustments& other) const { return !(*this == other); }

    void reset();
    // HARD CONTRACT: see the operator== note above - a field missing from the
    // hash mix makes the render caches collapse two different stacks into one
    // entry (stale preview), not just a weaker checksum.
    /** @brief Full-content hash of the whole adjustment stack.
     *
     * @note noexcept: mixes values already in memory (including masks and
     *       strings) without allocating; cache keys rely on this being cheap.
     */
    size_t hash() const noexcept;

    // Per-stage hashes for the incremental render cache. Each covers
    // exactly the parameters that influence one pipeline stage, so the cache can
    // detect which stage changed without hashing the whole stack:
    //   A: basic tonal (exposure/contrast/highlights/shadows/whites/blacks)
    //   B: tone curve
    //   C: white balance / dehaze / vibrance
    //   D: HSL / global saturation / split toning / point colour
    //   E: spatial (texture/clarity/sharpening/NR/distortion/grain/vignette/
    //      masks/spot removals)
    //   F: sRGB output -- no adjustment parameters, returns a constant.
    size_t hashStageA() const noexcept;
    size_t hashStageB() const noexcept;
    size_t hashStageC() const noexcept;
    size_t hashStageD() const noexcept;
    size_t hashStageE() const noexcept;
    static size_t hashStageF() noexcept;

    /** @brief Default-constructed stack (all sliders neutral).
     *
     * @note Ignoring the returned stack is always a bug, hence [[nodiscard]].
     *       Not noexcept: it allocates the contained vectors / strings.
     */
    [[nodiscard]] static Adjustments defaults();
};

/**
 * @brief Immutable, shared adjustment stack for cross-thread hand-off.
 *
 * The UI owns the mutable working copy (Adjustments) and publishes snapshots of
 * it as this type: preview/export workers and queued slots receive the pointer
 * and only read through it. This avoids copying the mask / curve / spot vectors
 * into every queued request, and because the pointee is const no consumer can
 * mutate the stack behind the UI's back.
 *
 * @note Build one snapshot per edit and pass it to every request derived from
 *       that edit; the shared owner may outlive the UI request, so the pointee
 *       must never be mutated after publication. Thread safety is that of
 *       const access to a fully constructed stack: any number of readers are
 *       safe once the publishing thread has finished writing it.
 */
using AdjustmentsPtr = std::shared_ptr<const Adjustments>;

} // namespace lrc
