#include "app/Application.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QStandardPaths>
#include <QTextStream>

#include "app/Constants.h"

Application::Application(int& argc, char** argv) : QApplication(argc, argv) {
    setApplicationName(QString::fromLatin1(lrc::app::kAppName));
    setApplicationVersion(QString::fromLatin1(lrc::app::kAppVersion));
    setOrganizationName(QString::fromLatin1(lrc::app::kAppOrganization));
    setWindowIcon(QIcon(QString::fromLatin1(lrc::app::kAppIconResource)));
    applyDarkTheme(*this);
}

Application::~Application() = default;

bool Application::applyDarkTheme(QApplication& app) {
    QFile file(QString::fromLatin1(lrc::app::kDarkQssResource));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    app.setStyleSheet(QString::fromUtf8(file.readAll()));
    return true;
}

QString Application::defaultCatalogPath() {
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/SilverHalide");
    QDir().mkpath(base);
    // 目录即库: the default catalog is a *folder* named after the default
    // catalog (base/Default), holding Default.shcat + previews.shcat/.
    const QString folder = base + QLatin1Char('/') +
                           QString::fromLatin1(lrc::app::kDefaultCatalogName);

    // Migrate catalogs created before the folder-as-library refactor: while a
    // lone Default.lrcat / Default.shcat / catalog.lrcat still exists (and the
    // new folder has not been created yet), keep returning the legacy *file*
    // path so the versioned migration can externalize its preview BLOBs on the
    // next open instead of orphaning existing photos.
    const QString legacyFile = base + QLatin1Char('/') +
                               QString::fromLatin1(lrc::app::kDefaultCatalogFile);
    const QString legacyLrcat =
        base + QLatin1Char('/') + QString::fromLatin1(lrc::app::kDefaultCatalogName) +
        QStringLiteral(".lrcat");
    if (!QDir(folder).exists()) {
        if (QFileInfo::exists(legacyFile))
            return legacyFile;
        if (QFileInfo::exists(legacyLrcat))
            return legacyLrcat;
        const QString legacy = base + QStringLiteral("/catalog.lrcat");
        if (QFileInfo::exists(legacy))
            return legacy;
        // Pre-rename identity (<AppData>/LRC/LRC Clone Flash): keep using its
        // default catalog while it exists and no new catalog has been created.
        const QString appDataRoot = QStandardPaths::writableLocation(
            QStandardPaths::GenericDataLocation);
        const QString legacyBase =
            appDataRoot + QStringLiteral("/LRC/LRC Clone Flash");
        const QString legacyDefault = legacyBase + QLatin1Char('/') +
                                      QString::fromLatin1(lrc::app::kDefaultCatalogFile);
        const QString legacyDefaultLrcat =
            legacyBase + QLatin1Char('/') + QString::fromLatin1(lrc::app::kDefaultCatalogName) +
            QStringLiteral(".lrcat");
        const QString legacyCatalog = legacyBase + QStringLiteral("/catalog.lrcat");
        if (QFileInfo::exists(legacyDefault))
            return legacyDefault;
        if (QFileInfo::exists(legacyDefaultLrcat))
            return legacyDefaultLrcat;
        if (QFileInfo::exists(legacyCatalog))
            return legacyCatalog;
    }
    QDir().mkpath(folder);
    return folder;
}

// Resolves the SQLite library file for a catalog location. A folder-as-library
// path yields <folder>/<folderName>.shcat (design: <dir>/<stem>.shcat); a lone
// legacy .lrcat file is returned unchanged so the versioned migration can
// externalize its preview BLOBs.
QString Application::catalogDbFile(const QString& catalogPath) {
    const QFileInfo info(catalogPath);
    if (info.isFile())
        return catalogPath;
    const QString stem = info.fileName();
    return catalogPath + QLatin1Char('/') + stem +
           QString::fromLatin1(lrc::app::kCatalogDbSuffix);
}

// Resolves the preview side-directory for a catalog location. A folder yields
// <folder>/previews.shcat; a lone legacy file yields an empty dir (CatalogDatabase
// derives a sibling `<stem> Previews.shcat/` from the db file in that case).
QString Application::catalogPreviewDir(const QString& catalogPath) {
    if (QFileInfo(catalogPath).isFile())
        return QString();
    return catalogPath + QLatin1Char('/') +
           QString::fromLatin1(lrc::app::kPreviewSideDir);
}

QString Application::defaultCatalogName() {
    return QString::fromLatin1(lrc::app::kDefaultCatalogName);
}

QString Application::defaultCacheDir() {
    QString base = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/SilverHalide");
    QDir().mkpath(base);
    return base;
}
