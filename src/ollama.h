#pragma once

#include "invoice.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace InvoiceDrop {

/// Default model. Any Ollama model that reports the `vision` capability works;
/// this one reads photographed receipts correctly and answers in about a second.
inline constexpr auto kDefaultModel = "gemma4:latest";

struct OllamaOptions {
    QString url;
    QString model = QString::fromUtf8(kDefaultModel);
    QString language = QStringLiteral("de");
    QString keepAlive = QStringLiteral("30m");

    /// Thinking models spend their time on a visible reasoning trace before
    /// answering. Measured on a receipt: 10.2 s with it, 1.0 s without, same
    /// result. Invoice reading needs no deliberation, so it is off.
    bool think = false;

    int timeoutMs = 300000;

    OllamaOptions();
};

/// Minimal synchronous client for the Ollama HTTP API.
///
/// Synchronous on purpose: the CLI analyses one file and exits, so an event
/// loop with callbacks would only add ceremony. Phase 4, where the daemon must
/// stay responsive, will wrap the same request building in async calls.
class OllamaClient
{
public:
    explicit OllamaClient(OllamaOptions options = {});

    const OllamaOptions &options() const { return m_options; }

    /// GET /api/tags. Returns false and fills `error` when the server is not
    /// reachable.
    bool probe(QString *error = nullptr) const;

    /// Model names the server has pulled. Empty on failure.
    QStringList installedModels(QString *error = nullptr) const;

    /// Sends the extracted text and page images and returns the parsed invoice.
    ///
    /// A reply that parses but is missing the fields phase 2 reports is retried
    /// exactly once, with the complaint appended to the prompt. One retry, not a
    /// loop: if the model could not read the total the first time, a third
    /// attempt rarely helps and always costs seconds.
    bool analyse(const QString &text,
                 const QList<QByteArray> &jpegPages,
                 Invoice *invoice,
                 QString *error = nullptr);

    /// Milliseconds spent in the last successful `analyse`, including the extra
    /// request when the issue date had to be read from the page.
    qint64 lastInferenceMs() const { return m_lastInferenceMs; }

    /// True when the issue date of the last `analyse` came from the separate
    /// question rather than from a labelled date in the text layer. Reported as a
    /// note: on a photograph this is now the ordinary route, and the note is there
    /// so a bill that took two requests is not read as one that took a single one.
    bool dateAskedSeparately() const { return m_dateAsked; }

    /// True when the gross total of the last `analyse` came from a question of its
    /// own and differs from what the extraction reply carried. Reported as a note:
    /// a total that changed because the reply had read a savings note is exactly
    /// the kind of correction that should not be silent.
    bool totalCorrected() const { return m_totalCorrected; }

private:
    /// The request body as an object, so the same value can be serialised for the
    /// wire and read back for `--debug`. A second builder for the log would have
    /// drifted from this one within a phase.
    QJsonObject buildRequest(const QString &text,
                             const QList<QByteArray> &jpegPages,
                             const QString &complaint) const;

    /// Asks for the issue date on its own, without a JSON schema, and returns it
    /// normalised. Empty when the answer held no complete date.
    ///
    /// The question carries whatever the extraction carried — the page images, the
    /// text layer, or both — because a document that arrived as text has no image
    /// to send. `normaliseDate` is what decides: it is the reason a reply like `17`,
    /// which names a day and nothing else, cannot come out of here as a date.
    QString askDate(const QString &text, const QList<QByteArray> &jpegPages) const;

    /// Asks for the gross total on its own, without a schema, and returns the
    /// amount it answered. Empty when the answer was not a single amount, in which
    /// case the reply's own total stands.
    std::optional<double> askTotal(const QString &text, const QList<QByteArray> &jpegPages) const;

    /// One question about one field, in a request that carries no `format`.
    ///
    /// `askDate` and `askTotal` differ in their prompt and in what they make of
    /// the answer, not in how the request is built, so the building lives here.
    /// The request carries whatever the extraction carried — page images, the text
    /// layer, or both — because a document that arrived as text has no image.
    QString askWithoutSchema(const QString &field,
                             const QString &prompt,
                             const QString &question,
                             const QString &text,
                             const QList<QByteArray> &jpegPages,
                             bool think) const;

    QString request(const QByteArray &method,
                    const QString &path,
                    const QByteArray &payload,
                    int timeoutMs,
                    QString *error) const;

    OllamaOptions m_options;
    qint64 m_lastInferenceMs = 0;
    bool m_dateAsked = false;
    bool m_totalAsked = false;
    bool m_totalCorrected = false;
};

} // namespace InvoiceDrop
