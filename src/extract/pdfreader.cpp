#include "extract/pdfreader.h"

#include "extract/imageops.h"
#include "extract/ocr.h"
#include "extract/threadctx.h"

#include <algorithm>
#include <cmath>
#include <exception>

#include <QFile>
#include <QStringList>
#include <QVector>

#include <mupdf/fitz.h>

namespace InvoiceDrop::Extract::Pdf {
namespace {

/// MuPDF throws C++ exceptions in a C++ build, so every handle needs a guard
/// rather than a manual drop at the end of a long function.
struct DocumentGuard {
    explicit DocumentGuard(fz_context *context) : ctx(context) {}
    ~DocumentGuard()
    {
        if (handle)
            fz_drop_document(ctx, handle);
    }
    fz_context *ctx;
    fz_document *handle = nullptr;
};

struct PageGuard {
    explicit PageGuard(fz_context *context) : ctx(context) {}
    ~PageGuard()
    {
        if (handle)
            fz_drop_page(ctx, handle);
    }
    fz_context *ctx;
    fz_page *handle = nullptr;
};

struct PixmapGuard {
    explicit PixmapGuard(fz_context *context) : ctx(context) {}
    ~PixmapGuard()
    {
        if (handle)
            fz_drop_pixmap(ctx, handle);
    }
    fz_context *ctx;
    fz_pixmap *handle = nullptr;
};

struct DeviceGuard {
    explicit DeviceGuard(fz_context *context) : ctx(context) {}
    ~DeviceGuard()
    {
        if (handle)
            fz_drop_device(ctx, handle);
    }
    fz_context *ctx;
    fz_device *handle = nullptr;
};

struct StextPageGuard {
    explicit StextPageGuard(fz_context *context) : ctx(context) {}
    ~StextPageGuard()
    {
        if (handle)
            fz_drop_stext_page(ctx, handle);
    }
    fz_context *ctx;
    fz_stext_page *handle = nullptr;
};

struct BufferGuard {
    explicit BufferGuard(fz_context *context) : ctx(context) {}
    ~BufferGuard()
    {
        if (handle)
            fz_drop_buffer(ctx, handle);
    }
    fz_context *ctx;
    fz_buffer *handle = nullptr;
};

/// One line of the text layer, with the box it occupies on the page.
struct TextLine {
    double left = 0;
    double right = 0;
    double centreY = 0;
    double height = 0;
    QString text;
};

/// Collects the text layer as visual lines rather than as content-stream order.
///
/// fz_print_stext_page_as_text emits blocks in the order they were drawn. On a
/// SAP-generated invoice that puts every label of a two-column form before every
/// value, so "Datum:" ends up far away from "20250430" and the model reports no
/// date at all. Grouping lines by their vertical position and sorting by x puts
/// the label back next to its value, which is the only thing that makes a field
/// readable.
QString layoutAwareText(fz_context *ctx, fz_stext_page *page)
{
    Q_UNUSED(ctx);

    QVector<TextLine> lines;

    for (fz_stext_block *block = page->first_block; block; block = block->next) {
        if (block->type != FZ_STEXT_BLOCK_TEXT)
            continue;
        for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
            QString text;
            for (fz_stext_char *character = line->first_char; character; character = character->next)
                text.append(QChar(character->c));
            text = text.trimmed();
            if (text.isEmpty())
                continue;

            TextLine entry;
            entry.left = line->bbox.x0;
            entry.right = line->bbox.x1;
            entry.centreY = (line->bbox.y0 + line->bbox.y1) / 2.0;
            entry.height = qMax(1.0, static_cast<double>(line->bbox.y1 - line->bbox.y0));
            entry.text = text;
            lines.append(entry);
        }
    }

    if (lines.isEmpty())
        return {};

    std::sort(lines.begin(), lines.end(), [](const TextLine &a, const TextLine &b) {
        if (qAbs(a.centreY - b.centreY) > 1.0)
            return a.centreY < b.centreY;
        return a.left < b.left;
    });

    QString result;
    double rowCentre = 0.0;
    double rowHeight = 1.0;
    double lastRight = 0.0;

    for (int index = 0; index < lines.size(); ++index) {
        const TextLine &line = lines.at(index);

        if (index == 0) {
            result += line.text;
            rowCentre = line.centreY;
            rowHeight = line.height;
            lastRight = line.right;
            continue;
        }

        // Lines share a row when their centres are close relative to the text
        // height, so a taller heading does not swallow the next line.
        const double tolerance = 0.6 * qMax(rowHeight, line.height);
        if (qAbs(line.centreY - rowCentre) > tolerance) {
            result += QLatin1Char('\n');
            result += line.text;
            rowCentre = line.centreY;
            rowHeight = line.height;
        } else {
            const double gap = line.left - lastRight;
            // A wide gap is a column boundary, and saying so helps the model
            // more than a lone space does.
            result += gap > 6.0 ? QStringLiteral("    ") : QStringLiteral(" ");
            result += line.text;
            rowHeight = qMax(rowHeight, line.height);
        }

        lastRight = line.right;
    }

    return result.trimmed();
}

struct OutputGuard {
    explicit OutputGuard(fz_context *context) : ctx(context) {}
    ~OutputGuard()
    {
        if (handle)
            fz_drop_output(ctx, handle);
    }
    fz_context *ctx;
    fz_output *handle = nullptr;
};

/// Text of a single page, empty when the page carries no text objects at all.
QString extractPageText(fz_context *ctx, fz_document *document, int index)
{
    PageGuard page(ctx);
    page.handle = fz_load_page(ctx, document, index);
    if (!page.handle)
        return {};

    StextPageGuard stext(ctx);
    stext.handle = fz_new_stext_page_from_page(ctx, page.handle, nullptr);
    if (!stext.handle)
        return {};

    return layoutAwareText(ctx, stext.handle);
}

/// Picks a zoom factor for one page.
///
/// The requested dpi is the baseline, but a narrow page needs more: a thermal
/// receipt 55 mm wide at 200 dpi is 440 px across, which is well below what OCR
/// can read. The short edge is therefore raised to `minShortEdge`. In the other
/// direction a huge sheet is capped by `maxPixels` so a single page cannot
/// exhaust memory.
float chooseZoom(const fz_rect &bounds, const ReadOptions &options, QStringList *notes)
{
    const double widthPoints = static_cast<double>(bounds.x1 - bounds.x0);
    const double heightPoints = static_cast<double>(bounds.y1 - bounds.y0);
    if (widthPoints <= 0.0 || heightPoints <= 0.0)
        return static_cast<float>(options.dpi) / 72.0f;

    double zoom = static_cast<double>(options.dpi) / 72.0;

    const double shortEdgePoints = std::min(widthPoints, heightPoints);
    if (options.minShortEdge > 0) {
        const double wanted = options.minShortEdge / shortEdgePoints;
        if (wanted > zoom) {
            zoom = wanted;
            const QString note = QStringLiteral("resolution raised to %1 dpi for a %2 mm narrow page")
                                     .arg(zoom * 72.0, 0, 'f', 0)
                                     .arg(shortEdgePoints * 25.4 / 72.0, 0, 'f', 0);
            if (notes && !notes->contains(note))
                notes->append(note);
        }
    }

    if (options.maxPixels > 0) {
        const double cap = std::sqrt(static_cast<double>(options.maxPixels)
                                     / (widthPoints * heightPoints));
        if (zoom > cap) {
            zoom = cap;
            const QString note =
                QStringLiteral("resolution limited to %1 dpi by the pixel budget")
                    .arg(zoom * 72.0, 0, 'f', 0);
            if (notes && !notes->contains(note))
                notes->append(note);
        }
    }

    return static_cast<float>(zoom);
}

/// Renders one page to a 32 bit Leptonica PIX at the requested resolution.
/// Returns nullptr and sets `error` on failure.
PIX *renderPage(fz_context *ctx,
                fz_document *document,
                int index,
                const ReadOptions &options,
                QStringList *notes,
                QString *error)
{
    PageGuard page(ctx);
    page.handle = fz_load_page(ctx, document, index);
    if (!page.handle) {
        *error = QStringLiteral("page %1 cannot be loaded").arg(index + 1);
        return nullptr;
    }

    const fz_rect bounds = fz_bound_page(ctx, page.handle);
    const float zoom = chooseZoom(bounds, options, notes);
    const fz_matrix transform = fz_scale(zoom, zoom);
    const fz_irect bbox = fz_round_rect(fz_transform_rect(bounds, transform));

    PixmapGuard pixmap(ctx);
    pixmap.handle = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bbox, nullptr, 0);
    if (!pixmap.handle) {
        *error = QStringLiteral("page %1 bitmap cannot be allocated").arg(index + 1);
        return nullptr;
    }
    // Paper is white; without this the margins come out transparent black.
    fz_clear_pixmap_with_value(ctx, pixmap.handle, 0xff);

    DeviceGuard device(ctx);
    device.handle = fz_new_draw_device(ctx, transform, pixmap.handle);
    if (!device.handle) {
        *error = QStringLiteral("page %1 draw device cannot be created").arg(index + 1);
        return nullptr;
    }
    // The draw device already carries the page-to-pixmap transform, so the page
    // itself must be run through the identity. Passing `transform` here as well
    // applies the zoom twice and clips the right and bottom of the page.
    fz_run_page(ctx, page.handle, device.handle, fz_identity, nullptr);
    fz_close_device(ctx, device.handle);

    PIX *pix = ImageOps::fromRgbSamples(fz_pixmap_samples(ctx, pixmap.handle),
                                        fz_pixmap_width(ctx, pixmap.handle),
                                        fz_pixmap_height(ctx, pixmap.handle),
                                        fz_pixmap_stride(ctx, pixmap.handle));
    if (!pix)
        *error = QStringLiteral("page %1 bitmap cannot be converted").arg(index + 1);
    return pix;
}

/// Turns a rendered page into the raster half of a DocumentPage. OCR text, when
/// it is wanted, is returned separately so the caller can decide its precedence
/// against the text layer.
bool finishPage(PIX *pagePix, int index, const ReadOptions &options, DocumentPage *page,
                QString *ocrText, QStringList *notes)
{
    PIX *rgb = ImageOps::toRgb32(pagePix);
    if (!rgb) {
        notes->append(QStringLiteral("page %1: conversion to RGB failed").arg(index + 1));
        return false;
    }

    PIX *scaled = ImageOps::downscale(rgb, options.longEdge, options.minShortEdge);
    pixDestroy(&rgb);
    if (!scaled) {
        notes->append(QStringLiteral("page %1: downscale failed").arg(index + 1));
        return false;
    }

    bool usable = false;

    if (options.want != Want::Text) {
        QString encodeError;
        const QByteArray jpeg = ImageOps::toJpeg(scaled, options.jpegQuality, &encodeError);
        if (jpeg.isEmpty()) {
            notes->append(QStringLiteral("page %1: %2").arg(index + 1).arg(encodeError));
        } else {
            page->jpeg = jpeg;
            page->width = pixGetWidth(scaled);
            page->height = pixGetHeight(scaled);
            usable = true;
        }
    }

    if (options.want != Want::Images && Ocr::available(options.ocr) && ocrText) {
        QString ocrError;
        const QString recognized = Ocr::imageToText(scaled, options.ocr, notes, &ocrError);
        if (!recognized.isEmpty()) {
            *ocrText = recognized;
            usable = true;
        } else if (!ocrError.isEmpty()) {
            notes->append(QStringLiteral("page %1 OCR: %2").arg(index + 1).arg(ocrError));
        }
    }

    pixDestroy(&scaled);
    return usable;
}

} // namespace

bool read(const QString &path, const ReadOptions &options, Document *document)
{
    fz_context *ctx = ThreadCtx::mupdf();
    if (!ctx) {
        document->error = QStringLiteral("cannot create a MuPDF context");
        return false;
    }

    try {
        const QByteArray nativePath = QFile::encodeName(path);
        DocumentGuard doc(ctx);
        doc.handle = fz_open_document(ctx, nativePath.constData());
        if (!doc.handle) {
            document->error = QStringLiteral("MuPDF cannot open the file");
            return false;
        }

        const int pageCount = fz_count_pages(ctx, doc.handle);
        if (pageCount <= 0) {
            document->error = QStringLiteral("the document reports no pages");
            return false;
        }

        document->pageCount = pageCount;

        const int limit = options.maxPages > 0 ? options.maxPages : pageCount;
        const int pagesToRead = std::clamp(limit, 1, pageCount);
        document->notes.append(
            QStringLiteral("MuPDF: %1 page(s), reading %2").arg(pageCount).arg(pagesToRead));
        if (pagesToRead < pageCount)
            document->notes.append(QStringLiteral("%1 page(s) skipped by the page limit")
                                       .arg(pageCount - pagesToRead));

        QVector<QString> pageTexts(pagesToRead);

        if (options.want != Want::Images) {
            qsizetype total = 0;
            for (int i = 0; i < pagesToRead; ++i) {
                pageTexts[i] = extractPageText(ctx, doc.handle, i);
                total += pageTexts.at(i).size();
            }
            const double average = static_cast<double>(total) / pagesToRead;
            document->hasTextLayer = average >= options.textLayerThreshold;
            document->notes.append(QStringLiteral("text layer: about %1 characters per page")
                                       .arg(average, 0, 'f', 0));
        }

        if (document->hasTextLayer && options.want != Want::Images) {
            for (int i = 0; i < pagesToRead; ++i) {
                DocumentPage page;
                page.index = i;
                page.fromTextLayer = true;
                page.text = pageTexts.at(i);
                document->pages.append(page);
            }
            document->text = pageTexts.join(QStringLiteral("\n\n")).trimmed();
            document->notes.append(QStringLiteral("text layer used, nothing rasterised"));
            return true;
        }

        if (options.want == Want::Text) {
            for (int i = 0; i < pagesToRead; ++i) {
                DocumentPage page;
                page.index = i;
                page.fromTextLayer = true;
                page.text = pageTexts.at(i);
                document->pages.append(page);
            }
            document->text = pageTexts.join(QStringLiteral("\n\n")).trimmed();
            if (document->text.isEmpty())
                document->error = QStringLiteral("no text layer and rasterising was not allowed");
            return !document->text.isEmpty();
        }

        for (int i = 0; i < pagesToRead; ++i) {
            DocumentPage page;
            page.index = i;

            QString renderError;
            PIX *pagePix = renderPage(ctx, doc.handle, i, options, &document->notes, &renderError);
            if (!pagePix) {
                document->notes.append(renderError);
                // Keep the page in the list even when it failed, so the bill
                // numbers the user wrote down still line up with the output.
                page.text = pageTexts.at(i);
                page.fromTextLayer = !page.text.isEmpty();
                document->pages.append(page);
                continue;
            }

            QString ocrText;
            finishPage(pagePix, i, options, &page, &ocrText, &document->notes);
            pixDestroy(&pagePix);

            // OCR wins over a text layer fragment that was too short to be
            // trusted, which is the same precedence the merged text used.
            if (!ocrText.isEmpty()) {
                page.text = ocrText;
                page.fromTextLayer = false;
            } else {
                page.text = pageTexts.at(i);
                page.fromTextLayer = !page.text.isEmpty();
            }

            document->pages.append(page);
        }

        if (document->pages.isEmpty()) {
            document->error = QStringLiteral("no page could be rendered or read");
            return false;
        }

        QStringList collected;
        for (const DocumentPage &page : std::as_const(document->pages)) {
            if (!page.text.trimmed().isEmpty())
                collected.append(page.text.trimmed());
        }
        document->text = collected.join(QStringLiteral("\n\n"));
        if (!document->text.isEmpty())
            document->notes.append(
                QStringLiteral("text recovered by OCR (%1)").arg(options.ocr.languages));
        if (!document->pages.isEmpty())
            document->notes.append(QStringLiteral("%1 page(s) rasterised at %2 dpi, long edge %3")
                                       .arg(document->pages.size())
                                       .arg(options.dpi)
                                       .arg(options.longEdge));

        return true;
    } catch (const std::exception &exception) {
        document->error = QStringLiteral("MuPDF: %1").arg(QString::fromUtf8(exception.what()));
        return false;
    } catch (...) {
        document->error = QStringLiteral("MuPDF raised an unknown error");
        return false;
    }
}

} // namespace InvoiceDrop::Extract::Pdf
