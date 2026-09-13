#include "extract/ocr.h"

#include "log.h"

#include <QtGlobal>

#include <tesseract/baseapi.h>

namespace InvoiceDrop::Extract::Ocr {
namespace {

class Engine
{
public:
    ~Engine()
    {
        if (m_initialized)
            m_api.End();
    }

    /// Initialises on first use and re-initialises whenever the language set or
    /// the page segmentation mode changes.
    bool ensure(const OcrOptions &options)
    {
        if (m_initialized && m_languages == options.languages && m_psm == options.pageSegMode)
            return true;

        if (m_initialized) {
            m_api.End();
            m_initialized = false;
        }

        const QByteArray requested = options.languages.toUtf8();
        // A null datapath makes Tesseract use TESSDATA_PREFIX or the prefix it
        // was compiled with, which is how the distribution packages work.
        if (m_api.Init(nullptr, requested.constData()) != 0)
            return false;

        m_api.SetPageSegMode(static_cast<tesseract::PageSegMode>(options.pageSegMode));

        m_languages = options.languages;
        m_psm = options.pageSegMode;
        m_initialized = true;
        return true;
    }

    tesseract::TessBaseAPI *api() { return &m_api; }

private:
    tesseract::TessBaseAPI m_api;
    QString m_languages;
    int m_psm = -1;
    bool m_initialized = false;
};

Engine &engine()
{
    thread_local Engine instance;
    return instance;
}

} // namespace

bool available(const OcrOptions &options)
{
    if (!options.enabled)
        return false;

    // A language pack that will not load is the silent failure here: OCR simply
    // does not happen, no note is written and the page is read from the image
    // alone. Worth a line of its own for that reason.
    if (!engine().ensure(options)) {
        Log::warn("ocr", QStringLiteral("Tesseract cannot load '%1', reading without OCR")
                             .arg(options.languages));
        return false;
    }
    return true;
}

PIX *prepare(PIX *pix, const OcrOptions &options, QStringList *notes)
{
    if (!pix)
        return nullptr;

    PIX *gray = pixConvertTo8(pix, 0);
    if (!gray)
        return nullptr;

    const int width = pixGetWidth(gray);
    const int height = pixGetHeight(gray);
    const int shortest = qMin(width, height);

    // Tesseract wants a capital letter to be roughly 30 px tall. A phone scan
    // of a receipt that is only 174 px wide has text a few pixels tall, so the
    // image is magnified before anything else happens. This invents no detail,
    // but it does give the recogniser something to work with.
    if (options.targetShortEdge > 0 && shortest > 0 && shortest < options.targetShortEdge) {
        double factor = static_cast<double>(options.targetShortEdge) / shortest;
        factor = qMin(factor, qMax(1.0, options.maxUpscale));
        if (factor > 1.05) {
            PIX *magnified =
                pixScale(gray, static_cast<l_float32>(factor), static_cast<l_float32>(factor));
            if (magnified) {
                pixDestroy(&gray);
                gray = magnified;
                if (notes)
                    Log::note(notes, "ocr", QStringLiteral("OCR: magnified %1x to %2x%3")
                                             .arg(factor, 0, 'f', 1)
                                             .arg(pixGetWidth(gray))
                                             .arg(pixGetHeight(gray)));
            }
        }
    }

    // Adaptive contrast normalisation. This is what rescues photographs of
    // thermal paper, where the print is barely darker than the paper.
    if (options.normalise) {
        PIX *normalised = pixContrastNorm(nullptr, gray, 30, 30, 40, 1, 1);
        if (normalised) {
            pixDestroy(&gray);
            gray = normalised;
            if (notes)
                Log::note(notes, "ocr", QStringLiteral("OCR: contrast normalised"));
        }
    }

    return gray;
}

QString imageToText(PIX *pix, const OcrOptions &options, QStringList *notes, QString *error)
{
    if (!pix) {
        if (error)
            *error = QStringLiteral("no image given");
        return {};
    }

    if (!options.enabled)
        return {};

    const int sourceShortEdge = qMin(pixGetWidth(pix), pixGetHeight(pix));
    if (options.minShortEdge > 0 && sourceShortEdge < options.minShortEdge) {
        if (error)
            *error = QStringLiteral("image is only %1 px across, too small for OCR")
                         .arg(sourceShortEdge);
        return {};
    }

    Engine &instance = engine();
    if (!instance.ensure(options)) {
        if (error)
            *error = QStringLiteral("Tesseract cannot load the language pack '%1'")
                         .arg(options.languages);
        return {};
    }

    PIX *prepared = prepare(pix, options, notes);
    if (!prepared) {
        if (error)
            *error = QStringLiteral("image cannot be prepared for OCR");
        return {};
    }

    const int width = pixGetWidth(prepared);
    const int height = pixGetHeight(prepared);
    // SetImage takes bytes per pixel, not bits: 1 for the 8 bit grey we just
    // built. Leptonica counts a line in 32 bit words, Tesseract in bytes.
    constexpr int kBytesPerPixel = 1;
    const int bytesPerLine = pixGetWpl(prepared) * 4;
    const unsigned char *samples = reinterpret_cast<const unsigned char *>(pixGetData(prepared));

    instance.api()->SetImage(samples, width, height, kBytesPerPixel, bytesPerLine);
    char *recognized = instance.api()->GetUTF8Text();
    instance.api()->Clear();

    QString text;
    if (recognized) {
        text = QString::fromUtf8(recognized).trimmed();
        delete[] recognized; // Tesseract allocates with new[]
    }

    pixDestroy(&prepared);

    // What the recogniser was given and what came back out of it. A page that
    // yields 800 characters here is the case where OCR was worth its seconds;
    // one that yields twelve is the case `--ocr` is off by default for.
    Log::step("ocr", QStringLiteral("%1x%2, psm %3: %4 characters")
                           .arg(width)
                           .arg(height)
                           .arg(options.pageSegMode)
                           .arg(text.size()));

    return text;
}

} // namespace InvoiceDrop::Extract::Ocr
