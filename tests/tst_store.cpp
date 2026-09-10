#include <QtTest>

#include "analysis.h"
#include "extract/documentreader.h"
#include "store.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace InvoiceDrop;

namespace {

Extract::ReadOptions baseOptions()
{
    Extract::ReadOptions options;
    options.maxPages = 2;
    options.dpi = 150;
    return options;
}

BillResult makeBill(int page, const QString &vendor, double total, const QString &date)
{
    BillResult bill;
    bill.page = page;
    bill.pageCount = 2;
    bill.ok = true;
    bill.invoice.vendor = vendor;
    bill.invoice.date = date;
    bill.invoice.currency = QStringLiteral("EUR");
    bill.invoice.grossTotal = total;
    return bill;
}

Extract::Document makeDocument(const QString &path)
{
    Extract::Document document;
    document.path = path;
    document.kind = Extract::DocumentKind::Pdf;
    document.hasTextLayer = false;
    return document;
}

} // namespace

class TestStore : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void fingerprintIgnoresIrrelevantChanges();
    void fingerprintChangesWithModel();
    void fingerprintChangesWithOptions();

    void storesAndFindsBills();
    void rejectsAStaleFingerprint();
    void replacesBillsOnASecondWrite();
    void forgetsADocument();
    void survivesReopening();

private:
    QTemporaryDir m_directory;
    QString m_path;
};

void TestStore::init()
{
    QVERIFY(m_directory.isValid());
    m_path = m_directory.filePath(QStringLiteral("store.db"));
    QFile::remove(m_path);
}

void TestStore::fingerprintIgnoresIrrelevantChanges()
{
    const QString a = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));
    const QString b = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));

    QVERIFY(!a.isEmpty());
    QCOMPARE(a, b);
}

void TestStore::fingerprintChangesWithModel()
{
    const QString a = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));
    const QString b = Store::fingerprint(baseOptions(), QStringLiteral("minicpm-v:8b"));

    QVERIFY(a != b);
}

void TestStore::fingerprintChangesWithOptions()
{
    const QString base = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));

    Extract::ReadOptions morePages = baseOptions();
    morePages.maxPages = 20;
    QVERIFY(base != Store::fingerprint(morePages, QStringLiteral("gemma4:latest")));

    Extract::ReadOptions withOcr = baseOptions();
    withOcr.ocr.enabled = true;
    QVERIFY(base != Store::fingerprint(withOcr, QStringLiteral("gemma4:latest")));

    Extract::ReadOptions sharper = baseOptions();
    sharper.dpi = 300;
    QVERIFY(base != Store::fingerprint(sharper, QStringLiteral("gemma4:latest")));
}

void TestStore::storesAndFindsBills()
{
    Store store(m_path);
    QVERIFY2(store.isOpen(), qPrintable(store.error()));

    const QString fingerprint = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));

    QVector<BillResult> bills;
    bills.append(makeBill(1, QStringLiteral("BERTAHÜTTE"), 732.00, QStringLiteral("2025-07-22")));
    bills.append(makeBill(2, QStringLiteral("BERTAHÜTTE"), 24.20, QStringLiteral("2025-07-22")));

    QVERIFY2(store.save(QStringLiteral("hash-a"), fingerprint,
                        makeDocument(QStringLiteral("/tmp/a.pdf")), bills,
                        QStringLiteral("gemma4:latest")),
             qPrintable(store.error()));

    const std::optional<CachedDocument> found = store.find(QStringLiteral("hash-a"), fingerprint);
    QVERIFY(found.has_value());
    QCOMPARE(found->pageCount, 2);

    const QVector<StoredBill> stored = store.bills(QStringLiteral("hash-a"));
    QCOMPARE(stored.size(), 2);
    QCOMPARE(stored.at(0).page, 1);
    QCOMPARE(stored.at(0).vendor, QStringLiteral("BERTAHÜTTE"));
    QVERIFY(qAbs(*stored.at(0).grossTotal - 732.00) < 0.005);
    QVERIFY(qAbs(*stored.at(1).grossTotal - 24.20) < 0.005);
    QCOMPARE(stored.at(1).date, QStringLiteral("2025-07-22"));
    QVERIFY(!stored.at(0).payload.isEmpty());
}

void TestStore::rejectsAStaleFingerprint()
{
    Store store(m_path);
    QVERIFY(store.isOpen());

    const QString a = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));
    QVector<BillResult> bills;
    bills.append(makeBill(1, QStringLiteral("HOFER"), 11.91, QStringLiteral("2025-07-09")));

    QVERIFY(store.save(QStringLiteral("hash-b"), a,
                       makeDocument(QStringLiteral("/tmp/b.pdf")), bills,
                       QStringLiteral("gemma4:latest")));

    // Same document, different settings: the stored answer was produced under
    // other conditions, so it must not be reused.
    const QString b = Store::fingerprint(baseOptions(), QStringLiteral("minicpm-v:8b"));
    QVERIFY(!store.find(QStringLiteral("hash-b"), b).has_value());

    QVERIFY(store.find(QStringLiteral("hash-b"), a).has_value());
}

void TestStore::replacesBillsOnASecondWrite()
{
    Store store(m_path);
    QVERIFY(store.isOpen());

    const QString fingerprint = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));
    const Extract::Document document = makeDocument(QStringLiteral("/tmp/c.pdf"));

    QVector<BillResult> first;
    first.append(makeBill(1, QStringLiteral("erste Lesung"), 1.00, QStringLiteral("2025-01-01")));
    QVERIFY(store.save(QStringLiteral("hash-c"), fingerprint, document, first,
                       QStringLiteral("gemma4:latest")));

    QVector<BillResult> second;
    second.append(makeBill(1, QStringLiteral("zweite Lesung"), 2.00, QStringLiteral("2025-02-02")));
    second.append(makeBill(2, QStringLiteral("zweite Lesung"), 3.00, QStringLiteral("2025-02-02")));
    QVERIFY(store.save(QStringLiteral("hash-c"), fingerprint, document, second,
                       QStringLiteral("gemma4:latest")));

    const QVector<StoredBill> stored = store.bills(QStringLiteral("hash-c"));
    QCOMPARE(stored.size(), 2);
    QCOMPARE(stored.at(0).vendor, QStringLiteral("zweite Lesung"));
    QCOMPARE(store.billCount(), 2);
}

void TestStore::forgetsADocument()
{
    Store store(m_path);
    QVERIFY(store.isOpen());

    const QString fingerprint = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));
    QVector<BillResult> bills;
    bills.append(makeBill(1, QStringLiteral("HOFER"), 11.91, QStringLiteral("2025-07-09")));

    QVERIFY(store.save(QStringLiteral("hash-d"), fingerprint,
                       makeDocument(QStringLiteral("/tmp/d.pdf")), bills,
                       QStringLiteral("gemma4:latest")));
    QCOMPARE(store.billCount(), 1);

    QVERIFY(store.forget(QStringLiteral("hash-d")));
    QVERIFY(!store.find(QStringLiteral("hash-d"), fingerprint).has_value());
    QCOMPARE(store.billCount(), 0);
}

void TestStore::survivesReopening()
{
    const QString fingerprint = Store::fingerprint(baseOptions(), QStringLiteral("gemma4:latest"));

    {
        Store store(m_path);
        QVERIFY(store.isOpen());
        QVector<BillResult> bills;
        bills.append(makeBill(1, QStringLiteral("HOFER"), 11.91, QStringLiteral("2025-07-09")));
        QVERIFY(store.save(QStringLiteral("hash-e"), fingerprint,
                           makeDocument(QStringLiteral("/tmp/e.pdf")), bills,
                           QStringLiteral("gemma4:latest")));
    }

    Store reopened(m_path);
    QVERIFY2(reopened.isOpen(), qPrintable(reopened.error()));
    QVERIFY(reopened.find(QStringLiteral("hash-e"), fingerprint).has_value());
    QCOMPARE(reopened.billCount(), 1);
    QCOMPARE(reopened.recent(5).size(), 1);
}

QTEST_GUILESS_MAIN(TestStore)

#include "tst_store.moc"
