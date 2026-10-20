#include <QLoggingCategory>

#include "app/Application.h"
#include "catalog/LibraryManager.h"
#include "ui/MainWindow.h"
#include "ui/MetaTypes.h"
#include "ui/PreviewWorker.h"

int main(int argc, char** argv) {
    // The [PERF1A] 1:1 diagnostics are opt-in: QLoggingCategory categories are
    // enabled by default, so install an explicit "off" rule. A user-provided
    // QT_LOGGING_RULES is left untouched (an explicit rule always wins).
    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"))
        QLoggingCategory::setFilterRules(QStringLiteral("lrc.perf.info=false"));
    qRegisterMetaType<lrc::ImageBufferPtr>();
    qRegisterMetaType<lrc::Adjustments>();
    // IM-6: queued preview/export requests carry a shared immutable adjustment
    // snapshot (one per edit) instead of a deep copy of the stack.
    qRegisterMetaType<lrc::AdjustmentsPtr>();
    // P1-4: watermark options cross the queued export connections.
    qRegisterMetaType<ExportWatermark>();
    // Scoped enum passed through importPaths' queued invocation.
    qRegisterMetaType<lrc::ImportMode>("lrc::ImportMode");
    // S17-18: ExportItem + QList<ExportItem> cross the queued connection
    // from MainWindow to PreviewWorker::batchExport.
    qRegisterMetaType<ExportItem>();
    qRegisterMetaType<QList<ExportItem>>();
    // P3-1: linear-light histogram bins cross the queued preview-worker
    // connection (worker thread -> UI thread).
    qRegisterMetaType<lrc::HistogramBins>();
    // Soft-proofing parameters are dispatched UI -> preview worker through a
    // queued invocation of requestProof().
    qRegisterMetaType<lrc::SoftProofParams>();

    Application app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
