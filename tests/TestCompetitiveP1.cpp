// P1 — competitive-alignment implementation tests.
//
// Covers the acceptance checks from docs/spec/competitive-optimization-
// implementation-spec.md §2:
//   • P1-1 DirectML: execution-provider API + graceful fallbacks.
//   • P1-2 ICC export: profile-byte generation + embedding in JPEG/PNG/TIFF.
//   • P1-3 batch rename: template rendering + catalog file_path sync.
//   • P1-4 watermark: deterministic alpha-composite over the U8 output.
//   • P1-5 snapshots: name-labelled edit-history records + apply/rename/delete.
//
// All checks are headless (engine + catalog, no Qt).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "catalog/CatalogDatabase.h"
#include "engine/AiInference.h"
#include "engine/ColorManager.h"
#include "engine/ExportEngine.h"
#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "utils/RenameTemplate.h"

using namespace lrc;

namespace {
std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}
bool contains(const std::vector<uint8_t>& hay, const std::string& needle) {
    if (needle.empty() || hay.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::equal(needle.begin(), needle.end(), hay.begin() +
                                                       static_cast<std::ptrdiff_t>(i)))
            return true;
    return false;
}
}

// ---------------------------------------------------------------------------
// P1-3 — rename template rendering
// ---------------------------------------------------------------------------
TEST_CASE("P1-3: rename template renders date/index/base tokens", "[p1][rename]") {
    RenameTemplateArgs a;
    a.tmpl = "{date}_{base}_{index:3}";
    a.base = "IMG_001";
    a.index = 7;
    a.indexWidth = 2;
    a.date = "2026-09-05";
    REQUIRE(renderRenameTemplate(a) == "2026-09-05_IMG_001_007");
}

TEST_CASE("P1-3: uniqueRenamePath resolves collisions before the extension",
          "[p1][rename]") {
    const auto tmp = std::filesystem::temp_directory_path() / "lrc_p1_rename_unique";
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);

    // Occupy the bare rendered name so the helper must fall back to -N.
    {
        std::ofstream(tmp / "Shot.jpg", std::ios::binary) << "x";
    }
    RenameTemplateArgs a;
    a.tmpl = "Shot";
    a.base = "IMG_001";
    a.index = 1;
    a.indexWidth = 2;

    // First call sees "Shot.jpg" taken -> "Shot-2.jpg".
    const auto p1 = lrc::uniqueRenamePath(tmp, a, ".jpg");
    REQUIRE(p1 == tmp / "Shot-2.jpg");
    // Claim it, then the next call must step to "-3".
    {
        std::ofstream(p1, std::ios::binary) << "x";
    }
    const auto p2 = lrc::uniqueRenamePath(tmp, a, ".jpg");
    REQUIRE(p2 == tmp / "Shot-3.jpg");

    std::filesystem::remove_all(tmp, ec);
}

TEST_CASE("P1-3: single-file-update path stays unique and rejects collisions",
          "[p1][rename][catalog]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto dbPath = tmp / "lrc_test_p1_rename.db";
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);

    {
        CatalogDatabase db(dbPath);
        REQUIRE(db.open());
        const int64_t a = db.addImage("C:/photos/IMG_0001.dng", "");
        const int64_t b = db.addImage("C:/photos/IMG_0002.dng", "");
        REQUIRE(a > 0);
        REQUIRE(b > 0);

        // Collision with another image's path must be refused.
        REQUIRE_FALSE(db.updateImagePath(a, "C:/photos/IMG_0002.dng"));
        // A fresh unique path must succeed and be queryable.
        REQUIRE(db.updateImagePath(a, "C:/photos/renamed_0001.dng"));
        const auto rec = db.findImageByPath("C:/photos/renamed_0001.dng");
        REQUIRE(rec.has_value());
        REQUIRE(rec->id == a);
        // The other image's path is untouched.
        REQUIRE(db.findImageByPath("C:/photos/IMG_0002.dng").has_value());
    }
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath.string() + "-wal", ec);
    std::filesystem::remove(dbPath.string() + "-shm", ec);
}

// ---------------------------------------------------------------------------
// P1-3 — batch-rename end-to-end integration (replicates ImportWorker Stage 1)
// ---------------------------------------------------------------------------
//
// These two cases reproduce, at the headless catalog layer, the exact sequence
// ImportWorker::importPaths performs after files have landed: render the rename
// template per selection-order index, uniquify the target inside the staging
// directory, move the file on disk, then persist the final path. The first
// case proves the batch contract from §2 (unique on-disk names + DB file_path
// sync, numeric suffix on collisions); the second proves the cancel contract
// (a rolled-back batch leaves no half-committed renamed path in the DB).

TEST_CASE("P1-3: batch rename yields unique on-disk names and synced DB",
          "[p1][rename][catalog]") {
    const auto tmp =
        std::filesystem::temp_directory_path() / "lrc_p1_batch_rename";
    const auto staging = tmp / "staging";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(staging, ec);

    // Files already "landed" in the catalog staging dir (the post-copy/move
    // state ImportWorker::importPaths renames from). Distinct source bases so
    // any collision below comes only from the occupied render target.
    const std::filesystem::path landed[] = {
        staging / "IMG_0001.dng", staging / "IMG_0002.dng",
        staging / "IMG_0003.dng"};
    for (const auto& f : landed)
        { std::ofstream(f, std::ios::binary) << "dummy"; }

    // Pre-occupy the render target of selection-order slot 1 (template
    // "2026_{index:2}" renders "2026_01" for index 1), forcing the worker to
    // step to a "-2" suffix and exercising the numeric-collision path.
    const auto occupied = staging / "2026_01.dng";
    { std::ofstream(occupied, std::ios::binary) << "x"; }

    CatalogDatabase db(tmp / "catalog.lrcat");
    REQUIRE(db.open());

    const lrc::RenameTemplateArgs base{"2026_{index:2}", "", 1, 2,
                                       "2026-09-05"};
    std::vector<std::filesystem::path> finalPaths;
    std::vector<int64_t> ids;
    int seq = 0;
    for (const auto& cur : landed) {
        const lrc::RenameTemplateArgs ra{base.tmpl, cur.stem().string(),
                                         ++seq, base.indexWidth, base.date};
        // Unique-target resolution against the live staging dir, then move.
        const std::filesystem::path newPath = lrc::uniqueRenamePath(
            cur.parent_path(), ra, cur.extension().string());
        if (newPath != cur) {
            std::filesystem::rename(cur, newPath, ec);
            REQUIRE_FALSE(ec);
        }
        finalPaths.push_back(newPath);
        // Persist the renamed path exactly as ImportWorker does.
        const int64_t id = db.addImage(newPath.string(), "");
        REQUIRE(id > 0);
        ids.push_back(id);
    }

    // All on-disk regular filenames in staging are unique after the batch.
    std::set<std::string> names;
    for (std::filesystem::directory_iterator it(staging), end;
         it != end; ++it)
        if (it->is_regular_file())
            names.insert(it->path().filename().string());
    REQUIRE(names.size() >= 4);  // 3 DNGs + the pre-occupied marker
    std::sort(finalPaths.begin(), finalPaths.end());
    for (const auto& p : finalPaths)
        REQUIRE(names.count(p.filename().string()) == 1);

    // Collision was resolved: slot 1 landed on "2026_01-2.dng"; slots 2-3 on
    // the unoccupied renders.
    REQUIRE(names.count("2026_01-2.dng") == 1);
    REQUIRE(names.count("2026_02.dng") == 1);
    REQUIRE(names.count("2026_03.dng") == 1);

    // Each DB file_path mirrors the renamed on-disk path (lookup by final path
    // resolves back to the inserted id — the catalog-level sync guarantee).
    for (size_t i = 0; i < finalPaths.size(); ++i) {
        const auto rec = db.findImageByPath(finalPaths[i].string());
        REQUIRE(rec.has_value());
        REQUIRE(rec->id == ids[i]);
    }

    std::filesystem::remove_all(tmp, ec);
}

TEST_CASE("P1-3: cancelled import rolls back renamed paths from the DB",
          "[p1][rename][catalog]") {
    const auto tmp =
        std::filesystem::temp_directory_path() / "lrc_p1_batch_cancel";
    const auto staging = tmp / "staging";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(staging, ec);

    const std::filesystem::path landed[] = {
        staging / "SRC_0001.dng", staging / "SRC_0002.dng"};
    for (const auto& f : landed)
        { std::ofstream(f, std::ios::binary) << "dummy"; }

    CatalogDatabase db(tmp / "catalog.lrcat");
    REQUIRE(db.open());
    // ImportWorker wraps the whole import in a transaction; cancel rolls it
    // back. Here the renames happen inside the transaction, then the batch is
    // aborted exactly like ImportWorker::importPaths does on abort_.
    REQUIRE(db.beginTransaction());
    int seq = 0;
    for (const auto& cur : landed) {
        const lrc::RenameTemplateArgs ra{"2026_{index:2}", cur.stem().string(),
                                         ++seq, 2, "2026-09-05"};
        const std::filesystem::path newPath = lrc::uniqueRenamePath(
            cur.parent_path(), ra, cur.extension().string());
        if (newPath != cur) {
            std::filesystem::rename(cur, newPath, ec);
            REQUIRE_FALSE(ec);
        }
        REQUIRE(db.addImage(newPath.string(), "") > 0);
    }
    // Cancel: nothing committed. No renamed or original path may survive.
    REQUIRE(db.rollbackTransaction());
    REQUIRE(db.listImages().empty());

    std::filesystem::remove_all(tmp, ec);
}

// ---------------------------------------------------------------------------
// P1-5 — named snapshots
// ---------------------------------------------------------------------------
TEST_CASE("P1-5: named snapshot create/apply/rename/delete round-trips",
          "[p1][snapshot][catalog]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto dbPath = tmp / "lrc_test_p1_snapshot.db";
    std::error_code ec;
    for (const std::string& tail : {"", "-wal", "-shm"})
        std::filesystem::remove(dbPath.string() + tail, ec);

    {
        CatalogDatabase db(dbPath);
        REQUIRE(db.open());
        const int64_t id = db.addImage("C:/photos/IMG_0001.dng", "");
        REQUIRE(id > 0);

        Adjustments adj;
        adj.exposure = 1.2;
        adj.saturation = -15.0;

        const int64_t snapId =
            db.saveEditSnapshot(id, adj, /*variant=*/0, "Golden");
        REQUIRE(snapId > 0);

        // list includes the name; the automatic (unlabelled) history stays ''.
        auto hist = db.listEditHistory(id);
        REQUIRE(hist.size() == 1);
        REQUIRE(hist[0].name == "Golden");

        // Apply restores the exact field values.
        auto applied = db.loadEditSnapshot(id, snapId);
        REQUIRE(applied.has_value());
        REQUIRE(applied->exposure == 1.2);
        REQUIRE(applied->saturation == -15.0);

        // Rename the snapshot label.
        REQUIRE(db.renameSnapshot(id, snapId, "Favorites"));
        auto renamed = db.listEditHistory(id);
        REQUIRE(renamed[0].name == "Favorites");

        // Delete removes the record.
        REQUIRE(db.deleteEditSnapshot(id, snapId));
        REQUIRE(db.listEditHistory(id).empty());
    }
    for (const std::string& tail : {"", "-wal", "-shm"})
        std::filesystem::remove(dbPath.string() + tail, ec);
}

TEST_CASE("P1-5: snapshot migration is idempotent across re-open", "[p1][snapshot]") {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto dbPath = tmp / "lrc_test_p1_snapshot_mig.db";
    std::error_code ec;
    for (const std::string& tail : {"", "-wal", "-shm"})
        std::filesystem::remove(dbPath.string() + tail, ec);

    {
        CatalogDatabase db(dbPath);
        REQUIRE(db.open());
        const int64_t id = db.addImage("C:/photos/x.dng", "");
        Adjustments adj;
        adj.contrast = 5.0;
        db.saveEditSnapshot(id, adj, 0, "A");
    }
    {  // second open must not re-add the column / duplicate anything
        CatalogDatabase db(dbPath);
        REQUIRE(db.open());
        const auto imgs = db.listImages();
        REQUIRE(imgs.size() == 1);
        const auto hist = db.listEditHistory(imgs[0].id);
        REQUIRE(hist.size() == 1);
        REQUIRE(hist[0].name == "A");
    }
    for (const std::string& tail : {"", "-wal", "-shm"})
        std::filesystem::remove(dbPath.string() + tail, ec);
}

// ---------------------------------------------------------------------------
// P1-1 — DirectML execution-provider API
// ---------------------------------------------------------------------------
TEST_CASE("P1-1: provider API defaults to CPU and never throws", "[p1][ai]") {
    AiInferenceEngine engine;
    REQUIRE(engine.preferredProvider() ==
            AiInferenceEngine::ExecutionProvider::Cpu);
    // CPU is always available; DirectML query is safe (false in stub builds).
    REQUIRE(AiInferenceEngine::isProviderAvailable(
        AiInferenceEngine::ExecutionProvider::Cpu));
    // Report the runtime DirectML capability so the P1-1 acceptance record knows
    // whether the "CPU vs DirectML ≥2x" leg is even measurable on this machine.
    // A true here is a prerequisite for running the 410 MB NAFNet comparison.
    INFO("DirectML available at runtime: "
         << AiInferenceEngine::isProviderAvailable(
                AiInferenceEngine::ExecutionProvider::DirectML)
         << " ; real ONNX backend: "
         << AiInferenceEngine::hasRealBackend());

    engine.setPreferredProvider(
        AiInferenceEngine::ExecutionProvider::DirectML);
    REQUIRE(engine.preferredProvider() ==
            AiInferenceEngine::ExecutionProvider::DirectML);
    engine.setPreferredProvider(AiInferenceEngine::ExecutionProvider::Cpu);
    REQUIRE(engine.preferredProvider() ==
            AiInferenceEngine::ExecutionProvider::Cpu);
}

TEST_CASE("P1-1: builtin model still runs with DirectML preferred", "[p1][ai]") {
    AiInferenceEngine engine;
    engine.setPreferredProvider(
        AiInferenceEngine::ExecutionProvider::DirectML);
    ModelInfo info;
    info.id = "builtin_p1";
    info.onnxPath = "builtin://add_2x2";
    REQUIRE(engine.loadModel(info));
    const std::vector<float> in = {0.0f, 1.0f, 2.0f};
    const auto out = engine.run("builtin_p1", in, {1, 1, 3});
    REQUIRE(out.size() == in.size());
    for (size_t i = 0; i < in.size(); ++i)
        REQUIRE(out[i] == in[i] + 0.5f);
}

// ---------------------------------------------------------------------------
// P1-2 — ICC profile bytes + embedding
// ---------------------------------------------------------------------------
TEST_CASE("P1-2: exportProfileBytes produces non-empty valid presets", "[p1][icc]") {
    for (const auto p : {ColorManager::ExportColorProfile::SRGB,
                         ColorManager::ExportColorProfile::AdobeRGB,
                         ColorManager::ExportColorProfile::DisplayP3}) {
        const auto bytes = ColorManager::exportProfileBytes(p);
        REQUIRE_FALSE(bytes.empty());
        // ICC profiles embed the "acsp" magic at offset 36.
        REQUIRE(bytes.size() > 36);
        REQUIRE(contains(bytes, "acsp"));
    }
}

TEST_CASE("P1-2: ICC is embedded in JPEG / PNG / TIFF (and absent by default)",
          "[p1][icc]") {
    auto u8 = ImageBuffer::create(16, 16, PixelFormat::U8);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            for (int c = 0; c < 3; ++c)
                u8->setPixel(x, y, c, 0.5f);
    const auto icc =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::SRGB);
    REQUIRE_FALSE(icc.empty());
    const auto tmp = std::filesystem::temp_directory_path();

    // Default (no ICC) — must NOT contain the JPEG ICC signature.
    const auto jpgPlain = tmp / "lrc_p1_nocc.jpg";
    REQUIRE(ImageIO::writeJpeg(jpgPlain, *u8, 90));
    REQUIRE_FALSE(contains(readFile(jpgPlain), std::string("ICC_PROFILE", 11)));

    // JPEG with ICC — APP2 ICC_PROFILE markers present.
    const auto jpgIcc = tmp / "lrc_p1_icc.jpg";
    REQUIRE(ImageIO::writeJpeg(jpgIcc, *u8, 90, icc));
    REQUIRE(contains(readFile(jpgIcc), std::string("ICC_PROFILE", 11)));

    // PNG with ICC — iCCP chunk present, absent without.
    const auto pngIcc = tmp / "lrc_p1_icc.png";
    REQUIRE(ImageIO::writePng(pngIcc, *u8, icc));
    REQUIRE(contains(readFile(pngIcc), "iCCP"));
    const auto pngPlain = tmp / "lrc_p1_nocc.png";
    REQUIRE(ImageIO::writePng(pngPlain, *u8));
    REQUIRE_FALSE(contains(readFile(pngPlain), "iCCP"));

    // TIFF with ICC — the profile bytes appear after the strip pixel data.
    const auto tifIcc = tmp / "lrc_p1_icc.tif";
    REQUIRE(ImageIO::writeTiff(tifIcc, *u8, icc));
    const auto tiffBytes = readFile(tifIcc);
    REQUIRE(tiffBytes.size() >= icc.size());
    // Tail of the file should hold the profile verbatim.
    const auto tail = std::vector<uint8_t>(
        tiffBytes.end() - static_cast<std::ptrdiff_t>(icc.size()),
        tiffBytes.end());
    REQUIRE(tail == icc);

    std::error_code e;
    for (const auto& p :
         {jpgPlain, jpgIcc, pngIcc, pngPlain, tifIcc})
        std::filesystem::remove(p, e);
}

// ---------------------------------------------------------------------------
// P1-4 — watermark compositing
// ---------------------------------------------------------------------------
TEST_CASE("P1-4: watermark composits over the display output after render",
          "[p1][watermark][export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);

    // All-white linear image (display output will be pure white U8).
    auto linear = ImageBuffer::create(64, 64, PixelFormat::F32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            for (int c = 0; c < 3; ++c)
                linear->setPixel(x, y, c, 1.0f);

    // 8x8 opaque red overlay, centred, half opacity.
    ExportEngine::ExportSpec spec;
    spec.format = ExportEngine::ExportSpec::Format::Png;
    spec.watermark.enabled = true;
    spec.watermark.overlayW = spec.watermark.overlayH = 8;
    spec.watermark.rgba.resize(8u * 8u * 4u, 0);
    for (size_t i = 0; i + 3 < spec.watermark.rgba.size(); i += 4) {
        spec.watermark.rgba[i] = 255;      // R
        spec.watermark.rgba[i + 3] = 255;  // A
    }
    spec.watermark.opacity = 0.5f;
    spec.watermark.scale = 0.2f;   // long edge -> ~13 px in a 64px image
    spec.watermark.position = 4;   // centre

    const auto tmp = std::filesystem::temp_directory_path();
    const auto out = tmp / "lrc_p1_watermark.png";
    Adjustments adj;
    REQUIRE(exporter.exportPng(out, 1, *linear, adj, spec));

    auto dec = ImageIO::loadImage(out);
    REQUIRE(dec);
    REQUIRE(dec->width() == 64);
    REQUIRE(dec->height() == 64);
    REQUIRE(dec->format() == PixelFormat::U8);

    // Centre pixel is in the watermark region -> red blend.
    const uint8_t* center = dec->data().data() + (32 * 64 + 32) * 3;
    REQUIRE(center[0] >= 250);  // R stays high
    REQUIRE(center[1] <= 130);  // G = 255*0.5 + 0*0.5 ~ 127
    REQUIRE(center[2] <= 130);  // B likewise
    // Corner (outside the overlay) stays pure white.
    const uint8_t* corner = dec->data().data() + (2 * 64 + 2) * 3;
    REQUIRE(corner[0] == 255);
    REQUIRE(corner[1] == 255);
    REQUIRE(corner[2] == 255);

    std::error_code e;
    std::filesystem::remove(out, e);
}

TEST_CASE("P1-4: no watermark when disabled keeps the output, unchanged data",
          "[p1][watermark][export]") {
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    ExportEngine exporter(cm, pipeline);
    auto linear = ImageBuffer::create(16, 16, PixelFormat::F32);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            for (int c = 0; c < 3; ++c)
                linear->setPixel(x, y, c, 0.5f);
    // Default spec: watermark disabled. Export must match the legacy byte-identical
    // output (no overlays, no ICC).
    ExportEngine::ExportSpec spec;
    spec.format = ExportEngine::ExportSpec::Format::Png;
    const auto tmp = std::filesystem::temp_directory_path();
    const auto outA = tmp / "lrc_p1_wn_on.png";
    const auto outB = tmp / "lrc_p1_wn_off.png";
    REQUIRE(exporter.exportPng(outA, 1, *linear, Adjustments{}, spec));
    REQUIRE(exporter.exportPng(outB, 1, *linear, Adjustments{}));
    REQUIRE(readFile(outA) == readFile(outB));
    REQUIRE(readFile(outA).size() > 8);
    std::error_code e;
    std::filesystem::remove(outA, e);
    std::filesystem::remove(outB, e);
}