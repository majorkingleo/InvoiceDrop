#pragma once

#include <QByteArray>
#include <QString>

#include <leptonica/allheaders.h>

namespace InvoiceDrop::Extract::ImageOps {

/// Wraps raw 24 bit RGB samples in a Leptonica PIX. Used for MuPDF output,
/// where the samples arrive as tightly or sparsely packed RGB in `stride`
/// byte rows.
PIX *fromRgbSamples(const unsigned char *samples, int width, int height, int stride);

/// Returns a PIX whose long edge is at most `longEdge`. Returns a clone when the
/// image is already small enough, so the caller always owns the result.
PIX *downscale(PIX *source, int longEdge);

/// Converts any depth to 32 bit RGB and returns a new PIX.
PIX *toRgb32(PIX *source);

/// Encodes the PIX as JPEG. Returns an empty array and sets `error` on failure.
QByteArray toJpeg(PIX *pix, int quality, QString *error);

} // namespace InvoiceDrop::Extract::ImageOps
