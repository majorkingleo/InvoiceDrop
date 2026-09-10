#include "cli.h"

#include "analysis.h"
#include "daemon.h"
#include "extract/documentreader.h"
#include "invoice.h"
#include "json.h"
#include "ollama.h"
#include "paths.h"
#include "store.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QStandardPaths>
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

/// Where processed originals go when --move is used.
QString archiveDirectory()
{
    return Paths::archiveDir();
}

/// Moves a fully read original out of the way. Never overwrites: a second file
/// with the same name gets a counter.
QString archiveOriginal(const QString &path, QString *error)
{
    QDir archive(archiveDirectory());
    if (!archive.exists() && !archive.mkpath(QStringLiteral("."))) {
        *error = QStringLiteral("cannot create %1").arg(archive.absolutePath());
        return {};
    }

    const QFileInfo info(path);
    QString target = archive.filePath(info.fileName());
    for (int counter = 1; QFile::exists(target); ++counter) {
        target = archive.filePath(QStringLiteral("%1-%2.%3")
                                      .arg(info.completeBaseName())
                                      .arg(counter)
                                      .arg(info.suffix()));
    }

    if (!QFile::rename(path, target)) {
        *error = QStringLiteral("cannot move %1 to %2").arg(path, target);
        return {};
    }
    return target;
}

/// Prints what a daemon returned in the shape this run asked for.
///
/// The daemon answers in JSON because that is the wire format, but the caller
/// asked for either JSON or a line per bill. Reprinting the wire format either
/// way would make the output depend on whether a daemon happened to be running.
int printDelegatedResult(const QString &json, bool asJson)
{
    int failures = 0;

    const QStringList lines = json.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QJsonObject object = QJsonDocument::fromJson(line.toUtf8()).object();
        if (object.isEmpty())
            continue;

        const QString status = object.value(QStringLiteral("status")).toString();
        if (status != QStringLiteral("ok"))
            ++failures;

        if (asJson) {
            out() << line << Qt::endl;
            continue;
        }

        const QString file = object.value(QStringLiteral("file")).toString();
        const int page = object.value(QStringLiteral("bill")).toInt();
        const int pageCount = object.value(QStringLiteral("bill_count")).toInt();
        const QString label = pageCount > 1 ? QStringLiteral("%1:%2").arg(file).arg(page) : file;

        const QString vendor = object.value(QStringLiteral("vendor")).toString();
        const QString date = object.value(QStringLiteral("date")).toString();
        const QJsonValue total = object.value(QStringLiteral("gross_total"));
        const QString currency = object.value(QStringLiteral("currency")).toString();

        const QString amount = total.isDouble()
            ? QLocale::system().toString(total.toDouble(), 'f', 2) + QLatin1Char(' ') + currency
            : QStringLiteral("-");

        out() << label << "  " << (vendor.isEmpty() ? QStringLiteral("-") : vendor) << "  "
              << (date.isEmpty() ? QStringLiteral("-") : date) << "  " << amount << Qt::endl;

        const QString warning = object.value(QStringLiteral("quality_warning")).toString();
        const QString error = object.value(QStringLiteral("error")).toString();
        if (!warning.isEmpty())
            err() << label << ": warning: " << warning << Qt::endl;
        if (!error.isEmpty())
            err() << label << ": " << error << Qt::endl;
    }

    out().flush();
    err().flush();
    return failures;
}

/// `invoicedrop history`. Lists what is stored, newest first.
int runHistory(Store &store, int limit, bool asJson){
    const QVector<StoredBill> bills = store.recent(qMax(1, limit));

    if (asJson) {
        for (const StoredBill &bill : bills) {
            QJsonObject object;
            object.insert(QStringLiteral("file"), bill.fileName);
            object.insert(QStringLiteral("page"), bill.page);
            object.insert(QStringLiteral("status"), bill.status);
            object.insert(QStringLiteral("vendor"), bill.vendor);
            object.insert(QStringLiteral("date"), bill.date);
            object.insert(QStringLiteral("gross_total"),
                          bill.grossTotal.has_value() ? QJsonValue(*bill.grossTotal)
                                                      : QJsonValue(QJsonValue::Null));
            object.insert(QStringLiteral("currency"), bill.currency);
            object.insert(QStringLiteral("model"), bill.model);
            object.insert(QStringLiteral("created_at"), bill.createdAt);

            const QJsonDocument document(object);
            out() << QString::fromUtf8(document.toJson(QJsonDocument::Compact)) << Qt::endl;
        }
        out().flush();
        return kExitOk;
    }

    if (bills.isEmpty()) {
        out() << "nothing stored yet in " << store.databasePath() << Qt::endl;
        out().flush();
        return kExitOk;
    }

    for (const StoredBill &bill : bills) {
        const QString amount = bill.grossTotal.has_value()
            ? QLocale::system().toString(*bill.grossTotal, 'f', 2) + QLatin1Char(' ')
                + bill.currency
            : QStringLiteral("-");

        // The file name goes last: it is the field that varies most, so it is
        // the one allowed to push a narrow terminal into wrapping. The page
        // marker sits in front of it, where a wrap cannot orphan it.
        out() << bill.createdAt.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')) << "  "
              << bill.vendor.left(20).leftJustified(20) << "  "
              << bill.date.leftJustified(10, QLatin1Char(' '), true).left(10) << "  "
              << amount.rightJustified(12) << "  p" << bill.page << ' ' << bill.fileName
              << Qt::endl;
    }
    out() << bills.size() << " of " << store.billCount() << " stored bill(s), database "
          << store.databasePath() << Qt::endl;
    out().flush();
    return kExitOk;
}

void printAnalysisLine(const QString &label, const Invoice &invoice)
{
    const QString amount = invoice.grossTotal.has_value()
        ? QLocale::system().toString(*invoice.grossTotal, 'f', 2) + QLatin1Char(' ')
            + invoice.currency
        : QStringLiteral("-");

    const QString vendor = invoice.vendor.isEmpty() ? QStringLiteral("-") : invoice.vendor;
    const QString date = invoice.date.isEmpty() ? QStringLiteral("-") : invoice.date;

    out() << label << "  " << vendor << "  " << date << "  " << amount << Qt::endl;
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
        const Extract::DocumentPage *raster = nullptr;
        for (const Extract::DocumentPage &page : std::as_const(document.pages)) {
            if (!page.jpeg.isEmpty()) {
                raster = &page;
                break;
            }
        }
        if (raster) {
            stream << "  pages      " << document.pages.size() << " (" << raster->width << "x"
                   << raster->height << " -> " << options.longEdge << " px long edge)" << Qt::endl;
        } else if (!document.pages.isEmpty()) {
            stream << "  pages      " << document.pages.size() << " (text layer only)"
                   << Qt::endl;
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

bool dumpPages(const Extract::Document &document, const QString &directory, QStringList *problems)
{
    QDir target(directory);
    if (!target.exists() && !target.mkpath(QStringLiteral("."))) {
        problems->append(QStringLiteral("cannot create image directory %1").arg(directory));
        return false;
    }

    const QString base = QFileInfo(document.path).completeBaseName();
    for (const Extract::DocumentPage &page : std::as_const(document.pages)) {
        if (page.jpeg.isEmpty())
            continue;
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
    const QCommandLineOption database(
        QStringLiteral("db"),
        QStringLiteral("Database file. Defaults to invoicedrop.db in the application data "
                       "directory."),
        QStringLiteral("path"));
    const QCommandLineOption noCache(
        QStringLiteral("no-cache"),
        QStringLiteral("Read the document again even if its hash is already stored."));
    const QCommandLineOption move(
        QStringLiteral("move"),
        QStringLiteral("Move the original into the archive folder once every bill was read."));
    const QCommandLineOption limit(
        QStringLiteral("limit"),
        QStringLiteral("How many bills `history` lists."),
        QStringLiteral("count"), QStringLiteral("20"));
    const QCommandLineOption inbox(
        QStringLiteral("inbox"),
        QStringLiteral("Folder the daemon watches."),
        QStringLiteral("dir"), QDir(Paths::dataDir()).filePath(QStringLiteral("inbox")));
    const QCommandLineOption once(
        QStringLiteral("once"),
        QStringLiteral("With `daemon`: read the inbox and exit instead of watching."));
    const QCommandLineOption noNotify(
        QStringLiteral("no-notify"),
        QStringLiteral("With `daemon`: send no desktop notifications."));
    const QCommandLineOption local(
        QStringLiteral("local"),
        QStringLiteral("Read the document here instead of asking a running daemon to do it."));
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
    parser.addOption(database);
    parser.addOption(noCache);
    parser.addOption(move);
    parser.addOption(limit);
    parser.addOption(inbox);
    parser.addOption(once);
    parser.addOption(noNotify);
    parser.addOption(local);
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

    // `history` and `daemon` are the subcommands. They are recognised as the
    // first positional argument because everything else is a file to read.
    const bool historyMode =
        files.first() == QStringLiteral("history") && !parser.isSet(extractOnly);
    const bool daemonMode =
        files.first() == QStringLiteral("daemon") && !parser.isSet(extractOnly);

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

    Store store(parser.isSet(database) ? parser.value(database) : Store::defaultPath());
    if (!store.isOpen()) {
        err() << "cannot open " << store.databasePath() << ": " << store.error() << Qt::endl;
        err().flush();
        return kExitFailure;
    }

    if (historyMode)
        return runHistory(store, parser.value(limit).toInt(), asJson);

    // A running daemon already has the model loaded. Handing the work over turns
    // a cold start into a queue entry, and the reply is the same JSON the local
    // path would have produced. --no-cache and --dump-images stay local, because
    // the daemon cannot honour them.
    const bool delegatable = !extractOnlyMode && !parser.isSet(local)
        && !parser.isSet(noCache) && !parser.isSet(dumpImages) && !daemonMode;

    if (delegatable) {
        const QDBusConnection bus = QDBusConnection::sessionBus();
        QDBusConnectionInterface *busInterface = bus.isConnected() ? bus.interface() : nullptr;
        if (busInterface
            && busInterface->isServiceRegistered(QString::fromLatin1(Daemon::kServiceName))) {
            QDBusInterface control(QString::fromLatin1(Daemon::kServiceName),
                                   QString::fromLatin1(Daemon::kObjectPath),
                                   QStringLiteral("org.kde.invoicedrop.Control"), bus);
            if (control.isValid()) {
                const QDBusReply<QString> reply = control.call(QStringLiteral("Analyze"), files);
                if (reply.isValid()) {
                    const int failures = printDelegatedResult(reply.value(), asJson);
                    return failures == 0 ? kExitOk : kExitFailure;
                }
                err() << "the daemon did not answer (" << reply.error().message()
                      << "), reading here instead" << Qt::endl;
                err().flush();
            }
        }
    }

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

    if (daemonMode) {
        DaemonOptions daemonOptions;
        daemonOptions.read = options;
        daemonOptions.ollama = ollamaOptions;
        daemonOptions.inbox = parser.value(inbox);
        daemonOptions.notify = !parser.isSet(noNotify);
        daemonOptions.watch = !parser.isSet(once);

        Daemon daemon(daemonOptions, &store);
        QString startError;
        if (!daemon.start(&startError)) {
            err() << startError << Qt::endl;
            err().flush();
            return kExitFailure;
        }

        if (!daemonOptions.watch) {
            const int handled = daemon.drainInbox();
            err() << "read " << handled << " file(s) from " << daemonOptions.inbox << Qt::endl;
            err().flush();
            return kExitOk;
        }

        err() << "watching " << daemonOptions.inbox << " with " << ollamaOptions.model
              << "\ndatabase " << store.databasePath() << "\nnotifications "
              << (daemonOptions.notify ? "on" : "off") << Qt::endl;
        err().flush();

        return QCoreApplication::exec();
    }

    QJsonArray results;
    bool allOk = true;

    for (const QString &file : files) {
        // --dump-images works off the raw extraction, so it needs the document
        // rather than the analysed bills. Only the debug flag pays for the
        // second read.
        if (parser.isSet(dumpImages)) {
            const Extract::Document document = Extract::readDocument(file, options);
            if (document.ok()) {
                QStringList problems;
                dumpPages(document, parser.value(dumpImages), &problems);
                for (const QString &problem : problems)
                    err() << "warning: " << problem << Qt::endl;
                err().flush();
            }
        }

        if (extractOnlyMode) {
            const Extract::Document document = Extract::readDocument(file, options);
            if (!document.ok()) {
                allOk = false;
                if (asJson)
                    results.append(extractionToJson(document, withText));
                else
                    err() << QFileInfo(file).fileName() << ": " << document.error << Qt::endl;
                err().flush();
                continue;
            }
            if (asJson)
                results.append(extractionToJson(document, withText));
            else
                printHuman(document, options, withText);
            continue;
        }

        // One record per bill, and a bill is one page.
        Store *cache = parser.isSet(noCache) ? nullptr : &store;
        const QVector<BillResult> bills = analyseFile(file, options, client, cache);

        if (parser.isSet(move)) {
            bool everyBillRead = !bills.isEmpty();
            for (const BillResult &bill : bills) {
                if (!bill.ok)
                    everyBillRead = false;
            }

            if (everyBillRead) {
                QString moveError;
                const QString target = archiveOriginal(file, &moveError);
                if (!target.isEmpty()) {
                    err() << QFileInfo(file).fileName() << ": moved to " << target << Qt::endl;
                } else {
                    err() << QFileInfo(file).fileName() << ": " << moveError << Qt::endl;
                }
                err().flush();
            }
        }

        for (const BillResult &bill : bills) {
            if (!bill.ok)
                allOk = false;

            if (verbose) {
                err() << bill.label() << ": extract " << bill.extractMs << " ms"
                      << ", model " << bill.inferMs << " ms"
                      << (bill.fromCache ? ", cached"
                                         : (bill.fromTextLayer ? ", text layer" : ", raster"))
                      << Qt::endl;
                for (const QString &note : bill.notes)
                    err() << "  note: " << note << Qt::endl;
                if (!bill.qualityWarning.isEmpty())
                    err() << "  warning: " << bill.qualityWarning << Qt::endl;
                if (!bill.error.isEmpty())
                    err() << "  problem: " << bill.error << Qt::endl;
                err().flush();
            }

            if (asJson) {
                results.append(billToJson(bill));
            } else {
                printAnalysisLine(bill.label(), bill.invoice);
                if (!bill.qualityWarning.isEmpty()) {
                    err() << bill.label() << ": warning: " << bill.qualityWarning << Qt::endl;
                    err().flush();
                }
                if (!bill.ok) {
                    err() << bill.label() << ": " << bill.error << Qt::endl;
                    err().flush();
                }
            }
        }
    }

    if (asJson) {
        // One object per line, always, whatever the number of files. An array
        // for several files and a bare object for one would make `jq '.vendor'`
        // work interactively and fail in a loop, which is the kind of difference
        // nobody remembers.
        for (const QJsonValue &entry : results) {
            const QJsonDocument document(entry.toObject());
            out() << QString::fromUtf8(document.toJson(QJsonDocument::Compact)) << Qt::endl;
        }
        out().flush();
    }

    return allOk ? kExitOk : kExitFailure;
}

} // namespace InvoiceDrop
