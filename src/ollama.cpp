#include "ollama.h"

#include "invoice-schema.h"
#include "log.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

namespace InvoiceDrop {
namespace {

const char *kSystemPrompt = R"(You read business documents and return structured data.
Rules:
- Read the attached images and the extracted text. The images are the source of truth;
  the extracted text is a hint and may be corrupted.
- A photographed receipt is narrow and long. Read it line by line, do not stop early.
- On a receipt, the total is the final amount actually paid, not a subtotal, not a
  promotional line and not a running balance. The total sits on the line the shop labels
  SUMME, Gesamt, Total or Zu zahlen, and the payment line below it carries the same amount.
  A savings note — "Ihre Ersparnis", "Sie sparen", "you saved" — is printed between those
  two lines with a smaller amount, and it is never the total.
- Use a dot as the decimal separator and no thousands separator.
- Give dates as ISO 8601, YYYY-MM-DD.
- A machine printed bill is read from its printed text. Disregard anything written on it by
  hand — a name, a note, a tick, a sum — even where it overlaps the printing. Handwriting is
  never the vendor, a date or an amount. A bill that is handwritten itself is read as written.
- If you cannot read a value, leave the field out. Never guess and never invent a number.
- Output the JSON object only, with no surrounding text.
)";

QString userPrompt(const QString &language)
{
    return QStringLiteral(
               "Extract the invoice fields. If the document is a receipt, the vendor is the "
               "shop that issued it. Write free text fields in \"%1\".")
        .arg(language);
}

/// The question that reads the issue date.
///
/// This is where the issue date comes from. The extraction schema still asks for
/// one and the reply still fills it in, but it is believed only when this question
/// finds nothing, so that a reply that invents a date cannot become the answer by
/// simply not being empty. `invoice-schema.h` has the measurement. Only a labelled
/// date in the text layer gets there first, so on a photograph this is the
/// ordinary route rather than a rescue.
///
/// It is deliberately the whole instruction, in a request that carries no system
/// message and no schema. `think` is on for this one request: the answer has to
/// choose between several dates on the paper, and without a reasoning trace the
/// model does not choose. Measured 2026-09-20 on the collection in
/// `tests/testdata`: with a system prompt, in three wordings, the model answered
/// the start of an offer period — `Vom 01.05.2025 - 07.09.2025` on a leaflet next
/// to the receipt — and a two line system prompt was no different. Without any
/// system message, this wording answered the transaction date on 20 of the 21
/// pages in one pass, and the one miss was answered right on the next three runs.
/// The wording is the one the measurement used, unmodified.
const char *kDateQuestion = 
    "give me the date when this bill was produced in iso date format. "
    "If you find a transaction date, use this one";

/// The prompt for the question that reads the gross total.
///
/// This exists for the reason the date question does: the extraction reply does
/// not leave the field empty when it cannot read it, it puts a number in it, and
/// on a receipt with a savings note the number it puts there is the note. Measured
/// on the three receipts of `GuSp_SoLa2025_Lebensmittel_Rechnungen.pdf` that print
/// `Ihre Ersparnis` under `SUMME` — 46.16 with 9.58 saved, 12.40 with 3.36, 48.85
/// with 4.47 — the reply answered the saved amount on all three.
///
/// Two other wordings were tried and thrown away. "The final amount payable"
/// answered 9.58 on a page whose reply had been right, and the payment line — which
/// looks like the same amount — is not: page 11 of that document carries a
/// `Bargeldauszahlung EUR 200,00`, so its `Mastercard EUR 262,62` is 200 more than
/// the bill, and page 18 pays 6,98 in cash and prints a rounded 7. The line the
/// shop labels as its total is the one thing on the paper that always means the
/// total. That wording answered 46,16 / 12,40 / 62,62 / 6,98 / 55,30 / 48,85 on
/// three runs out of three each, across the pages that the reply and the other two
/// wordings each get wrong. Measurement in `docs/architecture.md`.
const char *kTotalPrompt = R"(You read the total printed on a business document.
- The total is the amount on the line the shop labels SUMME, Zw-Summe, Gesamtsumme, Gesamt,
  Total or Zu zahlen. On a receipt that line comes at the end of the list of items.
- If the document shows no such line, answer NONE.
- Answer with one amount and nothing else.
)";

/// The follow-up question for the total: it names the printed label rather than the
/// field, which is what makes it answerable. The answer is an amount or NONE, and
/// `amountFromAnswer` is what accepts or rejects it. NONE means the reply's own
/// total stands, so a document without such a line is not made worse.
const char *kTotalQuestion = "Which amount is printed on the line labelled \"SUMME\"? Answer "
                             "with the amount and nothing else.";

/// Fits a duration into the sentence the user reads.
QString seconds(qint64 milliseconds)
{
    return QString::number(milliseconds / 1000.0, 'f', 1) + QStringLiteral(" s");
}

/// Writes what is about to be sent, read out of the request object itself rather
/// than out of the arguments. A change to the builder then turns up in the log
/// without a second place having to be remembered.
void logRequest(const OllamaOptions &options,
                const QJsonObject &request,
                const QByteArray &payload,
                const QList<QByteArray> &jpegPages,
                int attempt)
{
    QString system;
    QString user;
    const QJsonArray messages = request.value(QStringLiteral("messages")).toArray();
    for (const QJsonValue &entry : messages) {
        const QJsonObject message = entry.toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        const QString content = message.value(QStringLiteral("content")).toString();
        if (role == QLatin1String("system"))
            system = content;
        else if (role == QLatin1String("user"))
            user = content;
    }

    qsizetype imageBytes = 0;
    for (const QByteArray &page : jpegPages)
        imageBytes += page.size();

    const int schemaFields =
        request.value(QStringLiteral("format")).toObject().value(QStringLiteral("properties"))
            .toObject()
            .size();

    Log::step("ai", QStringLiteral("model %1, language %2, think %3, stream off, temperature 0, "
                                    "keep_alive %4, format: schema with %5 properties")
                           .arg(options.model)
                           .arg(options.language)
                           .arg(options.think ? QStringLiteral("on") : QStringLiteral("off"))
                           .arg(options.keepAlive)
                           .arg(schemaFields));
    Log::step("ai", QStringLiteral("payload: %1 characters of prompt, %2 image(s), %3 KB of "
                                    "jpeg before base64")
                           .arg(user.size())
                           .arg(jpegPages.size())
                           .arg(imageBytes / 1024));
    Log::model("ai", QStringLiteral("attempt %1: %2 KB to POST %3/api/chat, timeout %4 s")
                          .arg(attempt + 1)
                          .arg(payload.size() / 1024)
                          .arg(options.url)
                          .arg(options.timeoutMs / 1000));

    // The two halves of the prompt, in full. This is the answer to "what did the
    // model actually get", and there is no shorter one: the text layer goes in
    // verbatim, and so does the complaint on a second attempt.
    Log::block("ai", QStringLiteral("system prompt, %1 characters").arg(system.size()), system);
    Log::block("ai", QStringLiteral("user prompt, %1 characters").arg(user.size()), user);
}

/// Reads the answer to the total question.
///
/// The answer has to be one amount, and nothing else is accepted. `parseAmount`
/// keeps every digit it is handed, so a sentence naming two amounts —
/// `SUMME 12,40, Ersparnis 3,36` — would be glued into `12,403,36` and read as a
/// third one. Two numbers in the answer therefore mean "not an answer", and the
/// total the reply carried stands instead.
std::optional<double> amountFromAnswer(const QString &answer)
{
    static const QRegularExpression number(QStringLiteral("[0-9][0-9.,']*"));

    QStringList found;
    QRegularExpressionMatchIterator matches = number.globalMatch(answer);
    while (matches.hasNext())
        found.append(matches.next().captured(0));

    if (found.size() != 1)
        return std::nullopt;

    return parseAmount(found.first());
}

} // namespace

OllamaOptions::OllamaOptions()
{
    // Ollama's own convention, so a remote or proxied server needs no new flag.
    const QString host = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("OLLAMA_HOST"));
    url = host.isEmpty() ? QStringLiteral("http://127.0.0.1:11434") : host;
    if (!url.startsWith(QStringLiteral("http")))
        url.prepend(QStringLiteral("http://"));

    // There is no Ollama convention for the model, so this variable is ours. It is
    // here because `tst_bills` builds its own options and has no flag to pass one:
    // without it, comparing two models means editing `kDefaultModel` and rebuilding
    // between the runs. The CLI's `--model` defaults to whatever this chose.
    const QString chosen = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("INVOICEDROP_MODEL"));
    if (!chosen.isEmpty())
        model = chosen;
}

OllamaClient::OllamaClient(OllamaOptions options) : m_options(std::move(options))
{
    if (m_options.url.endsWith(QLatin1Char('/')))
        m_options.url.chop(1);
}

QString OllamaClient::askWithoutSchema(const QString &field,
                                       const QString &prompt,
                                       const QString &question,
                                       const QString &text,
                                       const QList<QByteArray> &jpegPages,
                                       bool think) const
{
    // Whatever the extraction was handed, this asks about: page images, the text
    // layer, or both. A document that arrived as text has no image to send, and
    // skipping the question for it would leave the field to the extraction reply,
    // which is the source these questions exist to check.
    QString asked = question;
    const QString carried = text.trimmed();
    if (!carried.isEmpty()) {
        asked += QStringLiteral("\n\n--- extracted text, may be corrupted ---\n");
        asked += carried;
    }

    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), asked);

    if (!jpegPages.isEmpty()) {
        QJsonArray images;
        for (const QByteArray &page : jpegPages)
            images.append(QString::fromLatin1(page.toBase64()));
        user.insert(QStringLiteral("images"), images);
    }

    QJsonArray messages;
    if (!prompt.isEmpty()) {
        QJsonObject system;
        system.insert(QStringLiteral("role"), QStringLiteral("system"));
        system.insert(QStringLiteral("content"), prompt);
        messages.append(system);
    }
    messages.append(user);

    QJsonObject options;
    options.insert(QStringLiteral("temperature"), 0);

    QJsonObject body;
    body.insert(QStringLiteral("model"), m_options.model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), false);
    body.insert(QStringLiteral("think"), think);
    body.insert(QStringLiteral("keep_alive"), m_options.keepAlive);
    // No `format`, which is the whole point of these requests: a schema is what
    // loses the field, and a schema narrowed to the one field was worse still.
    body.insert(QStringLiteral("options"), options);

    Log::block("ai", QStringLiteral("%1 prompt, %2 characters")
                         .arg(field)
                         .arg(prompt.size()),
               prompt);
    Log::block("ai", QStringLiteral("%1 question, %2 characters").arg(field).arg(asked.size()),
               asked);

    QString transportError;
    const QString reply = request("POST", QStringLiteral("/api/chat"),
                                  QJsonDocument(body).toJson(QJsonDocument::Compact),
                                  m_options.timeoutMs, &transportError);
    if (reply.isEmpty()) {
        Log::warn("ai", QStringLiteral("the %1 question failed: %2").arg(field, transportError));
        return {};
    }

    return QJsonDocument::fromJson(reply.toUtf8())
        .object()
        .value(QStringLiteral("message"))
        .toObject()
        .value(QStringLiteral("content"))
        .toString();
}

QString OllamaClient::askDate(const QString &text, const QList<QByteArray> &jpegPages) const
{
    if (text.trimmed().isEmpty() && jpegPages.isEmpty())
        return {};

    Log::step("ai", QStringLiteral("no labelled date in the text: asking for the date on its own, "
                                   "without a schema"));
    const QString content = askWithoutSchema(QStringLiteral("date"), QString(),
                                             QString::fromUtf8(kDateQuestion), text, jpegPages,
                                             /*think=*/true);

    // The answer is prose or a bare date, so it goes through the same normaliser
    // the text layer uses. That is deliberate: it finds a date inside a sentence,
    // converts whatever format came back, and rejects anything that is not a
    // complete date — `21`, which is what the constrained request answered, cannot
    // come out of it as a date.
    const QString date = normaliseDate(content);
    Log::model("ai", QStringLiteral("date answer: %1 characters, %2")
                         .arg(content.trimmed().size())
                         .arg(date.isEmpty() ? QStringLiteral("no date in it") : date));
    return date;
}

std::optional<double> OllamaClient::askTotal(const QString &text,
                                            const QList<QByteArray> &jpegPages) const
{
    if (text.trimmed().isEmpty() && jpegPages.isEmpty())
        return std::nullopt;

    Log::step("ai", QStringLiteral("asking for the amount on the SUMME line on its own, without a "
                                   "schema"));
    const QString content = askWithoutSchema(QStringLiteral("total"),
                                             QString::fromUtf8(kTotalPrompt),
                                             QString::fromUtf8(kTotalQuestion), text, jpegPages,
                                             m_options.think);

    const std::optional<double> total = amountFromAnswer(content);
    Log::model("ai", QStringLiteral("total answer: %1 characters, %2")
                         .arg(content.trimmed().size())
                         .arg(total.has_value()
                                  ? QString::number(*total, 'f', 2)
                                  : QStringLiteral("no single amount in it")));
    return total;
}

QString OllamaClient::request(const QByteArray &method,
                              const QString &path,
                              const QByteArray &payload,
                              int timeoutMs,
                              QString *error) const
{
    QNetworkAccessManager manager;
    QNetworkRequest networkRequest(QUrl(m_options.url + path));
    networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                             QStringLiteral("application/json"));

    QNetworkReply *reply = method == "POST" ? manager.post(networkRequest, payload)
                                            : manager.get(networkRequest);

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool timedOut = false;

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });

    deadline.start(timeoutMs);
    loop.exec();

    if (timedOut) {
        reply->abort();
        reply->deleteLater();
        Log::warn("ai", QStringLiteral("no answer from %1 after %2, giving up")
                             .arg(m_options.url, seconds(timeoutMs)));
        if (error) {
            *error = QStringLiteral("no answer from %1 after %2 - the model may still be loading")
                         .arg(m_options.url, seconds(timeoutMs));
        }
        return {};
    }

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkErrorText = reply->errorString();
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError && status == 0) {
        if (error) {
            if (networkError == QNetworkReply::ConnectionRefusedError
                || networkError == QNetworkReply::HostNotFoundError) {
                *error = QStringLiteral("no Ollama at %1 - start it with: systemctl start ollama")
                             .arg(m_options.url);
            } else {
                *error = QStringLiteral("cannot reach %1: %2").arg(m_options.url, networkErrorText);
            }
        }
        return {};
    }

    // Ollama reports its own failures in an `error` field with a 4xx or 5xx.
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (document.isObject()) {
        const QString apiError = document.object().value(QStringLiteral("error")).toString();
        if (!apiError.isEmpty()) {
            if (error) {
                if (apiError.contains(QStringLiteral("not found"), Qt::CaseInsensitive)) {
                    *error = QStringLiteral("model '%1' is not installed - run: ollama pull %2")
                                 .arg(m_options.model, m_options.model);
                } else {
                    *error = QStringLiteral("Ollama: %1").arg(apiError);
                }
            }
            return {};
        }
    }

    if (reply->error() != QNetworkReply::NoError) {
        if (error)
            *error = QStringLiteral("Ollama HTTP %1: %2").arg(status).arg(networkErrorText);
        return {};
    }

    return QString::fromUtf8(body);
}

bool OllamaClient::probe(QString *error) const
{
    const QString body = request("GET", QStringLiteral("/api/tags"), {}, 5000, error);
    return !body.isEmpty();
}

QStringList OllamaClient::installedModels(QString *error) const
{
    const QString body = request("GET", QStringLiteral("/api/tags"), {}, 5000, error);
    if (body.isEmpty())
        return {};

    const QJsonArray models =
        QJsonDocument::fromJson(body.toUtf8()).object().value(QStringLiteral("models")).toArray();

    QStringList names;
    names.reserve(models.size());
    for (const QJsonValue &entry : models)
        names.append(entry.toObject().value(QStringLiteral("name")).toString());
    return names;
}

QJsonObject OllamaClient::buildRequest(const QString &text,
                                      const QList<QByteArray> &jpegPages,
                                      const QString &complaint) const
{
    QJsonArray messages;

    QJsonObject system;
    system.insert(QStringLiteral("role"), QStringLiteral("system"));
    system.insert(QStringLiteral("content"), QString::fromUtf8(kSystemPrompt));
    messages.append(system);

    QString content = userPrompt(m_options.language);
    if (!complaint.isEmpty()) {
        content += QStringLiteral(
                       "\n\nYour previous answer was rejected: %1\nAnswer again, with the JSON "
                       "object only, and leave out any field you cannot read.")
                       .arg(complaint);
    }
    if (!text.trimmed().isEmpty()) {
        content += QStringLiteral("\n\n--- extracted text, may be corrupted ---\n");
        content += text.trimmed();
    }

    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), content);

    if (!jpegPages.isEmpty()) {
        QJsonArray images;
        for (const QByteArray &page : jpegPages)
            images.append(QString::fromLatin1(page.toBase64()));
        user.insert(QStringLiteral("images"), images);
    }
    messages.append(user);

    QJsonObject options;
    // Reading a receipt is not a creative task. Temperature zero also keeps the
    // answer stable across runs, which the cache in phase 3 depends on.
    options.insert(QStringLiteral("temperature"), 0);

    QJsonObject body;
    body.insert(QStringLiteral("model"), m_options.model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), false);
    body.insert(QStringLiteral("think"), m_options.think);
    body.insert(QStringLiteral("keep_alive"), m_options.keepAlive);
    body.insert(QStringLiteral("format"), invoiceSchema());
    body.insert(QStringLiteral("options"), options);

    return body;
}

bool OllamaClient::analyse(const QString &text,
                           const QList<QByteArray> &jpegPages,
                           Invoice *invoice,
                           QString *error)
{
    if (!invoice)
        return false;

    m_dateAsked = false;
    m_totalAsked = false;
    m_totalCorrected = false;

    if (text.trimmed().isEmpty() && jpegPages.isEmpty()) {
        if (error)
            *error = QStringLiteral("nothing to send: no text and no page images");
        return false;
    }

    QString complaint;

    for (int attempt = 0; attempt < 2; ++attempt) {
        const QJsonObject requestObject = buildRequest(text, jpegPages, complaint);
        const QByteArray payload = QJsonDocument(requestObject).toJson(QJsonDocument::Compact);

        logRequest(m_options, requestObject, payload, jpegPages, attempt);

        QString transportError;
        QElapsedTimer timer;
        timer.start();
        const QString body =
            request("POST", QStringLiteral("/api/chat"), payload, m_options.timeoutMs,
                    &transportError);
        m_lastInferenceMs = timer.elapsed();

        if (body.isEmpty()) {
            Log::warn("ai", QStringLiteral("nothing came back after %1 ms: %2")
                                 .arg(m_lastInferenceMs)
                                 .arg(transportError));
            if (error)
                *error = transportError;
            return false;
        }

        const QJsonObject object = QJsonDocument::fromJson(body.toUtf8()).object();
        const QJsonObject message = object.value(QStringLiteral("message")).toObject();
        const QString content = message.value(QStringLiteral("content")).toString();

        Log::model("ai", QStringLiteral("answer %1 in %2 ms: %3 bytes of JSON, %4 characters "
                                         "of content")
                              .arg(attempt + 1)
                              .arg(m_lastInferenceMs)
                              .arg(body.size())
                              .arg(content.size()));
        Log::block("ai", QStringLiteral("answer %1, %2 characters").arg(attempt + 1).arg(
                              content.size()), content);

        QString parseError;
        Invoice parsed = Invoice::fromModelReply(content, &parseError);

        if (parsed.plausible()) {
            // The issue date is decided here, and not by the reply. The reply does
            // carry one — the schema still asks for it, and that is left alone —
            // but it is the source that fills this field in when it cannot read
            // it, so it is kept only as the last resort.
            const QString fromReply = parsed.date;
            parsed.date.clear();

            // First source: a labelled date in the text layer. It is a regular
            // expression rather than a judgement, and it only accepts a date on a
            // line that carries a date label, so the earliest date on the paper,
            // often a service period, is not picked up by accident.
            if (!text.trimmed().isEmpty()) {
                parsed.date = findLabelledDate(text);
                if (!parsed.date.isEmpty()) {
                    Log::step("ai", QStringLiteral("date %1 taken off the labelled text")
                                           .arg(parsed.date));
                }
            }

            // Second source: the date question, in a request that carries no schema.
            // This is the ordinary route for a photograph, and the extra half second
            // it costs is what buys a date that was measured right on 13 of the 21
            // pages of the collection in tests/testdata, against 7 for the reply.
            //
            // A second request rather than a stronger prompt, because the two
            // cheaper ways to force the date out of the extraction were tried and
            // both cost more than they gave: adding the date to the schema's
            // `required` list turned the second bill of that same document into a
            // wrong date *and* a wrong total, and wording the system prompt to
            // insist on a date did the same.
            if (parsed.date.isEmpty() && (!text.trimmed().isEmpty() || !jpegPages.isEmpty())) {
                QElapsedTimer dateTimer;
                dateTimer.start();
                parsed.date = askDate(text, jpegPages);
                m_lastInferenceMs += dateTimer.elapsed();

                if (!parsed.date.isEmpty()) {
                    m_dateAsked = true;
                } else {
                    // The question answered nothing usable: no date, a fragment like
                    // `17`, or a failed request. The reply's own date is then the
                    // only evidence left, and it is taken even though the reply is
                    // the source that invents dates — an unverified date is still
                    // better than none, and this is exactly where the date came from
                    // before the question existed. It is worth saying out loud,
                    // because a wrong date here has no other trace in the output.
                    parsed.date = fromReply;
                    Log::warn("ai", fromReply.isEmpty()
                                       ? QStringLiteral("the date question found nothing and the "
                                                        "extraction carried no date either: "
                                                        "reporting the bill without one")
                                       : QStringLiteral("the date question found nothing, so the "
                                                        "date the extraction carried (%1) is used "
                                                        "unverified")
                                             .arg(fromReply));
                }
            }

            // Third source for a field the reply cannot be trusted with, and the
            // same shape as the date: the total is read off the line the shop
            // labelled as its total, by a question of its own in a request that
            // carries no schema. The answer is used when it is a single amount;
            // NONE — no such line — leaves the reply's own total standing, which is
            // the safe direction.
            const std::optional<double> fromReplyTotal = parsed.grossTotal;
            QElapsedTimer totalTimer;
            totalTimer.start();
            std::optional<double> askedTotal = askTotal(text, jpegPages);

            // One answer is not enough to overrule the reply. The question was
            // measured contradicting *itself*: on page 6 of the Lebensmittel
            // collection it answered the savings note once in three runs, where the
            // reply had the total right all three times. So a disagreement buys a
            // second asking, and the reply is replaced only when both answers say
            // the same thing. On page 8 both say 12,40 against a reply of 3,36, and
            // on page 6 the second answer is the total and the reply stands.
            if (askedTotal.has_value()
                && (!fromReplyTotal.has_value()
                    || qAbs(*fromReplyTotal - *askedTotal) >= 0.005)) {
                Log::step("ai",
                          QStringLiteral("the SUMME line reads %1 where the reply carried %2: "
                                         "asking once more before believing it")
                              .arg(QString::number(*askedTotal, 'f', 2),
                                   fromReplyTotal.has_value()
                                       ? QString::number(*fromReplyTotal, 'f', 2)
                                       : QStringLiteral("no total")));
                const std::optional<double> second = askTotal(text, jpegPages);
                if (!second.has_value() || qAbs(*second - *askedTotal) >= 0.005) {
                    Log::step("ai", QStringLiteral("the second answer is %1, so the reply's total "
                                                   "stands")
                                           .arg(second.has_value()
                                                    ? QString::number(*second, 'f', 2)
                                                    : QStringLiteral("nothing usable")));
                    askedTotal.reset();
                }
            }
            m_lastInferenceMs += totalTimer.elapsed();

            if (askedTotal.has_value()) {
                m_totalAsked = true;
                m_totalCorrected = !fromReplyTotal.has_value()
                    || qAbs(*fromReplyTotal - *askedTotal) >= 0.005;
                parsed.grossTotal = askedTotal;

                if (m_totalCorrected) {
                    // Loud on purpose. A total that changed because the reply had
                    // read a savings note is the one correction in this file that a
                    // user would otherwise never notice.
                    Log::warn("ai",
                              QStringLiteral("the SUMME line reads %1, the extraction reply "
                                             "carried %2: using %1")
                                  .arg(QString::number(*askedTotal, 'f', 2),
                                       fromReplyTotal.has_value()
                                           ? QString::number(*fromReplyTotal, 'f', 2)
                                           : QStringLiteral("no total at all")));
                } else {
                    Log::step("ai", QStringLiteral("the SUMME line agrees with the reply: %1")
                                           .arg(QString::number(*askedTotal, 'f', 2)));
                }
            } else {
                Log::step("ai", QStringLiteral("no SUMME line was answered, so the reply's total "
                                               "stands unverified"));
            }

            *invoice = parsed;
            return true;
        }

        if (attempt == 0) {
            complaint = !parseError.isEmpty()
                ? parseError
                : QStringLiteral("the vendor name and the gross total are both required");
            // The one retry there is, and the reason for it: the reply parsed, so
            // the model understood the format, but the fields phase 2 needs are
            // missing. Asking again with the complaint attached is worth a second
            // request; a third would not be.
            Log::warn("ai", QStringLiteral("answer rejected (%1), asking once more")
                                 .arg(complaint));
            continue;
        }

        // Second attempt failed too. Report what was read instead of pretending
        // the document was empty; a vendor without a total is still useful.
        *invoice = parsed;
        if (error) {
            if (!parseError.isEmpty())
                *error = parseError;
            else
                *error = QStringLiteral("the model did not return a vendor and a total");
        }
        Log::warn("ai", QStringLiteral("second answer rejected as well: %1")
                             .arg(parseError.isEmpty()
                                      ? QStringLiteral("no vendor and no total")
                                      : parseError));
        return false;
    }

    return false;
}

} // namespace InvoiceDrop
