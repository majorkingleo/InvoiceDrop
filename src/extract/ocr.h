#pragma once

#include <QString>

#include <leptonica/allheaders.h>

namespace InvoiceDrop::Extract::Ocr {

/// True when Tesseract initialised successfully at least once in this process.
bool available();

/// Runs OCR over `pix` and returns the recognised UTF-8 text. `languages` is a
/// Tesseract language string such as "deu+eng".
///
/// One engine is kept per thread and re-initialised only when the requested
/// language set changes, because loading language models is the expensive part.
QString imageToText(PIX *pix, const QString &languages, QString *error);

} // namespace InvoiceDrop::Extract::Ocr
