#include "firmware_image.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

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
    case FW_RESULT_ROLLED_BACK:
        return QCoreApplication::translate(
            "FirmwareImage",
            "the new firmware did not start properly, so the unit went back to the "
            "firmware before it");
    case FW_RESULT_RECOVERED:
        return QCoreApplication::translate(
            "FirmwareImage",
            "the installed firmware was found damaged and was written again from the "
            "unit's stored copy");
    case FW_RESULT_NO_IMAGE:
        return QCoreApplication::translate(
            "FirmwareImage", "no stored copy of the firmware could be used");
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
    // 2 GB video. No CAN Triple board has more than 512 KB of flash, so nothing
    // larger is firmware for any of them; the slot limit for the images THIS
    // Manager installs is applied once the header has said which board a file
    // is for, so a file for another board is refused by name, not by size.
    constexpr qint64 kLargestAnyBoard = 512 * 1024;
    const qint64 fileSize = file.size();
    if (fileSize < static_cast<qint64>(FW_IMAGE_MIN_SIZE)) {
        return fail(QCoreApplication::translate(
            "FirmwareImage", "%1 is only %2 bytes — too small to be a firmware image.")
                        .arg(QFileInfo(path).fileName())
                        .arg(fileSize));
    }
    const auto tooLarge = [&]() {
        return fail(QCoreApplication::translate(
            "FirmwareImage",
            "%1 is %2 bytes, larger than the %3 byte firmware slot. "
            "This is not a CAN Triple firmware image.")
                        .arg(QFileInfo(path).fileName())
                        .arg(fileSize)
                        .arg(FW_APP_MAX_SIZE));
    };
    if (fileSize > kLargestAnyBoard) {
        return tooLarge();
    }

    FirmwareImage image;
    image.m_bytes = file.readAll();
    file.close();
    if (image.m_bytes.size() != fileSize) {
        return fail(QCoreApplication::translate("FirmwareImage",
                                                "Could not read all of %1.")
                        .arg(QFileInfo(path).fileName()));
    }

    // WHICH BOARD the file is for, before anything else is judged. Each board
    // takes only its own images, and the update dialog holds a file to the
    // board of the unit it is about to go to (FirmwareImage::family). A file
    // for a board this build does not know is refused by its product ID, and
    // one too large for its own board's slot is refused naming that board, so
    // a user holding the wrong file is told which one it is. A file that does
    // not even look like an image goes on to the checks below and is refused
    // for what it actually is.
    BoardFamily family = BoardFamily::CanTriple;
    if (image.m_bytes.size() >= int(FW_IMAGE_HEADER_OFFSET + sizeof(FwImageHeader))) {
        FwImageHeader peek {};
        std::memcpy(&peek, image.m_bytes.constData() + FW_IMAGE_HEADER_OFFSET, sizeof(peek));
        if (peek.magic == FW_IMAGE_MAGIC) {
            family = familyForProduct(peek.product_id);
            if (family == BoardFamily::Unknown) {
                return fail(QCoreApplication::translate(
                    "FirmwareImage",
                    "%1 is firmware for a product this version of the Device Manager does "
                    "not know (product ID 0x%2).")
                                .arg(QFileInfo(path).fileName())
                                .arg(QString::number(peek.product_id, 16).toUpper()));
            }
            // A 2.0 file from before the 2.0 firmware line: its header and CRC
            // are perfect and a 2.0 bootloader would take it, but it was built
            // for the provisional firmware's layout and would not start. The
            // unit would go back to its previous firmware after a few restarts;
            // refusing the file here saves the user those restarts.
            if (peek.min_bootloader_version < oldestImageBootloader(family)) {
                return fail(QCoreApplication::translate(
                                "FirmwareImage",
                                "%1 is early CAN Triple 2.0 firmware (%2.%3.%4), from before "
                                "the 2.0 firmware line, and does not run on units that have "
                                "it. Use a 2.0.0 or later file.")
                                .arg(QFileInfo(path).fileName())
                                .arg(peek.fw_version_major)
                                .arg(peek.fw_version_minor)
                                .arg(peek.fw_version_patch));
            }
        }
    }
    // Each board bounds its images by its own application slot.
    const quint32 slotSize = appSlotSize(family);
    if (fileSize > static_cast<qint64>(slotSize)) {
        if (family != BoardFamily::CanTriple) {
            return fail(QCoreApplication::translate(
                "FirmwareImage",
                "%1 is firmware for the %2, but at %3 bytes it is larger than that board's "
                "%4 byte firmware slot.")
                            .arg(QFileInfo(path).fileName(), boardFamilyName(family))
                            .arg(fileSize)
                            .arg(slotSize));
        }
        return tooLarge();
    }

    // Hand the bytes to the firmware's own validator, in a buffer padded with
    // 0xFF to the full slot size — which is exactly what the device sees, an
    // image followed by erased flash. Validating against a snugly-sized buffer
    // would let an image whose declared size overran the file pass here and
    // fail on the device. fw_image_validate_product runs the checks the
    // board's bootloader runs, for the board the header names.
    QByteArray slot = image.m_bytes;
    slot.append(static_cast<int>(slotSize) - slot.size(), '\xFF');

    const quint8 verdict = fw_image_validate_product(slot.constData(), slotSize,
                                                     newestBootloader(family),
                                                     productForFamily(family));
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

    // The capacity block, if this image carries one. The magic is what tells a
    // block from the code an older image has at the same offset. The parser
    // tolerates the code that follows the block, so it is handed the rest of
    // the file rather than a guessed length. Read AFTER the CRC has passed:
    // the block is inside it, so a tampered capacity is a rejected file, never
    // a wrong number.
    if (image.m_bytes.size() >= int(FW_CAPS_OFFSET) + 4) {
        const quint32 magic =
            qFromLittleEndian<quint32>(image.m_bytes.constData() + FW_CAPS_OFFSET);
        if (magic == FW_CAPS_MAGIC) {
            DeviceCapacity capacity;
            if (parseCapacityReport(image.m_bytes.mid(int(FW_CAPS_OFFSET) + 4), &capacity))
                image.m_capacity = capacity;
        }
    }

    return image;
}

// The CAN Triple 2.0's numbers. Its firmware line has its own fw_image.h, which
// this Manager does not compile (it compiles the 1.x one, whose image header,
// CRC and validation are the same bytes), so the two values that differ are
// here, and test_firmware_v2 holds them against that header.
namespace {
constexpr quint32 kCanTriple2AppMaxSize = 224u * 1024u;
constexpr quint32 kCanTriple2Bootloader = 3u;
} // namespace

quint32 FirmwareImage::appSlotSize(BoardFamily family)
{
    return family == BoardFamily::CanTriple2 ? kCanTriple2AppMaxSize : FW_APP_MAX_SIZE;
}

quint32 FirmwareImage::newestBootloader(BoardFamily family)
{
    return family == BoardFamily::CanTriple2 ? kCanTriple2Bootloader : FW_BOOTLOADER_VERSION;
}

quint32 FirmwareImage::oldestImageBootloader(BoardFamily family)
{
    return family == BoardFamily::CanTriple2 ? kCanTriple2Bootloader : 0u;
}

QString FirmwareImage::packedVersionText(quint32 packed)
{
    if (packed == 0)
        return QString();
    return QStringLiteral("%1.%2.%3")
        .arg((packed >> 16) & 0xFFu)
        .arg((packed >> 8) & 0xFFu)
        .arg(packed & 0xFFu);
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
