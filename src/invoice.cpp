#include "invoice.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocale>
#include <QRegularExpression>

namespace InvoiceDrop {
namespace {

/// A model that cannot read a field tends to fill it with an apology rather
/// than leaving it out. Those all mean "nothing here".
const QStringList &placeholderFragments()
{
    static const QStringList fragments = {
        QStringLiteral("n/a"),
        QStringLiteral("na"),
        QStringLiteral("none"),
        QStringLiteral("null"),
        QStringLiteral("nil"),
        QStringLiteral("unknown"),
        QStringLiteral("unbekannt"),
        QStringLiteral("not available"),
        QStringLiteral("nicht verfügbar"),
        QStringLiteral("illegible"),
        QStringLiteral("unleserlich"),
        QStringLiteral("unreadable"),
        QStringLiteral("not visible"),
        QStringLiteral("keine angabe"),
        QStringLiteral("no data"),
    };
    return fragments;
}

QString clean(const QString &raw)
{
    return raw.trimmed();
}

/// Collapses a value the schema declared as "one line" into one line.
///
/// The model happily returns four lines of address plus a parenthetical about
/// where it found the footer. Nobody can display that, so only the first line
/// survives and the rest is dropped rather than wrapped.
QString singleLine(const QString &raw, int limit = 160)
{
    QString text = raw.simplified();
    const int breakAt = text.indexOf(QLatin1Char('\n'));
    if (breakAt >= 0)
        text = text.left(breakAt);
    text = text.trimmed();
    if (text.size() > limit)
        text = text.left(limit).trimmed() + QStringLiteral("...");
    return text;
}

/// Formats an ISO date, rejecting impossible calendar values.
QString buildDate(int year, int month, int day)
{
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return {};
    return QStringLiteral("%1-%2-%3")
        .arg(year, 4, 10, QLatin1Char('0'))
        .arg(month, 2, 10, QLatin1Char('0'))
        .arg(day, 2, 10, QLatin1Char('0'));
}

QString fourDigitYear(int year)
{
    if (year >= 100)
        return QString::number(year);
    // Two digit years on an invoice are this century; the alternative would be
    // turning 25 into 1925.
    return QStringLiteral("20%1").arg(year, 2, 10, QLatin1Char('0'));
}

} // namespace

bool isPlaceholder(const QString &text)
{
    const QString trimmed = clean(text);
    if (trimmed.isEmpty())
        return true;

    // A bare dash or question mark is a non-answer too.
    static const QRegularExpression punctuation(QStringLiteral("^[-–—?.]+$"));
    if (punctuation.match(trimmed).hasMatch())
        return true;

    const QString lowered = trimmed.toLower();
    for (const QString &fragment : placeholderFragments()) {
        if (lowered == fragment)
            return true;
        // "N/A (shop name illegible)" and friends.
        if (lowered.startsWith(fragment + QLatin1Char(' '))
            || lowered.startsWith(fragment + QLatin1Char('('))) {
            return true;
        }
    }
    return false;
}

std::optional<double> parseAmount(const QString &text)
{
    if (isPlaceholder(text))
        return std::nullopt;

    QString digits;
    digits.reserve(text.size());
    for (const QChar character : text) {
        if (character.isDigit() || character == QLatin1Char('.') || character == QLatin1Char(',')
            || character == QLatin1Char('\'')) {
            digits.append(character);
        } else if (character == QLatin1Char('-') && digits.isEmpty()) {
            digits.append(character);
        }
        // Everything else, currency symbols, spaces and letters, is noise. A
        // trailing minus is not a sign, so it is dropped.
    }

    digits.remove(QLatin1Char('\'')); // Swiss thousands separator
    if (digits.isEmpty() || digits == QStringLiteral("-"))
        return std::nullopt;

    const int lastComma = digits.lastIndexOf(QLatin1Char(','));
    const int lastDot = digits.lastIndexOf(QLatin1Char('.'));

    if (lastComma >= 0 && lastDot >= 0) {
        if (lastComma > lastDot) {
            // German: 1.190,00
            digits.remove(QLatin1Char('.'));
            digits.replace(QLatin1Char(','), QLatin1Char('.'));
        } else {
            // Anglo: 1,190.00
            digits.remove(QLatin1Char(','));
        }
    } else if (lastComma >= 0) {
        const int decimals = digits.size() - lastComma - 1;
        if (decimals == 1 || decimals == 2)
            digits.replace(QLatin1Char(','), QLatin1Char('.'));
        else
            digits.remove(QLatin1Char(',')); // 1,190 means one thousand one hundred
    } else if (lastDot >= 0) {
        const int dotCount = digits.count(QLatin1Char('.'));
        if (dotCount > 1) {
            digits.remove(QLatin1Char('.'));
        } else {
            const int decimals = digits.size() - lastDot - 1;
            const QString leading = digits.left(lastDot);
            // "1.190" is a thousands separator, "0.123" and "1190.000" are not.
            if (decimals == 3 && !leading.isEmpty() && leading.size() <= 3
                && leading != QStringLiteral("0")) {
                digits.remove(QLatin1Char('.'));
            }
        }
    }

    bool ok = false;
    const double value = digits.toDouble(&ok);
    if (!ok)
        return std::nullopt;
    return value;
}

QString firstDateIn(const QString &text)
{
    const QString trimmed = clean(text);
    if (trimmed.isEmpty())
        return {};

    // A range like "01.05.2025 - 07.09.2025" is common on receipts; the first
    // date is the one that matters.
    static const QRegularExpression iso(QStringLiteral("(\\d{4})-(\\d{1,2})-(\\d{1,2})"));
    static const QRegularExpression german(
        QStringLiteral("(\\d{1,2})[./](\\d{1,2})[./](\\d{2,4})"));
    static const QRegularExpression compact(QStringLiteral("\\b(\\d{4})(\\d{2})(\\d{2})\\b"));

    QRegularExpressionMatch match = iso.match(trimmed);
    if (match.hasMatch()) {
        return buildDate(match.captured(1).toInt(), match.captured(2).toInt(),
                         match.captured(3).toInt());
    }

    match = german.match(trimmed);
    if (match.hasMatch()) {
        const QString year = fourDigitYear(match.captured(3).toInt());
        return buildDate(year.toInt(), match.captured(2).toInt(), match.captured(1).toInt());
    }

    match = compact.match(trimmed);
    if (match.hasMatch()) {
        return buildDate(match.captured(1).toInt(), match.captured(2).toInt(),
                         match.captured(3).toInt());
    }

    return {};
}

QString normaliseDate(const QString &text)
{
    if (isPlaceholder(text))
        return {};
    return firstDateIn(text);
}

QString findLabelledDate(const QString &text)
{
    static const QStringList labels = {
        QStringLiteral("datum"),        QStringLiteral("date"),
        QStringLiteral("rechnungsdatum"), QStringLiteral("belegdatum"),
        QStringLiteral("ausgestellt"),   QStringLiteral("invoice date"),
        QStringLiteral("dated"),
    };

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString lowered = line.toLower();
        bool labelled = false;
        for (const QString &label : labels) {
            if (lowered.contains(label)) {
                labelled = true;
                break;
            }
        }
        if (!labelled)
            continue;

        const QString found = firstDateIn(line);
        if (!found.isEmpty())
            return found;
    }

    return {};
}

QString normaliseCurrency(const QString &text)
{
    const QString trimmed = clean(text);
    if (isPlaceholder(trimmed))
        return {};

    const QString upper = trimmed.toUpper();
    if (upper.contains(QStringLiteral("€")) || upper.contains(QStringLiteral("EUR"))
        || upper.contains(QStringLiteral("EURO"))) {
        return QStringLiteral("EUR");
    }
    if (upper.contains(QStringLiteral("£")) || upper.contains(QStringLiteral("GBP"))) {
        return QStringLiteral("GBP");
    }
    if (upper.contains(QStringLiteral("CHF"))) {
        return QStringLiteral("CHF");
    }
    if (upper.contains(QLatin1Char('$')) || upper.contains(QStringLiteral("USD"))) {
        return QStringLiteral("USD");
    }

    // Anything the model spelled out itself, e.g. "PLN" or "DKK".
    static const QRegularExpression code(QStringLiteral("\\b([A-Z]{3})\\b"));
    const QRegularExpressionMatch match = code.match(upper);
    if (match.hasMatch())
        return match.captured(1);

    return {};
}

QString extractJsonObject(const QString &text)
{
    QString body = text.trimmed();

    if (body.startsWith(QStringLiteral("```"))) {
        const int firstBreak = body.indexOf(QLatin1Char('\n'));
        if (firstBreak >= 0)
            body = body.mid(firstBreak + 1);
        const int closing = body.lastIndexOf(QStringLiteral("```"));
        if (closing >= 0)
            body = body.left(closing);
    }

    const int start = body.indexOf(QLatin1Char('{'));
    const int end = body.lastIndexOf(QLatin1Char('}'));
    if (start >= 0 && end > start)
        return body.mid(start, end - start + 1);

    return body.trimmed();
}

Invoice Invoice::fromJson(const QJsonObject &object)
{
    const auto text = [&object](const char *key) {
        const QString value = object.value(QString::fromLatin1(key)).toString().trimmed();
        return isPlaceholder(value) ? QString() : value;
    };
    const auto amount = [&object](const char *key) {
        const QJsonValue value = object.value(QString::fromLatin1(key));
        if (value.isDouble())
            return std::optional<double>(value.toDouble());
        if (value.isString())
            return parseAmount(value.toString());
        return std::optional<double>();
    };
    const auto flag = [&object](const char *key) {
        const QJsonValue value = object.value(QString::fromLatin1(key));
        if (value.isDouble())
            return std::optional<double>(value.toDouble());
        if (value.isString())
            return parseAmount(value.toString());
        return std::optional<double>();
    };

    Invoice invoice;
    invoice.vendor = text("vendor");
    invoice.vendorAddress = singleLine(text("vendor_address"));
    invoice.invoiceNumber = text("invoice_number");
    invoice.date = normaliseDate(text("date"));
    invoice.dueDate = normaliseDate(text("due_date"));
    invoice.currency = normaliseCurrency(text("currency"));
    invoice.netTotal = amount("net_total");
    invoice.taxTotal = amount("tax_total");
    invoice.grossTotal = amount("gross_total");
    invoice.iban = text("iban");
    invoice.iban.remove(QLatin1Char(' '));
    invoice.confidence = flag("confidence");
    return invoice;
}

Invoice Invoice::fromModelReply(const QString &reply, QString *error)
{
    const QString body = extractJsonObject(reply);
    if (body.isEmpty()) {
        if (error)
            *error = QStringLiteral("the model returned nothing");
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("the reply is not a JSON object: %1")
                         .arg(parseError.errorString());
        }
        return {};
    }

    return fromJson(document.object());
}

bool Invoice::plausible() const
{
    return !vendor.isEmpty() && grossTotal.has_value();
}

QJsonObject Invoice::toJson() const
{
    QJsonObject object;
    object.insert(QStringLiteral("vendor"), vendor);
    object.insert(QStringLiteral("vendor_address"), vendorAddress);
    object.insert(QStringLiteral("invoice_number"), invoiceNumber);
    object.insert(QStringLiteral("date"), date);
    object.insert(QStringLiteral("due_date"), dueDate);
    object.insert(QStringLiteral("currency"), currency);

    const auto put = [&object](const char *key, const std::optional<double> &value) {
        if (value.has_value())
            object.insert(QString::fromLatin1(key), *value);
        else
            object.insert(QString::fromLatin1(key), QJsonValue::Null);
    };
    put("net_total", netTotal);
    put("tax_total", taxTotal);
    put("gross_total", grossTotal);
    put("confidence", confidence);

    object.insert(QStringLiteral("iban"), iban);
    return object;
}

QString Invoice::summary() const
{
    const QString amount = grossTotal.has_value()
        ? QLocale::system().toString(*grossTotal, 'f', 2) + QLatin1Char(' ') + currency
        : QStringLiteral("?");
    const QString shop = vendor.isEmpty() ? QStringLiteral("?") : vendor;
    const QString when = date.isEmpty() ? QStringLiteral("?") : date;
    return QStringLiteral("%1, %2, %3").arg(shop, when, amount);
}

} // namespace InvoiceDrop
