#include "totals.h"

#include <QLocale>
#include <QtNumeric>

namespace InvoiceDrop {
namespace {

/// Money is added up in cents.
///
/// Summing doubles gives 0.1 + 0.2 = 0.30000000000000004, and a total that is
/// wrong in the last digit is a total nobody checks twice. Cents are exact for
/// every amount that fits the two decimal places a receipt prints.
qint64 toCents(double amount)
{
    return qRound64(amount * 100.0);
}

double fromCents(qint64 cents)
{
    return static_cast<double>(cents) / 100.0;
}

/// `1 bill` or `3 bills`, so that no sentence ever says "1 bills".
QString billCount(int count)
{
    return count == 1 ? QStringLiteral("1 bill")
                      : QStringLiteral("%1 bills").arg(count);
}

} // namespace

FileTotal totalFor(const QVector<BillResult> &bills)
{
    FileTotal total;
    total.present = bills.size();

    // The page count is the number the file claims to hold, and it is the only
    // way to notice that a caller was handed fewer bills than the file has. A
    // widget that keeps the last eight bills can cut a group in half, and a sum
    // over the visible half would look exactly as authoritative as a full one.
    for (const BillResult &bill : bills)
        total.expected = qMax(total.expected, bill.pageCount);
    total.expected = qMax(total.expected, total.present);

    QVector<qint64> cents;

    for (const BillResult &bill : bills) {
        if (!bill.ok) {
            total.failed += 1;
            continue;
        }
        if (!bill.invoice.grossTotal.has_value())
            continue;

        const QString currency = bill.invoice.currency.trimmed();

        int index = -1;
        for (int i = 0; i < total.byCurrency.size(); ++i) {
            if (total.byCurrency.at(i).currency.compare(currency, Qt::CaseInsensitive) == 0) {
                index = i;
                break;
            }
        }

        if (index < 0) {
            CurrencyTotal entry;
            entry.currency = currency;
            total.byCurrency.append(entry);
            cents.append(0);
            index = total.byCurrency.size() - 1;
        }

        cents[index] += toCents(*bill.invoice.grossTotal);
        total.byCurrency[index].bills += 1;
        total.counted += 1;
    }

    for (int i = 0; i < total.byCurrency.size(); ++i)
        total.byCurrency[i].amount = fromCents(cents.at(i));

    return total;
}

QString formatTotal(const FileTotal &total, const QLocale &locale)
{
    QStringList parts;
    for (const CurrencyTotal &entry : total.byCurrency) {
        const QString amount = locale.toString(entry.amount, 'f', 2);
        parts.append(entry.currency.isEmpty()
                         ? amount
                         : QStringLiteral("%1 %2").arg(amount, entry.currency));
    }

    return parts.join(QStringLiteral(" + "));
}

QString formatCoverage(const FileTotal &total)
{
    if (total.complete())
        return billCount(total.counted);

    QString text = QStringLiteral("%1 of %2").arg(total.counted).arg(billCount(total.expected));

    // Two different reasons for a gap, and they are worth telling apart: a bill
    // that could not be read is a failure to retry, a bill without a total is
    // usually a document the model could make nothing of.
    const int unread = total.failed;
    const int unamounted = qMax(0, total.present - total.failed - total.counted);
    if (unread > 0)
        text += QStringLiteral(", %1 unread").arg(unread);
    if (unamounted > 0)
        text += QStringLiteral(", %1 without an amount").arg(unamounted);

    return text;
}

} // namespace InvoiceDrop
