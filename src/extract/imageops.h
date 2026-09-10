#pragma once

#include <QByteArray>
#include <QString>

#include <leptonica/allheaders.h>

namespace InvoiceDrop::Extract::ImageOps {

/// Wraps raw 24 bit RGB samples in a Leptonica PIX. Used for MuPDF output,
/// where the samples arrive as tightly or sparsely packed RGB in `stride`
/// byte rows.
PIX *fromRgbSamples(const unsigned char *samples, int width, int height, int stride);

/// Returns a PIX whose long edge is at most `longEdge`, but never shrinks the
/// short edge below `minShortEdge`. That floor matters for narrow documents: a
/// receipt 1000x3640 px would otherwise be scaled to 440 px wide purely because
/// its long edge is large, and the text becomes unreadable.
///
/// Returns a clone when nothing needs to change, so the caller always owns the
/// result.
PIX *downscale(PIX *source, int longEdge, int minShortEdge);

/// Converts any depth to 32 bit RGB and returns a new PIX.
PIX *toRgb32(PIX *source);

/// Encodes the PIX as JPEG. Returns an empty array and sets `error` on failure.
QByteArray toJpeg(PIX *pix, int quality, QString *error);

} // namespace InvoiceDrop::Extract::ImageOps
