#pragma once

#include "analysis.h"

#include <QLocale>
#include <QString>
#include <QVector>

namespace InvoiceDrop {

/// The sum of one currency inside one file.
///
/// A file that mixes currencies gets one of these per currency, because adding
/// euros to dollars produces a number that is not money.
struct CurrencyTotal {
    QString currency;
    double amount = 0.0;
    int bills = 0;
};

/// What one file adds up to.
///
/// The interesting part is not the sum, it is whether the sum is the whole
/// story. A file whose third page failed to read still adds up, and a total that
/// quietly covers two of three bills is exactly the number that ends up in an
/// expense report. `complete()` is what keeps those two apart.
struct FileTotal {
    int expected = 0; ///< bills the file holds, taken from the page count
    int present = 0;  ///< bills that were handed over
    int counted = 0;  ///< bills that carried a usable amount
    int failed = 0;   ///< bills that could not be read at all
    QVector<CurrencyTotal> byCurrency;

    bool usable() const { return counted > 0; }

    bool complete() const { return usable() && failed == 0 && counted >= expected; }
};

/// Adds up one file's bills. Bills that failed and bills without a total are
/// both left out of the sum and both counted, so the caller can say so.
FileTotal totalFor(const QVector<BillResult> &bills);

/// `177,76 EUR`, or `177,76 EUR + 12,00 USD` for a file that mixes currencies.
/// Empty when there is nothing to add up.
///
/// The locale can be given so the tests do not depend on the machine's, which is
/// the only reason the parameter exists.
QString formatTotal(const FileTotal &total, const QLocale &locale = QLocale::system());

/// `3 bills`, `2 of 3 bills`, `2 of 3 bills, 1 unread`, or
/// `2 of 3 bills, 1 without an amount`.
QString formatCoverage(const FileTotal &total);

} // namespace InvoiceDrop
