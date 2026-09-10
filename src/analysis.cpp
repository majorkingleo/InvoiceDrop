#include "analysis.h"

#include "hash.h"
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

    // Hashing is a few milliseconds, rasterising and inferring are seconds, so
    // the cache is consulted before the file is opened at all.
    QString sha256;
    if (cache) {
        sha256 = fileSha256(path);
        const QString fingerprint = Store::fingerprint(readOptions, client.options().model);

        if (!sha256.isEmpty()) {
            const std::optional<CachedDocument> cached = cache->find(sha256, fingerprint);
            if (cached.has_value()) {
                for (const StoredBill &stored : cache->bills(sha256))
                    bills.append(fromStored(path, stored, cached->pageCount));
                if (!bills.isEmpty())
                    return bills;
                // A document row without bills is a half written record. Fall
                // through and read the file again rather than report nothing.
            }
        }
    }

    const Extract::Document document = Extract::readDocument(path, readOptions);

    if (!document.ok()) {
        BillResult failed;
        failed.path = path;
        failed.page = 1;
        failed.pageCount = 1;
        failed.error = document.error;
        failed.extractMs = document.elapsedMs;
        failed.notes = document.notes;
        bills.append(failed);
        return bills;
    }

    const int pageCount = qMax(1, document.pages.size());

    for (int index = 0; index < pageCount; ++index) {
        BillResult bill;
        bill.path = path;
        bill.page = index + 1;
        bill.pageCount = pageCount;
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
            if (!page.jpeg.isEmpty())
                images.append(page.jpeg);
        } else {
            // No per page split was available, for example when a reader only
            // produced a merged text. Send the whole document once.
            text = document.text;
        }

        bill.ok = client.analyse(text, images, &bill.invoice, &bill.error);
        bill.inferMs = client.lastInferenceMs();

        if (!bill.ok && bill.error.isEmpty())
            bill.error = QStringLiteral("the model returned no usable fields");

        bills.append(bill);
    }

    if (cache && !sha256.isEmpty())
        cache->save(sha256, Store::fingerprint(readOptions, client.options().model), document,
                    bills, client.options().model);

    return bills;
}

} // namespace InvoiceDrop
