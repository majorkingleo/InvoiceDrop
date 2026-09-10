#include "extract/imageops.h"

#include <QtGlobal>

namespace InvoiceDrop::Extract::ImageOps {
namespace {

// Leptonica stores a 32 bit pixel as a word with red in the most significant
// byte, green next, blue next, and a fourth byte that is ignored for depth 32.
// Keeping the shifts here matches the library's own layout on every endianness,
// because the value is composed as a word, never as raw memory.
constexpr unsigned int kRedShift = 24;
constexpr unsigned int kGreenShift = 16;
constexpr unsigned int kBlueShift = 8;
constexpr unsigned int kOpaque = 0xffu;

} // namespace

PIX *fromRgbSamples(const unsigned char *samples, int width, int height, int stride)
{
    if (!samples || width <= 0 || height <= 0)
        return nullptr;

    PIX *pix = pixCreateNoInit(width, height, 32);
    if (!pix)
        return nullptr;

    l_uint32 *dstData = pixGetData(pix);
    const int wordsPerLine = pixGetWpl(pix);

    for (int y = 0; y < height; ++y) {
        const unsigned char *src = samples + static_cast<qsizetype>(y) * stride;
        l_uint32 *row = dstData + static_cast<qsizetype>(y) * wordsPerLine;
        for (int x = 0; x < width; ++x) {
            const unsigned int r = src[0];
            const unsigned int g = src[1];
            const unsigned int b = src[2];
            src += 3;
            row[x] = (r << kRedShift) | (g << kGreenShift) | (b << kBlueShift) | kOpaque;
        }
    }

    return pix;
}

PIX *downscale(PIX *source, int longEdge)
{
    if (!source)
        return nullptr;

    const int width = pixGetWidth(source);
    const int height = pixGetHeight(source);
    const int longest = qMax(width, height);

    if (longEdge <= 0 || longest <= longEdge)
        return pixClone(source);

    const double factor = static_cast<double>(longEdge) / longest;
    const int targetWidth = qMax(1, static_cast<int>(width * factor + 0.5));
    const int targetHeight = qMax(1, static_cast<int>(height * factor + 0.5));

    PIX *scaled = pixScaleToSize(source, targetWidth, targetHeight);
    return scaled ? scaled : pixClone(source);
}

PIX *toRgb32(PIX *source)
{
    if (!source)
        return nullptr;
    if (pixGetDepth(source) == 32)
        return pixClone(source);
    return pixConvertTo32(source);
}

QByteArray toJpeg(PIX *pix, int quality, QString *error)
{
    if (!pix) {
        if (error)
            *error = QStringLiteral("no image to encode");
        return {};
    }

    l_uint8 *data = nullptr;
    size_t size = 0;
    const l_int32 status = pixWriteMemJpeg(&data, &size, pix, quality, 0);
    if (status != 0 || !data || size == 0) {
        if (error)
            *error = QStringLiteral("JPEG encoding failed");
        if (data)
            lept_free(data);
        return {};
    }

    const QByteArray encoded(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    lept_free(data);
    return encoded;
}

} // namespace InvoiceDrop::Extract::ImageOps
