#pragma once

#include <QString>

namespace InvoiceDrop::Paths {

/// `~/.local/share/invoicedrop`.
///
/// Built from GenericDataLocation plus a fixed name rather than from
/// QStandardPaths::AppLocalDataLocation, which appends the organisation and the
/// application name and produced `…/share/InvoiceDrop/InvoiceDrop`.
QString dataDir();

/// `~/.local/share/invoicedrop/invoicedrop.db`
QString databaseFile();

/// `~/.local/share/invoicedrop/archive`, where `--move` puts processed
/// originals.
QString archiveDir();

} // namespace InvoiceDrop::Paths
