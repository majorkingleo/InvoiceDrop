#include "extract/documentreader.h"

#include "extract/imagereader.h"
#include "extract/imageops.h"
#include "extract/ocr.h"
#include "extract/pdfreader.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>

namespace InvoiceDrop::Extract {
namespace {

const QSet<QString> &imageSuffixes()
{
    static const QSet<QString> suffixes = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("webp"), QStringLiteral("tif"),  QStringLiteral("tiff"),
        QStringLiteral("bmp"),
    };
    return suffixes;
}

void readImage(const QString &path, const ReadOptions &options, Document *document)
{
    QString error;
    PIX *pix = ImageFile::load(path, options, &document->notes, &error);
    if (!pix) {
        document->error = error;
        return;
    }

    DocumentPage page;
    page.index = 0;

    if (options.want != Want::Text) {
        QString encodeError;
        const QByteArray jpeg = ImageOps::toJpeg(pix, options.jpegQuality, &encodeError);
        if (jpeg.isEmpty()) {
            document->notes.append(encodeError);
        } else {
            page.jpeg = jpeg;
            page.width = pixGetWidth(pix);
            page.height = pixGetHeight(pix);
        }
    }

    if (options.want != Want::Images && Ocr::available(options.ocr)) {
        QString ocrError;
        const QString text = Ocr::imageToText(pix, options.ocr, &document->notes, &ocrError);
        if (!text.isEmpty()) {
            page.text = text;
            document->notes.append(
                QStringLiteral("text recovered by OCR (%1)").arg(options.ocr.languages));
        } else if (!ocrError.isEmpty()) {
            document->notes.append(ocrError);
        } else {
            document->notes.append(QStringLiteral("OCR found no text"));
        }
    }

    pixDestroy(&pix);

    if (page.jpeg.isEmpty() && page.text.isEmpty())
        document->error = QStringLiteral("nothing usable could be read from the image");

    document->pages.append(page);
    document->text = page.text;
}

} // namespace

Document readDocument(const QString &path, const ReadOptions &options)
{
    QElapsedTimer timer;
    timer.start();

    Document document;
    document.path = path;

    const QFileInfo info(path);
    if (!info.exists()) {
        document.error = QStringLiteral("file does not exist");
    } else if (!info.isFile()) {
        document.error = QStringLiteral("not a regular file");
    } else if (!info.isReadable()) {
        document.error = QStringLiteral("file is not readable");
    } else {
        const QString suffix = info.suffix().toLower();
        if (suffix == QLatin1String("pdf")) {
            document.kind = DocumentKind::Pdf;
            Pdf::read(path, options, &document);
        } else if (imageSuffixes().contains(suffix)) {
            document.kind = DocumentKind::Image;
            readImage(path, options, &document);
        } else {
            document.error = QStringLiteral("unsupported file type '%1'").arg(suffix.isEmpty()
                                                                                  ? QStringLiteral("(none)")
                                                                                  : suffix);
        }
    }

    document.elapsedMs = timer.elapsed();
    return document;
}

QString kindName(DocumentKind kind)
{
    switch (kind) {
    case DocumentKind::Pdf:
        return QStringLiteral("pdf");
    case DocumentKind::Image:
        return QStringLiteral("image");
    case DocumentKind::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

bool isSupportedSuffix(const QString &suffix)
{
    const QString lowered = suffix.toLower();
    return lowered == QLatin1String("pdf") || imageSuffixes().contains(lowered);
}

} // namespace InvoiceDrop::Extract
