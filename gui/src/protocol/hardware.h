// Which board a unit is: the hardware report, parsed.
//
// There are two CAN Triple boards and each takes its own firmware: the CAN
// Triple (1.x) and the CAN Triple 2.0. A unit says which it is in answer to
// CMD_GET_HARDWARE (firmware 1.0.15). A unit that cannot say is running older
// firmware, and every such unit is a CAN Triple 1.x — no 2.0 ships without the
// command — so builtIn() is that board rather than "unknown".
//
// Like capacity.h, deliberately free of DeviceLink and of the model:
// device_session reads it off a link, and the rest of the Manager asks it
// questions.
#pragma once

#include <QByteArray>
#include <QString>

#include "wire_structs.h"

namespace ct {

enum class BoardFamily : quint8 {
    Unknown = 0,
    CanTriple = HW_FAMILY_CAN_TRIPLE,
    CanTriple2 = HW_FAMILY_CAN_TRIPLE_2,
};

// "CAN Triple", "CAN Triple 2.0". A family this build does not know is named
// by its number, so a newer board reads as a newer board and never as a 1.x.
QString boardFamilyName(BoardFamily family);

// A firmware version, for the per-board gates below.
struct FirmwareVersion {
    int major;
    int minor;
    int patch;
};
// No firmware on that board's line has the feature.
constexpr FirmwareVersion kFirmwareNever{-1, 0, 0};
// Every firmware on that board's line has it.
constexpr FirmwareVersion kFirmwareAlways{0, 0, 0};

// The first firmware on EACH board's line that has a feature. The two lines are
// numbered separately — the CAN Triple (1.x) counts 1.0.x, the CAN Triple 2.0
// counts from 2.0.0 — so a version means nothing without the board it runs on,
// and a gate names one version per board. Ask with DeviceHardware::firmwareHas.
struct FirmwareSince {
    FirmwareVersion canTriple;  // the CAN Triple (1.x) line
    FirmwareVersion canTriple2; // the CAN Triple 2.0 line
};

// The features the Manager gates on the unit's firmware version.
namespace firmware_since {
// The extended device-channel list (CMD_WRITE/READ_DEVICE_CHANNELS_EXT) and the
// Tx Dropped counts on it: 1.0.15 on the 1.x line, and every 2.0 firmware (the
// first, numbered 1.0.15 before the lines split, already had them).
constexpr FirmwareSince kExtendedDeviceChannels{{1, 0, 15}, kFirmwareAlways};
} // namespace firmware_since

// The family a firmware image's product ID (FW_PRODUCT_*, fw_image.h) is built
// for, and back. Unknown for a product ID this build does not know.
BoardFamily familyForProduct(quint16 productId);
quint16 productForFamily(BoardFamily family);

struct DeviceHardware {
    BoardFamily family = BoardFamily::CanTriple;
    quint16 productId = 0;          // of the running image
    int boardRevMajor = 0;          // 0 = not known
    int boardRevMinor = 0;
    quint8 boardRevSource = HW_REV_SOURCE_NONE;
    quint8 package = 0;             // HW_PACKAGE_*; meaningful only when reported
    quint16 flashKb = 0;            // the part's flash-size register
    quint16 devId = 0;
    quint16 revId = 0;
    quint32 features = 0;           // HW_FEAT_*
    quint32 qspiBytes = 0;
    quint32 framBytes = 0;
    quint16 fwMajor = 0;
    quint16 fwMinor = 0;
    quint16 fwPatch = 0;
    quint16 storeVersion = 0;
    quint32 buildId = 0;
    quint8 bootloaderVersion = 0;
    // True when the unit answered CMD_GET_HARDWARE. False for builtIn(): a 1.x
    // unit on firmware older than 1.0.15, whose features are the 1.x board's
    // rather than read, and whose silicon and versions this cannot say.
    bool reported = false;

    bool hasFeature(quint32 feature) const { return (features & feature) != 0; }
    QString familyName() const { return boardFamilyName(family); }
    // "2.00", or empty when the unit does not know its revision.
    QString boardRevisionText() const;
    // "1.0.15", or empty when not reported.
    QString firmwareVersionText() const;
    // Whether the running firmware has a feature that arrived at `since` on
    // THIS unit's board. False when not reported (a 1.x on firmware older than
    // the report), and false on a board this build does not know. Gate on this,
    // not on firmwareAtLeast: a version means nothing without its board.
    bool firmwareHas(const FirmwareSince &since) const
    {
        const FirmwareVersion *first = nullptr;
        switch (family) {
        case BoardFamily::CanTriple:
            first = &since.canTriple;
            break;
        case BoardFamily::CanTriple2:
            first = &since.canTriple2;
            break;
        default:
            return false;
        }
        if (first->major < 0)
            return false;
        return firmwareAtLeast(first->major, first->minor, first->patch);
    }
    // Whether the running firmware's NUMBER is major.minor.patch or newer. Only
    // meaningful within one board's line (see firmwareHas). False when not
    // reported, which is right for any version from 1.0.15 on: firmware that
    // cannot report predates the report.
    bool firmwareAtLeast(int major, int minor, int patch) const
    {
        if (!reported)
            return false;
        if (fwMajor != major)
            return fwMajor > major;
        if (fwMinor != minor)
            return fwMinor > minor;
        return fwPatch >= patch;
    }
    // "STM32G47x, 48-pin package", or empty when not reported.
    QString processorText() const;
    // The chip is one board's and the firmware was built for the other: an
    // image running on the wrong hardware. Only a report can show it.
    bool siliconMismatch() const;
    // As much as is known, for a status bar: "CAN Triple 2.0 rev 2.00,
    // firmware 2.0.0", or just "CAN Triple".
    QString summary() const;
    // Whether this unit publishes device channel `id` (DEVCH_*): every one its
    // firmware knows, except the supply block on a board that does not measure
    // its supply (HW_FEAT_SUPPLY_SENSE), and the Tx Dropped counts on firmware
    // without the extended list (firmware_since::kExtendedDeviceChannels). What
    // such a unit does not publish reads 0 there.
    bool publishesDeviceChannel(int id) const;

    // A CAN Triple 1.x on firmware that predates the report: termination
    // switched in software, and nothing else.
    static DeviceHardware builtIn();
};

// Parse a reply from its first byte. Tolerates trailing bytes, which a later
// format may append, and refuses a format this build does not read or a reply
// too short to hold the report. On success `reported` is set.
bool parseHardwareReport(const QByteArray &bytes, DeviceHardware *out);

} // namespace ct
