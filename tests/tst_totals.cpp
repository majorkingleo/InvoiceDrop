#include <QtTest>

#include "totals.h"

using namespace InvoiceDrop;

namespace {

BillResult makeBill(int page, int pageCount, const QString &currency, double total)
{
    BillResult bill;
    bill.path = QStringLiteral("/tmp/beleg.pdf");
    bill.page = page;
    bill.pageCount = pageCount;
    bill.ok = true;
    bill.invoice.currency = currency;
    bill.invoice.grossTotal = total;
    return bill;
}

/// A bill that was read but whose total the model could not produce.
BillResult amountlessBill(int page, int pageCount)
{
    BillResult bill;
    bill.path = QStringLiteral("/tmp/beleg.pdf");
    bill.page = page;
    bill.pageCount = pageCount;
    bill.ok = true;
    return bill;
}

BillResult failedBill(int page, int pageCount)
{
    BillResult bill;
    bill.path = QStringLiteral("/tmp/beleg.pdf");
    bill.page = page;
    bill.pageCount = pageCount;
    bill.ok = false;
    bill.error = QStringLiteral("nothing to send: no text and no page images");
    return bill;
}

/// German, pinned, so the separators in the assertions do not depend on the
/// machine the suite runs on.
QLocale german()
{
    return QLocale(QLocale::German, QLocale::Germany);
}

} // namespace

class TestTotals : public QObject
{
    Q_OBJECT

private slots:
    void addsUpOneFile();
    void countsInCentsNotInFloats();
    void neverAddsTwoCurrenciesTogether();
    void countsWhatsMissingFromTheSum();
    void knowsWhenAPageLimitCutTheFile();
    void aFailedBillStillGetsANumber();
    void anEmptyFileHasNothingToAddUp();
};

void TestTotals::addsUpOneFile()
{
    const QVector<BillResult> bills = {
        makeBill(1, 3, QStringLiteral("EUR"), 65.50),
        makeBill(2, 3, QStringLiteral("EUR"), 90.91),
        makeBill(3, 3, QStringLiteral("EUR"), 21.35),
    };

    const FileTotal total = totalFor(bills);

    QCOMPARE(total.present, 3);
    QCOMPARE(total.expected, 3);
    QCOMPARE(total.counted, 3);
    QCOMPARE(total.failed, 0);
    QVERIFY(total.complete());
    QCOMPARE(total.byCurrency.size(), 1);
    QCOMPARE(total.byCurrency.first().amount, 177.76);
    QCOMPARE(formatTotal(total, german()), QStringLiteral("177,76 EUR"));
    QCOMPARE(formatCoverage(total), QStringLiteral("3 bills"));
}

void TestTotals::countsInCentsNotInFloats()
{
    // 0.1 + 0.2 is 0.30000000000000004 in binary, and a total that differs in
    // the last digit is a total nobody checks twice.
    const QVector<BillResult> bills = {
        makeBill(1, 2, QStringLiteral("EUR"), 0.10),
        makeBill(2, 2, QStringLiteral("EUR"), 0.20),
    };

    const FileTotal total = totalFor(bills);

    QCOMPARE(total.byCurrency.first().amount, 0.30);
    QCOMPARE(formatTotal(total, german()), QStringLiteral("0,30 EUR"));

    // Three amounts that each end in a half cent when multiplied by a hundred.
    const QVector<BillResult> rounded = {
        makeBill(1, 3, QStringLiteral("EUR"), 21.35),
        makeBill(2, 3, QStringLiteral("EUR"), 0.05),
        makeBill(3, 3, QStringLiteral("EUR"), 1.10),
    };
    QCOMPARE(totalFor(rounded).byCurrency.first().amount, 22.50);
}

void TestTotals::neverAddsTwoCurrenciesTogether()
{
    const QVector<BillResult> bills = {
        makeBill(1, 2, QStringLiteral("EUR"), 10.00),
        makeBill(2, 2, QStringLiteral("USD"), 5.00),
    };

    const FileTotal total = totalFor(bills);

    QCOMPARE(total.byCurrency.size(), 2);
    QCOMPARE(total.byCurrency.at(0).amount, 10.00);
    QCOMPARE(total.byCurrency.at(1).amount, 5.00);
    QCOMPARE(formatTotal(total, german()), QStringLiteral("10,00 EUR + 5,00 USD"));

    // Casing is the model's business, not a second currency.
    const QVector<BillResult> shouted = {
        makeBill(1, 2, QStringLiteral("EUR"), 1.00),
        makeBill(2, 2, QStringLiteral("eur"), 2.00),
    };
    QCOMPARE(totalFor(shouted).byCurrency.size(), 1);
    QCOMPARE(totalFor(shouted).byCurrency.first().amount, 3.00);
}

void TestTotals::countsWhatsMissingFromTheSum()
{
    const QVector<BillResult> bills = {
        makeBill(1, 3, QStringLiteral("EUR"), 10.00),
        amountlessBill(2, 3),
        makeBill(3, 3, QStringLiteral("EUR"), 5.00),
    };

    const FileTotal total = totalFor(bills);

    QCOMPARE(total.counted, 2);
    QCOMPARE(total.failed, 0);
    QVERIFY(!total.complete());
    QCOMPARE(formatTotal(total, german()), QStringLiteral("15,00 EUR"));
    QCOMPARE(formatCoverage(total), QStringLiteral("2 of 3 bills, 1 without an amount"));

    const QVector<BillResult> withFailure = {
        makeBill(1, 3, QStringLiteral("EUR"), 10.00),
        failedBill(2, 3),
        makeBill(3, 3, QStringLiteral("EUR"), 5.00),
    };

    const FileTotal broken = totalFor(withFailure);

    QCOMPARE(broken.failed, 1);
    QVERIFY(!broken.complete());
    QCOMPARE(formatCoverage(broken), QStringLiteral("2 of 3 bills, 1 unread"));
}

void TestTotals::knowsWhenAPageLimitCutTheFile()
{
    // --pages 2 on a file of five: two bills, and the file's own count says so.
    const QVector<BillResult> bills = {
        makeBill(1, 5, QStringLiteral("EUR"), 65.50),
        makeBill(2, 5, QStringLiteral("EUR"), 90.91),
    };

    const FileTotal total = totalFor(bills);

    QCOMPARE(total.present, 2);
    QCOMPARE(total.expected, 5);
    QVERIFY(!total.complete());
    QCOMPARE(formatCoverage(total), QStringLiteral("2 of 5 bills"));
    QCOMPARE(formatTotal(total, german()), QStringLiteral("156,41 EUR"));
}

void TestTotals::aFailedBillStillGetsANumber()
{
    // One bill, unreadable. There is a file, there is no amount, and the caller
    // has to be able to tell that apart from a file that added up to nothing.
    const QVector<BillResult> bills = { failedBill(1, 1) };

    const FileTotal total = totalFor(bills);

    QVERIFY(!total.usable());
    QVERIFY(!total.complete());
    QVERIFY(formatTotal(total, german()).isEmpty());
    QCOMPARE(formatCoverage(total), QStringLiteral("0 of 1 bill, 1 unread"));
}

void TestTotals::anEmptyFileHasNothingToAddUp()
{
    const FileTotal total = totalFor({});

    QCOMPARE(total.present, 0);
    QCOMPARE(total.expected, 0);
    QVERIFY(!total.usable());
    QVERIFY(!total.complete());
    QVERIFY(formatTotal(total, german()).isEmpty());
    QCOMPARE(formatCoverage(total), QStringLiteral("0 of 0 bills"));
}

QTEST_APPLESS_MAIN(TestTotals)

#include "tst_totals.moc"
