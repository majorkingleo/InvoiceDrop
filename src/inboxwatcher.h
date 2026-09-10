#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QTimer>

namespace InvoiceDrop {

/// Watches a folder and reports files once they have stopped growing.
///
/// A dropped file is not readable the instant it appears: a copy from a camera
/// or a browser download is still being written, and reading it early yields a
/// truncated PDF and a confusing error. The watcher therefore waits until the
/// size has been unchanged for two ticks before it says anything.
class InboxWatcher : public QObject
{
    Q_OBJECT

public:
    explicit InboxWatcher(const QString &directory, QObject *parent = nullptr);

    /// Creates the folder and starts watching. Also queues what is already
    /// there, because a file dropped while the daemon was down still needs
    /// reading.
    bool start(QString *error);

    QString directory() const { return m_directory; }

    /// Files seen but not yet reported as ready.
    QStringList pending() const;

    /// Returns the pending files and forgets them, so a caller that reads them
    /// itself does not get them delivered a second time by the timer.
    QStringList takePending();

    /// True when the file suffix is one the reader handles.
    static bool isSupported(const QString &path);

signals:
    /// Emitted once per file, after the size stopped changing.
    void fileReady(const QString &path);

private:
    void scan();
    void queue(const QString &path);

    QString m_directory;
    QFileSystemWatcher m_watcher;
    QTimer m_timer;

    /// Size seen at the previous tick, and how many ticks it has held.
    QHash<QString, qint64> m_lastSize;
    QHash<QString, int> m_stableTicks;

    /// Files already reported, with the size and modification time they had at
    /// that moment. Without this the next scan would queue them all over again,
    /// because a folder scan has no memory of its own.
    QHash<QString, QPair<qint64, qint64>> m_handled;
};

} // namespace InvoiceDrop
