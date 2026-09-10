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

/// Tesseract settings. OCR is a pre-pass for the vision model, so this is where
/// legibility is manufactured: phone scans of receipts are often only 174 px
/// wide, and no recogniser can read text a few pixels tall.
struct OcrOptions {
    /// Off by default, and that is a measured decision, not a shortcut. Across
    /// the real documents in tests/testdata Tesseract produced confident
    /// nonsense on every photographed receipt while the vision model read the
    /// same images correctly, and on the smallest one OCR burned 7.5 s to return
    /// noise. Text that is wrong is worse than no text, because it is fed to the
    /// model as if it were evidence. Clean flatbed scans are the case where OCR
    /// pays off, so `--ocr` turns it back on.
    bool enabled = false;

    QString languages = QStringLiteral("deu+eng");

    /// Short edge the image is scaled to before recognition.
    int targetShortEdge = 1400;

    /// Below this short edge OCR is skipped outright. A receipt photographed at
    /// 174 px across gives roughly 3.6 px per character; magnifying that only
    /// produces confident nonsense and costs seconds. The vision model has shape
    /// priors and does far better, so such a file goes to the model alone.
    int minShortEdge = 300;

    /// Ceiling on magnification, so a thumbnail is not blown up into a blur.
    double maxUpscale = 4.0;

    /// Adaptive contrast normalisation, which is what rescues photos of
    /// thermal paper.
    bool normalise = true;

    /// Tesseract page segmentation mode. 6 is "uniform block of text", which
    /// suits invoices; 4 handles mixed font sizes better.
    int pageSegMode = 6;
};
struct ReadOptions {
    int dpi = 200;
    int maxPages = 4;
    int longEdge = 1600;
    int jpegQuality = 85;

    /// Smallest acceptable short edge in pixels. A narrow thermal receipt at
    /// 200 dpi is only about 440 px wide, which no OCR can read, so the render
    /// resolution is raised and the downscale refuses to go below this.
    int minShortEdge = 1000;

    /// Hard ceiling on rendered pixels per page, so a poster-sized sheet cannot
    /// exhaust memory.
    qint64 maxPixels = 16'000'000;

    OcrOptions ocr;
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
