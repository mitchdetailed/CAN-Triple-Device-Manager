#include "firmware_image.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <cstring>

namespace ct {

QString FirmwareImage::resultText(quint8 fwResult)
{
    switch (fwResult) {
    case FW_RESULT_NONE:
        return QCoreApplication::translate("FirmwareImage", "no result recorded");
    case FW_RESULT_OK:
        return QCoreApplication::translate("FirmwareImage", "installed successfully");
    case FW_RESULT_BAD_MAGIC:
        return QCoreApplication::translate(
            "FirmwareImage", "not a CAN Triple firmware image");
    case FW_RESULT_WRONG_PRODUCT:
        return QCoreApplication::translate(
            "FirmwareImage", "built for a different product");
    case FW_RESULT_BAD_SIZE:
        return QCoreApplication::translate(
            "FirmwareImage", "image size is invalid or too large for this device");
    case FW_RESULT_BAD_CRC:
        return QCoreApplication::translate(
            "FirmwareImage", "checksum does not match — the image is corrupt or incomplete");
    case FW_RESULT_BL_TOO_OLD:
        return QCoreApplication::translate(
            "FirmwareImage", "needs a newer bootloader than this device has");
    case FW_RESULT_ERASE_FAILED:
        return QCoreApplication::translate("FirmwareImage", "flash erase failed");
    case FW_RESULT_PROGRAM_FAILED:
        return QCoreApplication::translate("FirmwareImage", "flash programming failed");
    case FW_RESULT_VERIFY_FAILED:
        return QCoreApplication::translate(
            "FirmwareImage", "the installed image did not read back correctly");
    case FW_RESULT_GAVE_UP:
        return QCoreApplication::translate(
            "FirmwareImage", "gave up after repeated failed attempts");
    default:
        return QCoreApplication::translate("FirmwareImage", "unknown result (%1)")
            .arg(fwResult);
    }
}

std::optional<FirmwareImage> FirmwareImage::load(const QString &path, QString *error)
{
    const auto fail = [error](const QString &text) -> std::optional<FirmwareImage> {
        if (error) {
            *error = text;
        }
        return std::nullopt;
    };

    QFile file(path);
    if (!file.exists()) {
        return fail(QCoreApplication::translate("FirmwareImage",
                                                "File not found: %1").arg(path));
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QCoreApplication::translate("FirmwareImage",
                                                "Cannot open %1: %2")
                        .arg(QFileInfo(path).fileName(), file.errorString()));
    }

    // Refuse anything that could not possibly be an image before reading it
    // all — this is a file chooser, so the user can and will point it at a
    // 2 GB video.
    const qint64 fileSize = file.size();
    if (fileSize < static_cast<qint64>(FW_IMAGE_MIN_SIZE)) {
        return fail(QCoreApplication::translate(
            "FirmwareImage", "%1 is only %2 bytes — too small to be a firmware image.")
                        .arg(QFileInfo(path).fileName())
                        .arg(fileSize));
    }
    if (fileSize > static_cast<qint64>(FW_APP_MAX_SIZE)) {
        return fail(QCoreApplication::translate(
            "FirmwareImage",
            "%1 is %2 bytes, larger than the %3 byte firmware slot. "
            "This is not a CAN Triple firmware image.")
                        .arg(QFileInfo(path).fileName())
                        .arg(fileSize)
                        .arg(FW_APP_MAX_SIZE));
    }

    FirmwareImage image;
    image.m_bytes = file.readAll();
    file.close();
    if (image.m_bytes.size() != fileSize) {
        return fail(QCoreApplication::translate("FirmwareImage",
                                                "Could not read all of %1.")
                        .arg(QFileInfo(path).fileName()));
    }

    // Hand the bytes to the firmware's own validator, in a buffer padded with
    // 0xFF to the full slot size — which is exactly what the device sees, an
    // image followed by erased flash. Validating against a snugly-sized buffer
    // would let an image whose declared size overran the file pass here and
    // fail on the device.
    QByteArray slot = image.m_bytes;
    slot.append(static_cast<int>(FW_APP_MAX_SIZE) - slot.size(), '\xFF');

    const quint8 verdict = fw_image_validate(slot.constData(), FW_APP_MAX_SIZE,
                                             FW_BOOTLOADER_VERSION);
    if (verdict != FW_RESULT_OK) {
        return fail(QCoreApplication::translate("FirmwareImage", "%1 was rejected: %2.")
                        .arg(QFileInfo(path).fileName(), resultText(verdict)));
    }

    std::memcpy(&image.m_header,
                image.m_bytes.constData() + FW_IMAGE_HEADER_OFFSET,
                sizeof(FwImageHeader));

    // The validator checks that the DECLARED size is consistent and covered by
    // the CRC; it cannot check it against the size of a file it never saw. A
    // file with extra bytes appended validates perfectly and is still not the
    // file that was built.
    if (image.m_header.image_size != static_cast<quint32>(image.m_bytes.size())) {
        return fail(QCoreApplication::translate(
            "FirmwareImage",
            "%1 declares %2 bytes but the file is %3 bytes. "
            "It may have been truncated or had data appended.")
                        .arg(QFileInfo(path).fileName())
                        .arg(image.m_header.image_size)
                        .arg(image.m_bytes.size()));
    }

    return image;
}

QString FirmwareImage::versionString() const
{
    return QStringLiteral("%1.%2.%3")
        .arg(m_header.fw_version_major)
        .arg(m_header.fw_version_minor)
        .arg(m_header.fw_version_patch);
}

QString FirmwareImage::buildDescription() const
{
    // Not necessarily NUL-terminated: a label using all 32 bytes fills the
    // array exactly, so bound the scan rather than trusting a terminator.
    const char *raw = m_header.build_desc;
    int len = 0;
    while (len < static_cast<int>(sizeof(m_header.build_desc)) && raw[len] != '\0') {
        ++len;
    }
    return QString::fromUtf8(raw, len);
}

int FirmwareImage::compareVersion(quint16 major, quint16 minor, quint16 patch) const
{
    if (m_header.fw_version_major != major) {
        return m_header.fw_version_major > major ? 1 : -1;
    }
    if (m_header.fw_version_minor != minor) {
        return m_header.fw_version_minor > minor ? 1 : -1;
    }
    if (m_header.fw_version_patch != patch) {
        return m_header.fw_version_patch > patch ? 1 : -1;
    }
    return 0;
}

} // namespace ct
