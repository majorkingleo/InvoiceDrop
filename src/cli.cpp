#include "cli.h"

#include "extract/documentreader.h"
#include "invoice.h"
#include "ollama.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTextStream>

#include <algorithm>
#include <limits>

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

/// True when the raster is too coarse to carry the text the model claims to
/// have read.
///
/// Measured: a 174 px wide receipt photo makes every model invent a vendor, a
/// date and a total, complete with a currency that is not on the paper. Numbers
/// like that are worse than no numbers, so they get flagged rather than printed
/// as fact.
QString qualityWarning(const Extract::Document &document)
{
    if (document.hasTextLayer || document.pages.isEmpty())
        return {};

    int shortest = std::numeric_limits<int>::max();
    for (const Extract::PageImage &page : document.pages)
        shortest = qMin(shortest, qMin(page.width, page.height));

    constexpr int kLegibleShortEdge = 400;
    if (shortest == std::numeric_limits<int>::max() || shortest >= kLegibleShortEdge)
        return {};

    return QStringLiteral("the source is only %1 px across, so the model had to guess; "
                          "treat the values as unverified")
        .arg(shortest);
}

void printAnalysisLine(const QString &path, const Invoice &invoice)
{
    const QString amount = invoice.grossTotal.has_value()
        ? QLocale::system().toString(*invoice.grossTotal, 'f', 2) + QLatin1Char(' ')
            + invoice.currency
        : QStringLiteral("-");

    const QString vendor = invoice.vendor.isEmpty() ? QStringLiteral("-") : invoice.vendor;
    const QString date = invoice.date.isEmpty() ? QStringLiteral("-") : invoice.date;

    out() << QFileInfo(path).fileName() << "  " << vendor << "  " << date << "  " << amount
          << Qt::endl;
    out().flush();
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

QJsonObject toJson(const Extract::Document &document,
                   const Invoice *invoice,
                   bool withText,
                   const QString &failure,
                   const QString &warning)
{
    QJsonObject object;
    object.insert(QStringLiteral("file"), QFileInfo(document.path).fileName());
    object.insert(QStringLiteral("path"), document.path);
    object.insert(QStringLiteral("kind"), Extract::kindName(document.kind));
    object.insert(QStringLiteral("status"), failure.isEmpty() ? QStringLiteral("ok")
                                                               : QStringLiteral("error"));
    if (!failure.isEmpty())
        object.insert(QStringLiteral("error"), failure);
    if (!warning.isEmpty())
        object.insert(QStringLiteral("quality_warning"), warning);

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

    // Flat, so `jq '.vendor, .gross_total'` works as documented.
    if (invoice) {
        const QJsonObject fields = invoice->toJson();
        for (auto entry = fields.constBegin(); entry != fields.constEnd(); ++entry)
            object.insert(entry.key(), entry.value());
    }

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
    const QCommandLineOption ocrLanguages(
        QStringLiteral("ocr-lang"),
        QStringLiteral("Tesseract language pack for --ocr, for example deu+eng."),
        QStringLiteral("langs"), QStringLiteral("deu+eng"));
    const QCommandLineOption language(
        QStringLiteral("lang"),
        QStringLiteral("Language the model writes free text fields in."),
        QStringLiteral("code"), QStringLiteral("de"));
    const QCommandLineOption model(
        QStringLiteral("model"),
        QStringLiteral("Ollama model. Must report the vision capability for scans and photos."),
        QStringLiteral("name"), QString::fromUtf8(kDefaultModel));
    const QCommandLineOption ollamaUrl(
        QStringLiteral("ollama-url"),
        QStringLiteral("Ollama base URL. Defaults to $OLLAMA_HOST or 127.0.0.1:11434."),
        QStringLiteral("url"));
    const QCommandLineOption timeout(
        QStringLiteral("timeout"),
        QStringLiteral("Seconds to wait for one answer from the model."),
        QStringLiteral("seconds"), QStringLiteral("300"));
    const QCommandLineOption think(
        QStringLiteral("think"),
        QStringLiteral("Let a reasoning model think before answering. Slower, and no more "
                       "accurate on invoices: measured 10.2 s against 1.0 s."));
    const QCommandLineOption verboseOption(
        QStringLiteral("verbose"),
        QStringLiteral("Print extraction notes and timings on stderr."));
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
    parser.addOption(ocrLanguages);
    parser.addOption(language);
    parser.addOption(model);
    parser.addOption(ollamaUrl);
    parser.addOption(timeout);
    parser.addOption(think);
    parser.addOption(verboseOption);
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
    options.ocr.languages = parser.value(ocrLanguages);
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
    const bool verbose = parser.isSet(verboseOption);
    const bool extractOnlyMode = parser.isSet(extractOnly);

    OllamaOptions ollamaOptions;
    if (parser.isSet(ollamaUrl))
        ollamaOptions.url = parser.value(ollamaUrl);
    ollamaOptions.model = parser.value(model);
    ollamaOptions.language = parser.value(language);
    ollamaOptions.think = parser.isSet(think);
    ollamaOptions.timeoutMs = qMax(1, parser.value(timeout).toInt()) * 1000;

    OllamaClient client(ollamaOptions);

    // One check up front beats the same failure repeated per file, and it lets
    // the message name the exact command that fixes it.
    if (!extractOnlyMode) {
        QString probeError;
        const QStringList installed = client.installedModels(&probeError);
        if (!probeError.isEmpty()) {
            err() << probeError << Qt::endl;
            err().flush();
            return kExitFailure;
        }

        const QString wanted = ollamaOptions.model;
        const bool found = std::any_of(installed.cbegin(), installed.cend(), [&wanted](const QString &name) {
            return name == wanted
                || name.section(QLatin1Char(':'), 0, 0) == wanted.section(QLatin1Char(':'), 0, 0);
        });
        if (!found) {
            err() << "model '" << wanted << "' is not installed - run: ollama pull " << wanted
                  << Qt::endl;
            err() << "installed: " << installed.join(QStringLiteral(", ")) << Qt::endl;
            err().flush();
            return kExitFailure;
        }
    }

    QJsonArray results;
    bool allOk = true;

    for (const QString &file : files) {
        const Extract::Document document = Extract::readDocument(file, options);

        // An extraction failure is fatal for the file: there is nothing left to
        // send to the model.
        if (!document.ok()) {
            allOk = false;
            if (asJson)
                results.append(toJson(document, nullptr, withText, document.error, QString()));
            else
                err() << QFileInfo(file).fileName() << ": " << document.error << Qt::endl;
            err().flush();
            continue;
        }

        if (parser.isSet(dumpImages) && !document.pages.isEmpty()) {
            QStringList problems;
            dumpPages(document, parser.value(dumpImages), &problems);
            for (const QString &problem : problems)
                err() << "warning: " << problem << Qt::endl;
            err().flush();
        }

        if (extractOnlyMode) {
            if (asJson)
                results.append(toJson(document, nullptr, withText, QString(), QString()));
            else
                printHuman(document, options, withText);
            continue;
        }

        QList<QByteArray> pages;
        pages.reserve(document.pages.size());
        for (const Extract::PageImage &page : document.pages)
            pages.append(page.jpeg);

        Invoice invoice;
        QString analysisError;
        const bool analysed = client.analyse(document.text, pages, &invoice, &analysisError);
        if (!analysed)
            allOk = false;

        const QString warning = analysed ? qualityWarning(document) : QString();

        if (verbose) {
            err() << QFileInfo(file).fileName() << ": " << document.pages.size() << " page image(s)"
                  << ", " << document.text.size() << " text chars"
                  << ", extract " << document.elapsedMs << " ms"
                  << ", model " << client.lastInferenceMs() << " ms" << Qt::endl;
            for (const QString &note : document.notes)
                err() << "  note: " << note << Qt::endl;
            if (!analysisError.isEmpty())
                err() << "  problem: " << analysisError << Qt::endl;
            err().flush();
        }

        if (asJson) {
            results.append(toJson(document, &invoice, withText, analysisError, warning));
        } else {
            printAnalysisLine(file, invoice);
            if (!warning.isEmpty()) {
                err() << QFileInfo(file).fileName() << ": warning: " << warning << Qt::endl;
                err().flush();
            }
            if (!analysed) {
                err() << QFileInfo(file).fileName() << ": " << analysisError << Qt::endl;
                err().flush();
            }
        }
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
