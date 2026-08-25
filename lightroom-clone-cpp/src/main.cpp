#include "app/Application.h"
#include "catalog/LibraryManager.h"
#include "ui/MainWindow.h"
#include "ui/MetaTypes.h"

int main(int argc, char** argv) {
    qRegisterMetaType<lrc::ImageBufferPtr>();
    qRegisterMetaType<lrc::Adjustments>();
    // Scoped enum passed through importPaths' queued invocation.
    qRegisterMetaType<lrc::ImportMode>("lrc::ImportMode");

    Application app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
