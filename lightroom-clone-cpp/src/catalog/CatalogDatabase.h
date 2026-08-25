#pragma once

// SQLite-backed catalog (a `.lrcat`-style single file). Pure C++ — no Qt.
// Thread-safe: every public method takes a lock, so background import workers
// can write while the UI reads. WAL journal mode keeps readers and writers
// from blocking each other.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/Adjustments.h"
#include "engine/ImageBuffer.h"
#include "utils/Json.h"

struct sqlite3;
struct sqlite3_stmt;

namespace lrc {

// Flag values (Lightroom picks).
enum ImageFlag : int {
    FlagRejected = -1,
    FlagNone = 0,
    FlagPicked = 1,
};

struct ImageRecord {
    int64_t id = 0;
    std::string filePath;
    std::string thumbnailPath;
    std::string fullresPath; // full-res (1:1) display JPEG cache path
    int rating = 0;      // 0..5
    int labelColor = 0;  // 0 = none, 1..7 colour labels
    int flag = ImageFlag::FlagNone; // -1 rejected, 0 none, 1 picked
    std::string importDate; // ISO-8601 UTC
    bool isManaged = false; // true when the file lives in the managed library

    // --- IPTC / metadata (M8) -------------------------------------------
    std::string title;     // IPTC title
    std::string caption;   // IPTC description / caption
    std::string copyright; // IPTC rights
    std::string creator;   // IPTC creator
    std::string dateTaken; // ISO-8601 capture date (empty until populated)
    std::string camera;    // camera model string (empty until populated)
    std::string lens;      // lens model string (empty until populated)
};

// A named collection (album / favorites) with a user-facing display order.
struct Collection {
    int64_t id = 0;
    std::string name;
    int sortOrder = 0;  // display order
};

// One committed edit-state snapshot (undo history). The adjustment values are
// loaded on demand via loadEditSnapshot().
struct EditSnapshot {
    int64_t id = 0;
    int64_t imageId = 0;
    std::string createdAt; // ISO-8601 UTC
};

// A virtual copy / edit variant of a photo. variantId 0 is the master copy
// that shares the physical file; higher ids are independent edit stacks that
// share the same thumbnail and previews (PHASE_6).
struct EditVariant {
    int64_t variantId = 0;
    std::string name;
    bool isMaster = false;
};

// A hierarchical keyword with its slash-joined full path and usage count.
struct KeywordInfo {
    int64_t id = 0;
    std::string name;
    std::string fullPath; // e.g. "风光/冰岛/瀑布"
    int64_t photoCount = 0;
};

// Basic IPTC metadata stored on the images row.
struct PhotoMetadata {
    std::string title;
    std::string caption;
    std::string copyright;
    std::string creator;
    std::string dateTaken;
};

// A rule-based (dynamic) collection. `predicateJson` is the rule document
// {"match":"all"|"any","rules":[...]}; `matchType` mirrors its ALL/ANY mode
// as stored in the smart_collections column.
struct SmartCollection {
    int64_t id = 0;
    std::string name;
    std::string predicateJson;
    std::string matchType; // "ALL" or "ANY"
};

// Evaluates a parsed smart-collection predicate against one image.
// `predicate` follows {"match":"all"|"any","rules":[{field,op,value}]}.
// `keywords` holds the image's keyword full paths; `hasAdjustments` reports
// whether the image carries non-default develop edits. Pure function, kept
// free so it can be unit-tested without a database (PHASE_9).
bool evaluatePredicate(const json::Value& predicate, const ImageRecord& img,
                       const std::vector<std::string>& keywords,
                       bool hasAdjustments);

// Filters for listImages(). Default (all zeros/empty) returns every image.
struct ImageFilter {
    int minRating = 0;                 // only images with rating >= this
    int flag = 0;                      // 0 = all flags; else match the exact flag
    int labelColor = 0;                // 0 = all labels; else exact colour
    std::string fileExtension;         // lowercase incl. dot, e.g. ".dng"; empty = all
    bool includeRejected = false;      // if false, skip flag == -1
};

// Kinds of preview embedded in the catalog file (thumbnail / full-res /
// standard). Standard previews reuse the same table: they are 2048px linear
// ProPhoto RGB F32 renders at default settings (see StandardPreview.h).
enum class PreviewKind : int { Thumbnail = 1, FullRes = 2, Standard = 3 };

class CatalogDatabase {
public:
    explicit CatalogDatabase(std::filesystem::path dbPath);
    ~CatalogDatabase();

    CatalogDatabase(const CatalogDatabase&) = delete;
    CatalogDatabase& operator=(const CatalogDatabase&) = delete;

    bool open();  // creates the schema when the file is new
    void close();
    bool isOpen() const { return db_ != nullptr; }
    std::string lastError() const { return lastError_; }
    // Number of prepared statements currently held in the reuse cache.
    // Observability hook used by the tests to confirm statements are cached
    // and reused rather than re-prepared on every call.
    size_t stmtCacheSize() const { return stmtCache_.size(); }

    // --- images ----------------------------------------------------------
    // Inserts a new image. If the path already exists, the existing row id is
    // returned instead (imports are idempotent).
    int64_t addImage(const std::string& filePath,
                     const std::string& thumbnailPath,
                     bool isManaged = false);
    bool setRating(int64_t id, int rating);
    bool setLabelColor(int64_t id, int color);
    bool setFlag(int64_t id, int flag);
    bool setThumbnailPath(int64_t id, const std::string& thumbnailPath);
    bool setFullResPath(int64_t id, const std::string& fullresPath);
    bool setManaged(int64_t id, bool isManaged);
    bool removeImage(int64_t id);
    std::vector<ImageRecord> listImages() const;
    // Filtered listing (empty/default filter returns everything).
    std::vector<ImageRecord> listImages(const ImageFilter& filter) const;
    std::optional<ImageRecord> findImageByPath(const std::string& filePath) const;
    // Stores an embedded preview (thumbnail or full-res JPEG) inside the
    // catalog file, replacing any previous value. (embedded preview blob)
    bool storePreview(int64_t imageId, PreviewKind kind,
                      const std::vector<uint8_t>& data,
                      int width = 0, int height = 0);
    // Returns the embedded preview bytes, or nullopt when absent.
    std::optional<std::vector<uint8_t>> loadPreview(
        int64_t imageId, PreviewKind kind) const;
    // Embedded preview pixel dimensions (0,0 when not recorded), or nullopt
    // when the preview is absent.
    std::optional<std::pair<int, int>> previewSize(
        int64_t imageId, PreviewKind kind) const;

    // --- standard preview --------------------------------------------------
    // Stores a standard preview (linear ProPhoto RGB F32, 2048px longest
    // edge) inside the catalog previews table, replacing any previous value.
    // Returns false when the buffer is empty / not F32 or the write fails.
    bool storeStandardPreview(int64_t imageId, const ImageBuffer& buf);
    // Returns the stored standard preview as a linear ProPhoto RGB F32 buffer,
    // or nullopt when absent (old catalogs simply have no Standard record) or
    // when the stored blob is malformed. Never crashes on a corrupt record.
    std::optional<std::shared_ptr<ImageBuffer>> getStandardPreview(
        int64_t imageId) const;
    // True when a Standard preview record exists for the image (regardless of
    // whether its blob is decodable -- use getStandardPreview to check that).
    bool hasStandardPreview(int64_t imageId) const;

    // --- catalog management ----------------------------------------------
    // Writes a consistent backup of the catalog file (WAL checkpointed).
    bool backup(const std::filesystem::path& destPath) const;
    // Runs VACUUM to reclaim space and rebuild indexes.
    bool optimize();

    // --- settings (catalog-level key/value state) -------------------------
    bool setSetting(const std::string& key, const std::string& value);
    std::optional<std::string> getSetting(const std::string& key) const;

    // --- edits (non-destructive parameter stack) -------------------------
    // Saves the current parameter stack and records an edit-history snapshot.
    // `variantId` selects which virtual copy's stack to write (0 = master).
    bool saveAdjustments(int64_t imageId, const Adjustments& adjustments,
                         int variantId = 0);
    std::optional<Adjustments> loadAdjustments(int64_t imageId,
                                               int variantId = 0) const;

    // --- edit history (undo snapshots, newest last) -----------------------
    int64_t saveEditSnapshot(int64_t imageId, const Adjustments& adjustments,
                             int variantId = 0);
    std::vector<EditSnapshot> listEditHistory(int64_t imageId,
                                              int variantId = 0) const;
    std::optional<Adjustments> loadEditSnapshot(int64_t imageId,
                                                int64_t snapshotId,
                                                int variantId = 0) const;
    bool deleteEditSnapshot(int64_t imageId, int64_t snapshotId,
                            int variantId = 0);

    // --- virtual copies (M6) ---------------------------------------------
    // Creates a virtual copy of `imageId`, copying the master (variant 0)
    // edits and edit history into a fresh variant id. Returns the new variant
    // id, or -1 on failure.
    int createVirtualCopy(int64_t imageId);
    // Lists the virtual copies (including the master) registered for an image.
    std::vector<EditVariant> listVariants(int64_t imageId) const;
    bool renameVariant(int64_t imageId, int variantId,
                       const std::string& name);
    // Deletes a virtual copy (its edits/history/variant row). Deleting the
    // master is refused while virtual copies exist (PHASE_6 policy).
    bool deleteVariant(int64_t imageId, int variantId);

    // --- folders ---------------------------------------------------------
    int64_t addFolder(const std::string& folderPath, int64_t parentId = 0);
    std::vector<std::string> listFolders() const;

    // --- collections (albums / favorites) ---------------------------------
    // Creates a named collection. Returns the new id, or -1 on failure.
    int64_t createCollection(const std::string& name);
    bool renameCollection(int64_t collectionId, const std::string& newName);
    bool deleteCollection(int64_t collectionId);      // cascades to members
    std::vector<Collection> listCollections() const;  // order by sort_order, then id
    bool addToCollection(int64_t collectionId, int64_t imageId);  // idempotent
    bool removeFromCollection(int64_t collectionId, int64_t imageId);
    bool isImageInCollection(int64_t collectionId, int64_t imageId) const;
    std::vector<ImageRecord> listImagesInCollection(int64_t collectionId) const;
    // Single-query replacement for the N+1 pattern (calling
    // listImagesInCollection() once per collection, as refreshFilmstrip did).
    // Returns imageId -> the collection ids that contain it; images that
    // belong to no collection are absent from the map.
    std::unordered_map<int64_t, std::vector<int64_t>>
    listCollectionMemberships() const;

    // --- keywords & metadata (M8) -----------------------------------------
    // Ensures a keyword hierarchy exists for `path` ("风光/冰岛/瀑布"),
    // creating missing segments top-down. Returns the leaf keyword id.
    int64_t ensureKeyword(const std::string& path);
    // Assigns / removes a keyword on an image (both idempotent).
    bool assignKeyword(int64_t imageId, int64_t keywordId);
    bool removeKeyword(int64_t imageId, int64_t keywordId);
    std::vector<int64_t> keywordsForImage(int64_t imageId) const;
    std::vector<KeywordInfo> listKeywords() const;
    // Images tagged with `keywordId` or any descendant keyword.
    std::vector<int64_t> findImagesByKeyword(int64_t keywordId) const;
    bool setPhotoMetadata(int64_t imageId, const std::string& title,
                          const std::string& caption,
                          const std::string& copyright,
                          const std::string& creator,
                          const std::string& dateTaken);
    PhotoMetadata getPhotoMetadata(int64_t imageId) const;
    // LIKE search over title / caption / keyword names for the search box.
    std::vector<ImageRecord> searchImages(const std::string& text) const;

    // --- smart collections (M9) -------------------------------------------
    // Creates a rule-based collection. `matchType` is "ALL" or "ANY"; the
    // matching mode is also injected into `predicateJson` so the evaluator is
    // self-contained. The member set is refreshed immediately. Returns the new
    // id, or -1 on failure.
    int64_t createSmartCollection(const std::string& name,
                                  const std::string& predicateJson,
                                  const std::string& matchType);
    bool updateSmartCollection(int64_t id, const std::string& name,
                               const std::string& predicateJson,
                               const std::string& matchType);
    bool deleteSmartCollection(int64_t id); // cascades to members
    std::vector<SmartCollection> listSmartCollections() const;
    // Recomputes the members of a smart collection by evaluating every image
    // against its predicate. Returns the number of matching images, or -1 on
    // failure.
    int refreshSmartCollection(int64_t id);
    std::vector<int64_t> listImagesInSmartCollection(int64_t id) const;

    // --- panorama sources (M10) -------------------------------------------
    // Records (replacing) the source images that produced a panorama. Calling
    // twice with the same set is a no-op.
    bool recordPanoramaSources(int64_t panoramaImageId,
                               const std::vector<int64_t>& sourceImageIds);
    std::vector<int64_t> listPanoramaSources(int64_t panoramaImageId) const;

    // --- transactions (primary connection) --------------------------------
    // Wrap a batch of writes on the primary connection. The import worker must
    // NOT use these -- it uses the isolated import connection below so its
    // transaction never absorbs UI-thread writes.
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // --- import connection (S3: isolated batch transactions) ---------------
    // The import worker writes through its own second connection to the same
    // catalog file so a batch-import transaction never absorbs UI-thread writes
    // (ratings / adjustments) issued on the primary connection. WAL mode lets
    // the two connections operate concurrently; a cancelled import's rollback
    // can therefore never undo an edit the user saved while the import ran.
    //
    // Caller must pair every openImportConnection() with closeImportConnection()
    // (RAII-style; ImportWorker does this) so the handle is released and no
    // file handle is left open. Operations on the import connection are guarded
    // by their own mutex -- sharing the primary mutex with SQLite's cross-
    // connection write lock would deadlock (UI holds the primary mutex while
    // waiting on SQLite, and the import thread needs that mutex to release the
    // lock).
    bool openImportConnection();
    void closeImportConnection();
    bool isImportConnectionOpen() const;
    bool beginImportTransaction();
    bool commitImportTransaction();
    bool rollbackImportTransaction();
    // addImage / storePreview against the import connection (valid only while
    // it is open). Distinct from the primary-connection addImage/storePreview.
    int64_t addImageImport(const std::string& filePath,
                           const std::string& thumbnailPath,
                           bool isManaged = false);
    bool storePreviewImport(int64_t imageId, PreviewKind kind,
                            const std::vector<uint8_t>& data,
                            int width = 0, int height = 0);
    // Error message from the last import-connection operation. Separate from
    // lastError() because the two connections are driven from different threads
    // and a UI-thread write must not clobber the import thread's error text.
    // Reads under importMutex_ so teardown (close on the UI thread) cannot race
    // the import thread's read.
    std::string importLastError() const;

private:
    bool exec(const char* sql);
    bool execOn(sqlite3* db, const char* sql, std::string* errOut);
    void ensureColumn(const char* column, const char* decl);
    // ALTER TABLE ADD COLUMN on an arbitrary table (used for edit_history).
    void ensureColumnOn(const char* table, const char* column,
                        const char* decl);
    // Rebuilds the edits table with the composite (image_id, variant_id)
    // primary key when it was created by an older build.
    void ensureEditsVariant();
    // True when the image has a variant-0 edit that differs from the defaults.
    bool hasNonDefaultEdits(int64_t imageId);
    bool isInTransaction() const { return inTransaction_; }
    // Shared statement bodies for addImage/storePreview; `db` selects the
    // primary or the import connection, `errOut` the matching error slot.
    int64_t addImageImpl(sqlite3* db, const std::string& filePath,
                         const std::string& thumbnailPath, bool isManaged,
                         std::string* errOut);
    bool storePreviewImpl(sqlite3* db, int64_t imageId, PreviewKind kind,
                          const std::vector<uint8_t>& data, int width,
                          int height, std::string* errOut);
    // Finalizes every statement held by stmtCache_. Called from close() so the
    // handles do not leak and sqlite3_close() can succeed, and from backup() so
    // cached statements cannot block the WAL checkpoint with SQLITE_LOCKED.
    void finalizeAllCached() const;

    std::filesystem::path path_;
    sqlite3* db_ = nullptr;
    bool inTransaction_ = false;
    mutable std::string lastError_;
    sqlite3* importDb_ = nullptr;
    bool importInTransaction_ = false;
    mutable std::string importError_;
    mutable std::mutex importMutex_;
    // Prepared-statement cache (key = SQL text). Statements are reset and
    // re-bound before every use and kept for the lifetime of the database,
    // avoiding a prepare/finalize cycle on every hot-path call. All access
    // happens while mutex_ is held, so no extra locking is required.
    mutable std::unordered_map<std::string, sqlite3_stmt*> stmtCache_;
    mutable std::recursive_mutex mutex_;
};

} // namespace lrc
