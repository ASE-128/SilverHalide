// GUI-layer (Qt) P1 acceptance tests.
//
// This target builds ONLY when LRC_ENABLE_QT=ON. It links the real widget
// sources (PreviewWorker + the import/export/snapshot dialogs) against Qt6 so
// the headless suite's gaps are covered with real object instances:
//   • PreviewWorker::batchExport real end-to-end export: a temporary JPEG is
//     decoded on the worker thread and encoded out to JPEG/PNG/TIFF16 files,
//     then the on-disk outputs assert their format magic + batchFinished counts.
//   • ExportDialog / ImportDialog / SnapshotDialog construction smoke.
//
// Unlike tests/lrc_tests (Qt-free Catch2), this uses the QtTest framework.

#include <QApplication>
#include <QCheckBox>
#include <QLoggingCategory>
#include <QColor>
#include <QColorSpace>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QImageReader>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "app/Constants.h"
#include "app/ModelManager.h"
#include "catalog/CatalogDatabase.h"
#include "catalog/ThumbnailCache.h"
#include "engine/ColorManager.h"
#include "engine/ImageBuffer.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"
#include "ui/DevelopPanel.h"
#include "ui/DuplicateResultsDialog.h"
#include "ui/ExportDialog.h"
#include "ui/ImageViewport.h"
#include "ui/ImportDialog.h"
#include "ui/ImportWorker.h"
#include "ui/PresetDialog.h"
#include "ui/PreviewWorker.h"
#include "ui/SnapshotDialog.h"
#include "ui/SoftProofingDialog.h"
#include "ui/WheelGuard.h"

// ui/ImportWorker.h pulls in ui/MetaTypes.h, the single declaration point for
// every cross-thread engine type (ImportMode, SoftProofParams, ...), so this file
// needs no Q_DECLARE_METATYPE of its own -- and must not add one: a second
// QMetaTypeId<T> specialization in the same translation unit is C2766.
#include "utils/PresetLibrary.h"

using namespace lrc;

// ---------------------------------------------------------------------------
// Small file helpers (mirror the ones in TestCompetitiveP1.cpp).
// ---------------------------------------------------------------------------
namespace {
std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

bool startsWith(const std::vector<uint8_t>& hay,
                const std::initializer_list<uint8_t>& front) {
    if (hay.size() < front.size())
        return false;
    size_t i = 0;
    for (uint8_t b : front)
        if (hay[i++] != b)
            return false;
    return true;
}

void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream os(p, std::ios::binary);
    os.write(reinterpret_cast<const char*>(b.data()),
             static_cast<std::streamsize>(b.size()));
}

// Deletes the start-of-scan marker from a JPEG byte stream, reproducing the
// exact shape of the R-08 defect (ICC payload intact, "FF DA" gone). Walks the
// length-prefixed segments itself so it cannot agree with the engine's parser by
// accident. Returns an empty vector when no SOS marker is found.
std::vector<uint8_t> stripJpegSos(std::vector<uint8_t> jpg) {
    if (jpg.size() < 2 || jpg[0] != 0xFF || jpg[1] != 0xD8)
        return {};
    size_t i = 2;
    while (i + 1 < jpg.size()) {
        if (jpg[i] != 0xFF)
            return {};
        const uint8_t m = jpg[i + 1];
        if (m == 0xDA) {
            jpg.erase(jpg.begin() + static_cast<std::ptrdiff_t>(i),
                      jpg.begin() + static_cast<std::ptrdiff_t>(i + 2));
            return jpg;
        }
        if (m == 0xD9 || i + 4 > jpg.size())
            return {};
        const size_t len = (static_cast<size_t>(jpg[i + 2]) << 8) | jpg[i + 3];
        if (len < 2 || i + 2 + len > jpg.size())
            return {};
        i += 2 + len;
    }
    return {};
}
}

// ---------------------------------------------------------------------------
// batchExport real end-to-end export
// ---------------------------------------------------------------------------
class BatchExportTest : public QObject {
    Q_OBJECT
public:
    explicit BatchExportTest(const std::filesystem::path& workDir)
        : workDir_(workDir) {}

private slots:
    void batchExportWritesAllFormats();

private:
    std::filesystem::path workDir_;
};

void BatchExportTest::batchExportWritesAllFormats() {
    std::error_code ec;
    std::filesystem::create_directories(workDir_, ec);

    // --- Source decodable file: write a synthetic U8 RGB image as JPEG. The
    // export path decodes via RawDecoder, so any supported format works; a JPEG
    // avoids shipping a camera RAW fixture.
    const auto src = workDir_ / "source.jpg";
    auto grad = ImageBuffer::create(48, 32, PixelFormat::U8);
    QVERIFY(grad && !grad->empty());
    {
        auto& px = grad->data();
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = static_cast<uint8_t>((i * 7) & 0xff);
    }
    QVERIFY(ImageIO::writeJpeg(src, *grad, 92));

    // --- Engine/catalog dependencies shared with the GUI at startup.
    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    RawDecoder decoder;
    decoder.setColorManager(cm);
    const auto cacheDir = workDir_ / "cache";
    std::filesystem::create_directories(cacheDir, ec);
    ThumbnailCache cache(cacheDir);
    CatalogDatabase db(workDir_ / "export_test.db");
    QVERIFY(db.open());

    // --- Run the worker on its own thread exactly like MainWindow::startWorkers.
    QThread thread;
    thread.setObjectName(QStringLiteral("p1_export_test"));
    auto* worker = new PreviewWorker(decoder, pipeline, cache, db, cm);
    worker->moveToThread(&thread);
    thread.start();

    // --- One ExportItem per format into distinct output files.
    struct C {
        ExportFormat fmt;
        const char* out;
        std::initializer_list<uint8_t> magic;
    };
    const C cases[] = {
        {ExportFormat::Jpeg, "out.jpg",
         {0xFF, 0xD8, 0xFF}},
        {ExportFormat::Png, "out.png",
         {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}},
        {ExportFormat::Tiff16, "out.tif",
         {'I', 'I', 0x2A, 0x00}},
    };
    QList<ExportItem> items;
    for (const auto& c : cases) {
        ExportItem it;
        it.imageId = 1;  // catalog id is only threaded through by encodeAndWrite
        it.filePath = QString::fromStdWString(src.wstring());
        // IM-6: the stack is frozen into a shared snapshot at enqueue time.
        it.adjustments =
            std::make_shared<const Adjustments>(Adjustments::defaults());
        it.outPath = QString::fromStdWString((workDir_ / c.out).wstring());
        it.format = c.fmt;
        it.quality = 92;
        it.maxDim = 0;
        items.append(it);
    }

    int success = -1, fail = -1;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(worker, &PreviewWorker::batchExportFinished, &loop,
                     [&](int s, int f, const QString&) {
                         success = s;
                         fail = f;
                         loop.quit();
                     });
    timeout.start(120000);  // generous safety: decode + 3 encodes.

    QMetaObject::invokeMethod(worker, "batchExport", Qt::QueuedConnection,
                              Q_ARG(QList<ExportItem>, items));
    loop.exec();

    QCOMPARE(success, static_cast<int>(items.size()));
    QCOMPARE(fail, 0);

    for (const auto& c : cases) {
        const auto bytes = readFile(workDir_ / c.out);
        QVERIFY2(startsWith(bytes, c.magic), c.out);
    }

    thread.quit();
    thread.wait();
    delete worker;

    std::filesystem::remove_all(workDir_, ec);
}

// ---------------------------------------------------------------------------
// Import / Export / Snapshot dialog construction smoke
// ---------------------------------------------------------------------------
class DialogSmokeTest : public QObject {
    Q_OBJECT
public:
    explicit DialogSmokeTest(const std::filesystem::path& workDir)
        : workDir_(workDir) {}

private slots:
    void initTestCase();
    void exportDialogSmoke();
    void importDialogSmoke();
    void snapshotDialogSmoke();
    void cleanupTestCase();

private:
    std::filesystem::path workDir_;
    std::unique_ptr<CatalogDatabase> db_;
    qint64 imageId_ = 0;
};

void DialogSmokeTest::initTestCase() {
    std::error_code ec;
    std::filesystem::create_directories(workDir_, ec);
    db_ = std::make_unique<CatalogDatabase>(workDir_ / "smoke.db");
    if (!db_->open())
        QFAIL("catalog open failed");
    imageId_ = db_->addImage("C:/photos/lrc_qt_smoke.jpg", "");
    QVERIFY(imageId_ > 0);
}

void DialogSmokeTest::cleanupTestCase() {
    const auto dbPath = db_ ? workDir_ / "smoke.db"
                            : std::filesystem::path{};
    db_.reset();
    if (!dbPath.empty()) {
        std::error_code ec;
        std::filesystem::remove(dbPath, ec);
        std::filesystem::remove(dbPath.string() + "-wal", ec);
        std::filesystem::remove(dbPath.string() + "-shm", ec);
    }
}

void DialogSmokeTest::exportDialogSmoke() {
    // A dialog can be constructed with a suggested path and interrogated without
    // accepting it (no exec, no modal loop).
    ExportDialog dlg(QStringLiteral("C:/photos/shot.jpg"), nullptr);
    QVERIFY(qobject_cast<QDialog*>(&dlg) != nullptr);
    QVERIFY(dlg.formatIndex() >= 0 && dlg.formatIndex() <= 4);
    QVERIFY(!dlg.outputPath().isEmpty());
    QVERIFY(!dlg.renameEnabled());

    // Variant selection activates only with at least one virtual copy.
    std::vector<EditVariant> variants = {EditVariant{0, "master", true},
                                         EditVariant{1, "copy", false}};
    dlg.setVariants(variants);
    QVERIFY(dlg.hasVariants());
    QCOMPARE(dlg.selectedVariantId(), qint64(0));  // master is the default.
    QVERIFY(ExportDialog::defaultFileName("IMG_001", 0).contains(".jpg"));
}

void DialogSmokeTest::importDialogSmoke() {
    ImportDialog dlg(nullptr);
    QVERIFY(qobject_cast<QDialog*>(&dlg) != nullptr);
    QVERIFY(!dlg.renameEnabled());
}

void DialogSmokeTest::snapshotDialogSmoke() {
    // Constructing against an image with no snapshots must not crash; reload()
    // re-queries the catalog's snapshot list for the master variant.
    SnapshotDialog dlg(*db_, imageId_, 0, nullptr);
    QVERIFY(qobject_cast<QDialog*>(&dlg) != nullptr);
    dlg.reload();

    // IM-6: applying a snapshot publishes an immutable shared snapshot. Store a
    // named snapshot with a distinctive stack, then apply it through the dialog
    // and check the signal payload is a non-null, fully populated pointer.
    Adjustments stored = Adjustments::defaults();
    stored.exposure = 0.75;
    stored.contrast = 30.0;
    QVERIFY(db_->saveEditSnapshot(imageId_, stored, 0, "SharedSignal") > 0);
    dlg.reload();
    // The dialog has a single list and does not name it.
    auto* list = dlg.findChild<QListWidget*>();
    QVERIFY(list != nullptr);
    QVERIFY(list->count() >= 1);
    QSignalSpy appliedSpy(&dlg, &SnapshotDialog::applied);
    list->setCurrentRow(0);
    QMetaObject::invokeMethod(&dlg, "onApplyClicked", Qt::DirectConnection);
    QCOMPARE(appliedSpy.count(), 1);
    const auto applied = appliedSpy.at(0).at(0).value<lrc::AdjustmentsPtr>();
    QVERIFY(applied != nullptr);
    QCOMPARE(applied->exposure, 0.75);
    QCOMPARE(applied->contrast, 30.0);
}

// ---------------------------------------------------------------------------
// P3-3 preset-manager dialog: save / list / apply / delete smoke
// ---------------------------------------------------------------------------
class PresetDialogSmokeTest : public QObject {
    Q_OBJECT
public:
    explicit PresetDialogSmokeTest(const std::filesystem::path& workDir)
        : workDir_(workDir) {}

private slots:
    void presetDialogFlow();

private:
    std::filesystem::path workDir_;
};

void PresetDialogSmokeTest::presetDialogFlow() {
    std::error_code ec;
    std::filesystem::create_directories(workDir_, ec);

    lrc::PresetLibrary lib(workDir_ / "presets");
    Adjustments current;
    current.exposure = 0.5;
    current.contrast = 25.0;

    // Constructing against an empty library must not crash.
    PresetDialog dlg(lib, current, nullptr);
    QVERIFY(qobject_cast<QDialog*>(&dlg) != nullptr);

    // Save a named preset through the dialog's name field + create slot.
    auto* nameEdit = dlg.findChild<QLineEdit*>(QStringLiteral("presetNameEdit"));
    QVERIFY(nameEdit != nullptr);
    nameEdit->setText(QStringLiteral("Smoke"));
    QMetaObject::invokeMethod(&dlg, "onCreateClicked", Qt::DirectConnection);
    QVERIFY(lib.exists("Smoke"));
    dlg.reload();

    // The stored preset appears in the list.
    auto* list = dlg.findChild<QListWidget*>(QStringLiteral("presetList"));
    QVERIFY(list != nullptr);
    QVERIFY(list->count() >= 1);

    // Applying fires applied() with the round-tripped stack.
    QSignalSpy appliedSpy(&dlg, &PresetDialog::applied);
    list->setCurrentRow(0);
    QMetaObject::invokeMethod(&dlg, "onApplyClicked", Qt::DirectConnection);
    QCOMPARE(appliedSpy.count(), 1);
    // IM-6: applied() publishes an immutable shared snapshot, not a reference.
    const auto adjPtr = appliedSpy.at(0).at(0).value<lrc::AdjustmentsPtr>();
    QVERIFY(adjPtr != nullptr);
    QCOMPARE(adjPtr->exposure, 0.5);
    QCOMPARE(adjPtr->contrast, 25.0);

    // Delete through the library; a fresh dialog then shows only the
    // empty-library placeholder row (no preset entries).
    QVERIFY(lib.deletePreset("Smoke"));
    PresetDialog dlg2(lib, current, nullptr);
    auto* list2 = dlg2.findChild<QListWidget*>(QStringLiteral("presetList"));
    QVERIFY(list2 != nullptr);
    QCOMPARE(list2->count(), 1);
    // The single row is the placeholder, not a real preset.
    QVERIFY(list2->item(0)->data(Qt::UserRole).isNull());
}

// ---------------------------------------------------------------------------
// ImportWorker regression: silent-failure holes
//
// Regression coverage for the "folder shows in the navigator, then nothing,
// no success and no error" import failure mode:
//   • requestAbort() must be one-shot — an abort left over from a previous
//     import (or a deferred closeEvent) must not silently swallow the next
//     import.
//   • A normal run must emit finishedImport with the real added count and
//     land the image row + thumbnail preview in the catalog.
// ---------------------------------------------------------------------------
class ImportWorkerTest : public QObject {
    Q_OBJECT
public:
    explicit ImportWorkerTest(const std::filesystem::path& workDir)
        : workDir_(workDir) {}

private slots:
    void leftoverAbortDoesNotSilentlyKillNextImport();
    void importAddsImageAndPreviews();

private:
    std::filesystem::path workDir_;
};

void ImportWorkerTest::leftoverAbortDoesNotSilentlyKillNextImport() {
    std::error_code ec;
    std::filesystem::create_directories(workDir_, ec);

    // A tiny JPEG is importable without a camera RAW fixture.
    const auto src = workDir_ / "leftover_abort.jpg";
    auto img = ImageBuffer::create(32, 24, PixelFormat::U8);
    QVERIFY(img && !img->empty());
    std::fill(img->data().begin(), img->data().end(), uint8_t{128});
    QVERIFY(ImageIO::writeJpeg(src, *img, 90));

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    RawDecoder decoder;
    decoder.setColorManager(cm);
    const auto cacheDir = workDir_ / "cache";
    std::filesystem::create_directories(cacheDir, ec);
    ThumbnailCache cache(cacheDir);
    CatalogDatabase db(workDir_ / "leftover_abort.db");
    QVERIFY(db.open());

    ImportWorker worker(db, cache, decoder, pipeline, cm);
    QSignalSpy finishedSpy(&worker, &ImportWorker::finishedImport);
    QSignalSpy warningSpy(&worker, &ImportWorker::importWarning);

    // Simulate an abort left over from an earlier import / deferred close.
    worker.requestAbort();

    // Direct (same-thread) call: the abort flag must be reset at import entry
    // and the import must complete. Before the fix this returned at the entry
    // check with no signal at all.
    worker.importPaths(QStringList(QString::fromStdWString(src.wstring())),
                       false, ImportMode::Add, QString(), QString());

    for (int i = 0; i < warningSpy.count(); ++i)
        qInfo("import warning: %s",
              qPrintable(warningSpy.at(i).at(0).toString()));
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(warningSpy.count(), 0);
    QCOMPARE(db.listImages().size(), size_t{1});
}

void ImportWorkerTest::importAddsImageAndPreviews() {
    std::error_code ec;
    std::filesystem::create_directories(workDir_, ec);

    const auto src = workDir_ / "plain.jpg";
    auto img = ImageBuffer::create(48, 32, PixelFormat::U8);
    QVERIFY(img && !img->empty());
    std::fill(img->data().begin(), img->data().end(), uint8_t{200});
    QVERIFY(ImageIO::writeJpeg(src, *img, 90));

    auto cm = std::make_shared<ColorManager>();
    Pipeline pipeline(cm);
    RawDecoder decoder;
    decoder.setColorManager(cm);
    const auto cacheDir = workDir_ / "cache";
    std::filesystem::create_directories(cacheDir, ec);
    ThumbnailCache cache(cacheDir);
    CatalogDatabase db(workDir_ / "plain.db");
    QVERIFY(db.open());

    ImportWorker worker(db, cache, decoder, pipeline, cm);
    QSignalSpy finishedSpy(&worker, &ImportWorker::finishedImport);
    QSignalSpy thumbSpy(&worker, &ImportWorker::thumbReadyImage);

    worker.importPaths(QStringList(QString::fromStdWString(src.wstring())),
                       false, ImportMode::Add, QString(), QString());

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 1);

    const auto images = db.listImages();
    QCOMPARE(images.size(), size_t{1});
    if (!images.empty()) {
        // Thumbnail + standard preview must land in the same import.
        QVERIFY(db.loadPreview(images[0].id, PreviewKind::Thumbnail));
        QVERIFY(db.loadPreview(images[0].id, PreviewKind::Standard));
    }
    // The worker ships the decoded thumbnail back for the filmstrip.
    QVERIFY(thumbSpy.count() >= 1);
}

// ---------------------------------------------------------------------------
// Viewport view-stability contract
//
// Regression coverage for the mask-editing "the view jumps while I draw"
// symptom: a same-image re-render (MainWindow::onPreviewReady ->
// setImageKeepView) must preserve the user's zoom; only a genuinely new image
// (setImage) may reset the view.
// ---------------------------------------------------------------------------
class ViewportViewStabilityTest : public QObject {
    Q_OBJECT
private slots:
    void keepViewPreservesZoomAcrossRefresh();
    void setImageResetsView();
};

void ViewportViewStabilityTest::keepViewPreservesZoomAcrossRefresh() {
    ImageViewport vp;
    vp.resize(800, 600);
    QImage first(400, 300, QImage::Format_RGB32);
    first.fill(Qt::darkGray);
    vp.setImage(first);
    vp.zoomIn();  // zoom_ 1.0 -> 1.25

    // Same-image refresh (edit re-render): zoom must survive.
    QImage refresh(400, 300, QImage::Format_RGB32);
    refresh.fill(Qt::lightGray);
    vp.setImageKeepView(refresh);
    QCOMPARE(vp.zoomFactor(), 1.25);
}

void ViewportViewStabilityTest::setImageResetsView() {
    ImageViewport vp;
    vp.resize(800, 600);
    QImage first(400, 300, QImage::Format_RGB32);
    first.fill(Qt::darkGray);
    vp.setImage(first);
    vp.zoomIn();  // zoom_ 1.0 -> 1.25

    // A genuinely new image re-establishes the default view.
    QImage second(500, 400, QImage::Format_RGB32);
    second.fill(Qt::black);
    vp.setImage(second);
    QCOMPARE(vp.zoomFactor(), 1.0);
}

// ---------------------------------------------------------------------------
// Mask overlay rendering (AI weights, including the sub-mask branch)
//
// `ImageViewport::rebuildMaskOverlay` used to route AI masks through the
// geometry-only `Pipeline::brushWeightField`, whose MaskType::AiMask case
// returns a constant 0.5. The overlay therefore painted one flat 50% red wash
// over the whole frame -- reading as "everything is selected" -- while the
// render applied the real content-driven field, and it did so in both the plain
// branch and the colour/luminance sub-mask branch. These cases drive the real
// widget and inspect the pixels it composites, so they fail again if the
// overlay stops following the AI field.
// ---------------------------------------------------------------------------
namespace {

// Mean overlay alpha of an image band. The overlay raster encodes its weight as
// the red alpha (`alpha = weight * 150`), so averaging a band instead of reading
// one pixel keeps the value stable against the anti-aliased mask edges.
double meanOverlayAlpha(const QImage& img, double y0Frac, double y1Frac) {
    const int y0 = static_cast<int>(img.height() * y0Frac);
    const int y1 = static_cast<int>(img.height() * y1Frac);
    const int x0 = static_cast<int>(img.width() * 0.3);
    const int x1 = static_cast<int>(img.width() * 0.7);
    double sum = 0.0;
    int count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            sum += static_cast<double>(qAlpha(img.pixel(x, y)));
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

// A uniform mid-grey preview: the base is neutral, so every red tint in the
// overlay comes from the weight field, and the sky heuristic's only signal is
// its upper-region bias.
QImage neutralPreview(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(128, 128, 128));
    return img;
}

} // namespace

class MaskOverlayRenderTest : public QObject {
    Q_OBJECT
private slots:
    void aiMaskOverlayFollowsFieldWithSubMask();
    void aiOverlayRecomputeIsDebounced();
};

void MaskOverlayRenderTest::aiMaskOverlayFollowsFieldWithSubMask() {
    ImageViewport vp;
    vp.resize(480, 360);
    vp.setImage(neutralPreview(480, 360));

    lrc::Mask m;
    m.type = lrc::MaskType::AiMask;
    m.ai().target = lrc::AiMaskTarget::Sky;
    // A luminance sub-mask that passes every pixel (widest range, no softness),
    // so the combined weight is exactly the AI field. This is the branch that
    // used to multiply the 0.5 placeholder by the sub-mask.
    m.useLuminance = true;
    m.luminanceRef = 50.0f;
    m.luminanceRange = 100.0f;
    m.luminanceSmoothness = 0.0f;

    vp.setMaskTool(lrc::MaskType::AiMask);
    vp.setMaskDraft(m);

    const QImage overlay = vp.maskOverlayImage();
    QVERIFY2(!overlay.isNull(),
             qPrintable(QStringLiteral("overlay=%1x%2")
                            .arg(overlay.width())
                            .arg(overlay.height())));
    const double top = meanOverlayAlpha(overlay, 0.05, 0.20);
    const double bottom = meanOverlayAlpha(overlay, 0.80, 0.95);
    QVERIFY2(top > bottom + 40.0,
             qPrintable(QStringLiteral("top=%1 bottom=%2").arg(top).arg(bottom)));
}

void MaskOverlayRenderTest::aiOverlayRecomputeIsDebounced() {
    ImageViewport vp;
    vp.resize(480, 360);
    vp.setImage(neutralPreview(480, 360));

    lrc::Mask m;
    m.type = lrc::MaskType::AiMask;
    m.ai().target = lrc::AiMaskTarget::Sky;
    m.ai().threshold = 0.5f;
    vp.setMaskTool(lrc::MaskType::AiMask);
    vp.setMaskDraft(m);

    const double before = meanOverlayAlpha(vp.maskOverlayImage(), 0.05, 0.20);
    QVERIFY2(before > 40.0, qPrintable(QStringLiteral("before=%1").arg(before)));

    // A parameter edit invalidates the memo, but the segmentation pass must not
    // run synchronously: the overlay keeps the previous weights and catches up
    // when the debounce expires. That is what stops a drag of the AI feather
    // slider from running one inference per tick on the GUI thread.
    lrc::Mask hot = m;
    hot.ai().threshold = 0.9f;  // far above the heuristic's upper-region weight
    vp.setMaskDraft(hot);
    const double duringDrag = meanOverlayAlpha(vp.maskOverlayImage(), 0.05, 0.20);
    QVERIFY2(duringDrag > before - 5.0 && duringDrag < before + 5.0,
             qPrintable(QStringLiteral("during=%1 before=%2")
                            .arg(duringDrag)
                            .arg(before)));

    QTest::qWait(2 * lrc::app::throttle::kAiOverlayFieldDebounceMs + 100);
    const double after = meanOverlayAlpha(vp.maskOverlayImage(), 0.05, 0.20);
    QVERIFY2(after < duringDrag - 20.0,
             qPrintable(QStringLiteral("after=%1 during=%2")
                            .arg(after)
                            .arg(duringDrag)));
}

// ---------------------------------------------------------------------------
// AI model availability surfacing
//
// Regression coverage for "every AI feature silently does nothing": with no
// engine / no models registered (the state the test process is in, since no
// MainWindow ever ran), invoking an AI feature must emit
// DevelopPanel::aiModelUnavailable with the missing manifest id, and the AI
// mask must still be created (heuristic fallback) so the feature degrades
// loudly instead of vanishing.
// ---------------------------------------------------------------------------
class AiModelAvailabilityTest : public QObject {
    Q_OBJECT
private slots:
    void aiMaskButtonsWarnWhenModelMissing();
    void aiDenoiseEnableWarnsWhenModelMissing();
};

void AiModelAvailabilityTest::aiMaskButtonsWarnWhenModelMissing() {
    DevelopPanel panel;
    QSignalSpy warnSpy(&panel, &DevelopPanel::aiModelUnavailable);
    QSignalSpy maskSpy(&panel, &DevelopPanel::maskEditRequested);

    auto* subjectBtn =
        panel.findChild<QPushButton*>(QStringLiteral("maskAiSubjectBtn"));
    QVERIFY(subjectBtn != nullptr);
    subjectBtn->click();

    QCOMPARE(warnSpy.count(), 1);
    QCOMPARE(warnSpy.at(0).at(0).toString(), QStringLiteral("AI 主体"));
    QCOMPARE(warnSpy.at(0).at(1).toString(),
             QStringLiteral("rmbg2-subject"));
    QCOMPARE(warnSpy.at(0).at(2).toBool(), true);
    // The mask is still created (heuristic fallback), only the quality
    // degrades — the warning says so explicitly.
    QCOMPARE(maskSpy.count(), 1);

    auto* promptBtn =
        panel.findChild<QPushButton*>(QStringLiteral("maskAiPromptBtn"));
    QVERIFY(promptBtn != nullptr);
    promptBtn->click();
    QCOMPARE(warnSpy.count(), 2);
    QCOMPARE(warnSpy.at(1).at(1).toString(),
             QStringLiteral("mobilesam-encoder"));
}

void AiModelAvailabilityTest::aiDenoiseEnableWarnsWhenModelMissing() {
    DevelopPanel panel;
    QSignalSpy warnSpy(&panel, &DevelopPanel::aiModelUnavailable);

    auto* enableCheck =
        panel.findChild<QCheckBox*>(QStringLiteral("aiDenoiseEnabledCheck"));
    QVERIFY(enableCheck != nullptr);
    enableCheck->setChecked(true);

    QCOMPARE(warnSpy.count(), 1);
    QCOMPARE(warnSpy.at(0).at(0).toString(), QStringLiteral("AI 降噪"));
    QCOMPARE(warnSpy.at(0).at(1).toString(),
             QStringLiteral("nafnet-denoise"));
    // No heuristic fallback exists for denoise: the warning must say the
    // feature did nothing.
    QCOMPARE(warnSpy.at(0).at(2).toBool(), false);
}

// ---------------------------------------------------------------------------
// Independent colour-range sampler (regression).
//
// The panel's own "取样" button used to emit colorRangeSamplerRequested with no
// receiver anywhere, so clicking it did nothing and only the C shortcut armed
// the viewport sampler. These tests pin both halves of the contract MainWindow
// now wires up: the button emits the request, and the state mirror used by the
// C/W handlers updates the button without re-emitting.
// ---------------------------------------------------------------------------
class ColorRangeSamplerTest : public QObject {
    Q_OBJECT
private slots:
    void panelEmitsSamplerRequest();
    void checkedMirrorIsSilent();
    void subSamplerMirrorIsSilent();
};

void ColorRangeSamplerTest::panelEmitsSamplerRequest() {
    DevelopPanel panel;
    auto* button = panel.findChild<QPushButton*>(
        QStringLiteral("colorRangeSampleButton"));
    QVERIFY(button != nullptr);
    QSignalSpy spy(&panel, &DevelopPanel::colorRangeSamplerRequested);

    button->setChecked(true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);

    button->setChecked(false);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
}

void ColorRangeSamplerTest::checkedMirrorIsSilent() {
    DevelopPanel panel;
    auto* button = panel.findChild<QPushButton*>(
        QStringLiteral("colorRangeSampleButton"));
    QVERIFY(button != nullptr);
    QSignalSpy spy(&panel, &DevelopPanel::colorRangeSamplerRequested);

    panel.setColorRangeSamplerChecked(true);
    QCOMPARE(button->isChecked(), true);
    QCOMPARE(spy.count(), 0);
    panel.setColorRangeSamplerChecked(false);
    QCOMPARE(button->isChecked(), false);
    QCOMPARE(spy.count(), 0);
}

// ---------------------------------------------------------------------------
// IM-6: edit-entry signals publish immutable shared snapshots
//
// The develop panel used to emit `const Adjustments&`, which forced MainWindow
// to deep-copy the stack for every edit entry. The panel now publishes
// `lrc::AdjustmentsPtr`; the contract worth pinning is that the receiver gets a
// non-null, fully populated snapshot it can adopt and dispatch without copying
// the stack again.
// ---------------------------------------------------------------------------
class AdjustmentsSignalTest : public QObject {
    Q_OBJECT
private slots:
    void panelPublishesSharedSnapshot();
};

void AdjustmentsSignalTest::panelPublishesSharedSnapshot() {
    DevelopPanel panel;
    QSignalSpy changedSpy(&panel, &DevelopPanel::adjustmentsChanged);
    QSignalSpy committedSpy(&panel, &DevelopPanel::adjustmentsCommitted);

    // resetAll() is a public one-shot edit path that emits both edit signals.
    panel.resetAll();

    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(committedSpy.count(), 1);
    const auto changed = changedSpy.at(0).at(0).value<lrc::AdjustmentsPtr>();
    const auto committed =
        committedSpy.at(0).at(0).value<lrc::AdjustmentsPtr>();
    QVERIFY(changed != nullptr);
    QVERIFY(committed != nullptr);
    QCOMPARE(changed->exposure, 0.0);
    QVERIFY(*changed == *committed);
    // The published snapshot is the panel's clamped current state.
    QVERIFY(*changed == panel.adjustments());
}

void ColorRangeSamplerTest::subSamplerMirrorIsSilent() {
    DevelopPanel panel;
    auto* subColor = panel.findChild<QPushButton*>(
        QStringLiteral("subColorRangeSampleButton"));
    auto* subLuma = panel.findChild<QPushButton*>(
        QStringLiteral("subLuminanceSampleButton"));
    QVERIFY(subColor != nullptr);
    QVERIFY(subLuma != nullptr);
    QSignalSpy colorSpy(&panel, &DevelopPanel::colorRangeSubSamplerRequested);
    QSignalSpy lumaSpy(&panel, &DevelopPanel::luminanceSamplerRequested);

    panel.setRangeSubSamplerChecked(true);
    QCOMPARE(subColor->isChecked(), true);
    QCOMPARE(subLuma->isChecked(), true);
    QCOMPARE(colorSpy.count(), 0);
    QCOMPARE(lumaSpy.count(), 0);
}

// ---------------------------------------------------------------------------
// Soft proofing dialog (视图 → 软打样…).
//
// The dialog used to be unreachable dead code. MainWindow now opens it modeless
// and forwards paramsChanged to the preview worker, so the contract worth
// pinning is: a control change emits paramsChanged carrying a usable
// destination key, and the enable state is reported separately.
// ---------------------------------------------------------------------------
class SoftProofingDialogSmokeTest : public QObject {
    Q_OBJECT
private slots:
    void emitsParamsChangedOnToggle();
    void reportsEnableState();
};

void SoftProofingDialogSmokeTest::emitsParamsChangedOnToggle() {
    // The dialog only stores the ColorManager (its profile list is static), so a
    // null manager is fine here and avoids building a real colour pipeline.
    lrc::SoftProofingDialog dlg(nullptr);
    QSignalSpy spy(&dlg, &lrc::SoftProofingDialog::paramsChanged);

    auto* enable = dlg.findChild<QCheckBox*>();
    QVERIFY(enable != nullptr);
    enable->setChecked(true);

    QCOMPARE(spy.count(), 1);
    const lrc::SoftProofParams params = dlg.currentParams();
    QVERIFY(!params.destinationProfilePath.empty());
    QVERIFY(params.useBlackPointCompensation);
}

void SoftProofingDialogSmokeTest::reportsEnableState() {
    lrc::SoftProofingDialog dlg(nullptr);
    QVERIFY(!dlg.proofingEnabled());
    auto* enable = dlg.findChild<QCheckBox*>();
    QVERIFY(enable != nullptr);
    enable->setChecked(true);
    QVERIFY(dlg.proofingEnabled());
}

// ---------------------------------------------------------------------------
// Duplicate results dialog (照片 → 查找重复项…).
//
// Also previously unreachable. The scan feeds it a group list and the catalog;
// passing a null catalog must still render the report (stacking disabled).
// ---------------------------------------------------------------------------
class DuplicateResultsDialogSmokeTest : public QObject {
    Q_OBJECT
private slots:
    void rendersGroupWithoutCatalog();
};

void DuplicateResultsDialogSmokeTest::rendersGroupWithoutCatalog() {
    std::vector<lrc::DuplicateGroup> groups;
    lrc::DuplicateGroup g;
    g.memberIds = {7, 8};
    g.meanSimilarity = 0.97f;
    groups.push_back(g);

    lrc::DuplicateResultsDialog dlg(groups, nullptr);
    QCOMPARE(dlg.findChildren<QListWidget*>().size(), 1);
    QCOMPARE(dlg.findChildren<QTableWidget*>().size(), 1);
    // Both members must be listed for the single group.
    auto* table = dlg.findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    QCOMPARE(table->rowCount(), 2);
}

// ---------------------------------------------------------------------------
// Exported JPEG decodability (R-08).
//
// The engine's own loader goes through stb_image, which is permissive enough to
// "decode" a structurally broken JPEG into a partially written buffer — so it
// cannot tell us whether writeJpeg() produced a valid file. Qt's JPEG plugin is
// an independent judge (bundled libjpeg), and this is the contract that matters
// to the user: an exported ICC JPEG must open in a normal application.
// ---------------------------------------------------------------------------
class IccJpegDecodabilityTest : public QObject {
    Q_OBJECT
private slots:
    void exportedIccJpegIsReadableByQt();
    void jpegWithoutSosMarkerIsNotDecodable();
};

void IccJpegDecodabilityTest::exportedIccJpegIsReadableByQt() {
    QVERIFY2(QImageReader::supportedImageFormats().contains("jpeg"),
             "Qt JPEG plugin missing from the test environment");

    // Flat patch: a constant colour survives a q95 JPEG round-trip to within a
    // few LSBs, so the pixel check below is about the ICC splice and not about
    // encoder rounding.
    auto src = ImageBuffer::create(32, 24, PixelFormat::U8);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 32; ++x) {
            auto* px = src->data().data() + (static_cast<size_t>(y) * 32 + x) * 3;
            px[0] = 60;
            px[1] = 160;
            px[2] = 100;
        }
    const auto icc =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::SRGB);
    QVERIFY(!icc.empty());

    const auto p = std::filesystem::temp_directory_path() / "lrc_qt_r08_icc.jpg";
    QVERIFY(ImageIO::writeJpeg(p, *src, 95, icc));

    QImageReader reader(QString::fromStdString(p.string()));
    QVERIFY2(reader.canRead(), qPrintable(reader.errorString()));
    const QImage img = reader.read();
    QVERIFY2(!img.isNull(), qPrintable(reader.errorString()));
    QCOMPARE(img.size(), QSize(32, 24));

    const QColor c = img.pixelColor(16, 12);
    QVERIFY2(std::abs(c.red() - 60) <= 4 && std::abs(c.green() - 160) <= 4 &&
                 std::abs(c.blue() - 100) <= 4,
             qPrintable(QString("decoded pixel rgb(%1,%2,%3) is not the written "
                                "flat patch")
                            .arg(c.red())
                            .arg(c.green())
                            .arg(c.blue())));

    // The embedded profile must survive the round trip intact, not merely be
    // present as bytes: Qt's JPEG reader drops any ICC_PROFILE chunk with a
    // payload of 146 bytes or fewer, so a needlessly small chunk size used to
    // truncate the profile (588 -> 510 bytes) and this parse silently failed.
    QVERIFY2(img.colorSpace().isValid(),
             "Qt did not recognise the embedded ICC profile");
    QCOMPARE(img.colorSpace().iccProfile().size(), int(icc.size()));

    std::error_code ec;
    std::filesystem::remove(p, ec);
}

void IccJpegDecodabilityTest::jpegWithoutSosMarkerIsNotDecodable() {
    // Negative control: proves Qt is sensitive to the exact defect (a missing
    // "FF DA"), so the positive test above would have caught it.
    QVERIFY2(QImageReader::supportedImageFormats().contains("jpeg"),
             "Qt JPEG plugin missing from the test environment");

    auto src = ImageBuffer::create(32, 24, PixelFormat::U8);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 32; ++x) {
            auto* px = src->data().data() + (static_cast<size_t>(y) * 32 + x) * 3;
            px[0] = 60;
            px[1] = 160;
            px[2] = 100;
        }
    const auto icc =
        ColorManager::exportProfileBytes(ColorManager::ExportColorProfile::SRGB);
    const auto valid = std::filesystem::temp_directory_path() / "lrc_qt_r08_ok.jpg";
    QVERIFY(ImageIO::writeJpeg(valid, *src, 95, icc));

    auto corrupted = stripJpegSos(readFile(valid));
    QVERIFY2(!corrupted.empty(), "test helper failed to locate the SOS marker");
    const auto bad = std::filesystem::temp_directory_path() / "lrc_qt_r08_nosos.jpg";
    writeFile(bad, corrupted);

    QImageReader reader(QString::fromStdString(bad.string()));
    const QImage img = reader.read();
    QVERIFY2(img.isNull(),
             "a JPEG with no SOS marker must not decode; Qt returned a "
             "non-null image");

    std::error_code ec;
    std::filesystem::remove(valid, ec);
    std::filesystem::remove(bad, ec);
}

// ---------------------------------------------------------------------------
// Soft proofing on the view's own render paths.
//
// requestProof() only proofed the preview-resolution frame, so the two frames it
// cannot reach stayed unproofed: the coarse drag frame (the image visibly jumped
// colour on release) and the 1:1 full-res frame (the viewport drops the
// preview-sized proof frame while showing full resolution). Both are covered
// here end-to-end through the real worker.
//
// The alarm colour is the observable: it is set to magenta, which no input in the
// fixture contains, and only the gamut-check leg paints it. So "does this frame
// carry proofing?" is an exact pixel count rather than a statistical question.
// ---------------------------------------------------------------------------
namespace {

constexpr uint8_t kAlarmR = 255;
constexpr uint8_t kAlarmG = 0;
constexpr uint8_t kAlarmB = 255;

// Saturated Adobe RGB colours. Interpreting them through the embedded wide-gamut
// profile is what puts them outside the sRGB proof destination — an sRGB-sourced
// image is inside it by construction, so it would give the gamut check nothing to
// mark.
bool writeWideGamutFixture(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto src = ImageBuffer::create(64, 48, PixelFormat::U8);
    if (!src || src->empty())
        return false;
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            if (y < 24 && x < 32)      r = 255;              // red
            else if (y < 24)           g = 255;              // green
            else if (x < 32)           { g = 255; b = 255; } // cyan
            else                       { r = 255; g = 255; } // yellow
            auto* px = src->data().data() + (static_cast<size_t>(y) * 64 + x) * 3;
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    const auto icc = ColorManager::exportProfileBytes(
        ColorManager::ExportColorProfile::AdobeRGB);
    return !icc.empty() && ImageIO::writePng(path, *src, icc);
}

// Number of pixels painted with the proofing alarm colour.
int countAlarmPixels(const lrc::ImageBufferPtr& frame) {
    if (!frame || frame->format() != lrc::PixelFormat::U8 || frame->empty())
        return -1;
    int n = 0;
    const uint8_t* p = frame->data().data();
    for (size_t i = 0; i + 2 < frame->data().size(); i += 3)
        if (p[i] == kAlarmR && p[i + 1] == kAlarmG && p[i + 2] == kAlarmB)
            ++n;
    return n;
}

lrc::SoftProofParams alarmProofParams() {
    lrc::SoftProofParams params;
    params.destinationProfilePath = "srgb";  // built-in preset key
    params.showOutOfGamut = true;
    params.outOfGamutR = static_cast<float>(kAlarmR) / 255.0f;
    params.outOfGamutG = static_cast<float>(kAlarmG) / 255.0f;
    params.outOfGamutB = static_cast<float>(kAlarmB) / 255.0f;
    return params;
}

// Engine objects + a running worker thread for one render-path case.
struct ProofHarness {
    std::filesystem::path dir;
    std::shared_ptr<ColorManager> cm;
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<RawDecoder> decoder;
    std::unique_ptr<ThumbnailCache> cache;
    std::unique_ptr<CatalogDatabase> db;
    QThread thread;
    PreviewWorker* worker = nullptr;
    bool ok = false;

    ~ProofHarness() { stop(); }

    void start(const std::filesystem::path& root,
               const std::filesystem::path& imagePath) {
        std::error_code ec;
        dir = root;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "cache", ec);
        if (!writeWideGamutFixture(imagePath))
            return;
        cm = std::make_shared<ColorManager>();
        pipeline = std::make_unique<Pipeline>(cm);
        decoder = std::make_unique<RawDecoder>();
        decoder->setColorManager(cm);
        cache = std::make_unique<ThumbnailCache>(dir / "cache");
        db = std::make_unique<CatalogDatabase>(dir / "proof.db");
        if (!db->open())
            return;
        thread.setObjectName(QStringLiteral("softproof_paths"));
        worker = new PreviewWorker(*decoder, *pipeline, *cache, *db, cm);
        worker->moveToThread(&thread);
        thread.start();
        ok = true;
    }

    void stop() {
        if (!worker)
            return;
        thread.quit();
        thread.wait();
        delete worker;
        worker = nullptr;
    }
};

} // namespace

class SoftProofRenderPathsTest : public QObject {
    Q_OBJECT
private slots:
    void coarseDragFrameIsProofed();
    void fullResFrameIsProofed();
};

void SoftProofRenderPathsTest::coarseDragFrameIsProofed() {
    const auto root = std::filesystem::temp_directory_path() / "lrc_proof_coarse";
    const auto image = root / "wide.png";
    ProofHarness h;
    h.start(root, image);
    QVERIFY(h.ok);

    const auto adj = Adjustments::defaults();
    // IM-6: queued requests carry a shared immutable snapshot of the stack.
    const auto adjPtr = std::make_shared<const Adjustments>(adj);
    const QString path = QString::fromStdWString(image.wstring());

    // Decode the fixture into the worker's working linear buffer (no catalog row
    // is needed: the standard-preview fast path just misses and it decodes).
    QSignalSpy previewSpy(h.worker, &PreviewWorker::previewReady);
    QMetaObject::invokeMethod(h.worker, "loadImage", Qt::QueuedConnection,
                              Q_ARG(qint64, 1), Q_ARG(QString, path),
                              Q_ARG(qint64, 1), Q_ARG(lrc::AdjustmentsPtr, adjPtr));
    QTRY_COMPARE_WITH_TIMEOUT(previewSpy.count(), 1, 60000);

    QSignalSpy coarseSpy(h.worker, &PreviewWorker::previewReadyCoarse);

    // Baseline: an unarmed coarse frame carries no alarm colour.
    QMetaObject::invokeMethod(h.worker, "adjustPreviewCoarse",
                              Qt::QueuedConnection, Q_ARG(qint64, 1),
                              Q_ARG(qint64, 2), Q_ARG(lrc::AdjustmentsPtr, adjPtr));
    QTRY_COMPARE_WITH_TIMEOUT(coarseSpy.count(), 1, 60000);
    auto plain = coarseSpy.at(0).at(1).value<lrc::ImageBufferPtr>();
    QVERIFY(plain && !plain->empty());
    QCOMPARE(countAlarmPixels(plain), 0);

    // Arm proofing, then ask for another coarse frame: it must come out proofed.
    // Both calls are queued to the same worker thread, so the state is recorded
    // before the frame is rendered.
    QMetaObject::invokeMethod(h.worker, "setProofState", Qt::QueuedConnection,
                              Q_ARG(lrc::SoftProofParams, alarmProofParams()),
                              Q_ARG(bool, true));
    QMetaObject::invokeMethod(h.worker, "adjustPreviewCoarse",
                              Qt::QueuedConnection, Q_ARG(qint64, 1),
                              Q_ARG(qint64, 3), Q_ARG(lrc::AdjustmentsPtr, adjPtr));
    QTRY_COMPARE_WITH_TIMEOUT(coarseSpy.count(), 2, 60000);
    auto proofed = coarseSpy.at(1).at(1).value<lrc::ImageBufferPtr>();
    QVERIFY(proofed && !proofed->empty());
    QVERIFY2(countAlarmPixels(proofed) > 0,
             "the coarse drag frame did not carry the soft proof");
}

void SoftProofRenderPathsTest::fullResFrameIsProofed() {
    const auto root =
        std::filesystem::temp_directory_path() / "lrc_proof_fullres";
    const auto image = root / "wide.png";
    ProofHarness h;
    h.start(root, image);
    QVERIFY(h.ok);

    const auto adj = Adjustments::defaults();
    // IM-6: one shared snapshot serves every queued request of this test.
    const auto adjPtr = std::make_shared<const Adjustments>(adj);
    const QString path = QString::fromStdWString(image.wstring());
    QSignalSpy fullSpy(h.worker, &PreviewWorker::fullResReady);

    // Armed: the 1:1 frame itself must be proofed (MainWindow drops the
    // preview-sized proofReady while the viewport shows full resolution).
    QMetaObject::invokeMethod(h.worker, "setProofState", Qt::QueuedConnection,
                              Q_ARG(lrc::SoftProofParams, alarmProofParams()),
                              Q_ARG(bool, true));
    QMetaObject::invokeMethod(h.worker, "loadFullRes", Qt::QueuedConnection,
                              Q_ARG(qint64, 1), Q_ARG(QString, path),
                              Q_ARG(qint64, 1), Q_ARG(lrc::AdjustmentsPtr, adjPtr));
    QTRY_COMPARE_WITH_TIMEOUT(fullSpy.count(), 1, 120000);
    auto proofed = fullSpy.at(0).at(1).value<lrc::ImageBufferPtr>();
    QVERIFY(proofed && !proofed->empty());
    QVERIFY2(countAlarmPixels(proofed) > 0,
             "the 1:1 full-res frame did not carry the soft proof");

    // Switching proofing off must invalidate the frame for the same adjustment
    // stack (the dedup key includes the proof state) and bring the plain frame
    // back.
    QMetaObject::invokeMethod(h.worker, "setProofState", Qt::QueuedConnection,
                              Q_ARG(lrc::SoftProofParams, alarmProofParams()),
                              Q_ARG(bool, false));
    QMetaObject::invokeMethod(h.worker, "adjustFullRes", Qt::QueuedConnection,
                              Q_ARG(qint64, 1), Q_ARG(qint64, 2),
                              Q_ARG(lrc::AdjustmentsPtr, adjPtr));
    QTRY_COMPARE_WITH_TIMEOUT(fullSpy.count(), 2, 120000);
    auto plain = fullSpy.at(1).at(1).value<lrc::ImageBufferPtr>();
    QVERIFY(plain && !plain->empty());
    QCOMPARE(countAlarmPixels(plain), 0);
}

// ---------------------------------------------------------------------------
// 1:1 entry hot-path timing (P-A anchor B-1 attribution)
//
// The B-1 verdict -- "50MP, default adjustments, cache hit: 1:1 entry <= 175 ms;
// cold entry <= 1500 ms" -- must be taken on a real machine with a real >=50MP
// RAW and the GUI in the loop. This suite measures the part that is reachable
// without either: how long PreviewWorker::loadFullRes takes to produce a frame
// for each of its cache legs, observed from the test thread (request -> signal,
// queued hop included).
//
// Two boundaries matter and are never blurred here:
//   • The fixture is small (256x192), so the cold "decode + render" number is a
//     scheduler/plumbing figure, NOT a 50MP performance claim.
//   • The early-return leg renders nothing and emits nothing, so it is excluded
//     from the anchor verdict: a ~0 ms repeat is not evidence of a fast hot path.
//
// The worker's own [PERF1A] qInfo lines (hit=embedded / hit=disk / hit=memU8 /
// hit=earlyReturn, plus decode/render legs) identify which branch answered each
// request; the numbers below are the request-to-frame wall time around them.
// ---------------------------------------------------------------------------
namespace {

// Writes a small synthetic JPEG fixture. No camera RAW sample is shipped in the
// repo, and the 1:1 path accepts anything RawDecoder can read, so a JPEG keeps
// the fixture self-contained (same trick as BatchExportTest).
bool writeFullResFixture(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto src = ImageBuffer::create(256, 192, PixelFormat::U8);
    if (!src || src->empty())
        return false;
    auto& px = src->data();
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = static_cast<uint8_t>((i * 11) & 0xff);
    return ImageIO::writeJpeg(path, *src, 92);
}

// Fills a U8 buffer with a deterministic ramp. Used both as the fixture pixels
// and as the seeded cache payload.
bool fillRamp(const ImageBufferPtr& buf, unsigned step) {
    if (!buf || buf->format() != PixelFormat::U8 || buf->empty())
        return false;
    auto& px = buf->data();
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = static_cast<uint8_t>((i * step) & 0xff);
    return true;
}

// Engine objects + a running worker thread for one 1:1 entry case.
struct FullResHarness {
    std::filesystem::path dir;
    std::filesystem::path image;
    std::shared_ptr<ColorManager> cm;
    std::unique_ptr<Pipeline> pipeline;
    std::unique_ptr<RawDecoder> decoder;
    std::unique_ptr<ThumbnailCache> cache;
    std::unique_ptr<CatalogDatabase> db;
    QThread thread;
    PreviewWorker* worker = nullptr;
    int64_t imageId = -1;
    bool ok = false;

    ~FullResHarness() { stop(); }

    void start(const std::filesystem::path& root) {
        std::error_code ec;
        dir = root;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "cache", ec);
        image = dir / "fixture.jpg";
        if (!writeFullResFixture(image))
            return;
        cm = std::make_shared<ColorManager>();
        pipeline = std::make_unique<Pipeline>(cm);
        decoder = std::make_unique<RawDecoder>();
        decoder->setColorManager(cm);
        cache = std::make_unique<ThumbnailCache>(dir / "cache");
        db = std::make_unique<CatalogDatabase>(dir / "fullres.db");
        if (!db->open())
            return;
        imageId = db->addImage(image.string(), std::string());
        if (imageId <= 0)
            return;
        thread.setObjectName(QStringLiteral("perf1a_fullres"));
        worker = new PreviewWorker(*decoder, *pipeline, *cache, *db, cm);
        worker->moveToThread(&thread);
        thread.start();
        ok = true;
    }

    void stop() {
        if (!worker)
            return;
        thread.quit();
        thread.wait();
        delete worker;
        worker = nullptr;
    }
};

struct FullResTiming {
    // Wall time from request to the fullResReady signal, or -1 on timeout. This
    // includes the queued UI -> worker hop, which is part of what the anchor
    // measures; it excludes the GUI presentation step (MainWindow's t7).
    qint64 ms = -1;
    lrc::ImageBufferPtr frame;
};

// Requests one 1:1 entry and waits for the resulting frame.
FullResTiming timeFullResEntry(FullResHarness& h, qint64 generation) {
    FullResTiming out;
    QSignalSpy readySpy(h.worker, &PreviewWorker::fullResReady);
    QElapsedTimer clock;
    clock.start();
    QMetaObject::invokeMethod(
        h.worker, "loadFullRes", Qt::QueuedConnection, Q_ARG(qint64, h.imageId),
        Q_ARG(QString, QString::fromStdWString(h.image.wstring())),
        Q_ARG(qint64, generation),
        Q_ARG(lrc::AdjustmentsPtr,
              std::make_shared<const lrc::Adjustments>(
                  lrc::Adjustments::defaults())));
    if (!readySpy.wait(30000))
        return out;  // ms stays -1: no frame (timeout or loadFailed).
    out.ms = clock.elapsed();
    out.frame = readySpy.at(0).at(1).value<lrc::ImageBufferPtr>();
    return out;
}

} // namespace

class FullResHotPathTest : public QObject {
    Q_OBJECT
private slots:
    void embeddedPreviewHitIsTimed();
    void diskCacheHitIsTimed();
    void repeatRequestHitsEarlyReturnWithoutRender();
};

void FullResHotPathTest::embeddedPreviewHitIsTimed() {
    const auto root =
        std::filesystem::temp_directory_path() / "lrc_perf1a_embedded";
    FullResHarness h;
    h.start(root);
    QVERIFY(h.ok);

    // Seed ONLY the catalog-embedded 1:1 preview: the fast path must serve it
    // without decoding the source file and without running the pipeline.
    auto preview = ImageBuffer::create(256, 192, PixelFormat::U8);
    QVERIFY(fillRamp(preview, 5));
    const auto jpeg = ImageIO::encodeJpeg(*preview, 92);
    QVERIFY(!jpeg.empty());
    QVERIFY(h.db->storePreview(h.imageId, PreviewKind::FullRes, jpeg, 256, 192));

    const auto first = timeFullResEntry(h, 1);
    QVERIFY2(first.ms >= 0, "no frame arrived for the embedded fast path");
    QVERIFY(first.frame && !first.frame->empty());
    qInfo("[PERF1A][test] hit=embedded request->frame %lld ms",
          static_cast<long long>(first.ms));

    // Repeating the request keeps using the catalog blob: the embedded fast path
    // never populates the linear buffer, so no early-return branch is involved.
    const auto again = timeFullResEntry(h, 2);
    QVERIFY2(again.ms >= 0, "the repeated embedded request produced no frame");
    QVERIFY(again.frame && !again.frame->empty());
    qInfo("[PERF1A][test] hit=embedded (repeat) request->frame %lld ms",
          static_cast<long long>(again.ms));
}

void FullResHotPathTest::diskCacheHitIsTimed() {
    const auto root = std::filesystem::temp_directory_path() / "lrc_perf1a_disk";
    FullResHarness h;
    h.start(root);
    QVERIFY(h.ok);

    // Seed ONLY the legacy disk cache: no catalog-embedded preview exists, so the
    // request has to be answered by the disk JPEG (still without a raw decode).
    auto seeded = ImageBuffer::create(256, 192, PixelFormat::U8);
    QVERIFY(fillRamp(seeded, 7));
    QVERIFY(h.cache->writeFullRes(h.image, *seeded));
    QVERIFY(h.cache->findFullRes(h.image).has_value());

    const auto first = timeFullResEntry(h, 1);
    QVERIFY2(first.ms >= 0, "no frame arrived for the disk-cache fast path");
    QVERIFY(first.frame && !first.frame->empty());
    qInfo("[PERF1A][test] hit=disk request->frame %lld ms",
          static_cast<long long>(first.ms));
}

void FullResHotPathTest::repeatRequestHitsEarlyReturnWithoutRender() {
    const auto root = std::filesystem::temp_directory_path() / "lrc_perf1a_cold";
    FullResHarness h;
    h.start(root);
    QVERIFY(h.ok);

    // Neither cache is seeded, so the first entry takes the decode + render leg --
    // the only leg that leaves a resident linear buffer behind.
    const auto cold = timeFullResEntry(h, 1);
    QVERIFY2(cold.ms >= 0, "the cold 1:1 entry produced no frame");
    QVERIFY(cold.frame && !cold.frame->empty());
    qInfo("[PERF1A][test] cold leg (decode + render, small fixture) %lld ms "
          "(not a 50MP claim)",
          static_cast<long long>(cold.ms));

    // Identical repeat: the worker still holds the linear buffer and the frame
    // key, so it returns without rendering and without re-emitting a frame.
    //
    // BlockingQueuedConnection makes the call synchronous with the worker, so the
    // "no frame" assertion below needs no fixed sleep to be meaningful: by the
    // time invokeMethod() returns, the slot has run to completion. A qWait() here
    // would only make the check weaker (a frame arriving after the wait would
    // still pass it).
    QSignalSpy readySpy(h.worker, &PreviewWorker::fullResReady);
    QMetaObject::invokeMethod(
        h.worker, "loadFullRes", Qt::BlockingQueuedConnection,
        Q_ARG(qint64, h.imageId),
        Q_ARG(QString, QString::fromStdWString(h.image.wstring())),
        Q_ARG(qint64, 2),
        Q_ARG(lrc::AdjustmentsPtr,
              std::make_shared<const lrc::Adjustments>(
                  lrc::Adjustments::defaults())));
    QCOMPARE(readySpy.count(), 0);
    qInfo("[PERF1A][test] repeat entry took hit=earlyReturn: no render, no "
          "frame (excluded from the B-1 anchor verdict)");

    // Releasing the 1:1 buffer must make the next entry produce a frame again.
    // Blocking for the same reason: with a queued clear the next request could
    // race against it and observe the stale (early-return) state, turning the
    // assertion below into a 30 s timeout under load.
    QMetaObject::invokeMethod(h.worker, "clearFullRes",
                              Qt::BlockingQueuedConnection);
    const auto after = timeFullResEntry(h, 3);
    QVERIFY2(after.ms >= 0, "clearFullRes did not release the full-res state");
    QVERIFY(after.frame && !after.frame->empty());
    qInfo("[PERF1A][test] entry after clearFullRes %lld ms",
          static_cast<long long>(after.ms));
}

// ---------------------------------------------------------------------------
// ModelManager: integrity and archive hardening.
//
// These drive the real ModelManager (QtNetwork-backed) without touching the
// network: both cases below must be decided before any socket is opened, which
// is precisely the property the review asked for.
// ---------------------------------------------------------------------------
namespace {

// Minimal CRC-32 (IEEE) for the hand-built ZIP fixtures below.
uint32_t testCrc32(const std::string& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : data) {
        crc ^= static_cast<uint8_t>(ch);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

// Builds a one-entry ZIP with the *stored* (uncompressed) method, which is all
// the extractor under test has to understand for the fixture to be valid.
std::vector<uint8_t> makeStoredZip(const std::string& name,
                                   const std::string& payload) {
    const uint32_t crc = testCrc32(payload);
    const uint32_t size = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> out;
    // Local file header.
    put32(out, 0x04034b50u);
    put16(out, 20);  // version needed
    put16(out, 0);   // flags
    put16(out, 0);   // method: stored
    put16(out, 0);
    put16(out, 0);   // time / date
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, static_cast<uint16_t>(name.size()));
    put16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    const uint32_t dataOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), payload.begin(), payload.end());
    // Central directory.
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    put32(out, 0x02014b50u);
    put16(out, 20);  // version made by
    put16(out, 20);  // version needed
    put16(out, 0);
    put16(out, 0);   // method: stored
    put16(out, 0);
    put16(out, 0);   // time / date
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, static_cast<uint16_t>(name.size()));
    put16(out, 0);   // extra length
    put16(out, 0);   // comment length
    put16(out, 0);   // disk number start
    put16(out, 0);   // internal attributes
    put32(out, 0);   // external attributes
    put32(out, 0);   // local header offset
    out.insert(out.end(), name.begin(), name.end());
    const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOffset;
    // End of central directory.
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);   // entries on this disk
    put16(out, 1);   // total entries
    put32(out, cdSize);
    put32(out, cdOffset);
    put16(out, 0);   // comment length
    return out;
}

bool writeFileBytes(const std::filesystem::path& path,
                    const std::vector<uint8_t>& bytes) {
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os)
        return false;
    os.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
    return os.good();
}

}  // namespace

class ModelManagerTest : public QObject {
    Q_OBJECT
private slots:
    void placeholderShaIsRefusedWithoutOpeningASocket();
    void zipEntryEscapingTheTargetIsRejected();
};

void ModelManagerTest::placeholderShaIsRefusedWithoutOpeningASocket() {
    const auto dir = std::filesystem::temp_directory_path() / "lrc_mm_placeholder";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto manifest = dir / "manifest.json";
    {
        std::ofstream os(manifest, std::ios::binary | std::ios::trunc);
        os << "{\"models\":[{\"id\":\"lrc-placeholder\",\"name\":\"P\","
              "\"filename\":\"lrc-test-placeholder-unused.onnx\","
              "\"download_url\":\"https://invalid.example/lrc-test-placeholder-unused.onnx\","
              "\"sha256\":\"TBD_ON_FIRST_UPLOAD\",\"size_bytes\":0}]}";
    }
    ModelManager mm(manifest);
    const auto entry = mm.findModel("lrc-placeholder");
    QVERIFY(entry.has_value());
    QVERIFY2(!mm.isModelReady(*entry),
             "the fixture model must not exist in the real cache");

    bool called = false;
    bool finished = false;
    std::string error;
    mm.downloadModel(*entry, [&](uint64_t, uint64_t, bool done,
                                 const std::string& msg) {
        called = true;
        finished = done;
        error = msg;
    });
    QVERIFY2(called && finished,
             "an entry without a usable SHA256 must fail synchronously");
    QVERIFY2(error.find("no verifiable SHA256") != std::string::npos,
             error.c_str());
}

void ModelManagerTest::zipEntryEscapingTheTargetIsRejected() {
    const auto dir = std::filesystem::temp_directory_path() / "lrc_mm_zip";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // A benign archive must still extract (positive control): the entry is
    // stored, so the extractor only has to copy it.
    const auto goodZip = dir / "good.zip";
    QVERIFY(writeFileBytes(goodZip, makeStoredZip("nested/model.onnx", "fake-onnx")));
    const auto goodTarget = dir / "out-good" / "model.onnx";
    std::string goodErr;
    QVERIFY2(ModelManager::extractOnnxFromZip(goodZip, goodTarget, goodErr),
             goodErr.c_str());
    QVERIFY(std::filesystem::exists(goodTarget));

    // A traversal entry must be refused before anything is written. Note that
    // QZipReader::fileInfoList() reports this entry as "escaped.onnx" (Qt
    // strips the leading ../) while extractAll() writes with the raw name, so
    // the check has to come from the archive's own directory records.
    const auto badZip = dir / "bad.zip";
    QVERIFY(writeFileBytes(badZip, makeStoredZip("../escaped.onnx", "fake-onnx")));
    const auto badTarget = dir / "out-bad" / "model.onnx";
    std::string badErr;
    QVERIFY2(!ModelManager::extractOnnxFromZip(badZip, badTarget, badErr),
             "a ZIP entry with .. must be rejected");
    QVERIFY2(badErr.find("escapes") != std::string::npos, badErr.c_str());
    QVERIFY(!std::filesystem::exists(dir / "escaped.onnx"));
    QVERIFY(!std::filesystem::exists(dir / "out-bad" / "escaped.onnx"));
}

// ---------------------------------------------------------------------------
// Mouse wheel must not rewrite values it merely passes over
//
// The develop panel is ~3000px tall, so reaching a section means scrolling past
// dozens of sliders and spin boxes. The guarded widgets ignore wheel events
// (rather than consuming them) so Qt forwards the event to the enclosing scroll
// area: the panel scrolls, the adjustment stays put.
// ---------------------------------------------------------------------------
class WheelGuardTest : public QObject {
    Q_OBJECT
private slots:
    void sliderIgnoresWheel();
    void spinBoxesIgnoreWheel();
};

namespace {

// A single notch of wheel movement. Ignored by the guarded widgets; without the
// guard QAbstractSlider/QAbstractSpinBox would step the value.
QWheelEvent makeWheelNotch() {
    const QPointF pos(5, 5);
    return QWheelEvent(pos, pos, QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                       Qt::NoModifier, Qt::NoScrollPhase, false);
}

} // namespace

void WheelGuardTest::sliderIgnoresWheel() {
    NoWheelSlider slider;
    slider.setRange(0, 100);
    slider.setValue(50);

    QWheelEvent wheel = makeWheelNotch();
    QApplication::sendEvent(&slider, &wheel);

    QCOMPARE(slider.value(), 50);
    // Ignored, not consumed: an accepted event would stop the scroll area from
    // ever seeing it, trading one annoyance for another.
    QVERIFY(!wheel.isAccepted());
}

void WheelGuardTest::spinBoxesIgnoreWheel() {
    NoWheelDoubleSpinBox doubleSpin;
    doubleSpin.setRange(0.0, 100.0);
    doubleSpin.setValue(42.0);
    QWheelEvent doubleWheel = makeWheelNotch();
    QApplication::sendEvent(&doubleSpin, &doubleWheel);
    QCOMPARE(doubleSpin.value(), 42.0);
    QVERIFY(!doubleWheel.isAccepted());

    NoWheelSpinBox intSpin;
    intSpin.setRange(0, 100);
    intSpin.setValue(7);
    QWheelEvent intWheel = makeWheelNotch();
    QApplication::sendEvent(&intSpin, &intWheel);
    QCOMPARE(intSpin.value(), 7);
    QVERIFY(!intWheel.isAccepted());
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Same default as the application (see src/main.cpp): the [PERF1A]
    // diagnostics are opt-in, so a plain test run stays quiet. A
    // user-provided QT_LOGGING_RULES still wins.
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"))
        QLoggingCategory::setFilterRules(QStringLiteral("lrc.perf.info=false"));

    // Register the metatype used across the queued worker connection.
    qRegisterMetaType<QList<ExportItem>>("QList<ExportItem>");
    // IM-6: the edit-entry signals publish a shared immutable adjustment stack
    // and QSignalSpy captures it by metatype.
    qRegisterMetaType<lrc::AdjustmentsPtr>();
    // Soft proofing parameters cross the UI -> preview-worker queued invocation
    // and are captured by QSignalSpy in the dialog smoke test.
    qRegisterMetaType<lrc::SoftProofParams>();

    const auto root = std::filesystem::temp_directory_path() / "lrc_qt_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    int status = 0;
    {
        DialogSmokeTest t(root / "smoke");
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        PresetDialogSmokeTest t(root / "presets");
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        BatchExportTest t(root / "export");
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ImportWorkerTest t(root / "import_worker");
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ViewportViewStabilityTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        MaskOverlayRenderTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        AiModelAvailabilityTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ColorRangeSamplerTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        AdjustmentsSignalTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        SoftProofingDialogSmokeTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        DuplicateResultsDialogSmokeTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        IccJpegDecodabilityTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        SoftProofRenderPathsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        FullResHotPathTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ModelManagerTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        WheelGuardTest t;
        status |= QTest::qExec(&t, argc, argv);
    }

    std::filesystem::remove_all(root, ec);
    return status;
}

#include "lrc_qt_tests.moc"
