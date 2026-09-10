#pragma once

#include "invoice.h"

#include <QByteArray>
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

    /// Milliseconds spent in the last successful `analyse`.
    qint64 lastInferenceMs() const { return m_lastInferenceMs; }

private:
    QByteArray buildRequestBody(const QString &text,
                                const QList<QByteArray> &jpegPages,
                                const QString &complaint) const;

    QString request(const QByteArray &method,
                    const QString &path,
                    const QByteArray &payload,
                    int timeoutMs,
                    QString *error) const;

    OllamaOptions m_options;
    qint64 m_lastInferenceMs = 0;
};

} // namespace InvoiceDrop
