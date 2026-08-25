#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "engine/Xmp.h"

using lrc::XmpDocument;

namespace {

// Golden sample from docs/XMP-MAPPING.md section 8.1 (verbatim).
const char* kGoldenSample = R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 5.6-c140">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    xmlns:exif="http://ns.adobe.com/exif/1.0/"
    crs:Exposure2012="+0.50"
    crs:Contrast2012="+25"
    crs:Highlights2012="-40"
    crs:Temperature="4000"
    crs:Tint="+10"
    crs:Clarity2012="+30"
    crs:ToneCurvePV2012="0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255"
    exif:UnknownTag="shouldBeIgnored"/>
 </rdf:RDF>
</x:xmpmeta>
)";

} // namespace

TEST_CASE("XmpDocument: golden sample extracts crs attributes", "[xmp]") {
    std::map<std::string, std::string> attrs;
    REQUIRE(XmpDocument::parse(kGoldenSample, attrs));

    REQUIRE(attrs.size() == 7);
    REQUIRE(attrs.count("Exposure2012") == 1);
    REQUIRE(attrs.at("Exposure2012") == "+0.50");
    REQUIRE(attrs.count("Contrast2012") == 1);
    REQUIRE(attrs.at("Contrast2012") == "+25");
    REQUIRE(attrs.count("Highlights2012") == 1);
    REQUIRE(attrs.at("Highlights2012") == "-40");
    REQUIRE(attrs.count("Temperature") == 1);
    REQUIRE(attrs.at("Temperature") == "4000");
    REQUIRE(attrs.count("Tint") == 1);
    REQUIRE(attrs.at("Tint") == "+10");
    REQUIRE(attrs.count("Clarity2012") == 1);
    REQUIRE(attrs.at("Clarity2012") == "+30");
    REQUIRE(attrs.count("ToneCurvePV2012") == 1);
    REQUIRE(attrs.at("ToneCurvePV2012") ==
              "0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255");

    // The exif: namespace attribute is not in the crs namespace and must be
    // ignored.
    REQUIRE(attrs.count("UnknownTag") == 0);
    REQUIRE(attrs.count("exif:UnknownTag") == 0);
}

TEST_CASE("XmpDocument: serialize then parse round-trips the map", "[xmp]") {
    const std::map<std::string, std::string> in = {
        {"Exposure2012", "+0.50"},
        {"Contrast2012", "+25"},
        {"Temperature", "4000"},
        {"Tint", "+10"},
        {"ToneCurvePV2012", "0, 0, 64, 80, 120, 128, 163, 168, 209, 210, 255, 255"},
    };

    const std::string xml = XmpDocument::serialize(in, "photo-123");
    REQUIRE_FALSE(xml.empty());

    std::map<std::string, std::string> out;
    REQUIRE(XmpDocument::parse(xml, out));
    REQUIRE(out == in);
}

TEST_CASE("XmpDocument: malformed XML fails to parse", "[xmp]") {
    // Unclosed element: the rdf:Description tag is never terminated.
    const std::string xml =
        "<x:xmpmeta><rdf:RDF><rdf:Description "
        "xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" "
        "crs:Exposure2012=\"+0.50\"";
    std::map<std::string, std::string> attrs;
    REQUIRE_FALSE(XmpDocument::parse(xml, attrs));
    REQUIRE(attrs.empty());
}

TEST_CASE("XmpDocument: well-formed XML without crs attributes returns false", "[xmp]") {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:exif="http://ns.adobe.com/exif/1.0/"
    exif:UnknownTag="shouldBeIgnored"/>
 </rdf:RDF>
</x:xmpmeta>)";

    std::map<std::string, std::string> attrs;
    REQUIRE_FALSE(XmpDocument::parse(xml, attrs));
    REQUIRE(attrs.empty());
}

TEST_CASE("XmpDocument: serialized output is well-formed and re-parseable", "[xmp]") {
    const std::map<std::string, std::string> in = {
        {"Exposure2012", "+0.50"},
        {"Highlights2012", "-40"},
    };

    const std::string xml = XmpDocument::serialize(in, /*rdfAbout=*/"");
    REQUIRE_FALSE(xml.empty());

    // The serialized text is a minimal but valid XMP RDF document.
    REQUIRE(xml.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") == 0);
    REQUIRE(xml.find("x:xmpmeta") != std::string::npos);
    REQUIRE(xml.find("xmlns:x=\"adobe:ns:meta/\"") != std::string::npos);
    REQUIRE(xml.find("rdf:RDF") != std::string::npos);
    REQUIRE(xml.find("rdf:Description") != std::string::npos);
    REQUIRE(xml.find("xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"") !=
            std::string::npos);
    REQUIRE(xml.find("crs:Exposure2012=\"+0.50\"") != std::string::npos);
    REQUIRE(xml.find("crs:Highlights2012=\"-40\"") != std::string::npos);

    // It re-parses cleanly and yields the same crs attributes.
    std::map<std::string, std::string> out;
    REQUIRE(XmpDocument::parse(xml, out));
    REQUIRE(out == in);
}

TEST_CASE("XmpDocument: collects crs attributes from multiple rdf:Description nodes",
          "[xmp]") {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    crs:Exposure2012="+0.50"/>
  <rdf:Description xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    crs:Contrast2012="+25"/>
 </rdf:RDF>
</x:xmpmeta>)";

    std::map<std::string, std::string> attrs;
    REQUIRE(XmpDocument::parse(xml, attrs));
    REQUIRE(attrs.size() == 2);
    REQUIRE(attrs.at("Exposure2012") == "+0.50");
    REQUIRE(attrs.at("Contrast2012") == "+25");
}
