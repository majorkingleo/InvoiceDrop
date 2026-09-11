#include <QtTest>

#include "paths.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace InvoiceDrop;

namespace {

/// The canonical form of a path that exists, which is what `resolvePath` returns
/// for one. Comparing against the raw string would fail whenever the temporary
/// directory, the checkout or `/tmp` itself is reached through a symlink.
QString canonical(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

} // namespace

class TestPaths : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void resolvesRelativePaths();
    void keepsAbsolutePaths();
    void foldsDotSegmentsInMissingPaths();
    void neverTouchesAnEmptyPath();

    void dataDirIsNotDoubled();
    void dataDirHasItsFiles();
};

void TestPaths::initTestCase()
{
    QVERIFY2(!QDir::currentPath().isEmpty(), "no working directory to test against");
}

void TestPaths::cleanupTestCase()
{
    // Nothing outside the test touches the working directory, but leaving it
    // somewhere random is the kind of thing that makes a later suite mystifying.
    QDir::setCurrent(QCoreApplication::applicationDirPath());
}

void TestPaths::resolvesRelativePaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QFile(QDir(dir.path()).filePath(QStringLiteral("beleg.pdf")))
                .open(QIODevice::WriteOnly));

    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(dir.path()));

    // The bug this exists for: the daemon is started by the session bus in
    // $HOME, so a relative path that reaches it over D-Bus is looked for in the
    // wrong directory and comes back as "file does not exist". The path has to
    // be absolute before it leaves the process.
    QCOMPARE(Paths::resolvePath(QStringLiteral("beleg.pdf")),
             canonical(QDir(dir.path()).filePath(QStringLiteral("beleg.pdf"))));

    QCOMPARE(Paths::resolvePath(QStringLiteral("./beleg.pdf")),
             canonical(QDir(dir.path()).filePath(QStringLiteral("beleg.pdf"))));

    // A symlink resolves to what it points at, which is also what the hash is
    // then taken of. Asserted rather than skipped when it cannot be created: a
    // branch that quietly does nothing is how a check stops checking.
    const QString target = QDir(dir.path()).filePath(QStringLiteral("beleg.pdf"));
    const QString link = QDir(dir.path()).filePath(QStringLiteral("link.pdf"));
    QVERIFY2(QFile::link(target, link), "cannot create a symlink in the temporary directory");
    QCOMPARE(Paths::resolvePath(QStringLiteral("link.pdf")), canonical(target));

    QDir::setCurrent(previous);
}

void TestPaths::keepsAbsolutePaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString file = QDir(dir.path()).filePath(QStringLiteral("beleg.pdf"));
    QVERIFY(QFile(file).open(QIODevice::WriteOnly));

    QCOMPARE(Paths::resolvePath(file), canonical(file));
}

void TestPaths::foldsDotSegmentsInMissingPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(dir.path()));

    // A missing file cannot be canonicalised, so this is the branch that keeps
    // the error message readable: absolute, and with the `..` folded away rather
    // than passed on to the reader as typed.
    const QString resolved = Paths::resolvePath(QStringLiteral("unter/../fehlt.pdf"));

    QVERIFY(QDir::isAbsolutePath(resolved));
    QCOMPARE(resolved, QDir::cleanPath(QDir(dir.path()).filePath(QStringLiteral("fehlt.pdf"))));
    QVERIFY(!resolved.contains(QStringLiteral("..")));

    // The file still does not exist, and that is the point of returning a usable
    // path instead of an empty string.
    QVERIFY(!QFileInfo::exists(resolved));

    QDir::setCurrent(previous);
}

void TestPaths::neverTouchesAnEmptyPath()
{
    // Called with nothing, so that a caller which lost its argument gets an
    // empty string back rather than the current directory dressed up as a file.
    QCOMPARE(Paths::resolvePath(QString()), QString());
}

void TestPaths::dataDirIsNotDoubled()
{
    const QString dir = Paths::dataDir();

    QVERIFY(QDir::isAbsolutePath(dir));
    QVERIFY(dir.endsWith(QStringLiteral("/invoicedrop")));

    // Built from GenericDataLocation plus a fixed name. The first version used
    // QStandardPaths::AppLocalDataLocation, which appends the organisation and
    // the application name and produced `…/share/InvoiceDrop/InvoiceDrop`.
    QVERIFY2(!dir.contains(QStringLiteral("InvoiceDrop")),
             qPrintable(QStringLiteral("data directory is doubled: %1").arg(dir)));

    QCOMPARE(Paths::dataDir(), dir);
}

void TestPaths::dataDirHasItsFiles()
{
    const QString dir = Paths::dataDir();

    QCOMPARE(Paths::databaseFile(), dir + QStringLiteral("/invoicedrop.db"));
    QCOMPARE(Paths::archiveDir(), dir + QStringLiteral("/archive"));
}

QTEST_MAIN(TestPaths)

#include "tst_paths.moc"
