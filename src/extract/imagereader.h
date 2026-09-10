#pragma once

#include "extract/documentreader.h"

#include <leptonica/allheaders.h>

namespace InvoiceDrop::Extract::ImageFile {

/// Decodes a raster image (JPEG, PNG, TIFF, BMP, WebP), applies the EXIF
/// orientation, converts to 32 bit RGB and downscales to the long edge limit.
///
/// The caller owns the returned PIX. Returns nullptr and sets `error` on
/// failure, appending what happened to `notes`.
PIX *load(const QString &path, const ReadOptions &options, QStringList *notes, QString *error);

/// EXIF orientation tag of a JPEG, 1 to 8. Returns 0 when the file has no
/// usable EXIF block. Other formats always return 0.
int exifOrientation(const QByteArray &head);

} // namespace InvoiceDrop::Extract::ImageFile
