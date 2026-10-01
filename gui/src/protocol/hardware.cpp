#include "hardware.h"

#include <QtEndian>

#include <cstddef>

#include "fw_image.h" // FW_PRODUCT_*: which image each family takes

namespace ct {

namespace {

// The package each family's board carries. A code no board has means the
// question cannot be asked of that family.
constexpr quint8 kNoPackage = 0xFF;

quint8 expectedPackage(BoardFamily family)
{
    switch (family) {
    case BoardFamily::CanTriple: return HW_PACKAGE_LQFP48;
    case BoardFamily::CanTriple2: return HW_PACKAGE_LQFP100;
    case BoardFamily::Unknown: break;
    }
    return kNoPackage;
}

QString packageText(quint8 package)
{
    switch (package) {
    case HW_PACKAGE_LQFP48: return QStringLiteral("48-pin package");
    case HW_PACKAGE_LQFP100: return QStringLiteral("100-pin package");
    default: break;
    }
    return QStringLiteral("package code 0x%1")
        .arg(QString::number(package, 16).toUpper().rightJustified(2, QLatin1Char('0')));
}

} // namespace

QString boardFamilyName(BoardFamily family)
{
    switch (family) {
    case BoardFamily::CanTriple: return QStringLiteral("CAN Triple");
    case BoardFamily::CanTriple2: return QStringLiteral("CAN Triple 2.0");
    case BoardFamily::Unknown: return QStringLiteral("unknown board");
    }
    return QStringLiteral("board family %1").arg(int(family));
}

BoardFamily familyForProduct(quint16 productId)
{
    switch (productId) {
    case FW_PRODUCT_CAN_TRIPLE: return BoardFamily::CanTriple;
    case FW_PRODUCT_CAN_TRIPLE_2: return BoardFamily::CanTriple2;
    default: return BoardFamily::Unknown;
    }
}

quint16 productForFamily(BoardFamily family)
{
    switch (family) {
    case BoardFamily::CanTriple: return FW_PRODUCT_CAN_TRIPLE;
    case BoardFamily::CanTriple2: return FW_PRODUCT_CAN_TRIPLE_2;
    case BoardFamily::Unknown: break;
    }
    return 0;
}

QString DeviceHardware::boardRevisionText() const
{
    if (boardRevMajor == 0)
        return {};
    // Two digits after the point, as boards are named: 2.00, 2.01.
    return QStringLiteral("%1.%2").arg(boardRevMajor).arg(boardRevMinor, 2, 10, QLatin1Char('0'));
}

QString DeviceHardware::firmwareVersionText() const
{
    if (!reported)
        return {};
    return QStringLiteral("%1.%2.%3").arg(fwMajor).arg(fwMinor).arg(fwPatch);
}

QString DeviceHardware::processorText() const
{
    if (!reported)
        return {};
    const QString part = devId == HW_DEV_ID_STM32G47X
                             ? QStringLiteral("STM32G47x")
                             : QStringLiteral("device ID 0x%1")
                                   .arg(QString::number(devId, 16).toUpper());
    return part + QStringLiteral(", ") + packageText(package);
}

bool DeviceHardware::siliconMismatch() const
{
    if (!reported)
        return false;
    const quint8 expected = expectedPackage(family);
    return expected != kNoPackage && package != expected;
}

QString DeviceHardware::summary() const
{
    QString text = familyName();
    const QString revision = boardRevisionText();
    if (!revision.isEmpty())
        text += QStringLiteral(" rev %1").arg(revision);
    const QString firmware = firmwareVersionText();
    if (!firmware.isEmpty())
        text += QStringLiteral(", firmware %1").arg(firmware);
    return text;
}

bool DeviceHardware::publishesDeviceChannel(int id) const
{
    if (id >= DEVCH_SUPPLY && id <= DEVCH_USB_SUPPLY)
        return hasFeature(HW_FEAT_SUPPLY_SENSE);
    if (id >= DEVCH_TX_DROPPED_BASE && id < DEVCH_TX_DROPPED_BASE + DEVCH_BUS_COUNT)
        return firmwareHas(firmware_since::kExtendedDeviceChannels);
    return true;
}

DeviceHardware DeviceHardware::builtIn()
{
    DeviceHardware hw;
    hw.family = BoardFamily::CanTriple;
    hw.productId = FW_PRODUCT_CAN_TRIPLE;
    hw.features = HW_FEAT_TERMINATION;
    hw.reported = false;
    return hw;
}

bool parseHardwareReport(const QByteArray &bytes, DeviceHardware *out)
{
    if (!out)
        return false;
    *out = DeviceHardware{};
    if (bytes.size() < int(sizeof(HardwareReport)))
        return false;
    // Field by field at the documented offsets, like every wire record here: the
    // parser depends on the layout, not on this compiler packing a struct the
    // way the firmware's did.
    const auto *p = reinterpret_cast<const quint8 *>(bytes.constData());
    const auto u8 = [p](size_t at) { return p[at]; };
    const auto u16 = [p](size_t at) { return qFromLittleEndian<quint16>(p + at); };
    const auto u32 = [p](size_t at) { return qFromLittleEndian<quint32>(p + at); };
    if (u8(offsetof(HardwareReport, format)) != HARDWARE_REPORT_FORMAT)
        return false; // laid out in a way this build does not know

    out->family = static_cast<BoardFamily>(u8(offsetof(HardwareReport, family)));
    out->productId = u16(offsetof(HardwareReport, product_id));
    out->boardRevMajor = u8(offsetof(HardwareReport, board_rev_major));
    out->boardRevMinor = u8(offsetof(HardwareReport, board_rev_minor));
    out->boardRevSource = u8(offsetof(HardwareReport, board_rev_source));
    out->package = u8(offsetof(HardwareReport, package));
    out->flashKb = u16(offsetof(HardwareReport, flash_kb));
    out->devId = u16(offsetof(HardwareReport, dev_id));
    out->revId = u16(offsetof(HardwareReport, rev_id));
    out->features = u32(offsetof(HardwareReport, features));
    out->qspiBytes = u32(offsetof(HardwareReport, qspi_bytes));
    out->framBytes = u32(offsetof(HardwareReport, fram_bytes));
    out->fwMajor = u16(offsetof(HardwareReport, fw_major));
    out->fwMinor = u16(offsetof(HardwareReport, fw_minor));
    out->fwPatch = u16(offsetof(HardwareReport, fw_patch));
    out->storeVersion = u16(offsetof(HardwareReport, store_version));
    out->buildId = u32(offsetof(HardwareReport, build_id));
    out->bootloaderVersion = u8(offsetof(HardwareReport, bootloader_version));
    out->reported = true;
    return true;
}

} // namespace ct
