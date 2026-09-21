#include "ui/MainWindow.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QByteArray>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QtGlobal>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/Application.h"
#include "app/Constants.h"
#include "catalog/CatalogDatabase.h"
#include "catalog/FolderMonitor.h"
#include "catalog/ThumbnailCache.h"
#include "engine/ColorManager.h"
#include "engine/DcpProfile.h"
#include "engine/LensfunProfile.h"
#include "engine/ImageIO.h"
#include "engine/Pipeline.h"
#include "engine/RawDecoder.h"
#include "engine/Xmp.h"
#include "engine/XmpSidecar.h"
// AI inference wiring. ModelManager reads the bundled manifest
// and probes <applicationDirPath>/models (read-only, ships with the installer)
// before the per-user cache, so an installed copy finds its bundled models
// without a re-download. AiInferenceEngine wraps ONNX Runtime sessions
// (per-thread) and is exposed to Pipeline via setGlobalAiEngine.
#include "app/ModelManager.h"
#include "engine/AiInference.h"
#include "ui/CollectionsDialog.h"
#include "ui/DevelopPanel.h"
#include "ui/EditHistoryDialog.h"
#include "ui/ExportDialog.h"
#include "ui/SnapshotDialog.h"
#include "ui/FilmstripWidget.h"
#include "ui/HdrMergeDialog.h"
#include "ui/HistogramWidget.h"
#include "ui/ImageViewport.h"
#include "ui/ImportDialog.h"
#include "ui/ImportWorker.h"
#include "ui/ModelManagementWidget.h"
#include "ui/NavigatorWidget.h"
#include "ui/PanoramaDialog.h"
#include "ui/PreviewWorker.h"
#include "ui/SmartCollectionsDialog.h"
#include "utils/MathUtils.h"
#include "utils/ProfileIO.h"
#include "utils/RenameTemplate.h"
#include "utils/XmpProfile.h"

namespace {

// Filmstrip lazy-thumbnail requests not yet dispatched. When the queue hits
// this bound the oldest pending request is dropped, so a huge filmstrip cannot
// build an unbounded decode backlog.
constexpr int kMaxPendingThumbnails = 64;
// Close-time worker shutdown. abort() + quit() is requested first, then
// this poll interval nudges the still-running threads until every one is gone.
constexpr int kWorkerStopPollMs = 100;
// After requesting abort, wait this long for the workers to finish before
// falling back to a blocking join (the only safe last resort -- a worker that
// is still inside a slot holds references into MainWindow-owned members).
constexpr int kWorkerStopWaitMs = 20000;

std::filesystem::path toStdPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

QString fromStdPath(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

// Local "YYYY-MM-DD" for the export {date} token (an export happens today).
std::string todayString() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

QStringList toQStringList(const std::vector<std::string>& values) {
    QStringList out;
    out.reserve(static_cast<int>(values.size()));
    for (const std::string& s : values)
        out.append(QString::fromStdString(s));
    return out;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    colorManager_ = std::make_shared<lrc::ColorManager>();
    // Lets the decoder convert imported JPEG/PNG/TIFF into linear ProPhoto.
    decoder_.setColorManager(colorManager_);
    pipeline_ = std::make_unique<lrc::Pipeline>(colorManager_);

    // AI engine + model manager. The bundled installer ships models at
    // <applicationDirPath>/models; ModelManager probes that directory via
    // readPathFor() before the per-user cache, so an installed
    // copy finds its models without triggering a re-download. Each ready
    // model is registered with the engine (per-thread ONNX sessions). The
    // engine is then exposed process-wide via Pipeline::setGlobalAiEngine
    // so the static applySpatialAdjustments / applyLocalAdjustments entry
    // points can reach it. When LRC_ENABLE_AI is off (CMake toggle) or the
    // bundled manifest is missing, the engine stays null and Pipeline
    // silently falls back to its pure-C++ NR / heuristic mask paths.
    initializeAiEngine();

    // Catalog path: honour a previously chosen catalog (QSettings), otherwise
    // fall back to the default location. Persist the path once the catalog
    // opens so "new/open catalog" survives a restart.
    QSettings settings;
    QString catalogPath = settings.value(QStringLiteral("catalogPath")).toString();
    if (catalogPath.isEmpty())
        catalogPath = Application::defaultCatalogPath();
    catalogPath_ = catalogPath;
    // The stored catalog location may be a folder (new layout) or a lone legacy
    // .lrcat file. Resolve the SQLite library file and preview side-directory
    // from it; CatalogDatabase externalizes previews to disk.
    catalog_ = std::make_unique<lrc::CatalogDatabase>(
        toStdPath(Application::catalogDbFile(catalogPath)),
        toStdPath(Application::catalogPreviewDir(catalogPath)));
    if (!catalog_->open()) {
        QMessageBox::critical(
            this, tr("目录错误"),
            tr("无法打开目录数据库: %1")
                .arg(QString::fromStdString(catalog_->lastError())));
    } else {
        settings.setValue(QStringLiteral("catalogPath"), catalogPath);
        catalogName_ = QFileInfo(catalogPath_).fileName();
    }
    thumbnailCache_ = std::make_unique<lrc::ThumbnailCache>(
        toStdPath(Application::defaultCacheDir()));

    // Managed-library root: stored in the catalog settings so re-opening the
    // same catalog always restores the same library location. Defaults to a
    // "Library" folder next to the catalog file.
    const auto storedRoot = catalog_->getSetting("library_root");
    if (storedRoot && !storedRoot->empty()) {
        libraryRoot_ = QString::fromStdString(*storedRoot);
    } else {
        // For a folder-as-library catalog the managed library lives *inside* the
        // catalog folder; for a lone legacy .lrcat it sits next to the file (its
        // containing directory).
        const QString container = QFileInfo(catalogPath).isDir()
                                      ? catalogPath
                                      : QFileInfo(catalogPath).absolutePath();
        libraryRoot_ = container + QStringLiteral("/Library");
        QDir().mkpath(libraryRoot_);
        catalog_->setSetting("library_root", libraryRoot_.toStdString());
    }

    setWindowTitle(tr("%1 — %2 — %3")
                       .arg(QString::fromLatin1(lrc::app::kAppName),
                            QString::fromLatin1(lrc::app::kAppVersion),
                            catalogName_));
    resize(1320, 820);

    buildUi();
    buildMenus();
    buildToolbar();
    startWorkers();

    // Watch the last imported folder for new files.
    folderTimer_ = new QTimer(this);
    folderTimer_->setInterval(2000);
    connect(folderTimer_, &QTimer::timeout, this, &MainWindow::watchFolderPoll);

    // Histogram/status-bar updates coalesce to app::throttle::kHistogramThrottleMs.
    // The one-shot timer guarantees the last coarse frame of a throttle window
    // still lands. Children of `this`,
    // so they are destroyed with the window.
    histogramThrottle_ = new QTimer(this);
    histogramThrottle_->setSingleShot(true);
    histogramThrottle_->setInterval(lrc::app::throttle::kHistogramThrottleMs);
    connect(histogramThrottle_, &QTimer::timeout, this,
            &MainWindow::flushHistogramThrottle);
    histogramClock_.start();
    lastHistogramMs_ = -10000;  // force the first update through immediately

    // Adjustment persistence leaves the hot path and coalesces on
    // app::throttle::kAdjustmentSaveDebounceMs; the timer always writes the
    // latest value (flushPendingSave is idempotent).
    saveDebounceTimer_ = new QTimer(this);
    saveDebounceTimer_->setSingleShot(true);
    saveDebounceTimer_->setInterval(lrc::app::throttle::kAdjustmentSaveDebounceMs);
    connect(saveDebounceTimer_, &QTimer::timeout, this,
            &MainWindow::onSaveDebounceTimeout);

    // A spot gesture emits spotRemovalsEdited live while a circle is dragged and
    // once more on release, but has no separate "gesture finished" signal. A
    // short idle debounce turns the trailing emissions into one undo entry.
    spotCommitTimer_ = new QTimer(this);
    spotCommitTimer_->setSingleShot(true);
    spotCommitTimer_->setInterval(lrc::app::throttle::kSpotCommitDebounceMs);
    connect(spotCommitTimer_, &QTimer::timeout, this,
            &MainWindow::onSpotCommitTimeout);

    refreshFilmstrip();
    // Application-level key filter implements the bare-key shortcuts.
    qApp->installEventFilter(this);
    statusBar()->showMessage(tr("就绪"));
}

MainWindow::~MainWindow() {
    // The backup/optimize thread must finish before catalog_ (a member) is
    // destroyed -- joining here is the last chance to avoid a use-after-free.
    joinDbOpThread();
    stopWorkers();
    // Safety net -- a debounced edit that was never flushed (e.g. the window was
    // destroyed without a closeEvent) must still be persisted.
    flushPendingSave();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Persist any debounced adjustment before the window disappears so a 500ms-window
    // edit is never lost on close.
    flushPendingSave();
    // If a background worker is still running (e.g. a long mergeHdr / big decode /
    // import copy), defer the close until it has stopped. ~QThread aborts a
    // still-running thread, which would tear the worker down while it
    // holds references into MainWindow-owned members -- a crash on exit. The
    // repeating poll timer re-issues close() once every worker has finished.
    if (!allWorkersStopped()) {
        requestWorkersStop();
        if (workerStopPollTimer_ == nullptr) {
            workerStopPollTimer_ = new QTimer(this);
            workerStopPollTimer_->setInterval(kWorkerStopPollMs);
            connect(workerStopPollTimer_, &QTimer::timeout, this, [this]() {
                if (allWorkersStopped()) {
                    workerStopPollTimer_->stop();
                    close();  // 全部 worker 已停,重新发起关闭 (re-close now that
                              // every worker has finished).
                }
            });
        }
        if (!workerStopPollTimer_->isActive()) {
            workerStopPollTimer_->start();
            statusBar()->showMessage(tr("正在停止后台任务…"), 0);
        }
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
    navigator_ = new NavigatorWidget(this);
    viewport_ = new ImageViewport(this);
    developPanel_ = new DevelopPanel(this);
    // The G/D/E module-focus shortcuts require the panels to accept focus.
    navigator_->setFocusPolicy(Qt::StrongFocus);
    developPanel_->setFocusPolicy(Qt::StrongFocus);

    auto* filmstripContainer = new QWidget(this);
    auto* fl = new QVBoxLayout(filmstripContainer);
    fl->setContentsMargins(0, 0, 0, 0);
    auto* filmLabel = new QLabel(tr("图库"), filmstripContainer);
    filmLabel->setProperty("caption", true);
    fl->addWidget(filmLabel);
    filmstrip_ = new FilmstripWidget(filmstripContainer);
    filmstrip_->setFixedHeight(150);
    fl->addWidget(filmstrip_);

    auto* center = new QSplitter(Qt::Vertical, this);
    center->addWidget(viewport_);
    center->addWidget(filmstripContainer);
    center->setStretchFactor(0, 1);
    center->setStretchFactor(1, 0);
    center->setSizes({600, 160});

    // The develop panel scrolls when the window is short.
    developScroll_ = new QScrollArea(this);
    developScroll_->setWidgetResizable(true);
    developScroll_->setFrameShape(QFrame::NoFrame);
    developScroll_->setWidget(developPanel_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(navigator_);
    splitter->addWidget(center);
    splitter->addWidget(developScroll_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({180, 780, 320});
    setCentralWidget(splitter);

    connect(filmstrip_, &FilmstripWidget::selectionChanged, this,
            &MainWindow::onImageSelected);
    // Virtual-copy rows also fire selectionChanged; onImageSelected ignores variant
    // rows (it checks currentVariantId_) so this signal is the authoritative load
    // for copies (variantId != 0).
    connect(filmstrip_, &FilmstripWidget::imageVariantSelected, this,
            &MainWindow::onImageVariantSelected);
    connect(filmstrip_, &FilmstripWidget::ratingRequested, this,
            &MainWindow::onSetRating);
    connect(filmstrip_, &FilmstripWidget::flagRequested, this,
            &MainWindow::onSetFlag);
    connect(filmstrip_, &FilmstripWidget::labelColorRequested, this,
            &MainWindow::onSetLabelColor);
    connect(filmstrip_, &FilmstripWidget::collectionRequested, this,
            &MainWindow::onCollectionToggle);
    connect(filmstrip_, &FilmstripWidget::removeRequested, this,
            &MainWindow::onRemoveImage);
    // The filmstrip asks for a thumbnail only when a visible row has none yet;
    // decode it on the background thumb loader thread, never on the UI
    // thread (writer-D contract: FilmstripWidget::thumbnailRequested(int)).
    connect(filmstrip_, &FilmstripWidget::thumbnailRequested, this,
            &MainWindow::onThumbnailRequested);
    connect(developPanel_, &DevelopPanel::adjustmentsChanged, this,
            &MainWindow::onAdjustmentsChanged);
    connect(developPanel_, &DevelopPanel::adjustmentsChangedCoarse, this,
            &MainWindow::onAdjustmentsChangedCoarse);
    connect(developPanel_, &DevelopPanel::adjustmentsCommitted, this,
            &MainWindow::onAdjustmentsCommitted);
        connect(developPanel_, &DevelopPanel::maskWindowRequested, this,
            &MainWindow::openMaskWindow);
    // Slider-drag gesture begin/end (currently just a flag; future work will
    // coalesce one drag into a single undo entry).
    connect(developPanel_, &DevelopPanel::editGestureStarted, this,
            &MainWindow::onEditGestureStarted);
    connect(developPanel_, &DevelopPanel::editGestureFinished, this,
            &MainWindow::onEditGestureFinished);
    connect(developPanel_, &DevelopPanel::autoRequested, this,
            &MainWindow::onAutoRequested);
    connect(developPanel_, &DevelopPanel::visualizationRequested, this,
            &MainWindow::onVisualizationRequested);
    connect(developPanel_, &DevelopPanel::visualizationCleared, this,
            &MainWindow::onVisualizationCleared);
    connect(developPanel_, &DevelopPanel::colorSamplerRequested, this,
            &MainWindow::onColorSamplerRequested);
    connect(viewport_, &ImageViewport::colorSampled, this,
            &MainWindow::onColorSampled);
    connect(viewport_, &ImageViewport::colorAdjusted, this,
            &MainWindow::onColorAdjusted);
    connect(viewport_, &ImageViewport::colorAdjustedFinished, this,
            &MainWindow::onColorAdjustedFinished);
    connect(developPanel_, &DevelopPanel::colorTargetSamplerRequested, this,
            &MainWindow::onColorTargetSamplerRequested);
    connect(viewport_, &ImageViewport::pointColorSampled, this,
            &MainWindow::onPointColorSampled);
    connect(viewport_, &ImageViewport::whiteBalanceSampled, this,
            &MainWindow::onWhiteBalanceSampled);
    connect(viewport_, &ImageViewport::colorRangeSampled, this,
            &MainWindow::onColorRangeSampled);
    // Range sub-mask samplers (colour + luminance sub-masks).
    connect(developPanel_, &DevelopPanel::colorRangeSubSamplerRequested,
            viewport_, &ImageViewport::setColorRangeSamplerActive);
    connect(viewport_, &ImageViewport::colorRangeSampled, this,
            [this](const QColor& c) {
                float lr, lg, lb;
                colorManager_->srgbPixelToLinearProPhoto(
                    static_cast<uint8_t>(c.red()),
                    static_cast<uint8_t>(c.green()),
                    static_cast<uint8_t>(c.blue()), lr, lg, lb);
                developPanel_->setColorRangeSubReference(c, lr, lg, lb);
            });
    connect(developPanel_, &DevelopPanel::luminanceSamplerRequested, viewport_,
            &ImageViewport::setLuminanceSamplerActive);
    connect(viewport_, &ImageViewport::luminanceSampled, developPanel_,
            &DevelopPanel::setLuminanceSubReference);

    // --- Spot removal -------------------------------------------------------------
    // Panel tool options -> viewport (mode / radius / visualization).
    connect(developPanel_, &DevelopPanel::spotModeChanged, viewport_,
            &ImageViewport::setSpotType);
    connect(developPanel_, &DevelopPanel::spotRadiusChanged, viewport_,
            &ImageViewport::setSpotRadius);
    connect(developPanel_, &DevelopPanel::visualizeSpotsToggled, viewport_,
            &ImageViewport::setSpotVisualizeVisible);
    // Viewport -> MainWindow: live spot-list edits and tool state changes.
    connect(viewport_, &ImageViewport::spotRemovalsEdited, this,
            &MainWindow::onSpotRemovalsEdited);
    connect(viewport_, &ImageViewport::spotToolActiveChanged, this,
            &MainWindow::onSpotToolActiveChanged);
    // Metadata edits (title/caption/copyright/creator/keywords) persist.
    connect(developPanel_, &DevelopPanel::metadataEdited, this,
            &MainWindow::onMetadataEdited);
    // The viewport disarms the spot tool internally when a sampler takes over
    // without emitting spotToolActiveChanged; uncheck the toolbar button there.
    connect(developPanel_, &DevelopPanel::colorRangeSubSamplerRequested, this,
            [this](bool active) {
                if (active)
                    disarmSpotToolButton();
            });
    connect(developPanel_, &DevelopPanel::luminanceSamplerRequested, this,
            [this](bool active) {
                if (active)
                    disarmSpotToolButton();
            });

    // --- masked local adjustments -----------------------------------------
    // The panel armed/selected a mask (or, with type None + index -1, asked to
    // disable the tool). Arm the viewport tool and show its draft overlay.
    connect(developPanel_, &DevelopPanel::maskEditRequested, this,
            [this](const lrc::Mask& mask, int index) {
                currentMaskIndex_ = index;
                if (mask.type == lrc::MaskType::None) {
                    viewport_->setMaskTool(lrc::MaskType::None);
                    return;
                }
                viewport_->setMaskTool(mask.type);
                viewport_->setMaskDraft(mask);
            });
    // The user dragged/painted in the viewport -> the stroke handler snapshots
    // the pre-stroke state once and feeds the geometry to the panel (which
    // preserves the local adjustment values and re-renders).
    connect(viewport_, &ImageViewport::maskGeometryChanged, this,
            &MainWindow::onMaskGeometryChanged);
    // Stroke end -> commit the whole stroke as one undo entry.
    connect(viewport_, &ImageViewport::maskEditFinished, this,
            &MainWindow::onMaskEditFinished);
    connect(developPanel_, &DevelopPanel::maskBrushSizeChanged, viewport_,
            &ImageViewport::setMaskBrushSize);
    // Brush softness follows the panel slider; also re-sent on tool arm / mask
    // selection so the viewport matches the panel value (0..1).
    connect(developPanel_, &DevelopPanel::maskSoftnessChanged, viewport_,
            &ImageViewport::setMaskBrushSoftness);
    // Click-prompt (MobileSAM) point mode follows the panel toggle.
    connect(developPanel_, &DevelopPanel::maskPromptLabelChanged, viewport_,
            &ImageViewport::setMaskPromptLabel);
    connect(developPanel_, &DevelopPanel::maskPromptClearRequested, this,
            &MainWindow::onMaskPromptClearRequested);

    connect(navigator_, &NavigatorWidget::allPhotosSelected, this,
            [this]() {
                clearImageFilters();
                refreshFilmstrip();
            });
    connect(navigator_, &NavigatorWidget::folderSelected, this,
            [this](const QString& folder) {
                setFilterFolder(folder);
                filterCollectionId_ = -1;
                filterKeywordId_ = -1;
                filterSmartCollectionId_ = -1;
                filterSearchText_.clear();
                if (searchDebounceTimer_)
                    searchDebounceTimer_->stop();
                if (searchEdit_) {
                    const QSignalBlocker blocker(searchEdit_);
                    searchEdit_->clear();
                }
                refreshFilmstrip();
            });
    connect(navigator_, &NavigatorWidget::collectionSelected, this,
            &MainWindow::onCollectionSelected);
    connect(navigator_, &NavigatorWidget::filtersChanged, this,
            &MainWindow::onFiltersChanged);
    // Navigator-driven selection signals.
    connect(navigator_, &NavigatorWidget::virtualCopySelected, this,
            &MainWindow::onVirtualCopySelected);
    connect(navigator_, &NavigatorWidget::keywordSelected, this,
            &MainWindow::onKeywordSelected);
    connect(navigator_, &NavigatorWidget::smartCollectionSelected, this,
            &MainWindow::onSmartCollectionSelected);
    connect(navigator_, &NavigatorWidget::newSmartCollectionRequested, this,
            &MainWindow::onNewSmartCollectionRequested);
}

void MainWindow::openMaskWindow() {
    if (maskWindow_ && maskWindow_->isVisible()) {
        maskWindow_->raise();
        maskWindow_->activateWindow();
        return;
    }
    if (!maskWindow_) {
        maskWindow_ = new QDialog(this);
        maskWindow_->setWindowTitle(tr("蒙版"));
        maskWindow_->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(maskWindow_, &QDialog::finished, this, [this]() {
            if (!maskWindow_)
                return;
            if (auto* layout = maskWindow_->layout())
                layout->removeWidget(developPanel_);
            developPanel_->setParent(developScroll_);
            developScroll_->setWidget(developPanel_);
            developPanel_->setSectionMode(0);
        });
    }
    developScroll_->takeWidget();
    developPanel_->setParent(maskWindow_);
    auto* layout = new QVBoxLayout(maskWindow_);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(developPanel_);
    maskWindow_->resize(420, 720);
    maskWindow_->show();
    maskWindow_->raise();
    maskWindow_->activateWindow();
}

void MainWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("文件"));
    fileMenu->addAction(tr("导入文件夹…"), this, &MainWindow::importFolder,
                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
    fileMenu->addAction(tr("导入文件…"), this, &MainWindow::importFiles,
                        QKeySequence::Open);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("导出…"), this, &MainWindow::exportImage,
                        QKeySequence(Qt::CTRL | Qt::Key_E));
    fileMenu->addAction(tr("转换为 DNG…"), this, &MainWindow::convertToDng);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("导入预设…"), QKeySequence(), this,
                        &MainWindow::importPreset);
    fileMenu->addAction(tr("导出预设…"), QKeySequence(), this,
                        &MainWindow::exportPreset);
    fileMenu->addAction(tr("快照…"), QKeySequence(), this,
                        &MainWindow::onManageSnapshots);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("退出"), this, &QWidget::close,
                        QKeySequence::Quit);

    QMenu* catalogMenu = menuBar()->addMenu(tr("目录"));
    catalogMenu->addAction(tr("新建目录…"), this, &MainWindow::newCatalog);
    catalogMenu->addAction(tr("打开目录…"), this, &MainWindow::openCatalog);
    auto* useDefault = catalogMenu->addAction(
        tr("切换到默认目录"), this, [this]() {
            const QString def = Application::defaultCatalogPath();
            QSettings settings;
            settings.setValue(QStringLiteral("catalogPath"), def);
            QProcess::startDetached(QCoreApplication::applicationFilePath());
            close();
        });
    // Disabled while the default catalog is already the active one.
    useDefault->setEnabled(
        QFileInfo(catalogPath_).canonicalFilePath() !=
        QFileInfo(Application::defaultCatalogPath()).canonicalFilePath());
    catalogMenu->addSeparator();
    catalogMenu->addAction(tr("备份目录…"), this, &MainWindow::backupCatalog);
    catalogMenu->addAction(tr("优化目录"), this, &MainWindow::optimizeCatalog);

    QMenu* photoMenu = menuBar()->addMenu(tr("照片"));
    photoMenu->addAction(tr("HDR 合成…"), this, &MainWindow::mergeHdr,
                         QKeySequence(Qt::CTRL | Qt::Key_H));
    photoMenu->addAction(tr("全景合成…"), this, &MainWindow::mergePanorama);
    photoMenu->addAction(tr("创建虚拟副本"), this, &MainWindow::createVirtualCopy);
    photoMenu->addAction(tr("参考视图…"), this, &MainWindow::toggleReferenceView);
    photoMenu->addAction(tr("编辑历史…"), this, &MainWindow::showEditHistory);
    photoMenu->addAction(tr("管理收藏夹…"), this,
                         &MainWindow::onManageCollections);
    photoMenu->addSeparator();
    photoMenu->addAction(tr("读取 sidecar"), QKeySequence(), this,
                         &MainWindow::readSidecar);
    photoMenu->addAction(tr("写入 sidecar"), QKeySequence(), this,
                         &MainWindow::writeSidecar);

    QMenu* viewMenu = menuBar()->addMenu(tr("视图"));
    viewMenu->addAction(tr("适合窗口"), this, [this]() {
        viewport_->setZoomMode(ImageViewport::ZoomMode::Fit);
    }, QKeySequence(Qt::CTRL | Qt::Key_0));
    viewMenu->addAction(tr("适合宽度"), this, [this]() {
        viewport_->setZoomMode(ImageViewport::ZoomMode::FitWidth);
    }, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_0));
    viewMenu->addAction(tr("1:1"), this, [this]() {
        viewport_->setZoomMode(ImageViewport::ZoomMode::OneToOne);
    }, QKeySequence(Qt::CTRL | Qt::Key_1));
    viewMenu->addSeparator();
    viewMenu->addAction(tr("放大"), viewport_, &ImageViewport::zoomIn,
                        QKeySequence::ZoomIn);
    viewMenu->addAction(tr("缩小"), viewport_, &ImageViewport::zoomOut,
                        QKeySequence::ZoomOut);

    QMenu* settingsMenu = menuBar()->addMenu(tr("设置"));
    settingsMenu->addAction(tr("模型管理…"), this, [this]() {
        ModelManagementWidget dlg(this);
        dlg.exec();
    });

    QMenu* helpMenu = menuBar()->addMenu(tr("帮助"));
    helpMenu->addAction(tr("关于"), this, [this]() {
        QMessageBox::about(
            this, tr("关于"),
            tr("%1 %2\n\n"
               "数字摄影非破坏性编辑工作流：RAW 解码 → 线性 ProPhoto 工作空间，\n"
               "参数堆叠，实时预览缓存。基于 C++20 与 Qt 6 构建。\n\n"
               "© ASE-128 2026")
                .arg(QString::fromLatin1(lrc::app::kAppName),
                     QString::fromLatin1(lrc::app::kAppVersion)));
    });

    // --- Edit/navigation shortcuts (modifier combos; bare keys live in eventFilter)
    // -----------------------------------------------------------------------------
    auto* undoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), this);
    connect(undoShortcut, &QShortcut::activated, this,
            &MainWindow::undoAdjustment);
    auto* redoShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), this);
    connect(redoShortcut, &QShortcut::activated, this,
            &MainWindow::redoAdjustment);
    auto* prevShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
    connect(prevShortcut, &QShortcut::activated, this,
            [this]() { navigateImages(-1); });
    auto* nextShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
    connect(nextShortcut, &QShortcut::activated, this,
            [this]() { navigateImages(1); });
    auto* resetShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this);
    connect(resetShortcut, &QShortcut::activated, this,
            [this]() { developPanel_->resetAll(); });
}

// --- Top toolbar -------------------------------------------------------------

void MainWindow::buildToolbar() {
    // Top toolbar. Two independent exclusive button groups mirror the
    // viewport zoom mode and the develop panel's active mask tool. The
    // viewport/panel are the source of truth; the toolbar is a mirror that
    // stays in sync via zoomModeChanged / maskToolActivated (see the two
    // on*Activated slots below). The groups are separate so each stays
    // exclusive within its own set of buttons.
    auto* toolBar = new QToolBar(tr("视图工具"), this);
    toolBar->setMovable(false);
    addToolBar(Qt::TopToolBarArea, toolBar);

    // --- view mode group: Fit / FitWidth / 1:1 ------------------------------
    viewButtonGroup_ = new QButtonGroup(this);
    viewButtonGroup_->setExclusive(true);
    auto makeViewButton = [&](const QString& text, const QString& tip,
                              ImageViewport::ZoomMode mode) {
        auto* button = new QToolButton(toolBar);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        toolBar->addWidget(button);
        // Reuse the enum value as the group id so the click handler maps a
        // button straight back to its zoom mode.
        viewButtonGroup_->addButton(button, static_cast<int>(mode));
    };
    makeViewButton(tr("适合窗口"), tr("适合窗口 (Ctrl+0)"),
                   ImageViewport::ZoomMode::Fit);
    makeViewButton(tr("适合宽度"), tr("适合宽度 (Ctrl+Alt+0)"),
                   ImageViewport::ZoomMode::FitWidth);
    makeViewButton(tr("1:1"), tr("1:1 (Ctrl+1 或 Z)"),
                   ImageViewport::ZoomMode::OneToOne);
    // Toolbar button -> viewport. buttonClicked fires on real clicks, never on
    // the setChecked sync path, so the two cannot form a loop.
    connect(viewButtonGroup_, &QButtonGroup::buttonClicked, this,
            [this](QAbstractButton* button) {
                viewport_->setZoomMode(static_cast<ImageViewport::ZoomMode>(
                    viewButtonGroup_->id(button)));
            });
    // Viewport -> toolbar mirror (wheel / double-click / Z / menu shortcuts all
    // route through zoomModeChanged, so the buttons stay in sync).
    connect(viewport_, &ImageViewport::zoomModeChanged, this,
            &MainWindow::onZoomModeChanged);

    // --- zoom in / out (non-checkable) --------------------------------------
    auto* zoomInAction =
        toolBar->addAction(tr("放大"), viewport_, &ImageViewport::zoomIn);
    zoomInAction->setToolTip(tr("放大 (Ctrl++)"));
    auto* zoomOutAction =
        toolBar->addAction(tr("缩小"), viewport_, &ImageViewport::zoomOut);
    zoomOutAction->setToolTip(tr("缩小 (Ctrl+-)"));
    toolBar->addSeparator();

    // --- mask tool group: linear / radial / brush / color ---------------------
    maskButtonGroup_ = new QButtonGroup(this);
    maskButtonGroup_->setExclusive(true);
    auto makeMaskButton = [&](const QString& text, const QString& tip,
                              lrc::MaskType tool) {
        auto* button = new QToolButton(toolBar);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        toolBar->addWidget(button);
        maskButtonGroup_->addButton(button, static_cast<int>(tool));
    };
    makeMaskButton(tr("线性"), tr("线性渐变 (Shift+M)"),
                   lrc::MaskType::LinearGradient);
    makeMaskButton(tr("径向"), tr("径向渐变 (M)"), lrc::MaskType::RadialGradient);
    makeMaskButton(tr("画笔"), tr("画笔 (K)"), lrc::MaskType::Brush);
    makeMaskButton(tr("颜色"), tr("颜色范围蒙版 (C)"), lrc::MaskType::ColorRange);
    // Toolbar button -> panel. selectMaskTool is the same public path the
    // shortcuts (K/M/Shift+M) use; its maskToolActivated echo is idempotent.
    connect(maskButtonGroup_, &QButtonGroup::buttonClicked, this,
            [this](QAbstractButton* button) {
                developPanel_->selectMaskTool(
                    static_cast<lrc::MaskType>(maskButtonGroup_->id(button)));
            });
    // Panel -> toolbar mirror (button path and shortcuts both emit
    // maskToolActivated).
    connect(developPanel_, &DevelopPanel::maskToolActivated, this,
            &MainWindow::onMaskToolActivated);

    // --- Spot removal tool --------------------------------------------------------
    toolBar->addSeparator();
    spotToolAction_ = toolBar->addAction(tr("污点"));
    spotToolAction_->setCheckable(true);
    spotToolAction_->setToolTip(tr("污点去除 / 修复画笔（与蒙版、吸管互斥）"));
    connect(spotToolAction_, &QAction::toggled, this, [this](bool checked) {
        if (checked) {
            viewport_->setSpotToolActive(true);
            // Disarm the mask tool: clearMaskTool emits maskToolActivated(None),
            // which unchecks the mask toolbar buttons (viewer-side exclusivity).
            developPanel_->clearMaskTool();
        } else {
            viewport_->setSpotToolActive(false);
        }
    });

    // --- Before/after compare mode group ------------------------------------------
    toolBar->addSeparator();
    compareButtonGroup_ = new QButtonGroup(this);
    compareButtonGroup_->setExclusive(true);
    auto makeCompareButton = [&](const QString& text, const QString& tip,
                                 ImageViewport::CompareMode mode) {
        auto* button = new QToolButton(toolBar);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        toolBar->addWidget(button);
        compareButtonGroup_->addButton(button, static_cast<int>(mode));
    };
    makeCompareButton(tr("关"), tr("退出比较视图"),
                      ImageViewport::CompareMode::Off);
    makeCompareButton(tr("左右"), tr("左右分屏（\\ 切换）"),
                      ImageViewport::CompareMode::LeftRight);
    makeCompareButton(tr("上下"), tr("上下分屏"),
                      ImageViewport::CompareMode::TopBottom);
    makeCompareButton(tr("单图"), tr("单图切换（\\ 前后对比）"),
                      ImageViewport::CompareMode::SingleToggle);
    // Toolbar button -> viewport. The mirror path goes through compareModeChanged
    // (blocked below), so the two cannot loop.
    connect(compareButtonGroup_, &QButtonGroup::buttonClicked, this,
            [this](QAbstractButton* button) {
                viewport_->setBeforeAfterMode(compareButtonGroup_->id(button));
            });
    connect(viewport_, &ImageViewport::compareModeChanged, this,
            &MainWindow::onCompareModeChanged);

    // --- Library search box -------------------------------------------------------
    toolBar->addSeparator();
    searchEdit_ = new QLineEdit(toolBar);
    searchEdit_->setPlaceholderText(tr("搜索照片…"));
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMaximumWidth(220);
    toolBar->addWidget(searchEdit_);
    searchDebounceTimer_ = new QTimer(this);
    searchDebounceTimer_->setSingleShot(true);
    searchDebounceTimer_->setInterval(lrc::app::throttle::kSearchDebounceMs);
    connect(searchDebounceTimer_, &QTimer::timeout, this,
            &MainWindow::applySearchFilter);
    connect(searchEdit_, &QLineEdit::returnPressed, this,
            &MainWindow::applySearchFilter);
    connect(searchEdit_, &QLineEdit::textChanged, this,
            [this](const QString&) {
                if (searchDebounceTimer_)
                    searchDebounceTimer_->start();
            });

    // Initial sync: reflect the current viewport mode. The mask starts at
    // None, which already matches the initially un-checked toolbar state.
    onZoomModeChanged(viewport_->zoomMode());
    onCompareModeChanged(viewport_->compareMode());
}

void MainWindow::onZoomModeChanged(ImageViewport::ZoomMode mode) {
    // Mirror the viewport's current zoom mode onto the toolbar. The blocker
    // guarantees setChecked can never bounce back into buttonClicked; the
    // mirror path is therefore one-way by construction.
    const QSignalBlocker blocker(viewButtonGroup_);
    if (QAbstractButton* button =
            viewButtonGroup_->button(static_cast<int>(mode)))
        button->setChecked(true);
}

void MainWindow::onMaskToolActivated(lrc::MaskType tool) {
    // Mirror the develop panel's active mask tool onto the toolbar. None
    // disarms every button: an exclusive group cannot reach the all-unchecked
    // state on its own, so drop exclusivity, clear, then restore it. The
    // blocker suppresses any signals this dance would otherwise emit.
    const QSignalBlocker blocker(maskButtonGroup_);
    if (tool == lrc::MaskType::None) {
        maskButtonGroup_->setExclusive(false);
        const auto buttons = maskButtonGroup_->buttons();
        for (QAbstractButton* button : buttons)
            button->setChecked(false);
        maskButtonGroup_->setExclusive(true);
        return;
    }
    if (QAbstractButton* button =
            maskButtonGroup_->button(static_cast<int>(tool)))
        button->setChecked(true);
    // A mask tool disarms the spot tool inside the viewport (setMaskTool sets
    // spotToolActive_ = false without emitting); mirror that on the toolbar.
    if (tool != lrc::MaskType::None)
        disarmSpotToolButton();
}

void MainWindow::newCatalog() {
    // The user picks a *folder*; the SQLite library file (<folder>/<folderName>.shcat)
    // and preview side-dir are created there on the next launch. A "catalog" is
    // now a directory, not a lone .lrcat file.
    const QString dir = Application::defaultCatalogPath();
    const QString path = QFileDialog::getExistingDirectory(
        this, tr("新建目录"), dir);
    if (path.isEmpty())
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("catalogPath"), path);
    // Restart so the (freshly created) catalog is opened from scratch.
    QProcess::startDetached(QCoreApplication::applicationFilePath());
    close();
}

void MainWindow::openCatalog() {
    // Select a catalog *folder*. A legacy lone .lrcat file is still openable — the
    // file dialog's filter accepts both so existing users can migrate their old
    // single-file catalogs in place.
    const QString dir = Application::defaultCatalogPath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开目录"), dir,
        tr("目录(*.shcat);;目录数据库 (*.lrcat)"));
    if (path.isEmpty())
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("catalogPath"), path);
    // Restart so the chosen catalog is loaded cleanly.
    QProcess::startDetached(QCoreApplication::applicationFilePath());
    close();
}

void MainWindow::backupCatalog() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("备份目录"), QDir::homePath() + QStringLiteral("/backup.shcat"),
        tr("目录 (*.shcat)"));
    if (path.isEmpty())
        return;
    if (dbOpRunning_) {
        statusBar()->showMessage(tr("目录操作正在进行,请稍候…"), 3000);
        return;
    }
    dbOpRunning_ = true;
    statusBar()->showMessage(tr("正在备份目录…"));
    // sqlite3_backup_step copies the whole catalog synchronously; on a large
    // library it freezes the UI for seconds. Run it on a std::thread and hop back
    // to the UI thread with the result (queued, so a closing window simply
    // discards the update).
    const std::filesystem::path dest = toStdPath(path);
    dbOpThread_ = std::thread([this, dest]() {
        const bool ok = catalog_->backup(dest);
        const QString error =
            ok ? QString() : QString::fromStdString(catalog_->lastError());
        QMetaObject::invokeMethod(this, "onCatalogBackupFinished",
                                  Qt::QueuedConnection, Q_ARG(bool, ok),
                                  Q_ARG(QString, error),
                                  Q_ARG(QString, fromStdPath(dest)));
    });
}

void MainWindow::onCatalogBackupFinished(bool ok, const QString& error,
                                         const QString& destPath) {
    dbOpRunning_ = false;
    if (dbOpThread_.joinable())
        dbOpThread_.detach();  // 线程已完成,仅回收句柄 (thread done; reclaim).
    if (ok)
        QMessageBox::information(this, tr("备份目录"),
                                 tr("目录已备份到 %1").arg(destPath));
    else
        QMessageBox::warning(this, tr("备份目录"),
                             tr("备份失败：%1").arg(error));
}

void MainWindow::optimizeCatalog() {
    if (dbOpRunning_) {
        statusBar()->showMessage(tr("目录操作正在进行,请稍候…"), 3000);
        return;
    }
    dbOpRunning_ = true;
    statusBar()->showMessage(tr("正在优化目录…"));
    // VACUUM rebuilds the catalog file and can take seconds on a large library;
    // run it off the UI thread (same pattern as backup).
    dbOpThread_ = std::thread([this]() {
        const bool ok = catalog_->optimize();
        const QString error =
            ok ? QString() : QString::fromStdString(catalog_->lastError());
        QMetaObject::invokeMethod(this, "onCatalogOptimizeFinished",
                                  Qt::QueuedConnection, Q_ARG(bool, ok),
                                  Q_ARG(QString, error));
    });
}

void MainWindow::onCatalogOptimizeFinished(bool ok, const QString& error) {
    dbOpRunning_ = false;
    if (dbOpThread_.joinable())
        dbOpThread_.detach();  // 线程已完成,仅回收句柄 (thread done; reclaim).
    if (ok)
        QMessageBox::information(this, tr("优化目录"), tr("目录已优化。"));
    else
        QMessageBox::warning(this, tr("优化目录"),
                             tr("优化失败：%1").arg(error));
}

void MainWindow::showEditHistory() {
    if (currentImageId_ < 0)
        return;
    EditHistoryDialog dlg(*catalog_, currentImageId_,
                          QFileInfo(currentFilePath_).fileName(), this);
    if (dlg.exec() == QDialog::Accepted) {
        if (auto adj = dlg.restored()) {
            // Restoring from the edit history is a user edit: it becomes an
            // undoable step (one push).
            commitAdjustments(*adj);
        }
    }
}

void MainWindow::startWorkers() {
    previewThread_ = new QThread(this);
    previewWorker_ = new PreviewWorker(decoder_, *pipeline_, *thumbnailCache_,
                                       *catalog_, colorManager_);
    previewWorker_->moveToThread(previewThread_);
    connect(previewThread_, &QThread::finished, previewWorker_,
            &QObject::deleteLater);
    connect(previewWorker_, &PreviewWorker::previewReady, this,
            &MainWindow::onPreviewReady);
    connect(previewWorker_, &PreviewWorker::previewReadyCoarse, this,
            &MainWindow::onPreviewReadyCoarse);
    connect(previewWorker_, &PreviewWorker::fullResReady, this,
            &MainWindow::onFullResReady);
    // 1:1 coarse intermediate frame (fast low-res feedback while a slider is dragged
    // in 1:1). Delivered ahead of fullResReady on the same channel shape.
    connect(previewWorker_, &PreviewWorker::fullResReadyCoarse, this,
            &MainWindow::onFullResReadyCoarse);
    connect(previewWorker_, &PreviewWorker::imageMeta, this,
            &MainWindow::onImageMeta);
        connect(previewWorker_, &PreviewWorker::cameraMeta, this,
            &MainWindow::onCameraMeta);
        connect(developPanel_, &DevelopPanel::dcpProfileSelected, this,
            &MainWindow::onDcpProfileSelected);
    connect(viewport_, &ImageViewport::fullResRequested, this,
            &MainWindow::onFullResRequested);
    // Preview-resolution rendering. The viewport reports its logical render size;
    // the worker switches to standard-preview rendering and the
    // current image is re-rendered at the new resolution when it changes.
    connect(viewport_, &ImageViewport::viewportResolutionChanged, previewWorker_,
            &PreviewWorker::setPreviewTargetSize, Qt::QueuedConnection);
    connect(viewport_, &ImageViewport::viewportResolutionChanged, this,
            [this](const QSize&) {
                if (currentImageId_ >= 0)
                    requestAdjust(currentAdjustments_);
            });
    connect(previewWorker_, &PreviewWorker::loadFailed, this,
            [this](const QString& path) {
                statusBar()->showMessage(tr("加载失败: %1").arg(path), 5000);
            });
    connect(previewWorker_, &PreviewWorker::exportFinished, this,
            &MainWindow::onExportFinished);
    // Batch-export status (master + virtual copies, rename template).
    connect(previewWorker_, &PreviewWorker::batchExportProgress, this,
            [this](int done, int total) {
                statusBar()->showMessage(
                    tr("正在批量导出 %1/%2（全尺寸渲染）…").arg(done).arg(total));
            });
    connect(previewWorker_, &PreviewWorker::batchExportFinished, this,
            [this](int success, int fail, const QString& message) {
                statusBar()->showMessage(
                    tr("批量导出完成：成功 %1，失败 %2。%3")
                        .arg(success)
                        .arg(fail)
                        .arg(message),
                    8000);
            });
    connect(previewWorker_, &PreviewWorker::autoAdjustReady, this,
            &MainWindow::onAutoAdjustReady);
    connect(previewWorker_, &PreviewWorker::visualizationReady, this,
            &MainWindow::onVisualizationReady);
    connect(previewWorker_, &PreviewWorker::hdrProgress, this,
            &MainWindow::onHdrProgress);
    connect(previewWorker_, &PreviewWorker::hdrFinished, this,
            &MainWindow::onHdrFinished);
    connect(previewWorker_, &PreviewWorker::convertFinished, this,
            &MainWindow::onConvertFinished);
    // Before/after + reference renders.
    connect(previewWorker_, &PreviewWorker::beforeAfterReady, this,
            &MainWindow::onBeforeAfterReady);
    connect(previewWorker_, &PreviewWorker::referenceReady, this,
            &MainWindow::onReferenceReady);
    previewThread_->start();

    importThread_ = new QThread(this);
    importWorker_ = new ImportWorker(*catalog_, *thumbnailCache_, decoder_,
                                     *pipeline_, colorManager_);
    importWorker_->moveToThread(importThread_);
    connect(importThread_, &QThread::finished, importWorker_,
            &QObject::deleteLater);
    connect(importWorker_, &ImportWorker::thumbReadyImage, this,
            &MainWindow::onThumbReadyImage);
    // Legacy fallback path (worker-side QImage decode failed) -- onThumbReady.
    connect(importWorker_, &ImportWorker::thumbReady, this,
            &MainWindow::onThumbReady);
    connect(importWorker_, &ImportWorker::progress, this,
            &MainWindow::onImportProgress);
    connect(importWorker_, &ImportWorker::finishedImport, this,
            &MainWindow::onImportFinished);
    connect(importWorker_, &ImportWorker::importWarning, this,
            &MainWindow::onImportWarning);
    importThread_->start();

    // A tiny dedicated thread for lazy filmstrip-thumbnail decode. The hub QObject
    // lives in this thread; the queued connection below runs the
    // decode lambda there and ships the QImage back to the UI thread via
    // invokeMethod (queued). One job is in flight at a time, so this thread
    // never competes with ImportWorker / PreviewWorker for CPU time.
    thumbLoaderThread_ = new QThread(this);
    thumbLoaderHub_ = new QObject;
    thumbLoaderHub_->moveToThread(thumbLoaderThread_);
    connect(thumbLoaderThread_, &QThread::finished, thumbLoaderHub_,
            &QObject::deleteLater);
    connect(this, &MainWindow::thumbDecodeRequested, thumbLoaderHub_,
            [this, db = catalog_.get()](int imageId,
                                        const QString& thumbnailPath) {
                QImage thumb;
                // Modern catalogs embed the thumbnail JPEG in the previews table.
                if (auto blob =
                        db->loadPreview(imageId, lrc::PreviewKind::Thumbnail)) {
                    thumb.loadFromData(
                        QByteArray(reinterpret_cast<const char*>(blob->data()),
                                   static_cast<int>(blob->size())));
                }
                // Legacy catalogs keep the thumbnail on disk.
                if (thumb.isNull() && !thumbnailPath.isEmpty())
                    thumb = QImage(thumbnailPath);
                if (!thumb.isNull())
                    QMetaObject::invokeMethod(this, "onThumbReadyImage",
                                              Qt::QueuedConnection,
                                              Q_ARG(int, imageId),
                                              Q_ARG(QImage, thumb));
                // Always report completion so the main thread dispatches the
                // next queued request even when the decode failed.
                QMetaObject::invokeMethod(this, "onThumbnailDecoded",
                                          Qt::QueuedConnection,
                                          Q_ARG(int, imageId));
            },
            Qt::QueuedConnection);
    thumbLoaderThread_->start();
}

void MainWindow::requestWorkersStop() {
    // Ask every worker to abort first: they bail at the next loop iteration /
    // slot entry. Then quit the event loops so the threads exit once the current
    // slot returns.
    if (importWorker_)
        importWorker_->requestAbort();
    if (previewWorker_)
        previewWorker_->requestAbort();
    if (importThread_)
        importThread_->quit();
    if (previewThread_)
        previewThread_->quit();
    if (thumbLoaderThread_)
        thumbLoaderThread_->quit();
}

bool MainWindow::allWorkersStopped() const {
    if (dbOpRunning_)
        return false;
    if (previewThread_ && previewThread_->isRunning())
        return false;
    if (importThread_ && importThread_->isRunning())
        return false;
    if (thumbLoaderThread_ && thumbLoaderThread_->isRunning())
        return false;
    return true;
}

void MainWindow::joinDbOpThread() {
    if (dbOpThread_.joinable())
        dbOpThread_.join();  // backup/optimize is done; reclaim the handle.
    // Once joined (or never started) the operation can no longer be in flight.
    dbOpRunning_ = false;
}

void MainWindow::stopWorkers() {
    requestWorkersStop();
    // Loop-wait with short intervals instead of one fixed 15s block: each
    // iteration waits on the threads that are still running, so the close is
    // never held hostage by a thread that has already finished. abort() makes
    // the worker return after the current file/decode; kWorkerStopWaitMs is a
    // generous budget for that.
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kWorkerStopWaitMs) {
        if (allWorkersStopped())
            break;
        if (previewThread_ && previewThread_->isRunning())
            previewThread_->wait(kWorkerStopPollMs);
        if (importThread_ && importThread_->isRunning())
            importThread_->wait(kWorkerStopPollMs);
        if (thumbLoaderThread_ && thumbLoaderThread_->isRunning())
            thumbLoaderThread_->wait(kWorkerStopPollMs);
    }
    // Blocking join is the only safe last resort for reference-holding workers;
    // detaching would leave dangling references into MainWindow-owned members.
    // Never destroy a running QThread or let a worker dereference freed members.
    if (!allWorkersStopped()) {
        qWarning("MainWindow: worker thread(s) still busy; blocking until they "
                 "finish.");
        if (previewThread_ && previewThread_->isRunning())
            previewThread_->wait();
        if (importThread_ && importThread_->isRunning())
            importThread_->wait();
        if (thumbLoaderThread_ && thumbLoaderThread_->isRunning())
            thumbLoaderThread_->wait();
    }
}

void MainWindow::importFolder() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择导入文件夹"), QDir::homePath());
    if (dir.isEmpty())
        return;
    QDir d(dir);
    QStringList files;
    for (const QFileInfo& fi :
         d.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase))
        if (lrc::FolderMonitor::isImportableFile(
                toStdPath(fi.absoluteFilePath())))
            files << fi.absoluteFilePath();
    if (files.isEmpty()) {
        statusBar()->showMessage(tr("该文件夹中没有可导入的图像文件"), 4000);
        return;
    }
    folderMonitor_ = std::make_unique<lrc::FolderMonitor>(toStdPath(dir));
    folderMonitor_->setCallback(
        [this](const auto& added, const auto& modified, const auto& removed) {
            if (!removed.empty()) {
                for (const auto& p : removed) {
                    if (auto rec =
                            catalog_->findImageByPath(lrc::pathToUtf8(p)))
                        catalog_->removeImage(rec->id);
                }
            }
            if (!modified.empty()) {
                // R-14: the source file was replaced in place (size/mtime
                // changed). Drop its cached previews so stale pixels are never
                // reused; the editor re-decodes from disk on next load.
                for (const auto& p : modified) {
                    if (auto rec =
                            catalog_->findImageByPath(lrc::pathToUtf8(p)))
                        catalog_->invalidatePreviews(rec->id);
                }
            }
            if (!added.empty()) {
                QStringList paths;
                for (const auto& p : added)
                    paths << fromStdPath(p);
                startImport(paths);
            }
            refreshFilmstrip();
        });
    folderMonitor_->start();
    folderTimer_->start();
    // Folder imports reference files in place (Add) — nothing is copied/moved.
    startImport(files, false, lrc::ImportMode::Add, QString());
}

void MainWindow::importFiles() {
    // Default Copy/Move destination is the managed-library root; the by-date
    // subfolder (<yyyy>/<yyyy-mm-dd>) is created by LibraryManager.
    const QString defaultDest = libraryRoot_;
    ImportDialog dlg(defaultDest, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList paths = dlg.selectedPaths();
    if (paths.isEmpty())
        return;
    const QString dest = dlg.destinationDir();
    startImport(paths, dlg.convertToDng(), dlg.importMode(),
                dest.isEmpty() ? libraryRoot_ : dest,
                dlg.renameEnabled() ? dlg.renameTemplate() : QString());
}

void MainWindow::startImport(const QStringList& paths, bool convertToDng,
                             lrc::ImportMode mode, const QString& destRoot,
                             const QString& renameTemplate) {
    if (paths.isEmpty())
        return;
    // Remember the folder for the navigation panel.
    const QString folder = QFileInfo(paths.first()).absolutePath();
    catalog_->addFolder(folder.toStdString());
    navigator_->setFolders(toQStringList(catalog_->listFolders()));
    if (convertToDng)
        statusBar()->showMessage(tr("正在导入 %1 个文件（先转换为 DNG）…")
                                     .arg(paths.size()));
    else
        statusBar()->showMessage(tr("正在导入 %1 个文件…").arg(paths.size()));
    QMetaObject::invokeMethod(importWorker_, "importPaths",
                              Qt::QueuedConnection,
                              Q_ARG(QStringList, paths),
                              Q_ARG(bool, convertToDng),
                              Q_ARG(lrc::ImportMode, mode),
                              Q_ARG(QString, destRoot),
                              Q_ARG(QString, renameTemplate));
}

void MainWindow::exportImage() {
    if (currentImageId_ < 0) {
        QMessageBox::information(this, tr("导出"), tr("请先在胶片栏选择一张图片。"));
        return;
    }
    const QString baseName = QFileInfo(currentFilePath_).baseName();
    // Default export path follows the source photo's directory.
    const QString suggested =
        QFileInfo(currentFilePath_).absolutePath() + "/" +
        ExportDialog::defaultFileName(baseName, 0);
    ExportDialog dlg(suggested, this);
    // Let the user export the master or any virtual copy. The selector defaults to
    // the master; track explicit picks via variantChosen so the
    // export uses what the user actually sees when they leave it alone.
    dlg.setVariants(catalog_->listVariants(currentImageId_));
    currentExportVariantId_ = currentVariantId_;
    connect(&dlg, &ExportDialog::variantChosen, this,
            [this](qint64 variantId) { currentExportVariantId_ = variantId; });
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString out = dlg.outputPath();
    if (out.isEmpty())
        return;
    // The export uses the selected variant's adjustment stack. When the user is
    // viewing that same variant, currentAdjustments_ is authoritative (it may
    // still carry un-flushed live edits); otherwise load the variant's stack.
    lrc::Adjustments exportAdj;
    const qint64 exportVariant =
        dlg.hasVariants() ? currentExportVariantId_ : 0;
    if (exportVariant == currentVariantId_)
        exportAdj = currentAdjustments_;
    else
        exportAdj =
            catalog_->loadAdjustments(currentImageId_,
                                      static_cast<int>(exportVariant))
                .value_or(currentAdjustments_);
    statusBar()->showMessage(tr("正在导出（全尺寸渲染）…"));
    const ExportWatermark wm = dlg.watermark();

    // Batch export of the master + every virtual copy whose outputs are named from
    // the rename template (instead of the single-variant legacy
    // export). The chosen folder is the output path's parent directory; each
    // rendered filename gets a numeric suffix if the on-disk name is taken.
    if (dlg.renameEnabled()) {
        const std::filesystem::path dir =
            toStdPath(QFileInfo(out).absolutePath());
        const std::string base =
            QFileInfo(currentFilePath_).completeBaseName().toStdString();
        static const char* suffixes[] = {".jpg", ".png", ".tif", ".tif",
                                         ".dng"};
        const int fi = dlg.formatIndex();
        const char* suffix = (fi >= 0 && fi < 5) ? suffixes[fi] : ".jpg";

        auto variants = catalog_->listVariants(currentImageId_);
        QList<ExportItem> items;
        items.reserve(static_cast<int>(variants.size()));
        const int total = static_cast<int>(variants.size());
        for (int i = 0; i < total; ++i) {
            const lrc::EditVariant& v = variants[i];
            // Resolve the variant's adjustment stack (current view is
            // authoritative when it matches; otherwise load from the catalog).
            lrc::Adjustments adj;
            if (v.variantId == currentVariantId_)
                adj = currentAdjustments_;
            else
                adj = catalog_
                          ->loadAdjustments(currentImageId_,
                                            static_cast<int>(v.variantId))
                          .value_or(currentAdjustments_);

            lrc::RenameTemplateArgs ra;
            ra.tmpl = dlg.renameTemplate().toStdString();
            ra.base = base;
            ra.index = i + 1;
            ra.indexWidth = 2;
            ra.date = todayString();
            const std::filesystem::path outPath =
                lrc::uniqueRenamePath(dir, ra, suffix);

            ExportItem item;
            item.imageId = currentImageId_;
            item.filePath = currentFilePath_;
            item.adjustments = adj;
            item.outPath = fromStdPath(outPath);
            item.format = static_cast<ExportFormat>(dlg.formatIndex());
            item.quality = dlg.jpegQuality();
            item.maxDim = dlg.maxDim();
            item.icc = dlg.iccIndex();
            item.watermark = wm;
            items.push_back(std::move(item));
        }
        statusBar()->showMessage(tr("正在批量导出 %1 个文件（全尺寸渲染）…")
                                     .arg(total));
        QMetaObject::invokeMethod(previewWorker_, "batchExport",
                                  Qt::QueuedConnection,
                                  Q_ARG(QList<ExportItem>, items));
        return;
    }

    QMetaObject::invokeMethod(
        previewWorker_, "exportImage", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(QString, currentFilePath_),
        Q_ARG(lrc::Adjustments, exportAdj), Q_ARG(QString, out),
        Q_ARG(int, dlg.formatIndex()), Q_ARG(int, dlg.jpegQuality()),
        Q_ARG(int, dlg.maxDim()), Q_ARG(int, dlg.iccIndex()),
        Q_ARG(ExportWatermark, wm));
}

void MainWindow::onManageSnapshots() {
    if (currentImageId_ < 0) {
        QMessageBox::information(this, tr("快照"),
                                 tr("请先在胶片栏选择一张图片。"));
        return;
    }
    SnapshotDialog dlg(*catalog_, currentImageId_, currentVariantId_, this);
    // Applying a snapshot is equivalent to restoring an undo step: reuse the
    // same panel/preview/persist path (without re-pushing the history). The
    // master variant is used for snapshots; virtual copies keep their own.
    connect(&dlg, &SnapshotDialog::applied, this,
            [this](const lrc::Adjustments& adjustments) {
                if (currentVariantId_ == 0)
                    applyAdjustmentState(adjustments);
                else if (catalog_->saveAdjustments(currentImageId_, adjustments,
                                                  currentVariantId_))
                    statusBar()->showMessage(tr("编辑已保存"), 2500);
            });
    dlg.exec();
}

void MainWindow::convertToDng() {
    if (currentImageId_ < 0 || currentFilePath_.isEmpty()) {
        QMessageBox::information(this, tr("转换为 DNG"),
                                 tr("请先在胶片栏选择一张 RAW 照片。"));
        return;
    }
    const QString suggested =
        QFileInfo(currentFilePath_).absolutePath() + "/" +
        QFileInfo(currentFilePath_).completeBaseName() + ".dng";
    const QString out = QFileDialog::getSaveFileName(
        this, tr("转换为 DNG"), suggested, tr("DNG 原始 (*.dng)"));
    if (out.isEmpty())
        return;
    statusBar()->showMessage(tr("正在转换为 DNG…"));
    QMetaObject::invokeMethod(previewWorker_, "convertToDng",
                              Qt::QueuedConnection,
                              Q_ARG(qint64, currentImageId_),
                              Q_ARG(QString, currentFilePath_),
                              Q_ARG(QString, out));
}

void MainWindow::onImageSelected(int imageId) {
    // A virtual-copy row also fires selectionChanged; the variant row is served
    // by onImageVariantSelected, so the master path ignores non-master rows to
    // avoid a redundant master load before the copy load.
    if (filmstrip_ && filmstrip_->currentVariantId() != 0)
        return;
    loadImageState(imageId, 0);
}

void MainWindow::loadImageState(int imageId, int variantId) {
    // The user is leaving the current image -- persist any debounced edit that is
    // still inside the 500ms save window so it is never lost.
    flushPendingSave();
    auto it = records_.constFind(imageId);
    QString filePath;
    if (it != records_.constEnd()) {
        filePath = QString::fromStdString(it->filePath);
    } else {
        // A virtual copy chosen from the Navigator may not be in the current
        // filtered list; fall back to a full catalog lookup for its path.
        for (const auto& rec : catalog_->listImages()) {
            if (rec.id == imageId) {
                filePath = QString::fromStdString(rec.filePath);
                break;
            }
        }
    }
    if (filePath.isEmpty())
        return;
    currentImageId_ = imageId;
    currentVariantId_ = variantId;
    currentFilePath_ = filePath;
    currentDcpProfilePath_.clear();
    developPanel_->setDcpProfiles({}, {});
    // Reset the worker's DCP selection on image switch so a profile chosen for the
    // previous camera never leaks into a new photo.
    const QString noDcp;
    QMetaObject::invokeMethod(previewWorker_, "setDcpProfile",
                              Qt::QueuedConnection,
                              Q_ARG(QString, noDcp));
    currentAdjustments_ =
        catalog_->loadAdjustments(imageId, variantId)
            .value_or(lrc::Adjustments::defaults());
    developPanel_->setAdjustments(currentAdjustments_);
    // Fresh undo history per image: the loaded initial state is the stack
    // bottom so edits can always be undone back to it.
    undoStack_.clear();
    undoStack_.push(currentAdjustments_);
    maskStrokeActive_ = false;  // a stale stroke from the previous image must not leak
    // A stale spot gesture must not leak into the new image.
    spotStrokeActive_ = false;
    if (spotCommitTimer_)
        spotCommitTimer_->stop();
    // A new image means new masks: disarm any mask tool the previous image had.
    currentMaskIndex_ = -1;
    developPanel_->clearMaskTool();
    viewport_->setMaskTool(lrc::MaskType::None);
    viewport_->setSamplerActive(false);
    viewport_->setDiagnosticOverlay(QImage());
    developPanel_->setSamplerChecked(false);
    // Reset the point-colour sampler on image change.
    viewport_->setPointColorSamplerActive(false);
    developPanel_->setColorTargetSamplerChecked(false);
    // Release the full-res buffer on image switch (the worker's loadImage also
    // releases defensively — double safety).
    clearFullRes();
    fullResActive_ = false;
    viewport_->setNativeImageSize(QSize());
    // The loaded spot list + count feed the viewport/panel; the tool type and radius
    // start from the panel's current values.
    syncSpotStateFromAdjustments();
    viewport_->setSpotType(developPanel_->spotMode());
    viewport_->setSpotRadius(developPanel_->spotRadius());
    // Populate the metadata panel + keyword chips.
    developPanel_->setMetadata(catalog_->getPhotoMetadata(imageId),
                               keywordFullPathsForImage(imageId));
    statusBar()->showMessage(
        tr("加载 %1 …").arg(QFileInfo(currentFilePath_).fileName()));
    requestPreview(currentFilePath_, currentAdjustments_);
}

void MainWindow::onCameraMeta(qint64 imageId, const QString& make,
                              const QString& model, const QString& lensMaker,
                              const QString& lensModel, float focalLengthMm,
                              float aperture, float cropFactor) {
    if (imageId != currentImageId_)
        return;
    refreshDcpProfiles(model);
    refreshLensfunProfile(make, model, lensMaker, lensModel, focalLengthMm,
                          aperture, cropFactor);
}

void MainWindow::refreshLensfunProfile(const QString& cameraMake,
                                       const QString& cameraModel,
                                       const QString& lensMaker,
                                       const QString& lensModel,
                                       float focalLengthMm, float aperture,
                                       float cropFactor) {
    if (!lrc::LensfunProfile::available()) {
        developPanel_->setLensfunProfile(tr("当前构建未启用 Lensfun"), false,
                                         false);
        return;
    }
    const auto profiles = lrc::LensfunProfile::find(
        cameraMake.toStdString(), cameraModel.toStdString(),
        lensMaker.toStdString(), lensModel.toStdString());
    if (profiles.empty()) {
        // No matching profile: force the correction off so a stale enabled
        // flag never keeps the stage running as a no-op. The identity fields
        // are left as-is; applyGeometry short-circuits when the lookup fails.
        if (currentAdjustments_.lensfunEnabled) {
            currentAdjustments_.lensfunEnabled = false;
            developPanel_->setAdjustments(currentAdjustments_);
            onAdjustmentsCommitted(currentAdjustments_);
        }
        developPanel_->setLensfunProfile(
            tr("未找到匹配：%1 / %2").arg(lensMaker, lensModel), false, false);
        return;
    }
    const auto& profile = profiles.front();
    // Only geometric distortion is actually applied by the pipeline stage.
    // Lensfun's physiological vignetting and chromatic aberration are NOT rendered,
    // so the UI must not advertise them as supported.
    const QString status = tr("已匹配：%1 @ %2mm f/%3（畸变 %4）")
        .arg(QString::fromStdString(profile.lensModel))
        .arg(QString::number(focalLengthMm, 'f', 1))
        .arg(QString::number(aperture, 'f', 1))
        .arg(profile.hasDistortion ? tr("支持") : tr("无"));
    // Populate the factual Lensfun identity + capture fields on the current
    // adjustment stack so the Pipeline stage has everything it needs. The
    // enabled flag defaults to true the first time a profile is detected for
    // this image (identity previously empty); afterwards the user's saved
    // on/off choice is respected across reloads.
    const bool firstDetection = currentAdjustments_.lensfunCameraMaker.empty();
    currentAdjustments_.lensfunCameraMaker = cameraMake.toStdString();
    currentAdjustments_.lensfunCameraModel = cameraModel.toStdString();
    currentAdjustments_.lensfunLensMaker = lensMaker.toStdString();
    currentAdjustments_.lensfunLensModel = lensModel.toStdString();
    currentAdjustments_.lensfunFocalLengthMm = focalLengthMm;
    currentAdjustments_.lensfunAperture = aperture;
    currentAdjustments_.lensfunCropFactor =
        cropFactor > 0.0f ? cropFactor : profile.cropFactor;
    if (firstDetection)
        currentAdjustments_.lensfunEnabled = true;
    developPanel_->setAdjustments(currentAdjustments_);
    developPanel_->setLensfunProfile(status, true,
                                     currentAdjustments_.lensfunEnabled);
    onAdjustmentsCommitted(currentAdjustments_);
}

void MainWindow::refreshDcpProfiles(const QString& cameraModel) {
    QStringList names;
    QStringList paths;
    QStringList roots;
    roots << QCoreApplication::applicationDirPath() + "/resources/Camera";
    roots << QCoreApplication::applicationDirPath() + "/../resources/Camera";
    for (const QString& root : roots) {
        const QDir dir(root + "/" + cameraModel);
        if (!dir.exists())
            continue;
        const QFileInfoList files = dir.entryInfoList(
            QStringList() << QStringLiteral("*.dcp"), QDir::Files,
            QDir::Name);
        for (const QFileInfo& file : files) {
            const auto profile = lrc::DcpProfileReader::read(
                toStdPath(file.absoluteFilePath()));
            if (!profile)
                continue;
            names.append(QString::fromStdString(profile->name));
            paths.append(file.absoluteFilePath());
        }
        if (!names.isEmpty())
            break;
    }
    developPanel_->setDcpProfiles(names, paths);
}

void MainWindow::onDcpProfileSelected(const QString& path) {
    currentDcpProfilePath_ = path;
    // Forward the selection to the worker thread so the next RAW decode injects the
    // profile's colour matrix / tone curve. The selection is stored
    // on the worker; any adjustment commit up to this point is harmless because
    // the DCP only takes effect at decode time.
    QMetaObject::invokeMethod(previewWorker_, "setDcpProfile",
                              Qt::QueuedConnection,
                              Q_ARG(QString, path));
    if (path.isEmpty()) {
        developPanel_->setDcpProfileStatus(tr("未加载 DCP"));
    } else {
        const auto profile = lrc::DcpProfileReader::read(toStdPath(path));
        if (!profile) {
            developPanel_->setDcpProfileStatus(tr("DCP 读取失败"));
        } else {
            developPanel_->setDcpProfileStatus(tr("已应用相机配置"));
        }
    }
    // A DCP change requires a full re-decode (the matrix/curve is baked at decode
    // time and the standard-preview cache is bypassed while a profile is
    // active), so re-issue the preview load rather than a re-render.
    if (currentImageId_ >= 0 && !currentFilePath_.isEmpty())
        requestPreview(currentFilePath_, currentAdjustments_);
}

void MainWindow::onImageVariantSelected(int64_t imageId, int variantId) {
    // Master rows (variant 0) are served by onImageSelected via selectionChanged;
    // this slot only loads actual virtual copies.
    if (variantId == 0)
        return;
    loadImageState(static_cast<int>(imageId), variantId);
}

void MainWindow::onAdjustmentsChanged(const lrc::Adjustments& adjustments) {
    currentAdjustments_ = adjustments;
    if (currentImageId_ >= 0) {
        // Keep the 1920 preview cache / histogram / exit-1:1 path warm.
        requestAdjust(adjustments);
        // While the 1:1 full-res view is live, also re-render it so the
        // full-res display stays in sync (no decode, just re-render).
        if (fullResActive_)
            requestAdjustFullRes();
    }
}

void MainWindow::onAdjustmentsChangedCoarse(
    const lrc::Adjustments& adjustments) {
    currentAdjustments_ = adjustments;
    if (currentImageId_ < 0)
        return;
    requestAdjustCoarse(adjustments);
    // While a slider is dragged in 1:1, feed the retained full-res buffer through
    // the coarse path too so the full-res view gets immediate low-res
    // feedback; the refine path (onAdjustmentsChanged) still renders the final
    // full-res frame on pause/release via requestAdjustFullRes.
    if (fullResActive_)
        requestAdjustFullResCoarse(adjustments);
}

void MainWindow::onAdjustmentsCommitted(const lrc::Adjustments& adjustments) {
    if (currentImageId_ < 0)
        return;
    if (maskStrokeActive_) {
        // Mid-stroke geometry updates: re-render but do NOT push an undo entry.
        // The whole stroke is committed as a single step at maskEditFinished
        // (one stroke = one undo). The DB write is coalesced into the 500ms save
        // debounce (pendingSave_ + saveDebounceTimer_) instead of being done
        // synchronously on every throttled commit; the stroke-end path
        // (onMaskEditFinished -> commitAdjustments) persists the final state.
        currentAdjustments_ = adjustments;
        developPanel_->setAdjustments(adjustments);
        requestAdjust(adjustments);
        if (fullResActive_)
            requestAdjustFullRes();
        pendingSave_ = adjustments;
        pendingSaveImageId_ = currentImageId_;
        pendingSaveVariantId_ = currentVariantId_;
        if (saveDebounceTimer_)
            saveDebounceTimer_->start();
        return;
    }
    commitAdjustments(adjustments);
}

void MainWindow::onAutoRequested() {
    if (currentImageId_ < 0 || currentFilePath_.isEmpty())
        return;
    statusBar()->showMessage(tr("正在计算自动调整…"));
    QMetaObject::invokeMethod(previewWorker_, "computeAutoAdjust",
                              Qt::QueuedConnection,
                              Q_ARG(qint64, currentImageId_),
                              Q_ARG(QString, currentFilePath_));
}

void MainWindow::onAutoAdjustReady(const lrc::Adjustments& adjustments) {
    if (currentImageId_ < 0)
        return;
    commitAdjustments(adjustments);
    statusBar()->showMessage(tr("已应用自动调整"), 2500);
}

// --- Undo / redo -------------------------------------------------------------

void MainWindow::commitAdjustments(const lrc::Adjustments& adjustments) {
    // A user edit: record it, sync the panel (suppressSignals_ prevents a
    // feedback loop), request a preview, keep the 1:1 view warm. The DB write
    // (JSON serialize + 2 sqlite statements) is debounced to 500ms: UI + undo stay
    // immediate, the persistence leaves the hot path. The latest
    // value is kept in pendingSave_ and flushed on timeout / image switch /
    // close via flushPendingSave().
    currentAdjustments_ = adjustments;
    undoStack_.push(adjustments);  // push() also discards the redo branch.
    developPanel_->setAdjustments(adjustments);
    syncSpotStateFromAdjustments();
    if (currentImageId_ < 0)
        return;
    requestAdjust(adjustments);
    if (fullResActive_)
        requestAdjustFullRes();
    pendingSave_ = adjustments;
    pendingSaveImageId_ = currentImageId_;
    pendingSaveVariantId_ = currentVariantId_;
    if (saveDebounceTimer_)
        saveDebounceTimer_->start();
}

void MainWindow::applyAdjustmentState(const lrc::Adjustments& adjustments) {
    // Undo/redo replay: same panel/preview/persist path, but does NOT push.
    // The undone/redone state already lives on the other side of the stack.
    currentAdjustments_ = adjustments;
    developPanel_->setAdjustments(adjustments);
    syncSpotStateFromAdjustments();
    if (currentImageId_ < 0)
        return;
    requestAdjust(adjustments);
    if (fullResActive_)
        requestAdjustFullRes();
    // Undo/redo persists immediately (not debounced), so the restored state reaches
    // the DB right away. Write to the active variant's stack.
    if (catalog_->saveAdjustments(currentImageId_, adjustments,
                                  currentVariantId_))
        statusBar()->showMessage(tr("编辑已保存"), 2500);
}

void MainWindow::flushPendingSave() {
    // Persist the latest debounced adjustment immediately. Called from the
    // save-debounce timeout, onImageSelected (image switch), closeEvent and the
    // destructor. Idempotent -- a no-op when nothing is pending.
    if (saveDebounceTimer_)
        saveDebounceTimer_->stop();
    if (!pendingSave_)
        return;
    const int imageId = pendingSaveImageId_;
    const int variantId = pendingSaveVariantId_;
    const lrc::Adjustments adjustments = *pendingSave_;
    pendingSave_.reset();
    pendingSaveImageId_ = -1;
    pendingSaveVariantId_ = 0;
    // The pending edit belongs to the variant that was active when it was scheduled
    // (a master or a virtual copy).
    if (imageId >= 0 &&
        catalog_->saveAdjustments(imageId, adjustments, variantId))
        statusBar()->showMessage(tr("编辑已保存"), 2500);
}

void MainWindow::cancelPendingSave() {
    // Discard a queued debounced save. Used by undo/redo, which persist the
    // restored state immediately via applyAdjustmentState.
    if (saveDebounceTimer_)
        saveDebounceTimer_->stop();
    pendingSave_.reset();
    pendingSaveImageId_ = -1;
    pendingSaveVariantId_ = 0;
}

void MainWindow::onSaveDebounceTimeout() {
    // The 500ms debounce window elapsed -- persist the latest value.
    flushPendingSave();
}

void MainWindow::undoAdjustment() {
    if (!undoStack_.canUndo())
        return;
    // An undo discards the in-flight debounced save; applyAdjustmentState below
    // persists the restored state immediately, so the DB cannot end up
    // holding an edit the user just reverted.
    cancelPendingSave();
    undoStack_.undo();
    // Safety: undoing the stack bottom (the loaded initial state) leaves the
    // undo side empty; there is no state to restore, so just report it.
    if (undoStack_.canUndo())
        applyAdjustmentState(undoStack_.top());
    else
        statusBar()->showMessage(tr("已是最早的状态"), 1500);
}

void MainWindow::redoAdjustment() {
    if (!undoStack_.canRedo())
        return;
    // Same as undo -- the redone state is persisted immediately below.
    cancelPendingSave();
    undoStack_.redo();
    applyAdjustmentState(undoStack_.top());
}

// --- Masked stroke undo (one stroke = one undo step) -------------------------

void MainWindow::onMaskGeometryChanged(const lrc::Mask& mask) {
    // First geometry change of a stroke/gesture: snapshot the pre-stroke state
    // so one undo lands exactly before the stroke.
    if (!maskStrokeActive_) {
        maskStrokeActive_ = true;
        undoStack_.push(currentAdjustments_);
    }
    if (currentMaskIndex_ >= 0)
        developPanel_->updateMaskGeometry(currentMaskIndex_, mask);
}

void MainWindow::onMaskEditFinished() {
    maskStrokeActive_ = false;
    if (currentImageId_ >= 0)
        commitAdjustments(currentAdjustments_);
}

void MainWindow::onMaskPromptClearRequested(int index) {
    if (index < 0 ||
        index >= static_cast<int>(currentAdjustments_.masks.size()))
        return;
    lrc::Mask& m = currentAdjustments_.masks[static_cast<size_t>(index)];
    if (m.type != lrc::MaskType::AiMask ||
        m.ai.target != lrc::AiMaskTarget::Prompt)
        return;
    // Clearing is a committed edit: snapshot for undo, drop the points, bump
    // the version so Pipeline re-rasterizes (Prompt falls back to a zero mask
    // with no points), then keep the viewport draft in sync.
    undoStack_.push(currentAdjustments_);
    m.ai.points.clear();
    m.ai.cachedWeightVersion++;
    commitAdjustments(currentAdjustments_);
    if (currentMaskIndex_ == index)
        viewport_->setMaskDraft(currentAdjustments_.masks[static_cast<size_t>(index)]);
}

// --- White-balance eyedropper (W) --------------------------------------------

void MainWindow::onWhiteBalanceSampled(const QColor& color) {
    if (currentImageId_ < 0)
        return;
    float lr = 0.0f, lg = 0.0f, lb = 0.0f;
    colorManager_->srgbPixelToLinearProPhoto(
        static_cast<uint8_t>(color.red()), static_cast<uint8_t>(color.green()),
        static_cast<uint8_t>(color.blue()), lr, lg, lb);
    const auto est = lrc::Pipeline::estimateWhiteBalance(lr, lg, lb);
    lrc::Adjustments adj = currentAdjustments_;
    adj.temperature =
        std::clamp(adj.temperature + est.temperature, -100.0, 100.0);
    adj.tint = std::clamp(adj.tint + est.tint, -100.0, 100.0);
    commitAdjustments(adj);
    // The eyedropper stays armed for repeated samples.
    statusBar()->showMessage(
        tr("白平衡取样: 色温 %1, 色调 %2")
            .arg(adj.temperature, 0, 'f', 1)
            .arg(adj.tint, 0, 'f', 1),
        4000);
}

// --- Colour-range mask eyedropper (C) ----------------------------------------

void MainWindow::onColorRangeSampled(const QColor& color) {
    if (currentImageId_ < 0)
        return;
    if (currentMaskIndex_ < 0 ||
        currentMaskIndex_ >= static_cast<int>(currentAdjustments_.masks.size()))
        return;
    float lr = 0.0f, lg = 0.0f, lb = 0.0f;
    colorManager_->srgbPixelToLinearProPhoto(
        static_cast<uint8_t>(color.red()), static_cast<uint8_t>(color.green()),
        static_cast<uint8_t>(color.blue()), lr, lg, lb);
    lrc::Adjustments adj = currentAdjustments_;
    lrc::Mask& m = adj.masks[static_cast<size_t>(currentMaskIndex_)];
    if (m.type != lrc::MaskType::ColorRange)   // only colour masks
        return;
    m.color.hasReference = true;
    m.color.refR = lr;
    m.color.refG = lg;
    m.color.refB = lb;
    commitAdjustments(adj);
    viewport_->setMaskDraft(m);
    viewport_->setMaskColorReference(color);
    statusBar()->showMessage(
        tr("颜色范围参考: R %1 G %2 B %3")
            .arg(lr, 0, 'f', 3)
            .arg(lg, 0, 'f', 3)
            .arg(lb, 0, 'f', 3),
        4000);
}

// --- Slider-drag gesture --------------------------------------------------

void MainWindow::onEditGestureStarted() {
    // A slider-drag gesture started. Reserved for coalescing a whole drag into one
    // undo entry; commitAdjustments already fires once on
    // release, so there is no behaviour change yet.
    gestureActive_ = true;
}

void MainWindow::onEditGestureFinished() { gestureActive_ = false; }

// --- Navigation + side-panel helpers ----------------------------------------

void MainWindow::navigateImages(int delta) {
    if (entries_.isEmpty())
        return;
    // Locate the exact current row (a master and its virtual copies share the
    // imageId, so match on the variant too).
    int index = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].imageId == currentImageId_ &&
            entries_[i].variantId == currentVariantId_) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;  // 当前图片不在筛选结果中：不导航 (not in the filtered list)
    showImageAtIndex(index + delta);
}

void MainWindow::showImageAtIndex(int index) {
    if (index < 0 || index >= entries_.size())
        return;
    // selectImage() emits selectionChanged -> onImageSelected (which starts a
    // fresh undo history for the new image). A virtual-copy row needs
    // selectVariant so the variant's edit stack is the one that loads.
    const FilmstripWidget::Entry& entry = entries_[index];
    if (entry.variantId != 0)
        filmstrip_->selectVariant(entry.imageId, entry.variantId);
    else
        filmstrip_->selectImage(entry.imageId);
}

void MainWindow::setSidePanelsVisible(bool visible) {
    navigator_->setVisible(visible);
    if (developScroll_)
        developScroll_->setVisible(visible);
}

// AI engine bootstrap. The body is #ifdef-guarded so a
// LRC_ENABLE_AI=OFF GUI build links cleanly (no ONNX Runtime symbols). When
// AI is enabled but the bundled manifest is missing (dev build without the
// installer stage), this logs a single warning and leaves aiEngine_ null;
// Pipeline then runs its pure-C++ fallback paths exactly as before.
void MainWindow::initializeAiEngine() {
#ifdef LRC_ENABLE_AI
    try {
        // Prefer the manifest bundled beside the exe (<appDir>/models/
        // manifest.json); fall back to ModelManager's default search path
        // (the Qt resource :/models/manifest.json or the dev source tree).
        const auto bundled = lrc::ModelManager::bundledRoot();
        const std::filesystem::path manifestPath =
            !bundled.empty() && std::filesystem::exists(bundled / "manifest.json")
                ? (bundled / "manifest.json")
                : lrc::ModelManager::defaultManifestPath();
        modelManager_ = std::make_unique<lrc::ModelManager>(manifestPath);
        aiEngine_ = std::make_unique<lrc::AiInferenceEngine>();

        // Register every bundled model that resolves to a ready file on disk
        // (installer-shipped or already cached). Failures are logged but do
        // not abort startup: the engine simply reports hasModel(id)==false
        // for the missing entries, and Pipeline falls back per-mask.
        const auto& models = modelManager_->allModels();
        int loaded = 0;
        for (const auto& entry : models) {
            if (entry.internalOnly)
                continue;  // test stub, never registered in production
            if (!modelManager_->isModelReady(entry))
                continue;
            const auto path = modelManager_->readPathFor(entry);
            lrc::ModelInfo info;
            info.id = entry.id;
            info.onnxPath = path;
            info.inputShape.dims = entry.inputShape;
            info.inputShape.layout = "NCHW";
            info.outputShape.dims = entry.outputShape;
            info.outputShape.layout = "NCHW";
            info.inputMean = entry.inputMean.empty()
                ? std::vector<float>{0.0f, 0.0f, 0.0f}
                : entry.inputMean;
            info.inputStd = entry.inputStd.empty()
                ? std::vector<float>{1.0f, 1.0f, 1.0f}
                : entry.inputStd;
            if (aiEngine_->loadModel(info))
                ++loaded;
            else
                qWarning("AiInferenceEngine: failed to load '%s' from %s",
                         entry.id.c_str(),
                         path.string().c_str());
        }
        // Publish the engine process-wide so Pipeline's static entry points
        // can reach it. Pipeline::globalAiEngine() returns null until this
        // call, which is why headless tests (no MainWindow) stay bit-identical
        // to the pre-AI baseline.
        lrc::Pipeline::setGlobalAiEngine(aiEngine_.get());
        qInfo("AiInferenceEngine: %d/%zu model(s) registered for inference.",
              loaded, models.size());
    } catch (const std::exception& e) {
        qWarning("AI engine initialization failed: %s", e.what());
        aiEngine_.reset();
        modelManager_.reset();
    }
#endif
}

// --- Global bare-key shortcuts ------------------------------------------------

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        // Never steal keys while a modal dialog is open.
        if (QApplication::activeModalWidget())
            return QMainWindow::eventFilter(watched, event);
        // Never steal keys while a popup (context menu / combo dropdown) is
        // open: bare keys must reach the popup, not the global shortcuts.
        if (QApplication::activePopupWidget())
            return QMainWindow::eventFilter(watched, event);
        // Ignore auto-repeat so holding a key does not spam toggles.
        if (keyEvent->isAutoRepeat())
            return QMainWindow::eventFilter(watched, event);
        // Focus whitelist: text-editing widgets keep their keys (typing,
        // arrows, Tab). QDoubleSpinBox is a QAbstractSpinBox, so this also
        // covers the spin boxes.
        if (QWidget* focus = QApplication::focusWidget()) {
            if (qobject_cast<QDoubleSpinBox*>(focus) ||
                qobject_cast<QAbstractSpinBox*>(focus) ||
                qobject_cast<QAbstractSlider*>(focus) ||
                qobject_cast<QLineEdit*>(focus) ||
                qobject_cast<QComboBox*>(focus))
                return QMainWindow::eventFilter(watched, event);
        }

        const Qt::KeyboardModifiers mods = keyEvent->modifiers();
        const bool ctrl = mods.testFlag(Qt::ControlModifier);
        const bool alt = mods.testFlag(Qt::AltModifier);
        const bool meta = mods.testFlag(Qt::MetaModifier);
        const bool shift = mods.testFlag(Qt::ShiftModifier);
        // Modifier combos (Ctrl/Alt/Meta) are handled by QShortcut / menu
        // actions, never here.
        if (ctrl || alt || meta)
            return QMainWindow::eventFilter(watched, event);

        switch (keyEvent->key()) {
        case Qt::Key_Z: {
            // 1:1 <-> last fit mode toggle (Lightroom's bare Z).
            viewport_->toggleOneToOne();
            return true;
        }
        case Qt::Key_Tab: {
            // Collapse / expand the left and right side panels.
            const bool anyVisible =
                navigator_->isVisible() ||
                (developScroll_ && developScroll_->isVisible());
            setSidePanelsVisible(!anyVisible);
            return true;
        }
        case Qt::Key_F:
            if (isFullScreen())
                showNormal();
            else
                showFullScreen();
            return true;
        case Qt::Key_G:
            navigator_->setFocus();
            return true;
        case Qt::Key_D:
            developPanel_->setFocus();
            return true;
        case Qt::Key_E:
            viewport_->setFocus();
            return true;
        case Qt::Key_L: {
            // Light cycle (two states): normal <-> preview only (side panels
            // hidden, filmstrip stays). A future state could black out the UI.
            const bool anyVisible =
                navigator_->isVisible() ||
                (developScroll_ && developScroll_->isVisible());
            setSidePanelsVisible(!anyVisible);
            statusBar()->showMessage(
                anyVisible ? tr("灯光关闭 (仅预览)") : tr("灯光正常"), 2000);
            return true;
        }
        case Qt::Key_Home:
            showImageAtIndex(0);
            return true;
        case Qt::Key_End:
            showImageAtIndex(entries_.size() - 1);
            return true;
        case Qt::Key_BracketLeft:
            developPanel_->changeMaskBrushSize(-1);
            return true;
        case Qt::Key_BracketRight:
            developPanel_->changeMaskBrushSize(1);
            return true;
        case Qt::Key_K:
            developPanel_->selectMaskTool(lrc::MaskType::Brush);
            return true;
        case Qt::Key_M:
            // M = radial, Shift+M = linear gradient (Lightroom convention).
            if (shift)
                developPanel_->selectMaskTool(lrc::MaskType::LinearGradient);
            else
                developPanel_->selectMaskTool(lrc::MaskType::RadialGradient);
            return true;
        case Qt::Key_Backslash:
            // Lightroom's '\' toggles the before/after single-view.
            viewport_->toggleBeforeAfter();
            return true;
        case Qt::Key_O:
            viewport_->setMaskOverlayVisible(!viewport_->maskOverlayVisible());
            return true;
        case Qt::Key_W:
            viewport_->setWhiteBalanceSamplerActive(
                !viewport_->whiteBalanceSamplerActive());
            // When W activates the white-balance sampler, uncheck the panel's other
            // sampler checkboxes.
            if (viewport_->whiteBalanceSamplerActive())
                disarmSpotToolButton();
            developPanel_->setSamplerChecked(false);
            developPanel_->setColorTargetSamplerChecked(false);
            return true;
        case Qt::Key_C:
            viewport_->setColorRangeSamplerActive(
                !viewport_->colorRangeSamplerActive());
            // Mutual exclusion: turning on the colour-range sampler disables white balance,
            // point-colour and HSL samplers.
            if (viewport_->colorRangeSamplerActive())
                disarmSpotToolButton();
            viewport_->setWhiteBalanceSamplerActive(false);
            developPanel_->setSamplerChecked(false);
            developPanel_->setColorTargetSamplerChecked(false);
            return true;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            // '=' (unshifted '+' on US layouts) doubles as '+'.
            developPanel_->adjustExposure(0.33);
            return true;
        case Qt::Key_Minus:
            developPanel_->adjustExposure(-0.33);
            return true;
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onColorSamplerRequested(bool active) {
    viewport_->setSamplerActive(active);
    // Mutual exclusion: arming the HSL point sampler disables the point-colour
    // sampler.
    viewport_->setPointColorSamplerActive(false);
    developPanel_->setColorTargetSamplerChecked(false);
    if (active)
        disarmSpotToolButton();  // 吸管激活时解除污点工具 (disarm the spot tool).
    statusBar()->showMessage(
        active ? tr("点选吸管：点击图像选取颜色，横向拖动调整饱和度，纵向调整明亮度")
               : tr("点选吸管已关闭"),
        4000);
    if (!active)
        developPanel_->setSamplerChecked(false);
}

void MainWindow::onColorTargetSamplerRequested(bool active) {
    if (active) {
        // When the point-colour sampler takes over an existing HSL pick, disarm the
        // HSL sampler first.
        viewport_->setSamplerActive(false);
        developPanel_->setSamplerChecked(false);
    }
    viewport_->setPointColorSamplerActive(active);
    if (active)
        disarmSpotToolButton();  // 点颜色吸管激活时解除污点工具 (disarm the spot tool).
    statusBar()->showMessage(
        active ? tr("点颜色：点击图像选取参考色") : tr("点颜色吸管已关闭"),
        4000);
    if (!active)
        developPanel_->setColorTargetSamplerChecked(false);
}

void MainWindow::onPointColorSampled(const QColor& color) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    colorManager_->srgbPixelToLinearProPhoto(
        static_cast<uint8_t>(color.red()), static_cast<uint8_t>(color.green()),
        static_cast<uint8_t>(color.blue()), r, g, b);
    const int index = developPanel_->currentColorTargetIndex();
    developPanel_->setColorTargetReference(index, color, r, g, b);
    statusBar()->showMessage(
        tr("已取样参考色 #%1%2%3")
            .arg(color.red(), 2, 16, QLatin1Char('0'))
            .arg(color.green(), 2, 16, QLatin1Char('0'))
            .arg(color.blue(), 2, 16, QLatin1Char('0')),
        4000);
}

void MainWindow::onColorSampled(int colorRangeIndex, const QColor& color) {
    const int idx = colorRangeIndex;
    if (idx < 0 || idx >= 8)
        return;
    viewport_->setSamplerBase(
        idx, currentAdjustments_.hsl[static_cast<size_t>(idx)].saturation,
        currentAdjustments_.hsl[static_cast<size_t>(idx)].luminance);
    static const char* kNames[8] = {"红", "橙", "黄", "绿",
                                    "青", "蓝", "紫", "品红"};
    statusBar()->showMessage(
        tr("已选取颜色范围：%1  (#%2%3%4)")
            .arg(QString::fromUtf8(kNames[idx]))
            .arg(color.red(), 2, 16, QLatin1Char('0'))
            .arg(color.green(), 2, 16, QLatin1Char('0'))
            .arg(color.blue(), 2, 16, QLatin1Char('0')),
        4000);
}

void MainWindow::onColorAdjusted(int colorRangeIndex, double saturation,
                                 double luminance) {
    if (colorRangeIndex < 0 || colorRangeIndex >= 8)
        return;
    auto& ch = currentAdjustments_
                   .hsl[static_cast<size_t>(colorRangeIndex)];
    ch.saturation = saturation;
    ch.luminance = luminance;
    developPanel_->setAdjustments(currentAdjustments_);
    requestAdjust(currentAdjustments_);
}

void MainWindow::onColorAdjustedFinished() {
    if (currentImageId_ < 0)
        return;
    commitAdjustments(currentAdjustments_);
    statusBar()->showMessage(tr("点选颜色调整已保存"), 2500);
}

void MainWindow::onVisualizationRequested(int mode,
                                          const lrc::Adjustments& adjustments) {
    if (currentImageId_ < 0)
        return;
    currentAdjustments_ = adjustments;
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "renderVisualization", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(qint64, previewGeneration_),
        Q_ARG(int, mode), Q_ARG(lrc::Adjustments, adjustments));
}

void MainWindow::onVisualizationCleared() {
    viewport_->setDiagnosticOverlay(QImage());
}

void MainWindow::onVisualizationReady(qint64 imageId, qint64 generation,
                                      const lrc::ImageBufferPtr& overlay) {
    if (imageId != currentImageId_ || generation != previewGeneration_)
        return;
    if (!overlay || overlay->format() != lrc::PixelFormat::U8 ||
        overlay->empty())
        return;
    const QImage image = toQImage(overlay);
    if (!image.isNull())
        viewport_->setDiagnosticOverlay(image);
}

void MainWindow::requestPreview(const QString& filePath,
                                const lrc::Adjustments& adjustments) {
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "loadImage", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(QString, filePath),
        Q_ARG(qint64, previewGeneration_), Q_ARG(lrc::Adjustments, adjustments));
}

void MainWindow::requestAdjust(const lrc::Adjustments& adjustments) {
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "adjustPreview", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(qint64, previewGeneration_),
        Q_ARG(lrc::Adjustments, adjustments));
}

void MainWindow::requestAdjustCoarse(const lrc::Adjustments& adjustments) {
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "adjustPreviewCoarse", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(qint64, previewGeneration_),
        Q_ARG(lrc::Adjustments, adjustments));
}

void MainWindow::requestFullRes() {
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "loadFullRes", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(QString, currentFilePath_),
        Q_ARG(qint64, previewGeneration_),
        Q_ARG(lrc::Adjustments, currentAdjustments_));
}

void MainWindow::clearFullRes() {
    QMetaObject::invokeMethod(previewWorker_, "clearFullRes",
                              Qt::QueuedConnection);
}

void MainWindow::requestAdjustFullRes() {
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "adjustFullRes", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(qint64, previewGeneration_),
        Q_ARG(lrc::Adjustments, currentAdjustments_));
}

void MainWindow::requestAdjustFullResCoarse(
    const lrc::Adjustments& adjustments) {
    ++previewGeneration_;
    QMetaObject::invokeMethod(
        previewWorker_, "adjustFullResCoarse", Qt::QueuedConnection,
        Q_ARG(qint64, currentImageId_), Q_ARG(qint64, previewGeneration_),
        Q_ARG(lrc::Adjustments, adjustments));
}

void MainWindow::onFullResRequested(bool needed) {
    fullResActive_ = needed;
    if (needed)
        requestFullRes();
    else
        clearFullRes();
}

void MainWindow::onImageMeta(qint64 imageId, int w, int h) {
    if (imageId != currentImageId_)
        return;
    viewport_->setNativeImageSize(QSize(w, h));
}

void MainWindow::onFullResReady(qint64 imageId,
                                const lrc::ImageBufferPtr& rgbU8) {
    if (imageId != currentImageId_)
        return;
    // Drop a stale full-res result if 1:1 was exited while the decode was in
    // flight.
    if (!fullResActive_)
        return;
    if (!rgbU8 || rgbU8->format() != lrc::PixelFormat::U8 || rgbU8->empty())
        return;
    const QImage image = toQImage(rgbU8);
    if (image.isNull())
        return;
    // Fallback: when the imageMeta has not arrived, use the full-res buffer's own
    // size as the native size.
    if (viewport_->nativeSize().isEmpty())
        viewport_->setNativeImageSize(image.size());
    viewport_->setImageKeepView(image);
    // Full-res is a final frame -- histogram/status refresh immediately (not
    // throttled).
    updateHistogramNow(image);
}

void MainWindow::onPreviewReady(qint64 imageId,
                                const lrc::ImageBufferPtr& rgbU8) {
    if (imageId != currentImageId_)
        return;
    const QImage image = toQImage(rgbU8);
    if (image.isNull())
        return;
    if (fullResActive_) {
        // 1:1 全尺寸在途/已就位：别用 1920 预览盖掉全尺寸视图 (don't cover the
        // full-res view). 直方图/状态栏仍用预览数据，保证即时反馈.
        updateHistogramNow(image);
        return;
    }
    viewport_->setImage(image);
    updateHistogramNow(image);
}

void MainWindow::onFullResReadyCoarse(qint64 imageId,
                                      const lrc::ImageBufferPtr& rgbU8) {
    if (imageId != currentImageId_)
        return;
    // 已退出 1:1:丢弃在途的全尺寸 coarse 中间帧 (drop stale full-res coarse if
    // 1:1 was exited while the frame was in flight).
    if (!fullResActive_)
        return;
    const QImage image = toQImage(rgbU8);
    if (image.isNull())
        return;
    // Mirrors onPreviewReadyCoarse: swap the coarse frame in immediately without
    // resetting the zoom/anchor; only the histogram/status bar coalesce (~150ms).
    viewport_->setImageKeepView(image);
    requestHistogramUpdate(image);
}

void MainWindow::onPreviewReadyCoarse(qint64 imageId,
                                      const lrc::ImageBufferPtr& rgbU8) {
    if (imageId != currentImageId_)
        return;
    const QImage image = toQImage(rgbU8);
    if (image.isNull())
        return;
    // Coarse preview replaces the 1920 preview layer, not the full-res view,
    // so it stays live even in 1:1 (fullResActive_) for immediate drag
    // feedback. setImageKeepView swaps it in without resetting the zoom/anchor.
    // The viewport update is immediate (never throttled); only the histogram
    // and status bar coalesce to ~150ms.
    viewport_->setImageKeepView(image);
    requestHistogramUpdate(image);
}

void MainWindow::updateHistogramAndStatus(const QImage& image) {
    // 全尺寸直喂直方图也便宜：统计已按 ~400 万像素子采样 (the histogram
    // subsamples to <=~4M pixels, so full-res input stays cheap).
    developPanel_->histogram()->setImage(image);
    statusBar()->showMessage(
        tr("%1 — %2×%3")
            .arg(QFileInfo(currentFilePath_).fileName())
            .arg(image.width())
            .arg(image.height()));
}

void MainWindow::updateHistogramNow(const QImage& image) {
    // Immediate histogram + status-bar refresh (final/refine frames). Cancels any
    // queued throttled update so a stale flush cannot double-run
    // right after a final frame.
    if (histogramThrottle_)
        histogramThrottle_->stop();
    histogramPending_ = false;
    lastHistogramMs_ = histogramClock_.elapsed();
    updateHistogramAndStatus(image);
}

void MainWindow::requestHistogramUpdate(const QImage& image) {
    // Coarse frames (up to ~33Hz) coalesce to ~150ms for the histogram and status
    // bar. The viewport update is NOT throttled -- the caller
    // already applied it. A one-shot timer guarantees the last frame of a
    // throttle window still gets its histogram/status update.
    pendingHistogramImage_ = image;
    if (histogramClock_.isValid() &&
        histogramClock_.elapsed() - lastHistogramMs_ <
            lrc::app::throttle::kHistogramThrottleMs) {
        histogramPending_ = true;
        if (histogramThrottle_)
            histogramThrottle_->start();
        return;
    }
    updateHistogramNow(image);
}

void MainWindow::flushHistogramThrottle() {
    // Trailing edge of a throttle window -- the last queued coarse frame lands
    // here, so the UI never freezes on stale histogram data.
    if (!histogramPending_ || pendingHistogramImage_.isNull())
        return;
    histogramPending_ = false;
    lastHistogramMs_ = histogramClock_.elapsed();
    updateHistogramAndStatus(pendingHistogramImage_);
}

void MainWindow::onThumbReadyImage(int imageId, const QImage& thumb) {
    // The thumbnail was decoded on the import worker thread; just apply it. No
    // synchronous disk decode on the main thread.
    if (records_.contains(imageId))
        records_[imageId].thumbnailPath.clear();
    if (!thumb.isNull())
        filmstrip_->setThumbnail(imageId, thumb);
}

void MainWindow::onThumbReady(int imageId, const QString& thumbnailPath) {
    // 优先从目录 BLOB 取内嵌缩略图 (prefer the embedded thumbnail blob).
    if (auto blob =
            catalog_->loadPreview(imageId, lrc::PreviewKind::Thumbnail)) {
        QImage thumb;
        if (thumb.loadFromData(
                QByteArray(reinterpret_cast<const char*>(blob->data()),
                           static_cast<int>(blob->size())))) {
            if (records_.contains(imageId))
                records_[imageId].thumbnailPath.clear();
            filmstrip_->setThumbnail(imageId, thumb);
            return;
        }
    }
    // 旧目录回退磁盘路径 (legacy catalogs fall back to the disk path).
    if (records_.contains(imageId))
        records_[imageId].thumbnailPath = thumbnailPath.toStdString();
    QImage thumb(thumbnailPath);
    if (!thumb.isNull())
        filmstrip_->setThumbnail(imageId, thumb);
}

void MainWindow::onThumbnailRequested(int imageId) {
    // The filmstrip scrolled a row into view that has no thumbnail yet. Enqueue a
    // background decode; dedupe so the same image is never requested
    // twice while queued or in flight.
    if (queuedThumbnails_.contains(imageId))
        return;
    if (pendingThumbnails_.size() >= kMaxPendingThumbnails) {
        // Bound the backlog: drop the oldest pending request, keep the newest.
        const int dropped = pendingThumbnails_.takeFirst();
        queuedThumbnails_.remove(dropped);
    }
    pendingThumbnails_.append(imageId);
    queuedThumbnails_.insert(imageId);
    maybeDispatchThumbDecode();
}

void MainWindow::maybeDispatchThumbDecode() {
    // At most one decode is in flight on the thumb loader thread. Read the
    // thumbnail path from records_ here (main thread) so the worker never
    // touches UI-owned state.
    if (thumbDecodeBusy_ || pendingThumbnails_.isEmpty())
        return;
    const int imageId = pendingThumbnails_.takeFirst();
    thumbDecodeBusy_ = true;
    QString thumbnailPath;
    const auto it = records_.constFind(imageId);
    if (it != records_.constEnd())
        thumbnailPath = QString::fromStdString(it->thumbnailPath);
    emit thumbDecodeRequested(imageId, thumbnailPath);
}

void MainWindow::onThumbnailDecoded(int imageId) {
    // The background decode finished (success or failure); the image is no longer
    // queued, so a later re-request may enqueue it again.
    queuedThumbnails_.remove(imageId);
    thumbDecodeBusy_ = false;
    maybeDispatchThumbDecode();
}

void MainWindow::onImportProgress(int done, int total) {
    statusBar()->showMessage(tr("导入 %1 / %2").arg(done).arg(total));
}

void MainWindow::onImportFinished(int addedCount) {
    refreshFilmstrip();
    statusBar()->showMessage(
        tr("导入完成：%1 张图片").arg(addedCount), 4000);
}

void MainWindow::onImportWarning(const QString& message) {
    statusBar()->showMessage(message, 6000);
}

void MainWindow::onExportFinished(bool ok, const QString& message) {
    statusBar()->showMessage(message, 6000);
    if (ok)
        QMessageBox::information(this, tr("导出"), message);
}

void MainWindow::mergeHdr() {
    const auto images = catalog_->listImages();
    if (images.size() < 2) {
        QMessageBox::information(
            this, tr("HDR 合成"),
            tr("目录中至少需要 2 张照片才能进行 HDR 合成。"));
        return;
    }
    QVector<HdrMergeImage> candidates;
    candidates.reserve(static_cast<int>(images.size()));
    for (const auto& rec : images) {
        const QString filePath = QString::fromStdString(rec.filePath);
        candidates.append(HdrMergeImage{
            static_cast<int>(rec.id), QFileInfo(filePath).fileName(), filePath});
    }
    HdrMergeDialog dlg(candidates, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList paths = dlg.selectedPaths();
    if (paths.size() < 2)
        return;
    const QString out = dlg.outputPath();
    if (out.isEmpty())
        return;
    statusBar()->showMessage(tr("正在合并 %1 张照片为 HDR…").arg(paths.size()));
    QMetaObject::invokeMethod(previewWorker_, "mergeHdr", Qt::QueuedConnection,
                              Q_ARG(QStringList, paths),
                              Q_ARG(bool, dlg.deghost()),
                              Q_ARG(QString, out));
}

void MainWindow::onHdrProgress(int done, int total) {
    statusBar()->showMessage(tr("解码包围照片 %1 / %2").arg(done).arg(total));
}

void MainWindow::onHdrFinished(bool ok, const QString& message,
                               const QString& outPath) {
    statusBar()->showMessage(message, 6000);
    if (!ok) {
        QMessageBox::warning(this, tr("HDR 合成"), message);
        return;
    }
    // Import the merged DNG back into the catalog.
    startImport(QStringList{outPath});
    QMessageBox::information(this, tr("HDR 合成"), message);
}

void MainWindow::onConvertFinished(bool ok, const QString& message,
                                   const QString& outPath) {
    statusBar()->showMessage(message, 6000);
    if (!ok) {
        QMessageBox::warning(this, tr("转换为 DNG"), message);
        return;
    }
    // Import the converted DNG back into the catalog.
    startImport(QStringList{outPath});
    QMessageBox::information(this, tr("转换为 DNG"), message);
}

void MainWindow::importPreset() {
    if (currentImageId_ < 0 || currentFilePath_.isEmpty()) {
        QMessageBox::information(this, tr("导入预设"), tr("先选择一张照片"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("导入预设"), QString(),
        // .xmp remains a readable legacy preset format (Lightroom compatibility);
        // the visible name uses our own vocabulary, not the Lightroom brand.
        tr("XMP 预设 (*.xmp);;本应用预设 (*.lrp)"));
    if (path.isEmpty())
        return;

    std::optional<lrc::Adjustments> preset;
    if (QFileInfo(path).suffix().compare(QStringLiteral("lrp"),
                                         Qt::CaseInsensitive) == 0) {
        preset = lrc::ProfileIO::loadPreset(toStdPath(path));
    } else {
        // XMP preset: parse the crs: attributes, then map them to adjustments.
        std::ifstream in(toStdPath(path), std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            std::map<std::string, std::string> crsAttrs;
            if (lrc::XmpDocument::parse(ss.str(), crsAttrs))
                preset = lrc::XmpProfile::fromCrsAttrs(crsAttrs);
        }
    }

    if (!preset) {
        QMessageBox::warning(this, tr("导入预设"), tr("预设文件无效或无法解析。"));
        return;
    }

    // A preset import is a user edit: undoable via commitAdjustments.
    commitAdjustments(*preset);
    statusBar()->showMessage(
        tr("已导入预设：%1").arg(QFileInfo(path).fileName()), 4000);
}

void MainWindow::exportPreset() {
    if (currentImageId_ < 0 || currentFilePath_.isEmpty()) {
        QMessageBox::information(this, tr("导出预设"), tr("先选择一张照片"));
        return;
    }
    const QString dir = QFileInfo(currentFilePath_).absolutePath();
    const QString suggested = dir + QStringLiteral("/") +
                              QFileInfo(currentFilePath_).baseName() +
                              QStringLiteral(".xmp");
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("导出预设"), suggested,
        tr("XMP 预设 (*.xmp);;本应用预设 (*.lrp)"), &selectedFilter);
    if (path.isEmpty())
        return;

    // Append the extension implied by the chosen filter when the user omitted one.
    if (QFileInfo(path).suffix().isEmpty()) {
        const bool wantsXmp =
            selectedFilter.contains(QStringLiteral("*.xmp"), Qt::CaseInsensitive);
        path += wantsXmp ? QStringLiteral(".xmp") : QStringLiteral(".lrp");
    }

    bool ok = false;
    if (QFileInfo(path).suffix().compare(QStringLiteral("lrp"),
                                         Qt::CaseInsensitive) == 0) {
        ok = lrc::ProfileIO::savePreset(toStdPath(path), currentAdjustments_);
    } else {
        // XMP preset: map adjustments to crs: attributes, serialize, write.
        const auto attrs = lrc::XmpProfile::toCrsAttrs(currentAdjustments_);
        const std::string xml = lrc::XmpDocument::serialize(attrs);
        std::ofstream os(toStdPath(path), std::ios::binary);
        os << xml;
        os.close();
        ok = os.good();
    }

    if (ok)
        statusBar()->showMessage(
            tr("已导出预设：%1").arg(QFileInfo(path).fileName()), 4000);
    else
        QMessageBox::warning(this, tr("导出预设"), tr("写入预设文件失败。"));
}

void MainWindow::readSidecar() {
    if (currentImageId_ < 0 || currentFilePath_.isEmpty()) {
        QMessageBox::information(this, tr("读取 sidecar"), tr("先选择一张照片"));
        return;
    }
    lrc::Adjustments adj;
    if (!lrc::XmpSidecar::readFromSidecar(toStdPath(currentFilePath_), adj)) {
        QMessageBox::information(this, tr("读取 sidecar"),
                                 tr("未找到可读取的 sidecar"));
        return;
    }
    // Reading a sidecar is a user edit: undoable via commitAdjustments.
    commitAdjustments(adj);
    statusBar()->showMessage(tr("已从 sidecar 读取调整"), 4000);
}

void MainWindow::writeSidecar() {
    if (currentImageId_ < 0 || currentFilePath_.isEmpty()) {
        QMessageBox::information(this, tr("写入 sidecar"), tr("先选择一张照片"));
        return;
    }
    const QString sidecarPath =
        QFileInfo(currentFilePath_).absolutePath() + QStringLiteral("/") +
        QFileInfo(currentFilePath_).completeBaseName() + QStringLiteral(".xmp");
    if (lrc::XmpSidecar::writeToSidecar(toStdPath(currentFilePath_),
                                        currentAdjustments_))
        statusBar()->showMessage(
            tr("已写入 sidecar：%1").arg(QFileInfo(sidecarPath).fileName()),
            4000);
    else
        QMessageBox::warning(this, tr("写入 sidecar"), tr("写入 sidecar 失败。"));
}

void MainWindow::onSetRating(int imageId, int rating) {
    if (!catalog_->setRating(imageId, rating))
        return;
    if (records_.contains(imageId))
        records_[imageId].rating = rating;
    for (auto& e : entries_) {
        if (e.imageId == imageId) {
            e.rating = rating;
            break;
        }
    }
    filmstrip_->setRating(imageId, rating);
    // Update the stats footer without a full filmstrip rebuild.
    int rated = 0;
    for (const auto& rec : records_)
        if (rec.rating > 0)
            ++rated;
    navigator_->setStatistics(entries_.size(), rated);
    statusBar()->showMessage(tr("评分: %1 星").arg(rating), 2000);
}

void MainWindow::onSetFlag(int imageId, int flag) {
    if (!catalog_->setFlag(imageId, flag))
        return;
    if (records_.contains(imageId))
        records_[imageId].flag = flag;
    for (auto& e : entries_) {
        if (e.imageId == imageId) {
            e.flag = flag;
            break;
        }
    }
    filmstrip_->setFlag(imageId, flag);
    const char* name = flag == lrc::ImageFlag::FlagPicked
                           ? "已选取"
                           : (flag == lrc::ImageFlag::FlagRejected ? "已拒绝"
                                                                   : "无旗标");
    statusBar()->showMessage(tr("旗标: %1").arg(QString::fromUtf8(name)), 2000);
}

void MainWindow::onSetLabelColor(int imageId, int color) {
    if (!catalog_->setLabelColor(imageId, color))
        return;
    if (records_.contains(imageId))
        records_[imageId].labelColor = color;
    for (auto& e : entries_) {
        if (e.imageId == imageId) {
            e.labelColor = color;
            break;
        }
    }
    filmstrip_->setLabelColor(imageId, color);
    // A label change only updates this tile; no full filmstrip rebuild. 0 = 清除
    // (clear), 1..7 = colour labels.
    static const char* kLabelNames[8] = {"清除", "红", "橙", "黄",
                                         "绿", "青", "蓝", "紫"};
    const int ci = (color >= 0 && color <= 7) ? color : 0;
    statusBar()->showMessage(
        tr("色标: %1").arg(QString::fromUtf8(kLabelNames[ci])), 2000);
}

void MainWindow::onCollectionToggle(int imageId, int collectionId, bool add) {
    const bool ok = add ? catalog_->addToCollection(collectionId, imageId)
                        : catalog_->removeFromCollection(collectionId, imageId);
    if (!ok)
        return;
    // Incremental membership update -- only re-badge this tile instead of a full
    // filmstrip rebuild. records_ has no collection field, so keep the
    // membership list in the matching entry in sync for the badge source.
    bool inCollection = false;
    for (auto& e : entries_) {
        if (e.imageId != imageId)
            continue;
        if (add) {
            if (!e.collectionIds.contains(collectionId))
                e.collectionIds.append(collectionId);
        } else {
            e.collectionIds.removeAll(collectionId);
        }
        inCollection = !e.collectionIds.isEmpty();
        break;
    }
    filmstrip_->setInCollection(imageId, inCollection);
    // Removing from the collection currently being viewed drops the tile out of
    // the filtered result -- that still needs a full rebuild (filter changed).
    if (filterCollectionId_ >= 0 && !add && collectionId == filterCollectionId_)
        refreshFilmstrip();
    statusBar()->showMessage(add ? tr("已加入收藏夹") : tr("已移出收藏夹"), 2000);
}

void MainWindow::onCollectionSelected(int collectionId) {
    filterCollectionId_ = collectionId;
    filterFolder_.clear();
    filterKeywordId_ = -1;
    filterSmartCollectionId_ = -1;
    filterSearchText_.clear();
    if (searchDebounceTimer_)
        searchDebounceTimer_->stop();
    if (searchEdit_) {
        const QSignalBlocker blocker(searchEdit_);
        searchEdit_->clear();
    }
    refreshFilmstrip();
}

void MainWindow::onManageCollections() {
    CollectionsDialog dlg(catalog_.get(), this);
    connect(&dlg, &CollectionsDialog::collectionsChanged, this,
            [this]() { refreshFilmstrip(); });
    dlg.exec();
}

void MainWindow::onRemoveImage(int imageId) {
    const auto it = records_.constFind(imageId);
    const QString name = it == records_.constEnd()
                             ? QString()
                             : QFileInfo(QString::fromStdString(it->filePath))
                                   .fileName();
    const QString msg = name.isEmpty()
                            ? tr("从目录移除这张照片？")
                            : tr("从目录移除「%1」？").arg(name);
    if (QMessageBox::question(this, tr("移除"), msg) != QMessageBox::Yes)
        return;
    // Capture the adjacent photo (prefer the next, else the previous) from the
    // visible filmstrip list BEFORE the removal rebuilds entries_/records_.
    int neighborId = -1;
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_[i].imageId != imageId)
            continue;
        if (i + 1 < entries_.size())
            neighborId = entries_[i + 1].imageId;
        else if (i - 1 >= 0)
            neighborId = entries_[i - 1].imageId;
        break;
    }
    if (!catalog_->removeImage(imageId))
        return;
    const bool removingCurrent = (currentImageId_ == imageId);
    if (removingCurrent) {
        currentImageId_ = -1;
        viewport_->setImage(QImage());
    }
    refreshFilmstrip();
    // After removing the current image, select the adjacent one so the view never
    // falls blank. refreshFilmstrip's re-select logic only restores the
    // highlight for a surviving current image, so we drive the load here.
    if (removingCurrent && neighborId >= 0 && records_.contains(neighborId)) {
        const QSignalBlocker blocker(filmstrip_);
        filmstrip_->selectImage(neighborId);  // visual highlight on the filmstrip
        onImageSelected(neighborId);          // normal load path for the viewport
    }
    statusBar()->showMessage(tr("已从目录移除"), 2000);
}

void MainWindow::onFiltersChanged(int minRating, int flagFilter,
                                  int labelColorFilter) {
    filterRating_ = minRating;
    filterFlag_ = flagFilter;
    filterLabelColor_ = labelColorFilter;
    refreshFilmstrip();
}

void MainWindow::watchFolderPoll() {
    if (folderMonitor_)
        folderMonitor_->poll();
}

void MainWindow::refreshFilmstrip() {
    lrc::ImageFilter filter;
    filter.minRating = filterRating_;
    if (filterFlag_ == lrc::ImageFlag::FlagPicked ||
        filterFlag_ == lrc::ImageFlag::FlagRejected)
        filter.flag = filterFlag_;
    // 被拒筛选必须放行被拒照片,否则 flag=-1 AND flag!=-1 恒空 (rejected filter).
    // 且 filterFlag_==0(所有照片) 默认也显示被拒照片,保留清除旗标入口 (all-photos
    // shows rejected photos too, matching Lightroom).
    if (filterFlag_ == lrc::ImageFlag::FlagRejected || filterFlag_ == 0)
        filter.includeRejected = true;
    filter.labelColor = filterLabelColor_;
    const auto images = catalog_->listImages(filter);

    // Push the current collection list to the navigator and filmstrip, and
    // build an imageId -> collectionIds membership map for the filmstrip badges.
    const auto collections = catalog_->listCollections();
    const QVector<lrc::Collection> collectionVec(collections.begin(),
                                                 collections.end());
    navigator_->setCollections(collectionVec);
    filmstrip_->setCollections(collectionVec);
    QHash<int, QVector<int>> imageCollections;  // image id -> collection ids
    for (const lrc::Collection& c : collections) {
        for (const auto& rec : catalog_->listImagesInCollection(c.id))
            imageCollections[static_cast<int>(rec.id)].append(
                static_cast<int>(c.id));
    }

    // When a specific collection is selected, restrict to its members.
    QSet<int> collectionMembers;
    if (filterCollectionId_ >= 0) {
        for (const auto& rec :
             catalog_->listImagesInCollection(filterCollectionId_))
            collectionMembers.insert(static_cast<int>(rec.id));
    }

    // Keyword / smart-collection / search filters (each replaces the others via
    // clearImageFilters() in the selection handlers, but they can
    // stack with the rating/flag/label combos and the folder filter below).
    QSet<int> keywordMembers;
    if (filterKeywordId_ >= 0) {
        for (const int64_t id : catalog_->findImagesByKeyword(filterKeywordId_))
            keywordMembers.insert(static_cast<int>(id));
    }
    QSet<int> smartMembers;
    if (filterSmartCollectionId_ >= 0) {
        for (const int64_t id :
             catalog_->listImagesInSmartCollection(filterSmartCollectionId_))
            smartMembers.insert(static_cast<int>(id));
    }
    QSet<int> searchMembers;
    if (!filterSearchText_.isEmpty()) {
        for (const auto& rec : catalog_->searchImages(filterSearchText_.toStdString()))
            searchMembers.insert(static_cast<int>(rec.id));
    }

    entries_.clear();
    records_.clear();
    int rated = 0;
    for (const auto& rec : images) {
        // "No flag" is a special case the catalog filter cannot express
        // (flag == 0 means "any"), so post-filter it here.
        if (filterFlag_ == 2 && rec.flag != lrc::ImageFlag::FlagNone)
            continue;
        const QString filePath = QString::fromStdString(rec.filePath);
        if (!filterFolder_.isEmpty()) {
            // Normalize both sides to '/' so the library sub-folders (joined
            // with '/') match the catalog's native-separator paths.
            const QString normFile = QDir::fromNativeSeparators(filePath);
            QString normFolder = QDir::fromNativeSeparators(filterFolder_);
            while (normFolder.endsWith(QLatin1Char('/')))
                normFolder.chop(1);
            // 边界匹配:避免 C:/Photos 误含 C:/Photoshop/... (boundary match so a
            // folder prefix never swallows a sibling like C:/Photoshop/).
            const bool matched =
                normFile == normFolder ||
                normFile.startsWith(normFolder + QLatin1Char('/'),
                                    Qt::CaseInsensitive);
            if (!matched)
                continue;
        }
        if (filterCollectionId_ >= 0 &&
            !collectionMembers.contains(static_cast<int>(rec.id)))
            continue;
        if (filterKeywordId_ >= 0 &&
            !keywordMembers.contains(static_cast<int>(rec.id)))
            continue;
        if (filterSmartCollectionId_ >= 0 &&
            !smartMembers.contains(static_cast<int>(rec.id)))
            continue;
        if (!filterSearchText_.isEmpty() &&
            !searchMembers.contains(static_cast<int>(rec.id)))
            continue;
        records_.insert(static_cast<int>(rec.id), rec);
        // The thumbnail is left empty on purpose -- the filmstrip stores metadata and
        // emits thumbnailRequested for visible rows, and the
        // background thumb loader decodes and fills the tiles asynchronously.
        FilmstripWidget::Entry entry{
            static_cast<int>(rec.id), filePath,
            QString::fromStdString(rec.thumbnailPath), QImage(), rec.rating,
            rec.flag, rec.labelColor,
            imageCollections.value(static_cast<int>(rec.id))};
        entries_.append(entry);
        if (rec.rating > 0)
            ++rated;
        // Append one row per virtual copy right after its master. The copy inherits
        // the master's metadata/thumbnail/collections and only differs
        // by variantId + variantName (independent edit stack).
        for (const auto& variant : catalog_->listVariants(rec.id)) {
            if (variant.isMaster)
                continue;
            FilmstripWidget::Entry copy = entry;
            copy.variantId = static_cast<int>(variant.variantId);
            copy.variantName = variant.name;
            entries_.append(copy);
        }
    }
    filmstrip_->setImages(entries_);
    navigator_->setStatistics(entries_.size(), rated);
    navigator_->setFolders(toQStringList(catalog_->listFolders()));
    // Keep the navigator keyword counts and smart-collection list fresh.
    navigator_->setKeywords(catalog_->listKeywords());
    navigator_->setSmartCollections(catalog_->listSmartCollections());
    // Rebuild the navigator virtual-copies group only when the library's image set
    // changed (import/remove) or after an explicit invalidation, so
    // per-refresh N+1 variant queries stay bounded.
    if (!virtualCopiesBuilt_ ||
        static_cast<int>(images.size()) != lastVcImageCount_) {
        buildNavigatorVirtualCopies();
        lastVcImageCount_ = static_cast<int>(images.size());
        virtualCopiesBuilt_ = true;
    }
    refreshLibraryFolders();
    // Re-select the current image. When it survived the refresh we only restore
    // the visual highlight — the viewport/panel/undo state already reflect it,
    // so letting selectionChanged replay through onImageSelected would clear the
    // undo history and force a full RAW decode for no benefit. Restore the exact
    // variant row (a master and its copies share the imageId).
    if (currentImageId_ >= 0 && records_.contains(currentImageId_)) {
        const QSignalBlocker blocker(filmstrip_);
        if (currentVariantId_ != 0)
            filmstrip_->selectVariant(currentImageId_, currentVariantId_);
        else
            filmstrip_->selectImage(currentImageId_);
    } else if (!entries_.isEmpty() && currentImageId_ >= 0) {
        // 当前图被筛选掉:选中新列表第一张,走正常加载路径 (current image was
        // filtered out: select the first entry through the normal load path).
        filmstrip_->selectImage(entries_.first().imageId);
    } else if (entries_.isEmpty()) {
        // 空列表:清空视图与编辑状态,避免残留旧图 (empty list: clear the view
        // and edit state so no stale image lingers).
        currentImageId_ = -1;
        currentVariantId_ = 0;
        currentFilePath_.clear();
        viewport_->setImage(QImage());
        developPanel_->setAdjustments(lrc::Adjustments::defaults());
        // Clear the metadata/keyword panel so no stale photo's info lingers.
        developPanel_->setMetadata(lrc::PhotoMetadata{},
                                   std::vector<std::string>{});
        undoStack_.clear();
        clearFullRes();
        fullResActive_ = false;
        viewport_->setNativeImageSize(QSize());
        viewport_->setMaskTool(lrc::MaskType::None);
    }
}

void MainWindow::refreshLibraryFolders() {
    // Collect the by-date sub-folders of the managed library for the
    // "目录图片库" navigator group. A single unfiltered listing feeds the set;
    // only managed records under the library root are considered.
    const QString libRoot = QDir::fromNativeSeparators(libraryRoot_);
    QSet<QString> folders;
    if (!libraryRoot_.isEmpty()) {
        for (const auto& rec : catalog_->listImages()) {
            if (!rec.isManaged)
                continue;
            const QString filePath =
                QDir::fromNativeSeparators(QString::fromStdString(rec.filePath));
            if (!filePath.startsWith(libRoot, Qt::CaseInsensitive))
                continue;
            QString rel = filePath.mid(libRoot.size());
            while (rel.startsWith(QLatin1Char('/')))
                rel.remove(0, 1);
            while (rel.endsWith(QLatin1Char('/')))
                rel.chop(1);
            const int slash = rel.lastIndexOf(QLatin1Char('/'));
            if (slash > 0)
                folders.insert(rel.left(slash));
        }
    }
    QStringList subfolders(folders.constBegin(), folders.constEnd());
    subfolders.sort(Qt::CaseInsensitive);
    navigator_->setLibrary(libraryRoot_, subfolders);
}

void MainWindow::setFilterFolder(const QString& folder) {
    filterFolder_ = folder;
}

// --- Integration implementation ------------------------------------------------

void MainWindow::disarmSpotToolButton() {
    if (spotToolAction_) {
        const QSignalBlocker blocker(spotToolAction_);
        spotToolAction_->setChecked(false);
    }
}

void MainWindow::syncSpotStateFromAdjustments() {
    viewport_->setSpotRemovals(currentAdjustments_.spotRemovals);
    developPanel_->setCurrentSpotCount(
        static_cast<int>(currentAdjustments_.spotRemovals.size()));
}

void MainWindow::onSpotToolActiveChanged(bool active) {
    if (!spotToolAction_)
        return;
    // The viewport is the source of truth; the toolbar button mirrors it. The
    // blocker stops setChecked from bouncing back into the toggled handler.
    const QSignalBlocker blocker(spotToolAction_);
    spotToolAction_->setChecked(active);
}

void MainWindow::onSpotRemovalsEdited(
    const std::vector<lrc::SpotRemoval>& spots) {
    if (currentImageId_ < 0)
        return;
    // First emission of a gesture snapshots the pre-stroke state as the undo
    // restore point; the trailing (release) emission is committed by the idle
    // timer below (one drag/add/delete = one undo step, mirroring the mask
    // stroke pattern).
    if (!spotStrokeActive_) {
        spotStrokeActive_ = true;
        undoStack_.push(currentAdjustments_);
    }
    currentAdjustments_.spotRemovals = spots;
    developPanel_->setCurrentSpotCount(static_cast<int>(spots.size()));
    requestAdjust(currentAdjustments_);
    if (fullResActive_)
        requestAdjustFullRes();
    pendingSave_ = currentAdjustments_;
    pendingSaveImageId_ = currentImageId_;
    pendingSaveVariantId_ = currentVariantId_;
    if (saveDebounceTimer_)
        saveDebounceTimer_->start();
    if (spotCommitTimer_)
        spotCommitTimer_->start();
}

void MainWindow::onSpotCommitTimeout() {
    if (!spotStrokeActive_)
        return;
    spotStrokeActive_ = false;
    // Push the final state + persist (commitAdjustments debounces the DB write
    // to the save timer, so the persistence still leaves the hot path).
    commitAdjustments(currentAdjustments_);
}

void MainWindow::createVirtualCopy() {
    if (currentImageId_ < 0) {
        QMessageBox::information(this, tr("创建虚拟副本"),
                                 tr("请先选择一张照片。"));
        return;
    }
    const int newVariant = catalog_->createVirtualCopy(currentImageId_);
    if (newVariant < 0) {
        QMessageBox::warning(this, tr("创建虚拟副本"),
                             tr("创建虚拟副本失败。"));
        return;
    }
    Q_UNUSED(newVariant);
    // Invalidate the navigator virtual-copies group so refreshFilmstrip rebuilds
    // it (the variant count changed even though the image count did not).
    virtualCopiesBuilt_ = false;
    refreshFilmstrip();
    statusBar()->showMessage(tr("已创建虚拟副本"), 2500);
}

void MainWindow::onVirtualCopySelected(int64_t imageId, int variantId) {
    loadImageState(static_cast<int>(imageId), variantId);
    // Highlight the row without replaying the selection signals (blocked).
    if (filmstrip_) {
        const QSignalBlocker blocker(filmstrip_);
        filmstrip_->selectVariant(static_cast<int>(imageId), variantId);
    }
}

void MainWindow::buildNavigatorVirtualCopies() {
    std::vector<std::pair<int64_t, lrc::EditVariant>> copies;
    const auto images = catalog_->listImages();
    copies.reserve(images.size());
    for (const auto& rec : images) {
        for (const auto& v : catalog_->listVariants(rec.id))
            copies.emplace_back(rec.id, v);
    }
    navigator_->setVirtualCopies(copies);
}

void MainWindow::clearImageFilters() {
    filterFolder_.clear();
    filterCollectionId_ = -1;
    filterKeywordId_ = -1;
    filterSmartCollectionId_ = -1;
    filterSearchText_.clear();
    if (searchDebounceTimer_)
        searchDebounceTimer_->stop();
    if (searchEdit_) {
        const QSignalBlocker blocker(searchEdit_);
        searchEdit_->clear();
    }
}

void MainWindow::onCompareModeChanged(ImageViewport::CompareMode mode) {
    // Mirror the viewport's compare mode onto the toolbar (blocked so setChecked
    // can never bounce back into buttonClicked).
    if (compareButtonGroup_) {
        const QSignalBlocker blocker(compareButtonGroup_);
        if (QAbstractButton* button =
                compareButtonGroup_->button(static_cast<int>(mode)))
            button->setChecked(true);
    }
    // Entering a split comparison (re)renders the before/after pair. The
    // viewport keeps the "after" side live through the normal preview, so this
    // request supplies the zero-adjustment "before" render. Reference mode is
    // served by requestReference instead and needs no before/after pair.
    const bool isSplit =
        mode == ImageViewport::CompareMode::LeftRight ||
        mode == ImageViewport::CompareMode::TopBottom ||
        mode == ImageViewport::CompareMode::SingleToggle;
    // Exiting to Off (toolbar "关" or closing the reference) drops the reference
    // so the next "参考视图…" opens the picker again.
    if (mode == ImageViewport::CompareMode::Off)
        referenceImageId_ = -1;
    if (isSplit && currentImageId_ >= 0 && previewWorker_) {
        // A split mode overrides an active reference view; drop the stale ref.
        referenceImageId_ = -1;
        ++previewGeneration_;
        QMetaObject::invokeMethod(
            previewWorker_, "requestBeforeAfter", Qt::QueuedConnection,
            Q_ARG(qint64, currentImageId_), Q_ARG(qint64, previewGeneration_),
            Q_ARG(lrc::Adjustments, currentAdjustments_),
            Q_ARG(lrc::Adjustments, lrc::Adjustments::defaults()),
            Q_ARG(int, lrc::app::kPreviewMaxDim), Q_ARG(bool, true));
    }
}

void MainWindow::onBeforeAfterReady(qint64 imageId, const QImage& before,
                                    const QImage& after) {
    // The worker already guards the generation internally; only match the image.
    if (imageId != currentImageId_)
        return;
    viewport_->setBeforeAfterImages(before, after);
}

void MainWindow::toggleReferenceView() {
    if (referenceImageId_ >= 0) {
        viewport_->setReferenceMode(false);
        referenceImageId_ = -1;
        statusBar()->showMessage(tr("参考视图已关闭"), 2000);
        return;
    }
    const auto images = catalog_->listImages();
    if (images.empty())
        return;
    QStringList items;
    items.reserve(static_cast<int>(images.size()));
    for (const auto& rec : images) {
        // Prefix with the catalog id so duplicate file names stay distinguishable.
        items << QStringLiteral("%1: %2")
                     .arg(static_cast<qlonglong>(rec.id))
                     .arg(QFileInfo(QString::fromStdString(rec.filePath))
                              .fileName());
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, tr("参考视图"), tr("选择参考照片:"), items, 0, false, &ok);
    if (!ok || chosen.isEmpty())
        return;
    const int colon = chosen.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return;
    bool idOk = false;
    const int refId = chosen.left(colon).toInt(&idOk);
    if (!idOk || refId < 0)
        return;
    if (refId == currentImageId_) {
        statusBar()->showMessage(tr("参考照片与当前照片相同"), 3000);
        return;
    }
    referenceImageId_ = refId;
    const auto refAdj =
        catalog_->loadAdjustments(refId).value_or(lrc::Adjustments::defaults());
    ++previewGeneration_;
    QMetaObject::invokeMethod(previewWorker_, "requestReference",
                              Qt::QueuedConnection, Q_ARG(qint64, refId),
                              Q_ARG(qint64, previewGeneration_),
                              Q_ARG(lrc::Adjustments, refAdj),
                              Q_ARG(int, lrc::app::kPreviewMaxDim));
    statusBar()->showMessage(tr("正在加载参考视图…"));
}

void MainWindow::onReferenceReady(qint64 refImageId, const QImage& image) {
    if (refImageId != referenceImageId_)
        return;
    if (image.isNull()) {
        referenceImageId_ = -1;
        statusBar()->showMessage(tr("参考照片加载失败"), 4000);
        return;
    }
    viewport_->setReferenceImage(image);
    viewport_->setReferenceMode(true);
    statusBar()->showMessage(tr("参考视图"), 2500);
}

std::vector<std::string> MainWindow::keywordFullPathsForImage(
    int imageId) const {
    std::vector<std::string> out;
    const auto keywords = catalog_->listKeywords();
    std::unordered_map<int64_t, std::string> pathById;
    pathById.reserve(keywords.size());
    for (const auto& k : keywords)
        pathById.emplace(k.id, k.fullPath);
    for (const int64_t kid : catalog_->keywordsForImage(imageId)) {
        const auto it = pathById.find(kid);
        if (it != pathById.end())
            out.push_back(it->second);
    }
    return out;
}

void MainWindow::onMetadataEdited(
    const std::string& title, const std::string& caption,
    const std::string& copyright, const std::string& creator,
    const std::vector<std::string>& keywords) {
    if (currentImageId_ < 0)
        return;
    // Preserve the existing capture date (not part of the panel signal).
    const auto md = catalog_->getPhotoMetadata(currentImageId_);
    catalog_->setPhotoMetadata(currentImageId_, title, caption, copyright,
                               creator, md.dateTaken);
    // Keyword diff: build path<->id maps once.
    const auto allKw = catalog_->listKeywords();
    std::unordered_map<int64_t, std::string> pathById;
    std::unordered_map<std::string, int64_t> idByPath;
    for (const auto& k : allKw) {
        pathById.emplace(k.id, k.fullPath);
        idByPath.emplace(k.fullPath, k.id);
    }
    std::set<std::string> oldPaths;
    for (const int64_t oldId : catalog_->keywordsForImage(currentImageId_)) {
        const auto it = pathById.find(oldId);
        if (it != pathById.end())
            oldPaths.insert(it->second);
    }
    const std::set<std::string> newPaths(keywords.begin(), keywords.end());
    // Assign newly added keywords (ensureKeyword creates the hierarchy).
    for (const auto& path : keywords) {
        if (!oldPaths.count(path))
            catalog_->assignKeyword(currentImageId_,
                                    catalog_->ensureKeyword(path));
    }
    // Remove keywords that are no longer present.
    for (const auto& old : oldPaths) {
        if (!newPaths.count(old)) {
            const auto it = idByPath.find(old);
            if (it != idByPath.end())
                catalog_->removeKeyword(currentImageId_, it->second);
        }
    }
    navigator_->setKeywords(catalog_->listKeywords());
    statusBar()->showMessage(tr("元数据已保存"), 2500);
}

void MainWindow::onKeywordSelected(int64_t keywordId, const QString&) {
    filterKeywordId_ = keywordId;
    filterFolder_.clear();
    filterCollectionId_ = -1;
    filterSmartCollectionId_ = -1;
    filterSearchText_.clear();
    if (searchDebounceTimer_)
        searchDebounceTimer_->stop();
    if (searchEdit_) {
        const QSignalBlocker blocker(searchEdit_);
        searchEdit_->clear();
    }
    refreshFilmstrip();
}

void MainWindow::applySearchFilter() {
    if (!searchEdit_)
        return;
    filterSearchText_ = searchEdit_->text().trimmed();
    // Search replaces the navigator-driven filters (clear the others).
    filterFolder_.clear();
    filterCollectionId_ = -1;
    filterKeywordId_ = -1;
    filterSmartCollectionId_ = -1;
    refreshFilmstrip();
}

void MainWindow::onSmartCollectionSelected(int64_t smartCollectionId) {
    filterSmartCollectionId_ = smartCollectionId;
    filterFolder_.clear();
    filterCollectionId_ = -1;
    filterKeywordId_ = -1;
    filterSearchText_.clear();
    if (searchDebounceTimer_)
        searchDebounceTimer_->stop();
    if (searchEdit_) {
        const QSignalBlocker blocker(searchEdit_);
        searchEdit_->clear();
    }
    refreshFilmstrip();
}

void MainWindow::onNewSmartCollectionRequested() {
    SmartCollectionsDialog dlg(catalog_.get(), this);
    connect(&dlg, &SmartCollectionsDialog::smartCollectionsChanged, this,
            [this]() {
                navigator_->setSmartCollections(
                    catalog_->listSmartCollections());
                refreshFilmstrip();
            });
    dlg.exec();
}

void MainWindow::mergePanorama() {
    const auto images = catalog_->listImages();
    if (images.size() < 2) {
        QMessageBox::information(
            this, tr("全景合成"),
            tr("目录中至少需要 2 张照片才能进行全景合成。"));
        return;
    }
    // Multi-select source picker: the filmstrip is single-select, so a small
    // inline dialog is the multi-select entry point for panorama sources.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("全景合成 — 选择源照片"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(
        tr("选择 2–20 张重叠的照片（按目录顺序拼接）："), &dlg));
    auto* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setSelectionBehavior(QAbstractItemView::SelectRows);
    QVector<int> rowId;
    rowId.reserve(static_cast<int>(images.size()));
    for (const auto& rec : images) {
        new QListWidgetItem(
            QFileInfo(QString::fromStdString(rec.filePath)).fileName(), list);
        rowId.append(static_cast<int>(rec.id));
    }
    layout->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QVector<int> selIds;
    for (QListWidgetItem* item : list->selectedItems()) {
        const int row = list->row(item);
        if (row >= 0 && row < rowId.size())
            selIds.append(rowId[row]);
    }
    if (selIds.size() < 2) {
        QMessageBox::information(this, tr("全景合成"),
                                 tr("请至少选择 2 张照片。"));
        return;
    }
    if (selIds.size() > 20) {
        QMessageBox::information(this, tr("全景合成"),
                                 tr("最多支持 20 张照片。"));
        return;
    }
    // Decode sources to linear ProPhoto. Prefer the import-time standard preview
    // (2048px linear F32, no RAW decode); fall back to a bounded direct decode.
    statusBar()->showMessage(tr("正在解码全景源照片…"));
    std::vector<lrc::ImageBufferPtr> buffers;
    buffers.reserve(static_cast<size_t>(selIds.size()));
    for (int id : selIds) {
        if (auto sp = catalog_->getStandardPreview(id)) {
            if (*sp && !(*sp)->empty()) {
                buffers.push_back(*sp);
                continue;
            }
        }
        std::string path;
        const auto rit = records_.constFind(id);
        if (rit != records_.constEnd()) {
            path = rit->filePath;
        } else {
            for (const auto& rec : images) {
                if (rec.id == id) {
                    path = rec.filePath;
                    break;
                }
            }
        }
        if (path.empty())
            continue;
        lrc::DecodeParams params;
        params.halfSize = true;
        params.maxDim = lrc::app::kPreviewMaxDim;
        auto decoded =
            decoder_.decode(toStdPath(QString::fromStdString(path)), params);
        if (decoded && decoded->linear && !decoded->linear->empty())
            buffers.push_back(decoded->linear);
    }
    if (buffers.size() < 2) {
        QMessageBox::warning(this, tr("全景合成"),
                             tr("源照片解码失败，无法合成。"));
        return;
    }
    panoramaSourceIds_.clear();
    for (int id : selIds)
        panoramaSourceIds_.push_back(id);
    PanoramaDialog pano(this);
    connect(&pano, &PanoramaDialog::mergedReady, this,
            &MainWindow::onPanoramaMerged);
    pano.setSources(buffers);
    pano.exec();
}

lrc::ImageBufferPtr MainWindow::f32ToU16(
    const lrc::ImageBufferPtr& f32) const {
    if (!f32 || f32->format() != lrc::PixelFormat::F32 || f32->empty())
        return {};
    auto out = lrc::ImageBuffer::create(f32->width(), f32->height(),
                                        lrc::PixelFormat::U16);
    if (!out)
        return {};
    const float* src = reinterpret_cast<const float*>(f32->data().data());
    uint16_t* dst = reinterpret_cast<uint16_t*>(out->data().data());
    const size_t n = f32->data().size() / sizeof(float);
    for (size_t i = 0; i < n; ++i) {
        float v = src[i];
        if (v < 0.0f)
            v = 0.0f;
        else if (v > 1.0f)
            v = 1.0f;
        dst[i] = static_cast<uint16_t>(v * 65535.0f + 0.5f);
    }
    return out;
}

void MainWindow::onPanoramaMerged(const lrc::ImageBufferPtr& result) {
    if (!result || result->empty()) {
        QMessageBox::warning(this, tr("全景合成"), tr("合成结果为空。"));
        return;
    }
    // Output path: managed-library Panoramas folder, else the current photo's
    // directory, else the home directory.
    QString outDir;
    if (!libraryRoot_.isEmpty())
        outDir = libraryRoot_ + QStringLiteral("/Panoramas");
    else if (!currentFilePath_.isEmpty())
        outDir = QFileInfo(currentFilePath_).absolutePath();
    else
        outDir = QDir::homePath();
    QDir().mkpath(outDir);
    const QString baseName = currentFilePath_.isEmpty()
                                 ? QStringLiteral("panorama")
                                 : QFileInfo(currentFilePath_).completeBaseName();
    const QString outPath =
        outDir + QStringLiteral("/") + baseName + QStringLiteral("_panorama.dng");
    // Convert the linear F32 result to linear U16 for the DNG writer (the
    // stitcher always returns F32; handle U16 defensively).
    const lrc::ImageBufferPtr u16 =
        result->format() == lrc::PixelFormat::U16 ? result : f32ToU16(result);
    if (!u16 || !lrc::ImageIO::writeDng(toStdPath(outPath), *u16)) {
        QMessageBox::warning(this, tr("全景合成"), tr("写入全景 DNG 失败。"));
        return;
    }
    const QString normOut = QDir::fromNativeSeparators(outPath);
    const QString normRoot = QDir::fromNativeSeparators(libraryRoot_);
    const bool managed =
        !libraryRoot_.isEmpty() && normOut.startsWith(normRoot, Qt::CaseInsensitive);
    const int64_t newId = catalog_->addImage(outPath.toStdString(), "", managed);
    if (newId < 0) {
        QMessageBox::warning(this, tr("全景合成"), tr("导入全景照片失败。"));
        return;
    }
    catalog_->recordPanoramaSources(newId, panoramaSourceIds_);
    virtualCopiesBuilt_ = false;
    refreshFilmstrip();
    if (records_.contains(static_cast<int>(newId))) {
        const QSignalBlocker blocker(filmstrip_);
        filmstrip_->selectImage(static_cast<int>(newId));
        loadImageState(static_cast<int>(newId), 0);
    }
    QMessageBox::information(this, tr("全景合成"),
                             tr("全景照片已生成并加入目录。"));
}

QImage MainWindow::toQImage(const lrc::ImageBufferPtr& buffer) const {
    if (!buffer || buffer->format() != lrc::PixelFormat::U8 ||
        buffer->empty())
        return {};
    // Zero-copy: the QImage borrows the buffer's pixel data; a copy of the
    // shared_ptr held in the cleanup info keeps the buffer alive for the
    // lifetime of the QImage (buffer is immutable once rendered/cached).
    auto* holder = new lrc::ImageBufferPtr(buffer);  // keeps buffer alive
    QImage img(buffer->data().data(), buffer->width(), buffer->height(),
               buffer->width() * 3, QImage::Format_RGB888,
               [](void* ptr) { delete static_cast<lrc::ImageBufferPtr*>(ptr); },
               holder);
    // `holder` may leak if QImage construction fails; clean it up.
    if (img.isNull()) { delete holder; return {}; }
    return img;
}
