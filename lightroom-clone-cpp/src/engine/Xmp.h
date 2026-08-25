#pragma once

// Minimal XMP/RDF document handling for Lightroom .xmp sidecars and presets.
// Only the Adobe camera-raw-settings (crs:) namespace is collected; every other
// namespace and structural element is ignored.  Kept Qt-free so the mapping
// layer and sidecar I/O stay testable in the headless build.
//
// The design deliberately separates concerns:
//   XmpDocument  <->  std::map<crs-name, value>  <->  lrc::Adjustments (XmpProfile)
// The map is the only interface contract between the XML layer and the mapping
// layer for the flat crs attributes, so the two can be developed and tested
// independently.  Structured elements (crs:SpotCorrections, dc:/lr: metadata)
// use their own string-valued intermediate structs declared below; numeric and
// enum interpretation stays in the mapping layer (XmpProfile).

#include <map>
#include <string>
#include <vector>

namespace lrc {

// Structured Dublin-Core / Lightroom metadata exchanged with an XMP sidecar.
// This is deliberately NOT part of Adjustments (metadata is not a develop
// parameter); it rides alongside Adjustments through the sidecar API.
struct XmpMetadata {
    // Keyword labels.  Hierarchical keywords use '|' as the level separator,
    // e.g. "Scenery|Iceland|Waterfall" (Lightroom's lr:hierarchicalSubject
    // convention).  dc:subject is stored the same way.
    std::vector<std::string> keywords;

    std::string title;      // dc:title
    std::string caption;    // dc:description
    std::string copyright;  // dc:rights
    std::string creator;    // dc:creator

    bool operator==(const XmpMetadata&) const = default;
};

class XmpDocument {
public:
    // Parses XMP XML text (an .xmp sidecar or RAW-embedded xmpdata) and fills
    // `outCrsAttrs` with every attribute in the crs: namespace
    // (http://ns.adobe.com/camera-raw-settings/1.0/).  Names are stored WITHOUT
    // the "crs:" prefix (e.g. "Exposure2012").  Returns false when the text is
    // not well-formed XML or contains no crs attributes.  Non-crs attributes,
    // unknown namespaces and nested rdf: structures are ignored.
    static bool parse(const std::string& xmlText,
                      std::map<std::string, std::string>& outCrsAttrs);

    // Serializes crs attributes into a minimal but valid XMP RDF document:
    //   <x:xmpmeta><rdf:RDF><rdf:Description ... crs attrs .../></rdf:RDF></x:xmpmeta>
    // with the crs namespace declared.  `rdfAbout` becomes rdf:about (empty =
    // omitted).  Attribute values are emitted verbatim (already formatted by the
    // mapping layer).
    static std::string serialize(const std::map<std::string, std::string>& crsAttrs,
                                 const std::string& rdfAbout = std::string());

    // --- Structured spot-correction element -------------------------------
    // One crs:SpotCorrection entry as found in a crs:SpotCorrections rdf:Bag.
    // String-valued on purpose: this is the XML layer; value interpretation
    // (normalized float, Clone/Heal enum) is done by XmpProfile.
    struct SpotCorrectionXml {
        std::string type;     // crs:CorrectionType "Clone" / "Heal"
        std::string source;   // crs:CorrectionSource "Source" (manual source)
        std::string amount;   // crs:CorrectionAmount "100" (opacity, fixed)
        std::string active;   // crs:CorrectionActive "true" / "false"
        std::string centerX;  // crs:CenterX in crs:CorrectionMask, normalized 0..1
        std::string centerY;  // crs:CenterY in crs:CorrectionMask, normalized 0..1
        std::string radius;   // crs:Radius in crs:CorrectionMask, normalized 0..1
        std::string sourceX;  // crs:SourceX custom key (no LRC equivalent), normalized 0..1
        std::string sourceY;  // crs:SourceY custom key (no LRC equivalent), normalized 0..1
    };

    // Parses crs:SpotCorrections from XMP XML text into string-valued entries.
    // Returns false when the text is malformed or contains no spot corrections.
    // Tolerant of both attribute-form and element-form properties and of both
    // crs:CorrectionMask (project) and crs:CorrectionMasks (Lightroom plural).
    static bool parseSpotCorrections(const std::string& xmlText,
                                     std::vector<SpotCorrectionXml>& out);

    // Serializes spot corrections into a crs:SpotCorrections XML fragment.
    static std::string serializeSpotCorrections(
        const std::vector<SpotCorrectionXml>& spots);

    // --- dc:/lr: metadata ---------------------------------------------------
    // Reads dc:subject / lr:hierarchicalSubject keywords, dc:title,
    // dc:description, dc:rights and dc:creator from XMP XML text.  Returns
    // false only when the text is not well-formed XML; `out` is left as the
    // defaults when nothing is found.
    static bool readMetadata(const std::string& xmlText, XmpMetadata& out);

    // Serializes metadata into a minimal XMP RDF document that carries only the
    // dc:/lr: elements (no crs attributes).  Empty fields are omitted.
    static std::string serializeMetadata(const XmpMetadata& md);

    // Full document serialization: crs attributes plus, optionally, dc:/lr:
    // metadata and crs:SpotCorrections, all on a single rdf:Description.
    // Passing null / empty keeps the corresponding section out of the output,
    // so callers that only need the flat crs attributes keep the legacy shape.
    static std::string serializeFull(
        const std::map<std::string, std::string>& crsAttrs,
        const std::string& rdfAbout = std::string(),
        const XmpMetadata* metadata = nullptr,
        const std::vector<SpotCorrectionXml>* spots = nullptr);
};

} // namespace lrc
