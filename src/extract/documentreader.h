#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

namespace InvoiceDrop::Extract {

enum class DocumentKind {
    Unknown,
    Pdf,
    Image,
};

/// One rasterised page, already downscaled and JPEG encoded.
struct PageImage {
    int index = 0;
    QByteArray jpeg;
    int width = 0;
    int height = 0;
};

/// What the caller wants back. `Both` is the default and lets the reader pick
/// the cheapest route that still yields usable content.
enum class Want {
    Text,   ///< text layer only, never rasterise
    Images, ///< always rasterise, skip the text layer
    Both,   ///< text layer when present, otherwise rasterise and OCR
};

struct ReadOptions {
    int dpi = 200;
    int maxPages = 4;
    int longEdge = 1600;
    int jpegQuality = 85;
    QString languages = QStringLiteral("deu+eng");
    Want want = Want::Both;

    /// Average characters per page above which a PDF counts as having a real
    /// text layer.
    int textLayerThreshold = 120;
};

struct Document {
    QString path;
    DocumentKind kind = DocumentKind::Unknown;
    bool hasTextLayer = false;
    QString text;
    QVector<PageImage> pages;
    QStringList notes;
    QString error;
    qint64 elapsedMs = 0;

    bool ok() const { return error.isEmpty(); }
    bool hasText() const { return !text.trimmed().isEmpty(); }
};

/// Reads a document and returns everything the caller asked for. Never throws;
/// failures land in `Document::error`.
Document readDocument(const QString &path, const ReadOptions &options = {});

QString kindName(DocumentKind kind);

/// True for the file suffixes the reader accepts.
bool isSupportedSuffix(const QString &suffix);

} // namespace InvoiceDrop::Extract
