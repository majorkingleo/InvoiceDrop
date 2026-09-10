#pragma once

#include "extract/documentreader.h"
#include "invoice.h"
#include "ollama.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace InvoiceDrop {

/// One bill: the result of analysing one page.
///
/// The unit of output is the page, not the file, because a file is not a bill.
/// A two page scan holds two receipts and a collection PDF holds twenty, so
/// analysing the file as a whole would silently merge them.
struct BillResult {
    QString path;
    int page = 0;      ///< one based, in paper order: the bill number
    int pageCount = 0; ///< pages read from this file

    bool ok = false;
    QString error;

    /// Set when the raster is too coarse to carry the text the model claims to
    /// have read. The numbers are then unverified rather than absent.
    QString qualityWarning;

    Invoice invoice;

    bool fromTextLayer = false;
    int rasterShortEdge = 0;
    qint64 extractMs = 0;
    qint64 inferMs = 0;
    QStringList notes;

    /// `file.pdf` for a single page file, `file.pdf:2` for the second bill of a
    /// multi page one.
    QString label() const;
};

/// Reads a document and analyses every page as its own bill.
///
/// The CLI, the tests and later the daemon all call this, so the pipeline exists
/// in exactly one place and cannot drift between them.
QVector<BillResult> analyseFile(const QString &path,
                                const Extract::ReadOptions &readOptions,
                                OllamaClient &client);

} // namespace InvoiceDrop
