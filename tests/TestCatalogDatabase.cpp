#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>

#include <sqlite3.h>

#include "catalog/CatalogDatabase.h"
#include "catalog/FolderMonitor.h"

using namespace Catch;
using namespace lrc;

namespace {

std::filesystem::path makeTempDb() {
    static size_t counter = 0;
    const auto dir =
        std::filesystem::temp_directory_path() /
        ("lrc_catalog_test_" + std::to_string(++counter) + "_" +
         std::to_string(static_cast<unsigned long>(
             std::chrono::system_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    return dir / "test.lrcat";
}

// Runs EXPLAIN QUERY PLAN on a second connection and returns the `detail`
// column (the fourth output column) of every plan row, joined with " | ".
std::string queryPlan(sqlite3* conn, const char* sql) {
    std::string out;
    sqlite3_stmt* stmt = nullptr;
    const std::string explainSql = std::string("EXPLAIN QUERY PLAN ") + sql;
    if (sqlite3_prepare_v2(conn, explainSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* p = sqlite3_column_text(stmt, 3);
        if (p) {
            if (!out.empty())
                out += " | ";
            out += reinterpret_cast<const char*>(p);
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

} // namespace

TEST_CASE("Catalog: open creates a working database", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    REQUIRE(db.isOpen());
    const auto images = db.listImages();
    REQUIRE(images.empty());
}

TEST_CASE("Catalog: addImage is idempotent by path", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id1 = db.addImage("/photos/one.arw", "");
    const int64_t id2 = db.addImage("/photos/one.arw", "");
    REQUIRE(id1 > 0);
    REQUIRE(id2 == id1);
    REQUIRE(db.listImages().size() == 1);
}

TEST_CASE("Catalog: adjustment stack round-trips through JSON", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/edited.arw", "");

    Adjustments a;
    a.exposure = 1.25;
    a.contrast = 33;
    a.highlights = -20;
    a.saturation = 15;
    a.curve.push_back({0.1f, 0.2f});
    a.curve.push_back({0.5f, 0.7f});
    // HSL per-channel.
    a.hsl[static_cast<size_t>(ColorRange::Red)].hue = 15.0;
    a.hsl[static_cast<size_t>(ColorRange::Green)].saturation = 50.0;
    a.hsl[static_cast<size_t>(ColorRange::Blue)].luminance = -30.0;
    // Split toning.
    a.splitTone.highlightHue = 30.0;
    a.splitTone.highlightSaturation = 40.0;
    a.splitTone.shadowHue = 240.0;
    a.splitTone.shadowSaturation = 60.0;
    a.splitTone.balance = -20.0;

    REQUIRE(db.saveAdjustments(id, a));
    auto loaded = db.loadAdjustments(id);
    REQUIRE(loaded.has_value());
    REQUIRE(*loaded == a);
}

TEST_CASE("Catalog: missing edits return nullopt", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    REQUIRE_FALSE(db.loadAdjustments(999).has_value());
}

TEST_CASE("Catalog: transactions commit and roll back", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());

    REQUIRE(db.beginTransaction());
    for (int i = 0; i < 3; ++i)
        REQUIRE(db.addImage("/photos/batch_" + std::to_string(i) + ".arw", "") > 0);
    REQUIRE(db.commitTransaction());
    REQUIRE(db.listImages().size() == 3);

    REQUIRE(db.beginTransaction());
    REQUIRE(db.addImage("/photos/rollback.arw", "") > 0);
    REQUIRE(db.rollbackTransaction());
    REQUIRE(db.listImages().size() == 3);
}

TEST_CASE("Catalog: rating and label persist", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/rated.arw", "");
    REQUIRE(db.setRating(id, 4));
    REQUIRE(db.setLabelColor(id, 2));

    const auto rec = db.findImageByPath("/photos/rated.arw");
    REQUIRE(rec.has_value());
    REQUIRE(rec->rating == 4);
    REQUIRE(rec->labelColor == 2);
}

TEST_CASE("Catalog: folders are unique", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    REQUIRE(db.addFolder("/photos/a") > 0);
    REQUIRE(db.addFolder("/photos/a") > 0); // same folder -> same id
    const auto folders = db.listFolders();
    REQUIRE(folders.size() == 1);
    REQUIRE(folders[0] == "/photos/a");
}

TEST_CASE("Catalog: flag persists and is listed", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/picked.arw", "");
    REQUIRE(db.setFlag(id, ImageFlag::FlagPicked));
    REQUIRE(db.setFlag(id, -1)); // reject
    REQUIRE(db.setFlag(id, 99) == false); // invalid flag rejected

    const auto rec = db.findImageByPath("/photos/picked.arw");
    REQUIRE(rec.has_value());
    REQUIRE(rec->flag == ImageFlag::FlagRejected);
}

TEST_CASE("Catalog: filtered listing by rating and label", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id1 = db.addImage("/photos/a.arw", "");
    const int64_t id2 = db.addImage("/photos/b.arw", "");
    const int64_t id3 = db.addImage("/photos/c.dng", "");
    REQUIRE(db.setRating(id1, 5));
    REQUIRE(db.setRating(id2, 2));
    REQUIRE(db.setLabelColor(id1, 3));

    ImageFilter f;
    f.minRating = 4;
    const auto top = db.listImages(f);
    REQUIRE(top.size() == 1);
    REQUIRE(top[0].id == id1);

    ImageFilter lab;
    lab.labelColor = 3;
    REQUIRE(db.listImages(lab).size() == 1);

    ImageFilter ext;
    ext.fileExtension = ".dng";
    const auto dngs = db.listImages(ext);
    REQUIRE(dngs.size() == 1);
    REQUIRE(dngs[0].id == id3);
}

TEST_CASE("Catalog: filtered listing excludes rejected by default", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t keep = db.addImage("/photos/keep.arw", "");
    const int64_t drop = db.addImage("/photos/drop.arw", "");
    REQUIRE(db.setFlag(drop, ImageFlag::FlagRejected));

    ImageFilter f;
    REQUIRE(db.listImages(f).size() == 1);
    REQUIRE(db.listImages(f)[0].id == keep);

    f.includeRejected = true;
    REQUIRE(db.listImages(f).size() == 2);
}

TEST_CASE("Catalog: rejected filter works with includeRejected", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t rejected = db.addImage("/photos/rejected.arw", "");
    const int64_t picked = db.addImage("/photos/picked.arw", "");
    REQUIRE(db.setFlag(rejected, ImageFlag::FlagRejected));
    REQUIRE(db.setFlag(picked, ImageFlag::FlagPicked));

    // (a) The default filter excludes rejected photos.
    const auto all = db.listImages(ImageFilter{});
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].id == picked);

    // (b) flag == -1 with includeRejected returns the rejected photo (this is
    // the combination the UI "rejected" filter must produce; without
    // includeRejected the SQL becomes flag=-1 AND flag!=-1 and is always empty).
    ImageFilter rej;
    rej.flag = ImageFlag::FlagRejected;
    rej.includeRejected = true;
    const auto rejectedOnly = db.listImages(rej);
    REQUIRE(rejectedOnly.size() == 1);
    REQUIRE(rejectedOnly[0].id == rejected);

    // (c) flag == 0 (all flags) with includeRejected returns both photos.
    ImageFilter any;
    any.includeRejected = true;
    const auto withRejected = db.listImages(any);
    REQUIRE(withRejected.size() == 2);
}

TEST_CASE("Catalog: settings round-trip", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    REQUIRE(db.setSetting("library_root", "C:/Library"));
    REQUIRE(db.getSetting("library_root").value_or("") == "C:/Library");
    // Overwrite updates in place.
    REQUIRE(db.setSetting("library_root", "C:/Library2"));
    REQUIRE(db.getSetting("library_root").value_or("") == "C:/Library2");
    REQUIRE_FALSE(db.getSetting("missing").has_value());
}

TEST_CASE("Catalog: collections are created and listed in order", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());

    const int64_t c1 = db.createCollection("Landscapes");
    const int64_t c2 = db.createCollection("Portraits");
    REQUIRE(c1 > 0);
    REQUIRE(c2 > 0);
    REQUIRE(c1 != c2);

    const auto collections = db.listCollections();
    REQUIRE(collections.size() == 2);
    REQUIRE(collections[0].id == c1);
    REQUIRE(collections[0].name == "Landscapes");
    REQUIRE(collections[1].id == c2);
    REQUIRE(collections[1].name == "Portraits");
    // sort_order is assigned in creation order.
    REQUIRE(collections[0].sortOrder < collections[1].sortOrder);
}

TEST_CASE("Catalog: createCollection rejects an empty name", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    REQUIRE(db.createCollection("") == -1);
    REQUIRE(db.listCollections().empty());
}

TEST_CASE("Catalog: renameCollection renames and rejects empty names",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t c = db.createCollection("Old");
    REQUIRE(db.renameCollection(c, "New"));
    REQUIRE(db.listCollections().size() == 1);
    REQUIRE(db.listCollections()[0].name == "New");
    // Empty new name is rejected and the old name is kept.
    REQUIRE_FALSE(db.renameCollection(c, ""));
    REQUIRE(db.listCollections()[0].name == "New");
}

TEST_CASE("Catalog: addToCollection is idempotent", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t c = db.createCollection("Favorites");
    const int64_t img = db.addImage("/photos/fav.arw", "");

    REQUIRE(db.addToCollection(c, img));
    REQUIRE(db.addToCollection(c, img)); // second add is a no-op
    REQUIRE(db.isImageInCollection(c, img));
    REQUIRE_FALSE(db.isImageInCollection(c, img + 1000));

    const auto members = db.listImagesInCollection(c);
    REQUIRE(members.size() == 1);
    REQUIRE(members[0].id == img);
}

TEST_CASE("Catalog: removeFromCollection drops membership", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t c = db.createCollection("Favorites");
    const int64_t img = db.addImage("/photos/rm.arw", "");
    REQUIRE(db.addToCollection(c, img));

    REQUIRE(db.removeFromCollection(c, img));
    REQUIRE_FALSE(db.isImageInCollection(c, img));
    REQUIRE(db.listImagesInCollection(c).empty());
}

TEST_CASE("Catalog: deleteCollection cascades to members", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t c = db.createCollection("Temp");
    const int64_t img1 = db.addImage("/photos/one.arw", "");
    const int64_t img2 = db.addImage("/photos/two.arw", "");
    REQUIRE(db.addToCollection(c, img1));
    REQUIRE(db.addToCollection(c, img2));
    REQUIRE(db.listImagesInCollection(c).size() == 2);

    REQUIRE(db.deleteCollection(c));
    REQUIRE(db.listCollections().empty());
    // Membership rows were removed by ON DELETE CASCADE...
    REQUIRE(db.listImagesInCollection(c).empty());
    // ...but the images themselves survive.
    REQUIRE(db.listImages().size() == 2);
}

TEST_CASE("Catalog: removing an image cascades out of collections",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t c = db.createCollection("Favorites");
    const int64_t img = db.addImage("/photos/gone.arw", "");
    REQUIRE(db.addToCollection(c, img));
    REQUIRE(db.isImageInCollection(c, img));

    REQUIRE(db.removeImage(img));
    REQUIRE_FALSE(db.isImageInCollection(c, img));
    REQUIRE(db.listImagesInCollection(c).empty());
}

TEST_CASE("Catalog: listImagesInCollection returns full image records",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t c = db.createCollection("Favorites");
    const int64_t img = db.addImage("/photos/rec.arw", "/thumbs/rec.jpg");
    REQUIRE(db.setRating(img, 4));
    REQUIRE(db.setLabelColor(img, 5));
    REQUIRE(db.addToCollection(c, img));

    const auto members = db.listImagesInCollection(c);
    REQUIRE(members.size() == 1);
    REQUIRE(members[0].id == img);
    REQUIRE(members[0].filePath == "/photos/rec.arw");
    REQUIRE(members[0].thumbnailPath == "/thumbs/rec.jpg");
    REQUIRE(members[0].rating == 4);
    REQUIRE(members[0].labelColor == 5);
}

TEST_CASE("Catalog: listCollectionMemberships matches N+1 aggregation",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());

    // Four images across three collections with overlapping memberships; img4
    // belongs to no collection and must be absent from the map.
    const int64_t img1 = db.addImage("/photos/m1.arw", "");
    const int64_t img2 = db.addImage("/photos/m2.arw", "");
    const int64_t img3 = db.addImage("/photos/m3.arw", "");
    const int64_t img4 = db.addImage("/photos/m4.arw", "");
    const int64_t c1 = db.createCollection("Alpha");
    const int64_t c2 = db.createCollection("Beta");
    const int64_t c3 = db.createCollection("Gamma");
    REQUIRE(db.addToCollection(c1, img1));
    REQUIRE(db.addToCollection(c1, img2));
    REQUIRE(db.addToCollection(c2, img2));
    REQUIRE(db.addToCollection(c2, img3));
    REQUIRE(db.addToCollection(c3, img1));
    REQUIRE(db.addToCollection(c3, img3));

    // Hand-built expectation: img1 in {c1,c3}, img2 in {c1,c2}, img3 in
    // {c2,c3}, img4 in none.
    std::unordered_map<int64_t, std::vector<int64_t>> expected;
    expected[img1] = {c1, c3};
    expected[img2] = {c1, c2};
    expected[img3] = {c2, c3};

    const auto memberships = db.listCollectionMemberships();
    REQUIRE(memberships.size() == 3);
    REQUIRE(memberships.find(img4) == memberships.end());

    // (a) Equivalent to the N+1 aggregation of listImagesInCollection.
    std::unordered_map<int64_t, std::vector<int64_t>> aggregated;
    for (const Collection& c : db.listCollections())
        for (const ImageRecord& rec : db.listImagesInCollection(c.id))
            aggregated[rec.id].push_back(c.id);
    REQUIRE(aggregated.size() == memberships.size());
    for (const auto& [imageId, collectionIds] : memberships) {
        const auto it = aggregated.find(imageId);
        REQUIRE(it != aggregated.end());
        // Collection lists may come back in any order; compare as sets.
        const std::set<int64_t> got(collectionIds.begin(), collectionIds.end());
        const std::set<int64_t> want(it->second.begin(), it->second.end());
        REQUIRE(got == want);
    }

    // (b) Matches the hand-built expectation.
    for (const auto& [imageId, collectionIds] : expected) {
        const auto it = memberships.find(imageId);
        REQUIRE(it != memberships.end());
        const std::set<int64_t> got(it->second.begin(), it->second.end());
        const std::set<int64_t> want(collectionIds.begin(), collectionIds.end());
        REQUIRE(got == want);
    }

    // (c) The single query is cached and reused (M2-6): a second call returns
    // the same memberships without growing the statement cache.
    const size_t cacheBefore = db.stmtCacheSize();
    const auto again = db.listCollectionMemberships();
    REQUIRE(again.size() == memberships.size());
    REQUIRE(db.stmtCacheSize() == cacheBefore);
}

TEST_CASE("Catalog: filtered listing combines minRating and label",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id1 = db.addImage("/photos/a.arw", "");
    const int64_t id2 = db.addImage("/photos/b.arw", "");
    const int64_t id3 = db.addImage("/photos/c.arw", "");
    REQUIRE(db.setRating(id1, 5));
    REQUIRE(db.setLabelColor(id1, 3));
    REQUIRE(db.setRating(id2, 2));
    REQUIRE(db.setLabelColor(id2, 3));
    REQUIRE(db.setRating(id3, 5));
    REQUIRE(db.setLabelColor(id3, 0));

    ImageFilter f;
    f.minRating = 4;
    f.labelColor = 3;
    const auto matched = db.listImages(f);
    REQUIRE(matched.size() == 1);
    REQUIRE(matched[0].id == id1);
}

TEST_CASE("Catalog: fullres path persists and round-trips", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/fullres.arw", "");
    REQUIRE(db.setFullResPath(id, "/tmp/fr.jpg"));

    const auto rec = db.findImageByPath("/photos/fullres.arw");
    REQUIRE(rec.has_value());
    REQUIRE(rec->fullresPath == "/tmp/fr.jpg");

    const auto all = db.listImages();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].fullresPath == "/tmp/fr.jpg");
}

TEST_CASE("Catalog: managed flag persists and round-trips", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/managed/shot.arw", "", true);
    REQUIRE(db.findImageByPath("/managed/shot.arw")->isManaged == true);
    // Idempotent re-import promotes an "added" row to managed.
    const int64_t addId = db.addImage("/added/keep.arw", "", false);
    REQUIRE(db.findImageByPath("/added/keep.arw")->isManaged == false);
    const int64_t sameId = db.addImage("/added/keep.arw", "", true);
    REQUIRE(sameId == addId);
    REQUIRE(db.findImageByPath("/added/keep.arw")->isManaged == true);
    REQUIRE(db.setManaged(id, false));
    REQUIRE(db.findImageByPath("/managed/shot.arw")->isManaged == false);
    (void)id;
}

TEST_CASE("Catalog: saveAdjustments records a history snapshot", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/hist.arw", "");

    Adjustments a1;
    a1.exposure = 1.0;
    Adjustments a2;
    a2.exposure = 2.0;
    REQUIRE(db.saveAdjustments(id, a1));
    REQUIRE(db.saveAdjustments(id, a2));

    const auto history = db.listEditHistory(id);
    REQUIRE(history.size() == 2);
    REQUIRE(db.loadAdjustments(id)->exposure == Approx(2.0));

    // Oldest snapshot is the first one saved.
    REQUIRE(db.loadEditSnapshot(id, history[0].id)->exposure == Approx(1.0));
    REQUIRE(db.loadEditSnapshot(id, history[1].id)->exposure == Approx(2.0));
    REQUIRE_FALSE(db.loadEditSnapshot(id, 9999).has_value());

    REQUIRE(db.deleteEditSnapshot(id, history[0].id));
    REQUIRE(db.listEditHistory(id).size() == 1);
    // Deleting an image cascades to its snapshots.
    REQUIRE(db.removeImage(id));
    REQUIRE(db.listEditHistory(id).empty());
}

TEST_CASE("Catalog: backup produces a readable copy", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    db.addImage("/photos/backup.arw", "");
    Adjustments a;
    a.exposure = 1.5;
    db.saveAdjustments(db.findImageByPath("/photos/backup.arw")->id, a);

    const auto backupPath = path.string() + ".bak";
    REQUIRE(db.backup(backupPath));

    CatalogDatabase copy(backupPath);
    REQUIRE(copy.open());
    const auto rec = copy.findImageByPath("/photos/backup.arw");
    REQUIRE(rec.has_value());
    auto loaded = copy.loadAdjustments(rec->id);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->exposure == Approx(1.5));
}

TEST_CASE("Catalog: optimize reclaims deleted rows", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/delete_me.arw", "");
    REQUIRE(db.removeImage(id));
    REQUIRE(db.optimize());
    // Database still works after VACUUM.
    REQUIRE(db.addImage("/photos/after.arw", "") > 0);
    REQUIRE(db.listImages().size() == 1);
}

TEST_CASE("Catalog: embedded previews round-trip through the catalog",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/preview.arw", "");

    // (a) Thumbnail preview stores and loads byte-identically.
    const std::vector<uint8_t> thumb{0xFF, 0xD8, 0xFF, 0xE0, 0x10, 0x20};
    REQUIRE(db.storePreview(id, PreviewKind::Thumbnail, thumb, 160, 120));
    auto loadedThumb = db.loadPreview(id, PreviewKind::Thumbnail);
    REQUIRE(loadedThumb.has_value());
    REQUIRE(*loadedThumb == thumb);
    auto thumbSize = db.previewSize(id, PreviewKind::Thumbnail);
    REQUIRE(thumbSize.has_value());
    REQUIRE(thumbSize->first == 160);
    REQUIRE(thumbSize->second == 120);

    // (b) Full-res preview stores and loads byte-identically.
    const std::vector<uint8_t> full{0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08};
    REQUIRE(db.storePreview(id, PreviewKind::FullRes, full, 1920, 1080));
    auto loadedFull = db.loadPreview(id, PreviewKind::FullRes);
    REQUIRE(loadedFull.has_value());
    REQUIRE(*loadedFull == full);
    REQUIRE(db.listImages().size() == 1); // no extra image rows created

    // (c) Re-storing the same (imageId, kind) overwrites (upsert).
    const std::vector<uint8_t> thumb2{0x89, 0x50, 0x4E, 0x47};
    REQUIRE(db.storePreview(id, PreviewKind::Thumbnail, thumb2, 64, 64));
    auto overwritten = db.loadPreview(id, PreviewKind::Thumbnail);
    REQUIRE(overwritten.has_value());
    REQUIRE(*overwritten == thumb2);
    auto overwrittenSize = db.previewSize(id, PreviewKind::Thumbnail);
    REQUIRE(overwrittenSize.has_value());
    REQUIRE(overwrittenSize->first == 64);
    REQUIRE(overwrittenSize->second == 64);

    // (d) An unstored (imageId, kind) reports nullopt / no size.
    REQUIRE_FALSE(db.loadPreview(id + 1000, PreviewKind::Thumbnail).has_value());
    REQUIRE_FALSE(db.previewSize(id + 1000, PreviewKind::FullRes).has_value());

    // (e) previewSize reports (0,0) when width/height were not recorded.
    const std::vector<uint8_t> noSize{0x01, 0x02, 0x03};
    REQUIRE(db.storePreview(id, PreviewKind::FullRes, noSize));
    auto zeroSize = db.previewSize(id, PreviewKind::FullRes);
    REQUIRE(zeroSize.has_value());
    REQUIRE(zeroSize->first == 0);
    REQUIRE(zeroSize->second == 0);

    // (f) Removing the image cascades: its previews are deleted too.
    REQUIRE(db.removeImage(id));
    REQUIRE_FALSE(db.loadPreview(id, PreviewKind::Thumbnail).has_value());
    REQUIRE_FALSE(db.loadPreview(id, PreviewKind::FullRes).has_value());
    REQUIRE_FALSE(db.previewSize(id, PreviewKind::Thumbnail).has_value());
}

TEST_CASE("Catalog: listImages uses the new query-plan indexes", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());

    for (int i = 0; i < 50; ++i)
        REQUIRE(db.addImage("/photos/idx_" + std::to_string(i) + ".arw", "") > 0);

    // The indexes are committed schema objects, so a second connection to the
    // same file sees them and can be asked for the query plan.
    sqlite3* conn = nullptr;
    REQUIRE(sqlite3_open_v2(path.string().c_str(), &conn,
                            SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    REQUIRE(conn != nullptr);

    // Default listImages sort: import_date DESC. Must walk the new index rather
    // than sort the whole table with a temp b-tree.
    const std::string orderPlan = queryPlan(
        conn, "SELECT id FROM images ORDER BY import_date DESC;");
    WARN("DIAG orderPlan=[" << orderPlan << "]");
    {
        sqlite3_stmt* e = nullptr;
        const int pr = sqlite3_prepare_v2(conn, "EXPLAIN QUERY PLAN SELECT id FROM images ORDER BY import_date DESC;", -1, &e, nullptr);
        const int ncol = e ? sqlite3_column_count(e) : -1;
        WARN("DIAG eprep=" << pr << " ncol=" << ncol << " ROW=" << SQLITE_ROW << " DONE=" << SQLITE_DONE);
        if (e) {
            int st = sqlite3_step(e);
            int row = 0;
            while (st == SQLITE_ROW) {
                for (int i = 0; i < ncol; ++i) {
                    const unsigned char* v = sqlite3_column_text(e, i);
                    WARN("DIAG row" << row << "col" << i << "=[" << (v ? reinterpret_cast<const char*>(v) : "NULL") << "]");
                }
                ++row;
                st = sqlite3_step(e);
            }
            WARN("DIAG estep_end=" << st);
            sqlite3_finalize(e);
        }
        WARN("DIAG db_rows=" << db.listImages().size());
    }
    REQUIRE(orderPlan.find("idx_images_import_date") != std::string::npos);
    REQUIRE(orderPlan.find("USE TEMP B-TREE") == std::string::npos);

    // Each filter predicate resolves to a SEARCH on its dedicated index. The
    // plan may read "USING INDEX" or "USING COVERING INDEX" (the id is the
    // rowid, so the index alone covers the projection), so assert the index
    // name plus the SEARCH keyword rather than the exact phrasing.
    const std::string ratingPlan = queryPlan(
        conn, "SELECT id FROM images WHERE rating >= ?1;");
    REQUIRE(ratingPlan.find("idx_images_rating") != std::string::npos);
    REQUIRE(ratingPlan.find("SEARCH") != std::string::npos);

    const std::string flagPlan = queryPlan(
        conn, "SELECT id FROM images WHERE flag = ?1;");
    REQUIRE(flagPlan.find("idx_images_flag") != std::string::npos);
    REQUIRE(flagPlan.find("SEARCH") != std::string::npos);

    const std::string labelPlan = queryPlan(
        conn, "SELECT id FROM images WHERE label_color = ?1;");
    REQUIRE(labelPlan.find("idx_images_label_color") != std::string::npos);
    REQUIRE(labelPlan.find("SEARCH") != std::string::npos);

    sqlite3_close(conn);
}

TEST_CASE("Catalog: cached statements stay correct under heavy reuse",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/cached.arw", "");

    // 200 save/load cycles with a distinct adjustment value each time. The
    // upsert and select statements are cached and reused; a stale binding or a
    // missing reset would surface as a wrong value here.
    for (int i = 0; i < 200; ++i) {
        Adjustments a;
        a.exposure = static_cast<double>(i) * 0.1;
        a.contrast = i;
        REQUIRE(db.saveAdjustments(id, a));
        const auto loaded = db.loadAdjustments(id);
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->exposure == Approx(a.exposure));
        REQUIRE(loaded->contrast == a.contrast);
    }

    // Populate rows with varied rating / label_color / flag so the dynamic-SQL
    // listImages cache entries (different WHERE bind counts) are exercised.
    for (int i = 0; i < 20; ++i)
        REQUIRE(db.addImage("/photos/f_" + std::to_string(i) + ".arw", "") > 0);
    for (int i = 0; i < 20; ++i) {
        const int64_t imgId =
            db.addImage("/photos/attr_" + std::to_string(i) + ".arw", "");
        REQUIRE(imgId > 0);
        REQUIRE(db.setRating(imgId, (i % 5) + 1)); // rating 1..5
        REQUIRE(db.setLabelColor(imgId, i % 3));   // label 0..2
        if (i % 4 == 0)
            REQUIRE(db.setFlag(imgId, ImageFlag::FlagPicked));
    }

    // 3-bind filter first, then a 1-bind filter. If bindings leaked across the
    // reused statements the 1-bind query would inherit the 3-bind filter's
    // flag/label constraints and return far fewer rows.
    ImageFilter three;
    three.minRating = 2;
    three.flag = ImageFlag::FlagPicked;
    three.labelColor = 1;
    const auto withThree = db.listImages(three);
    REQUIRE(withThree.size() == 2); // i=4 (rating 5) and i=16 (rating 2)

    ImageFilter one;
    one.minRating = 2;
    REQUIRE(db.listImages(one).size() == 16); // 20 rows, 4 with rating 1

    // Re-running the 1-bind filter must reuse the cached statement: the cache
    // size does not grow (no re-prepare) and the result stays identical.
    const size_t before = db.stmtCacheSize();
    REQUIRE(db.listImages(one).size() == 16);
    REQUIRE(db.stmtCacheSize() == before);

    // Re-running the 3-bind filter after the 1-bind one gives the same rows.
    const auto threeAgain = db.listImages(three);
    REQUIRE(threeAgain.size() == 2);

    // The cache is actually in effect: the statements prepared during the 200
    // save/load cycles and the filtered listings are held (a small, bounded
    // set), not leaked one-per-call.
    REQUIRE(db.stmtCacheSize() > 0);
    REQUIRE(db.stmtCacheSize() < 20);
}

TEST_CASE("Catalog: virtual copies get independent adjustments", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/vc.arw", "");

    Adjustments master;
    master.exposure = 0.5;
    REQUIRE(db.saveAdjustments(id, master));

    const int v1 = db.createVirtualCopy(id);
    const int v2 = db.createVirtualCopy(id);
    REQUIRE(v1 > 0);
    REQUIRE(v2 > v1);

    // listVariants shows master + two copies, master flagged first.
    const auto variants = db.listVariants(id);
    REQUIRE(variants.size() == 3);
    REQUIRE(variants[0].variantId == 0);
    REQUIRE(variants[0].isMaster);
    REQUIRE(variants[1].variantId == v1);
    REQUIRE_FALSE(variants[1].isMaster);
    REQUIRE(variants[2].variantId == v2);
    REQUIRE(variants[2].name == "\xe5\x89\xaf\xe6\x9c\xac 2"); // 副本 2

    // A new copy starts from the master's current parameter stack.
    REQUIRE(db.loadAdjustments(id, v1)->exposure == Approx(0.5));
    REQUIRE(db.loadAdjustments(id, v2)->exposure == Approx(0.5));

    // Each copy then edits independently.
    Adjustments a1;
    a1.exposure = 1.0;
    a1.contrast = 10;
    REQUIRE(db.saveAdjustments(id, a1, v1));
    Adjustments a2;
    a2.exposure = 2.0;
    a2.saturation = 20;
    REQUIRE(db.saveAdjustments(id, a2, v2));

    // The master is untouched; each variant round-trips its own JSON.
    REQUIRE(db.loadAdjustments(id)->exposure == Approx(0.5));
    REQUIRE(db.loadAdjustments(id, v1)->exposure == Approx(1.0));
    REQUIRE(db.loadAdjustments(id, v1)->contrast == 10);
    REQUIRE(db.loadAdjustments(id, v2)->exposure == Approx(2.0));
    REQUIRE(db.loadAdjustments(id, v2)->saturation == 20);
    REQUIRE_FALSE(db.loadAdjustments(id, 99).has_value());
}

TEST_CASE("Catalog: deleting a virtual copy leaves others intact", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/vc_del.arw", "");
    const int v1 = db.createVirtualCopy(id);
    const int v2 = db.createVirtualCopy(id);

    Adjustments a1;
    a1.exposure = 1.0;
    Adjustments a2;
    a2.exposure = 2.0;
    REQUIRE(db.saveAdjustments(id, a1, v1));
    REQUIRE(db.saveAdjustments(id, a2, v2));

    REQUIRE(db.deleteVariant(id, v1));
    REQUIRE_FALSE(db.loadAdjustments(id, v1).has_value());
    REQUIRE(db.loadAdjustments(id, v2)->exposure == Approx(2.0));
    REQUIRE(db.listVariants(id).size() == 2);

    // PHASE_6: deleting the master is refused while copies exist.
    REQUIRE_FALSE(db.deleteVariant(id, 0));
    REQUIRE(db.listVariants(id).size() == 2);

    REQUIRE(db.deleteVariant(id, v2));
    REQUIRE(db.listVariants(id).size() == 1);
    // With no copies left the master may be deleted (clears its edits/history).
    REQUIRE(db.deleteVariant(id, 0));
    REQUIRE(db.listVariants(id).empty());
}

TEST_CASE("Catalog: edit history is variant-scoped", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/vc_hist.arw", "");
    const int v1 = db.createVirtualCopy(id);

    Adjustments a1;
    a1.exposure = 1.0;
    Adjustments a2;
    a2.exposure = 2.0;
    REQUIRE(db.saveAdjustments(id, a1));      // master history
    REQUIRE(db.saveAdjustments(id, a2, v1));  // copy history

    const auto masterHist = db.listEditHistory(id);
    REQUIRE(masterHist.size() == 1);
    const auto copyHist = db.listEditHistory(id, v1);
    REQUIRE(copyHist.size() == 1);

    // Snapshots are isolated per variant.
    REQUIRE(db.loadEditSnapshot(id, masterHist[0].id)->exposure == Approx(1.0));
    REQUIRE(db.loadEditSnapshot(id, copyHist[0].id, v1)->exposure == Approx(2.0));
    REQUIRE_FALSE(db.loadEditSnapshot(id, copyHist[0].id, 0).has_value());

    REQUIRE(db.deleteEditSnapshot(id, copyHist[0].id, v1));
    REQUIRE(db.listEditHistory(id, v1).empty());
    REQUIRE(db.listEditHistory(id, 0).size() == 1);
}

TEST_CASE("Catalog: named snapshots survive createVirtualCopy", "[catalog]") {
    // R-13 regression: createVirtualCopy() must copy the snapshot `name`
    // column. Before the fix only image_id/adjustment_json/created_at were
    // copied, so named snapshots silently became unlabelled history entries.
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/named_snap.arw", "");

    Adjustments a;
    a.exposure = 0.75;
    // Master has one *named* snapshot plus one unlabelled auto-history entry.
    const int64_t namedId = db.saveEditSnapshot(id, a, 0, "Golden Hour");
    const bool savedAuto = db.saveAdjustments(id, a);
    REQUIRE(namedId > 0);
    REQUIRE(savedAuto);

    const int v1 = db.createVirtualCopy(id);

    const auto copyHist = db.listEditHistory(id, v1);
    REQUIRE(copyHist.size() == 2);
    // The named master snapshot must carry its label into the copy.
    bool foundNamed = false;
    for (const auto& s : copyHist) {
        if (s.name == "Golden Hour")
            foundNamed = true;
    }
    REQUIRE(foundNamed);
}

TEST_CASE("Catalog: virtual copies survive a reopen", "[catalog]") {
    const auto path = makeTempDb();
    {
        CatalogDatabase db(path);
        REQUIRE(db.open());
        const int64_t id = db.addImage("/photos/restart.arw", "");
        const int v1 = db.createVirtualCopy(id);
        Adjustments a;
        a.exposure = 1.75;
        REQUIRE(db.saveAdjustments(id, a, v1));
    }
    // Rebuild the database object on the same file (simulated app restart).
    CatalogDatabase db2(path);
    REQUIRE(db2.open());
    const auto rec = db2.findImageByPath("/photos/restart.arw");
    REQUIRE(rec.has_value());
    const auto variants = db2.listVariants(rec->id);
    REQUIRE(variants.size() == 2);
    REQUIRE(variants[0].variantId == 0);
    REQUIRE(variants[0].isMaster);
    REQUIRE(variants[1].variantId == 1);
    REQUIRE_FALSE(variants[1].isMaster);
    const auto loaded = db2.loadAdjustments(rec->id, variants[1].variantId);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->exposure == Approx(1.75));
}

TEST_CASE("Catalog: keyword hierarchy creation is idempotent", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());

    const int64_t k1 = db.ensureKeyword("风光/冰岛/瀑布");
    REQUIRE(k1 > 0);
    const int64_t k2 = db.ensureKeyword("风光/冰岛/瀑布");
    REQUIRE(k2 == k1); // same branch reused
    const int64_t k3 = db.ensureKeyword("风光/冰岛"); // non-leaf of the branch
    REQUIRE(k3 > 0);
    REQUIRE(k3 != k1);

    const auto keywords = db.listKeywords();
    REQUIRE(keywords.size() == 3);
    for (const auto& kw : keywords) {
        if (kw.id == k1) {
            REQUIRE(kw.name == "瀑布");
            REQUIRE(kw.fullPath == "风光/冰岛/瀑布");
        } else if (kw.id == k3) {
            REQUIRE(kw.fullPath == "风光/冰岛");
        }
    }
}

TEST_CASE("Catalog: keyword assign/remove is idempotent", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/kw.arw", "");
    const int64_t kw = db.ensureKeyword("风光/冰岛");

    REQUIRE(db.assignKeyword(id, kw));
    REQUIRE(db.assignKeyword(id, kw)); // second assign is a no-op
    REQUIRE(db.keywordsForImage(id).size() == 1);
    REQUIRE(db.keywordsForImage(id)[0] == kw);

    REQUIRE(db.removeKeyword(id, kw));
    REQUIRE(db.keywordsForImage(id).empty());
}

TEST_CASE("Catalog: findImagesByKeyword includes descendants", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t img1 = db.addImage("/photos/kw1.arw", "");
    const int64_t img2 = db.addImage("/photos/kw2.arw", "");
    const int64_t kwA = db.ensureKeyword("风光");
    const int64_t kwB = db.ensureKeyword("风光/冰岛");
    const int64_t kwC = db.ensureKeyword("风光/冰岛/瀑布");

    REQUIRE(db.assignKeyword(img1, kwC)); // deep keyword
    REQUIRE(db.assignKeyword(img2, kwA)); // top keyword

    // A keyword query includes images tagged with any descendant.
    const auto viaTop = db.findImagesByKeyword(kwA);
    REQUIRE(viaTop.size() == 2);
    const auto viaMid = db.findImagesByKeyword(kwB);
    REQUIRE(viaMid.size() == 1);
    REQUIRE(viaMid[0] == img1);
    const auto viaLeaf = db.findImagesByKeyword(kwC);
    REQUIRE(viaLeaf.size() == 1);
    REQUIRE(viaLeaf[0] == img1);
}

TEST_CASE("Catalog: keyword list aggregates photo counts", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t img1 = db.addImage("/photos/c1.arw", "");
    const int64_t img2 = db.addImage("/photos/c2.arw", "");
    const int64_t kwA = db.ensureKeyword("风光");
    const int64_t kwB = db.ensureKeyword("风光/冰岛");
    REQUIRE(db.assignKeyword(img1, kwB));
    REQUIRE(db.assignKeyword(img2, kwA));

    std::unordered_map<int64_t, int64_t> counts;
    for (const auto& kw : db.listKeywords())
        counts[kw.id] = kw.photoCount;
    REQUIRE(counts[kwA] == 2); // both images via the subtree
    REQUIRE(counts[kwB] == 1);
}

TEST_CASE("Catalog: photo metadata round-trips", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/meta.arw", "");
    REQUIRE(db.setPhotoMetadata(id, "Alps Sunrise", "A misty morning",
                                "ASE-128", "Photographer", "2026-06-15"));

    const auto meta = db.getPhotoMetadata(id);
    REQUIRE(meta.title == "Alps Sunrise");
    REQUIRE(meta.caption == "A misty morning");
    REQUIRE(meta.copyright == "ASE-128");
    REQUIRE(meta.creator == "Photographer");
    REQUIRE(meta.dateTaken == "2026-06-15");

    // The fields also surface on the image record.
    const auto rec = db.findImageByPath("/photos/meta.arw");
    REQUIRE(rec.has_value());
    REQUIRE(rec->title == "Alps Sunrise");
    REQUIRE(rec->dateTaken == "2026-06-15");
    const auto all = db.listImages();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].caption == "A misty morning");
}

TEST_CASE("Catalog: searchImages matches title, caption and keyword", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t img1 = db.addImage("/photos/s1.arw", "");
    const int64_t img2 = db.addImage("/photos/s2.arw", "");
    const int64_t img3 = db.addImage("/photos/s3.arw", "");
    REQUIRE(db.setPhotoMetadata(img1, "Alps", "night sky", "", "", ""));
    const int64_t kw = db.ensureKeyword("风光/冰岛");
    REQUIRE(db.assignKeyword(img2, kw));

    REQUIRE(db.searchImages("Alps").size() == 1);
    REQUIRE(db.searchImages("Alps")[0].id == img1);
    REQUIRE(db.searchImages("night").size() == 1);
    REQUIRE(db.searchImages("night")[0].id == img1);
    REQUIRE(db.searchImages("冰岛").size() == 1);
    REQUIRE(db.searchImages("冰岛")[0].id == img2);
    REQUIRE(db.searchImages("no-such-text").empty());
    REQUIRE(db.searchImages("").empty());
}

TEST_CASE("Catalog: predicate evaluation (rating, ALL/ANY, keyword, dates)",
          "[catalog]") {
    // Standalone evaluator -- no database involved.
    auto pred = json::Value::parse(
        R"({"match":"all","rules":[{"field":"rating","op":">=","value":4}]})");
    REQUIRE(pred.has_value());
    ImageRecord img;
    img.rating = 5;
    REQUIRE(evaluatePredicate(*pred, img, {}, false));
    img.rating = 3;
    REQUIRE_FALSE(evaluatePredicate(*pred, img, {}, false));

    // ALL: rating>=4 AND keyword contains 风光.
    auto allPred = json::Value::parse(
        R"({"match":"all","rules":[{"field":"rating","op":">=","value":4},{"field":"keyword","op":"contains","value":"风光"}]})");
    REQUIRE(allPred.has_value());
    ImageRecord img2;
    img2.rating = 5;
    REQUIRE(evaluatePredicate(*allPred, img2, {"风光/冰岛/瀑布"}, false));
    REQUIRE_FALSE(evaluatePredicate(*allPred, img2, {"人像"}, false));
    REQUIRE_FALSE(evaluatePredicate(*allPred, img2, {}, false));

    // ANY: rating>=4 OR keyword contains 风光.
    auto anyPred = json::Value::parse(
        R"({"match":"any","rules":[{"field":"rating","op":">=","value":4},{"field":"keyword","op":"contains","value":"风光"}]})");
    REQUIRE(anyPred.has_value());
    ImageRecord img3;
    img3.rating = 1;
    REQUIRE(evaluatePredicate(*anyPred, img3, {"风光"}, false));
    REQUIRE_FALSE(evaluatePredicate(*anyPred, img3, {"人像"}, false));

    // between on ISO date strings.
    auto datePred = json::Value::parse(
        R"({"match":"all","rules":[{"field":"date_taken","op":"between","value":["2026-01-01","2026-12-31"]}]})");
    REQUIRE(datePred.has_value());
    ImageRecord img4;
    img4.dateTaken = "2026-06-15";
    REQUIRE(evaluatePredicate(*datePred, img4, {}, false));
    img4.dateTaken = "2025-12-31";
    REQUIRE_FALSE(evaluatePredicate(*datePred, img4, {}, false));

    // label compared by colour name (tolerant conversion).
    auto labelPred = json::Value::parse(
        R"({"match":"all","rules":[{"field":"label","op":"==","value":"red"}]})");
    REQUIRE(labelPred.has_value());
    ImageRecord img5;
    img5.labelColor = 1;
    REQUIRE(evaluatePredicate(*labelPred, img5, {}, false));
    img5.labelColor = 2;
    REQUIRE_FALSE(evaluatePredicate(*labelPred, img5, {}, false));

    // has_adjustments boolean field.
    auto adjPred = json::Value::parse(
        R"({"match":"all","rules":[{"field":"has_adjustments","op":"==","value":true}]})");
    REQUIRE(adjPred.has_value());
    REQUIRE(evaluatePredicate(*adjPred, img5, {}, true));
    REQUIRE_FALSE(evaluatePredicate(*adjPred, img5, {}, false));

    // file_type from the path extension.
    auto typePred = json::Value::parse(
        R"({"match":"all","rules":[{"field":"file_type","op":"==","value":"arw"}]})");
    REQUIRE(typePred.has_value());
    ImageRecord img6;
    img6.filePath = "/photos/x.ARW";
    REQUIRE(evaluatePredicate(*typePred, img6, {}, false));
}

TEST_CASE("Catalog: smart collection refresh tracks rating changes", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t img1 = db.addImage("/photos/sc1.arw", "");
    const int64_t img2 = db.addImage("/photos/sc2.arw", "");
    const int64_t img3 = db.addImage("/photos/sc3.arw", "");
    REQUIRE(db.setRating(img1, 5));
    REQUIRE(db.setRating(img2, 3));
    REQUIRE(db.setRating(img3, 4));

    const int64_t sc = db.createSmartCollection(
        "four stars",
        R"({"rules":[{"field":"rating","op":">=","value":4}],"match":"all"})",
        "ALL");
    REQUIRE(sc > 0);

    // createSmartCollection fills the member table immediately.
    auto members = db.listImagesInSmartCollection(sc);
    REQUIRE(members.size() == 2); // img1 + img3

    // Promote img2; a refresh recomputes the membership.
    REQUIRE(db.setRating(img2, 5));
    REQUIRE(db.refreshSmartCollection(sc) == 3);
    members = db.listImagesInSmartCollection(sc);
    REQUIRE(members.size() == 3);

    // Demote img3 below the threshold and drop one member.
    REQUIRE(db.setRating(img3, 2));
    REQUIRE(db.refreshSmartCollection(sc) == 2);
    members = db.listImagesInSmartCollection(sc);
    REQUIRE(members.size() == 2);
}

TEST_CASE("Catalog: smart collection rule change recomputes members",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t img1 = db.addImage("/photos/sca.arw", "");
    const int64_t img2 = db.addImage("/photos/scb.arw", "");
    REQUIRE(db.setRating(img1, 2));
    REQUIRE(db.setRating(img2, 5));

    const int64_t sc = db.createSmartCollection(
        "high", R"({"rules":[{"field":"rating","op":">=","value":4}]})", "ALL");
    REQUIRE(sc > 0);
    REQUIRE(db.listImagesInSmartCollection(sc) == std::vector<int64_t>({img2}));

    // Loosening the rule recomputes the members automatically.
    REQUIRE(db.updateSmartCollection(
        sc, "two plus", R"({"rules":[{"field":"rating","op":">=","value":2}]})",
        "ALL"));
    REQUIRE(db.listImagesInSmartCollection(sc).size() == 2);

    const auto collections = db.listSmartCollections();
    REQUIRE(collections.size() == 1);
    REQUIRE(collections[0].id == sc);
    REQUIRE(collections[0].name == "two plus");
    REQUIRE(collections[0].matchType == "ALL");

    REQUIRE(db.deleteSmartCollection(sc));
    REQUIRE(db.listSmartCollections().empty());
    REQUIRE(db.listImagesInSmartCollection(sc).empty());
}

TEST_CASE("Catalog: panorama sources record, replace and list", "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t pano = db.addImage("/photos/pano.dng", "");
    const int64_t s1 = db.addImage("/photos/s1.arw", "");
    const int64_t s2 = db.addImage("/photos/s2.arw", "");
    const int64_t s3 = db.addImage("/photos/s3.arw", "");

    REQUIRE(db.recordPanoramaSources(pano, {s1, s2}));
    REQUIRE(db.listPanoramaSources(pano) == std::vector<int64_t>({s1, s2}));

    // Re-recording the same set is a no-op (idempotent).
    REQUIRE(db.recordPanoramaSources(pano, {s1, s2}));
    REQUIRE(db.listPanoramaSources(pano).size() == 2);

    // Recording a different set replaces the previous sources.
    REQUIRE(db.recordPanoramaSources(pano, {s2, s3}));
    REQUIRE(db.listPanoramaSources(pano) == std::vector<int64_t>({s2, s3}));

    // Unrelated panoramas are not affected.
    REQUIRE(db.listPanoramaSources(s1).empty());
}

TEST_CASE("Catalog: file fingerprint tracking detects external modification",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/fp.arw", "", true, 1000, 2000);
    REQUIRE(id > 0);

    // A fingerprint equal to the stored one is not "modified".
    REQUIRE(db.detectModifiedImages(1000, 2000).empty());

    // A differing size or mtime marks the file externally modified.
    const auto sizeChanged = db.detectModifiedImages(9999, 2000);
    REQUIRE(std::find(sizeChanged.begin(), sizeChanged.end(), id) !=
            sizeChanged.end());
    const auto mtimeChanged = db.detectModifiedImages(1000, 9999);
    REQUIRE(std::find(mtimeChanged.begin(), mtimeChanged.end(), id) !=
            mtimeChanged.end());

    // setFingerprint refreshes the stored value; the new pair is no longer dirty.
    REQUIRE(db.setFingerprint(id, 5000, 6000));
    REQUIRE(db.detectModifiedImages(5000, 6000).empty());
    REQUIRE(!db.setFingerprint(424242, 1, 1));  // absent image -> false
}

TEST_CASE("Catalog: duplicate phash and embedding ANN index (goal 6)",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t a = db.addImage("/photos/a.arw", "");
    const int64_t b = db.addImage("/photos/b.arw", "");
    const int64_t c = db.addImage("/photos/c.arw", "");

    // a and b share a perceptual hash; c differs.
    const uint64_t phashShared = 0xABCDABCDABCDABCDULL;
    const uint64_t phashOther = 0x123456789ABCDEF0ULL;
    std::vector<float> va(64, 1.0f);
    std::vector<float> vb(va);        // near, but distinct from a -> small, nonzero distance
    vb[0] = 0.5f;
    std::vector<float> vc(64, 0.0f);
    REQUIRE(db.storePhotoFeatures(a, phashShared, va));
    REQUIRE(db.storePhotoFeatures(b, phashShared, vb));
    REQUIRE(db.storePhotoFeatures(c, phashOther, vc));

    // phash segment screen: the shared-hash pair is within hamming 0.
    const auto pairs = db.scanPhashCandidates(0);
    REQUIRE(std::any_of(pairs.begin(), pairs.end(), [&](const auto& p) {
        return (p.aId == a && p.bId == b) || (p.aId == b && p.bId == a);
    }));

    // Rebuild the derived (FTS / phash-seg / vec0) indexes, then ANN query the
    // vec0 virtual table: a's vector is an exact match and must come back first.
    const bool rebuilt = db.rebuildSearchIndexes();
    INFO("rebuildSearchIndexes lastError: " << db.lastError());
    REQUIRE(rebuilt);
    const auto nearest = db.annByEmbedding(va, 5);
    REQUIRE(!nearest.empty());
    REQUIRE(nearest.front() == a);
}

TEST_CASE("Catalog: automatic edit history prunes to 20, snapshots survive",
          "[catalog]") {
    const auto path = makeTempDb();
    CatalogDatabase db(path);
    REQUIRE(db.open());
    const int64_t id = db.addImage("/photos/history.arw", "");
    const Adjustments adj;

    // 25 automatic saves; only the newest 20 may survive pruning.
    for (int i = 0; i < 25; ++i)
        REQUIRE(db.saveEditSnapshot(id, adj) > 0);
    auto hist = db.listEditHistory(id);
    REQUIRE(hist.size() == 20);

    // Named snapshots are exempt from automatic pruning.
    REQUIRE(db.saveEditSnapshot(id, adj, 0, "keep flagship") > 0);
    hist = db.listEditHistory(id);
    REQUIRE(hist.size() == 21);
    REQUIRE(std::any_of(hist.begin(), hist.end(), [](const EditSnapshot& s) {
        return s.name == "keep flagship";
    }));

    // Additional automatic saves still prune only the auto kind.
    for (int i = 0; i < 5; ++i)
        REQUIRE(db.saveEditSnapshot(id, adj) > 0);
    hist = db.listEditHistory(id);
    REQUIRE(hist.size() == 21);
    REQUIRE(std::any_of(hist.begin(), hist.end(), [](const EditSnapshot& s) {
        return s.name == "keep flagship";
    }));
}

TEST_CASE("FolderMonitor: isRawFile accepts every Sony RAW extension",
          "[catalog]") {
    for (const std::string& ext : lrc::FolderMonitor::rawExtensions())
        REQUIRE(lrc::FolderMonitor::isRawFile(
            std::filesystem::path("IMG_0001" + ext)));
    // Case-insensitive (Windows): uppercase variant must match too.
    REQUIRE(lrc::FolderMonitor::isRawFile(
        std::filesystem::path("IMG_0001.ARW")));
    REQUIRE(lrc::FolderMonitor::isRawFile(
        std::filesystem::path("IMG_0001.SR3")));
    REQUIRE_FALSE(
        lrc::FolderMonitor::isRawFile(std::filesystem::path("photo.jpg")));
    REQUIRE_FALSE(
        lrc::FolderMonitor::isRawFile(std::filesystem::path("photo.tiff")));
}

TEST_CASE("FolderMonitor: isImportableFile accepts the export formats",
          "[catalog]") {
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.jpg")));
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.jpeg")));
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.PNG")));
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.tif")));
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.tiff")));
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.dng")));
    REQUIRE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("photo.arw")));
    REQUIRE_FALSE(lrc::FolderMonitor::isImportableFile(
        std::filesystem::path("notes.txt")));
}

TEST_CASE("FolderMonitor: poll reports add/modify and defers removals",
          "[catalog]") {
    // Unique temp folder so parallel test runs never collide.
    const auto dir = std::filesystem::temp_directory_path() /
                     ("lrc_fmon_" + std::to_string(static_cast<unsigned long>(
                         std::chrono::system_clock::now()
                             .time_since_epoch()
                             .count())));
    std::filesystem::create_directories(dir);
    const auto fileA = dir / "a.jpg";
    const auto fileB = dir / "b.jpg";

    std::vector<std::filesystem::path> added, modified, removed;
    lrc::FolderMonitor fmon(dir);
    fmon.setCallback([&](const auto& ad, const auto& md, const auto& rm) {
        added = ad;
        modified = md;
        removed = rm;
    });

    // Baseline: only a.jpg present.
    {
        std::ofstream f(fileA, std::ios::binary);
        f.write("short", 5);
    }
    fmon.start();

    // Add b.jpg -> reported as added.
    {
        std::ofstream f(fileB, std::ios::binary);
        f.write("another", 7);
    }
    fmon.poll();
    REQUIRE(added.size() == 1);
    REQUIRE(added[0] == fileB);
    REQUIRE(modified.empty());
    REQUIRE(removed.empty());

    // Overwrite a.jpg with a different length -> modified (R-14).
    added.clear();
    {
        std::ofstream f(fileA, std::ios::binary);
        f.write("much longer content!!", 21);
    }
    fmon.poll();
    REQUIRE(modified.size() == 1);
    REQUIRE(modified[0] == fileA);
    REQUIRE(added.empty());
    REQUIRE(removed.empty());

    // Delete b.jpg -> first missing poll only starts the offline counter (R-15).
    added.clear();
    modified.clear();
    std::filesystem::remove(fileB);
    fmon.poll();
    REQUIRE(removed.empty());
    // Second consecutive missing poll -> real removal.
    fmon.poll();
    REQUIRE(removed.size() == 1);
    REQUIRE(removed[0] == fileB);

    // Folder becomes unavailable -> nothing is removed even across polls (R-15).
    removed.clear();
    std::error_code ec;
    // Offline target must be a *sibling*; a directory cannot be renamed into
    // its own subtree.
    const auto renamed = dir.string() + "_offline";
    std::filesystem::rename(dir, renamed, ec);  // move the watched dir away
    REQUIRE_FALSE(ec);
    fmon.poll();
    fmon.poll();
    REQUIRE(removed.empty());
    // Restore the folder and confirm a.jpg is still tracked, never removed.
    ec.clear();
    std::filesystem::rename(renamed, dir, ec);
    REQUIRE_FALSE(ec);
    fmon.poll();
    REQUIRE(removed.empty());

    ec.clear();
    std::filesystem::remove_all(dir, ec);
}
