#include "daemon.h"

#include "json.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace InvoiceDrop {
namespace {

/// What the widget and the shell see on the bus.
class ControlAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.invoicedrop.Control")
    Q_CLASSINFO("D-Bus Introspection",
                "  <interface name=\"org.kde.invoicedrop.Control\">\n"
                "    <method name=\"Analyze\">\n"
                "      <arg direction=\"in\" type=\"as\" name=\"paths\"/>\n"
                "      <arg direction=\"out\" type=\"s\" name=\"bills\"/>\n"
                "    </method>\n"
                "    <method name=\"History\">\n"
                "      <arg direction=\"in\" type=\"u\" name=\"limit\"/>\n"
                "      <arg direction=\"out\" type=\"s\" name=\"bills\"/>\n"
                "    </method>\n"
                "    <method name=\"Status\">\n"
                "      <arg direction=\"out\" type=\"a{sv}\" name=\"status\"/>\n"
                "    </method>\n"
                "  </interface>\n")

public:
    explicit ControlAdaptor(Daemon *daemon)
        : QDBusAbstractAdaptor(daemon), m_daemon(daemon)
    {
    }

public slots:
    QString Analyze(const QStringList &paths) { return m_daemon->analyzePaths(paths); }
    QString History(uint limit) { return m_daemon->historyJson(static_cast<int>(limit)); }
    QVariantMap Status() { return m_daemon->status(); }

private:
    Daemon *m_daemon;
};

} // namespace

Daemon::Daemon(DaemonOptions options, Store *store, QObject *parent)
    : QObject(parent),
      m_options(std::move(options)),
      m_store(store),
      m_client(m_options.ollama),
      m_notifier(m_options.notify)
{
}

bool Daemon::start(QString *error)
{
    if (!m_store || !m_store->isOpen()) {
        *error = QStringLiteral("the database is not open");
        return false;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        *error = QStringLiteral("no session bus, so nothing can reach the daemon");
        return false;
    }

    if (!bus.registerService(QString::fromLatin1(kServiceName))) {
        *error = QStringLiteral("%1 is already running (%2)")
                     .arg(QString::fromLatin1(kServiceName), bus.lastError().message());
        return false;
    }

    // The adaptor has to exist before the object is registered, and it must be a
    // child of the exported object.
    new ControlAdaptor(this);

    if (!bus.registerObject(QString::fromLatin1(kObjectPath), this,
                            QDBusConnection::ExportAdaptors)) {
        bus.unregisterService(QString::fromLatin1(kServiceName));
        *error = QStringLiteral("cannot export %1: %2")
                     .arg(QString::fromLatin1(kObjectPath), bus.lastError().message());
        return false;
    }

    m_watcher = new InboxWatcher(m_options.inbox, this);
    connect(m_watcher, &InboxWatcher::fileReady, this, &Daemon::handleFile);

    if (!m_watcher->start(error)) {
        bus.unregisterObject(QString::fromLatin1(kObjectPath));
        bus.unregisterService(QString::fromLatin1(kServiceName));
        return false;
    }

    m_notifier.notify(QStringLiteral("InvoiceDrop is watching"),
                      QStringLiteral("%1\nmodel %2")
                          .arg(m_options.inbox, m_options.ollama.model));
    return true;
}

void Daemon::handleFile(const QString &path)
{
    // Reading a document blocks on the HTTP reply, and that wait runs a local
    // event loop. Watcher signals therefore arrive in the middle of a job, and
    // without this guard the same file would be read twice.
    if (m_busy) {
        if (!m_deferred.contains(path))
            m_deferred.append(path);
        return;
    }

    QString current = path;
    while (!current.isEmpty()) {
        m_busy = true;
        const QVector<BillResult> bills =
            analyseFile(current, m_options.read, m_client, m_store);
        m_busy = false;

        m_lastFile = QFileInfo(current).fileName();
        notify(current, bills);
        print(bills);

        current = m_deferred.isEmpty() ? QString() : m_deferred.takeFirst();
    }
}

void Daemon::notify(const QString &path, const QVector<BillResult> &bills)
{
    bool allOk = !bills.isEmpty();
    for (const BillResult &bill : bills) {
        if (bill.ok) {
            ++m_processed;
        } else {
            ++m_failed;
            allOk = false;
        }
    }

    const QString name = QFileInfo(path).fileName();

    if (allOk) {
        m_notifier.notify(QStringLiteral("InvoiceDrop: %1").arg(name),
                          Notifier::bodyFor(name, bills));
    } else {
        m_notifier.notify(QStringLiteral("InvoiceDrop could not read %1").arg(name),
                          Notifier::bodyFor(name, bills), true);
    }
}

void Daemon::print(const QVector<BillResult> &bills)
{
    // The same JSON the CLI emits, so a daemon run can be piped anywhere a CLI
    // run can and the two can be compared directly.
    const QString json = billsToJsonLines(bills);
    if (json.isEmpty())
        return;
    fputs(json.toUtf8().constData(), stdout);
    fflush(stdout);
}

QVariantMap Daemon::status() const
{
    QVariantMap map;
    map.insert(QStringLiteral("busy"), m_busy);
    map.insert(QStringLiteral("model"), m_options.ollama.model);
    map.insert(QStringLiteral("endpoint"), m_options.ollama.url);
    map.insert(QStringLiteral("inbox"), m_options.inbox);
    map.insert(QStringLiteral("database"), m_store ? m_store->databasePath() : QString());
    map.insert(QStringLiteral("stored_bills"), m_store ? m_store->billCount() : 0);
    map.insert(QStringLiteral("processed"), m_processed);
    map.insert(QStringLiteral("failed"), m_failed);
    map.insert(QStringLiteral("last_file"), m_lastFile);
    map.insert(QStringLiteral("pending"), m_watcher ? m_watcher->pending() : QStringList());
    map.insert(QStringLiteral("notifications"), m_notifier.isAvailable());
    return map;
}

QString Daemon::historyJson(int limit) const
{
    if (!m_store)
        return {};

    QString out;
    for (const StoredBill &bill : m_store->recent(limit)) {
        QJsonObject object;
        object.insert(QStringLiteral("file"), bill.fileName);
        object.insert(QStringLiteral("page"), bill.page);
        object.insert(QStringLiteral("status"), bill.status);
        object.insert(QStringLiteral("vendor"), bill.vendor);
        object.insert(QStringLiteral("date"), bill.date);
        object.insert(QStringLiteral("gross_total"),
                      bill.grossTotal.has_value() ? QJsonValue(*bill.grossTotal)
                                                  : QJsonValue(QJsonValue::Null));
        object.insert(QStringLiteral("currency"), bill.currency);
        object.insert(QStringLiteral("model"), bill.model);
        object.insert(QStringLiteral("created_at"), bill.createdAt);

        out += QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
        out += QLatin1Char('\n');
    }
    return out;
}

QString Daemon::analyzePaths(const QStringList &paths)
{
    QString out;
    for (const QString &path : paths) {
        const QVector<BillResult> bills =
            analyseFile(path, m_options.read, m_client, m_store);
        out += billsToJsonLines(bills);
        notify(path, bills);
    }
    return out;
}

int Daemon::drainInbox()
{
    if (!m_watcher)
        return 0;

    int handled = 0;
    const QStringList paths = m_watcher->takePending();
    for (const QString &path : paths) {
        handleFile(path);
        ++handled;
    }
    return handled;
}

} // namespace InvoiceDrop

#include "daemon.moc"
