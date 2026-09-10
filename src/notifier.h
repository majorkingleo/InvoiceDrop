#pragma once

#include "analysis.h"

#include <QString>
#include <QVector>

namespace InvoiceDrop {

/// Desktop notifications through the freedesktop notification service.
///
/// Deliberately not KNotification. Sending a notification is one DBus call to
/// `org.freedesktop.Notifications`, and going straight to it keeps the build on
/// Qt alone instead of pulling in KDE Frameworks for a toast. Every desktop this
/// tool targets implements that interface, including Plasma.
class Notifier
{
public:
    explicit Notifier(bool enabled = true);

    /// True when a notification service is on the bus. Checked once and cached.
    bool isAvailable() const;

    /// Sends one notification. Silent no-op when notifications are off or no
    /// service is present, because a missing toast must never fail a run.
    void notify(const QString &summary, const QString &body, bool urgent = false);

    /// Notification text for a finished document: one line per bill, because a
    /// file can hold twenty of them.
    static QString bodyFor(const QString &fileName, const QVector<BillResult> &bills);

private:
    bool m_enabled;
};

} // namespace InvoiceDrop
