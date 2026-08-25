#pragma once

// Disk thumbnail cache. Generation happens on background threads and never
// blocks the UI.
//
//   * Embedded JPEG previews are extracted when they are large enough (fast).
//   * Otherwise the raw is decoded at reduced size and rendered through the
//     pipeline at default settings (accurate but slower).
//
// Thumbnails and 1:1 previews are rendered to JPEG bytes in memory and embedded
// in the catalog file's previews table (M8); the UI reads them from there, not
// from this disk cache. A legacy disk cache lives under <cacheDir>/thumbnails
// and holds <fullres>/<key>.jpg for old catalogs. Cache keys carry a version
// suffix so stale entries -- including any pre-M8 .ppm thumbnails -- are never
// matched, and the disk usage is bounded by an LRU eviction pass (by file
// mtime) run at startup and after every write.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine/ImageBuffer.h"

namespace lrc {

class Pipeline;
class RawDecoder;

class ThumbnailCache {
public:
    // `memCapacityBytes` is retained for source compatibility (the in-memory
    // LRU tier was removed in M8); callers pass only `cacheDir`.
    explicit ThumbnailCache(std::filesystem::path cacheDir,
                            size_t memCapacityBytes = 64ull * 1024 * 1024);

    // Returns the on-disk path of an existing full-res (1:1) preview for
    // rawPath, or nullopt when not yet built.
    std::optional<std::filesystem::path> findFullRes(
        const std::filesystem::path& rawPath) const;

    // Builds a full-res (1:1) display sRGB JPEG at <diskDir>/fullres/<key>.jpg
    // by decoding at full resolution and rendering through the pipeline with
    // default adjustments. Returns the path when (re)built or already present.
    std::optional<std::filesystem::path> ensureFullRes(
        const std::filesystem::path& rawPath, RawDecoder& decoder,
        Pipeline& pipeline);

    // Serializes an already-rendered display sRGB U8 buffer to the full-res
    // cache slot for rawPath (used by PreviewWorker's 1:1 write-back).
    bool writeFullRes(const std::filesystem::path& rawPath,
                      const ImageBuffer& rgbU8);

    // Stable content hash used for cache file names.
    static std::string stableKey(const std::string& input);

    // JPEG bytes plus pixel dimensions, produced without touching the disk
    // cache (the caller stores them, e.g. inside the catalog file).
    struct RenderedJpeg {
        std::vector<uint8_t> bytes;
        int width = 0;
        int height = 0;
    };
    // Renders + encodes a thumbnail JPEG (longest side ~targetDim) at default
    // settings. Returns bytes + dimensions, or nullopt on failure.
    std::optional<RenderedJpeg> renderThumbJpeg(
        const std::filesystem::path& rawPath, int targetDim,
        RawDecoder& decoder, Pipeline& pipeline);
    // Renders + encodes a full-res (1:1) display sRGB JPEG at default
    // settings. Returns bytes + dimensions, or nullopt on failure.
    std::optional<RenderedJpeg> renderFullResJpeg(
        const std::filesystem::path& rawPath, RawDecoder& decoder,
        Pipeline& pipeline);

private:
    std::filesystem::path diskDir_;

    // Full-res cache slot for rawPath: <diskDir>/fullres/<key>.jpg, keyed by a
    // stable hash of the canonical path plus "@fullres@v2" (versioned so stale
    // pre-M5-2 entries -- old .ppm thumbnails and pre-v2 JPEGs -- are ignored).
    // Shared by findFullRes / ensureFullRes / writeFullRes.
    std::filesystem::path fullResPathFor(
        const std::filesystem::path& rawPath) const;

    // Enforces the disk-cache caps: deletes the oldest files (by mtime) across
    // the whole <diskDir_> tree until both the byte and entry-count caps are
    // satisfied. Must be called with mutex_ held; the constructor calls it
    // before the object is shared with worker threads.
    void pruneLocked();

    // Disk cache caps (M5-2). Eviction is triggered once either is exceeded.
    static constexpr std::uintmax_t kDiskMaxBytes = 1ull << 30;  // 1 GiB
    static constexpr std::size_t kDiskMaxEntries = 100000;       // ~100k files

    // findFullRes() is const but locks the mutex, so it stays mutable.
    mutable std::mutex mutex_;
};

} // namespace lrc
