#include <QtTest>

#include "analysis.h"
#include "inboxwatcher.h"
#include "notifier.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace InvoiceDrop;

namespace {

/// Waits for the watcher's own settle timer instead of asserting on a sleep.
bool waitForSignal(QSignalSpy &spy, int timeoutMs = 5000)
{
    return spy.wait(timeoutMs);
}

BillResult makeBill(int page, const QString &vendor, double total)
{
    BillResult bill;
    bill.page = page;
    bill.pageCount = 1;
    bill.ok = true;
    bill.invoice.vendor = vendor;
    bill.invoice.date = QStringLiteral("2025-07-22");
    bill.invoice.currency = QStringLiteral("EUR");
    bill.invoice.grossTotal = total;
    return bill;
}

} // namespace

class TestDaemon : public QObject
{
    Q_OBJECT

private slots:
    // ------------------------------------------------------- inbox watching
    void recognizesSupportedFiles_data();
    void recognizesSupportedFiles();

    void createsTheInbox();
    void reportsAWrittenFileOnce();
    void ignoresAnUnsupportedFile();
    void takePendingConsumesTheQueue();

    // -------------------------------------------------------- notifications
    void notificationListsOneLinePerBill();
    void notificationAddsUpTheFile();
    void notificationSaysWhenTheSumIsPartial();
    void notificationNamesTheFailedBill();
    void notificationIsSilentWhenDisabled();
};

void TestDaemon::recognizesSupportedFiles_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("supported");

    QTest::newRow("pdf") << QStringLiteral("a.pdf") << true;
    QTest::newRow("uppercase pdf") << QStringLiteral("a.PDF") << true;
    QTest::newRow("jpeg") << QStringLiteral("a.jpeg") << true;
    QTest::newRow("png") << QStringLiteral("a.png") << true;
    QTest::newRow("text") << QStringLiteral("a.txt") << false;
    QTest::newRow("no suffix") << QStringLiteral("a") << false;
    QTest::newRow("hidden") << QStringLiteral(".pdf") << false;
}

void TestDaemon::recognizesSupportedFiles()
{
    QFETCH(QString, path);
    QFETCH(bool, supported);
    QCOMPARE(InboxWatcher::isSupported(path), supported);
}

void TestDaemon::createsTheInbox()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString inbox = QDir(temporary.path()).filePath(QStringLiteral("inbox/deeper"));
    QVERIFY(!QDir(inbox).exists());

    InboxWatcher watcher(inbox);
    QString error;
    QVERIFY2(watcher.start(&error), qPrintable(error));
    QVERIFY(QDir(inbox).exists());
}

void TestDaemon::reportsAWrittenFileOnce()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    InboxWatcher watcher(temporary.path());
    QString error;
    QVERIFY2(watcher.start(&error), qPrintable(error));

    QSignalSpy spy(&watcher, &InboxWatcher::fileReady);
    QVERIFY(spy.isValid());

    const QString path = QDir(temporary.path()).filePath(QStringLiteral("rechnung.pdf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(2048, 'x'));
    file.close();

    QVERIFY(waitForSignal(spy));

    // The point of the settle delay and the handled list is that one file is
    // reported exactly once, however often the folder is scanned afterwards.
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), path);

    QTest::qWait(1200); // several more scans
    QCOMPARE(spy.count(), 1);
}

void TestDaemon::ignoresAnUnsupportedFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    InboxWatcher watcher(temporary.path());
    QString error;
    QVERIFY2(watcher.start(&error), qPrintable(error));

    QSignalSpy spy(&watcher, &InboxWatcher::fileReady);

    const QString path = QDir(temporary.path()).filePath(QStringLiteral("notes.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("kein Beleg");
    file.close();

    QTest::qWait(1500);
    QCOMPARE(spy.count(), 0);
}

void TestDaemon::takePendingConsumesTheQueue()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const QString path = QDir(temporary.path()).filePath(QStringLiteral("beleg.pdf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(1024, 'y'));
    file.close();

    InboxWatcher watcher(temporary.path());
    QString error;
    QVERIFY2(watcher.start(&error), qPrintable(error));

    const QStringList first = watcher.takePending();
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.at(0), path);

    // Taken means taken: a caller that read the file itself must not have it
    // delivered a second time by the timer.
    QCOMPARE(watcher.pending().size(), 0);

    QSignalSpy spy(&watcher, &InboxWatcher::fileReady);
    QTest::qWait(1200);
    QCOMPARE(spy.count(), 0);
}

void TestDaemon::notificationListsOneLinePerBill()
{
    QVector<BillResult> bills;
    bills.append(makeBill(1, QStringLiteral("BERTAHÜTTE"), 732.00));
    bills.append(makeBill(2, QStringLiteral("BERTAHÜTTE"), 24.20));

    const QString body = Notifier::bodyFor(QStringLiteral("beleg.pdf"), bills);
    const QStringList lines = body.split(QLatin1Char('\n'));

    // Two bills and the sum they add up to.
    QCOMPARE(lines.size(), 3);
    QVERIFY(lines.at(0).contains(QStringLiteral("beleg.pdf p1")));
    QVERIFY(lines.at(1).contains(QStringLiteral("beleg.pdf p2")));
    QVERIFY(lines.at(0).contains(QStringLiteral("BERTAHÜTTE")));
}

void TestDaemon::notificationAddsUpTheFile()
{
    QVector<BillResult> bills;
    bills.append(makeBill(1, QStringLiteral("Tank Roth GmbH"), 65.50));
    bills.append(makeBill(2, QStringLiteral("Eni Service-Station"), 90.91));
    bills.append(makeBill(3, QStringLiteral("Tank Roth GmbH"), 21.35));

    const QString body = Notifier::bodyFor(QStringLiteral("tanken.pdf"), bills);
    const QStringList lines = body.split(QLatin1Char('\n'));

    QCOMPARE(lines.size(), 4);
    QVERIFY(lines.last().contains(QStringLiteral("tanken.pdf: sum")));

    // Built with the same formatter the notification uses, so this asserts that
    // the three amounts were added up rather than asserting what a decimal
    // separator looks like on the machine running the suite.
    const QString sum = QLocale::system().toString(177.76, 'f', 2) + QStringLiteral(" EUR");
    QVERIFY(lines.last().contains(sum));
    QVERIFY(!lines.last().contains(QStringLiteral("65")));

    // Nothing is missing, so nothing is claimed to be missing. A toast has no
    // room for reassurance.
    QVERIFY(!lines.last().contains(QStringLiteral("bills")));
}

void TestDaemon::notificationSaysWhenTheSumIsPartial()
{
    // The page count says the file holds three bills and only one arrived, so
    // the total is not the file's total and the toast has to say so.
    QVector<BillResult> bills;
    bills.append(makeBill(1, QStringLiteral("Tank Roth GmbH"), 65.50));

    BillResult second;
    second.page = 2;
    second.pageCount = 3;
    second.ok = false;
    second.error = QStringLiteral("nothing to send: no text and no page images");
    bills.append(second);

    const QString body = Notifier::bodyFor(QStringLiteral("tanken.pdf"), bills);
    const QStringList lines = body.split(QLatin1Char('\n'));

    QCOMPARE(lines.size(), 3);
    QVERIFY(lines.last().contains(QStringLiteral("1 of 3 bill")));
    QVERIFY(lines.last().contains(QStringLiteral("1 unread")));
}

void TestDaemon::notificationNamesTheFailedBill()
{
    BillResult failed;
    failed.page = 1;
    failed.pageCount = 1;
    failed.ok = false;
    failed.error = QStringLiteral("the model returned no usable fields");

    QVector<BillResult> bills;
    bills.append(failed);

    const QString body = Notifier::bodyFor(QStringLiteral("kaputt.pdf"), bills);
    QVERIFY(body.contains(QStringLiteral("kaputt.pdf")));
    QVERIFY(body.contains(QStringLiteral("no usable fields")));
}

void TestDaemon::notificationIsSilentWhenDisabled()
{
    Notifier disabled(false);
    QVERIFY(!disabled.isAvailable());
    disabled.notify(QStringLiteral("nichts"), QStringLiteral("passiert"));
}

QTEST_GUILESS_MAIN(TestDaemon)

#include "tst_daemon.moc"
