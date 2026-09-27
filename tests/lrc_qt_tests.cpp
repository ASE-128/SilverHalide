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
#include <QColor>
#include <QColorSpace>
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
        it.adjustments = Adjustments::defaults();
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
    const auto adj = appliedSpy.at(0).at(0).value<lrc::Adjustments>();
    QCOMPARE(adj.exposure, 0.5);
    QCOMPARE(adj.contrast, 25.0);

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
    const QString path = QString::fromStdWString(image.wstring());

    // Decode the fixture into the worker's working linear buffer (no catalog row
    // is needed: the standard-preview fast path just misses and it decodes).
    QSignalSpy previewSpy(h.worker, &PreviewWorker::previewReady);
    QMetaObject::invokeMethod(h.worker, "loadImage", Qt::QueuedConnection,
                              Q_ARG(qint64, 1), Q_ARG(QString, path),
                              Q_ARG(qint64, 1), Q_ARG(lrc::Adjustments, adj));
    QTRY_COMPARE_WITH_TIMEOUT(previewSpy.count(), 1, 60000);

    QSignalSpy coarseSpy(h.worker, &PreviewWorker::previewReadyCoarse);

    // Baseline: an unarmed coarse frame carries no alarm colour.
    QMetaObject::invokeMethod(h.worker, "adjustPreviewCoarse",
                              Qt::QueuedConnection, Q_ARG(qint64, 1),
                              Q_ARG(qint64, 2), Q_ARG(lrc::Adjustments, adj));
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
                              Q_ARG(qint64, 3), Q_ARG(lrc::Adjustments, adj));
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
    const QString path = QString::fromStdWString(image.wstring());
    QSignalSpy fullSpy(h.worker, &PreviewWorker::fullResReady);

    // Armed: the 1:1 frame itself must be proofed (MainWindow drops the
    // preview-sized proofReady while the viewport shows full resolution).
    QMetaObject::invokeMethod(h.worker, "setProofState", Qt::QueuedConnection,
                              Q_ARG(lrc::SoftProofParams, alarmProofParams()),
                              Q_ARG(bool, true));
    QMetaObject::invokeMethod(h.worker, "loadFullRes", Qt::QueuedConnection,
                              Q_ARG(qint64, 1), Q_ARG(QString, path),
                              Q_ARG(qint64, 1), Q_ARG(lrc::Adjustments, adj));
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
                              Q_ARG(lrc::Adjustments, adj));
    QTRY_COMPARE_WITH_TIMEOUT(fullSpy.count(), 2, 120000);
    auto plain = fullSpy.at(1).at(1).value<lrc::ImageBufferPtr>();
    QVERIFY(plain && !plain->empty());
    QCOMPARE(countAlarmPixels(plain), 0);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Register the metatype used across the queued worker connection.
    qRegisterMetaType<QList<ExportItem>>("QList<ExportItem>");
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
        AiModelAvailabilityTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ColorRangeSamplerTest t;
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

    std::filesystem::remove_all(root, ec);
    return status;
}

#include "lrc_qt_tests.moc"