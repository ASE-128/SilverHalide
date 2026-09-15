#pragma once

// XMP sidecar I/O: read/write a .xmp sidecar that lives next to an image file
// (same name, .xmp extension), converting it to/from lrc::Adjustments via
// XmpDocument + XmpProfile.  Kept Qt-free for headless testing.
//
// Embedded RAW XMP (libraw xmpdata) is an optional v1 feature: readFromEmbedded
// returns false when not implemented - callers fall back to the sidecar.
//
// In addition to the develop settings, a sidecar may carry dc:/lr: metadata
// (keywords, title, caption, copyright, creator) and crs:SpotCorrections.
// The optional XmpMetadata* parameter lets callers round-trip that metadata
// without changing the default (develop-settings-only) behaviour.

#include <filesystem>

#include "engine/Adjustments.h"
#include "engine/Xmp.h"

namespace lrc {

class XmpSidecar {
public:
    // The sidecar path for an image: same directory + filename with the
    // extension replaced by ".xmp".
    static std::filesystem::path sidecarPathFor(const std::filesystem::path& imagePath);

    // Loads the sidecar (if present and parseable) into `out`.  When `metadata`
    // is non-null, dc:/lr: metadata is loaded into it as well.  Spot
    // corrections are merged into `out.spotRemovals` when present.  Returns
    // false (and leaves `out` untouched) when the sidecar is missing or invalid.
    // A metadata-only sidecar (no crs attributes) returns true when `metadata`
    // is requested, leaving `out` untouched.
    static bool readFromSidecar(const std::filesystem::path& imagePath,
                                Adjustments& out,
                                XmpMetadata* metadata = nullptr);

    // Writes `adjustments` as an XMP sidecar next to the image.  When
    // `metadata` is non-null, dc:/lr: metadata is embedded as well; spot
    // corrections (adjustments.spotRemovals) are embedded when non-empty.
    // Returns false on I/O failure.
    static bool writeToSidecar(const std::filesystem::path& imagePath,
                               const Adjustments& adjustments,
                               const XmpMetadata* metadata = nullptr);

    // Optional: reads the XMP embedded in a RAW file's metadata (libraw
    // xmpdata).  Returns false when not implemented or nothing embedded.
    static bool readFromEmbedded(const std::filesystem::path& imagePath,
                                 Adjustments& out);
};

} // namespace lrc
