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
///
/// `date` is asked for here and then not believed by default. A schema does not
/// only lose a field when it cannot read one, it fills one in: measured on
/// `tests/testdata/2-2.png`, a Sonnenapotheke receipt that prints 14.07.2025
/// twice, the reply carried `2025-04-17` — a date that is nowhere on the paper —
/// on three runs out of three at temperature 0. So `OllamaClient` takes the date
/// from the labelled text or from the question in `askDate`, and this field is
/// only the last resort.
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
    // Asked for, then not believed by default — see the note above. It stays here to
    // be the last resort, for the document where neither the labelled text nor the
    // question in `askDate` comes back with a date, because an unverified date still
    // beats no date and that is where this field came from before the question
    // existed.
    //
    // Dropping the property instead was measured and gained nothing: on page 6 of
    // the collection in tests/testdata the reply reported the same gross total, 9.58
    // where the receipt says 46.16, with the property and without it. So the schema
    // is left as it was and only the date policy changed.
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
