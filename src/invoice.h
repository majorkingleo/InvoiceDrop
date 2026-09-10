#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

namespace InvoiceDrop {

/// The fields InvoiceDrop reports.
///
/// Everything is optional. A model that cannot read a field must be able to say
/// nothing rather than invent a number, so a missing value stays missing all the
/// way to the UI instead of silently becoming zero.
struct Invoice {
    QString vendor;
    QString vendorAddress;
    QString invoiceNumber;
    QString date;    ///< ISO 8601, YYYY-MM-DD
    QString dueDate; ///< ISO 8601, YYYY-MM-DD
    QString currency;
    std::optional<double> netTotal;
    std::optional<double> taxTotal;
    std::optional<double> grossTotal;
    QString iban;
    std::optional<double> confidence;

    /// True when the fields phase 2 reports are present. Used to decide whether
    /// a reply is worth keeping or worth a second attempt.
    bool plausible() const;

    /// Flat object, matching the documented `--json` contract.
    QJsonObject toJson() const;

    /// One human readable line, for notifications and logs.
    QString summary() const;

    /// Parses a raw model reply. Handles a markdown fence, surrounding prose
    /// and German number and date formats. Sets `error` when the text is not
    /// JSON at all.
    static Invoice fromModelReply(const QString &reply, QString *error = nullptr);

    static Invoice fromJson(const QJsonObject &object);
};

/// Normalisation helpers. Exposed because they carry all the format knowledge
/// and are the most valuable thing in this file to test.

/// `1190,00`, `1.190,00`, `1190.00`, `€ 1.190,00` all become 1190.
std::optional<double> parseAmount(const QString &text);

/// `01.05.2025`, `2025-05-07`, `17.07.2025 11:37:45`, `2025-05-07T11:37:45`
/// all become `2025-05-01`, `2025-05-07`, `2025-07-17`, `2025-05-07`.
QString normaliseDate(const QString &text);

/// First parseable date anywhere in `text`, or an empty string.
QString firstDateIn(const QString &text);

/// Scans the extracted text for a date that sits on a line with a date label.
///
/// Measured: given the same text and `temperature: 0`, the model returns the
/// issue date in some runs and omits it in others. A date is a regular
/// expression, not a judgement call, so this runs whenever the model stays
/// silent. It insists on a label, because the earliest date on a document is
/// often a service period rather than the issue date.
QString findLabelledDate(const QString &text);

/// `€`, `eur`, `Euro` become `EUR`.
QString normaliseCurrency(const QString &text);

/// Removes a ```json fence and any prose around the outermost object.
QString extractJsonObject(const QString &text);

/// True for the answers a model gives when it cannot read something: an empty
/// string, `-`, `n/a`, `unknown`, `N/A (shop name illegible)` and friends.
bool isPlaceholder(const QString &text);

} // namespace InvoiceDrop
