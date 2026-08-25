#pragma once

// Thin QApplication subclass: applies the dark theme, sets application
// metadata, and computes default catalog/cache paths.

#include <QApplication>
#include <QString>

class Application : public QApplication {
public:
    Application(int& argc, char** argv);
    ~Application() override;

    // Loads the dark QSS bundle. Returns false if the resource is missing.
    static bool applyDarkTheme(QApplication& app);

    // <AppData>/ASE-128/Silver Halide/Default.lrcat. Falls back to the
    // pre-rename identity (<AppData>/LRC/LRC Clone Flash) and to the legacy
    // catalog.lrcat while they still exist and a new Default.lrcat has not
    // been created yet, so existing photos are not orphaned.
    static QString defaultCatalogPath();
    // Display name of the built-in default catalog ("Default").
    static QString defaultCatalogName();
    // <AppData>/ASE-128/Silver Halide/cache
    static QString defaultCacheDir();
};
