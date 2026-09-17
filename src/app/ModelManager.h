#pragma once

/**
 * @brief Manages ONNX model assets: manifest registry, download, and cache.
 *
 * Responsibilities:
 *   • Parse resources/models/manifest.json into an in-memory registry.
 *   • Determine the OS-appropriate cache directory for downloaded .onnx files:
 *         Windows: %LOCALAPPDATA%/Silver Halide/models/
 *         Posix:   ~/.local/share/SilverHalide/models/  (fallback ~/.silverhalide)
 *   • Validate a model's SHA256 checksum against the manifest.
 *   • Trigger Qt-network downloads (QNetworkAccessManager) with progress
 *     reporting, including optional mirror fallbacks. Download is also
 *     available as a pure-C++ stub (the no-Qt fallback path so the catalog
 *     layer tests still work).
 *   • Manage version upgrades: bumping a manifest entry's version triggers an
 *     automatic redownload.
 *
 * ENGINE / LAYERING RULES:
 *   • This header intentionally avoids including Qt types directly — the
 *     manifest-query / SHA256 / path helpers are pure C++ and safe to call
 *     from engine/catalog threads. Only ModelManager::downloadModel() (the
 *     actual network call) is implemented in the .cpp guarded by Qt.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lrc {

/** @brief One model entry declared in the manifest (resources/models/manifest.json). */
struct ModelManifestEntry {
    std::string id;
    std::string name;
    std::string description;
    std::string downloadUrl;     // primary URL (HuggingFace / GitHub)
    std::string mirrorUrl;       // optional: hf-mirror.com etc.
    std::string filename;        // basename of the cached .onnx
    std::string sha256;          // hex-encoded (lowercase)
    uint64_t sizeBytes = 0;
    int version = 1;
    std::vector<int64_t> inputShape;   // e.g. {1,3,-1,-1}
    std::vector<int64_t> outputShape;
    std::string inputFormat;     // e.g. "RGB_F32_NCHW"
    std::vector<float> inputMean;
    std::vector<float> inputStd;
    std::string license;         // SPDX id: MIT, Apache-2.0, …
    bool internalOnly = false;   // true for the builtin test stub
};

/** Progress callback used by downloads. Bytes 0 = not started yet. */
using DownloadProgressFn = std::function<void(uint64_t bytesReceived,
                                              uint64_t bytesTotal,
                                              bool finished,
                                              const std::string& errorMsg)>;

class ModelManager {
public:
    explicit ModelManager(std::filesystem::path manifestPath = defaultManifestPath());
    ~ModelManager();

    // ---------- manifest queries -------------------------------------------
    /** Returns all models declared in the manifest. */
    const std::vector<ModelManifestEntry>& allModels() const;

    /** Lookup a specific model by id (nullopt when missing). */
    std::optional<ModelManifestEntry> findModel(const std::string& id) const;

    /** Parses / reparses the manifest JSON. Returns false on syntax error. */
    bool reloadManifest(const std::filesystem::path& path);

    // ---------- cache paths ------------------------------------------------
    /** Per-OS cache root (with "models/" appended — the leaf dir). */
    static std::filesystem::path cacheRoot();

    /** Absolute path where the given model should live on disk. */
    std::filesystem::path cachePathFor(const ModelManifestEntry& entry) const;

    /**
     * Bundled (install-dir) models root: <applicationDirPath>/models. The
     * installer ships models read-only at $INSTDIR\models; readPathFor probes
     * here first so installed copies don't re-download. Returns empty when the
     * Qt application object isn't available (headless tests / pre-QApp init).
     */
    static std::filesystem::path bundledRoot();

    /**
     * Path used to READ a model: prefers the bundled (install-dir) copy if
     * present, otherwise the per-user cache path. Downloads always write to
     * cachePathFor (the writable per-user location), so a bundled model is
     * never re-downloaded once isModelReady() resolves it here.
     */
    std::filesystem::path readPathFor(const ModelManifestEntry& entry) const;

    // ---------- validation -------------------------------------------------
    /** True iff the file exists and its SHA256 matches entry.sha256. */
    bool isModelReady(const ModelManifestEntry& entry) const;

    /** Returns lowercase hex SHA256 of any file (empty on I/O error). */
    static std::string sha256File(const std::filesystem::path& path);

    /**
     * @brief Extract the largest .onnx entry from a ZIP archive into
     *        `targetPath`. Used when a manifest entry's download_url points
     *        at a .zip rather than a bare .onnx (e.g. qualcomm/NAFNet-DeNoise,
     *        which ships a 410 MB ZIP of model.onnx + metadata.json).
     *
     * Disk-based extraction (no full-file load into memory). Returns true on
     * success and clears `errMsg`; returns false and sets `errMsg` on any
     * failure (targetPath is left untouched on failure). Implemented in the
     * .cpp via Qt's private QZipReader; the header signature stays Qt-free.
     */
    static bool extractOnnxFromZip(const std::filesystem::path& zipPath,
                                   const std::filesystem::path& targetPath,
                                   std::string& errMsg);

    // ---------- download ---------------------------------------------------
    /**
     * @brief Start a background download. Returns immediately; progress is
     *        reported asynchronously via `cb`.  If a Qt event loop is
     *        available (LRC_ENABLE_QT) the transfer uses QNetworkAccessManager
     *        and supports HTTP Range resumes; otherwise the call falls back
     *        to a synchronous stub that returns "download unavailable" via
     *        the callback — the engine never crashes without Qt.
     *
     * Returns an opaque handle that can be passed to cancelDownload().
     */
    uint64_t downloadModel(const ModelManifestEntry& entry,
                           DownloadProgressFn cb);

    /** Attempts to cancel an in-flight download. Safe to call on any id. */
    void cancelDownload(uint64_t handle);

    /** The standard baked-in manifest path (resources/models/manifest.json). */
    static std::filesystem::path defaultManifestPath();

    /** Last error message (empty if no error has occurred). */
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lrc
