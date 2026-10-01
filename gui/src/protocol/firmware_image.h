// Loading and validating a .ctf firmware image.
//
// A .ctf is not a container wrapped around the firmware — it IS the firmware,
// byte for byte as it will sit in flash, with a 64-byte header embedded at
// offset 0x200. So the file on disk, the bytes on the wire, the bytes in the
// device's staging slot and the bytes in its application slot are all one
// sequence, and a single CRC covers all four.
//
// Validation here runs the DEVICE'S OWN CODE. fw_image_validate() comes from
// the firmware tree (firmware/src/fw_image.c, reached through the firmware
// junction and compiled into this target), so a file this class accepts is a
// file the bootloader accepts, and one it rejects is rejected for the same
// stated reason. The alternative — reimplementing the header checks and the
// CRC32 in C++ — would agree with the firmware by inspection, and a CRC that
// agrees by inspection is one that disagrees the day someone edits either
// side. Being able to tell a user "this file is corrupt" without spending a
// transfer to find out is worth the odd-looking dependency.
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

#include "capacity.h"
#include "fw_image.h"
#include "hardware.h"

namespace ct {

class FirmwareImage
{
public:
    // Read and fully validate a .ctf. Returns nullopt with *error set to
    // something a user can act on when the file is missing, unreadable, not a
    // firmware image, for another product, or corrupt. A file for another
    // board is refused by NAME ("firmware for the CAN Triple 2.0") before any
    // other check, so a user holding the wrong file is told which one it is
    // rather than that it is too large or corrupt.
    static std::optional<FirmwareImage> load(const QString &path, QString *error);

    const FwImageHeader &header() const { return m_header; }
    quint16 productId() const { return m_header.product_id; }
    // The board this image runs on: the CAN Triple (1.x) or the CAN Triple 2.0,
    // the only two load() accepts. An image goes only to a unit of its own
    // board; the update dialog enforces that.
    BoardFamily family() const { return familyForProduct(m_header.product_id); }
    // The application slot on each board, which bounds its images.
    static quint32 appSlotSize(BoardFamily family);
    // The newest bootloader of each board this Manager knows, which is what a
    // file for that board is validated against: an image that asks for more
    // is one no unit of that board can install.
    static quint32 newestBootloader(BoardFamily family);
    // The oldest min_bootloader_version an image for the board may carry. 0 for
    // the CAN Triple; 3 for the 2.0, whose earlier images were built for the
    // layout of its provisional firmware and would not start on the 2.0 line.
    static quint32 oldestImageBootloader(BoardFamily family);
    // "2.0.1" for a version the 2.0 firmware packed (FwUpdateStatus2); empty
    // for 0, which means none.
    static QString packedVersionText(quint32 packed);
    const QByteArray &bytes() const { return m_bytes; }
    quint32 size() const { return static_cast<quint32>(m_bytes.size()); }

    QString versionString() const;   // "2.1.0"
    QString buildDescription() const; // the 32-byte label, or empty
    quint16 flashStoreVersion() const { return m_header.flash_store_version; }
    quint32 crc32() const { return m_header.image_crc32; }
    quint32 minBootloaderVersion() const { return m_header.min_bootloader_version; }

    // The capacity block after the header (FW_CAPS_OFFSET, fw_image.h): what
    // this image can hold, table by table, read from the file alone and
    // parsed by the same function that reads a unit's reply. nullopt for an
    // image built before the block existed, or whose report this build cannot
    // read — the Manager then assumes DeviceCapacity::builtIn() for it, which
    // is what such firmware holds.
    const std::optional<DeviceCapacity> &capacity() const { return m_capacity; }

    // Compare against a device's running version. Returns <0 older, 0 same,
    // >0 newer. Used to warn about installing an older build, which is legal
    // and occasionally intended, but should never happen by accident.
    int compareVersion(quint16 major, quint16 minor, quint16 patch) const;

    // Human-readable text for an FW_RESULT_* code, for reporting both a file
    // rejected here and a commit the bootloader refused.
    static QString resultText(quint8 fwResult);

private:
    FirmwareImage() = default;

    QByteArray m_bytes;
    FwImageHeader m_header {};
    std::optional<DeviceCapacity> m_capacity;
};

} // namespace ct
