#include "json.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace InvoiceDrop {

QJsonObject billToJson(const BillResult &bill)
{
    QJsonObject object;
    object.insert(QStringLiteral("file"), QFileInfo(bill.path).fileName());
    object.insert(QStringLiteral("path"), bill.path);
    object.insert(QStringLiteral("bill"), bill.page);
    object.insert(QStringLiteral("bill_count"), bill.pageCount);
    object.insert(QStringLiteral("status"), bill.ok ? QStringLiteral("ok")
                                                    : QStringLiteral("error"));
    if (!bill.error.isEmpty())
        object.insert(QStringLiteral("error"), bill.error);
    if (!bill.qualityWarning.isEmpty())
        object.insert(QStringLiteral("quality_warning"), bill.qualityWarning);

    object.insert(QStringLiteral("has_text_layer"), bill.fromTextLayer);
    object.insert(QStringLiteral("from_cache"), bill.fromCache);
    object.insert(QStringLiteral("extract_ms"), bill.extractMs);
    object.insert(QStringLiteral("inference_ms"), bill.inferMs);

    QJsonArray notes;
    for (const QString &note : bill.notes)
        notes.append(note);
    object.insert(QStringLiteral("notes"), notes);

    const QJsonObject fields = bill.invoice.toJson();
    for (auto entry = fields.constBegin(); entry != fields.constEnd(); ++entry)
        object.insert(entry.key(), entry.value());

    return object;
}

QString billsToJsonLines(const QVector<BillResult> &bills)
{
    QString out;
    for (const BillResult &bill : bills) {
        const QJsonDocument document(billToJson(bill));
        out += QString::fromUtf8(document.toJson(QJsonDocument::Compact));
        out += QLatin1Char('\n');
    }
    return out;
}

QJsonObject extractionToJson(const Extract::Document &document, bool withText)
{
    QJsonObject object;
    object.insert(QStringLiteral("file"), QFileInfo(document.path).fileName());
    object.insert(QStringLiteral("path"), document.path);
    object.insert(QStringLiteral("kind"), Extract::kindName(document.kind));
    object.insert(QStringLiteral("status"),
                  document.ok() ? QStringLiteral("ok") : QStringLiteral("error"));
    if (!document.error.isEmpty())
        object.insert(QStringLiteral("error"), document.error);

    object.insert(QStringLiteral("has_text_layer"), document.hasTextLayer);
    object.insert(QStringLiteral("bill_count"), document.pages.size());
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

} // namespace InvoiceDrop
