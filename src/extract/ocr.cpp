#include "extract/ocr.h"

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

    /// Initialises for `languages` on first use and whenever the set changes.
    bool ensure(const QString &languages)
    {
        if (m_initialized && m_languages == languages)
            return true;

        if (m_initialized) {
            m_api.End();
            m_initialized = false;
        }

        const QByteArray requested = languages.toUtf8();
        // A null datapath makes Tesseract use TESSDATA_PREFIX or the prefix it
        // was compiled with, which is how the distribution packages work.
        if (m_api.Init(nullptr, requested.constData()) != 0)
            return false;

        m_languages = languages;
        m_initialized = true;
        return true;
    }

    tesseract::TessBaseAPI *api() { return &m_api; }

private:
    tesseract::TessBaseAPI m_api;
    QString m_languages;
    bool m_initialized = false;
};

Engine &engine()
{
    thread_local Engine instance;
    return instance;
}

} // namespace

bool available()
{
    return engine().ensure(QStringLiteral("eng"));
}

QString imageToText(PIX *pix, const QString &languages, QString *error)
{
    if (!pix) {
        if (error)
            *error = QStringLiteral("no image given");
        return {};
    }

    Engine &instance = engine();
    if (!instance.ensure(languages)) {
        if (error)
            *error = QStringLiteral("Tesseract cannot load the language pack '%1'").arg(languages);
        return {};
    }

    PIX *gray = pixConvertTo8(pix, 0);
    if (!gray) {
        if (error)
            *error = QStringLiteral("image cannot be converted to 8 bit grey");
        return {};
    }

    const int width = pixGetWidth(gray);
    const int height = pixGetHeight(gray);
    // SetImage takes bytes per pixel, not bits: 1 for the 8 bit grey we just
    // built. Leptonica counts a line in 32 bit words, Tesseract in bytes.
    constexpr int kBytesPerPixel = 1;
    const int bytesPerLine = pixGetWpl(gray) * 4;
    const unsigned char *samples = reinterpret_cast<const unsigned char *>(pixGetData(gray));

    instance.api()->SetImage(samples, width, height, kBytesPerPixel, bytesPerLine);
    char *recognized = instance.api()->GetUTF8Text();
    instance.api()->Clear();

    QString text;
    if (recognized) {
        text = QString::fromUtf8(recognized).trimmed();
        delete[] recognized; // Tesseract allocates with new[]
    }

    pixDestroy(&gray);
    return text;
}

} // namespace InvoiceDrop::Extract::Ocr
