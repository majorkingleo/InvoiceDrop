#pragma once

#include "analysis.h"
#include "inboxwatcher.h"
#include "notifier.h"
#include "ollama.h"
#include "store.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace InvoiceDrop {

struct DaemonOptions {
    Extract::ReadOptions read;
    OllamaOptions ollama;
    QString inbox;

    bool notify = true;

    /// Keep running after the inbox has been drained.
    bool watch = true;
};

/// The background service: watches the inbox, reads what lands in it, stores the
/// result and says so.
///
/// The whole point is that the model stays loaded. Reading a document in a fresh
/// process pays the weight load every time, which dominates the wall clock for a
/// single file.
class Daemon : public QObject
{
    Q_OBJECT

public:
    Daemon(DaemonOptions options, Store *store, QObject *parent = nullptr);

    /// Claims the session bus name and starts watching. Fails when another
    /// daemon already owns the name, which is the desired behaviour.
    bool start(QString *error);

    /// Reads everything currently in the inbox and returns the number of files
    /// handled. Used by `daemon --once`.
    int drainInbox();

    /// Analyses a list of paths and returns one JSON object per bill, newline
    /// separated. This is what the DBus method and the delegating CLI return.
    QString analyzePaths(const QStringList &paths);

    /// Recent bills as newline separated JSON.
    QString historyJson(int limit) const;

    /// What the daemon is doing, for `status` and for the widget.
    QVariantMap status() const;

    bool isBusy() const { return m_busy; }

    static constexpr auto kServiceName = "org.kde.invoicedrop";
    static constexpr auto kObjectPath = "/InvoiceDrop";

private:
    void handleFile(const QString &path);
    void notify(const QString &path, const QVector<BillResult> &bills);
    void print(const QVector<BillResult> &bills);

    DaemonOptions m_options;
    Store *m_store = nullptr;
    OllamaClient m_client;
    Notifier m_notifier;
    InboxWatcher *m_watcher = nullptr;

    bool m_busy = false;
    int m_processed = 0;
    int m_failed = 0;
    QString m_lastFile;

    /// Files that arrived while a document was being read. The HTTP wait runs a
    /// local event loop, so watcher signals can arrive in the middle of a job.
    QStringList m_deferred;
};

} // namespace InvoiceDrop
