#include "paths.h"

#include <QDir>
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

} // namespace InvoiceDrop::Paths
