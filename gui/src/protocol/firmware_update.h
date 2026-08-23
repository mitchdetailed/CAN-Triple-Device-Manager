// Driving a firmware update over the serial link.
//
// The device receives the image into its bank-2 staging slot while it carries
// on running; the bootloader installs it on the next boot. Nothing this class
// does can damage the running firmware — every byte goes to staging, and an
// upload that is interrupted, corrupted or cancelled costs the transfer and
// nothing else. That is what makes it safe to retry.
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include "device_link.h"
#include "firmware_image.h"
#include "wire_structs.h"

namespace ct {

class FirmwareUpdater : public QObject
{
    Q_OBJECT
public:
    explicit FirmwareUpdater(DeviceLink *link, QObject *parent = nullptr);

    // Bytes of image data per CMD_FW_UPDATE_DATA frame.
    //
    // MAX_TX_PAYLOAD (496) less the 4-byte offset leaves 492, rounded DOWN to
    // a multiple of 8 because this part programs 64 bits at a time and the
    // device refuses a chunk that is not a whole number of doublewords. 488 is
    // that number; picking 490 would NACK every frame.
    static constexpr int kChunkBytes = 488;
    static_assert(kChunkBytes % 8 == 0, "chunks must be whole doublewords");
    static_assert(kChunkBytes + 4 <= MAX_TX_PAYLOAD, "chunk overruns the payload cap");

    // Read the device's update status. Also the way to find out whether a unit
    // has a bootloader at all: bootloader_version == 0 means it does not, and
    // no update can be installed on it.
    bool readStatus(FwUpdateStatus *out, QString *error);

    // BEGIN, then every chunk, then END. On success the device is armed and
    // needs a reset to install. Returns false with *error set otherwise; the
    // device is left running its existing firmware either way.
    bool upload(const FirmwareImage &image, QString *error);

    // Invalidate whatever is staged and clear any pending flag.
    bool abort(QString *error);

    // Ask the device to reboot so the bootloader can install. The reply is the
    // ACK before the reset, so a link error after it is expected, not a fault.
    bool requestReset(QString *error);

    // Stop an upload in progress at the next chunk boundary.
    void cancel() { m_cancelled = true; }

signals:
    void progress(qint64 sent, qint64 total);
    void statusMessage(const QString &text);

private:
    DeviceLink *m_link;
    bool m_cancelled = false;
};

} // namespace ct
