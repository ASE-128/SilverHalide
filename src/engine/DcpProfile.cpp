#include "engine/DcpProfile.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace lrc {
namespace {

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size <= 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    return input ? bytes : std::vector<uint8_t>{};
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t offset, bool le) {
    if (offset + 2 > bytes.size())
        return 0;
    return le ? static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8))
              : static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset, bool le) {
    if (offset + 4 > bytes.size())
        return 0;
    if (le)
        return static_cast<uint32_t>(bytes[offset]) |
               (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
               (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
               (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

bool hasTag(const std::vector<uint8_t>& bytes, uint16_t wanted, bool le) {
    if (bytes.size() < 8)
        return false;
    const uint32_t ifd = read32(bytes, 4, le);
    if (static_cast<uint64_t>(ifd) + 2 > bytes.size())
        return false;
    const uint16_t count = read16(bytes, ifd, le);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
        if (entry + 12 > bytes.size())
            return false;
        if (read16(bytes, entry, le) == wanted)
            return true;
    }
    return false;
}

struct TagValue {
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t offset = 0;
    size_t entry = 0;
};

bool findTag(const std::vector<uint8_t>& bytes, uint16_t wanted, bool le,
             TagValue& value) {
    if (bytes.size() < 8)
        return false;
    const uint32_t ifd = read32(bytes, 4, le);
    if (static_cast<uint64_t>(ifd) + 2 > bytes.size())
        return false;
    const uint16_t count = read16(bytes, ifd, le);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = static_cast<size_t>(ifd) + 2 + i * 12;
        if (entry + 12 > bytes.size())
            return false;
        if (read16(bytes, entry, le) == wanted) {
            value.type = read16(bytes, entry + 2, le);
            value.count = read32(bytes, entry + 4, le);
            value.offset = read32(bytes, entry + 8, le);
            value.entry = entry;
            return true;
        }
    }
    return false;
}

size_t typeSize(uint16_t type) {
    switch (type) {
    case 1: return 1;  // BYTE
    case 2: return 1;  // ASCII
    case 3: return 2;  // SHORT
    case 4: return 4;  // LONG
    case 5: return 8;  // RATIONAL
    case 7: return 1;  // UNDEFINED
    case 9: return 4;  // SLONG
    case 10: return 8; // SRATIONAL
    default: return 0;
    }
}

size_t valueOffset(const TagValue& tag) {
    return static_cast<size_t>(tag.count) * typeSize(tag.type) <= 4
               ? tag.entry + 8
               : tag.offset;
}

std::string readAscii(const std::vector<uint8_t>& bytes, const TagValue& tag) {
    if (tag.type != 2 || tag.count == 0 || tag.count > bytes.size())
        return {};
    const size_t offset = valueOffset(tag);
    if (offset + tag.count > bytes.size())
        return {};
    const auto* begin = reinterpret_cast<const char*>(bytes.data() + offset);
    size_t length = 0;
    while (length < tag.count && begin[length] != '\0')
        ++length;
    return std::string(begin, length);
}

bool readMatrix(const std::vector<uint8_t>& bytes, const TagValue& tag,
                bool le, std::array<float, 9>& matrix) {
    if (tag.type != 10 || tag.count != matrix.size())
        return false;
    const size_t offset = valueOffset(tag);
    if (offset + tag.count * 8 > bytes.size())
        return false;
    for (size_t i = 0; i < matrix.size(); ++i) {
        const size_t item = offset + i * 8;
        const int32_t numerator = static_cast<int32_t>(read32(bytes, item, le));
        const int32_t denominator = static_cast<int32_t>(read32(bytes, item + 4, le));
        if (denominator == 0)
            return false;
        matrix[i] = static_cast<float>(numerator) / denominator;
    }
    return true;
}

// Reads one DCP curve sample (ToneCurve tag 50940) whose storage type varies
// by encoder: SHORT(3) / UNDEFINED(7) are 16-bit; BYTE(1) is a single byte.
// Values are returned as their raw numeric magnitude (DCP stores 16-bit curve
// magnitudes, so typical samples land in [0, 65535]).
float readCurveSample(const std::vector<uint8_t>& bytes, size_t offset,
                      uint16_t type, bool le) {
    switch (type) {
    case 1:  // BYTE
        return offset < bytes.size() ? static_cast<float>(bytes[offset]) : 0.0f;
    case 3:  // SHORT
    case 7:  // UNDEFINED (commonly carries the 16-bit curve)
        return offset + 2 <= bytes.size()
                   ? static_cast<float>(read16(bytes, offset, le))
                   : 0.0f;
    default:
        return 0.0f;
    }
}

} // namespace

bool DcpProfileReader::isDcp(const std::filesystem::path& path) {
    const auto bytes = readFile(path);
    if (bytes.size() < 8)
        return false;
    const bool le = bytes[0] == 'I' && bytes[1] == 'I';
    const bool be = bytes[0] == 'M' && bytes[1] == 'M';
    // Adobe DCP files use the TIFF IFD layout but identify themselves with
    // the little-endian "RC" signature (the bytes are "IIRC").
    if ((!le && !be) ||
        (read16(bytes, 2, le) != 42 && read16(bytes, 2, le) != 0x4352))
        return false;
        // ProfileName (50936) is an Adobe DCP tag. Requiring it avoids classifying
        // an ordinary DNG, which may contain ColorMatrix1/2, as a DCP profile.
        return hasTag(bytes, 50936, le) && hasTag(bytes, 50940, le);
}

std::optional<DcpProfile> DcpProfileReader::read(
    const std::filesystem::path& path) {
    if (!isDcp(path))
        return std::nullopt;
    const auto bytes = readFile(path);
    const bool le = bytes.size() >= 2 && bytes[0] == 'I' && bytes[1] == 'I';
    DcpProfile profile;
    TagValue tag;
    if (findTag(bytes, 50936, le, tag))
        profile.name = readAscii(bytes, tag);
    if (findTag(bytes, 50708, le, tag))
        profile.cameraModel = readAscii(bytes, tag);
    if (findTag(bytes, 50721, le, tag))
        profile.hasColorMatrix1 = readMatrix(bytes, tag, le, profile.colorMatrix1);
    if (findTag(bytes, 50722, le, tag))
        profile.hasColorMatrix2 = readMatrix(bytes, tag, le, profile.colorMatrix2);
    // Tone curve (50940) carries the DCP output curve as a run of 16-bit
    // samples. `count` is the full sample count; we keep up to the curve array
    // capacity (most profiles ship 256 points). The curve domain is implicit:
    // input x is spread uniformly across the samples from 0 to 65535.
    if (findTag(bytes, 50940, le, tag) && tag.count > 0) {
        const size_t sample = typeSize(tag.type) ? typeSize(tag.type) : 1;
        const size_t n = std::min<size_t>(tag.count, profile.toneCurve.size());
        const size_t offset = valueOffset(tag);
        for (size_t i = 0; i < n; ++i)
            profile.toneCurve[i] =
                readCurveSample(bytes, offset + i * sample, tag.type, le);
        profile.toneCurveSize = n;
    }
    if (profile.name.empty())
        profile.name = path.stem().string();
    return profile;
}

} // namespace lrc
