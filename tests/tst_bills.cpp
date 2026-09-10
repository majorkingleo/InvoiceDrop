#include <QtTest>

#include "analysis.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

using namespace InvoiceDrop;

namespace {

constexpr auto kSuffix = ".expected-result.json";

/// One expected bill. A field left out, or set to "*", is not compared.
struct ExpectedBill {
    int page = 0;
    QString vendor;
    QString date;
    std::optional<double> grossTotal;
    QString currency;
    bool checkVendor = false;
    bool checkDate = false;
    bool checkTotal = false;
    bool checkCurrency = false;

    /// `null` in the file means "must be empty"; absent means "do not check".
    bool vendorMustBeEmpty = false;
    bool dateMustBeEmpty = false;
    bool totalMustBeEmpty = false;
    bool currencyMustBeEmpty = false;
};

struct ExpectedDocument {
    QString document;
    QString expectation;
    QVector<ExpectedBill> bills;
    QString error;
};

QString testDataDir()
{
    static const QString located = QFINDTESTDATA("testdata");
    if (!located.isEmpty() && QFileInfo(located).isDir())
        return located;

    QDir probe(QDir::currentPath());
    for (int depth = 0; depth < 4; ++depth) {
        const QString candidate = probe.filePath(QStringLiteral("tests/testdata"));
        if (QFileInfo(candidate).isDir())
            return candidate;
        if (!probe.cdUp())
            break;
    }
    return {};
}

/// Reads the field of a bill and records what has to be checked about it.
void readField(const QJsonObject &object,
               const char *key,
               bool *check,
               bool *mustBeEmpty,
               QString *text,
               std::optional<double> *number = nullptr)
{
    const QJsonValue value = object.value(QString::fromLatin1(key));
    if (value.isUndefined() || value.toString() == QStringLiteral("*"))
        return;

    *check = true;

    if (value.isNull()) {
        *mustBeEmpty = true;
        return;
    }

    if (number) {
        if (value.isDouble())
            *number = value.toDouble();
        else
            *number = value.toString().toDouble();
        return;
    }

    *text = value.toString();
}

ExpectedDocument parseExpectation(const QString &expectationPath)
{
    ExpectedDocument expected;
    expected.expectation = expectationPath;
    expected.document =
        QString(expectationPath).remove(QString::fromLatin1(kSuffix));

    QFile file(expectationPath);
    if (!file.open(QIODevice::ReadOnly)) {
        expected.error = QStringLiteral("cannot open the expectation file");
        return expected;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        expected.error = QStringLiteral("not valid JSON: %1").arg(parseError.errorString());
        return expected;
    }

    const QJsonArray bills = document.object().value(QStringLiteral("bills")).toArray();
    if (bills.isEmpty()) {
        expected.error = QStringLiteral("no bills listed");
        return expected;
    }

    for (int index = 0; index < bills.size(); ++index) {
        const QJsonObject object = bills.at(index).toObject();

        ExpectedBill bill;
        bill.page = object.contains(QStringLiteral("bill"))
            ? object.value(QStringLiteral("bill")).toInt(index + 1)
            : index + 1;

        readField(object, "vendor", &bill.checkVendor, &bill.vendorMustBeEmpty, &bill.vendor);
        readField(object, "date", &bill.checkDate, &bill.dateMustBeEmpty, &bill.date);
        readField(object, "currency", &bill.checkCurrency, &bill.currencyMustBeEmpty,
                  &bill.currency);
        readField(object, "gross_total", &bill.checkTotal, &bill.totalMustBeEmpty, nullptr,
                  &bill.grossTotal);

        expected.bills.append(bill);
    }

    return expected;
}

/// Case and spacing are not correctness signals for a company name, so the
/// comparison folds them away. Everything else is compared exactly.
QString foldVendor(const QString &value)
{
    return value.simplified().toCaseFolded();
}

} // namespace

class TestBills : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void bills_data();
    void bills();

private:
    /// Analysing a document costs seconds, and every data row of the same
    /// document would pay it again, so results are kept for the whole run.
    static QVector<BillResult> analyse(const QString &document);

    OllamaOptions m_ollama;
    bool m_available = false;
};

void TestBills::initTestCase()
{
    QString error;
    OllamaClient client(m_ollama);
    m_available = client.probe(&error);

    if (!m_available) {
        QSKIP(qPrintable(QStringLiteral("Ollama is not reachable (%1). This suite checks what a "
                                        "model read and is skipped without one.")
                             .arg(error)));
    }
}

QVector<BillResult> TestBills::analyse(const QString &document)
{
    static QHash<QString, QVector<BillResult>> cache;

    const auto cached = cache.constFind(document);
    if (cached != cache.constEnd())
        return *cached;

    Extract::ReadOptions options;
    // Lift the page limit: the expectation lists one bill per page, and a
    // collection PDF has twenty one of them.
    options.maxPages = 0;

    OllamaOptions ollama;
    OllamaClient client(ollama);

    const QVector<BillResult> results = analyseFile(document, options, client);
    cache.insert(document, results);
    return results;
}

void TestBills::bills_data()
{
    QTest::addColumn<QString>("document");
    QTest::addColumn<int>("page");
    QTest::addColumn<QString>("vendor");
    QTest::addColumn<QString>("date");
    QTest::addColumn<double>("total");
    QTest::addColumn<QString>("currency");
    QTest::addColumn<bool>("checkVendor");
    QTest::addColumn<bool>("checkDate");
    QTest::addColumn<bool>("checkTotal");
    QTest::addColumn<bool>("checkCurrency");
    QTest::addColumn<bool>("totalMustBeEmpty");

    const QString directory = testDataDir();
    if (directory.isEmpty())
        return;

    const QStringList expectations =
        QDir(directory).entryList({QStringLiteral("*") + QString::fromLatin1(kSuffix)}, QDir::Files,
                                  QDir::Name);

    for (const QString &name : expectations) {
        const ExpectedDocument expected = parseExpectation(QDir(directory).filePath(name));

        if (!expected.error.isEmpty()) {
            QTest::newRow(qPrintable(name)) << expected.document << 0 << expected.error
                                            << QString() << 0.0 << QString() << false << false
                                            << false << false << false;
            continue;
        }

        for (const ExpectedBill &bill : expected.bills) {
            const QString row = QStringLiteral("%1 bill %2").arg(name).arg(bill.page);
            QTest::newRow(qPrintable(row))
                << expected.document << bill.page << bill.vendor << bill.date
                << (bill.grossTotal.has_value() ? *bill.grossTotal : 0.0) << bill.currency
                << bill.checkVendor << bill.checkDate << bill.checkTotal << bill.checkCurrency
                << bill.totalMustBeEmpty;
        }
    }
}

void TestBills::bills()
{
    QFETCH(QString, document);
    QFETCH(int, page);
    QFETCH(QString, vendor);
    QFETCH(QString, date);
    QFETCH(double, total);
    QFETCH(QString, currency);
    QFETCH(bool, checkVendor);
    QFETCH(bool, checkDate);
    QFETCH(bool, checkTotal);
    QFETCH(bool, checkCurrency);
    QFETCH(bool, totalMustBeEmpty);

    if (!m_available)
        QSKIP("no Ollama");

    const QString directory = testDataDir();
    if (directory.isEmpty())
        QSKIP("tests/testdata not found");

    // A row with page 0 carries a parse error from the expectation file.
    if (page == 0) {
        QFAIL(qPrintable(QStringLiteral("expectation file cannot be read: %1").arg(vendor)));
    }

    const QVector<BillResult> results = analyse(QDir(directory).filePath(document));

    QVERIFY2(page <= results.size(),
             qPrintable(QStringLiteral("%1 has only %2 page(s), but the expectation asks for "
                                       "bill %3")
                            .arg(document)
                            .arg(results.size())
                            .arg(page)));

    const BillResult &bill = results.at(page - 1);

    QVERIFY2(bill.ok, qPrintable(QStringLiteral("%1 bill %2: %3")
                                     .arg(document)
                                     .arg(page)
                                     .arg(bill.error)));

    if (checkVendor) {
        if (vendor.isEmpty()) {
            QVERIFY2(bill.invoice.vendor.isEmpty(),
                     qPrintable(QStringLiteral("vendor should be empty, got '%1'")
                                    .arg(bill.invoice.vendor)));
        } else {
            QCOMPARE(foldVendor(bill.invoice.vendor), foldVendor(vendor));
        }
    }

    if (checkDate)
        QCOMPARE(bill.invoice.date, date);

    if (checkCurrency)
        QCOMPARE(bill.invoice.currency, currency);

    if (checkTotal) {
        if (totalMustBeEmpty) {
            QVERIFY2(!bill.invoice.grossTotal.has_value(),
                     "a total was reported, but the expectation says there is none");
        } else {
            QVERIFY2(bill.invoice.grossTotal.has_value(), "no total was reported");
            // Amounts are compared to the cent; a tolerance protects against a
            // double that travelled through a decimal string.
            QVERIFY2(qAbs(*bill.invoice.grossTotal - total) < 0.005,
                     qPrintable(QStringLiteral("total %1, expected %2")
                                    .arg(*bill.invoice.grossTotal, 0, 'f', 2)
                                    .arg(total, 0, 'f', 2)));
        }
    }
}

QTEST_GUILESS_MAIN(TestBills)

#include "tst_bills.moc"
