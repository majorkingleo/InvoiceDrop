#include "cli.h"

#include "extract/documentreader.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace InvoiceDrop {
namespace {

namespace Extract = InvoiceDrop::Extract;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailure = 2;

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

QString indentNotes(const QStringList &notes)
{
    if (notes.isEmpty())
        return QStringLiteral("(none)");
    return notes.join(QStringLiteral("\n             "));
}

void printHuman(const Extract::Document &document, const Extract::ReadOptions &options, bool withText)
{
    QTextStream &stream = out();
    stream << QFileInfo(document.path).fileName() << Qt::endl;

    if (!document.ok()) {
        stream << "  error      " << document.error << Qt::endl;
    } else {
        stream << "  kind       " << Extract::kindName(document.kind) << Qt::endl;
        stream << "  text layer " << (document.hasTextLayer ? "yes" : "no") << Qt::endl;
        stream << "  text       " << document.text.size() << " characters" << Qt::endl;
        if (!document.pages.isEmpty()) {
            const Extract::PageImage &first = document.pages.first();
            stream << "  pages      " << document.pages.size() << " (" << first.width << "x"
                   << first.height << " -> " << options.longEdge << " px long edge)" << Qt::endl;
        }
    }

    stream << "  time       " << document.elapsedMs << " ms" << Qt::endl;
    stream << "  notes      " << indentNotes(document.notes) << Qt::endl;

    if (withText && !document.text.isEmpty()) {
        stream << "--- text ---" << Qt::endl;
        stream << document.text << Qt::endl;
        stream << "--- end ---" << Qt::endl;
    }

    stream.flush();
}

QJsonObject toJson(const Extract::Document &document, bool withText)
{
    QJsonObject object;
    object.insert(QStringLiteral("file"), QFileInfo(document.path).fileName());
    object.insert(QStringLiteral("path"), document.path);
    object.insert(QStringLiteral("kind"), Extract::kindName(document.kind));
    object.insert(QStringLiteral("status"), document.ok() ? QStringLiteral("ok")
                                                          : QStringLiteral("error"));
    if (!document.error.isEmpty())
        object.insert(QStringLiteral("error"), document.error);

    object.insert(QStringLiteral("has_text_layer"), document.hasTextLayer);
    object.insert(QStringLiteral("page_count"), document.pages.size());
    object.insert(QStringLiteral("text_chars"), static_cast<qint64>(document.text.size()));
    object.insert(QStringLiteral("elapsed_ms"), document.elapsedMs);

    QJsonArray notes;
    for (const QString &note : document.notes)
        notes.append(note);
    object.insert(QStringLiteral("notes"), notes);

    if (withText)
        object.insert(QStringLiteral("text"), document.text);

    return object;
}

bool dumpPages(const Extract::Document &document, const QString &directory, QStringList *problems)
{
    QDir target(directory);
    if (!target.exists() && !target.mkpath(QStringLiteral("."))) {
        problems->append(QStringLiteral("cannot create image directory %1").arg(directory));
        return false;
    }

    const QString base = QFileInfo(document.path).completeBaseName();
    for (const Extract::PageImage &page : document.pages) {
        const QString name = QStringLiteral("%1-p%2.jpg").arg(base).arg(page.index + 1);
        QFile file(target.filePath(name));
        if (!file.open(QIODevice::WriteOnly)) {
            problems->append(QStringLiteral("cannot write %1").arg(file.fileName()));
            continue;
        }
        file.write(page.jpeg);
        file.close();
        out() << "  wrote      " << file.fileName() << " (" << page.width << "x" << page.height
              << ", " << page.jpeg.size() << " bytes)" << Qt::endl;
    }
    out().flush();
    return true;
}

} // namespace

int runCli(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Read invoices from PDF, JPEG and PNG files using a local AI model."));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption extractOnly(
        {QStringLiteral("e"), QStringLiteral("extract-only")},
        QStringLiteral("Print what the extractor found and skip the model."));
    const QCommandLineOption textOnly(QStringLiteral("text-only"),
                                      QStringLiteral("Use the PDF text layer only, never rasterise."));
    const QCommandLineOption imagesOnly(QStringLiteral("images-only"),
                                        QStringLiteral("Always rasterise, never read the text layer."));
    const QCommandLineOption json(QStringLiteral("json"),
                                  QStringLiteral("Write one JSON object per file to stdout."));
    const QCommandLineOption dpi(QStringLiteral("dpi"),
                                 QStringLiteral("Raster resolution for PDF pages."),
                                 QStringLiteral("dpi"), QStringLiteral("200"));
    const QCommandLineOption pages(QStringLiteral("pages"),
                                   QStringLiteral("Maximum number of pages to read."),
                                   QStringLiteral("count"), QStringLiteral("4"));
    const QCommandLineOption longEdge(QStringLiteral("long-edge"),
                                      QStringLiteral("Downscale images so the long edge is at most this "
                                                     "many pixels."),
                                      QStringLiteral("px"), QStringLiteral("1600"));
    const QCommandLineOption languages(QStringLiteral("lang"),
                                       QStringLiteral("Tesseract language pack, for example deu+eng."),
                                       QStringLiteral("langs"), QStringLiteral("deu+eng"));
    const QCommandLineOption ocrShortEdge(
        QStringLiteral("ocr-short-edge"),
        QStringLiteral("Short edge OCR scales to before recognising. Tiny scans are magnified."),
        QStringLiteral("px"), QStringLiteral("1400"));
    const QCommandLineOption psm(QStringLiteral("psm"),
                                 QStringLiteral("Tesseract page segmentation mode, 0 to 13."),
                                 QStringLiteral("mode"), QStringLiteral("6"));
    const QCommandLineOption noNormalise(
        QStringLiteral("no-normalise"),
        QStringLiteral("Skip adaptive contrast normalisation before OCR."));
    const QCommandLineOption ocr(
        QStringLiteral("ocr"),
        QStringLiteral("Run Tesseract over rasterised pages. Off by default: it helps clean "
                       "scans and actively hurts on photographed receipts."));
    const QCommandLineOption dumpImages(
        QStringLiteral("dump-images"),
        QStringLiteral("Write the rasterised pages into this directory, for inspection."),
        QStringLiteral("dir"));

    parser.addOption(extractOnly);
    parser.addOption(textOnly);
    parser.addOption(imagesOnly);
    parser.addOption(json);
    parser.addOption(dpi);
    parser.addOption(pages);
    parser.addOption(longEdge);
    parser.addOption(languages);
    parser.addOption(ocrShortEdge);
    parser.addOption(psm);
    parser.addOption(noNormalise);
    parser.addOption(ocr);
    parser.addOption(dumpImages);
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("Invoice files to read."),
                                 QStringLiteral("[files...]"));

    // QCommandLineParser treats the first entry as the program name, so the
    // caller's list of options has to be shifted by one. Without this every
    // leading option is silently swallowed.
    QStringList fullArguments;
    fullArguments.reserve(arguments.size() + 1);
    fullArguments.append(QCoreApplication::applicationFilePath());
    fullArguments.append(arguments);
    parser.process(fullArguments);

    const QStringList files = parser.positionalArguments();
    if (files.isEmpty()) {
        err() << "no input files given" << Qt::endl << Qt::endl;
        err() << parser.helpText();
        err().flush();
        return kExitUsage;
    }

    if (parser.isSet(textOnly) && parser.isSet(imagesOnly)) {
        err() << "--text-only and --images-only cannot be combined" << Qt::endl;
        err().flush();
        return kExitUsage;
    }

    Extract::ReadOptions options;
    options.dpi = qMax(30, parser.value(dpi).toInt());
    options.maxPages = qMax(1, parser.value(pages).toInt());
    options.longEdge = qMax(200, parser.value(longEdge).toInt());
    options.ocr.languages = parser.value(languages);
    options.ocr.enabled = parser.isSet(ocr);
    options.ocr.targetShortEdge = parser.value(ocrShortEdge).toInt();
    options.ocr.pageSegMode = qBound(0, parser.value(psm).toInt(), 13);
    options.ocr.normalise = !parser.isSet(noNormalise);

    if (parser.isSet(textOnly))
        options.want = Extract::Want::Text;
    else if (parser.isSet(imagesOnly))
        options.want = Extract::Want::Images;

    const bool withText = parser.isSet(extractOnly);
    const bool asJson = parser.isSet(json);

    // Phase 1 only extracts. Say so once, on stderr, so the stdout stream stays
    // clean for piping.
    if (!withText && !asJson) {
        err() << "note: model analysis is not wired up yet, showing the extraction result "
                 "(use --json for machine readable output)"
              << Qt::endl;
        err().flush();
    }

    QJsonArray results;
    bool allOk = true;

    for (const QString &file : files) {
        const Extract::Document document = Extract::readDocument(file, options);

        if (!asJson)
            printHuman(document, options, withText);

        if (parser.isSet(dumpImages) && !document.pages.isEmpty()) {
            QStringList problems;
            dumpPages(document, parser.value(dumpImages), &problems);
            for (const QString &problem : problems) {
                err() << "warning: " << problem << Qt::endl;
            }
            err().flush();
        }

        if (!document.ok())
            allOk = false;

        results.append(toJson(document, withText));
    }

    if (asJson) {
        QJsonDocument payload;
        if (results.size() == 1)
            payload.setObject(results.first().toObject());
        else
            payload.setArray(results);
        out() << QString::fromUtf8(payload.toJson(QJsonDocument::Compact)) << Qt::endl;
        out().flush();
    }

    return allOk ? kExitOk : kExitFailure;
}

} // namespace InvoiceDrop
