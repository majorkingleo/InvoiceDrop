#pragma once

#include "analysis.h"
#include "extract/documentreader.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace InvoiceDrop {

/// JSON for one analysed bill. Shared by the CLI, the daemon and the tests, so
/// the shape the plasmoid reads cannot drift from the shape the shell reads.
///
/// The invoice fields sit at the top level, so `jq '.vendor'` works without
/// digging.
QJsonObject billToJson(const BillResult &bill);

/// One object per line, always, whatever the number of bills. An array for
/// several bills and a bare object for one would make `jq '.vendor'` work
/// interactively and fail in a loop.
QString billsToJsonLines(const QVector<BillResult> &bills);

/// JSON for the extraction only view, where no model was consulted.
QJsonObject extractionToJson(const Extract::Document &document, bool withText);

} // namespace InvoiceDrop
