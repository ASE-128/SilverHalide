#include "catalog/CatalogDatabase.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

#include <sqlite3.h>

#include "catalog/StandardPreview.h"
#include "utils/ProfileIO.h"

namespace lrc {

namespace {

std::string nowIso8601Utc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// RAII for sqlite prepared statements.
//
// Two modes:
//  * Uncached (two-arg ctor): prepares on construction and finalizes on
//    destruction. Used for one-shot statements outside the catalog methods.
//  * Cached (three-arg ctor): the statement is owned by the CatalogDatabase
//    stmtCache_ map (key = SQL text) and is reused across calls. On
//    construction the statement is looked up (or prepared and inserted) and
//    then reset + cleared; on destruction it stays in the cache, so the handle
//    is NOT finalized. This preserves the RAII exception safety: an early
//    return still hands the handle back to the cache ready for the next use.
struct Stmt {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    std::unordered_map<std::string, sqlite3_stmt*>* cache = nullptr;

    Stmt(sqlite3* d, const char* sql) : db(d) {
        sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    }
    Stmt(sqlite3* d, const char* sql,
         std::unordered_map<std::string, sqlite3_stmt*>* c)
        : db(d), cache(c) {
        const auto it = cache->find(sql);
        if (it != cache->end()) {
            stmt = it->second;
            resetAndClear(stmt);
        } else {
            sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
            if (stmt)
                (*cache)[sql] = stmt;
        }
    }
    ~Stmt() {
        // Cached statements are reset + cleared here so the handle is never
        // left active holding a read lock, then returned to the cache for
        // reuse. Only the uncached two-arg form finalizes its handle.
        if (stmt && cache) {
            resetAndClear(stmt);
        } else if (stmt) {
            sqlite3_finalize(stmt);
        }
    }
    bool ok() const { return stmt != nullptr; }
    int step() {
        const int rc = sqlite3_step(stmt);
        // This amalgamation build does not rewind a statement when it is
        // re-bound after SQLITE_DONE, and binding a done statement returns
        // SQLITE_MISUSE. Reset as soon as execution finishes so the statement
        // is immediately bindable again; reusing one Stmt for a bind+step loop
        // therefore runs once per bind. SELECT while(step()==ROW) loops are
        // unaffected: they exit on DONE and never call step() again.
        if (rc == SQLITE_DONE || rc == SQLITE_ERROR)
            sqlite3_reset(stmt);
        return rc;
    }
    // Rewinds a cached statement and drops every previously bound parameter so
    // a shorter run does not inherit stale bindings from a longer one.
    static void resetAndClear(sqlite3_stmt* s) {
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
};

// Maps the current row to an ImageRecord. Callers must project the columns in
// this exact order: id, file_path, thumbnail_path, rating, label_color, flag,
// import_date, is_managed, fullres_path, title, caption, copyright, creator,
// date_taken, camera, lens. Shared by listImages / findImageByPath /
// listImagesInCollection / searchImages so the mapping logic lives in one
// place.
ImageRecord readImageRecord(sqlite3_stmt* stmt) {
    ImageRecord r;
    r.id = sqlite3_column_int64(stmt, 0);
    if (const unsigned char* p = sqlite3_column_text(stmt, 1))
        r.filePath = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 2))
        r.thumbnailPath = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 8))
        r.fullresPath = reinterpret_cast<const char*>(p);
    r.rating = sqlite3_column_int(stmt, 3);
    r.labelColor = sqlite3_column_int(stmt, 4);
    r.flag = sqlite3_column_int(stmt, 5);
    if (const unsigned char* p = sqlite3_column_text(stmt, 6))
        r.importDate = reinterpret_cast<const char*>(p);
    r.isManaged = sqlite3_column_int(stmt, 7) != 0;
    if (const unsigned char* p = sqlite3_column_text(stmt, 9))
        r.title = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 10))
        r.caption = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 11))
        r.copyright = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 12))
        r.creator = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 13))
        r.dateTaken = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 14))
        r.camera = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(stmt, 15))
        r.lens = reinterpret_cast<const char*>(p);
    return r;
}

// Canonical image-column projection shared by every SELECT that feeds
// readImageRecord. Order must match the readImageRecord column indices above.
const char* kImageColumns =
    "id, file_path, thumbnail_path, rating, label_color, flag, import_date, "
    "is_managed, fullres_path, title, caption, copyright, creator, date_taken, "
    "camera, lens";

// --- smart-collection predicate evaluation (M9) ----------------------------

std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

double parseNumberText(const std::string& s) {
    if (s.empty())
        return 0.0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return (end && *end == '\0') ? v : 0.0;
}

// Maps a colour-label name (English or Chinese, case-insensitive) to the
// 1..7 label code. Returns -1 when the name is not a known label.
int labelColorNameToInt(const std::string& name) {
    const std::string n = toLowerCopy(name);
    if (n == "none" || n == "\xE6\x97\xA0")
        return 0;
    if (n == "red" || n == "\xE7\xBA\xA2")
        return 1;
    if (n == "orange" || n == "\xE6\xA9\x99")
        return 2;
    if (n == "yellow" || n == "\xE9\xBB\x84")
        return 3;
    if (n == "green" || n == "\xE7\xBB\xBF")
        return 4;
    if (n == "cyan" || n == "\xE9\x9D\x92")
        return 5;
    if (n == "blue" || n == "\xE8\x93\x9D")
        return 6;
    if (n == "purple" || n == "\xE7\xB4\xAB")
        return 7;
    return -1;
}

// Lowercase file extension without the leading dot ("" for no extension).
std::string fileTypeOf(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return "";
    return toLowerCopy(path.substr(dot + 1));
}

// Parent directory of a path ("" when there is no directory component).
std::string folderPathOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
        return "";
    return path.substr(0, slash);
}

// Numeric comparison with ==, !=, <, <=, >, >= plus array-backed in/between.
bool numericMatch(double lhs, const std::string& op, const json::Value& value) {
    if (op == "in") {
        if (!value.isArray())
            return false;
        for (const auto& item : value.items()) {
            if (item.isNumber() && lhs == item.asNumber())
                return true;
            if (item.isString() && lhs == parseNumberText(item.asString()))
                return true;
        }
        return false;
    }
    if (op == "between") {
        if (!value.isArray() || value.size() < 2)
            return false;
        const double lo = value[0].isNumber()
                              ? value[0].asNumber()
                              : parseNumberText(value[0].asString());
        const double hi = value[1].isNumber()
                              ? value[1].asNumber()
                              : parseNumberText(value[1].asString());
        return lhs >= lo && lhs <= hi;
    }
    double rhs = 0.0;
    if (value.isNumber())
        rhs = value.asNumber();
    else if (value.isString())
        rhs = parseNumberText(value.asString());
    else if (value.isBool())
        rhs = value.asBool() ? 1.0 : 0.0;
    else
        return false;
    if (op == "==")
        return lhs == rhs;
    if (op == "!=")
        return lhs != rhs;
    if (op == "<")
        return lhs < rhs;
    if (op == "<=")
        return lhs <= rhs;
    if (op == ">")
        return lhs > rhs;
    if (op == ">=")
        return lhs >= rhs;
    return false;
}

// String comparison: order/equality operators plus contains / starts_with and
// array-backed in / between (ISO dates compare lexicographically).
bool stringMatch(const std::string& lhs, const std::string& op,
                 const json::Value& value) {
    if (op == "in") {
        if (!value.isArray())
            return false;
        for (const auto& item : value.items()) {
            if (item.isString() && lhs == item.asString())
                return true;
            if (item.isNumber() && lhs == json::Value(item.asNumber()).serialize())
                return true;
        }
        return false;
    }
    if (op == "between") {
        if (!value.isArray() || value.size() < 2)
            return false;
        const std::string lo = value[0].isString() ? value[0].asString()
                                                   : value[0].serialize();
        const std::string hi = value[1].isString() ? value[1].asString()
                                                   : value[1].serialize();
        return lhs >= lo && lhs <= hi;
    }
    std::string rhs;
    if (value.isString())
        rhs = value.asString();
    else if (value.isNumber())
        rhs = json::Value(value.asNumber()).serialize();
    else
        return false;
    if (op == "==")
        return lhs == rhs;
    if (op == "!=")
        return lhs != rhs;
    if (op == "<")
        return lhs < rhs;
    if (op == "<=")
        return lhs <= rhs;
    if (op == ">")
        return lhs > rhs;
    if (op == ">=")
        return lhs >= rhs;
    if (op == "contains")
        return lhs.find(rhs) != std::string::npos;
    if (op == "starts_with")
        return lhs.rfind(rhs, 0) == 0;
    return false;
}

// Keyword matching against a set of keyword full paths. The value is matched
// (case-insensitively) against both full paths and leaf names.
bool keywordMatch(const std::string& op, const json::Value& value,
                  const std::vector<std::string>& keywords) {
    std::vector<std::string> tokens;
    for (const auto& kw : keywords) {
        const std::string lower = toLowerCopy(kw);
        tokens.push_back(lower);
        const size_t slash = lower.rfind('/');
        if (slash != std::string::npos)
            tokens.push_back(lower.substr(slash + 1));
    }
    const auto hasToken = [&tokens](const std::string& needle) {
        for (const auto& t : tokens)
            if (t == needle)
                return true;
        return false;
    };

    if (op == "in") {
        if (!value.isArray())
            return false;
        for (const auto& item : value.items())
            if (item.isString() && hasToken(toLowerCopy(item.asString())))
                return true;
        return false;
    }

    std::string needle;
    if (value.isString())
        needle = toLowerCopy(value.asString());
    else if (value.isNumber())
        needle = json::Value(value.asNumber()).serialize();
    else
        return false;

    if (op == "==")
        return hasToken(needle);
    if (op == "!=") {
        for (const auto& t : tokens)
            if (t == needle)
                return false;
        return true; // no assigned keyword matches -> the negative holds
    }
    if (op == "contains") {
        for (const auto& t : tokens)
            if (t.find(needle) != std::string::npos)
                return true;
        return false;
    }
    if (op == "starts_with") {
        for (const auto& t : tokens)
            if (t.rfind(needle, 0) == 0)
                return true;
        return false;
    }
    return false;
}

// The string value used by string-ish rule fields.
std::string fieldStringValue(const ImageRecord& img, const std::string& field) {
    if (field == "camera")
        return img.camera;
    if (field == "lens")
        return img.lens;
    if (field == "file_type")
        return fileTypeOf(img.filePath);
    if (field == "folder_path")
        return folderPathOf(img.filePath);
    if (field == "date_taken")
        return img.dateTaken;
    if (field == "title")
        return img.title;
    if (field == "caption")
        return img.caption;
    return "";
}

// Evaluates one rule object against an image.
bool matchRule(const json::Value& rule, const ImageRecord& img,
               const std::vector<std::string>& keywords, bool hasAdjustments) {
    if (!rule.isObject())
        return false;
    const std::string field = rule["field"].asString();
    const std::string op = rule["op"].asString();
    const json::Value& value = rule["value"];

    if (field == "has_adjustments") {
        const bool want = value.asBool(false);
        if (op == "==")
            return hasAdjustments == want;
        if (op == "!=")
            return hasAdjustments != want;
        return false;
    }

    if (field == "rating" || field == "flag") {
        const double lhs =
            static_cast<double>(field == "rating" ? img.rating : img.flag);
        return numericMatch(lhs, op, value);
    }

    if (field == "label") {
        const double lhs = static_cast<double>(img.labelColor);
        if (value.isString()) {
            const int mapped = labelColorNameToInt(value.asString());
            if (mapped >= 0)
                return numericMatch(lhs, op, json::Value(mapped));
            return numericMatch(lhs, op, json::Value(parseNumberText(value.asString())));
        }
        return numericMatch(lhs, op, value);
    }

    if (field == "keyword")
        return keywordMatch(op, value, keywords);

    return stringMatch(fieldStringValue(img, field), op, value);
}

} // namespace

bool evaluatePredicate(const json::Value& predicate, const ImageRecord& img,
                       const std::vector<std::string>& keywords,
                       bool hasAdjustments) {
    if (!predicate.isObject())
        return false;
    const std::string match = toLowerCopy(predicate["match"].asString());
    const json::Value& rules = predicate["rules"];
    if (!rules.isArray())
        return false;
    size_t total = 0;
    size_t matched = 0;
    for (const auto& rule : rules.items()) {
        if (!rule.isObject())
            continue;
        ++total;
        if (matchRule(rule, img, keywords, hasAdjustments))
            ++matched;
    }
    if (total == 0)
        return false;
    return (match == "any") ? (matched > 0) : (matched == total);
}

namespace {

// Checks whether a table has a column (uses pragma_table_info).
bool tableHasColumn(sqlite3* db, const char* table, const char* column) {
    sqlite3_stmt* stmt = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM pragma_table_info('") +
                            table + "') WHERE name = ?1;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, column, -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count > 0;
}

// Adds `column` to `table` if it is missing (lightweight migration for
// catalogs created by older builds).
void ensureTableColumnImpl(sqlite3* db, const char* table, const char* column,
                           const char* decl) {
    if (tableHasColumn(db, table, column))
        return;
    std::string alter = "ALTER TABLE ";
    alter += table;
    alter += " ADD COLUMN ";
    alter += column;
    alter += ' ';
    alter += decl;
    alter += ';';
    sqlite3_exec(db, alter.c_str(), nullptr, nullptr, nullptr);
}

} // namespace

CatalogDatabase::CatalogDatabase(std::filesystem::path dbPath)
    : path_(std::move(dbPath)) {}

CatalogDatabase::~CatalogDatabase() { close(); }

bool CatalogDatabase::open() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_)
        return true;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(path_.string().c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    // M1: every PRAGMA that changes durability or cascade behaviour must be
    // verified. A silently failing foreign_keys PRAGMA would disable ON DELETE
    // CASCADE without any signal; a failing WAL/synchronous PRAGMA would leave
    // the catalog on a different journal mode than the rest of the app expects.
    // busy_timeout lets a concurrent writer (e.g. an active import on its own
    // connection) finish before a UI-thread write fails with SQLITE_BUSY.
    if (!execOn(db_, "PRAGMA foreign_keys=ON;", &lastError_) ||
        !execOn(db_, "PRAGMA journal_mode=WAL;", &lastError_) ||
        !execOn(db_, "PRAGMA synchronous=NORMAL;", &lastError_) ||
        !execOn(db_, "PRAGMA busy_timeout=5000;", &lastError_)) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    const char* kSchema =
        "CREATE TABLE IF NOT EXISTS images ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  file_path TEXT NOT NULL UNIQUE,"
        "  import_date TEXT NOT NULL,"
        "  rating INTEGER NOT NULL DEFAULT 0,"
        "  label_color INTEGER NOT NULL DEFAULT 0,"
        "  flag INTEGER NOT NULL DEFAULT 0,"
        "  thumbnail_path TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS edits ("
        "  image_id INTEGER NOT NULL,"
        "  variant_id INTEGER NOT NULL DEFAULT 0,"
        "  adjustment_json TEXT NOT NULL,"
        "  PRIMARY KEY (image_id, variant_id),"
        "  FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE"
        ");"
        "CREATE TABLE IF NOT EXISTS edit_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  image_id INTEGER NOT NULL,"
        "  variant_id INTEGER NOT NULL DEFAULT 0,"
        "  adjustment_json TEXT NOT NULL,"
        "  created_at TEXT NOT NULL,"
        "  FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_edit_history_image "
        "ON edit_history(image_id, id);"
        "CREATE TABLE IF NOT EXISTS variants ("
        "  image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
        "  variant_id INTEGER NOT NULL DEFAULT 0,"
        "  name TEXT NOT NULL DEFAULT '',"
        "  is_master INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (image_id, variant_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS keywords ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  parent_id INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS photo_keywords ("
        "  image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
        "  keyword_id INTEGER NOT NULL REFERENCES keywords(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (image_id, keyword_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_photo_keywords_keyword "
        "ON photo_keywords(keyword_id);"
        "CREATE TABLE IF NOT EXISTS smart_collections ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  predicate_json TEXT NOT NULL,"
        "  match_type TEXT NOT NULL DEFAULT 'ALL'"
        ");"
        "CREATE TABLE IF NOT EXISTS smart_collection_photos ("
        "  smart_collection_id INTEGER NOT NULL REFERENCES smart_collections(id) "
        "ON DELETE CASCADE,"
        "  image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (smart_collection_id, image_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS panorama_sources ("
        "  panorama_image_id INTEGER NOT NULL REFERENCES images(id) "
        "ON DELETE CASCADE,"
        "  source_image_id INTEGER NOT NULL REFERENCES images(id) "
        "ON DELETE CASCADE,"
        "  PRIMARY KEY (panorama_image_id, source_image_id)"
        ");"
        // PHASE_14: perceptual hash + 64-dim deep embedding per image.  Both
        // are updated atomically via storePhotoFeatures(); the stack_id
        // column lives on images itself (ensureColumn'd after this CREATE).
        "CREATE TABLE IF NOT EXISTS photo_features ("
        "  image_id INTEGER PRIMARY KEY NOT NULL REFERENCES images(id) "
        "ON DELETE CASCADE,"
        "  phash INTEGER NOT NULL DEFAULT 0,"
        "  model_version INTEGER NOT NULL DEFAULT 1,"
        "  embedding BLOB NOT NULL DEFAULT X''"
        ");"
        "CREATE TABLE IF NOT EXISTS folders ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  path TEXT NOT NULL UNIQUE,"
        "  parent_id INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_images_path ON images(file_path);"
        "CREATE TABLE IF NOT EXISTS collections ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  sort_order INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS collection_images ("
        "  collection_id INTEGER NOT NULL REFERENCES collections(id) "
        "ON DELETE CASCADE,"
        "  image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
        "  PRIMARY KEY (collection_id, image_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_collection_images_image "
        "ON collection_images(image_id);"
        "CREATE TABLE IF NOT EXISTS previews ("
        "  image_id INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,"
        "  kind INTEGER NOT NULL,"
        "  data BLOB NOT NULL,"
        "  width INTEGER NOT NULL DEFAULT 0,"
        "  height INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (image_id, kind)"
        ");";
    if (!exec(kSchema))
        return false;

    // Migrate catalogs created before the `flag` / `is_managed` /
    // `fullres_path` columns, and before the M6/M8 columns.
    ensureColumn("flag", "INTEGER NOT NULL DEFAULT 0");
    ensureColumn("is_managed", "INTEGER NOT NULL DEFAULT 0");
    ensureColumn("fullres_path", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("title", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("caption", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("copyright", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("creator", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("date_taken", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("camera", "TEXT NOT NULL DEFAULT ''");
    ensureColumn("lens", "TEXT NOT NULL DEFAULT ''");
    // PHASE_14: stack id for duplicate / burst stacking — 0 = unstacked.
    ensureColumn("stack_id", "INTEGER NOT NULL DEFAULT 0");

    // M6: edit_history gains variant_id (the edits table needs a full rebuild,
    // handled by ensureEditsVariant below because its old primary key cannot be
    // widened in place). The variant index is created after the column exists so
    // the migration also works on catalogs that predate M6.
    ensureColumnOn("edit_history", "variant_id",
                   "INTEGER NOT NULL DEFAULT 0");
    if (!exec("CREATE INDEX IF NOT EXISTS idx_edit_history_variant "
              "ON edit_history(image_id, variant_id, id);"))
        return false;
    ensureEditsVariant();

    // Query-plan indexes for the common listImages filters and the default
    // sort. Created after ensureColumn so they are valid even on catalogs
    // migrated from builds that predate the `flag` column; IF NOT EXISTS keeps
    // the migration idempotent on every re-open.
    if (!exec(
            "CREATE INDEX IF NOT EXISTS idx_images_rating ON images(rating);"
            "CREATE INDEX IF NOT EXISTS idx_images_flag ON images(flag);"
            "CREATE INDEX IF NOT EXISTS idx_images_import_date "
            "ON images(import_date);"
            "CREATE INDEX IF NOT EXISTS idx_images_label_color "
            "ON images(label_color);"))
        return false;
    return true;
}

void CatalogDatabase::close() {
    // Teardown closes the secondary import connection first (rolling back a
    // dangling transaction so no uncommitted writes linger in the WAL), then
    // the primary connection.
    std::lock_guard<std::mutex> importLock(importMutex_);
    if (importDb_) {
        if (importInTransaction_) {
            (void)execOn(importDb_, "ROLLBACK;", &importError_);
            importInTransaction_ = false;
        }
        sqlite3_close(importDb_);
        importDb_ = nullptr;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (db_) {
        // Release every cached prepared statement first; sqlite3_close() fails
        // (SQLITE_BUSY) while any statement is still outstanding.
        finalizeAllCached();
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void CatalogDatabase::finalizeAllCached() const {
    for (auto& [sql, stmt] : stmtCache_) {
        (void)sql;
        if (stmt)
            sqlite3_finalize(stmt);
    }
    stmtCache_.clear();
}

void CatalogDatabase::ensureColumn(const char* column, const char* decl) {
    ensureTableColumnImpl(db_, "images", column, decl);
}

void CatalogDatabase::ensureColumnOn(const char* table, const char* column,
                                     const char* decl) {
    ensureTableColumnImpl(db_, table, column, decl);
}

void CatalogDatabase::ensureEditsVariant() {
    if (tableHasColumn(db_, "edits", "variant_id"))
        return;
    // The old table's primary key is image_id (a rowid alias). Adding a
    // variant_id column via ALTER TABLE cannot widen that key, so the table is
    // rebuilt with the composite key and existing rows become variant 0. FK
    // checks are suspended only for this connection so the RENAME / DROP cannot
    // trip cascade checks mid-rebuild; open() runs before any import connection
    // exists, so no other connection can observe the window.
    (void)execOn(db_, "PRAGMA foreign_keys=OFF;", &lastError_);
    const bool ok =
        exec("BEGIN;") &&
        exec("ALTER TABLE edits RENAME TO edits_old;") &&
        exec("CREATE TABLE edits ("
             "  image_id INTEGER NOT NULL,"
             "  variant_id INTEGER NOT NULL DEFAULT 0,"
             "  adjustment_json TEXT NOT NULL,"
             "  PRIMARY KEY (image_id, variant_id),"
             "  FOREIGN KEY(image_id) REFERENCES images(id) ON DELETE CASCADE"
             ");") &&
        exec("INSERT INTO edits(image_id, variant_id, adjustment_json) "
             "SELECT image_id, 0, adjustment_json FROM edits_old;") &&
        exec("DROP TABLE edits_old;") &&
        exec("COMMIT;");
    if (!ok)
        (void)exec("ROLLBACK;");
    (void)execOn(db_, "PRAGMA foreign_keys=ON;", &lastError_);
}

bool CatalogDatabase::exec(const char* sql) {
    return execOn(db_, sql, &lastError_);
}

bool CatalogDatabase::execOn(sqlite3* db, const char* sql, std::string* errOut) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        *errOut = err ? err : "unknown sqlite error";
        sqlite3_free(err);
        return false;
    }
    return true;
}

int64_t CatalogDatabase::addImageImpl(sqlite3* db, const std::string& filePath,
                                      const std::string& thumbnailPath,
                                      bool isManaged, std::string* errOut) {
    // An existing row keeps its current managed state so re-importing the same
    // file (idempotent import) never flips a copied file to "added".
    Stmt ins(db,
             "INSERT OR IGNORE INTO images(file_path, import_date, "
             "thumbnail_path, is_managed) VALUES(?1, ?2, ?3, ?4);");
    if (!ins.ok()) {
        *errOut = sqlite3_errmsg(db);
        return -1;
    }
    const std::string date = nowIso8601Utc();
    sqlite3_bind_text(ins.stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.stmt, 2, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.stmt, 3, thumbnailPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(ins.stmt, 4, isManaged ? 1 : 0);
    if (ins.step() != SQLITE_DONE) {
        *errOut = sqlite3_errmsg(db);
        return -1;
    }

    Stmt sel(db, "SELECT id, is_managed FROM images WHERE file_path = ?1;");
    if (!sel.ok()) {
        *errOut = sqlite3_errmsg(db);
        return -1;
    }
    sqlite3_bind_text(sel.stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    if (sel.step() == SQLITE_ROW) {
        // Idempotent re-import: promote the record to managed when needed. The
        // UPDATE runs on the same connection so a re-import never mixes the
        // primary and import connections mid-statement.
        if (isManaged && sqlite3_column_int(sel.stmt, 1) == 0) {
            Stmt upd(db, "UPDATE images SET is_managed = 1 WHERE id = ?1;");
            if (!upd.ok()) {
                *errOut = sqlite3_errmsg(db);
                return -1;
            }
            sqlite3_bind_int64(upd.stmt, 1, sqlite3_column_int64(sel.stmt, 0));
            if (upd.step() != SQLITE_DONE) {
                *errOut = sqlite3_errmsg(db);
                return -1;
            }
        }
        return sqlite3_column_int64(sel.stmt, 0);
    }
    *errOut = sqlite3_errmsg(db);
    return -1;
}

int64_t CatalogDatabase::addImage(const std::string& filePath,
                                  const std::string& thumbnailPath,
                                  bool isManaged) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    return addImageImpl(db_, filePath, thumbnailPath, isManaged, &lastError_);
}

int64_t CatalogDatabase::addImageImport(const std::string& filePath,
                                        const std::string& thumbnailPath,
                                        bool isManaged) {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (!importDb_) {
        importError_ = "import connection not open";
        return -1;
    }
    return addImageImpl(importDb_, filePath, thumbnailPath, isManaged,
                        &importError_);
}

bool CatalogDatabase::setRating(int64_t id, int rating) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "UPDATE images SET rating = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(s.stmt, 1, rating);
    sqlite3_bind_int64(s.stmt, 2, id);
    if (s.step() != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

bool CatalogDatabase::setLabelColor(int64_t id, int color) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "UPDATE images SET label_color = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(s.stmt, 1, color);
    sqlite3_bind_int64(s.stmt, 2, id);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::setFlag(int64_t id, int flag) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (flag < -1 || flag > 1)
        return false;
    Stmt s(db_, "UPDATE images SET flag = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(s.stmt, 1, flag);
    sqlite3_bind_int64(s.stmt, 2, id);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::setThumbnailPath(int64_t id,
                                       const std::string& thumbnailPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "UPDATE images SET thumbnail_path = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(s.stmt, 1, thumbnailPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.stmt, 2, id);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::setFullResPath(int64_t id,
                                     const std::string& fullresPath) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "UPDATE images SET fullres_path = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(s.stmt, 1, fullresPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.stmt, 2, id);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::setManaged(int64_t id, bool isManaged) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "UPDATE images SET is_managed = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(s.stmt, 1, isManaged ? 1 : 0);
    sqlite3_bind_int64(s.stmt, 2, id);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::removeImage(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "DELETE FROM images WHERE id = ?1;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, id);
    return s.step() == SQLITE_DONE;
}

std::vector<ImageRecord> CatalogDatabase::listImages() const {
    return listImages(ImageFilter{});
}

std::vector<ImageRecord> CatalogDatabase::listImages(
    const ImageFilter& filter) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ImageRecord> out;
    if (!db_)
        return out;

    std::string sql = std::string("SELECT ") + kImageColumns + " FROM images";
    std::vector<std::pair<std::string, std::string>> wheres; // (clause, bound)
    if (filter.minRating > 0)
        wheres.emplace_back("rating >= ?", "rating");
    if (filter.flag != 0)
        wheres.emplace_back("flag = ?", "flag");
    if (filter.labelColor != 0)
        wheres.emplace_back("label_color = ?", "label");
    if (!filter.includeRejected)
        wheres.emplace_back("flag != -1", "");
    if (!filter.fileExtension.empty())
        wheres.emplace_back("file_path LIKE ?", "ext");
    for (size_t i = 0; i < wheres.size(); ++i) {
        sql += (i == 0) ? " WHERE " : " AND ";
        sql += wheres[i].first;
    }
    sql += " ORDER BY import_date DESC, id DESC;";

    Stmt s(db_, sql.c_str(), &stmtCache_);
    if (!s.ok())
        return out;
    int bindIndex = 1;
    for (const auto& [clause, name] : wheres) {
        if (name == "rating")
            sqlite3_bind_int(s.stmt, bindIndex++, filter.minRating);
        else if (name == "flag")
            sqlite3_bind_int(s.stmt, bindIndex++, filter.flag);
        else if (name == "label")
            sqlite3_bind_int(s.stmt, bindIndex++, filter.labelColor);
        else if (name == "ext")
            sqlite3_bind_text(s.stmt, bindIndex++,
                              ("%" + filter.fileExtension).c_str(), -1,
                              SQLITE_TRANSIENT);
        // plain clauses ("flag != -1") have no bound parameter
    }
    while (s.step() == SQLITE_ROW)
        out.push_back(readImageRecord(s.stmt));
    return out;
}

std::optional<ImageRecord> CatalogDatabase::findImageByPath(
    const std::string& filePath) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    const std::string sql = std::string("SELECT ") + kImageColumns +
                            " FROM images WHERE file_path = ?1;";
    Stmt s(db_, sql.c_str(), &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_text(s.stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    return readImageRecord(s.stmt);
}

std::optional<ImageRecord> CatalogDatabase::findImageById(int64_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_ || id <= 0)
        return std::nullopt;
    const std::string sql = std::string("SELECT ") + kImageColumns +
                            " FROM images WHERE id = ?1;";
    Stmt s(db_, sql.c_str(), &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, id);
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    return readImageRecord(s.stmt);
}

bool CatalogDatabase::storePreviewImpl(sqlite3* db, int64_t imageId,
                                       PreviewKind kind,
                                       const std::vector<uint8_t>& data,
                                       int width, int height,
                                       std::string* errOut) {
    Stmt s(db,
           "INSERT INTO previews(image_id, kind, data, width, height) "
           "VALUES(?1,?2,?3,?4,?5) "
           "ON CONFLICT(image_id, kind) DO UPDATE SET "
           "data=excluded.data, width=excluded.width, height=excluded.height;");
    if (!s.ok()) {
        *errOut = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, static_cast<int>(kind));
    sqlite3_bind_blob(s.stmt, 3, data.empty() ? nullptr : data.data(),
                      static_cast<int>(data.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(s.stmt, 4, width);
    sqlite3_bind_int(s.stmt, 5, height);
    if (s.step() != SQLITE_DONE) {
        *errOut = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool CatalogDatabase::storePreview(int64_t imageId, PreviewKind kind,
                                   const std::vector<uint8_t>& data,
                                   int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    return storePreviewImpl(db_, imageId, kind, data, width, height,
                            &lastError_);
}

bool CatalogDatabase::storePreviewImport(int64_t imageId, PreviewKind kind,
                                         const std::vector<uint8_t>& data,
                                         int width, int height) {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (!importDb_) {
        importError_ = "import connection not open";
        return false;
    }
    return storePreviewImpl(importDb_, imageId, kind, data, width, height,
                            &importError_);
}

std::optional<std::vector<uint8_t>> CatalogDatabase::loadPreview(
    int64_t imageId, PreviewKind kind) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_, "SELECT data FROM previews WHERE image_id=?1 AND kind=?2;",
           &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, static_cast<int>(kind));
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    const int bytes = sqlite3_column_bytes(s.stmt, 0);
    if (bytes == 0)
        return std::vector<uint8_t>{};
    const auto* p = static_cast<const uint8_t*>(
        sqlite3_column_blob(s.stmt, 0));
    if (!p)
        return std::nullopt;
    return std::vector<uint8_t>(p, p + bytes);
}

std::optional<std::pair<int, int>> CatalogDatabase::previewSize(
    int64_t imageId, PreviewKind kind) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_,
           "SELECT width, height FROM previews "
           "WHERE image_id=?1 AND kind=?2;");
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, static_cast<int>(kind));
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    return std::make_pair(sqlite3_column_int(s.stmt, 0),
                          sqlite3_column_int(s.stmt, 1));
}

bool CatalogDatabase::storeStandardPreview(int64_t imageId,
                                           const ImageBuffer& buf) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (buf.empty() || buf.format() != PixelFormat::F32)
        return false;
    const std::vector<uint8_t> blob = encodeStandardPreview(buf);
    if (blob.empty())
        return false;
    return storePreviewImpl(db_, imageId, PreviewKind::Standard, blob,
                            buf.width(), buf.height(), &lastError_);
}

std::optional<std::shared_ptr<ImageBuffer>>
CatalogDatabase::getStandardPreview(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_, "SELECT data FROM previews WHERE image_id=?1 AND kind=?2;",
           &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, static_cast<int>(PreviewKind::Standard));
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    const int bytes = sqlite3_column_bytes(s.stmt, 0);
    if (bytes == 0)
        return std::nullopt;
    const auto* p =
        static_cast<const uint8_t*>(sqlite3_column_blob(s.stmt, 0));
    if (!p)
        return std::nullopt;
    std::vector<uint8_t> blob(p, p + bytes);
    auto decoded = decodeStandardPreview(blob);
    if (!decoded.buf || decoded.buf->empty())
        return std::nullopt;
    return decoded.buf;
}

std::optional<CatalogDatabase::DecodedStandardPreviewEntry>
CatalogDatabase::getStandardPreviewWithMeta(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_, "SELECT data FROM previews WHERE image_id=?1 AND kind=?2;",
           &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, static_cast<int>(PreviewKind::Standard));
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    const int bytes = sqlite3_column_bytes(s.stmt, 0);
    if (bytes == 0)
        return std::nullopt;
    const auto* p =
        static_cast<const uint8_t*>(sqlite3_column_blob(s.stmt, 0));
    if (!p)
        return std::nullopt;
    std::vector<uint8_t> blob(p, p + bytes);
    auto decoded = decodeStandardPreview(blob);
    if (!decoded.buf || decoded.buf->empty())
        return std::nullopt;
    return DecodedStandardPreviewEntry{decoded.buf, decoded.nativeWidth,
                                       decoded.nativeHeight};
}

bool CatalogDatabase::hasStandardPreview(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "SELECT 1 FROM previews WHERE image_id=?1 AND kind=?2;",
           &stmtCache_);
    if (!s.ok())
        return false;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, static_cast<int>(PreviewKind::Standard));
    return s.step() == SQLITE_ROW;
}

bool CatalogDatabase::saveAdjustments(int64_t imageId,
                                      const Adjustments& adjustments,
                                      int variantId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    const std::string jsonText = ProfileIO::toJson(adjustments).serialize();

    // M2: the edits upsert and the edit_history snapshot are one atomic unit.
    // Wrap them in a short-lived transaction of their own so a snapshot write
    // failure rolls back the upsert instead of leaving an undo history that
    // disagrees with the live edit. This runs on the primary connection and is
    // deliberately independent of the (separate) import transaction, so a
    // cancelled import can never roll a saved edit back.
    const bool nested = inTransaction_;
    if (!nested && !exec("BEGIN IMMEDIATE;"))
        return false;

    bool ok = true;
    {
        Stmt s(db_,
               "INSERT INTO edits(image_id, variant_id, adjustment_json) "
               "VALUES(?1, ?2, ?3) "
               "ON CONFLICT(image_id, variant_id) DO UPDATE SET "
               "adjustment_json = excluded.adjustment_json;", &stmtCache_);
        if (!s.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            ok = false;
        } else {
            sqlite3_bind_int64(s.stmt, 1, imageId);
            sqlite3_bind_int(s.stmt, 2, variantId);
            sqlite3_bind_text(s.stmt, 3, jsonText.c_str(), -1, SQLITE_TRANSIENT);
            if (s.step() != SQLITE_DONE) {
                lastError_ = sqlite3_errmsg(db_);
                ok = false;
            }
        }
    }
    if (ok) {
        // Every committed edit is also captured as a history snapshot so the
        // current state can be restored later without touching the original
        // file.
        Stmt ins(db_,
                 "INSERT INTO edit_history(image_id, variant_id, "
                 "adjustment_json, created_at) VALUES(?1, ?2, ?3, ?4);");
        if (!ins.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            ok = false;
        } else {
            sqlite3_bind_int64(ins.stmt, 1, imageId);
            sqlite3_bind_int(ins.stmt, 2, variantId);
            sqlite3_bind_text(ins.stmt, 3, jsonText.c_str(), -1,
                              SQLITE_TRANSIENT);
            const std::string date = nowIso8601Utc();
            sqlite3_bind_text(ins.stmt, 4, date.c_str(), -1, SQLITE_TRANSIENT);
            if (ins.step() != SQLITE_DONE) {
                lastError_ = sqlite3_errmsg(db_);
                ok = false;
            }
        }
    }

    if (nested)
        return ok;  // the enclosing transaction owns the commit / rollback.
    if (ok) {
        if (exec("COMMIT;"))
            return true;
        // COMMIT failed (disk full / locked); roll the two writes back so they
        // do not linger in an uncommitted transaction.
        (void)exec("ROLLBACK;");
        return false;
    }
    (void)exec("ROLLBACK;");
    return false;
}

bool CatalogDatabase::setSetting(const std::string& key,
                                 const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "INSERT INTO settings(key, value) VALUES(?1, ?2) "
           "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(s.stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    return s.step() == SQLITE_DONE;
}

std::optional<std::string> CatalogDatabase::getSetting(
    const std::string& key) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_, "SELECT value FROM settings WHERE key = ?1;", &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_text(s.stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    const unsigned char* p = sqlite3_column_text(s.stmt, 0);
    if (!p)
        return std::nullopt;
    return std::string(reinterpret_cast<const char*>(p));
}

int64_t CatalogDatabase::saveEditSnapshot(
    int64_t imageId, const Adjustments& adjustments, int variantId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    const std::string jsonText = ProfileIO::toJson(adjustments).serialize();
    Stmt ins(db_,
             "INSERT INTO edit_history(image_id, variant_id, adjustment_json, "
             "created_at) VALUES(?1, ?2, ?3, ?4);");
    if (!ins.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    sqlite3_bind_int64(ins.stmt, 1, imageId);
    sqlite3_bind_int(ins.stmt, 2, variantId);
    sqlite3_bind_text(ins.stmt, 3, jsonText.c_str(), -1, SQLITE_TRANSIENT);
    const std::string date = nowIso8601Utc();
    sqlite3_bind_text(ins.stmt, 4, date.c_str(), -1, SQLITE_TRANSIENT);
    if (ins.step() != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    return sqlite3_last_insert_rowid(db_);
}

std::vector<EditSnapshot> CatalogDatabase::listEditHistory(
    int64_t imageId, int variantId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EditSnapshot> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT id, image_id, created_at FROM edit_history "
           "WHERE image_id = ?1 AND variant_id = ?2 ORDER BY id;");
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, variantId);
    while (s.step() == SQLITE_ROW) {
        EditSnapshot snap;
        snap.id = sqlite3_column_int64(s.stmt, 0);
        snap.imageId = sqlite3_column_int64(s.stmt, 1);
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 2))
            snap.createdAt = reinterpret_cast<const char*>(p);
        out.push_back(std::move(snap));
    }
    return out;
}

std::optional<Adjustments> CatalogDatabase::loadEditSnapshot(
    int64_t imageId, int64_t snapshotId, int variantId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_,
           "SELECT adjustment_json FROM edit_history "
           "WHERE image_id = ?1 AND variant_id = ?2 AND id = ?3;");
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, variantId);
    sqlite3_bind_int64(s.stmt, 3, snapshotId);
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    const unsigned char* p = sqlite3_column_text(s.stmt, 0);
    if (!p)
        return std::nullopt;
    auto value = json::Value::parse(reinterpret_cast<const char*>(p));
    if (!value)
        return std::nullopt;
    return ProfileIO::fromJson(*value);
}

bool CatalogDatabase::deleteEditSnapshot(int64_t imageId,
                                        int64_t snapshotId, int variantId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "DELETE FROM edit_history "
           "WHERE image_id = ?1 AND variant_id = ?2 AND id = ?3;",
           &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, variantId);
    sqlite3_bind_int64(s.stmt, 3, snapshotId);
    return s.step() == SQLITE_DONE;
}

std::optional<Adjustments> CatalogDatabase::loadAdjustments(
    int64_t imageId, int variantId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_,
           "SELECT adjustment_json FROM edits "
           "WHERE image_id = ?1 AND variant_id = ?2;", &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, variantId);
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    const unsigned char* p = sqlite3_column_text(s.stmt, 0);
    if (!p)
        return std::nullopt;
    auto value = json::Value::parse(reinterpret_cast<const char*>(p));
    if (!value)
        return std::nullopt;
    return ProfileIO::fromJson(*value);
}

bool CatalogDatabase::hasNonDefaultEdits(int64_t imageId) {
    const auto adj = loadAdjustments(imageId, 0);
    if (!adj)
        return false;
    return !(*adj == Adjustments{});
}

int CatalogDatabase::createVirtualCopy(int64_t imageId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    if (!beginTransaction())
        return -1;

    // Register the master (variant 0) so MAX(variant_id) has a defined base.
    {
        Stmt master(db_,
                    "INSERT OR IGNORE INTO variants(image_id, variant_id, "
                    "name, is_master, created_at) VALUES(?1, 0, '', 1, ?2);",
                    &stmtCache_);
        if (!master.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
        sqlite3_bind_int64(master.stmt, 1, imageId);
        sqlite3_bind_int64(master.stmt, 2,
                           static_cast<int64_t>(std::time(nullptr)));
        if (master.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
    }

    int maxVariant = 0;
    {
        Stmt maxv(db_,
                  "SELECT COALESCE(MAX(variant_id), 0) FROM variants "
                  "WHERE image_id = ?1;", &stmtCache_);
        if (!maxv.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
        sqlite3_bind_int64(maxv.stmt, 1, imageId);
        if (maxv.step() != SQLITE_ROW) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
        maxVariant = sqlite3_column_int(maxv.stmt, 0);
    }
    const int newVariant = maxVariant + 1;

    // Copy the master's parameter stack and undo history to the new variant.
    {
        Stmt cpEdits(db_,
                     "INSERT OR IGNORE INTO edits(image_id, variant_id, "
                     "adjustment_json) SELECT image_id, ?2, adjustment_json "
                     "FROM edits WHERE image_id = ?1 AND variant_id = 0;",
                     &stmtCache_);
        if (!cpEdits.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
        sqlite3_bind_int64(cpEdits.stmt, 1, imageId);
        sqlite3_bind_int(cpEdits.stmt, 2, newVariant);
        if (cpEdits.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }

        Stmt cpHist(db_,
                    "INSERT INTO edit_history(image_id, variant_id, "
                    "adjustment_json, created_at) SELECT image_id, ?2, "
                    "adjustment_json, created_at FROM edit_history "
                    "WHERE image_id = ?1 AND variant_id = 0;", &stmtCache_);
        if (!cpHist.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
        sqlite3_bind_int64(cpHist.stmt, 1, imageId);
        sqlite3_bind_int(cpHist.stmt, 2, newVariant);
        if (cpHist.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
    }

    {
        Stmt ins(db_,
                 "INSERT INTO variants(image_id, variant_id, name, is_master, "
                 "created_at) VALUES(?1, ?2, ?3, 0, ?4);", &stmtCache_);
        if (!ins.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
        const std::string name = "\xe5\x89\xaf\xe6\x9c\xac " + std::to_string(newVariant);
        sqlite3_bind_int64(ins.stmt, 1, imageId);
        sqlite3_bind_int(ins.stmt, 2, newVariant);
        sqlite3_bind_text(ins.stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins.stmt, 4, static_cast<int64_t>(std::time(nullptr)));
        if (ins.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return -1;
        }
    }

    if (!commitTransaction()) {
        (void)rollbackTransaction();
        return -1;
    }
    return newVariant;
}

std::vector<EditVariant> CatalogDatabase::listVariants(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<EditVariant> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT variant_id, name, is_master FROM variants "
           "WHERE image_id = ?1 ORDER BY variant_id;", &stmtCache_);
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    while (s.step() == SQLITE_ROW) {
        EditVariant v;
        v.variantId = sqlite3_column_int64(s.stmt, 0);
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 1))
            v.name = reinterpret_cast<const char*>(p);
        v.isMaster = sqlite3_column_int(s.stmt, 2) != 0;
        out.push_back(std::move(v));
    }
    return out;
}

bool CatalogDatabase::renameVariant(int64_t imageId, int variantId,
                                    const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (name.empty())
        return false;
    Stmt s(db_,
           "UPDATE variants SET name = ?3 "
           "WHERE image_id = ?1 AND variant_id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int(s.stmt, 2, variantId);
    sqlite3_bind_text(s.stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    if (s.step() != SQLITE_DONE)
        return false;
    return sqlite3_changes(db_) > 0;
}

bool CatalogDatabase::deleteVariant(int64_t imageId, int variantId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (variantId == 0) {
        // PHASE_6: deleting the master is refused while virtual copies exist.
        Stmt cnt(db_,
                 "SELECT COUNT(*) FROM variants "
                 "WHERE image_id = ?1 AND variant_id != 0;", &stmtCache_);
        if (!cnt.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(cnt.stmt, 1, imageId);
        if (cnt.step() == SQLITE_ROW && sqlite3_column_int(cnt.stmt, 0) > 0)
            return false;
    }
    bool ok = beginTransaction();
    if (!ok)
        return false;
    {
        Stmt de(db_,
                "DELETE FROM edits WHERE image_id = ?1 AND variant_id = ?2;",
                &stmtCache_);
        if (!de.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
        sqlite3_bind_int64(de.stmt, 1, imageId);
        sqlite3_bind_int(de.stmt, 2, variantId);
        if (de.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }

        Stmt dh(db_,
                "DELETE FROM edit_history "
                "WHERE image_id = ?1 AND variant_id = ?2;", &stmtCache_);
        if (!dh.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
        sqlite3_bind_int64(dh.stmt, 1, imageId);
        sqlite3_bind_int(dh.stmt, 2, variantId);
        if (dh.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }

        Stmt dv(db_,
                "DELETE FROM variants WHERE image_id = ?1 AND variant_id = ?2;",
                &stmtCache_);
        if (!dv.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
        sqlite3_bind_int64(dv.stmt, 1, imageId);
        sqlite3_bind_int(dv.stmt, 2, variantId);
        if (dv.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
    }
    if (!commitTransaction()) {
        (void)rollbackTransaction();
        return false;
    }
    return true;
}

int64_t CatalogDatabase::addFolder(const std::string& folderPath,
                                   int64_t parentId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    Stmt ins(db_,
             "INSERT OR IGNORE INTO folders(path, parent_id) VALUES(?1, ?2);");
    if (!ins.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    sqlite3_bind_text(ins.stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins.stmt, 2, parentId);
    if (ins.step() != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    Stmt sel(db_, "SELECT id FROM folders WHERE path = ?1;");
    if (!sel.ok())
        return -1;
    sqlite3_bind_text(sel.stmt, 1, folderPath.c_str(), -1, SQLITE_TRANSIENT);
    if (sel.step() == SQLITE_ROW)
        return sqlite3_column_int64(sel.stmt, 0);
    return -1;
}

std::vector<std::string> CatalogDatabase::listFolders() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> out;
    if (!db_)
        return out;
    Stmt s(db_, "SELECT path FROM folders ORDER BY path;", &stmtCache_);
    if (!s.ok())
        return out;
    while (s.step() == SQLITE_ROW) {
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 0))
            out.emplace_back(reinterpret_cast<const char*>(p));
    }
    return out;
}

int64_t CatalogDatabase::ensureKeyword(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    if (path.empty())
        return -1;
    int64_t parent = 0;
    int64_t leaf = -1;
    std::istringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty())
            continue;
        Stmt sel(db_,
                 "SELECT id FROM keywords WHERE name = ?1 AND parent_id = ?2;",
                 &stmtCache_);
        if (!sel.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            return -1;
        }
        sqlite3_bind_text(sel.stmt, 1, part.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(sel.stmt, 2, parent);
        if (sel.step() == SQLITE_ROW) {
            leaf = sqlite3_column_int64(sel.stmt, 0);
        } else {
            Stmt ins(db_,
                     "INSERT INTO keywords(name, parent_id) VALUES(?1, ?2);");
            if (!ins.ok()) {
                lastError_ = sqlite3_errmsg(db_);
                return -1;
            }
            sqlite3_bind_text(ins.stmt, 1, part.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins.stmt, 2, parent);
            if (ins.step() != SQLITE_DONE) {
                lastError_ = sqlite3_errmsg(db_);
                return -1;
            }
            leaf = sqlite3_last_insert_rowid(db_);
        }
        parent = leaf;
    }
    return leaf;
}

bool CatalogDatabase::assignKeyword(int64_t imageId, int64_t keywordId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "INSERT OR IGNORE INTO photo_keywords(image_id, keyword_id) "
           "VALUES(?1, ?2);", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int64(s.stmt, 2, keywordId);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::removeKeyword(int64_t imageId, int64_t keywordId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "DELETE FROM photo_keywords WHERE image_id = ?1 AND keyword_id = ?2;",
           &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, imageId);
    sqlite3_bind_int64(s.stmt, 2, keywordId);
    return s.step() == SQLITE_DONE;
}

std::vector<int64_t> CatalogDatabase::keywordsForImage(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT keyword_id FROM photo_keywords "
           "WHERE image_id = ?1 ORDER BY keyword_id;", &stmtCache_);
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    while (s.step() == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(s.stmt, 0));
    return out;
}

std::vector<KeywordInfo> CatalogDatabase::listKeywords() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<KeywordInfo> out;
    if (!db_)
        return out;
    // id -> (parent_id, name) so full paths can be rebuilt without recursion.
    std::unordered_map<int64_t, std::pair<int64_t, std::string>> kw;
    {
        Stmt s(db_, "SELECT id, name, parent_id FROM keywords ORDER BY id;",
               &stmtCache_);
        if (!s.ok())
            return out;
        while (s.step() == SQLITE_ROW) {
            const int64_t id = sqlite3_column_int64(s.stmt, 0);
            std::string name;
            if (const unsigned char* p = sqlite3_column_text(s.stmt, 1))
                name = reinterpret_cast<const char*>(p);
            kw[id] = {sqlite3_column_int64(s.stmt, 2), std::move(name)};
        }
    }
    // photo_count per keyword, scoped to the keyword's whole subtree so it
    // matches findImagesByKeyword() (an image tagged with a descendant counts
    // toward every ancestor keyword).
    std::unordered_map<int64_t, int64_t> counts;
    {
        Stmt s(db_,
               "WITH RECURSIVE up(image_id, kw_id) AS ("
               "  SELECT pk.image_id, pk.keyword_id FROM photo_keywords pk"
               "  UNION ALL"
               "  SELECT up.image_id, k.parent_id FROM up "
               "  JOIN keywords k ON k.id = up.kw_id "
               "  WHERE k.parent_id != 0"
               ") "
               "SELECT kw_id, COUNT(DISTINCT image_id) FROM up "
               "GROUP BY kw_id;", &stmtCache_);
        if (!s.ok())
            return out;
        while (s.step() == SQLITE_ROW)
            counts[sqlite3_column_int64(s.stmt, 0)] =
                sqlite3_column_int64(s.stmt, 1);
    }
    for (const auto& [id, parentName] : kw) {
        KeywordInfo info;
        info.id = id;
        info.name = parentName.second;
        // Walk the parent chain to the root, then join segments with '/'.
        std::vector<std::string> segs;
        int64_t cur = id;
        while (cur != 0) {
            const auto it = kw.find(cur);
            if (it == kw.end())
                break;
            segs.push_back(it->second.second);
            cur = it->second.first;
        }
        std::reverse(segs.begin(), segs.end());
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i)
                info.fullPath += '/';
            info.fullPath += segs[i];
        }
        info.photoCount = counts[id];
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<int64_t> CatalogDatabase::findImagesByKeyword(
    int64_t keywordId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> out;
    if (!db_)
        return out;
    // Recursive CTE walks the subtree rooted at keywordId (self-inclusive),
    // then returns every image tagged with any descendant keyword.
    Stmt s(db_,
           "WITH RECURSIVE sub(id) AS ("
           "  SELECT ?1"
           "  UNION ALL"
           "  SELECT k.id FROM keywords k JOIN sub ON k.parent_id = sub.id"
           ") "
           "SELECT DISTINCT pk.image_id FROM photo_keywords pk "
           "JOIN sub ON sub.id = pk.keyword_id ORDER BY pk.image_id;");
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, keywordId);
    while (s.step() == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(s.stmt, 0));
    return out;
}

bool CatalogDatabase::setPhotoMetadata(int64_t imageId,
                                       const std::string& title,
                                       const std::string& caption,
                                       const std::string& copyright,
                                       const std::string& creator,
                                       const std::string& dateTaken) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "UPDATE images SET title = ?1, caption = ?2, copyright = ?3, "
           "creator = ?4, date_taken = ?5 WHERE id = ?6;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(s.stmt, 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.stmt, 2, caption.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.stmt, 3, copyright.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.stmt, 4, creator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s.stmt, 5, dateTaken.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.stmt, 6, imageId);
    if (s.step() != SQLITE_DONE)
        return false;
    return sqlite3_changes(db_) > 0;
}

PhotoMetadata CatalogDatabase::getPhotoMetadata(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    PhotoMetadata meta;
    if (!db_)
        return meta;
    Stmt s(db_,
           "SELECT title, caption, copyright, creator, date_taken FROM images "
           "WHERE id = ?1;", &stmtCache_);
    if (!s.ok())
        return meta;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    if (s.step() != SQLITE_ROW)
        return meta;
    if (const unsigned char* p = sqlite3_column_text(s.stmt, 0))
        meta.title = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(s.stmt, 1))
        meta.caption = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(s.stmt, 2))
        meta.copyright = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(s.stmt, 3))
        meta.creator = reinterpret_cast<const char*>(p);
    if (const unsigned char* p = sqlite3_column_text(s.stmt, 4))
        meta.dateTaken = reinterpret_cast<const char*>(p);
    return meta;
}

std::vector<ImageRecord> CatalogDatabase::searchImages(
    const std::string& text) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ImageRecord> out;
    if (!db_)
        return out;
    if (text.empty())
        return out;
    const std::string sql =
        std::string("SELECT ") + kImageColumns + " FROM images i "
        "WHERE i.title LIKE ?1 OR i.caption LIKE ?1 OR EXISTS ("
        "  SELECT 1 FROM photo_keywords pk JOIN keywords k ON k.id = pk.keyword_id "
        "  WHERE pk.image_id = i.id AND k.name LIKE ?1"
        ") ORDER BY i.import_date DESC, i.id DESC;";
    Stmt s(db_, sql.c_str(), &stmtCache_);
    if (!s.ok())
        return out;
    const std::string pattern = "%" + text + "%";
    sqlite3_bind_text(s.stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    while (s.step() == SQLITE_ROW)
        out.push_back(readImageRecord(s.stmt));
    return out;
}

int64_t CatalogDatabase::createCollection(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    if (name.empty())
        return -1;
    Stmt ins(db_,
             "INSERT INTO collections(name, sort_order) VALUES(?1, "
             "(SELECT COALESCE(MAX(sort_order), 0) + 1 FROM collections));");
    if (!ins.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    sqlite3_bind_text(ins.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (ins.step() != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    return sqlite3_last_insert_rowid(db_);
}

bool CatalogDatabase::renameCollection(int64_t collectionId,
                                      const std::string& newName) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (newName.empty())
        return false;
    Stmt s(db_, "UPDATE collections SET name = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(s.stmt, 1, newName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s.stmt, 2, collectionId);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::deleteCollection(int64_t collectionId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "DELETE FROM collections WHERE id = ?1;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, collectionId);
    return s.step() == SQLITE_DONE;
}

std::vector<Collection> CatalogDatabase::listCollections() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<Collection> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT id, name, sort_order FROM collections "
           "ORDER BY sort_order, id;");
    if (!s.ok())
        return out;
    while (s.step() == SQLITE_ROW) {
        Collection c;
        c.id = sqlite3_column_int64(s.stmt, 0);
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 1))
            c.name = reinterpret_cast<const char*>(p);
        c.sortOrder = sqlite3_column_int(s.stmt, 2);
        out.push_back(std::move(c));
    }
    return out;
}

bool CatalogDatabase::addToCollection(int64_t collectionId, int64_t imageId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "INSERT OR IGNORE INTO collection_images(collection_id, image_id) "
           "VALUES(?1, ?2);");
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, collectionId);
    sqlite3_bind_int64(s.stmt, 2, imageId);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::removeFromCollection(int64_t collectionId,
                                          int64_t imageId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "DELETE FROM collection_images "
           "WHERE collection_id = ?1 AND image_id = ?2;");
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, collectionId);
    sqlite3_bind_int64(s.stmt, 2, imageId);
    return s.step() == SQLITE_DONE;
}

bool CatalogDatabase::isImageInCollection(int64_t collectionId,
                                         int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "SELECT 1 FROM collection_images "
           "WHERE collection_id = ?1 AND image_id = ?2;");
    if (!s.ok())
        return false;
    sqlite3_bind_int64(s.stmt, 1, collectionId);
    sqlite3_bind_int64(s.stmt, 2, imageId);
    return s.step() == SQLITE_ROW;
}

std::vector<ImageRecord> CatalogDatabase::listImagesInCollection(
    int64_t collectionId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<ImageRecord> out;
    if (!db_)
        return out;
    // Project the columns explicitly (same order as listImages) so the shared
    // readImageRecord helper maps the row; `images.*` would expose the physical
    // schema order, which differs on catalogs migrated via ensureColumn.
    const std::string sql =
        "SELECT images.id, images.file_path, images.thumbnail_path, "
        "images.rating, images.label_color, images.flag, images.import_date, "
        "images.is_managed, images.fullres_path, images.title, images.caption, "
        "images.copyright, images.creator, images.date_taken, images.camera, "
        "images.lens FROM images "
        "JOIN collection_images ci ON ci.image_id = images.id "
        "WHERE ci.collection_id = ?1 ORDER BY images.id;";
    Stmt s(db_, sql.c_str(), &stmtCache_);
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, collectionId);
    while (s.step() == SQLITE_ROW)
        out.push_back(readImageRecord(s.stmt));
    return out;
}

std::unordered_map<int64_t, std::vector<int64_t>>
CatalogDatabase::listCollectionMemberships() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::unordered_map<int64_t, std::vector<int64_t>> out;
    if (!db_)
        return out;
    // One JOIN query reads every image -> collection pair at once, replacing
    // the N+1 loop that called listImagesInCollection() per collection. The
    // JOIN to collections drops any orphaned membership row whose collection
    // no longer exists (the FK cascade normally removes these already, so it
    // is belt-and-braces). Rows are grouped by image id below; the ORDER BY
    // keeps each image's collection list in id order.
    Stmt s(db_,
           "SELECT ci.image_id, ci.collection_id FROM collection_images ci "
           "JOIN collections c ON c.id = ci.collection_id "
           "ORDER BY ci.image_id, ci.collection_id;",
           &stmtCache_);
    if (!s.ok())
        return out;
    while (s.step() == SQLITE_ROW) {
        const int64_t imageId = sqlite3_column_int64(s.stmt, 0);
        const int64_t collectionId = sqlite3_column_int64(s.stmt, 1);
        out[imageId].push_back(collectionId);
    }
    return out;
}

namespace {

std::string normalizeMatchType(const std::string& mt) {
    return toLowerCopy(mt) == "any" ? "ANY" : "ALL";
}

// Returns `predicateJson` with its "match" field set from `matchType`
// ("ALL" -> "all", "ANY" -> "any"). Malformed JSON is returned unchanged.
std::string injectMatchMode(const std::string& predicateJson,
                            const std::string& matchType) {
    auto v = json::Value::parse(predicateJson);
    if (!v || !v->isObject())
        return predicateJson;
    const bool any = normalizeMatchType(matchType) == "ANY";
    (*v)["match"] = json::Value(any ? "any" : "all");
    return v->serialize();
}

// Rebuilds the slash-joined keyword path for one keyword id using a map of
// id -> (parent_id, name).
std::string buildKeywordPath(
    int64_t id, const std::unordered_map<int64_t, std::pair<int64_t, std::string>>& kw) {
    std::vector<std::string> segs;
    int64_t cur = id;
    while (cur != 0) {
        const auto it = kw.find(cur);
        if (it == kw.end())
            break;
        segs.push_back(it->second.second);
        cur = it->second.first;
    }
    std::reverse(segs.begin(), segs.end());
    std::string path;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i)
            path += '/';
        path += segs[i];
    }
    return path;
}

} // namespace

int64_t CatalogDatabase::createSmartCollection(const std::string& name,
                                               const std::string& predicateJson,
                                               const std::string& matchType) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;
    if (name.empty() || predicateJson.empty())
        return -1;
    if (!json::Value::parse(predicateJson))
        return -1; // reject malformed rules
    const std::string stored = injectMatchMode(predicateJson, matchType);
    Stmt ins(db_,
             "INSERT INTO smart_collections(name, predicate_json, match_type) "
             "VALUES(?1, ?2, ?3);");
    if (!ins.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    sqlite3_bind_text(ins.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.stmt, 2, stored.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins.stmt, 3, normalizeMatchType(matchType).c_str(), -1,
                      SQLITE_TRANSIENT);
    if (ins.step() != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return -1;
    }
    const int64_t id = sqlite3_last_insert_rowid(db_);
    (void)refreshSmartCollection(id); // immediate initial fill
    return id;
}

bool CatalogDatabase::updateSmartCollection(int64_t id,
                                            const std::string& name,
                                            const std::string& predicateJson,
                                            const std::string& matchType) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (name.empty() || predicateJson.empty())
        return false;
    if (!json::Value::parse(predicateJson))
        return false;
    const std::string stored = injectMatchMode(predicateJson, matchType);
    Stmt upd(db_,
             "UPDATE smart_collections SET name = ?1, predicate_json = ?2, "
             "match_type = ?3 WHERE id = ?4;", &stmtCache_);
    if (!upd.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(upd.stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upd.stmt, 2, stored.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upd.stmt, 3, normalizeMatchType(matchType).c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(upd.stmt, 4, id);
    if (upd.step() != SQLITE_DONE)
        return false;
    (void)refreshSmartCollection(id); // recompute members for the new rules
    return true;
}

bool CatalogDatabase::deleteSmartCollection(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_, "DELETE FROM smart_collections WHERE id = ?1;", &stmtCache_);
    if (!s.ok()) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(s.stmt, 1, id);
    if (s.step() != SQLITE_DONE)
        return false;
    return sqlite3_changes(db_) > 0; // members cascade via FK
}

std::vector<SmartCollection> CatalogDatabase::listSmartCollections() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<SmartCollection> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT id, name, predicate_json, match_type FROM smart_collections "
           "ORDER BY id;", &stmtCache_);
    if (!s.ok())
        return out;
    while (s.step() == SQLITE_ROW) {
        SmartCollection sc;
        sc.id = sqlite3_column_int64(s.stmt, 0);
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 1))
            sc.name = reinterpret_cast<const char*>(p);
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 2))
            sc.predicateJson = reinterpret_cast<const char*>(p);
        if (const unsigned char* p = sqlite3_column_text(s.stmt, 3))
            sc.matchType = reinterpret_cast<const char*>(p);
        out.push_back(std::move(sc));
    }
    return out;
}

int CatalogDatabase::refreshSmartCollection(int64_t id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return -1;

    std::string predText;
    {
        Stmt sel(db_,
                 "SELECT predicate_json FROM smart_collections WHERE id = ?1;",
                 &stmtCache_);
        if (!sel.ok())
            return -1;
        sqlite3_bind_int64(sel.stmt, 1, id);
        if (sel.step() != SQLITE_ROW)
            return -1;
        const unsigned char* p = sqlite3_column_text(sel.stmt, 0);
        if (p)
            predText = reinterpret_cast<const char*>(p);
    }
    auto pred = json::Value::parse(predText);
    if (!pred)
        return -1;

    const std::vector<ImageRecord> images = listImages();

    // Build imageId -> keyword full paths once for the whole scan.
    std::unordered_map<int64_t, std::pair<int64_t, std::string>> kwById;
    {
        Stmt k(db_, "SELECT id, name, parent_id FROM keywords;", &stmtCache_);
        if (!k.ok())
            return -1;
        while (k.step() == SQLITE_ROW) {
            const int64_t kid = sqlite3_column_int64(k.stmt, 0);
            std::string name;
            if (const unsigned char* p = sqlite3_column_text(k.stmt, 1))
                name = reinterpret_cast<const char*>(p);
            kwById[kid] = {sqlite3_column_int64(k.stmt, 2), std::move(name)};
        }
    }
    std::unordered_map<int64_t, std::vector<std::string>> imageKeywords;
    {
        Stmt pk(db_, "SELECT image_id, keyword_id FROM photo_keywords;",
                &stmtCache_);
        if (!pk.ok())
            return -1;
        while (pk.step() == SQLITE_ROW) {
            const int64_t imageId = sqlite3_column_int64(pk.stmt, 0);
            const int64_t keywordId = sqlite3_column_int64(pk.stmt, 1);
            imageKeywords[imageId].push_back(
                buildKeywordPath(keywordId, kwById));
        }
    }

    std::vector<int64_t> matches;
    for (const auto& img : images) {
        auto it = imageKeywords.find(img.id);
        const std::vector<std::string> kws =
            it == imageKeywords.end() ? std::vector<std::string>{} : it->second;
        if (evaluatePredicate(*pred, img, kws, hasNonDefaultEdits(img.id)))
            matches.push_back(img.id);
    }

    if (!beginTransaction())
        return -1;
    bool failed = false;
    {
        Stmt del(db_,
                 "DELETE FROM smart_collection_photos "
                 "WHERE smart_collection_id = ?1;", &stmtCache_);
        if (!del.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            failed = true;
        } else {
            sqlite3_bind_int64(del.stmt, 1, id);
            if (del.step() != SQLITE_DONE)
                failed = true;
        }
    }
    if (!failed) {
        Stmt ins(db_,
                 "INSERT OR IGNORE INTO smart_collection_photos("
                 "smart_collection_id, image_id) VALUES(?1, ?2);", &stmtCache_);
        if (!ins.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            failed = true;
        } else {
            for (const int64_t imageId : matches) {
                sqlite3_bind_int64(ins.stmt, 1, id);
                sqlite3_bind_int64(ins.stmt, 2, imageId);
                if (ins.step() != SQLITE_DONE) {
                    lastError_ = sqlite3_errmsg(db_);
                    failed = true;
                    break;
                }
            }
        }
    }
    if (failed) {
        (void)rollbackTransaction();
        return -1;
    }
    if (!commitTransaction()) {
        (void)rollbackTransaction();
        return -1;
    }
    return static_cast<int>(matches.size());
}

std::vector<int64_t> CatalogDatabase::listImagesInSmartCollection(
    int64_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT image_id FROM smart_collection_photos "
           "WHERE smart_collection_id = ?1 ORDER BY image_id;", &stmtCache_);
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, id);
    while (s.step() == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(s.stmt, 0));
    return out;
}

bool CatalogDatabase::recordPanoramaSources(
    int64_t panoramaImageId, const std::vector<int64_t>& sourceImageIds) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (!beginTransaction())
        return false;
    {
        Stmt del(db_,
                 "DELETE FROM panorama_sources "
                 "WHERE panorama_image_id = ?1;", &stmtCache_);
        if (!del.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
        sqlite3_bind_int64(del.stmt, 1, panoramaImageId);
        if (del.step() != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
    }
    {
        Stmt ins(db_,
                 "INSERT OR IGNORE INTO panorama_sources(panorama_image_id, "
                 "source_image_id) VALUES(?1, ?2);", &stmtCache_);
        if (!ins.ok()) {
            lastError_ = sqlite3_errmsg(db_);
            (void)rollbackTransaction();
            return false;
        }
        for (const int64_t sourceId : sourceImageIds) {
            sqlite3_bind_int64(ins.stmt, 1, panoramaImageId);
            sqlite3_bind_int64(ins.stmt, 2, sourceId);
            if (ins.step() != SQLITE_DONE) {
                lastError_ = sqlite3_errmsg(db_);
                (void)rollbackTransaction();
                return false;
            }
        }
    }
    if (!commitTransaction()) {
        (void)rollbackTransaction();
        return false;
    }
    return true;
}

std::vector<int64_t> CatalogDatabase::listPanoramaSources(
    int64_t panoramaImageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::vector<int64_t> out;
    if (!db_)
        return out;
    Stmt s(db_,
           "SELECT source_image_id FROM panorama_sources "
           "WHERE panorama_image_id = ?1 ORDER BY source_image_id;", &stmtCache_);
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, panoramaImageId);
    while (s.step() == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(s.stmt, 0));
    return out;
}

bool CatalogDatabase::beginTransaction() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (inTransaction_)
        return true;
    if (!exec("BEGIN IMMEDIATE;"))
        return false;
    inTransaction_ = true;
    return true;
}

bool CatalogDatabase::commitTransaction() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!inTransaction_)
        return true;
    if (exec("COMMIT;")) {
        inTransaction_ = false;
        return true;
    }
    // A failed COMMIT (disk full / file locked) leaves the transaction open on
    // the connection; roll it back so the connection returns to a clean state
    // and later writes cannot be absorbed into a dangling transaction. The
    // caller sees false and must report the failure instead of pretending the
    // commit succeeded.
    (void)exec("ROLLBACK;");
    inTransaction_ = false;
    return false;
}

bool CatalogDatabase::rollbackTransaction() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!inTransaction_)
        return true;
    const bool ok = exec("ROLLBACK;");
    inTransaction_ = false;
    return ok;
}

bool CatalogDatabase::openImportConnection() {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (importDb_)
        return true;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(path_.string().c_str(), &importDb_, flags,
                                   nullptr);
    if (rc != SQLITE_OK) {
        importError_ = importDb_ ? sqlite3_errmsg(importDb_)
                                 : "sqlite open failed";
        if (importDb_)
            sqlite3_close(importDb_);
        importDb_ = nullptr;
        return false;
    }
    // Same pragmas as the primary connection: FK cascades, WAL mode (must match
    // the file's journal mode), synchronous NORMAL and a busy timeout so a
    // transient lock held by the UI connection does not fail the import.
    if (!execOn(importDb_, "PRAGMA foreign_keys=ON;", &importError_) ||
        !execOn(importDb_, "PRAGMA journal_mode=WAL;", &importError_) ||
        !execOn(importDb_, "PRAGMA synchronous=NORMAL;", &importError_) ||
        !execOn(importDb_, "PRAGMA busy_timeout=5000;", &importError_)) {
        sqlite3_close(importDb_);
        importDb_ = nullptr;
        return false;
    }
    importInTransaction_ = false;
    return true;
}

void CatalogDatabase::closeImportConnection() {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (importDb_) {
        // Roll back a dangling transaction so the connection is clean before
        // it is released.
        if (importInTransaction_) {
            (void)execOn(importDb_, "ROLLBACK;", &importError_);
            importInTransaction_ = false;
        }
        sqlite3_close(importDb_);
        importDb_ = nullptr;
    }
}

bool CatalogDatabase::isImportConnectionOpen() const {
    std::lock_guard<std::mutex> lock(importMutex_);
    return importDb_ != nullptr;
}

std::string CatalogDatabase::importLastError() const {
    std::lock_guard<std::mutex> lock(importMutex_);
    return importError_;
}

bool CatalogDatabase::beginImportTransaction() {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (!importDb_) {
        importError_ = "import connection not open";
        return false;
    }
    if (importInTransaction_)
        return true;
    if (!execOn(importDb_, "BEGIN IMMEDIATE;", &importError_))
        return false;
    importInTransaction_ = true;
    return true;
}

bool CatalogDatabase::commitImportTransaction() {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (!importDb_) {
        importError_ = "import connection not open";
        return false;
    }
    if (!importInTransaction_)
        return true;
    if (execOn(importDb_, "COMMIT;", &importError_)) {
        importInTransaction_ = false;
        return true;
    }
    // COMMIT failed (disk full / file locked) and the transaction is still
    // open; roll it back so the import connection returns to a clean state and
    // later writes cannot be absorbed into a dangling transaction.
    (void)execOn(importDb_, "ROLLBACK;", &importError_);
    importInTransaction_ = false;
    return false;
}

bool CatalogDatabase::rollbackImportTransaction() {
    std::lock_guard<std::mutex> lock(importMutex_);
    if (!importDb_) {
        importError_ = "import connection not open";
        return false;
    }
    if (!importInTransaction_)
        return true;
    const bool ok = execOn(importDb_, "ROLLBACK;", &importError_);
    importInTransaction_ = false;
    return ok;
}

bool CatalogDatabase::backup(const std::filesystem::path& destPath) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    // Release cached prepared statements first; they hold read locks on the same
    // connection that make the WAL checkpoint below fail with SQLITE_LOCKED.
    finalizeAllCached();
    // Flush the WAL so the backup contains every committed change.
    if (sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_FULL,
                                  nullptr, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3* dest = nullptr;
    const int rc = sqlite3_open_v2(destPath.string().c_str(), &dest,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                                   nullptr);
    if (rc != SQLITE_OK) {
        lastError_ = dest ? sqlite3_errmsg(dest) : "sqlite open failed";
        if (dest)
            sqlite3_close(dest);
        return false;
    }
    sqlite3_backup* bk = sqlite3_backup_init(dest, "main", db_, "main");
    bool ok = bk != nullptr;
    if (ok) {
        (void)sqlite3_backup_step(bk, -1); // copy everything
        ok = sqlite3_backup_finish(bk) == SQLITE_OK;
    }
    if (!ok)
        lastError_ = sqlite3_errmsg(dest);
    sqlite3_close(dest);
    return ok;
}

bool CatalogDatabase::optimize() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (inTransaction_) {
        lastError_ = "cannot VACUUM inside a transaction";
        return false;
    }
    // VACUUM rebuilds the file, reclaiming space left by deletions/edits.
    if (!exec("VACUUM;"))
        return false;
    return true;
}

// --- photo_features (PHASE_14: duplicate detection / stacking) -----------

bool CatalogDatabase::storePhotoFeatures(int64_t imageId, uint64_t perceptualHash,
                                         const std::vector<float>& embedding,
                                         int modelVersion) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    Stmt s(db_,
           "INSERT INTO photo_features(image_id, phash, model_version, embedding) "
           "VALUES(?1, ?2, ?3, ?4) "
           "ON CONFLICT(image_id) DO UPDATE SET "
           "  phash=excluded.phash, "
           "  model_version=excluded.model_version, "
           "  embedding=excluded.embedding;",
           &stmtCache_);
    if (!s.ok())
        return false;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    // Store the 64-bit phash as a signed integer since SQLite's integer type
    // is signed 64-bit.  The hamming distance computation below reads it back
    // via the same bit_cast, preserving all 64 bits.
    sqlite3_bind_int64(s.stmt, 2, std::bit_cast<int64_t>(perceptualHash));
    sqlite3_bind_int(s.stmt, 3, modelVersion);
    if (!embedding.empty()) {
        sqlite3_bind_blob(s.stmt, 4, embedding.data(),
                          static_cast<int>(embedding.size() * sizeof(float)),
                          SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(s.stmt, 4);
    }
    return s.step() == SQLITE_DONE;
}

std::optional<CatalogDatabase::PhotoFeatureRow>
CatalogDatabase::getPhotoFeatures(int64_t imageId) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return std::nullopt;
    Stmt s(db_,
           "SELECT phash, model_version, embedding FROM photo_features "
           "WHERE image_id = ?1 LIMIT 1;",
           &stmtCache_);
    if (!s.ok())
        return std::nullopt;
    sqlite3_bind_int64(s.stmt, 1, imageId);
    if (s.step() != SQLITE_ROW)
        return std::nullopt;
    PhotoFeatureRow row;
    row.perceptualHash = std::bit_cast<uint64_t>(sqlite3_column_int64(s.stmt, 0));
    row.modelVersion = sqlite3_column_int(s.stmt, 1);
    const int bytes = sqlite3_column_bytes(s.stmt, 2);
    const void* blob = sqlite3_column_blob(s.stmt, 2);
    if (bytes > 0 && blob && (bytes % static_cast<int>(sizeof(float)) == 0)) {
        const int count = bytes / static_cast<int>(sizeof(float));
        row.embedding.resize(count);
        std::memcpy(row.embedding.data(), blob, bytes);
    }
    return row;
}

std::vector<CatalogDatabase::PhashCandidatePair>
CatalogDatabase::scanPhashCandidates(int maxHamming) const {
    std::vector<PhashCandidatePair> out;
    if (!db_)
        return out;
    if (maxHamming < 0)
        maxHamming = 0;
    // Materialize all (id, phash) rows then do the pairwise comparison in C++
    // — SQLite has no built-in popcount; doing the O(N^2/2) loop here lets us
    // use std::popcount on 64-bit words, which is several times faster than
    // any byte-at-a-time SQL UDF the engine would otherwise call.
    std::vector<std::pair<int64_t, uint64_t>> rows;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Stmt s(db_, "SELECT image_id, phash FROM photo_features;", &stmtCache_);
        if (!s.ok())
            return out;
        while (s.step() == SQLITE_ROW) {
            const int64_t id = sqlite3_column_int64(s.stmt, 0);
            const uint64_t h = std::bit_cast<uint64_t>(sqlite3_column_int64(s.stmt, 1));
            rows.emplace_back(id, h);
        }
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = i + 1; j < rows.size(); ++j) {
            const uint64_t diff = rows[i].second ^ rows[j].second;
            const int dist = static_cast<int>(std::popcount(diff));
            if (dist <= maxHamming) {
                out.push_back(PhashCandidatePair{rows[i].first, rows[j].first, dist});
            }
        }
    }
    return out;
}

bool CatalogDatabase::setStackId(int64_t imageId, int64_t stackId) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return false;
    if (imageId <= 0)
        return false;
    if (stackId < 0)
        stackId = 0;
    Stmt s(db_, "UPDATE images SET stack_id = ?1 WHERE id = ?2;", &stmtCache_);
    if (!s.ok())
        return false;
    sqlite3_bind_int64(s.stmt, 1, stackId);
    sqlite3_bind_int64(s.stmt, 2, imageId);
    return s.step() == SQLITE_DONE;
}

std::vector<int64_t> CatalogDatabase::getStackMembers(int64_t stackId) const {
    std::vector<int64_t> out;
    if (stackId <= 0)
        return out;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return out;
    Stmt s(db_, "SELECT id FROM images WHERE stack_id = ?1 ORDER BY id;", &stmtCache_);
    if (!s.ok())
        return out;
    sqlite3_bind_int64(s.stmt, 1, stackId);
    while (s.step() == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(s.stmt, 0));
    return out;
}

int64_t CatalogDatabase::nextStackId() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!db_)
        return 1;
    Stmt s(db_, "SELECT COALESCE(MAX(stack_id), 0) FROM images WHERE stack_id > 0;",
           &stmtCache_);
    if (!s.ok())
        return 1;
    if (s.step() != SQLITE_ROW)
        return 1;
    const int64_t v = sqlite3_column_int64(s.stmt, 0);
    return (v <= 0) ? 1 : (v + 1);
}

} // namespace lrc
