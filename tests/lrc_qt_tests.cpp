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
#include <QEventLoop>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
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
#include "ui/ExportDialog.h"
#include "ui/ImportDialog.h"
#include "ui/PresetDialog.h"
#include "ui/PreviewWorker.h"
#include "ui/SnapshotDialog.h"
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
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // Register the metatype used across the queued worker connection.
    qRegisterMetaType<QList<ExportItem>>("QList<ExportItem>");

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

    std::filesystem::remove_all(root, ec);
    return status;
}

#include "lrc_qt_tests.moc"