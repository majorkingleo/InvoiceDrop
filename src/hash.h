#pragma once

#include <QString>

namespace InvoiceDrop {

/// SHA-256 of a file, read in chunks so a 50 MB scan does not land in memory.
/// Returns an empty string when the file cannot be read.
QString fileSha256(const QString &path);

} // namespace InvoiceDrop
