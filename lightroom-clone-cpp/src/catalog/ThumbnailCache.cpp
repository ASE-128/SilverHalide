#include "catalog/ThumbnailCache.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "catalog/StandardPreview.h"
#include "engine/Adjustments.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"
#include "utils/MathUtils.h"

namespace lrc {

namespace {

// FNV-1a 64-bit stable hash.
uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Version marker appended to cache-key inputs (M5-2). Bumping it invalidates
// every previously-cached disk entry -- pre-M8 .ppm thumbnails and pre-v2
// fullres JPEGs alike -- so stale files are never matched on lookup and are
// only reclaimed later by the LRU disk-cap pass.
constexpr const char* kCacheKeyVersion = "@v2";

} // namespace

ThumbnailCache::ThumbnailCache(std::filesystem::path cacheDir,
                               size_t /*memCapacityBytes*/)
    : diskDir_(std::move(cacheDir) / "thumbnails") {
    // Startup: enforce the disk-cache caps once, before the object is shared
    // with worker threads. No lock needed (construction is single-threaded).
    pruneLocked();
}

std::string ThumbnailCache::stableKey(const std::string& input) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(input)));
    return buf;
}

std::filesystem::path ThumbnailCache::fullResPathFor(
    const std::filesystem::path& rawPath) const {
    const std::string canonical =
        std::filesystem::weakly_canonical(rawPath).string();
    const std::string key =
        stableKey(canonical + "@fullres" + kCacheKeyVersion);
    return diskDir_ / "fullres" / (key + ".jpg");
}

std::optional<std::filesystem::path> ThumbnailCache::findFullRes(
    const std::filesystem::path& rawPath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    const auto path = fullResPathFor(rawPath);
    if (std::filesystem::exists(path, ec))
        return path;
    return std::nullopt;
}

std::optional<std::filesystem::path> ThumbnailCache::ensureFullRes(
    const std::filesystem::path& rawPath, RawDecoder& decoder,
    Pipeline& pipeline) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    if (!std::filesystem::exists(rawPath, ec))
        return std::nullopt;
    const auto path = fullResPathFor(rawPath);
    if (std::filesystem::exists(path, ec))
        return path; // idempotent: already present

    std::filesystem::create_directories(diskDir_ / "fullres", ec);

    // Default DecodeParams: full resolution (halfSize=false, maxDim=0).
    DecodeParams params;
    auto decoded = decoder.decode(rawPath, params);
    if (!decoded)
        return std::nullopt;

    const std::string canonical =
        std::filesystem::weakly_canonical(rawPath).string();
    const uint64_t imageId = fnv1a64(canonical);
    ImageBufferPtr u8 = pipeline.renderFullDisplay(
        imageId, *decoded->linear, Adjustments::defaults());
    if (!u8 || u8->empty())
        return std::nullopt;
    if (!ImageIO::writeJpeg(path, *u8, 92))
        return std::nullopt;
    pruneLocked();
    return path;
}

bool ThumbnailCache::writeFullRes(const std::filesystem::path& rawPath,
                                  const ImageBuffer& rgbU8) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(diskDir_ / "fullres", ec);
    const bool ok = ImageIO::writeJpeg(fullResPathFor(rawPath), rgbU8, 92);
    if (ok)
        pruneLocked();
    return ok;
}

void ThumbnailCache::pruneLocked() {
    std::error_code ec;
    if (!std::filesystem::exists(diskDir_, ec))
        return;

    // One pass over the whole cache tree (the fullres/ subdir plus any legacy
    // top-level thumbnails). Best-effort: permission errors skip an entry, and
    // a mid-scan failure aborts the pass without deleting anything.
    struct CacheEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
    };
    std::vector<CacheEntry> files;
    std::uintmax_t totalBytes = 0;

    std::filesystem::recursive_directory_iterator it(
        diskDir_,
        std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return;
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec))
            continue;
        const std::uintmax_t size = it->file_size(fec);
        if (fec)
            continue;
        const auto mtime = it->last_write_time(fec);
        if (fec)
            continue;
        totalBytes += size;
        files.push_back(CacheEntry{it->path(), mtime, size});
    }
    if (ec)
        return; // scan aborted on a partial view: refuse to delete.

    if (files.size() <= kDiskMaxEntries && totalBytes <= kDiskMaxBytes)
        return; // within limits.

    // LRU eviction: delete oldest files (by mtime) until both caps are met.
    std::sort(files.begin(), files.end(),
              [](const CacheEntry& a, const CacheEntry& b) {
                  return a.mtime < b.mtime;
              });

    std::size_t remaining = files.size();
    std::uintmax_t remainingBytes = totalBytes;
    for (const CacheEntry& e : files) {
        if (remaining <= kDiskMaxEntries && remainingBytes <= kDiskMaxBytes)
            break;
        std::error_code rec;
        std::filesystem::remove(e.path, rec);
        if (!rec) {
            --remaining;
            remainingBytes -= e.size;
        }
        // A failed removal (permission, file in use) keeps the entry counted so
        // the rest of the list still gets a chance to evict.
    }
}

std::optional<ThumbnailCache::RenderedJpeg> ThumbnailCache::renderThumbJpeg(
    const std::filesystem::path& rawPath, int targetDim, RawDecoder& decoder,
    Pipeline& pipeline) {
    // Reduced-size decode, default render, then JPEG-encode to memory (no disk
    // cache write; the caller stores the bytes, e.g. in the catalog file).
    DecodeParams params;
    params.halfSize = true;
    params.maxDim = targetDim * 2; // oversample, pipeline downscales
    auto decoded = decoder.decode(rawPath, params);
    if (!decoded)
        return std::nullopt;

    const std::string canonical =
        std::filesystem::weakly_canonical(rawPath).string();
    const uint64_t imageId = fnv1a64(canonical);
    ImageBufferPtr u8 = pipeline.render(imageId, *decoded->linear,
                                        Adjustments::defaults(), targetDim);
    if (!u8 || u8->empty())
        return std::nullopt;

    RenderedJpeg out;
    out.bytes = ImageIO::encodeJpeg(*u8, 88);
    if (out.bytes.empty())
        return std::nullopt;
    out.width = u8->width();
    out.height = u8->height();
    return out;
}

std::optional<ThumbnailCache::RenderedJpeg> ThumbnailCache::renderFullResJpeg(
    const std::filesystem::path& rawPath, RawDecoder& decoder,
    Pipeline& pipeline) {
    // Full-resolution decode, default full-res display render, then JPEG-encode
    // to memory (no disk cache write).
    DecodeParams params; // full resolution (halfSize=false, maxDim=0)
    auto decoded = decoder.decode(rawPath, params);
    if (!decoded)
        return std::nullopt;

    const std::string canonical =
        std::filesystem::weakly_canonical(rawPath).string();
    const uint64_t imageId = fnv1a64(canonical);
    ImageBufferPtr u8 = pipeline.renderFullDisplay(
        imageId, *decoded->linear, Adjustments::defaults());
    if (!u8 || u8->empty())
        return std::nullopt;

    RenderedJpeg out;
    out.bytes = ImageIO::encodeJpeg(*u8, 92);
    if (out.bytes.empty())
        return std::nullopt;
    out.width = u8->width();
    out.height = u8->height();
    return out;
}

std::optional<ThumbnailCache::RenderedStandardPreview>
ThumbnailCache::renderStandardPreviewBlob(
    const std::filesystem::path& rawPath) {
    // S5: one-call wrapper around buildStandardPreview2 + encodeStandardPreview
    // (v2 header). Lets ImportWorker's batch loop produce the catalog blob
    // without pulling in StandardPreview.h or wiring a decoder/pipeline.
    const auto built =
        buildStandardPreview2(lrc::pathToUtf8(rawPath));
    if (!built.buf || built.buf->empty())
        return std::nullopt;

    auto blob = encodeStandardPreview(*built.buf, built.nativeWidth,
                                       built.nativeHeight, built.sourceColor);
    if (blob.empty())
        return std::nullopt;

    RenderedStandardPreview out;
    out.blob = std::move(blob);
    out.previewWidth = built.buf->width();
    out.previewHeight = built.buf->height();
    out.nativeWidth = built.nativeWidth;
    out.nativeHeight = built.nativeHeight;
    return out;
}

} // namespace lrc
