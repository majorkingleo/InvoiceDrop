#include "cli.h"

#include "analysis.h"
#include "daemon.h"
#include "extract/documentreader.h"
#include "extract/ocr.h"
#include "extract/threadctx.h"
#include "invoice.h"
#include "json.h"
#include "ollama.h"
#include "paths.h"
#include "store.h"
#include "totals.h"

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

/// Prints what a file adds up to. Defined below, next to the line printer it
/// finishes off.
void printFileSum(const QVector<BillResult> &bills);

/// Prints what a daemon returned in the shape this run asked for.
///
/// The daemon answers in JSON because that is the wire format, but the caller
/// asked for either JSON or a line per bill. Reprinting the wire format either
/// way would make the output depend on whether a daemon happened to be running.
int printDelegatedResult(const QString &json, bool asJson)
{
    int failures = 0;

    // The daemon answers with one bill per line, and the sum belongs after the
    // last bill of a file. A delegated run therefore has to group again, and it
    // does so through the same BillResult the local path uses, so a run with a
    // daemon and a run without one print the same thing.
    QVector<BillResult> run;

    const auto flushRun = [&run]() {
        if (run.size() > 1)
            printFileSum(run);
        run.clear();
    };

    const QStringList lines = json.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QJsonObject object = QJsonDocument::fromJson(line.toUtf8()).object();
        if (object.isEmpty())
            continue;

        if (asJson) {
            // Passed through untouched. Adding a summary line here would put a
            // second kind of object on the wire and every consumer would have to
            // learn to skip it.
            if (object.value(QStringLiteral("status")).toString() != QStringLiteral("ok"))
                ++failures;
            out() << line << Qt::endl;
            continue;
        }

        const BillResult bill = billFromJson(object);
        if (!bill.ok)
            ++failures;

        if (!run.isEmpty() && run.last().path != bill.path)
            flushRun();
        run.append(bill);

        const QString label = bill.label();
        const QString vendor = bill.invoice.vendor.isEmpty() ? QStringLiteral("-")
                                                             : bill.invoice.vendor;
        const QString date = bill.invoice.date.isEmpty() ? QStringLiteral("-")
                                                          : bill.invoice.date;
        const QString amount = bill.invoice.grossTotal.has_value()
            ? QLocale::system().toString(*bill.invoice.grossTotal, 'f', 2) + QLatin1Char(' ')
                + bill.invoice.currency
            : QStringLiteral("-");

        out() << label << "  " << vendor << "  " << date << "  " << amount << Qt::endl;

        if (!bill.qualityWarning.isEmpty())
            err() << label << ": warning: " << bill.qualityWarning << Qt::endl;
        if (!bill.error.isEmpty())
            err() << label << ": " << bill.error << Qt::endl;
    }

    if (!asJson)
        flushRun();

    out().flush();
    err().flush();
    return failures;
}

/// `invoicedrop doctor`. Says what is missing and what command fixes it.
///
/// Every check names the thing that is absent and the command that puts it
/// there. A diagnosis that only says "not working" costs the reader a search.
int runDoctor(const Extract::ReadOptions &readOptions, OllamaClient &client, Store &store)
{
    int problems = 0;

    /// `required` is false for the parts that only one optional flag needs, so a
    /// missing tesseract does not read as a broken installation.
    const auto check = [&problems](const QString &what, bool ok, bool required,
                                   const QString &detail, const QString &fix) {
        const QString mark = ok ? QStringLiteral("  ok    ")
                                : (required ? QStringLiteral("  FAIL  ")
                                            : QStringLiteral("  --    "));
        out() << mark << what.leftJustified(22) << detail << Qt::endl;
        if (!ok && required) {
            ++problems;
            if (!fix.isEmpty())
                out() << "        fix: " << fix << Qt::endl;
        }
    };

    out() << "Building blocks" << Qt::endl;

    // MuPDF, Leptonica and Tesseract are linked in, not shelled out to, so what
    // matters is not whether a binary is on the PATH but whether the versions
    // that got linked actually work.
    const bool mupdfOk = Extract::ThreadCtx::mupdf() != nullptr;
    check(QStringLiteral("pdf engine"), mupdfOk, true,
          mupdfOk ? QStringLiteral("MuPDF context created")
                  : QStringLiteral("cannot create a MuPDF context"),
          QStringLiteral("pacman -S libmupdf"));

    Extract::OcrOptions ocrCheck = readOptions.ocr;
    ocrCheck.enabled = true; // the point is to find out whether it could work
    const bool ocrOk = Extract::Ocr::available(ocrCheck);
    check(QStringLiteral("ocr"), ocrCheck.enabled && ocrOk, false,
          ocrOk ? QStringLiteral("tesseract ready for '%1'").arg(readOptions.ocr.languages)
                : QStringLiteral("tesseract cannot load '%1'").arg(readOptions.ocr.languages),
          QStringLiteral("pacman -S tesseract tesseract-data-deu tesseract-data-eng"));

    out() << Qt::endl << "Model" << Qt::endl;

    QString probeError;
    const QStringList models = client.installedModels(&probeError);
    check(QStringLiteral("endpoint"), probeError.isEmpty(), true,
          probeError.isEmpty() ? QStringLiteral("%1, %2 model(s) available")
                                     .arg(client.options().url)
                                     .arg(models.size())
                               : probeError,
          QStringLiteral("systemctl start ollama"));

    if (probeError.isEmpty()) {
        const QString wanted = client.options().model;
        const bool found = std::any_of(models.cbegin(), models.cend(),
                                       [&wanted](const QString &name) {
            return name == wanted
                || name.section(QLatin1Char(':'), 0, 0) == wanted.section(QLatin1Char(':'), 0, 0);
        });

        check(QStringLiteral("model"), found, true,
              found ? wanted : QStringLiteral("'%1' is not installed").arg(wanted),
              QStringLiteral("ollama pull %1").arg(wanted));
    }

    out() << Qt::endl << "Storage" << Qt::endl;

    check(QStringLiteral("database"), store.isOpen(), true,
          store.isOpen() ? QStringLiteral("%1, %2 bill(s)")
                               .arg(store.databasePath())
                               .arg(store.billCount())
                         : store.error(),
          QStringLiteral("check that the folder is writable"));

    const QString inbox = QDir(Paths::dataDir()).filePath(QStringLiteral("inbox"));
    QDir inboxDir(inbox);
    const bool inboxOk = inboxDir.exists() || inboxDir.mkpath(QStringLiteral("."));
    check(QStringLiteral("inbox"), inboxOk, true,
          inboxOk ? inbox : QStringLiteral("cannot create %1").arg(inbox),
          QStringLiteral("check the permissions on %1").arg(Paths::dataDir()));

    out() << Qt::endl << "Extras" << Qt::endl;

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface *busInterface = bus.isConnected() ? bus.interface() : nullptr;
    const bool daemonUp = busInterface
        && busInterface->isServiceRegistered(QString::fromLatin1(Daemon::kServiceName));
    check(QStringLiteral("daemon"), true, false,
          daemonUp ? QStringLiteral("running, calls are delegated to it")
                   : QStringLiteral("not running, every call loads the model itself"),
          QString());

    const bool notifications = busInterface
        && busInterface->isServiceRegistered(QStringLiteral("org.freedesktop.Notifications"));
    check(QStringLiteral("notifications"), notifications, false,
          notifications ? QStringLiteral("a notification service is on the bus")
                        : QStringLiteral("no notification service on the bus"),
          QString());

    out() << Qt::endl
          << (problems == 0 ? QStringLiteral("Everything needed is in place.")
                            : QStringLiteral("%1 problem(s) found.").arg(problems))
          << Qt::endl;
    out().flush();

    return problems == 0 ? kExitOk : kExitFailure;
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

/// `invoicedrop --wipe`. Removes what the tool has collected.
///
/// Three places are emptied, and for different reasons. The database holds the
/// bills and the cached reads, and is what `history` prints. The archive holds
/// the originals `--move` put aside, which for a document that was only ever
/// dropped once is the only copy left. The inbox holds what the daemon has not
/// read yet, and leaving it behind would mean the data came back on the next
/// daemon start, which is the opposite of what a wipe is asked for.
///
/// Only the paths InvoiceDrop resolves on its own are touched. `--inbox` names a
/// directory the daemon happens to watch, and that can be any folder the reader
/// keeps documents in, so `--wipe --inbox ~/Belege` must not delete `~/Belege`.
/// The directories themselves stay, because the daemon watches them and a
/// missing inbox is a different problem than an empty one.
int runWipe(Store &store)
{
    const int documents = store.documents().size();
    const int bills = store.billCount();
    const bool cleared = store.clear();

    int failures = 0;

    /// Empties a directory without removing it, and returns how many entries
    /// went. Entry by entry rather than `removeRecursively()` on the folder, so
    /// one unreadable file does not abandon the rest.
    const auto emptyDirectory = [&failures](const QString &path) {
        QDir directory(path);
        if (!directory.exists())
            return 0;

        int removed = 0;
        const QFileInfoList entries = directory.entryInfoList(
            QDir::NoDotAndDotDot | QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System);
        for (const QFileInfo &entry : entries) {
            const bool ok = entry.isDir() ? QDir(entry.absoluteFilePath()).removeRecursively()
                                          : QFile::remove(entry.absoluteFilePath());
            if (ok) {
                ++removed;
            } else {
                ++failures;
                err() << "cannot remove " << entry.absoluteFilePath() << Qt::endl;
            }
        }
        return removed;
    };

    const QString archive = archiveDirectory();
    const int archived = emptyDirectory(archive);
    const QString inbox = QDir(Paths::dataDir()).filePath(QStringLiteral("inbox"));
    const int waiting = emptyDirectory(inbox);

    if (!cleared) {
        err() << "cannot empty " << store.databasePath() << ": " << store.error() << Qt::endl;
        err().flush();
        return kExitFailure;
    }

    out() << "wiped " << bills << " bill(s) in " << documents << " document(s) from "
          << store.databasePath() << Qt::endl;
    out() << "wiped " << archived << " file(s) from " << archive << Qt::endl;
    out() << "wiped " << waiting << " file(s) from " << inbox << Qt::endl;
    out().flush();
    err().flush();

    return failures == 0 ? kExitOk : kExitFailure;
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

/// Prints what a file adds up to.
///
/// Only for a file that holds more than one bill. A single page file would get a
/// sum line that repeats the line above it word for word, and a report full of
/// duplicated lines is harder to read than one without sums at all.
///
/// The bill count is printed even when nothing is missing, because a total
/// without it cannot be checked against the file: `177,76 EUR` claims nothing,
/// `177,76 EUR (3 bills)` claims that three bills are in there.
void printFileSum(const QVector<BillResult> &bills)
{
    const FileTotal total = totalFor(bills);
    const QString name = QFileInfo(bills.first().path).fileName();

    out() << name << "  sum  " << formatTotal(total) << "  (" << formatCoverage(total) << ')'
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
                                  QStringLiteral("Write one JSON object per bill, one per line, to "
                                                 "stdout."));
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
        QStringLiteral("Send no desktop notification for this run. With `daemon` the watcher "
                       "stays silent, and a running daemon that reads the files for this "
                       "command stays silent too."));
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
    const QCommandLineOption wipe(
        QStringLiteral("wipe"),
        QStringLiteral("Delete every stored bill, the archive and the inbox. Takes no files. "
                       "This removes documents, not just what was read out of them."));

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
    parser.addOption(wipe);
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

    // Absolute before anything else looks at them. A relative path is only
    // meaningful next to the shell it was typed in, and the daemon that may end
    // up doing the reading was started by the session bus in `$HOME`, where
    // `tests/testdata/rechnung.pdf` does not exist. The subcommands below are
    // matched on the first argument, and `resolvePath` leaves those words alone
    // because there is no such file to canonicalise.
    const QStringList positional = parser.positionalArguments();

    // `--wipe` is the one thing that names nothing and still has work to do, so
    // it is exempt from the file check. It is a flag rather than a subcommand
    // because it is a property of the store that every other command shares:
    // `--wipe` next to `history`, `doctor` or a file would be two jobs in one
    // command, and which one came first would be guesswork.
    if (positional.isEmpty() && !parser.isSet(wipe)) {
        err() << "no input files given" << Qt::endl << Qt::endl;
        err() << parser.helpText();
        err().flush();
        return kExitUsage;
    }

    if (parser.isSet(wipe) && !positional.isEmpty()) {
        err() << "--wipe takes no files" << Qt::endl;
        err().flush();
        return kExitUsage;
    }

    // `history`, `daemon` and `doctor` are the subcommands. They are recognised
    // as the first positional argument because everything else is a file.
    //
    // The list can be empty at this point, for `--wipe`, which names no files:
    // `positional.first()` on it is an assertion failure, not an empty string.
    const QString first = positional.isEmpty() ? QString() : positional.first();
    const bool historyMode = first == QStringLiteral("history") && !parser.isSet(extractOnly);
    const bool daemonMode = first == QStringLiteral("daemon") && !parser.isSet(extractOnly);
    const bool doctorMode = first == QStringLiteral("doctor") && !parser.isSet(extractOnly);

    const bool subcommand = historyMode || daemonMode || doctorMode;

    // A relative path is only meaningful next to the shell it was typed in, and
    // the daemon that may end up doing the reading was started by the session
    // bus in `$HOME`, where `tests/testdata/rechnung.pdf` does not exist. So the
    // paths are made absolute here, at the edge, before they are printed, hashed,
    // cached or sent over the bus.
    //
    // After the subcommand check, not before: `resolvePath` turns a word into
    // `<cwd>/word` when no such file exists, which would hide `history` from the
    // comparison above. It also means `invoicedrop history` keeps working from
    // any directory, where an absolute path would be a file that is not there.
    QStringList files = positional;
    if (!subcommand) {
        for (QString &file : files)
            file = Paths::resolvePath(file);
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

    Store store(parser.isSet(database) ? parser.value(database) : Store::defaultPath());
    if (!store.isOpen()) {
        err() << "cannot open " << store.databasePath() << ": " << store.error() << Qt::endl;
        err().flush();
        return kExitFailure;
    }

    // Before the subcommands, because a wipe is not something they do: the store
    // is emptied once and the run ends, whether the reader also typed `history`
    // or `doctor`. Those two are rejected above as files next to `--wipe`.
    if (parser.isSet(wipe))
        return runWipe(store);

    if (historyMode)
        return runHistory(store, parser.value(limit).toInt(), asJson);

    if (doctorMode) {
        OllamaOptions doctorOptions;
        if (parser.isSet(ollamaUrl))
            doctorOptions.url = parser.value(ollamaUrl);
        doctorOptions.model = parser.value(model);
        doctorOptions.timeoutMs = 10000;

        OllamaClient doctorClient(doctorOptions);
        return runDoctor(options, doctorClient, store);
    }

    // A running daemon already has the model loaded. Handing the work over turns
    // a cold start into a queue entry, and the reply is the same JSON the local
    // path would have produced. --no-cache and --dump-images stay local, because
    // the daemon cannot honour them.
    const bool delegatable = !extractOnlyMode && !parser.isSet(local)
        && !parser.isSet(noCache) && !parser.isSet(dumpImages) && !daemonMode && !doctorMode;

    if (delegatable) {
        const QDBusConnection bus = QDBusConnection::sessionBus();
        QDBusConnectionInterface *busInterface = bus.isConnected() ? bus.interface() : nullptr;
        if (busInterface) {
            const QString service = QString::fromLatin1(Daemon::kServiceName);
            bool registered = busInterface->isServiceRegistered(service);

            if (!registered) {
                // Ask the bus to start the daemon. This is what makes the first
                // drop pay off: it costs one model load, and every later call
                // finds the weights already resident. Without a service file
                // the bus refuses immediately and nothing changes.
                const QDBusReply<void> started = busInterface->startService(service);
                registered = started.isValid();
            }

            if (registered) {
                QDBusInterface control(service, QString::fromLatin1(Daemon::kObjectPath),
                                       QStringLiteral("org.kde.invoicedrop.Control"), bus);
                if (control.isValid()) {
                    // The daemon raises the toast, so whether one is wanted has
                    // to travel with the call. `--no-notify` does, which is what
                    // lets the widget ask for silence without giving up the warm
                    // model by reading locally instead.
                    const bool wanted = !parser.isSet(noNotify);
                    QDBusReply<QString> reply =
                        control.call(QStringLiteral("Analyze"), files, wanted);
                    if (!reply.isValid() && wanted) {
                        // A daemon from before the second argument existed
                        // exports `Analyze(as)` only. Retry rather than fall back
                        // to a local read, which would pay a model load for the
                        // whole session of a daemon that is merely one version
                        // behind.
                        reply = control.call(QStringLiteral("Analyze"), files);
                    }
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

        // After the file's bills, not after each one: the sum belongs to the
        // file, and printing it between two bills of the same file would read as
        // a total for the first one.
        if (!asJson && bills.size() > 1)
            printFileSum(bills);
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
