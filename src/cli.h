#pragma once

#include <QStringList>

namespace InvoiceDrop {

/// Runs the command line interface and returns the process exit code.
///
/// 0 = every file succeeded, 1 = usage error, 2 = at least one file failed.
int runCli(const QStringList &arguments);

} // namespace InvoiceDrop
