#include <QtTest>

#include "invoice.h"

using namespace InvoiceDrop;

class TestInvoice : public QObject
{
    Q_OBJECT

private slots:
    // --------------------------------------------------------- amounts
    void parsesAmount_data();
    void parsesAmount();
    void rejectsNonAmounts_data();
    void rejectsNonAmounts();

    // ----------------------------------------------------------- dates
    void normalisesDate_data();
    void normalisesDate();
    void rejectsNonDates_data();
    void rejectsNonDates();

    // -------------------------------------------------------- currency
    void normalisesCurrency_data();
    void normalisesCurrency();

    // ---------------------------------------------------------- replies
    void readsPlainJson();
    void readsFencedJson();
    void readsJsonSurroundedByProse();
    void rejectsNonJson();

    // ------------------------------------------------------ placeholders
    void treatsApologiesAsMissing_data();
    void treatsApologiesAsMissing();

    // --------------------------------------------------------- invoice
    void normalisesGermanInvoice();
    void leavesUnreadableFieldsEmpty();
    void isImplausibleWithoutTotal();
    void roundTripsThroughJson();
};

// ------------------------------------------------------------------ amounts

void TestInvoice::parsesAmount_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<double>("expected");

    QTest::newRow("dot decimal") << QStringLiteral("1190.00") << 1190.00;
    QTest::newRow("german decimal") << QStringLiteral("1190,00") << 1190.00;
    QTest::newRow("german thousands") << QStringLiteral("1.190,00") << 1190.00;
    QTest::newRow("swiss thousands") << QStringLiteral("1'190.00") << 1190.00;
    QTest::newRow("with euro sign") << QStringLiteral("€ 1.190,00") << 1190.00;
    QTest::newRow("with currency code") << QStringLiteral("EUR 18,48") << 18.48;
    QTest::newRow("trailing minus is not a sign") << QStringLiteral("18,48-") << 18.48;
    QTest::newRow("millions") << QStringLiteral("1.190.000,55") << 1190000.55;
    QTest::newRow("plain integer") << QStringLiteral("42") << 42.0;
    QTest::newRow("zero") << QStringLiteral("0,00") << 0.0;
    QTest::newRow("dot is not always thousands") << QStringLiteral("0.123") << 0.123;
    QTest::newRow("three decimals stay decimals") << QStringLiteral("1190.000") << 1190.0;
}

void TestInvoice::parsesAmount()
{
    QFETCH(QString, input);
    QFETCH(double, expected);

    const std::optional<double> value = parseAmount(input);
    QVERIFY2(value.has_value(), qPrintable(input));
    QVERIFY2(qAbs(*value - expected) < 0.0001,
             qPrintable(QStringLiteral("%1 parsed as %2, expected %3")
                            .arg(input)
                            .arg(*value)
                            .arg(expected)));
}

void TestInvoice::rejectsNonAmounts_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("empty") << QString();
    QTest::newRow("apology") << QStringLiteral("N/A");
    QTest::newRow("dash") << QStringLiteral("-");
    QTest::newRow("word") << QStringLiteral("unbekannt");
    QTest::newRow("no digits") << QStringLiteral("EUR");
}

void TestInvoice::rejectsNonAmounts()
{
    QFETCH(QString, input);
    QVERIFY2(!parseAmount(input).has_value(), qPrintable(input));
}

// -------------------------------------------------------------------- dates

void TestInvoice::normalisesDate_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("iso") << QStringLiteral("2025-05-07") << QStringLiteral("2025-05-07");
    QTest::newRow("iso with time") << QStringLiteral("2025-05-07T11:37:45")
                                   << QStringLiteral("2025-05-07");
    QTest::newRow("german") << QStringLiteral("01.05.2025") << QStringLiteral("2025-05-01");
    QTest::newRow("german with time") << QStringLiteral("17.07.2025 11:37:45")
                                      << QStringLiteral("2025-07-17");
    QTest::newRow("german slash") << QStringLiteral("07/05/2025") << QStringLiteral("2025-05-07");
    QTest::newRow("two digit year") << QStringLiteral("17.07.25") << QStringLiteral("2025-07-17");
    QTest::newRow("compact") << QStringLiteral("20250717") << QStringLiteral("2025-07-17");
    QTest::newRow("range takes the first") << QStringLiteral("01.05.2025 - 07.09.2025")
                                           << QStringLiteral("2025-05-01");
    QTest::newRow("padded") << QStringLiteral("  17.07.2025  ") << QStringLiteral("2025-07-17");
}

void TestInvoice::normalisesDate()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(normaliseDate(input), expected);
}

void TestInvoice::rejectsNonDates_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("empty") << QString();
    QTest::newRow("apology") << QStringLiteral("not visible");
    QTest::newRow("article number") << QStringLiteral("01341698");
    QTest::newRow("impossible month") << QStringLiteral("17.13.2025");
    QTest::newRow("word") << QStringLiteral("gestern");
}

void TestInvoice::rejectsNonDates()
{
    QFETCH(QString, input);
    QVERIFY2(normaliseDate(input).isEmpty(), qPrintable(input));
}

// ----------------------------------------------------------------- currency

void TestInvoice::normalisesCurrency_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("euro sign") << QStringLiteral("€") << QStringLiteral("EUR");
    QTest::newRow("lowercase code") << QStringLiteral("eur") << QStringLiteral("EUR");
    QTest::newRow("word") << QStringLiteral("Euro") << QStringLiteral("EUR");
    QTest::newRow("dollar") << QStringLiteral("$") << QStringLiteral("USD");
    QTest::newRow("pound") << QStringLiteral("£") << QStringLiteral("GBP");
    QTest::newRow("francs") << QStringLiteral("CHF") << QStringLiteral("CHF");
    QTest::newRow("other code") << QStringLiteral("PLN") << QStringLiteral("PLN");
}

void TestInvoice::normalisesCurrency()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(normaliseCurrency(input), expected);
}

// ------------------------------------------------------------------ replies

void TestInvoice::readsPlainJson()
{
    const Invoice invoice = Invoice::fromModelReply(
        QStringLiteral(R"({"vendor":"HOFER","gross_total":11.91,"currency":"EUR"})"));
    QCOMPARE(invoice.vendor, QStringLiteral("HOFER"));
    QCOMPARE(invoice.currency, QStringLiteral("EUR"));
    QVERIFY(invoice.plausible());
}

void TestInvoice::readsFencedJson()
{
    const Invoice invoice = Invoice::fromModelReply(QStringLiteral(
        "```json\n{\"vendor\":\"EDEKA\",\"gross_total\":71.52,\"currency\":\"EUR\"}\n```"));
    QCOMPARE(invoice.vendor, QStringLiteral("EDEKA"));
    QVERIFY(qAbs(*invoice.grossTotal - 71.52) < 0.0001);
}

void TestInvoice::readsJsonSurroundedByProse()
{
    const Invoice invoice = Invoice::fromModelReply(QStringLiteral(
        "Sure, here is the data:\n{\"vendor\":\"ÖBB\",\"gross_total\":\"18,48\","
        "\"currency\":\"EUR\"}\nLet me know if you need more."));
    QCOMPARE(invoice.vendor, QStringLiteral("ÖBB"));
    QVERIFY(qAbs(*invoice.grossTotal - 18.48) < 0.0001);
}

void TestInvoice::rejectsNonJson()
{
    QString error;
    const Invoice invoice = Invoice::fromModelReply(QStringLiteral("I cannot read this image."),
                                                   &error);
    QVERIFY(!invoice.plausible());
    QVERIFY(!error.isEmpty());
}

// ------------------------------------------------------------- placeholders

void TestInvoice::treatsApologiesAsMissing_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("empty") << QString();
    QTest::newRow("dash") << QStringLiteral("-");
    QTest::newRow("slashes") << QStringLiteral("--");
    QTest::newRow("question") << QStringLiteral("?");
    QTest::newRow("na") << QStringLiteral("N/A");
    QTest::newRow("na with excuse") << QStringLiteral("N/A (shop name illegible)");
    QTest::newRow("unknown") << QStringLiteral("unknown");
    QTest::newRow("german") << QStringLiteral("unbekannt");
    QTest::newRow("illegible") << QStringLiteral("illegible");
}

void TestInvoice::treatsApologiesAsMissing()
{
    QFETCH(QString, input);
    QVERIFY2(isPlaceholder(input), qPrintable(input));

    // And the same strings must not survive into an invoice.
    const Invoice invoice = Invoice::fromJson(
        QJsonObject{{QStringLiteral("vendor"), input},
                    {QStringLiteral("gross_total"), 4.5},
                    {QStringLiteral("currency"), input}});
    QVERIFY(invoice.vendor.isEmpty());
    QVERIFY(!invoice.plausible());
}

// ------------------------------------------------------------------ invoice

void TestInvoice::normalisesGermanInvoice()
{
    const Invoice invoice = Invoice::fromJson(QJsonObject{
        {QStringLiteral("vendor"), QStringLiteral("  Unser Lagerhaus Warenhandels ges.mb.H. ")},
        {QStringLiteral("invoice_number"), QStringLiteral("649445")},
        {QStringLiteral("date"), QStringLiteral("17.07.2025 11:37:45")},
        {QStringLiteral("currency"), QStringLiteral("€")},
        {QStringLiteral("gross_total"), QStringLiteral("18,48")},
        {QStringLiteral("iban"), QStringLiteral("AT61 1904 3002 3457 3201")},
    });

    QCOMPARE(invoice.vendor, QStringLiteral("Unser Lagerhaus Warenhandels ges.mb.H."));
    QCOMPARE(invoice.date, QStringLiteral("2025-07-17"));
    QCOMPARE(invoice.currency, QStringLiteral("EUR"));
    QVERIFY(qAbs(*invoice.grossTotal - 18.48) < 0.0001);
    QCOMPARE(invoice.iban, QStringLiteral("AT611904300234573201")); // spaces removed
    QVERIFY(invoice.plausible());
}

void TestInvoice::leavesUnreadableFieldsEmpty()
{
    const Invoice invoice = Invoice::fromJson(QJsonObject{
        {QStringLiteral("vendor"), QStringLiteral("HOFER KG")},
        {QStringLiteral("date"), QStringLiteral("n/a")},
        {QStringLiteral("currency"), QStringLiteral("EUR")},
        {QStringLiteral("gross_total"), 11.91},
    });

    QVERIFY(invoice.date.isEmpty());
    QVERIFY(!invoice.netTotal.has_value());
    QVERIFY(invoice.plausible());
}

void TestInvoice::isImplausibleWithoutTotal()
{
    const Invoice vendorOnly = Invoice::fromJson(
        QJsonObject{{QStringLiteral("vendor"), QStringLiteral("HOFER KG")}});
    QVERIFY(!vendorOnly.plausible());

    const Invoice totalOnly = Invoice::fromJson(
        QJsonObject{{QStringLiteral("gross_total"), 11.91}});
    QVERIFY(!totalOnly.plausible());
}

void TestInvoice::roundTripsThroughJson()
{
    const Invoice original = Invoice::fromJson(QJsonObject{
        {QStringLiteral("vendor"), QStringLiteral("HOFER KG")},
        {QStringLiteral("date"), QStringLiteral("2025-05-01")},
        {QStringLiteral("currency"), QStringLiteral("EUR")},
        {QStringLiteral("gross_total"), 11.91},
        {QStringLiteral("net_total"), 9.93},
    });

    const Invoice copy = Invoice::fromJson(original.toJson());
    QCOMPARE(copy.vendor, original.vendor);
    QCOMPARE(copy.date, original.date);
    QCOMPARE(copy.currency, original.currency);
    QVERIFY(qAbs(*copy.grossTotal - *original.grossTotal) < 0.0001);
    QVERIFY(qAbs(*copy.netTotal - *original.netTotal) < 0.0001);
}

QTEST_GUILESS_MAIN(TestInvoice)

#include "tst_invoice.moc"
