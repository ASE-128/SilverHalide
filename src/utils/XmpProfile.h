#pragma once

// Bidirectional mapping between Lightroom XMP "crs:" develop settings and the
// lrc::Adjustments parameter stack. Works purely on a map of
// crs-attribute-name -> string value (names WITHOUT the "crs:" prefix), so it is
// completely independent of the XML layer (XmpDocument) and stays headless-testable.
//
// The field names, units, scales, defaults, clamping and unknown-key behaviour
// all follow docs/XMP-MAPPING.md. Round-tripping is lossless for every scalar
// field; the temperature offset uses a fixed 6500 K mired-linear anchor (see the
// mapping document, section 6.1).
//
// Spot corrections (crs:SpotCorrections) are structured XML, so they do not fit
// the flat attribute map. They are mapped through the string-valued
// XmpDocument::SpotCorrectionXml intermediate: XmpProfile converts between that
// representation and the Adjustments spot-removal list
// (SpotRemoval / Adjustments::spotRemovals in engine/Adjustments.h).

#include <map>
#include <string>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/Xmp.h"

namespace lrc {

/**
 * @brief Converts between Lightroom XMP "crs:" attributes and the adjustment
 *        stack.
 *
 * Operates on a flat attribute-name -> string map (names without the "crs:"
 * prefix), so it is independent of the XML layer and headless-testable. Field
 * names, scales, defaults and clamping follow docs/XMP-MAPPING.md.
 */
class XmpProfile {
public:
    /** @brief Exports an adjustment stack to a crs attribute map.
     *
     * Only non-default fields are emitted (see the mapping document for
     * per-field defaults).
     */
    static std::map<std::string, std::string> toCrsAttrs(const Adjustments& adjustments);

    /** @brief Imports a crs attribute map into an adjustment stack.
     *
     * Unknown keys and unparseable values are skipped (never fatal); missing
     * keys keep the field defaults.
     */
    static Adjustments fromCrsAttrs(const std::map<std::string, std::string>& attrs);

    /** @brief Converts the spot-removal list into the string-valued XML
     *         representation consumed by XmpDocument.
     *
     * Source/target/radius are normalized 0..1 (the engine's SpotRemoval unit:
     * coordinates relative to the image, radius against the longer edge).
     *
     * @param spots Spot removals to encode.
     * @param out   Receives the per-spot XML records.
     *
     * @note The mask circle is centred on the target point; the source point is
     *       carried by the custom crs:SourceX/crs:SourceY keys, because
     *       Lightroom's per-spot XMP has no direct source offset.
     */
    static void toSpotCorrectionXml(
        const std::vector<SpotRemoval>& spots,
        std::vector<XmpDocument::SpotCorrectionXml>& out);

    /** @brief Imports the string-valued XML representation back into a
     *         spot-removal list.
     *
     * Unknown CorrectionType values default to Clone; missing or unparseable
     * geometry keeps the field default (0.0 / visible). Geometry is clamped to
     * 0..1.
     */
    static std::vector<SpotRemoval> fromSpotCorrectionXml(
        const std::vector<XmpDocument::SpotCorrectionXml>& xmlSpots);
};

} // namespace lrc
