#include "inboxwatcher.h"

#include "extract/documentreader.h"

#include <QDir>
#include <QFileInfo>

namespace InvoiceDrop {
namespace {

/// How often the size is sampled. Two unchanged samples mean the file settled.
constexpr int kTickMs = 300;
constexpr int kSettledTicks = 2;

} // namespace

InboxWatcher::InboxWatcher(const QString &directory, QObject *parent)
    : QObject(parent), m_directory(directory)
{
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &InboxWatcher::scan);

    // The watch is on the folder, not on individual files: editors and copy
    // tools replace files rather than writing them in place, which silently
    // drops a per-file watch.
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { scan(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { scan(); });
}

bool InboxWatcher::isSupported(const QString &path)
{
    const QFileInfo info(path);

    // Hidden files are skipped. Editors and download tools write to a dot file
    // first and rename it into place, and ".pdf" has a suffix even though it has
    // no name.
    if (info.fileName().startsWith(QLatin1Char('.')))
        return false;

    const QString suffix = info.suffix();
    return !suffix.isEmpty() && Extract::isSupportedSuffix(suffix);
}

bool InboxWatcher::start(QString *error)
{
    QDir directory(m_directory);
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        *error = QStringLiteral("cannot create the inbox folder %1").arg(m_directory);
        return false;
    }

    if (!m_watcher.addPath(m_directory)) {
        *error = QStringLiteral("cannot watch %1").arg(m_directory);
        return false;
    }

    m_timer.start();
    scan();
    return true;
}

QStringList InboxWatcher::pending() const
{
    QStringList paths = m_lastSize.keys();
    paths.sort();
    return paths;
}

QStringList InboxWatcher::takePending()
{
    QStringList paths = pending();
    for (const QString &path : std::as_const(paths)) {
        m_lastSize.remove(path);
        m_stableTicks.remove(path);
        // Marked as handled, so the next scan does not queue it again.
        const QFileInfo info(path);
        if (info.exists())
            m_handled.insert(path, {info.size(), info.lastModified().toMSecsSinceEpoch()});
    }
    return paths;
}

void InboxWatcher::queue(const QString &path)
{
    if (!isSupported(path))
        return;

    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink())
        return;

    if (!m_lastSize.contains(path)) {
        m_lastSize.insert(path, -1);
        m_stableTicks.insert(path, 0);
    }
}

void InboxWatcher::scan()
{
    // A watch can be lost when the folder is replaced, so it is re-added.
    if (!m_watcher.directories().contains(m_directory))
        m_watcher.addPath(m_directory);

    const QDir inbox(m_directory);
    const QFileInfoList entries = inbox.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);

    // A file that is gone may come back, so the note about it goes away too.
    const QStringList handled = m_handled.keys();
    for (const QString &path : handled) {
        if (!QFileInfo::exists(path))
            m_handled.remove(path);
    }

    for (const QFileInfo &entry : entries) {
        const QString path = entry.absoluteFilePath();

        // Already reported and unchanged since: nothing to do. The size and the
        // modification time are compared, so a file dropped again after being
        // edited is picked up while an untouched one is left alone.
        const auto previous = m_handled.constFind(path);
        if (previous != m_handled.constEnd()
            && previous->first == entry.size()
            && previous->second == entry.lastModified().toMSecsSinceEpoch()) {
            continue;
        }

        queue(path);
    }

    const QStringList candidates = m_lastSize.keys();
    for (const QString &path : candidates) {
        const QFileInfo info(path);

        // A file that vanished before it settled is simply forgotten.
        if (!info.exists()) {
            m_lastSize.remove(path);
            m_stableTicks.remove(path);
            continue;
        }

        const qint64 size = info.size();
        if (size != m_lastSize.value(path)) {
            m_lastSize.insert(path, size);
            m_stableTicks.insert(path, 0);
            continue;
        }

        const int ticks = m_stableTicks.value(path) + 1;
        m_stableTicks.insert(path, ticks);

        if (ticks >= kSettledTicks) {
            m_lastSize.remove(path);
            m_stableTicks.remove(path);
            m_handled.insert(path, {size, info.lastModified().toMSecsSinceEpoch()});
            emit fileReady(path);
        }
    }
}

} // namespace InvoiceDrop
