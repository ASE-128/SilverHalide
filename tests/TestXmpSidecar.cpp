#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/Xmp.h"
#include "engine/XmpSidecar.h"
#include "engine/ZoneCurve.h"
#include "utils/XmpProfile.h"

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

// Point-wise curve comparison with an absolute tolerance.  The round-trip
// quantizes through integer 0..255 and the zone handles, so a small tolerance
// absorbs that rounding.
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

// Deletes the file on destruction (also when an assertion fails mid-test).
struct ScopedRemove {
    explicit ScopedRemove(std::filesystem::path p) : path(std::move(p)) {}
    ~ScopedRemove() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::filesystem::path path;
};

}  // namespace

TEST_CASE("XmpSidecar: sidecarPathFor replaces the extension", "[xmpsidecar]") {
    const auto p =
        XmpSidecar::sidecarPathFor(std::filesystem::path("C:\\photos\\img.arw"));
    REQUIRE(p.parent_path() == std::filesystem::path("C:\\photos"));
    REQUIRE(p.filename() == std::filesystem::path("img.xmp"));
    REQUIRE(p.extension() == std::filesystem::path(".xmp"));
    REQUIRE(p.generic_string() == "C:/photos/img.xmp");
}

TEST_CASE("XmpSidecar: sidecarPathFor preserves directory and base name",
          "[xmpsidecar]") {
    // Only the last extension is replaced.
    const auto p =
        XmpSidecar::sidecarPathFor(std::filesystem::path("C:\\photos\\photo.final.nef"));
    REQUIRE(p.parent_path() == std::filesystem::path("C:\\photos"));
    REQUIRE(p.filename() == std::filesystem::path("photo.final.xmp"));
    REQUIRE(p.generic_string() == "C:/photos/photo.final.xmp");
}

TEST_CASE("XmpSidecar: sidecarPathFor with no extension appends .xmp",
          "[xmpsidecar]") {
    const auto p =
        XmpSidecar::sidecarPathFor(std::filesystem::path("C:\\photos\\img"));
    REQUIRE(p.parent_path() == std::filesystem::path("C:\\photos"));
    REQUIRE(p.filename() == std::filesystem::path("img.xmp"));
    REQUIRE(p.extension() == std::filesystem::path(".xmp"));
    REQUIRE(p.generic_string() == "C:/photos/img.xmp");
}

TEST_CASE("XmpSidecar: write then read round-trips adjustments", "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_roundtrip.arw";
    const auto sidecarPath = dir / "lrc_sidecar_roundtrip.xmp";
    ScopedRemove cleanup(sidecarPath);

    Adjustments a;
    a.exposure = 0.5;
    a.contrast = 25.0;
    a.highlights = -40.0;
    a.shadows = 20.0;
    a.whites = 15.0;
    a.blacks = -10.0;
    a.temperature = 74.0;
    a.tint = 20.0;
    a.saturation = 30.0;
    a.curve = zonecurve::fromOutputs(0.2f, 0.4f, 0.6f, 0.8f);
    a.hsl[static_cast<size_t>(ColorRange::Red)].hue = 10.0;
    a.hsl[static_cast<size_t>(ColorRange::Red)].saturation = -15.0;
    a.hsl[static_cast<size_t>(ColorRange::Blue)].luminance = 20.0;

    REQUIRE(XmpSidecar::writeToSidecar(imagePath, a));
    REQUIRE(std::filesystem::exists(sidecarPath));

    Adjustments r;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, r));

    REQUIRE(r.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(r.contrast == Approx(25.0).margin(1e-6));
    REQUIRE(r.highlights == Approx(-40.0).margin(1e-6));
    REQUIRE(r.shadows == Approx(20.0).margin(1e-6));
    REQUIRE(r.whites == Approx(15.0).margin(1e-6));
    REQUIRE(r.blacks == Approx(-10.0).margin(1e-6));
    REQUIRE(r.saturation == Approx(30.0).margin(1e-6));
    REQUIRE(r.tint == Approx(20.0).margin(1e-6));
    // Temperature round-trips through an integer kelvin, allow +/-1.0.
    REQUIRE(std::abs(r.temperature - 74.0) <= 1.0);

    REQUIRE(curveApprox(r.curve, a.curve));

    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Red)].hue ==
            Approx(10.0).margin(1e-6));
    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Red)].saturation ==
            Approx(-15.0).margin(1e-6));
    REQUIRE(r.hsl[static_cast<size_t>(ColorRange::Blue)].luminance ==
            Approx(20.0).margin(1e-6));
}

TEST_CASE("XmpSidecar: missing sidecar returns false and leaves out untouched",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_missing.arw";
    // No sidecar is ever written for this image.
    ScopedRemove cleanup(dir / "lrc_sidecar_missing.xmp");

    Adjustments out;
    out.exposure = 99.0;    // sentinel
    out.temperature = -55.0;

    REQUIRE_FALSE(XmpSidecar::readFromSidecar(imagePath, out));
    REQUIRE(out.exposure == 99.0);
    REQUIRE(out.temperature == -55.0);
}

TEST_CASE("XmpSidecar: corrupt sidecar text returns false and leaves out untouched",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_corrupt.arw";
    const auto sidecarPath = dir / "lrc_sidecar_corrupt.xmp";
    ScopedRemove cleanup(sidecarPath);

    REQUIRE(writeFile(sidecarPath, "not xml at all"));

    Adjustments out;
    out.exposure = 99.0;  // sentinel
    REQUIRE_FALSE(XmpSidecar::readFromSidecar(imagePath, out));
    REQUIRE(out.exposure == 99.0);
}

TEST_CASE("XmpSidecar: valid XML without crs attributes returns false and leaves out untouched",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_empty.arw";
    const auto sidecarPath = dir / "lrc_sidecar_empty.xmp";
    ScopedRemove cleanup(sidecarPath);

    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""/>
 </rdf:RDF>
</x:xmpmeta>)";
    REQUIRE(writeFile(sidecarPath, xml));

    Adjustments out;
    out.exposure = 99.0;  // sentinel
    REQUIRE_FALSE(XmpSidecar::readFromSidecar(imagePath, out));
    REQUIRE(out.exposure == 99.0);
}

TEST_CASE("XmpSidecar: writeToSidecar succeeds for a default (empty) adjustment set",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_default.arw";
    const auto sidecarPath = dir / "lrc_sidecar_default.xmp";
    ScopedRemove cleanup(sidecarPath);

    // A fully-default stack still writes a minimal valid XMP document.
    REQUIRE(XmpSidecar::writeToSidecar(imagePath, Adjustments::defaults()));
    REQUIRE(std::filesystem::exists(sidecarPath));

    Adjustments r;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, r));
    REQUIRE(r == Adjustments::defaults());
}

TEST_CASE("XmpSidecar: readFromEmbedded is deferred (returns false)",
          "[xmpsidecar]") {
    const auto imagePath = std::filesystem::path("C:\\photos\\img.arw");
    Adjustments out;
    out.exposure = 99.0;  // sentinel
    REQUIRE_FALSE(XmpSidecar::readFromEmbedded(imagePath, out));
    REQUIRE(out.exposure == 99.0);
}

TEST_CASE("XmpSidecar: dc: metadata round-trips through the sidecar",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_meta.arw";
    const auto sidecarPath = dir / "lrc_sidecar_meta.xmp";
    ScopedRemove cleanup(sidecarPath);

    XmpMetadata md;
    md.keywords = {"Scenery", "Iceland", "Waterfall"};
    md.title = "Midnight sun";
    md.caption = "Waterfall under the midnight sun.";
    md.copyright = "(c) 2026";
    md.creator = "Alex";

    Adjustments a;
    a.exposure = 0.5;

    REQUIRE(XmpSidecar::writeToSidecar(imagePath, a, &md));
    REQUIRE(std::filesystem::exists(sidecarPath));

    Adjustments r;
    XmpMetadata rmd;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, r, &rmd));
    REQUIRE(r.exposure == Approx(0.5).margin(1e-6));
    REQUIRE(rmd == md);
}

TEST_CASE("XmpSidecar: hierarchical keywords round-trip as '|' paths",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_hier.arw";
    const auto sidecarPath = dir / "lrc_sidecar_hier.xmp";
    ScopedRemove cleanup(sidecarPath);

    XmpMetadata md;
    md.keywords = {"Scenery|Iceland|Waterfall", "NorthernLights"};

    REQUIRE(XmpSidecar::writeToSidecar(imagePath, Adjustments::defaults(), &md));

    // The hierarchical paths must land in lr:hierarchicalSubject.
    std::ifstream in(sidecarPath, std::ios::binary);
    const std::string xml((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    REQUIRE(xml.find("lr:hierarchicalSubject") != std::string::npos);
    REQUIRE(xml.find("Scenery|Iceland|Waterfall") != std::string::npos);

    XmpMetadata rmd;
    Adjustments r;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, r, &rmd));
    REQUIRE(rmd.keywords.size() == 2);
    REQUIRE(rmd.keywords[0] == "Scenery|Iceland|Waterfall");
    REQUIRE(rmd.keywords[1] == "NorthernLights");

    // A '/' path normalizes to '|' on import (lr:hierarchicalSubject is the
    // authoritative hierarchy source).
    const std::string slashXml =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description xmlns:lr=\"http://ns.adobe.com/lightroom/1.0/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<lr:hierarchicalSubject><rdf:Bag><rdf:li>Scenery/Iceland</rdf:li>"
        "</rdf:Bag></lr:hierarchicalSubject>"
        "</rdf:Description></rdf:RDF></x:xmpmeta>";
    XmpMetadata smd;
    REQUIRE(XmpDocument::readMetadata(slashXml, smd));
    REQUIRE(smd.keywords.size() == 1);
    REQUIRE(smd.keywords[0] == "Scenery|Iceland");
}

TEST_CASE("XmpSidecar: empty keywords omit dc:subject and lr:hierarchicalSubject",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_nokw.arw";
    const auto sidecarPath = dir / "lrc_sidecar_nokw.xmp";
    ScopedRemove cleanup(sidecarPath);

    XmpMetadata md;
    md.title = "No keywords";

    REQUIRE(XmpSidecar::writeToSidecar(imagePath, Adjustments::defaults(), &md));

    std::ifstream in(sidecarPath, std::ios::binary);
    const std::string xml((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    // Empty keyword list must not produce empty rdf:Bag structures, but the
    // title still lands in the document.
    REQUIRE(xml.find("dc:subject") == std::string::npos);
    REQUIRE(xml.find("lr:hierarchicalSubject") == std::string::npos);
    REQUIRE(xml.find("No keywords") != std::string::npos);

    XmpMetadata rmd;
    Adjustments r;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, r, &rmd));
    REQUIRE(rmd.keywords.empty());
    REQUIRE(rmd.title == "No keywords");
}

TEST_CASE("XmpSidecar: metadata-only sidecar reads with the metadata pointer",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_metaonly.arw";
    const auto sidecarPath = dir / "lrc_sidecar_metaonly.xmp";
    ScopedRemove cleanup(sidecarPath);

    XmpMetadata md;
    md.keywords = {"a", "b"};
    md.title = "Title only";
    REQUIRE(writeFile(sidecarPath, XmpDocument::serializeMetadata(md)));

    // With metadata requested, a metadata-only sidecar is a successful read;
    // the adjustments stay untouched (no crs attributes present).
    Adjustments out;
    out.exposure = 99.0;  // sentinel
    XmpMetadata rmd;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, out, &rmd));
    REQUIRE(out.exposure == 99.0);
    REQUIRE(rmd == md);

    // The 2-arg form keeps the legacy contract: no crs attributes -> false.
    Adjustments out2;
    out2.exposure = 77.0;  // sentinel
    REQUIRE_FALSE(XmpSidecar::readFromSidecar(imagePath, out2));
    REQUIRE(out2.exposure == 77.0);
}

TEST_CASE("XmpSidecar: spot corrections round-trip through the sidecar",
          "[xmpsidecar]") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto imagePath = dir / "lrc_sidecar_spot.arw";
    const auto sidecarPath = dir / "lrc_sidecar_spot.xmp";
    ScopedRemove cleanup(sidecarPath);

    Adjustments a;
    SpotRemoval s;
    s.type = SpotType::Heal;
    s.sourceX = 0.10f;
    s.sourceY = 0.20f;
    s.targetX = 0.30f;
    s.targetY = 0.40f;
    s.radius = 0.05f;
    s.visible = true;
    a.spotRemovals.push_back(s);

    REQUIRE(XmpSidecar::writeToSidecar(imagePath, a));
    REQUIRE(std::filesystem::exists(sidecarPath));

    std::ifstream in(sidecarPath, std::ios::binary);
    const std::string xml((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    REQUIRE(xml.find("crs:SpotCorrections") != std::string::npos);

    Adjustments r;
    REQUIRE(XmpSidecar::readFromSidecar(imagePath, r));
    REQUIRE(r.spotRemovals.size() == 1);
    REQUIRE(r.spotRemovals[0].type == SpotType::Heal);
    REQUIRE(r.spotRemovals[0].sourceX == Approx(0.10).margin(1e-6));
    REQUIRE(r.spotRemovals[0].sourceY == Approx(0.20).margin(1e-6));
    REQUIRE(r.spotRemovals[0].targetX == Approx(0.30).margin(1e-6));
    REQUIRE(r.spotRemovals[0].targetY == Approx(0.40).margin(1e-6));
    REQUIRE(r.spotRemovals[0].radius == Approx(0.05).margin(1e-6));
    REQUIRE(r.spotRemovals[0].visible);
}

TEST_CASE("XmpSidecar: reads Lightroom element-form spot corrections",
          "[xmpsidecar]") {
    // Lightroom writes structured values as child elements inside an
    // rdf:parseType="Resource" li and uses the plural crs:CorrectionMasks.
    const std::string lrcXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/">
   <crs:SpotCorrections>
    <rdf:Bag>
     <rdf:li rdf:parseType="Resource">
      <crs:CorrectionType>Heal</crs:CorrectionType>
      <crs:CorrectionSource>Source</crs:CorrectionSource>
      <crs:CorrectionAmount>1.000000</crs:CorrectionAmount>
      <crs:CorrectionActive>true</crs:CorrectionActive>
      <crs:CorrectionMasks>
       <rdf:Bag>
        <rdf:li rdf:parseType="Resource">
         <crs:CenterX>0.300000</crs:CenterX>
         <crs:CenterY>0.400000</crs:CenterY>
         <crs:Radius>0.050000</crs:Radius>
        </rdf:li>
       </rdf:Bag>
      </crs:CorrectionMasks>
     </rdf:li>
    </rdf:Bag>
   </crs:SpotCorrections>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>)";

    std::vector<XmpDocument::SpotCorrectionXml> xml;
    REQUIRE(XmpDocument::parseSpotCorrections(lrcXml, xml));
    REQUIRE(xml.size() == 1);
    REQUIRE(xml[0].type == "Heal");
    REQUIRE(xml[0].centerX == "0.300000");
    REQUIRE(xml[0].centerY == "0.400000");
    REQUIRE(xml[0].radius == "0.050000");

    const auto r = XmpProfile::fromSpotCorrectionXml(xml);
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].type == SpotType::Heal);
    REQUIRE(r[0].targetX == Approx(0.3).margin(1e-6));
    REQUIRE(r[0].targetY == Approx(0.4).margin(1e-6));
    REQUIRE(r[0].radius == Approx(0.05).margin(1e-6));
    REQUIRE(r[0].visible);
}
