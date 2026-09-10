#include "hash.h"

#include <QCryptographicHash>
#include <QFile>

namespace InvoiceDrop {

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr qint64 kChunk = 1 << 20;
    while (!file.atEnd()) {
        const QByteArray block = file.read(kChunk);
        if (block.isEmpty())
            break;
        hash.addData(block);
    }

    return QString::fromLatin1(hash.result().toHex());
}

} // namespace InvoiceDrop
