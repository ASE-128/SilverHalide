#pragma once

/**
 * @brief Thin QApplication subclass for the desktop entry point.
 *
 * Applies the dark theme at startup, sets application metadata (name,
 * version, organization, icon) and computes the default on-disk
 * catalog/cache paths used during bootstrap.
 */

#include <QApplication>
#include <QString>

/**
 * @brief Qt application entry point.
 *
 * Owns the app lifecycle; wires the dark theme and application metadata on
 * construction and exposes the default catalog/cache locations used at
 * startup. Not thread-safe; intended to be created once on the main thread
 * before any other UI.
 */
class Application : public QApplication {
public:
    Application(int& argc, char** argv);
    ~Application() override;

    /**
     * @brief Loads and applies the built-in dark stylesheet bundle.
     *
     * @return true if the resource was found and applied, false if the QSS
     *         resource is missing.
     */
    static bool applyDarkTheme(QApplication& app);

    /**
     * @brief Resolves the default catalog location.
     *
     * The default catalog is a *folder* (目录即库) named "Default" under the
     * app-data dir. For backwards compatibility, while a pre-refactor lone
     * Default.lrcat / catalog.lrcat still exists and the new folder layout has
     * not been created, the legacy file path is returned instead so the
     * versioned migration can externalize its preview BLOBs on the next open;
     * it falls back to the pre-rename identity (<AppData>/LRC/LRC Clone Flash)
     * for the same reason.
     *
     * @return Absolute default catalog path (a folder, or a legacy file while
     *         migrating).
     */
    static QString defaultCatalogPath();

    /**
     * @brief Display name of the built-in default catalog.
     *
     * @return "Default".
     */
    static QString defaultCatalogName();

    /**
     * @brief Resolves the SQLite library file for a catalog location.
     *
     * The catalog is a *folder* (目录即库): a folder yields
     * <folder>/<folderName>.shcat. A lone legacy file (pre-refactor .lrcat) is
     * returned unchanged so the versioned migration can externalize its
     * preview BLOBs.
     *
     * @param catalogPath Catalog folder or legacy file path.
     * @return Path to the SQLite library file.
     */
    static QString catalogDbFile(const QString& catalogPath);

    /**
     * @brief Resolves the preview side-directory for a catalog location.
     *
     * A folder yields <folder>/previews.shcat. A lone legacy file yields an
     * empty dir (CatalogDatabase derives a sibling `<stem> Previews.shcat/`
     * from the db file when no dir is supplied).
     *
     * @param catalogPath Catalog folder or legacy file path.
     * @return Preview side-directory path (empty for a lone legacy file).
     */
    static QString catalogPreviewDir(const QString& catalogPath);

    /**
     * @brief Default cache directory.
     *
     * @return The writable app-data root directory that cache content is
     *         stored under.
     */
    static QString defaultCacheDir();
};
