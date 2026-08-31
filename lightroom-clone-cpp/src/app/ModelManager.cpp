// PHASE_11: ModelManager implementation.
//
// The pure-C++ bits (manifest parsing, cache paths, SHA-256 checksum) are
// always available.  Only downloadModel() optionally pulls in QtNetwork.

#include "app/ModelManager.h"
#include "utils/Json.h"
#include "utils/Sha256.h"   // lrc::sha256File (extracted for headless testing)

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if LRC_ENABLE_QT
#  include <QCoreApplication>
#  include <QFile>
#  include <QNetworkAccessManager>
#  include <QNetworkReply>
#  include <QNetworkRequest>
#  include <QObject>
#  include <QSaveFile>
#  include <QUrl>
// QZipReader is a Qt private API (header under QtCore/<ver>/QtCore/private/).
// Used only to extract a bare .onnx from a ZIP-packaged download_url — the
// only viable NAFNet-DeNoise ONNX source (qualcomm/NAFNet-DeNoise) ships as
// a 410 MB ZIP. Linking Qt6::CorePrivate (added in CMakeLists.txt) provides
// both the include path and the exported symbol.
#  include <QtCore/private/qzipreader_p.h>
#endif

namespace lrc {

// SHA-256 now lives in utils/Sha256.cpp (Qt-free, headless-tested by
// tests/TestSha256.cpp). Keep a thin static forwarder here so the public
// ModelManager API is unchanged for existing callers (smoke test, the
// download verify path below, etc.).
std::string ModelManager::sha256File(const std::filesystem::path& path) {
    return lrc::sha256File(path);
}

// ---------------------------------------------------------------------------
// cacheRoot / cachePathFor / defaultManifestPath (pure C++, OS-aware)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  include <cstdlib>   // getenv_s
#endif

std::filesystem::path ModelManager::cacheRoot() {
    std::filesystem::path base;
#if defined(_WIN32)
    char buf[32767] = {};
    size_t n = 0;
    if (getenv_s(&n, buf, "LOCALAPPDATA") == 0 && n > 1) {
        base = buf;
    } else {
        base = std::filesystem::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".") / "AppData" / "Local";
    }
    base /= "Silver Halide";
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = ".";
    // $XDG_DATA_HOME overrides ~/.local/share
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) base = xdg;
    else             base = std::filesystem::path(home) / ".local" / "share";
    base /= "SilverHalide";
#endif
    base /= "models";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

std::filesystem::path ModelManager::cachePathFor(const ModelManifestEntry& e) const {
    auto root = cacheRoot();
    // Version-qualified filename: nafnet-denoise_v2.onnx so bumping version
    // automatically invalidates any stale old download.
    if (e.version > 1) {
        const auto ext  = std::filesystem::path(e.filename).extension();
        const auto stem = std::filesystem::path(e.filename).stem();
        std::ostringstream oss;
        oss << stem.string() << "_v" << e.version << ext.string();
        return root / oss.str();
    }
    return root / e.filename;
}

std::filesystem::path ModelManager::defaultManifestPath() {
    // Prefer the absolute path baked in at compile time (CMake passes the
    // project source dir via LRC_PROJECT_MANIFEST_DIR) so the manifest is
    // resolvable regardless of the runtime working directory.
#if defined(LRC_PROJECT_MANIFEST_DIR)
    {
        auto p = std::filesystem::path(LRC_PROJECT_MANIFEST_DIR)
                 / "resources" / "models" / "manifest.json";
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
#endif
    // Fallback: relative to CWD (legacy dev-build behavior).
    return std::filesystem::path("resources") / "models" / "manifest.json";
}

// ---------------------------------------------------------------------------
// Manifest parsing
// ---------------------------------------------------------------------------
static std::optional<ModelManifestEntry> parseEntry(const lrc::json::Value& v) {
    if (!v.isObject()) return std::nullopt;
    ModelManifestEntry e;
    e.id          = v.contains("id") ? v["id"].asString() : std::string();
    e.name        = v.contains("name") ? v["name"].asString() : e.id;
    e.description = v.contains("description") ? v["description"].asString() : std::string();
    e.downloadUrl = v.contains("download_url") ? v["download_url"].asString() :
                    (v.contains("url") ? v["url"].asString() : std::string());
    e.mirrorUrl   = v.contains("mirror_url") ? v["mirror_url"].asString() : std::string();
    e.filename    = v.contains("filename") ? v["filename"].asString() : (e.id + ".onnx");
    e.sha256      = v.contains("sha256") ? v["sha256"].asString() : std::string();
    e.sizeBytes   = v.contains("size_bytes") ? static_cast<uint64_t>(v["size_bytes"].asNumber()) :
                    (v.contains("size") ? static_cast<uint64_t>(v["size"].asNumber()) : 0);
    e.version     = v.contains("version") ? v["version"].asInt() : 1;
    e.license     = v.contains("license") ? v["license"].asString() : "UNKNOWN";
    e.internalOnly = v.contains("internal_only") && v["internal_only"].asBool();
    if (v.contains("input_shape") && v["input_shape"].isArray()) {
        for (const auto& d : v["input_shape"].items())
            e.inputShape.push_back(static_cast<int64_t>(d.asNumber()));
    }
    if (v.contains("output_shape") && v["output_shape"].isArray()) {
        for (const auto& d : v["output_shape"].items())
            e.outputShape.push_back(static_cast<int64_t>(d.asNumber()));
    }
    if (v.contains("input_format")) e.inputFormat = v["input_format"].asString();
    if (v.contains("input_mean") && v["input_mean"].isArray()) {
        for (const auto& m : v["input_mean"].items())
            e.inputMean.push_back(static_cast<float>(m.asNumber()));
    }
    if (v.contains("input_std") && v["input_std"].isArray()) {
        for (const auto& m : v["input_std"].items())
            e.inputStd.push_back(static_cast<float>(m.asNumber()));
    }
    if (e.id.empty()) return std::nullopt;
    return e;
}

// ---------------------------------------------------------------------------
// Impl + download plumbing
// ---------------------------------------------------------------------------
#if LRC_ENABLE_QT
// Extract an ONNX model from a ZIP archive into `targetPath`'s directory.
// Used when a manifest entry's download_url points to a .zip archive rather
// than a bare .onnx file (currently: qualcomm/NAFNet-DeNoise, which ships as
// a 410 MB ZIP containing nafnet_denoise.onnx + nafnet_denoise.data +
// metadata.json).
//
// The ONNX "external data" pattern: large models (>2 GB, or just by author
// choice) store the graph topology in a small .onnx file and the raw weight
// blobs in a sibling .data file. The .onnx graph references its .data file by
// relative path (just the leaf filename). ONNX Runtime resolves that path
// relative to the .onnx's directory at load time. So we MUST keep the .data
// file next to the .onnx, with its ORIGINAL leaf name (the name the .onnx
// graph references internally).
//
// Strategy: extractAll() into a temp dir, then move every regular file (with
// its original leaf name) into targetPath.parent_path(), flattening any
// subdirectory the ZIP may have wrapped them in. Finally rename the extracted
// .onnx to targetPath's filename so the cache path matches the manifest, and
// clean up the temp dir. The .data file stays alongside with its original
// name (matching what the .onnx references).
//
// Disk-based extraction avoids loading the ~442 MB .data into memory via
// QZipReader::fileData(). Returns true on success and clears `errMsg`;
// returns false and sets `errMsg` on any failure (targetPath is left
// untouched on failure).
bool ModelManager::extractOnnxFromZip(const std::filesystem::path& zipPath,
                                     const std::filesystem::path& targetPath,
                                     std::string& errMsg) {
    errMsg.clear();
    QZipReader reader(QString::fromStdString(zipPath.string()));
    if (!reader.isReadable() || reader.status() != QZipReader::NoError) {
        errMsg = "ZIP unreadable or status != NoError: " + zipPath.string();
        return false;
    }

    // Pre-check: is there at least one .onnx entry? Lets us fail fast
    // before creating a temp dir and extracting gigabytes.
    bool hasOnnx = false;
    for (const auto& fi : reader.fileInfoList()) {
        if (!fi.isFile) continue;
        if (fi.filePath.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive)) {
            hasOnnx = true;
            break;
        }
    }
    if (!hasOnnx) {
        errMsg = "ZIP contains no .onnx entry: " + zipPath.string();
        return false;
    }

    const auto outDir = targetPath.parent_path();
    // Temp extraction dir next to the final target. Using a leaf suffix that
    // is unique per-target avoids colliding with concurrent downloads.
    const auto tmpDir = outDir / (targetPath.filename().string() + ".extract");
    std::error_code ec;
    std::filesystem::remove_all(tmpDir, ec);  // stale from a previous run
    std::filesystem::create_directories(tmpDir, ec);
    if (ec) {
        errMsg = "cannot create temp extract dir: " + ec.message();
        return false;
    }
    // Also make sure the output directory exists.
    std::filesystem::create_directories(outDir, ec);

    if (!reader.extractAll(QString::fromStdString(tmpDir.string()))) {
        errMsg = "QZipReader::extractAll failed";
        reader.close();
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }
    reader.close();

    // Walk the extracted tree: move every regular file into outDir using
    // only its leaf name (flattening any ZIP wrapper directory). Track which
    // one is the .onnx so we can rename it to targetPath afterwards.
    std::filesystem::path bestOnnx;
    uintmax_t bestOnnxSize = 0;
    std::error_code iterEc;
    for (auto it = std::filesystem::recursive_directory_iterator(tmpDir, iterEc);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (iterEc) break;
        const auto& src = it->path();
        if (!std::filesystem::is_regular_file(src, ec) || ec) continue;

        const auto leaf = src.filename();
        const auto dst = outDir / leaf;

        // If a file with the same leaf name already exists at the destination
        // (e.g. a stale .data from a prior extraction), remove it first so
        // the rename doesn't fail spuriously.
        if (std::filesystem::exists(dst, ec)) {
            std::filesystem::remove(dst, ec);
        }
        std::filesystem::rename(src, dst, ec);
        if (ec) {
            // Cross-volume or busy: fall back to copy+delete.
            std::filesystem::copy_file(src, dst,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(src, ec);
        }

        const auto ext = leaf.extension().string();
        if (ext == ".onnx" || ext == ".ONNX") {
            const auto sz = std::filesystem::file_size(dst, ec);
            if (!ec && sz > bestOnnxSize) {
                bestOnnxSize = sz;
                bestOnnx = dst;
            }
        }
    }
    // Drop the temp dir (now empty or containing only empty subdirs).
    std::filesystem::remove_all(tmpDir, ec);

    if (bestOnnx.empty()) {
        errMsg = "extraction produced no .onnx file";
        return false;
    }

    // Rename the extracted .onnx to targetPath so the cache path matches
    // the manifest. The sibling .data file is left with its original leaf
    // name — which is the name the .onnx graph references internally, so
    // ONNX Runtime will still find it (the .data path is resolved relative
    // to the model file's directory, not the model file's name).
    if (bestOnnx != targetPath) {
        std::filesystem::remove(targetPath, ec);
        std::filesystem::rename(bestOnnx, targetPath, ec);
        if (ec) {
            std::filesystem::copy_file(bestOnnx, targetPath,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(bestOnnx, ec);
        }
    }

    if (!std::filesystem::exists(targetPath, ec)) {
        errMsg = "extraction produced no target file at " + targetPath.string();
        return false;
    }
    return true;
}
#endif

struct ModelManager::Impl {
    std::vector<ModelManifestEntry> entries;
    std::filesystem::path manifestPath;
    std::string lastError;
    std::mutex mu;
    uint64_t nextHandle = 1;
    std::unordered_map<uint64_t, DownloadProgressFn> callbacks;

#if LRC_ENABLE_QT
    struct QtDownload {
        std::unique_ptr<QNetworkAccessManager> nam;
        QNetworkReply* reply = nullptr;
        std::filesystem::path targetPath;
        std::filesystem::path partPath;
        ModelManifestEntry entry;
        bool usingMirror = false;
        // Recursive re-fetch entry point (mirror fallback). Stored here rather
        // than as a downloadModel() local so it outlives the function return:
        // QNetworkReply::finished fires asynchronously. The stored callable
        // captures `this`+`h` (NOT dl) and re-resolves dl from qtDownloads[h],
        // avoiding a shared_ptr cycle (dl owns beginFetch; beginFetch must not
        // own dl).
        std::function<void(const std::string&, bool)> beginFetch;
    };
    std::unordered_map<uint64_t, std::shared_ptr<QtDownload>> qtDownloads;
#endif
};

ModelManager::ModelManager(std::filesystem::path manifestPath)
    : impl_(std::make_unique<Impl>()) {
    if (!manifestPath.empty() && std::filesystem::exists(manifestPath)) {
        reloadManifest(manifestPath);
        impl_->manifestPath = std::move(manifestPath);
    } else {
        // Try the default relative path plus a couple of build-tree relatives
        const auto tryPaths = {
            manifestPath,
            std::filesystem::path(LRC_PROJECT_MANIFEST_DIR) / "resources" / "models" / "manifest.json",
            std::filesystem::current_path() / "resources" / "models" / "manifest.json",
            std::filesystem::current_path() / "lightroom-clone-cpp" / "resources" / "models" / "manifest.json",
            std::filesystem::current_path().parent_path() / "resources" / "models" / "manifest.json",
        };
        for (const auto& p : tryPaths) {
            if (!p.empty() && std::filesystem::exists(p)) {
                reloadManifest(p);
                impl_->manifestPath = p;
                break;
            }
        }
    }
}

ModelManager::~ModelManager() = default;

bool ModelManager::reloadManifest(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::ifstream f(path);
    if (!f) {
        impl_->lastError = "manifest not found: " + path.string();
        return false;
    }
    std::ostringstream ss; ss << f.rdbuf();
    auto root = json::Value::parse(ss.str());
    if (!root || !root->isObject()) {
        impl_->lastError = "manifest parse failed (not JSON object)";
        return false;
    }
    impl_->entries.clear();
    if (root->contains("models") && (*root)["models"].isArray()) {
        for (const auto& item : (*root)["models"].items()) {
            auto e = parseEntry(item);
            if (e) impl_->entries.push_back(std::move(*e));
        }
    }
    impl_->lastError.clear();
    impl_->manifestPath = path;
    return true;
}

const std::vector<ModelManifestEntry>& ModelManager::allModels() const {
    return impl_->entries;
}

std::optional<ModelManifestEntry> ModelManager::findModel(const std::string& id) const {
    for (const auto& e : impl_->entries)
        if (e.id == id) return e;
    return std::nullopt;
}

bool ModelManager::isModelReady(const ModelManifestEntry& entry) const {
    const auto p = cachePathFor(entry);
    if (!std::filesystem::exists(p)) return false;
    // Placeholder / TBD SHA: accept the file if size matches (best-effort).
    // This keeps the model usable on a development machine before the first
    // formal upload produces a stable SHA.
    const auto& sha = entry.sha256;
    const bool shaValid = !sha.empty() &&
                          sha.find("TBD") == std::string::npos &&
                          sha.find("PLACEHOLDER") == std::string::npos;
    if (!shaValid) {
        // Accept if size matches (or size is 0, accept any)
        if (entry.sizeBytes == 0) return true;
        std::error_code ec;
        const auto sz = std::filesystem::file_size(p, ec);
        return !ec && sz == entry.sizeBytes;
    }
    const auto actual = sha256File(p);
    return actual == sha;
}

std::string ModelManager::lastError() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->lastError;
}

uint64_t ModelManager::downloadModel(const ModelManifestEntry& entry,
                                     DownloadProgressFn cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    const uint64_t h = impl_->nextHandle++;
    impl_->callbacks[h] = std::move(cb);

    // --- fast-path success: already ready ------------------------------------
    if (isModelReady(entry)) {
        if (auto& fn = impl_->callbacks[h])
            fn(entry.sizeBytes, entry.sizeBytes, true, "");
        return h;
    }

#if LRC_ENABLE_QT
    auto dl = std::make_shared<Impl::QtDownload>();
    dl->nam = std::make_unique<QNetworkAccessManager>();
    dl->entry = entry;
    dl->targetPath = cachePathFor(entry);
    dl->partPath   = dl->targetPath;
    dl->partPath += ".part";
    // Ensure the containing folder exists
    std::error_code ec;
    std::filesystem::create_directories(dl->targetPath.parent_path(), ec);
    impl_->qtDownloads[h] = dl;

    dl->beginFetch = [this, h](const std::string& url, bool mirror) {
        // Re-resolve dl from the registry each call so this stored callable
        // does not capture the shared_ptr (avoids a dl<->beginFetch cycle).
        auto it = impl_->qtDownloads.find(h);
        if (it == impl_->qtDownloads.end() || !it->second) return;
        auto& dl = it->second;
        dl->usingMirror = mirror;
        QNetworkRequest req(QUrl(QString::fromStdString(url)));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        // Resume from partial download if the .part file exists
        uint64_t already = 0;
        if (std::filesystem::exists(dl->partPath)) {
            std::error_code lec;
            already = std::filesystem::file_size(dl->partPath, lec);
            if (already > 0) {
                QByteArray range = "bytes=" + QByteArray::number(already) + "-";
                req.setRawHeader("Range", range);
            }
        }
        dl->reply = dl->nam->get(req);

        QObject::connect(dl->reply, &QNetworkReply::downloadProgress,
            dl->reply, [this, h, already](qint64 recv, qint64 total) {
                auto it = impl_->callbacks.find(h);
                if (it == impl_->callbacks.end() || !it->second) return;
                const uint64_t r = already + static_cast<uint64_t>(recv);
                const uint64_t t = total > 0 ? static_cast<uint64_t>(total) + already : 0;
                it->second(r, t, false, "");
            });
        QObject::connect(dl->reply, &QNetworkReply::readyRead,
            dl->reply, [this, h, dl]() {
                // Append the ready-read bytes to the .part file.
                const auto bytes = dl->reply->readAll();
                if (bytes.isEmpty()) return;
                QFile f(QString::fromStdString(dl->partPath.string()));
                if (!f.open(QIODevice::Append)) return;
                f.write(bytes);
                f.close();
            });
        QObject::connect(dl->reply, &QNetworkReply::finished,
            dl->reply, [this, h, dl]() mutable {
                const auto err = dl->reply->error();
                const auto errStr = dl->reply->errorString().toStdString();
                const int status = dl->reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                // Flush any remaining bytes to the .part
                const auto tail = dl->reply->readAll();
                if (!tail.isEmpty()) {
                    QFile f(QString::fromStdString(dl->partPath.string()));
                    if (f.open(QIODevice::Append)) { f.write(tail); f.close(); }
                }
                dl->reply->deleteLater();
                dl->reply = nullptr;

                auto itCb = impl_->callbacks.find(h);
                const bool hasCb = itCb != impl_->callbacks.end() && bool(itCb->second);

                // Mirror fallback on 4xx / network errors (unless we are already on mirror)
                const bool shouldMirror = !dl->usingMirror && !dl->entry.mirrorUrl.empty() &&
                    (err != QNetworkReply::NoError ||
                     (status >= 400 && status < 600));
                if (shouldMirror) {
                    // Remove broken partial and re-start from the mirror URL.
                    std::error_code lec;
                    std::filesystem::remove(dl->partPath, lec);
                    dl->beginFetch(dl->entry.mirrorUrl, true);
                    return;
                }

                if (err != QNetworkReply::NoError) {
                    if (hasCb) itCb->second(0, 0, true, errStr);
                    impl_->qtDownloads.erase(h);
                    impl_->callbacks.erase(h);
                    return;
                }
                // SHA256 verify -> move to final filename or delete + retry
                std::error_code lec;
                if (!std::filesystem::exists(dl->partPath, lec)) {
                    if (hasCb) itCb->second(0, 0, true, "download produced no file");
                    impl_->qtDownloads.erase(h); impl_->callbacks.erase(h); return;
                }

                // ZIP-packaged download_url (e.g. qualcomm/NAFNet-DeNoise
                // ships as a 410 MB ZIP containing model.onnx + metadata.json).
                // Detect .zip suffix on the URL that was actually fetched
                // (primary or mirror). For ZIP URLs the .part file holds the
                // archive bytes; we extract the largest .onnx to targetPath,
                // delete the archive, then SHA256-verify the extracted file.
                const std::string& usedUrl = dl->usingMirror
                    ? dl->entry.mirrorUrl : dl->entry.downloadUrl;
                const bool isZip = usedUrl.size() >= 4 &&
                    (usedUrl.compare(usedUrl.size() - 4, 4, ".zip") == 0 ||
                     usedUrl.compare(usedUrl.size() - 4, 4, ".ZIP") == 0 ||
                     usedUrl.compare(usedUrl.size() - 4, 4, ".Zip") == 0);
                if (isZip) {
                    std::string zipErr;
                    if (!extractOnnxFromZip(dl->partPath, dl->targetPath, zipErr)) {
                        std::filesystem::remove(dl->partPath, lec);
                        if (hasCb) itCb->second(0, 0, true,
                            "ZIP extraction failed: " + zipErr);
                        impl_->qtDownloads.erase(h); impl_->callbacks.erase(h); return;
                    }
                    std::filesystem::remove(dl->partPath, lec);  // archive no longer needed
                    const bool shaOk = dl->entry.sha256.empty() ||
                        dl->entry.sha256.find("TBD") == 0 ||
                        dl->entry.sha256.find("PLACEHOLDER") == 0 ||
                        sha256File(dl->targetPath) == dl->entry.sha256;
                    if (!shaOk) {
                        std::filesystem::remove(dl->targetPath, lec);
                        if (hasCb) itCb->second(0, 0, true,
                            "SHA256 mismatch after ZIP extraction — model corrupted, retry");
                        impl_->qtDownloads.erase(h); impl_->callbacks.erase(h); return;
                    }
                    const uint64_t sz = std::filesystem::exists(dl->targetPath, lec)
                        ? static_cast<uint64_t>(std::filesystem::file_size(dl->targetPath, lec))
                        : dl->entry.sizeBytes;
                    if (hasCb) itCb->second(sz, sz, true, "");
                    impl_->qtDownloads.erase(h); impl_->callbacks.erase(h); return;
                }

                // Non-ZIP path: bare .onnx download. SHA256 verify the part
                // file then atomically rename to the final target.
                const bool shaValid = dl->entry.sha256.empty() ||
                    dl->entry.sha256.find("TBD") == 0 ||
                    dl->entry.sha256.find("PLACEHOLDER") == 0 ||
                    sha256File(dl->partPath) == dl->entry.sha256;
                if (!shaValid) {
                    std::filesystem::remove(dl->partPath, lec);
                    if (hasCb) itCb->second(0, 0, true, "SHA256 mismatch — model corrupted, retry");
                    impl_->qtDownloads.erase(h); impl_->callbacks.erase(h); return;
                }
                // Atomically rename (overwrite existing stale).
                std::filesystem::remove(dl->targetPath, lec);
                std::filesystem::rename(dl->partPath, dl->targetPath, lec);
                const uint64_t sz = std::filesystem::exists(dl->targetPath, lec)
                    ? static_cast<uint64_t>(std::filesystem::file_size(dl->targetPath, lec))
                    : dl->entry.sizeBytes;
                if (hasCb) itCb->second(sz, sz, true, "");
                impl_->qtDownloads.erase(h); impl_->callbacks.erase(h);
            });
    };
    dl->beginFetch(entry.downloadUrl, false);
#else
    // No Qt: report failure via the callback immediately so callers can
    // handle the UI gray-out (button disabled, tooltip "model missing").
    if (auto& fn = impl_->callbacks[h])
        fn(0, 0, true,
           "Download unavailable — ModelManager built without QtNetwork. "
           "Place the model file manually in " + cachePathFor(entry).string());
#endif
    return h;
}

void ModelManager::cancelDownload(uint64_t handle) {
    std::lock_guard<std::mutex> lk(impl_->mu);
#if LRC_ENABLE_QT
    auto it = impl_->qtDownloads.find(handle);
    if (it != impl_->qtDownloads.end() && it->second->reply) {
        it->second->reply->abort();
        QObject::disconnect(it->second->reply, nullptr, nullptr, nullptr);
        it->second->reply->deleteLater();
        it->second->reply = nullptr;
    }
#endif
    impl_->callbacks.erase(handle);
}

} // namespace lrc
