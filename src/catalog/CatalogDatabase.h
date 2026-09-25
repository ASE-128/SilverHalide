#pragma once

/**
 * @brief SQLite-backed catalog storing the whole image library in a single
 *        `.lrcat`-style file. Pure C++ — no Qt.
 *
 * Thread-safe: every public method acquires an internal recursive mutex, so
 * background import workers can write while the UI reads. Fallback to WAL
 * journaling keeps readers and writers from blocking each other.
 */

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

    // --- IPTC / metadata ------------------------------------------------
    std::string title;     // IPTC title
    std::string caption;   // IPTC description / caption
    std::string copyright; // IPTC rights
    std::string creator;   // IPTC creator
    std::string dateTaken; // ISO-8601 capture date (empty until populated)
    std::string camera;    // camera model string (empty until populated)
    std::string lens;      // lens model string (empty until populated)
};

// P-B B-2 import pregrade marker (persisted on the images row).
struct PregradeRecord {
    bool hasSubject = false;  // foreground saliency detected
    int score = 0;            // quality score in [0,100]
    bool suggestAuto = false; // tonal stats recommend B-1 auto-develop
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
    // User-facing snapshot label. Empty for ordinary (unlabelled) undo
    // history entries produced automatically by saveAdjustments(); set for a
    // named snapshot created via the snapshot panel.
    std::string name;
};

// A virtual copy / edit variant of a photo. variantId 0 is the master copy
// that shares the physical file; higher ids are independent edit stacks that
// share the same thumbnail and previews.
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

/**
 * @brief Evaluates a parsed smart-collection predicate against one image.
 *
 * The predicate follows {"match":"all"|"any","rules":[{field,op,value}]}.
 * `keywords` holds the image's keyword full paths; `hasAdjustments` reports
 * whether the image carries non-default develop edits. Pure function, kept
 * free so it can be unit-tested without a database.
 *
 * @param predicate  The parsed `{"match", "rules"}` rule document.
 * @param img        The image to test.
 * @param keywords   The image's keyword full paths (may be empty).
 * @param hasAdjustments Whether the image carries non-default develop edits.
 * @return true when the predicate matches; an empty/invalid rule set never
 *         matches.
 */
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
    /**
     * @brief Opens a catalog stored at `dbPath` (the SQLite database file),
     *        with externalized preview blobs kept under `previewDir`.
     *
     * Historically the catalog was a single `.lrcat` file; UI callers now point
     * at a catalog *folder* and resolve the database file plus a
     * `previews.shcat/` data directory from it (Lightroom-style layout). When a
     * caller still holds only a `.lrcat` path, an adjacent
     * `<stem> Previews.shcat/` directory is derived automatically.
     *
     * @param dbPath     Path to the SQLite catalog file.
     * @param previewDir Directory holding externalized preview files (created
     *                   on first use). Defaults to a sibling of dbPath.
     */
    explicit CatalogDatabase(std::filesystem::path dbPath,
                             std::filesystem::path previewDir = {});
    ~CatalogDatabase();

    CatalogDatabase(const CatalogDatabase&) = delete;
    CatalogDatabase& operator=(const CatalogDatabase&) = delete;

    /**
     * @brief Opens the catalog connection and prepares the schema.
     *
     * Creates the schema and runs any pending versioned migrations when the
     * file is new or from an older build, then finishes deferred preview
     * externalization. Any failure closes the connection and leaves the
     * catalog unusable so a partially migrated file is never used.
     *
     * @return true on success; false leaves lastError() populated.
     */
    bool open();
    void close();  ///< Closes the connection and releases all cached statements.
    bool isOpen() const { return db_ != nullptr; }
    std::string lastError() const { return lastError_; }
    // Number of prepared statements currently held in the reuse cache.
    // Observability hook used by the tests to confirm statements are cached
    // and reused rather than re-prepared on every call.
    size_t stmtCacheSize() const { return stmtCache_.size(); }

    // --- images ----------------------------------------------------------
    /**
     * @brief Inserts a new image, or returns the existing row on a re-import.
     *
     * Imports are idempotent: when `filePath` already exists the existing row
     * id is returned. `fileSize` / `fileMtime` are the on-disk fingerprint
     * (bytes and unix-last-write-time) used to detect later external
     * modification; 0 means "unknown / keep current". On an idempotent hit the
     * fingerprint is refreshed so re-imports never leave stale sizes behind.
     *
     * @return The image row id, or -1 on failure.
     */
    int64_t addImage(const std::string& filePath,
                     const std::string& thumbnailPath,
                     bool isManaged = false,
                     int64_t fileSize = 0, int64_t fileMtime = 0);
    // Updates the on-disk fingerprint of an existing row. Returns false when the
    // image is absent.
    bool setFingerprint(int64_t id, int64_t fileSize, int64_t fileMtime);
    // Returns the ids whose stored fingerprint no longer matches the given
    // (size, mtime) — i.e. externally modified files to re-import / re-decode.
    std::vector<int64_t> detectModifiedImages(int64_t fileSize,
                                              int64_t fileMtime) const;
    bool setRating(int64_t id, int rating);
    bool setLabelColor(int64_t id, int color);
    bool setFlag(int64_t id, int flag);
    bool setThumbnailPath(int64_t id, const std::string& thumbnailPath);
    bool setFullResPath(int64_t id, const std::string& fullresPath);
    bool setManaged(int64_t id, bool isManaged);

    // --- P-B B-2 import pregrade markers ------------------------------------
    /**
     * @brief Persists the batch-pregrade marker for an image (B-2).
     *
     * @param id          Image row id.
     * @param hasSubject  Foreground-saliency flag.
     * @param score       Deterministic quality score in [0,100] (clamped).
     * @param suggestAuto Whether tonal stats recommend B-1 auto-develop.
     * @return true on success, false when the image is absent / write failed.
     *
     * @note score is clamped into [0,100]; the stored default for "not yet
     *       pregraded" is -1 (see migrateV5).
     */
    bool setPregrade(int64_t id, bool hasSubject, int score, bool suggestAuto);
    /**
     * @brief Reads the pregrade marker for an image.
     *
     * @return nullopt when the image is absent or has not been pregraded
     *         (stored score == -1).
     */
    std::optional<PregradeRecord> getPregrade(int64_t id) const;

    bool removeImage(int64_t id);
    std::vector<ImageRecord> listImages() const;
    // Filtered listing (empty/default filter returns everything).
    std::vector<ImageRecord> listImages(const ImageFilter& filter) const;
    std::optional<ImageRecord> findImageByPath(const std::string& filePath) const;
    /** Look up a single image by primary key. Empty when id <= 0 or absent. */
    std::optional<ImageRecord> findImageById(int64_t id) const;
    /** Updates the stored file path for an image after a rename/move on disk.
     *  Returns false when `newFilePath` already belongs to another row (would
     *  violate the UNIQUE(file_path) constraint) or the image is absent. */
    bool updateImagePath(int64_t id, const std::string& newFilePath);
    /**
     * @brief Stores an embedded preview (thumbnail / full-res / standard).
     *
     * The API keeps a byte-vector form so existing UI callers
     * (MainWindow / PreviewWorker) need no changes; the bytes are written to
     * disk under `previews.shcat/<hex>/<imageId>_<kind>.shcat` and only the
     * path column is persisted in the database (externalized BLOBs).
     *
     * @return false when the database is closed or the write fails.
     */
    bool storePreview(int64_t imageId, PreviewKind kind,
                      const std::vector<uint8_t>& data,
                      int width = 0, int height = 0);
    // Returns the embedded preview bytes by reading the externalized file, or
    // nullopt when absent.
    std::optional<std::vector<uint8_t>> loadPreview(
        int64_t imageId, PreviewKind kind) const;
    // Embedded preview pixel dimensions (0,0 when not recorded), or nullopt
    // when the preview is absent.
    std::optional<std::pair<int, int>> previewSize(
        int64_t imageId, PreviewKind kind) const;
    // Absolute path of the externalized preview file for (imageId, kind), or
    // nullopt when it is not recorded. Callers that read the bytes themselves
    // (e.g. mmap / streaming) can use this to skip the byte round-trip.
    std::optional<std::filesystem::path> previewFilePath(
        int64_t imageId, PreviewKind kind) const;

    // --- standard preview --------------------------------------------------
    /**
     * @brief Stores a standard preview.
     *
     * A standard preview is linear ProPhoto RGB F32 at a 2048px longest edge
     * inside the catalog previews table, replacing any previous value.
     *
     * @return false when the buffer is empty / not F32 or the write fails.
     */
    bool storeStandardPreview(int64_t imageId, const ImageBuffer& buf);
    /**
     * @brief Returns the stored standard preview as a linear ProPhoto RGB F32
     *        buffer.
     *
     * @return nullopt when absent (old catalogs simply have no Standard
     *         record) or when the stored blob is malformed. Never crashes on a
     *         corrupt record.
     */
    std::optional<std::shared_ptr<ImageBuffer>> getStandardPreview(
        int64_t imageId) const;

    /** @brief Like getStandardPreview but also returns the native full-res
     *         dimensions carried in the v2 blob header (0 for v1 blobs).
     *
     *  The preview pipeline uses these to scale spatial kernels correctly; the
     *  UI uses them to report the photo's native size without a RAW decode. */
    struct DecodedStandardPreviewEntry {
        ImageBufferPtr buf;
        int nativeWidth = 0;
        int nativeHeight = 0;
    };
    std::optional<DecodedStandardPreviewEntry> getStandardPreviewWithMeta(
        int64_t imageId) const;

    // True when a Standard preview record exists for the image (regardless of
    // whether its blob is decodable -- use getStandardPreview to check that).
    bool hasStandardPreview(int64_t imageId) const;

    // Removes every preview row (embedded thumb/full-res and standard) for the
    // image. Called when the source file is modified in place so a stale
    // cached decode is not reused; the pixels are re-decoded on next load.
    bool invalidatePreviews(int64_t imageId);

    // --- catalog management ----------------------------------------------
    // Writes a consistent backup of the catalog file (WAL checkpointed).
    bool backup(const std::filesystem::path& destPath) const;
    // Runs VACUUM to reclaim space and rebuild indexes.
    bool optimize();

    // --- settings (catalog-level key/value state) -------------------------
    bool setSetting(const std::string& key, const std::string& value);
    std::optional<std::string> getSetting(const std::string& key) const;

    // --- edits (non-destructive parameter stack) -------------------------
    /**
     * @brief Saves the current parameter stack and records an edit-history
     *        snapshot.
     *
     * The upsert of the live edit and the history snapshot are one atomic unit,
     * wrapped in their own transaction so a snapshot write failure rolls back
     * the upsert instead of leaving conflicting undo state.
     *
     * @param variantId Selects which virtual copy's stack to write (0 = master).
     */
    bool saveAdjustments(int64_t imageId, const Adjustments& adjustments,
                         int variantId = 0);
    std::optional<Adjustments> loadAdjustments(int64_t imageId,
                                               int variantId = 0) const;

    // --- edit history (undo snapshots, newest last) -----------------------
    // `name` labels a snapshot. Empty names produce ordinary unlabelled
    // undo-history entries (the automatic saveAdjustments() path); non-empty
    // names mark user-created snapshots shown by the snapshot panel.
    int64_t saveEditSnapshot(int64_t imageId, const Adjustments& adjustments,
                             int variantId = 0, const std::string& name = {});
    std::vector<EditSnapshot> listEditHistory(int64_t imageId,
                                              int variantId = 0) const;
    std::optional<Adjustments> loadEditSnapshot(int64_t imageId,
                                                int64_t snapshotId,
                                                int variantId = 0) const;
    bool deleteEditSnapshot(int64_t imageId, int64_t snapshotId,
                            int variantId = 0);
    // Renames a history/snapshot record (used by the snapshot panel). Returns
    // false when the record does not exist for this image/variant.
    bool renameSnapshot(int64_t imageId, int64_t snapshotId,
                        const std::string& name, int variantId = 0);

    // --- virtual copies ---------------------------------------------------
    /**
     * @brief Creates a virtual copy of `imageId`.
     *
     * Copies the master (variant 0) edits and edit history into a fresh
     * variant id. All variants of one image share the same physical file and
     * previews; only the edit stacks / history are independent.
     *
     * @return The new variant id, or -1 on failure.
     */
    int createVirtualCopy(int64_t imageId);
    // Lists the virtual copies (including the master) registered for an image.
    std::vector<EditVariant> listVariants(int64_t imageId) const;
    bool renameVariant(int64_t imageId, int variantId,
                       const std::string& name);
    // Deletes a virtual copy (its edits/history/variant row). Deleting the
    // master is refused while virtual copies exist.
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

    // --- keywords & metadata -----------------------------------------------
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

    // --- smart collections ---------------------------------------------------
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

    // --- panorama sources ----------------------------------------------------
    // Records (replacing) the source images that produced a panorama. Calling
    // twice with the same set is a no-op.
    bool recordPanoramaSources(int64_t panoramaImageId,
                               const std::vector<int64_t>& sourceImageIds);
    std::vector<int64_t> listPanoramaSources(int64_t panoramaImageId) const;

    // --- photo_features (duplicate detection / stacking) --------------------
    // pHash (64-bit perceptual), 64-dim embedding (raw BLOB), and an
    // automatically-bumped model_version so the deep-embedding stage can be
    // re-run when the model is updated. Duplicate scanning compares phash
    // first (fast) then re-ranks by embedding cosine distance for pairs whose
    // phash hamming-distance is within the configured threshold.
    bool storePhotoFeatures(int64_t imageId, uint64_t perceptualHash,
                            const std::vector<float>& embedding,
                            int modelVersion = 1);
    // Returns (phash, model_version, embedding-vector) for an image, or
    // nullopt when the row does not exist.  The returned embedding is always
    // empty when the database BLOB is malformed — never crashes on legacy
    // catalogs or on corrupt records (catch-all per-call error handling).
    struct PhotoFeatureRow {
        uint64_t perceptualHash = 0;
        int modelVersion = 0;
        std::vector<float> embedding;
    };
    std::optional<PhotoFeatureRow> getPhotoFeatures(int64_t imageId) const;
    // Returns image_ids whose phash hamming distance <= `maxHamming` against
    // every row in the catalog — the call site then re-ranks by embedding
    // cosine distance (so distances larger than maxHamming never reach the
    // expensive re-rank step).  Result is (aId, bId, hammingDistance) for
    // every unique aId < bId pair.
    struct PhashCandidatePair {
        int64_t aId;
        int64_t bId;
        int hamming;
    };
    std::vector<PhashCandidatePair> scanPhashCandidates(
        int maxHamming = 8) const;
    // Approximate nearest neighbours in the 64-dim embedding space via the
    // sqlite-vec vec0 virtual table. Returns the k closest image ids
    // ordered by ascending distance, or empty when the vec index is
    // unavailable (sqlite-vec not linked) or has no data.
    std::vector<int64_t> annByEmbedding(const std::vector<float>& query,
                                        int k = 200) const;
    // Rebuilds the derived search data (keyword/phash-segment/embedding
    // indices). Returns false only when the database is closed.
    bool rebuildSearchIndexes();
    // Stack management (duplicate groups).  stackId == 0 means "not stacked".
    bool setStackId(int64_t imageId, int64_t stackId);
    // Returns every image id belonging to the same stack (including the
    // passed-in id itself).  Empty when the image is not in a stack.
    std::vector<int64_t> getStackMembers(int64_t stackId) const;
    // Next available stack id: MAX(stack_id) + 1 from the rows that are
    // stacked at all, or 1 when no stack has ever been created.
    int64_t nextStackId() const;

    // --- transactions -------------------------------------------------------
    // Wrap a batch of writes on the single catalog connection. The import
    // worker drives its batch through these (single connection + a
    // busy_timeout replaces the historical dual-connection design) so a
    // cancelled import's rollback can never undo an edit the user saved while
    // the import ran concurrently.
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

private:
    // --- versioned schema migrations -----------------------------------------
    // Runs every migration 1..kSchemaVersion whose ledger is not yet recorded,
    // each inside its own transaction (see migrateSqlVersion). Returns false,
    // leaving the connection unusable, when any step fails so a half-applied
    // schema is never left behind.
    bool applyMigrations();
    // Runs the transactional SQL body for schema version `v`: suspends FK
    // checks, BEGIN IMMEDIATE, dispatches migrateV1..V4, then on success commits
    // and (outside the transaction) bumps PRAGMA user_version and inserts the
    // schema_migrations ledger row. Any failure ROLLBACKs the whole step.
    bool migrateSqlVersion(int v, std::string* errOut);
    bool migrateV1(std::string* errOut);
    bool migrateV2(std::string* errOut);
    bool migrateV3(std::string* errOut);
    bool migrateV4(std::string* errOut);
    bool migrateV5(std::string* errOut);
    // Migration-scoped ALTER TABLE ADD COLUMN on images / an arbitrary table,
    // used only from within a migration transaction.
    void migrateEnsureColumn(const char* column, const char* decl);
    void migrateEnsureColumnOn(const char* table, const char* column,
                               const char* decl);
    // Resumable export of legacy `previews` BLOBs to externalized files.
    // Idempotent: rows already exported (path column populated / `_bak` row
    // dropped) are skipped, so an interrupted migration continues cleanly.
    bool finishPreviewExport();

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
    // Shared insert body for addImage; `errOut` selects the error slot.
    int64_t addImageImpl(sqlite3* db, const std::string& filePath,
                         const std::string& thumbnailPath, bool isManaged,
                         int64_t fileSize, int64_t fileMtime,
                         std::string* errOut);
    // Shared insert/upsert body for storePreview on a specific connection.
    // Writes the bytes to an externalized file and records only the path
    // column. `errOut` selects the error slot.
    bool storePreviewImpl(sqlite3* db, int64_t imageId, PreviewKind kind,
                          const std::vector<uint8_t>& data, int width, int height,
                          std::string* errOut);
    // Finalizes every statement held by stmtCache_. Called from close() so the
    // handles do not leak and sqlite3_close() can succeed, and from backup() so
    // cached statements cannot block the WAL checkpoint with SQLITE_LOCKED.
    void finalizeAllCached() const;

    std::filesystem::path path_;
    std::filesystem::path previewDir_;  // externalized preview files root
    sqlite3* db_ = nullptr;
    bool inTransaction_ = false;
    mutable std::string lastError_;
    // Prepared-statement cache (key = SQL text). Statements are reset and
    // re-bound before every use and kept for the lifetime of the database,
    // avoiding a prepare/finalize cycle on every hot-path call. All access
    // happens while mutex_ is held, so no extra locking is required.
    mutable std::unordered_map<std::string, sqlite3_stmt*> stmtCache_;
    mutable std::recursive_mutex mutex_;

    // Resolves the on-disk path for a preview blob, creating the shard
    // directory as needed, and the preview-root path. `mkdir` must be true for
    // writers. When no preview root is configured, a sibling `<stem>
    // Previews.shcat/` is derived from the database file.
    std::filesystem::path previewRootDir() const;
    std::filesystem::path previewPathFor(int64_t imageId, PreviewKind kind,
                                         bool mkdir) const;
};

} // namespace lrc
