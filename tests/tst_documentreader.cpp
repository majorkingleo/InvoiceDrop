#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include "extract/documentreader.h"
#include "extract/imagereader.h"

using namespace InvoiceDrop::Extract;

class TestDocumentReader : public QObject
{
    Q_OBJECT

private slots:
    void rejectsMissingFile();
    void rejectsUnsupportedSuffix();
    void acceptsUppercaseSuffix();

    void testDataIsRead_data();
    void testDataIsRead();

    void detectsExifOrientation_data();
    void detectsExifOrientation();

    void ignoresNonJpegForExif();

private:
    /// The tests/testdata directory, located relative to this source file and,
    /// failing that, by walking up from the working directory.
    static QString testDataDir();

    /// Builds the smallest possible JPEG whose APP1 block carries an EXIF
    /// orientation tag, so the parser can be tested without a real photo.
    static QByteArray jpegWithOrientation(int orientation);
};

void TestDocumentReader::rejectsMissingFile()
{
    const Document document = readDocument(QStringLiteral("/nonexistent/invoice.pdf"));
    QVERIFY(!document.ok());
    QVERIFY(document.error.contains(QStringLiteral("does not exist")));
    QVERIFY(document.kind == DocumentKind::Unknown);
}

void TestDocumentReader::rejectsUnsupportedSuffix()
{
    const QString path = QDir::temp().filePath(QStringLiteral("invoicedrop-test.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not an invoice");
    file.close();

    const Document document = readDocument(path);
    QFile::remove(path);

    QVERIFY(!document.ok());
    QVERIFY(document.error.contains(QStringLiteral("unsupported file type")));
}

void TestDocumentReader::acceptsUppercaseSuffix()
{
    QVERIFY(isSupportedSuffix(QStringLiteral("PDF")));
    QVERIFY(isSupportedSuffix(QStringLiteral("JPG")));
    QVERIFY(!isSupportedSuffix(QStringLiteral("txt")));
}

QString TestDocumentReader::testDataDir()
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

void TestDocumentReader::testDataIsRead_data()
{
    QTest::addColumn<QString>("path");

    const QString directory = testDataDir();
    if (directory.isEmpty())
        return;

    QDirIterator iterator(directory, QDir::Files);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!isSupportedSuffix(QFileInfo(path).suffix()))
            continue;
        QTest::newRow(qPrintable(QFileInfo(path).fileName())) << path;
    }
}

void TestDocumentReader::testDataIsRead()
{
    QFETCH(QString, path);

    if (path.isEmpty())
        QSKIP("no test data found in tests/testdata");

    // One page keeps the suite quick. The page limit itself is exercised
    // separately by passing a larger --pages value on the command line.
    ReadOptions options;
    options.maxPages = 1;

    QElapsedTimer timer;
    timer.start();
    const Document document = readDocument(path, options);
    const qint64 elapsed = timer.elapsed();

    qInfo("%s: %s, %lld ms, %lld characters, %lld page image(s)",
          qPrintable(QFileInfo(path).fileName()),
          document.ok() ? "ok" : "failed",
          static_cast<long long>(elapsed),
          static_cast<long long>(document.text.size()),
          static_cast<long long>(document.pages.size()));

    QVERIFY2(document.ok(), qPrintable(document.error));
    QVERIFY(document.kind != DocumentKind::Unknown);
    QVERIFY(!document.notes.isEmpty());
    QVERIFY2(document.hasText() || !document.pages.isEmpty(),
             qPrintable(QStringLiteral("nothing usable extracted, notes: %1")
                            .arg(document.notes.join(QStringLiteral(" | ")))));
}

void TestDocumentReader::detectsExifOrientation_data()
{
    QTest::addColumn<int>("orientation");

    for (int orientation = 1; orientation <= 8; ++orientation)
        QTest::newRow(qPrintable(QString::number(orientation))) << orientation;
}

void TestDocumentReader::detectsExifOrientation()
{
    QFETCH(int, orientation);
    QCOMPARE(ImageFile::exifOrientation(jpegWithOrientation(orientation)), orientation);
}

void TestDocumentReader::ignoresNonJpegForExif()
{
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    png.append(QByteArray(64, '\0'));
    QCOMPARE(ImageFile::exifOrientation(png), 0);
    QCOMPARE(ImageFile::exifOrientation(QByteArray()), 0);
}

QByteArray TestDocumentReader::jpegWithOrientation(int orientation)
{
    QByteArray tiff;
    tiff.append("II", 2);                        // little endian
    tiff.append(char(0x2a));                     // magic 42
    tiff.append(char(0x00));
    tiff.append(char(0x08));                     // IFD starts right after the header
    tiff.append(QByteArray(3, '\0'));

    tiff.append(char(0x01));                     // one entry
    tiff.append(char(0x00));

    tiff.append(char(0x12));                     // tag 0x0112, orientation
    tiff.append(char(0x01));
    tiff.append(char(0x03));                     // type SHORT
    tiff.append(char(0x00));
    tiff.append(char(0x01));                     // count 1
    tiff.append(QByteArray(3, '\0'));
    tiff.append(char(orientation));              // the value
    tiff.append(char(0x00));
    tiff.append(QByteArray(2, '\0'));

    tiff.append(QByteArray(4, '\0'));            // no next IFD

    QByteArray payload("Exif", 4);
    payload.append(char(0x00));
    payload.append(char(0x00));
    payload.append(tiff);

    const int segmentLength = payload.size() + 2;

    QByteArray jpeg;
    jpeg.append(char(0xff));
    jpeg.append(char(0xd8));                     // SOI
    jpeg.append(char(0xff));
    jpeg.append(char(0xe1));                     // APP1
    jpeg.append(char((segmentLength >> 8) & 0xff));
    jpeg.append(char(segmentLength & 0xff));
    jpeg.append(payload);
    return jpeg;
}

QTEST_GUILESS_MAIN(TestDocumentReader)

#include "tst_documentreader.moc"
