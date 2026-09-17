#include "analysis.h"

#include "hash.h"
#include "log.h"
#include "paths.h"
#include "store.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>

namespace InvoiceDrop {
namespace {

/// Below this short edge a raster cannot carry the text a model claims to have
/// read. Measured: at 174 px every model tested invents a vendor, a date and a
/// total, and one of them invented a currency that is not on the paper.
constexpr int kLegibleShortEdge = 400;

QString warningFor(const Extract::DocumentPage &page)
{
    if (page.fromTextLayer || page.jpeg.isEmpty())
        return {};

    const int shortest = qMin(page.width, page.height);
    if (shortest <= 0 || shortest >= kLegibleShortEdge)
        return {};

    return QStringLiteral("the source is only %1 px across, so the model had to guess; "
                          "treat the values as unverified")
        .arg(shortest);
}

/// Rebuilds a bill from the database, so a cached run does not touch the file.
BillResult fromStored(const QString &path, const StoredBill &stored, int pageCount)
{
    BillResult bill;
    bill.path = path;
    bill.page = stored.page;
    bill.pageCount = pageCount;
    bill.ok = stored.status == QStringLiteral("ok");
    bill.fromCache = true;
    bill.invoice = Invoice::fromJson(
        QJsonDocument::fromJson(stored.payload.toUtf8()).object());

    if (!bill.ok)
        bill.error = QStringLiteral("stored as %1").arg(stored.status);
    bill.notes.append(QStringLiteral("answered from the cache, stored %1").arg(stored.createdAt));
    return bill;
}

} // namespace

QString BillResult::label() const
{
    const QString name = QFileInfo(path).fileName();
    if (pageCount <= 1)
        return name;
    return QStringLiteral("%1:%2").arg(name).arg(page);
}

QVector<BillResult> analyseFile(const QString &path,
                                const Extract::ReadOptions &readOptions,
                                OllamaClient &client,
                                Store *cache)
{
    QVector<BillResult> bills;

    // Resolved once, at the only entry point every caller shares. The daemon is
    // reached over D-Bus with no idea of the caller's working directory, so a
    // relative path has to become absolute before it can cross that boundary at
    // all; doing it here rather than in the CLI also means a future caller
    // cannot reintroduce the same silent failure.
    const QString target = Paths::resolvePath(path);

    // Hashing is a few milliseconds, rasterising and inferring are seconds, so
    // the cache is consulted before the file is opened at all.
    QString sha256;
    if (cache) {
        sha256 = fileSha256(target);
        const QString fingerprint = Store::fingerprint(readOptions, client.options().model);

        // The two keys a cached answer stands on. A run that returns the wrong
        // bill is nearly always a run whose fingerprint matched when it should
        // not have, so both are worth seeing.
        if (!sha256.isEmpty())
            Log::step("cache", QStringLiteral("sha256 %1..., fingerprint %2...")
                                   .arg(sha256.left(12), fingerprint.left(12)));

        if (!sha256.isEmpty()) {
            const std::optional<CachedDocument> cached = cache->find(sha256, fingerprint);
            if (cached.has_value()) {
                for (const StoredBill &stored : cache->bills(sha256))
                    bills.append(fromStored(target, stored, cached->pageCount));
                if (!bills.isEmpty()) {
                    Log::step("cache", QStringLiteral("hit, stored %1: %2 bill(s), no model call")
                                           .arg(cached->createdAt)
                                           .arg(bills.size()));
                    return bills;
                }
                // A document row without bills is a half written record. Fall
                // through and read the file again rather than report nothing.
                Log::warn("cache",
                          QStringLiteral("the stored document holds no bill, reading again"));
            }
        }
    } else {
        Log::step("cache", QStringLiteral("not consulted for this run"));
    }

    if (cache)
        Log::step("cache", QStringLiteral("miss, reading the document"));

    const Extract::Document document = Extract::readDocument(target, readOptions);

    if (!document.ok()) {
        BillResult failed;
        failed.path = target;
        failed.page = 1;
        failed.pageCount = 1;
        failed.error = document.error;
        failed.extractMs = document.elapsedMs;
        failed.notes = document.notes;
        bills.append(failed);
        return bills;
    }

    // Two different counts, and mixing them up broke --pages within a minute of
    // being written. `billsToProduce` is how many bills this run reads: one per
    // page that was actually extracted. `filePages` is how many the file holds,
    // which is what a bill carries and what lets a sum say it covers two of
    // three. Producing a bill per page of the file instead would ask the model
    // about pages the page limit deliberately skipped, and answer with an error
    // for each of them.
    const int filePages = document.pageCount > 0 ? document.pageCount
                                                 : qMax(1, document.pages.size());
    const int billsToProduce = qMax(1, document.pages.size());

    for (int index = 0; index < billsToProduce; ++index) {
        BillResult bill;
        bill.path = target;
        bill.page = index + 1;
        bill.pageCount = filePages;
        bill.extractMs = document.elapsedMs;
        bill.notes = document.notes;

        QString text;
        QList<QByteArray> images;

        if (index < document.pages.size()) {
            const Extract::DocumentPage &page = document.pages.at(index);
            text = page.text;
            bill.fromTextLayer = page.fromTextLayer;
            bill.rasterShortEdge = page.jpeg.isEmpty() ? 0 : qMin(page.width, page.height);
            bill.qualityWarning = warningFor(page);
            if (!bill.qualityWarning.isEmpty())
                Log::warn("quality", bill.qualityWarning);
            if (!page.jpeg.isEmpty())
                images.append(page.jpeg);
        } else {
            // No per page split was available, for example when a reader only
            // produced a merged text. Send the whole document once.
            text = document.text;
        }

        // What is about to be handed over, which is the question `--verbose`
        // exists to answer. The route decides it, and the route was a decision.
        const QString carried = text.trimmed().isEmpty()
            ? QStringLiteral("no text")
            : QStringLiteral("%1 characters from the %2")
                  .arg(text.size())
                  .arg(bill.fromTextLayer ? QStringLiteral("text layer")
                                          : QStringLiteral("raster or OCR"));
        Log::step("ai", QStringLiteral("page %1 of %2: sending %3")
                            .arg(bill.page)
                            .arg(bill.pageCount)
                            .arg(carried));

        bill.ok = client.analyse(text, images, &bill.invoice, &bill.error);
        bill.inferMs = client.lastInferenceMs();

        // A second answer was needed for the date. Worth saying: the bill took two
        // requests, and the note is the only trace of that in the output.
        if (client.dateWasAskedAgain())
            bill.notes.append(
                QStringLiteral("issue date asked on its own, the extraction had left it out"));

        if (!bill.ok && bill.error.isEmpty())
            bill.error = QStringLiteral("the model returned no usable fields");

        Log::step("bill", QStringLiteral("page %1: %2 in %3 ms")
                              .arg(bill.page)
                              .arg(bill.ok ? bill.invoice.summary() : bill.error)
                              .arg(bill.inferMs));

        bills.append(bill);
    }

    if (cache && !sha256.isEmpty()) {
        const bool saved =
            cache->save(sha256, Store::fingerprint(readOptions, client.options().model), document,
                        bills, client.options().model);
        if (saved) {
            Log::step("store", QStringLiteral("stored %1 bill(s) under %2...")
                                   .arg(bills.size())
                                   .arg(sha256.left(12)));
        } else {
            Log::warn("store", QStringLiteral("not stored: %1").arg(cache->error()));
        }
    }

    return bills;
}

} // namespace InvoiceDrop
