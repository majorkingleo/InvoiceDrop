#include "ollama.h"

#include "invoice-schema.h"

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
  promotional line and not a running balance.
- Use a dot as the decimal separator and no thousands separator.
- Give dates as ISO 8601, YYYY-MM-DD.
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

/// Fits a duration into the sentence the user reads.
QString seconds(qint64 milliseconds)
{
    return QString::number(milliseconds / 1000.0, 'f', 1) + QStringLiteral(" s");
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
}

OllamaClient::OllamaClient(OllamaOptions options) : m_options(std::move(options))
{
    if (m_options.url.endsWith(QLatin1Char('/')))
        m_options.url.chop(1);
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

QByteArray OllamaClient::buildRequestBody(const QString &text,
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

    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool OllamaClient::analyse(const QString &text,
                           const QList<QByteArray> &jpegPages,
                           Invoice *invoice,
                           QString *error)
{
    if (!invoice)
        return false;

    if (text.trimmed().isEmpty() && jpegPages.isEmpty()) {
        if (error)
            *error = QStringLiteral("nothing to send: no text and no page images");
        return false;
    }

    QString complaint;

    for (int attempt = 0; attempt < 2; ++attempt) {
        const QByteArray payload = buildRequestBody(text, jpegPages, complaint);

        QString transportError;
        QElapsedTimer timer;
        timer.start();
        const QString body =
            request("POST", QStringLiteral("/api/chat"), payload, m_options.timeoutMs,
                    &transportError);
        m_lastInferenceMs = timer.elapsed();

        if (body.isEmpty()) {
            if (error)
                *error = transportError;
            return false;
        }

        const QJsonObject object = QJsonDocument::fromJson(body.toUtf8()).object();
        const QJsonObject message = object.value(QStringLiteral("message")).toObject();
        const QString content = message.value(QStringLiteral("content")).toString();

        QString parseError;
        Invoice parsed = Invoice::fromModelReply(content, &parseError);

        if (parsed.plausible()) {
            // The model is not asked twice for something a regular expression can
            // read off the text, and it does drop the date on some runs.
            if (parsed.date.isEmpty()) {
                const QString fallback = findLabelledDate(text);
                if (!fallback.isEmpty())
                    parsed.date = fallback;
            }
            *invoice = parsed;
            return true;
        }

        if (attempt == 0) {
            complaint = !parseError.isEmpty()
                ? parseError
                : QStringLiteral("the vendor name and the gross total are both required");
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
        return false;
    }

    return false;
}

} // namespace InvoiceDrop
