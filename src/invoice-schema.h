#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

namespace InvoiceDrop {

/// The schema handed to the model through Ollama's `format` field.
///
/// A schema constrains decoding, so the model cannot answer with prose or with
/// half a JSON object. Only the fields phase 2 reports are listed: line items
/// multiply the output tokens and the latency, and nothing stores or displays
/// them yet. They come back with the store in phase 3.
inline QJsonObject invoiceSchema()
{
    const auto string = [](const char *description) {
        QJsonObject field;
        field.insert(QStringLiteral("type"), QStringLiteral("string"));
        field.insert(QStringLiteral("description"), QString::fromUtf8(description));
        return field;
    };

    const auto number = [](const char *description) {
        QJsonObject field;
        field.insert(QStringLiteral("type"), QStringLiteral("number"));
        field.insert(QStringLiteral("description"), QString::fromUtf8(description));
        return field;
    };

    QJsonObject properties;
    properties.insert(QStringLiteral("vendor"), string("Name of the company that issued the invoice"));
    properties.insert(QStringLiteral("vendor_address"),
                      string("Postal address of the issuer, one line"));
    properties.insert(QStringLiteral("invoice_number"), string("Invoice or receipt number"));
    properties.insert(QStringLiteral("date"), string("Issue date, ISO 8601, YYYY-MM-DD"));
    properties.insert(QStringLiteral("due_date"), string("Payment due date, ISO 8601, YYYY-MM-DD"));
    properties.insert(QStringLiteral("currency"), string("ISO 4217 code such as EUR"));
    properties.insert(QStringLiteral("net_total"), number("Total excluding tax"));
    properties.insert(QStringLiteral("tax_total"), number("Total tax amount"));
    properties.insert(QStringLiteral("gross_total"), number("Final amount payable including tax"));
    properties.insert(QStringLiteral("iban"), string("IBAN, without spaces"));
    properties.insert(QStringLiteral("confidence"), number("Your confidence in this reading, 0 to 1"));

    QJsonArray required;
    required.append(QStringLiteral("vendor"));
    required.append(QStringLiteral("gross_total"));
    required.append(QStringLiteral("currency"));

    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    schema.insert(QStringLiteral("required"), required);
    return schema;
}

} // namespace InvoiceDrop
