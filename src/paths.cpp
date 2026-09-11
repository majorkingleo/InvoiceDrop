#include "paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace InvoiceDrop::Paths {

QString dataDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString root = base.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : base;
    return QDir(root).filePath(QStringLiteral("invoicedrop"));
}

QString databaseFile()
{
    return QDir(dataDir()).filePath(QStringLiteral("invoicedrop.db"));
}

QString archiveDir()
{
    return QDir(dataDir()).filePath(QStringLiteral("archive"));
}

QString resolvePath(const QString &path)
{
    if (path.isEmpty())
        return path;

    // canonicalFilePath() resolves symlinks and `..` and returns an empty string
    // when the file is not there, which is exactly the case where the caller
    // wants to be told which path was tried. cleanPath() keeps that message
    // readable by folding `./` and `..` without touching the filesystem.
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;

    return QDir::cleanPath(info.absoluteFilePath());
}

} // namespace InvoiceDrop::Paths
