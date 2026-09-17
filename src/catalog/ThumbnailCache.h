#pragma once

/**
 * @brief Disk thumbnail / full-res preview cache for the catalog layer.
 *
 * Generation happens on background threads and never blocks the UI:
 *   * Embedded JPEG previews are extracted when they are large enough (fast).
 *   * Otherwise the raw is decoded at reduced size and rendered through the
 *     pipeline at default settings (accurate but slower).
 *
 * Thumbnails and 1:1 previews are rendered to JPEG bytes in memory and embedded
 * in the catalog file's previews table; the UI reads them from there, not from
 * this disk cache. A legacy disk cache lives under <cacheDir>/thumbnails and
 * holds <fullres>/<key>.jpg for old catalogs. Cache keys carry a version
 * suffix so stale entries -- including any legacy .ppm thumbnails -- are never
 * matched, and the disk usage is bounded by an LRU eviction pass (by file
 * mtime) run at startup and after every write.
 */

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
    /** @brief Constructs the disk cache rooted at `cacheDir`/thumbnails.
     *
     *  `memCapacityBytes` is retained for source compatibility only (the
     *  in-memory LRU tier was removed); callers pass just `cacheDir`. */
    explicit ThumbnailCache(std::filesystem::path cacheDir,
                            size_t memCapacityBytes = 64ull * 1024 * 1024);

    /** @brief Returns the on-disk path of an existing full-res (1:1) preview
     *         for `rawPath`, or nullopt when not yet built. */
    std::optional<std::filesystem::path> findFullRes(
        const std::filesystem::path& rawPath) const;

    /** @brief Builds a full-res (1:1) display sRGB JPEG at
     *         <diskDir>/fullres/<key>.jpg.
     *
     *  Decodes at full resolution and renders through the pipeline with
     *  default adjustments. Returns the path when (re)built or already
     *  present, or nullopt when the source is missing / the render fails. */
    std::optional<std::filesystem::path> ensureFullRes(
        const std::filesystem::path& rawPath, RawDecoder& decoder,
        Pipeline& pipeline);

    /** @brief Serializes an already-rendered display sRGB U8 buffer to the
     *         full-res cache slot for rawPath (PreviewWorker's 1:1
     *         write-back).
     *
     *  @return false only on a hard I/O failure. */
    bool writeFullRes(const std::filesystem::path& rawPath,
                      const ImageBuffer& rgbU8);

    /// Stable content hash used for cache file names.
    static std::string stableKey(const std::string& input);

    /** @brief JPEG bytes plus pixel dimensions, produced without touching the
     *         disk cache (the caller stores them, e.g. inside the catalog
     *         file). */
    struct RenderedJpeg {
        std::vector<uint8_t> bytes;
        int width = 0;
        int height = 0;
    };
    /** @brief Renders + encodes a thumbnail JPEG (longest side ~`targetDim`)
     *         at default settings.
     *
     *  @return Bytes + dimensions, or nullopt on failure. */
    std::optional<RenderedJpeg> renderThumbJpeg(
        const std::filesystem::path& rawPath, int targetDim,
        RawDecoder& decoder, Pipeline& pipeline);
    /** @brief Renders + encodes a full-res (1:1) display sRGB JPEG at default
     *         settings.
     *
     *  @return Bytes + dimensions, or nullopt on failure. */
    std::optional<RenderedJpeg> renderFullResJpeg(
        const std::filesystem::path& rawPath, RawDecoder& decoder,
        Pipeline& pipeline);

    /** @brief Renders + encodes a standard preview blob (linear ProPhoto F32,
     *         2048px longest edge, v2 header carrying native dimensions).
     *
     *  Self-contained -- builds its own decoder/colour manager, so callers do
     *  not need to thread a RawDecoder/Pipeline through just to produce the
     *  catalog blob. Returns blob + dimensions, or nullopt on failure.
     */
    struct RenderedStandardPreview {
        std::vector<uint8_t> blob;   // v2 header + F32 RGB payload
        int previewWidth = 0;
        int previewHeight = 0;
        int nativeWidth = 0;
        int nativeHeight = 0;
    };
    std::optional<RenderedStandardPreview> renderStandardPreviewBlob(
        const std::filesystem::path& rawPath);

private:
    std::filesystem::path diskDir_;

    // Full-res cache slot for rawPath: <diskDir>/fullres/<key>.jpg, keyed by a
    // stable hash of the canonical path plus "@fullres@v2" (versioned so stale
    // legacy .ppm thumbnails and pre-v2 JPEGs are ignored). Shared by
    // findFullRes / ensureFullRes / writeFullRes.
    std::filesystem::path fullResPathFor(
        const std::filesystem::path& rawPath) const;

    // Enforces the disk-cache caps: deletes the oldest files (by mtime) across
    // the whole <diskDir_> tree until both the byte and entry-count caps are
    // satisfied. Must be called with mutex_ held; the constructor calls it
    // before the object is shared with worker threads.
    void pruneLocked();

    // Disk cache caps. Eviction is triggered once either is exceeded.
    static constexpr std::uintmax_t kDiskMaxBytes = 1ull << 30;  // 1 GiB
    static constexpr std::size_t kDiskMaxEntries = 100000;       // ~100k files

    // findFullRes() is const but locks the mutex, so it stays mutable.
    mutable std::mutex mutex_;
};

} // namespace lrc
