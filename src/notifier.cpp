#include "notifier.h"

#include "analysis.h"
#include "invoice.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QLocale>
#include <QStringList>
#include <QVariant>

namespace InvoiceDrop {
namespace {

constexpr auto kService = "org.freedesktop.Notifications";
constexpr auto kPath = "/org/freedesktop/Notifications";
constexpr auto kInterface = "org.freedesktop.Notifications";
constexpr auto kAppName = "InvoiceDrop";

} // namespace

Notifier::Notifier(bool enabled) : m_enabled(enabled) {}

bool Notifier::isAvailable() const
{
    if (!m_enabled)
        return false;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    QDBusConnectionInterface *interface = bus.interface();
    return interface && interface->isServiceRegistered(QString::fromLatin1(kService));
}

void Notifier::notify(const QString &summary, const QString &body, bool urgent)
{
    if (!isAvailable())
        return;

    QDBusInterface notifications(QString::fromLatin1(kService), QString::fromLatin1(kPath),
                                 QString::fromLatin1(kInterface), QDBusConnection::sessionBus());

    // The hint is what makes a failure stand out instead of joining the stack.
    QVariantMap hints;
    if (urgent)
        hints.insert(QStringLiteral("urgency"), QVariant::fromValue<unsigned char>(2));

    QDBusReply<uint> reply = notifications.call(
        QStringLiteral("Notify"), QString::fromLatin1(kAppName),
        static_cast<uint>(0),                          // replaces an earlier toast
        QStringLiteral("invoice"),                     // icon name
        summary, body, QStringList(), hints,
        static_cast<int>(8000));                       // milliseconds

    // A failing toast is not worth failing a run over, so nothing is reported.
    Q_UNUSED(reply);
}

QString Notifier::bodyFor(const QString &fileName, const QVector<BillResult> &bills)
{
    QString body;
    for (const BillResult &bill : bills) {
        if (!body.isEmpty())
            body += QLatin1Char('\n');

        const QString where = bills.size() > 1
            ? QStringLiteral("%1 p%2").arg(fileName).arg(bill.page)
            : fileName;

        if (!bill.ok) {
            body += QStringLiteral("%1: %2").arg(where, bill.error);
            continue;
        }

        const QString amount = bill.invoice.grossTotal.has_value()
            ? QLocale::system().toString(*bill.invoice.grossTotal, 'f', 2) + QLatin1Char(' ')
                + bill.invoice.currency
            : QStringLiteral("?");
        const QString shop =
            bill.invoice.vendor.isEmpty() ? QStringLiteral("?") : bill.invoice.vendor;
        const QString date =
            bill.invoice.date.isEmpty() ? QStringLiteral("?") : bill.invoice.date;

        body += QStringLiteral("%1: %2, %3, %4").arg(where, shop, date, amount);
    }
    return body;
}

} // namespace InvoiceDrop
