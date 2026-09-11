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

/// Turns the path a user typed into an absolute one.
///
/// A relative path is only meaningful next to the directory it was typed in, and
/// that directory is gone the moment the path leaves the process. Handing
/// `tests/testdata/rechnung.pdf` to the daemon over D-Bus fails with "file does
/// not exist", because the daemon was started by the session bus and its working
/// directory is `$HOME`. The caller's directory cannot be recovered on the other
/// side, so it has to be applied on this one.
///
/// Existing paths are canonicalised, which also resolves symlinks and `..`.
/// Non-existent ones are only made absolute, so that the error message names a
/// path the reader can find.
QString resolvePath(const QString &path);

} // namespace InvoiceDrop::Paths
