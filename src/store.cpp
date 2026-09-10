#include "store.h"

#include "paths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace InvoiceDrop {
namespace {

constexpr auto kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS documents (
    sha256         TEXT PRIMARY KEY,
    fingerprint    TEXT NOT NULL,
    source_path    TEXT NOT NULL,
    file_name      TEXT NOT NULL,
    page_count     INTEGER NOT NULL,
    has_text_layer INTEGER NOT NULL DEFAULT 0,
    model          TEXT NOT NULL,
    created_at     TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS bills (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    sha256          TEXT NOT NULL,
    page            INTEGER NOT NULL,
    status          TEXT NOT NULL,
    vendor          TEXT,
    vendor_address  TEXT,
    invoice_number  TEXT,
    bill_date       TEXT,
    due_date        TEXT,
    currency        TEXT,
    net_total       REAL,
    tax_total       REAL,
    gross_total     REAL,
    iban            TEXT,
    confidence      REAL,
    quality_warning TEXT,
    payload         TEXT NOT NULL,
    created_at      TEXT NOT NULL,
    UNIQUE(sha256, page),
    FOREIGN KEY(sha256) REFERENCES documents(sha256) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_bills_vendor ON bills(vendor);
CREATE INDEX IF NOT EXISTS idx_bills_date   ON bills(bill_date);
CREATE INDEX IF NOT EXISTS idx_bills_recent ON bills(created_at);
)SQL";

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

/// Inserts a nullable double as either a value or SQL NULL, so a missing amount
/// never turns into a zero.
QVariant nullable(const std::optional<double> &value)
{
    return value.has_value() ? QVariant(*value) : QVariant(QMetaType(QMetaType::Double));
}

StoredBill toStoredBill(const QSqlQuery &query)
{
    StoredBill bill;
    bill.fileName = query.value(QStringLiteral("file_name")).toString();
    bill.sourcePath = query.value(QStringLiteral("source_path")).toString();
    bill.page = query.value(QStringLiteral("page")).toInt();
    bill.status = query.value(QStringLiteral("status")).toString();
    bill.vendor = query.value(QStringLiteral("vendor")).toString();
    bill.date = query.value(QStringLiteral("bill_date")).toString();
    bill.currency = query.value(QStringLiteral("currency")).toString();
    bill.model = query.value(QStringLiteral("model")).toString();
    bill.createdAt = query.value(QStringLiteral("created_at")).toString();
    bill.payload = query.value(QStringLiteral("payload")).toString();

    const QVariant total = query.value(QStringLiteral("gross_total"));
    if (!total.isNull())
        bill.grossTotal = total.toDouble();
    return bill;
}

} // namespace

Store::Store(const QString &databasePath)
    : m_path(databasePath),
      m_connection(QStringLiteral("invoicedrop-%1").arg(reinterpret_cast<quintptr>(this), 0, 16))
{
    const QFileInfo info(m_path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        m_error = QStringLiteral("cannot create %1").arg(info.absolutePath());
        return;
    }

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    database.setDatabaseName(m_path);
    if (!database.open()) {
        m_error = database.lastError().text();
        QSqlDatabase::removeDatabase(m_connection);
        return;
    }

    m_open = createSchema();
    if (!m_open && m_error.isEmpty())
        m_error = QStringLiteral("cannot create the database schema");
}

Store::~Store()
{
    if (QSqlDatabase::contains(m_connection)) {
        {
            QSqlDatabase database = QSqlDatabase::database(m_connection, false);
            if (database.isOpen())
                database.close();
        }
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool Store::createSchema()
{
    QSqlDatabase database = QSqlDatabase::database(m_connection);
    QSqlQuery query(database);

    for (const QString &statement : QString::fromUtf8(kSchema).split(QStringLiteral(";"),
                                                                     Qt::SkipEmptyParts)) {
        if (statement.trimmed().isEmpty())
            continue;
        if (!query.exec(statement)) {
            m_error = query.lastError().text();
            return false;
        }
    }

    query.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    return true;
}

QString Store::defaultPath()
{
    return Paths::databaseFile();
}

QString Store::fingerprint(const Extract::ReadOptions &options, const QString &model)
{
    const QString key = QStringLiteral(
                            "model=%1;pages=%2;dpi=%3;longEdge=%4;minShortEdge=%5;"
                            "threshold=%6;ocr=%7;ocrLang=%8")
                            .arg(model)
                            .arg(options.maxPages)
                            .arg(options.dpi)
                            .arg(options.longEdge)
                            .arg(options.minShortEdge)
                            .arg(options.textLayerThreshold)
                            .arg(options.ocr.enabled ? 1 : 0)
                            .arg(options.ocr.languages);

    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

std::optional<CachedDocument> Store::find(const QString &sha256, const QString &fingerprint) const
{
    if (!m_open || sha256.isEmpty())
        return std::nullopt;

    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral(
        "SELECT page_count, created_at FROM documents WHERE sha256 = ? AND fingerprint = ?"));
    query.addBindValue(sha256);
    query.addBindValue(fingerprint);

    if (!query.exec() || !query.next())
        return std::nullopt;

    CachedDocument document;
    document.pageCount = query.value(0).toInt();
    document.createdAt = query.value(1).toString();
    return document;
}

QVector<StoredBill> Store::bills(const QString &sha256) const
{
    QVector<StoredBill> result;
    if (!m_open || sha256.isEmpty())
        return result;

    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral(
        "SELECT b.*, d.file_name, d.source_path, d.model FROM bills b "
        "JOIN documents d ON d.sha256 = b.sha256 "
        "WHERE b.sha256 = ? ORDER BY b.page"));
    query.addBindValue(sha256);

    if (!query.exec())
        return result;

    while (query.next())
        result.append(toStoredBill(query));
    return result;
}

bool Store::save(const QString &sha256,
                 const QString &fingerprint,
                 const Extract::Document &document,
                 const QVector<BillResult> &bills,
                 const QString &model)
{
    if (!m_open)
        return false;

    QSqlDatabase database = QSqlDatabase::database(m_connection);
    if (!database.transaction()) {
        m_error = database.lastError().text();
        return false;
    }

    const QString createdAt = nowIso();
    const QString fileName = QFileInfo(document.path).fileName();

    QSqlQuery insertDocument(database);
    insertDocument.prepare(QStringLiteral(
        "INSERT INTO documents (sha256, fingerprint, source_path, file_name, page_count, "
        "has_text_layer, model, created_at) VALUES (?,?,?,?,?,?,?,?) "
        "ON CONFLICT(sha256) DO UPDATE SET fingerprint=excluded.fingerprint, "
        "source_path=excluded.source_path, file_name=excluded.file_name, "
        "page_count=excluded.page_count, has_text_layer=excluded.has_text_layer, "
        "model=excluded.model, created_at=excluded.created_at"));
    insertDocument.addBindValue(sha256);
    insertDocument.addBindValue(fingerprint);
    insertDocument.addBindValue(document.path);
    insertDocument.addBindValue(fileName);
    insertDocument.addBindValue(bills.size());
    insertDocument.addBindValue(document.hasTextLayer ? 1 : 0);
    insertDocument.addBindValue(model);
    insertDocument.addBindValue(createdAt);

    if (!insertDocument.exec()) {
        m_error = insertDocument.lastError().text();
        database.rollback();
        return false;
    }

    QSqlQuery insertBill(database);
    insertBill.prepare(QStringLiteral(
        "INSERT INTO bills (sha256, page, status, vendor, vendor_address, invoice_number, "
        "bill_date, due_date, currency, net_total, tax_total, gross_total, iban, confidence, "
        "quality_warning, payload, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(sha256, page) DO UPDATE SET status=excluded.status, vendor=excluded.vendor, "
        "vendor_address=excluded.vendor_address, invoice_number=excluded.invoice_number, "
        "bill_date=excluded.bill_date, due_date=excluded.due_date, currency=excluded.currency, "
        "net_total=excluded.net_total, tax_total=excluded.tax_total, "
        "gross_total=excluded.gross_total, iban=excluded.iban, confidence=excluded.confidence, "
        "quality_warning=excluded.quality_warning, payload=excluded.payload, "
        "created_at=excluded.created_at"));

    for (const BillResult &bill : bills) {
        const Invoice &invoice = bill.invoice;

        insertBill.addBindValue(sha256);
        insertBill.addBindValue(bill.page);
        insertBill.addBindValue(bill.ok ? QStringLiteral("ok") : QStringLiteral("error"));
        insertBill.addBindValue(invoice.vendor);
        insertBill.addBindValue(invoice.vendorAddress);
        insertBill.addBindValue(invoice.invoiceNumber);
        insertBill.addBindValue(invoice.date);
        insertBill.addBindValue(invoice.dueDate);
        insertBill.addBindValue(invoice.currency);
        insertBill.addBindValue(nullable(invoice.netTotal));
        insertBill.addBindValue(nullable(invoice.taxTotal));
        insertBill.addBindValue(nullable(invoice.grossTotal));
        insertBill.addBindValue(invoice.iban);
        insertBill.addBindValue(nullable(invoice.confidence));
        insertBill.addBindValue(bill.qualityWarning);
        insertBill.addBindValue(
            QString::fromUtf8(QJsonDocument(invoice.toJson()).toJson(QJsonDocument::Compact)));
        insertBill.addBindValue(createdAt);

        if (!insertBill.exec()) {
            m_error = insertBill.lastError().text();
            database.rollback();
            return false;
        }
    }

    if (!database.commit()) {
        m_error = database.lastError().text();
        database.rollback();
        return false;
    }

    return true;
}

QVector<StoredBill> Store::recent(int limit) const
{
    QVector<StoredBill> result;
    if (!m_open || limit <= 0)
        return result;

    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(QStringLiteral(
        "SELECT b.*, d.file_name, d.source_path, d.model FROM bills b "
        "JOIN documents d ON d.sha256 = b.sha256 "
        "ORDER BY b.created_at DESC, b.id DESC LIMIT ?"));
    query.addBindValue(limit);

    if (!query.exec())
        return result;

    while (query.next())
        result.append(toStoredBill(query));
    return result;
}

int Store::billCount() const
{
    if (!m_open)
        return 0;

    QSqlQuery query(QSqlDatabase::database(m_connection));
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM bills")) || !query.next())
        return 0;
    return query.value(0).toInt();
}

bool Store::forget(const QString &sha256)
{
    if (!m_open || sha256.isEmpty())
        return false;

    QSqlDatabase database = QSqlDatabase::database(m_connection);
    QSqlQuery query(database);
    query.prepare(QStringLiteral("DELETE FROM documents WHERE sha256 = ?"));
    query.addBindValue(sha256);
    return query.exec();
}

QVector<QString> Store::documents() const
{
    QVector<QString> result;
    if (!m_open)
        return result;

    QSqlQuery query(QSqlDatabase::database(m_connection));
    if (!query.exec(QStringLiteral("SELECT sha256 FROM documents ORDER BY created_at DESC")))
        return result;
    while (query.next())
        result.append(query.value(0).toString());
    return result;
}

} // namespace InvoiceDrop
