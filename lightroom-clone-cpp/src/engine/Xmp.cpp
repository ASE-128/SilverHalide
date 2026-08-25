#include "engine/Xmp.h"

#include <pugixml.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace lrc {

namespace {

// pugi::xml_writer that appends every written chunk to a std::string.  Used to
// capture the serialized XMP document (XmpDocument::serialize*).
class StringWriter final : public pugi::xml_writer {
public:
    void write(const void* data, size_t size) override {
        out_.append(static_cast<const char*>(data), size);
    }

    std::string str() const { return out_; }

private:
    std::string out_;
};

// Local part of a (possibly prefix-qualified) XML name.  "rdf:Description"
// yields "Description"; a name with no prefix is returned unchanged.
const char* localName(const char* name) {
    const char* colon = std::strrchr(name, ':');
    return colon ? colon + 1 : name;
}

// Namespace URIs (Lightroom / Dublin Core / RDF).
constexpr const char* kCrsNs = "http://ns.adobe.com/camera-raw-settings/1.0/";
constexpr const char* kDcNs = "http://purl.org/dc/elements/1.1/";
constexpr const char* kLrNs = "http://ns.adobe.com/lightroom/1.0/";
constexpr const char* kRdfNs = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

// True when `node` is an element whose local name equals `local` and whose
// literal prefix equals `prefix` (empty prefix matches any / no prefix).  The
// prefix is matched literally, mirroring how the rest of this file treats
// "crs:"/"dc:"/"lr:" - XMP writers overwhelmingly use these fixed prefixes.
bool isElement(const pugi::xml_node& node, const char* local,
               const char* prefix) {
    if (node.type() != pugi::node_element)
        return false;
    const char* name = node.name();
    const char* colon = std::strrchr(name, ':');
    const char* nodeLocal = colon ? colon + 1 : name;
    if (std::strcmp(nodeLocal, local) != 0)
        return false;
    if (!prefix || !*prefix)
        return true;
    const size_t plen = std::strlen(prefix);
    if (!colon || static_cast<size_t>(colon - name) != plen)
        return false;
    return std::strncmp(name, prefix, plen) == 0;
}

// First direct child element matching `local`/`prefix`.
pugi::xml_node findChild(const pugi::xml_node& parent, const char* local,
                         const char* prefix) {
    for (pugi::xml_node c : parent.children()) {
        if (isElement(c, local, prefix))
            return c;
    }
    return pugi::xml_node();
}

// First descendant element (any depth, depth-first) matching `local`/`prefix`.
pugi::xml_node findDescendant(const pugi::xml_node& parent, const char* local,
                              const char* prefix) {
    for (pugi::xml_node c : parent.children()) {
        if (c.type() != pugi::node_element)
            continue;
        if (isElement(c, local, prefix))
            return c;
        pugi::xml_node found = findDescendant(c, local, prefix);
        if (found)
            return found;
    }
    return pugi::xml_node();
}

// Collects the text of every rdf:li descendant into `out`.
void collectLiTexts(const pugi::xml_node& parent,
                    std::vector<std::string>& out) {
    for (pugi::xml_node c : parent.children()) {
        if (c.type() != pugi::node_element)
            continue;
        if (isElement(c, "li", "rdf"))
            out.push_back(c.text().as_string());
        collectLiTexts(c, out);
    }
}

// First rdf:li descendant's text; falls back to the element's own text.
void firstText(const pugi::xml_node& node, std::string& out) {
    pugi::xml_node li = findDescendant(node, "li", "rdf");
    out = li ? li.text().as_string() : node.text().as_string();
}

// Reads a property that may appear either as an XML attribute
// (prefix:Local="value") or as an element (<prefix:Local>value</prefix:Local>),
// matching how Lightroom writes structured values.
std::string readProperty(const pugi::xml_node& node, const char* local,
                         const char* prefix) {
    const std::string qualified = std::string(prefix) + ":" + local;
    pugi::xml_attribute a = node.attribute(qualified.c_str());
    if (a)
        return std::string(a.value());
    pugi::xml_node child = findChild(node, local, prefix);
    if (child)
        return std::string(child.text().as_string());
    return std::string();
}

// --- crs:SpotCorrections parsing -------------------------------------------

// Parses one rdf:li spot-correction item.  Handles both the project's write
// shape (properties as crs: attributes on a nested rdf:Description) and
// Lightroom's element-form shape (properties as child elements of an
// rdf:parseType="Resource" li).
void parseSpotLi(const pugi::xml_node& li, XmpDocument::SpotCorrectionXml& sc) {
    pugi::xml_node desc = findChild(li, "Description", "rdf");
    pugi::xml_node container = desc ? desc : li;

    sc.type = readProperty(container, "CorrectionType", "crs");
    sc.source = readProperty(container, "CorrectionSource", "crs");
    sc.amount = readProperty(container, "CorrectionAmount", "crs");
    sc.active = readProperty(container, "CorrectionActive", "crs");
    sc.sourceX = readProperty(container, "SourceX", "crs");
    sc.sourceY = readProperty(container, "SourceY", "crs");

    // The circular mask: accept both the project's singular crs:CorrectionMask
    // and Lightroom's plural crs:CorrectionMasks.
    pugi::xml_node mask = findDescendant(li, "CorrectionMask", "crs");
    if (!mask)
        mask = findDescendant(li, "CorrectionMasks", "crs");
    if (mask) {
        pugi::xml_node mli = findDescendant(mask, "li", "rdf");
        if (mli) {
            pugi::xml_node mdesc = findChild(mli, "Description", "rdf");
            pugi::xml_node mcontainer = mdesc ? mdesc : mli;
            sc.centerX = readProperty(mcontainer, "CenterX", "crs");
            sc.centerY = readProperty(mcontainer, "CenterY", "crs");
            sc.radius = readProperty(mcontainer, "Radius", "crs");
        }
    }
}

// Recursively walks `parent`; every crs:SpotCorrections rdf:Bag item is parsed
// into `out`.  Degenerate empty items are dropped.
void collectSpotCorrections(const pugi::xml_node& parent,
                            std::vector<XmpDocument::SpotCorrectionXml>& out) {
    for (pugi::xml_node node : parent.children()) {
        if (node.type() != pugi::node_element)
            continue;
        if (isElement(node, "SpotCorrections", "crs")) {
            for (pugi::xml_node bag : node.children()) {
                if (!isElement(bag, "Bag", "rdf"))
                    continue;
                for (pugi::xml_node li : bag.children()) {
                    if (!isElement(li, "li", "rdf"))
                        continue;
                    XmpDocument::SpotCorrectionXml sc;
                    parseSpotLi(li, sc);
                    if (!sc.type.empty() || !sc.centerX.empty() ||
                        !sc.centerY.empty() || !sc.radius.empty() ||
                        !sc.active.empty()) {
                        out.push_back(std::move(sc));
                    }
                }
            }
        }
        collectSpotCorrections(node, out);
    }
}

// Appends the crs:SpotCorrections element to `description`.
void appendSpotCorrections(pugi::xml_node description,
                           const std::vector<XmpDocument::SpotCorrectionXml>& spots) {
    pugi::xml_node corrections = description.append_child("crs:SpotCorrections");
    pugi::xml_node bag = corrections.append_child("rdf:Bag");
    for (const XmpDocument::SpotCorrectionXml& s : spots) {
        pugi::xml_node li = bag.append_child("rdf:li");
        pugi::xml_node inner = li.append_child("rdf:Description");
        auto putAttr = [&](const char* name, const std::string& value) {
            if (!value.empty())
                inner.append_attribute(name) = value.c_str();
        };
        putAttr("crs:CorrectionType", s.type);
        putAttr("crs:CorrectionSource", s.source);
        putAttr("crs:CorrectionAmount", s.amount);
        putAttr("crs:CorrectionActive", s.active);
        putAttr("crs:SourceX", s.sourceX);
        putAttr("crs:SourceY", s.sourceY);

        pugi::xml_node mask = inner.append_child("crs:CorrectionMask");
        pugi::xml_node mbag = mask.append_child("rdf:Bag");
        pugi::xml_node mli = mbag.append_child("rdf:li");
        pugi::xml_node mdesc = mli.append_child("rdf:Description");
        auto putMask = [&](const char* name, const std::string& value) {
            if (!value.empty())
                mdesc.append_attribute(name) = value.c_str();
        };
        putMask("crs:CenterX", s.centerX);
        putMask("crs:CenterY", s.centerY);
        putMask("crs:Radius", s.radius);
    }
}

// --- dc:/lr: metadata --------------------------------------------------------

// Recursively walks `parent`, filling `out` from dc:/lr: elements and
// `hierarchical` from lr:hierarchicalSubject.
void collectMetadataNodes(const pugi::xml_node& parent, XmpMetadata& out,
                          std::vector<std::string>& hierarchical) {
    for (pugi::xml_node node : parent.children()) {
        if (node.type() != pugi::node_element)
            continue;
        if (isElement(node, "subject", "dc")) {
            collectLiTexts(node, out.keywords);
        } else if (isElement(node, "hierarchicalSubject", "lr")) {
            collectLiTexts(node, hierarchical);
        } else if (isElement(node, "title", "dc")) {
            firstText(node, out.title);
        } else if (isElement(node, "description", "dc")) {
            firstText(node, out.caption);
        } else if (isElement(node, "rights", "dc")) {
            firstText(node, out.copyright);
        } else if (isElement(node, "creator", "dc")) {
            firstText(node, out.creator);
        }
        collectMetadataNodes(node, out, hierarchical);
    }
}

// Appends dc:/lr: metadata elements to `description`.  The xmlns:dc and
// xmlns:lr declarations are added only when a value actually needs them.
void appendMetadata(pugi::xml_node description, const XmpMetadata& md) {
    const bool hasDc = !md.keywords.empty() || !md.title.empty() ||
                       !md.caption.empty() || !md.copyright.empty() ||
                       !md.creator.empty();
    const bool hasLr = !md.keywords.empty();
    if (hasDc)
        description.append_attribute("xmlns:dc") = kDcNs;
    if (hasLr)
        description.append_attribute("xmlns:lr") = kLrNs;

    if (hasLr) {
        // dc:subject carries the keyword labels as-is; lr:hierarchicalSubject
        // carries the full hierarchy with '|' separators (Lightroom uses '|').
        pugi::xml_node subject = description.append_child("dc:subject");
        pugi::xml_node sbag = subject.append_child("rdf:Bag");
        pugi::xml_node hier = description.append_child("lr:hierarchicalSubject");
        pugi::xml_node hbag = hier.append_child("rdf:Bag");
        for (const std::string& kw : md.keywords) {
            pugi::xml_node sli = sbag.append_child("rdf:li");
            sli.append_child(pugi::node_pcdata).set_value(kw.c_str());

            std::string normalized = kw;
            for (char& c : normalized)
                if (c == '/')
                    c = '|';
            pugi::xml_node hli = hbag.append_child("rdf:li");
            hli.append_child(pugi::node_pcdata).set_value(normalized.c_str());
        }
    }

    // Single-value fields use rdf:Alt with an xml:lang="x-default" entry.
    auto appendAlt = [&](const char* name, const std::string& value) {
        if (value.empty())
            return;
        pugi::xml_node elem = description.append_child(name);
        pugi::xml_node alt = elem.append_child("rdf:Alt");
        pugi::xml_node li = alt.append_child("rdf:li");
        li.append_attribute("xml:lang") = "x-default";
        li.append_child(pugi::node_pcdata).set_value(value.c_str());
    };
    // dc:creator is an rdf:Seq of one entry.
    auto appendSeq = [&](const char* name, const std::string& value) {
        if (value.empty())
            return;
        pugi::xml_node elem = description.append_child(name);
        pugi::xml_node seq = elem.append_child("rdf:Seq");
        pugi::xml_node li = seq.append_child("rdf:li");
        li.append_child(pugi::node_pcdata).set_value(value.c_str());
    };

    appendAlt("dc:title", md.title);
    appendAlt("dc:description", md.caption);
    appendAlt("dc:rights", md.copyright);
    appendSeq("dc:creator", md.creator);
}

// Recursively walk every element under `parent`.  Any element whose local name
// is "Description" contributes the attributes that carry the Lightroom
// camera-raw-settings prefix (crs:); the prefix is stripped from the map key
// and every other attribute/namespace is ignored.
void collectCrsAttrs(const pugi::xml_node& parent,
                     std::map<std::string, std::string>& out) {
    for (pugi::xml_node node : parent.children()) {
        if (node.type() != pugi::node_element) {
            continue;
        }
        if (std::strcmp(localName(node.name()), "Description") == 0) {
            for (pugi::xml_attribute attr : node.attributes()) {
                const char* name = attr.name();
                // Lightroom always writes crs attributes with the literal
                // "crs:" prefix; match the prefix rather than resolving the
                // namespace URI (the xmlns:crs declaration itself is ignored
                // because its name does not start with "crs:").
                if (name && std::strncmp(name, "crs:", 4) == 0) {
                    out[name + 4] = attr.value();
                }
            }
        }
        collectCrsAttrs(node, out);
    }
}

} // namespace

bool XmpDocument::parse(const std::string& xmlText,
                        std::map<std::string, std::string>& outCrsAttrs) {
    outCrsAttrs.clear();

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xmlText.c_str());
    if (!result) {
        return false;
    }

    collectCrsAttrs(doc, outCrsAttrs);
    return !outCrsAttrs.empty();
}

bool XmpDocument::parseSpotCorrections(
    const std::string& xmlText, std::vector<SpotCorrectionXml>& out) {
    out.clear();

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xmlText.c_str());
    if (!result) {
        return false;
    }

    collectSpotCorrections(doc, out);
    return !out.empty();
}

bool XmpDocument::readMetadata(const std::string& xmlText, XmpMetadata& out) {
    out = XmpMetadata{};

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xmlText.c_str());
    if (!result) {
        return false;
    }

    XmpMetadata raw;
    std::vector<std::string> hierarchical;
    collectMetadataNodes(doc, raw, hierarchical);

    // Restore keywords: lr:hierarchicalSubject is the authoritative full-path
    // source ('|' canonical separator, '/' tolerated); dc:subject entries are
    // appended after, deduped on the '|'-normalized form so our own round-trip
    // (which writes every keyword to both elements) does not duplicate them.
    out.title = raw.title;
    out.caption = raw.caption;
    out.copyright = raw.copyright;
    out.creator = raw.creator;

    auto normalize = [](std::string s) {
        for (char& c : s)
            if (c == '/')
                c = '|';
        return s;
    };
    auto pushUnique = [&](const std::string& s) {
        const std::string norm = normalize(s);
        for (const std::string& existing : out.keywords) {
            if (normalize(existing) == norm)
                return;
        }
        out.keywords.push_back(norm);
    };

    out.keywords.reserve(hierarchical.size() + raw.keywords.size());
    for (const std::string& k : hierarchical)
        pushUnique(k);
    for (const std::string& k : raw.keywords)
        pushUnique(k);

    return true;
}

std::string XmpDocument::serializeSpotCorrections(
    const std::vector<SpotCorrectionXml>& spots) {
    // Emit a minimal XMP document whose single rdf:Description carries only the
    // crs:SpotCorrections child (used for testing and metadata-only writes).
    pugi::xml_document doc;

    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
    xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";

    pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
    rdf.append_attribute("xmlns:rdf") = kRdfNs;

    pugi::xml_node description = rdf.append_child("rdf:Description");
    description.append_attribute("xmlns:crs") = kCrsNs;
    appendSpotCorrections(description, spots);

    StringWriter writer;
    doc.print(writer, "  ", pugi::format_default, pugi::encoding_utf8);
    return writer.str();
}

std::string XmpDocument::serializeMetadata(const XmpMetadata& md) {
    pugi::xml_document doc;

    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
    xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";

    pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
    rdf.append_attribute("xmlns:rdf") = kRdfNs;

    pugi::xml_node description = rdf.append_child("rdf:Description");
    appendMetadata(description, md);

    StringWriter writer;
    doc.print(writer, "  ", pugi::format_default, pugi::encoding_utf8);
    return writer.str();
}

std::string XmpDocument::serialize(const std::map<std::string, std::string>& crsAttrs,
                                   const std::string& rdfAbout) {
    return serializeFull(crsAttrs, rdfAbout, nullptr, nullptr);
}

std::string XmpDocument::serializeFull(
    const std::map<std::string, std::string>& crsAttrs,
    const std::string& rdfAbout, const XmpMetadata* metadata,
    const std::vector<SpotCorrectionXml>* spots) {
    pugi::xml_document doc;

    // <?xml version="1.0" encoding="UTF-8"?>
    pugi::xml_node decl = doc.append_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    // <x:xmpmeta xmlns:x="adobe:ns:meta/">
    pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
    xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";

    // <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
    rdf.append_attribute("xmlns:rdf") = kRdfNs;

    // <rdf:Description xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    //   rdf:about="..." crs:Name="value" .../>
    pugi::xml_node description = rdf.append_child("rdf:Description");
    description.append_attribute("xmlns:crs") = kCrsNs;
    if (!rdfAbout.empty()) {
        description.append_attribute("rdf:about") = rdfAbout.c_str();
    }
    for (const auto& entry : crsAttrs) {
        description.append_attribute(("crs:" + entry.first).c_str()) =
            entry.second.c_str();
    }

    if (metadata) {
        appendMetadata(description, *metadata);
    }
    if (spots && !spots->empty()) {
        appendSpotCorrections(description, *spots);
    }

    StringWriter writer;
    doc.print(writer, "  ", pugi::format_default, pugi::encoding_utf8);
    return writer.str();
}

} // namespace lrc
