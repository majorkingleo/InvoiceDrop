#include "extract/imagereader.h"

#include "extract/imageops.h"

#include <cstring>

#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace InvoiceDrop::Extract::ImageFile {
namespace {

constexpr int kExifReadLimit = 256 * 1024; // enough for the APP1 block and IFD0

int readShort(const QByteArray &data, int offset, bool littleEndian)
{
    if (offset < 0 || offset + 2 > data.size())
        return -1;
    const int first = static_cast<unsigned char>(data.at(offset));
    const int second = static_cast<unsigned char>(data.at(offset + 1));
    return littleEndian ? (first | (second << 8)) : ((first << 8) | second);
}

qint64 readLong(const QByteArray &data, int offset, bool littleEndian)
{
    if (offset < 0 || offset + 4 > data.size())
        return -1;
    const quint32 a = static_cast<unsigned char>(data.at(offset));
    const quint32 b = static_cast<unsigned char>(data.at(offset + 1));
    const quint32 c = static_cast<unsigned char>(data.at(offset + 2));
    const quint32 d = static_cast<unsigned char>(data.at(offset + 3));
    if (littleEndian)
        return static_cast<qint64>(a | (b << 8) | (c << 16) | (d << 24));
    return static_cast<qint64>((a << 24) | (b << 16) | (c << 8) | d);
}

/// Walks the IFD0 chain of a TIFF header looking for the orientation tag.
int parseTiffOrientation(const QByteArray &tiff)
{
    if (tiff.size() < 8)
        return 0;

    const bool littleEndian = tiff.at(0) == 'I' && tiff.at(1) == 'I';
    const bool bigEndian = tiff.at(0) == 'M' && tiff.at(1) == 'M';
    if (!littleEndian && !bigEndian)
        return 0;

    if (readShort(tiff, 2, littleEndian) != 42)
        return 0;

    const qint64 ifdOffset = readLong(tiff, 4, littleEndian);
    if (ifdOffset < 8 || ifdOffset + 2 > tiff.size())
        return 0;

    const int entryCount = readShort(tiff, static_cast<int>(ifdOffset), littleEndian);
    if (entryCount <= 0 || entryCount > 512)
        return 0;

    for (int i = 0; i < entryCount; ++i) {
        const int entry = static_cast<int>(ifdOffset) + 2 + i * 12;
        if (entry + 12 > tiff.size())
            break;
        if (readShort(tiff, entry, littleEndian) != 0x0112)
            continue;
        const int value = readShort(tiff, entry + 8, littleEndian);
        return (value >= 1 && value <= 8) ? value : 0;
    }

    return 0;
}

/// Applies the EXIF orientation so downstream code sees the photo the right way
/// up. Returns a new PIX; the input is not modified.
PIX *applyOrientation(PIX *pix, int orientation)
{
    switch (orientation) {
    case 2:
        return pixFlipLR(nullptr, pix);
    case 3:
        return pixRotate180(nullptr, pix);
    case 4:
        return pixFlipTB(nullptr, pix);
    case 5: { // transpose: rotate clockwise, then mirror horizontally
        PIX *rotated = pixRotate90(pix, 1);
        if (!rotated)
            return nullptr;
        PIX *flipped = pixFlipLR(nullptr, rotated);
        pixDestroy(&rotated);
        return flipped;
    }
    case 6:
        return pixRotate90(pix, 1);
    case 7: { // transverse: rotate clockwise, then mirror vertically
        PIX *rotated = pixRotate90(pix, 1);
        if (!rotated)
            return nullptr;
        PIX *flipped = pixFlipTB(nullptr, rotated);
        pixDestroy(&rotated);
        return flipped;
    }
    case 8:
        return pixRotate90(pix, -1);
    default:
        return nullptr;
    }
}

} // namespace

int exifOrientation(const QByteArray &head)
{
    if (head.size() < 4)
        return 0;
    // JPEG only: SOI marker plus a segment walk.
    if (static_cast<unsigned char>(head.at(0)) != 0xff
        || static_cast<unsigned char>(head.at(1)) != 0xd8)
        return 0;

    int position = 2;
    while (position + 4 <= head.size()) {
        if (static_cast<unsigned char>(head.at(position)) != 0xff) {
            ++position;
            continue;
        }

        const int marker = static_cast<unsigned char>(head.at(position + 1));
        if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            position += 2;
            continue;
        }
        if (marker == 0xda || marker == 0xd9) // start of scan: no metadata beyond
            break;

        const int segmentLength = readShort(head, position + 2, false);
        if (segmentLength < 2)
            break;

        const bool isExif = marker == 0xe1 && position + 10 <= head.size()
            && std::memcmp(head.constData() + position + 4, "Exif\0\0", 6) == 0;
        if (isExif) {
            const QByteArray tiff = head.mid(position + 10, segmentLength - 8);
            return parseTiffOrientation(tiff);
        }

        position += 2 + segmentLength;
    }

    return 0;
}

PIX *load(const QString &path, const ReadOptions &options, QStringList *notes, QString *error)
{
    const QByteArray nativePath = QFile::encodeName(path);
    PIX *pix = pixRead(nativePath.constData());
    if (!pix) {
        *error = QStringLiteral("unsupported or corrupt image");
        return nullptr;
    }

    notes->append(QStringLiteral("decoded %1x%2, depth %3")
                      .arg(pixGetWidth(pix))
                      .arg(pixGetHeight(pix))
                      .arg(pixGetDepth(pix)));

    // EXIF lives in the JPEG header, so only read it when it can matter.
    if (QFileInfo(path).suffix().compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0
        || QFileInfo(path).suffix().compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray head = file.read(kExifReadLimit);
            const int orientation = exifOrientation(head);
            if (orientation > 1) {
                PIX *rotated = applyOrientation(pix, orientation);
                if (rotated) {
                    pixDestroy(&pix);
                    pix = rotated;
                    notes->append(QStringLiteral("EXIF orientation %1 applied").arg(orientation));
                } else {
                    notes->append(
                        QStringLiteral("EXIF orientation %1 could not be applied").arg(orientation));
                }
            }
        }
    }

    PIX *rgb = ImageOps::toRgb32(pix);
    pixDestroy(&pix);
    if (!rgb) {
        *error = QStringLiteral("image cannot be converted to RGB");
        return nullptr;
    }

    PIX *scaled = ImageOps::downscale(rgb, options.longEdge, options.minShortEdge);
    pixDestroy(&rgb);
    if (!scaled) {
        *error = QStringLiteral("image cannot be downscaled");
        return nullptr;
    }

    notes->append(QStringLiteral("prepared %1x%2 for the model")
                      .arg(pixGetWidth(scaled))
                      .arg(pixGetHeight(scaled)));

    return scaled;
}

} // namespace InvoiceDrop::Extract::ImageFile
