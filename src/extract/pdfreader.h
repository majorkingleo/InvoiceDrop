#pragma once

#include "extract/documentreader.h"

namespace InvoiceDrop::Extract::Pdf {

/// Reads a PDF into `document`. Returns false and fills `document->error` when
/// MuPDF cannot open or render the file.
///
/// Text layer present and `Want::Both` or `Want::Text` -> text only, no raster.
/// No text layer -> the leading pages are rasterised and OCR'd.
bool read(const QString &path, const ReadOptions &options, Document *document);

} // namespace InvoiceDrop::Extract::Pdf
