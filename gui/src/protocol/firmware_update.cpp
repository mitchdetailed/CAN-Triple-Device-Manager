#include "firmware_update.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <cstring>

namespace ct {

namespace {

QString describeNack(quint8 errCode, const QString &fallback)
{
    if (errCode == 0) {
        return fallback;   // link failure, not a device refusal
    }
    if (errCode == ERR_FW_REJECTED) {
        return QCoreApplication::translate(
            "FirmwareUpdater",
            "The device refused the image. Check Firmware Update status for the reason.");
    }
    if (errCode == ERR_LOCKED) {
        return QCoreApplication::translate(
            "FirmwareUpdater",
            "The device is password protected. Unlock it before updating firmware.");
    }
    return DeviceLink::errorCodeText(errCode);
}

} // namespace

FirmwareUpdater::FirmwareUpdater(DeviceLink *link, QObject *parent)
    : QObject(parent), m_link(link)
{
}

bool FirmwareUpdater::readStatus(FwUpdateStatus *out, QString *error)
{
    QByteArray reply;
    quint8 errCode = 0;
    if (!m_link->requestSync(CMD_FW_UPDATE_STATUS, QByteArray(), &reply, error,
                             DeviceLink::kDefaultTimeoutMs,
                             DeviceLink::kDefaultRetries, &errCode)) {
        if (error && errCode == ERR_INVALID_CMD) {
            // The single most likely reason, and the one with a real remedy:
            // this firmware predates the bootloader entirely.
            *error = QCoreApplication::translate(
                "FirmwareUpdater",
                "This firmware does not support over-the-wire updates. "
                "The device needs a one-time bootloader installation over SWD.");
        } else if (error) {
            *error = describeNack(errCode, *error);
        }
        return false;
    }
    if (reply.size() != sizeof(FwUpdateStatus)) {
        if (error) {
            *error = QCoreApplication::translate(
                "FirmwareUpdater", "Unexpected status reply (%1 bytes, expected %2).")
                         .arg(reply.size())
                         .arg(sizeof(FwUpdateStatus));
        }
        return false;
    }
    std::memcpy(out, reply.constData(), sizeof(FwUpdateStatus));
    return true;
}

bool FirmwareUpdater::upload(const FirmwareImage &image, QString *error)
{
    m_cancelled = false;

    const QByteArray &bytes = image.bytes();
    const qint64 total = bytes.size();

    FwUpdateBeginPayload begin {};
    begin.image_size = static_cast<quint32>(total);
    begin.image_crc32 = image.crc32();
    begin.product_id = image.header().product_id;
    begin.version_major = image.header().fw_version_major;
    begin.version_minor = image.header().fw_version_minor;
    begin.version_patch = image.header().fw_version_patch;

    emit statusMessage(QCoreApplication::translate(
        "FirmwareUpdater", "Preparing device (erasing staging area)…"));

    // BEGIN erases as many staging pages as the image needs — several hundred
    // milliseconds of flash work before the ACK. The default 250 ms timeout
    // would expire mid-erase and the retry would arrive while the device was
    // still busy, which is the exact retransmit cascade that corrupted config
    // Sends before kFlashTimeoutMs existed. Use the flash timeout.
    {
        QByteArray payload(reinterpret_cast<const char *>(&begin), sizeof(begin));
        quint8 errCode = 0;
        if (!m_link->requestSync(CMD_FW_UPDATE_BEGIN, payload, nullptr, error,
                                 DeviceLink::kFlashTimeoutMs,
                                 DeviceLink::kDefaultRetries, &errCode)) {
            if (error) {
                *error = describeNack(errCode, *error);
            }
            return false;
        }
    }

    emit statusMessage(QCoreApplication::translate(
        "FirmwareUpdater", "Sending firmware…"));
    emit progress(0, total);

    qint64 offset = 0;
    while (offset < total) {
        if (m_cancelled) {
            // Leave nothing installable behind. A cancelled upload that left a
            // valid image in staging could still be installed later by the
            // bootloader's self-heal path without anyone asking for it.
            QString ignored;
            abort(&ignored);
            if (error) {
                *error = QCoreApplication::translate("FirmwareUpdater",
                                                     "Cancelled.");
            }
            return false;
        }

        const qint64 chunk = qMin<qint64>(kChunkBytes, total - offset);

        QByteArray payload;
        payload.reserve(static_cast<int>(chunk) + 4);
        const quint32 off32 = static_cast<quint32>(offset);
        payload.append(reinterpret_cast<const char *>(&off32), 4);
        payload.append(bytes.constData() + offset, static_cast<int>(chunk));

        quint8 errCode = 0;
        if (!m_link->requestSync(CMD_FW_UPDATE_DATA, payload, nullptr, error,
                                 DeviceLink::kDefaultTimeoutMs,
                                 DeviceLink::kDefaultRetries, &errCode)) {
            if (error) {
                *error = QCoreApplication::translate(
                             "FirmwareUpdater", "Failed at byte %1 of %2: ")
                             .arg(offset)
                             .arg(total)
                         + describeNack(errCode, *error);
            }
            return false;
        }

        offset += chunk;
        emit progress(offset, total);
    }

    emit statusMessage(QCoreApplication::translate(
        "FirmwareUpdater", "Verifying…"));

    // END re-validates the staged image on the device — full header check and
    // a CRC32 over every staged byte — and only then arms the bootloader. The
    // device does not take the host's word for any of it, which is what makes
    // a transport fault that slipped past the per-frame CRC survivable.
    {
        quint8 errCode = 0;
        if (!m_link->requestSync(CMD_FW_UPDATE_END, QByteArray(), nullptr, error,
                                 DeviceLink::kFlashTimeoutMs,
                                 DeviceLink::kDefaultRetries, &errCode)) {
            if (error) {
                *error = describeNack(errCode, *error);
            }
            return false;
        }
    }

    return true;
}

bool FirmwareUpdater::abort(QString *error)
{
    quint8 errCode = 0;
    if (!m_link->requestSync(CMD_FW_UPDATE_ABORT, QByteArray(), nullptr, error,
                             DeviceLink::kFlashTimeoutMs,
                             DeviceLink::kDefaultRetries, &errCode)) {
        if (error) {
            *error = describeNack(errCode, *error);
        }
        return false;
    }
    return true;
}

bool FirmwareUpdater::requestReset(QString *error)
{
    quint8 errCode = 0;
    // The device ACKs and then reboots. A link error AFTER the ACK is the
    // normal outcome, not a failure — but requestSync completes on the ACK, so
    // an error here really is one.
    if (!m_link->requestSync(CMD_RESET_DEVICE, QByteArray(), nullptr, error,
                             DeviceLink::kDefaultTimeoutMs,
                             DeviceLink::kDefaultRetries, &errCode)) {
        if (error) {
            *error = describeNack(errCode, *error);
        }
        return false;
    }
    return true;
}

} // namespace ct
