#include "engine/XmpSidecar.h"

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/Xmp.h"
#include "utils/XmpProfile.h"

namespace lrc {

namespace {

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

std::filesystem::path XmpSidecar::sidecarPathFor(
    const std::filesystem::path& imagePath) {
    std::filesystem::path p = imagePath;
    return p.replace_extension(".xmp");
}

bool XmpSidecar::readFromSidecar(const std::filesystem::path& imagePath,
                                 Adjustments& out, XmpMetadata* metadata) {
    const std::filesystem::path sidecarPath = sidecarPathFor(imagePath);
    if (!std::filesystem::exists(sidecarPath))
        return false;

    std::ifstream in(sidecarPath, std::ios::binary);
    if (!in)
        return false;

    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    if (in.bad())
        return false;

    std::map<std::string, std::string> attrs;
    const bool parsed = XmpDocument::parse(text, attrs);

    if (metadata) {
        XmpMetadata md;
        const bool wellFormed = XmpDocument::readMetadata(text, md);
        *metadata = std::move(md);
        if (!parsed) {
            // No crs attributes.  A metadata-only sidecar is a successful read
            // when the caller asked for metadata; otherwise keep the legacy
            // contract (returns false, leaves `out` untouched).
            return wellFormed;
        }
    } else if (!parsed) {
        return false;
    }

    out = XmpProfile::fromCrsAttrs(attrs);

    // Structured spot corrections merge into the Adjustments spot list.
    std::vector<XmpDocument::SpotCorrectionXml> spotXml;
    if (XmpDocument::parseSpotCorrections(text, spotXml)) {
        out.spotRemovals = XmpProfile::fromSpotCorrectionXml(spotXml);
    }

    return true;
}

bool XmpSidecar::writeToSidecar(const std::filesystem::path& imagePath,
                                const Adjustments& adjustments,
                                const XmpMetadata* metadata) {
    const auto attrs = XmpProfile::toCrsAttrs(adjustments);

    std::vector<XmpDocument::SpotCorrectionXml> spotXml;
    XmpProfile::toSpotCorrectionXml(adjustments.spotRemovals, spotXml);

    const std::string text = XmpDocument::serializeFull(
        attrs, /*rdfAbout=*/std::string(), metadata,
        spotXml.empty() ? nullptr : &spotXml);
    return writeFileAtomic(sidecarPathFor(imagePath), text);
}

bool XmpSidecar::readFromEmbedded(const std::filesystem::path&, Adjustments&) {
    // v1: embedded RAW XMP not read (deferred).
    return false;
}

} // namespace lrc
