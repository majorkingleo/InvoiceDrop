#pragma once

#include "extract/documentreader.h"

#include <QString>
#include <QStringList>

#include <leptonica/allheaders.h>

namespace InvoiceDrop::Extract::Ocr {

/// True when OCR is switched on and Tesseract accepts the language set.
bool available(const OcrOptions &options);

/// Prepares an image for recognition: magnifies it if the text is too small to
/// read, then normalises contrast. Returns a new 8 bit grey PIX, or nullptr.
/// Exposed so the effect of the preparation can be inspected and tested on its
/// own.
PIX *prepare(PIX *pix, const OcrOptions &options, QStringList *notes);

/// Runs OCR over `pix` and returns the recognised UTF-8 text.
///
/// One engine is kept per thread and re-initialised only when the requested
/// settings change, because loading language models is the expensive part.
QString imageToText(PIX *pix, const OcrOptions &options, QStringList *notes, QString *error);

} // namespace InvoiceDrop::Extract::Ocr
