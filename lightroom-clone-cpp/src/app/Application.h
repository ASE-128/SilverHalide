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

    // The catalog is a *folder* (目录即库). Returns the default catalog
    // folder <AppData>/ASE-128/Silver Halide/Default. For backwards
    // compatibility, while a pre-refactor lone Default.lrcat / catalog.lrcat
    // still exists and the new folder layout has not been created, the legacy
    // file path is returned instead so the versioned migration can externalize
    // its preview BLOBs on the next open. Falls back to the pre-rename identity
    // (<AppData>/LRC/LRC Clone Flash) for the same reason.
    static QString defaultCatalogPath();
    // Display name of the built-in default catalog ("Default").
    static QString defaultCatalogName();

    // --- folder-as-library path resolution (目录即库) -------------------------
    // Given a catalog location `catalogPath`, resolve the SQLite library file
    // and the preview side-directory.
    //   * A *folder* → <folder>/<folderName>.shcat and <folder>/previews.shcat.
    //   * A *lone legacy file* (pre-refactor .lrcat) → returned unchanged, with
    //     an empty preview dir (CatalogDatabase derives a sibling
    //     `<stem> Previews.shcat/` from the db file when no dir is supplied).
    static QString catalogDbFile(const QString& catalogPath);
    static QString catalogPreviewDir(const QString& catalogPath);

    // <AppData>/ASE-128/Silver Halide/cache
    static QString defaultCacheDir();
};
