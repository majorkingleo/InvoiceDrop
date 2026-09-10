#pragma once

#include "analysis.h"
#include "extract/documentreader.h"

#include <QString>
#include <QVector>
#include <optional>

namespace InvoiceDrop {

/// One stored bill, as `history` prints it.
struct StoredBill {
    QString fileName;
    QString sourcePath;
    int page = 0;
    QString status;
    QString vendor;
    QString date;
    std::optional<double> grossTotal;
    QString currency;
    QString model;
    QString createdAt;

    /// The full invoice as stored, so a cached run rebuilds it instead of
    /// re-reading the document.
    QString payload;
};

/// A document whose pages are already in the database.
struct CachedDocument {
    int pageCount = 0;
    QString createdAt;
};

/// SQLite persistence for analysed bills.
///
/// Bills are keyed on the hash of the source file plus the page number, because
/// one file can hold twenty of them. The cache is only honoured when the
/// fingerprint matches as well: reading the same file with a different model or
/// a different page limit is a different question.
class Store
{
public:
    explicit Store(const QString &databasePath);
    ~Store();

    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    bool isOpen() const { return m_open; }
    QString error() const { return m_error; }
    QString databasePath() const { return m_path; }

    /// Default location, `~/.local/share/invoicedrop/invoicedrop.db`.
    static QString defaultPath();

    /// Covers everything that changes what a document yields. Two runs share a
    /// cache entry only when all of it matches.
    static QString fingerprint(const Extract::ReadOptions &options, const QString &model);

    std::optional<CachedDocument> find(const QString &sha256, const QString &fingerprint) const;
    QVector<StoredBill> bills(const QString &sha256) const;

    bool save(const QString &sha256,
              const QString &fingerprint,
              const Extract::Document &document,
              const QVector<BillResult> &bills,
              const QString &model);

    QVector<StoredBill> recent(int limit) const;
    int billCount() const;

    /// Drops one document and its bills, so the next run reads it again.
    bool forget(const QString &sha256);

    /// Every document hash in the store, newest first.
    QVector<QString> documents() const;

private:
    bool createSchema();

    QString m_path;
    QString m_connection;
    QString m_error;
    bool m_open = false;
};

} // namespace InvoiceDrop
