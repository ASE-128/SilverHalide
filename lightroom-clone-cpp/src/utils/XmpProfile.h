#pragma once

// Bidirectional mapping between Lightroom XMP "crs:" develop settings and the
// lrc::Adjustments parameter stack.  Works purely on a map of
// crs-attribute-name -> string value (names WITHOUT the "crs:" prefix), so it is
// completely independent of the XML layer (XmpDocument) and stays headless-testable.
//
// The field names, units, scales, defaults, clamping and unknown-key behaviour
// all follow docs/XMP-MAPPING.md.  Round-tripping is lossless for every scalar
// field; the temperature offset uses a fixed 6500 K mired-linear anchor (see the
// mapping document, section 6.1).
//
// Spot corrections (PHASE_5, crs:SpotCorrections) are structured XML, so they
// do not fit the flat attribute map.  They are mapped through the string-valued
// XmpDocument::SpotCorrectionXml intermediate: XmpProfile converts between that
// representation and the Adjustments spot-removal list (SpotRemoval /
// Adjustments::spotRemovals, added by PHASE_5 in engine/Adjustments.h).

#include <map>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/Xmp.h"

namespace lrc {

class XmpProfile {
public:
    // Export: Adjustments -> crs attribute map.  Only non-default fields are
    // emitted (see the mapping document for per-field defaults).
    static std::map<std::string, std::string> toCrsAttrs(const Adjustments& adjustments);

    // Import: crs attribute map -> Adjustments.  Unknown keys and unparseable
    // values are skipped (never fatal); missing keys keep field defaults.
    static Adjustments fromCrsAttrs(const std::map<std::string, std::string>& attrs);

    // --- Spot corrections (crs:SpotCorrections) ---------------------------
    // Converts the spot-removal list into the string-valued XML representation
    // consumed by XmpDocument.  Source/target/radius are normalized 0..1 (the
    // engine's SpotRemoval unit: coordinates relative to the image, radius
    // against the longer edge).  Mask center maps to the target point; the
    // custom crs:SourceX/crs:SourceY keys carry the source point (Lightroom has
    // no direct per-spot source coordinates).
    static void toSpotCorrectionXml(
        const std::vector<SpotRemoval>& spots,
        std::vector<XmpDocument::SpotCorrectionXml>& out);

    // Import: string-valued XML representation -> spot-removal list.  Unknown
    // CorrectionType values default to Clone; missing/unparseable geometry
    // keeps the field default (0.0 / visible).  Clamped to 0..1.
    static std::vector<SpotRemoval> fromSpotCorrectionXml(
        const std::vector<XmpDocument::SpotCorrectionXml>& xmlSpots);
};

} // namespace lrc
