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
    const QString def = base + QLatin1Char('/') +
                        QString::fromLatin1(lrc::app::kDefaultCatalogFile);
    // Migrate catalogs created under the pre-rename identity
    // (<AppData>/LRC/LRC Clone Flash) so existing photos are not orphaned:
    // keep using the old default catalog until a new one is created/opened.
    const QString appDataRoot = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    const QString legacyBase =
        appDataRoot + QStringLiteral("/LRC/LRC Clone Flash");
    const QString legacyDefault = legacyBase + QLatin1Char('/') +
                                  QString::fromLatin1(lrc::app::kDefaultCatalogFile);
    const QString legacyCatalog = legacyBase + QStringLiteral("/catalog.lrcat");
    if (!QFileInfo::exists(def)) {
        if (QFileInfo::exists(legacyDefault))
            return legacyDefault;
        if (QFileInfo::exists(legacyCatalog))
            return legacyCatalog;
    }
    // Migrate the pre-2.11 catalog within the current base: when the new
    // Default.lrcat has not been created yet but the legacy catalog.lrcat
    // exists, keep using the legacy path.
    const QString legacy = base + QStringLiteral("/catalog.lrcat");
    if (!QFileInfo::exists(def) && QFileInfo::exists(legacy))
        return legacy;
    return def;
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
