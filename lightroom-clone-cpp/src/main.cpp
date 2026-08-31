#include "app/Application.h"
#include "catalog/LibraryManager.h"
#include "ui/MainWindow.h"
#include "ui/MetaTypes.h"
#include "ui/PreviewWorker.h"

int main(int argc, char** argv) {
    qRegisterMetaType<lrc::ImageBufferPtr>();
    qRegisterMetaType<lrc::Adjustments>();
    // Scoped enum passed through importPaths' queued invocation.
    qRegisterMetaType<lrc::ImportMode>("lrc::ImportMode");
    // S17-18: ExportItem + QList<ExportItem> cross the queued connection
    // from MainWindow to PreviewWorker::batchExport.
    qRegisterMetaType<ExportItem>();
    qRegisterMetaType<QList<ExportItem>>();

    Application app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
