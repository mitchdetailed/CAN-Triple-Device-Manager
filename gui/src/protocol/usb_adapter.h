// CAN Triple 2.0 units that are a SocketCAN adapter now, and the way back.
//
// As an adapter (CMD_SET_USB_MODE, wire_structs.h) a unit has no serial port,
// so nothing the rest of this application does can reach it: the Device
// Manager's protocol does not exist on that device. What does is one USB
// vendor request to the device itself, CT_USB_REQ_SET_MODE, which the firmware
// answers in either mode and which turns it back into the Manager's port. It
// is unguarded on purpose: it only ever gives the buses back.
//
// Linux only. There the unit is in sysfs whatever driver has it, and usbfs
// carries a control request to a device without claiming any interface (the
// cantriple-socketcan package's udev rule opens the node to the person at the
// seat; without it the request needs root, and the error says so). Windows has
// no driver for the adapter at all, and needs none here: the firmware falls
// back to the Manager's port by itself on a PC that cannot use the adapter.
// Elsewhere adapterUnits() finds nothing and switchToManager() says why.
#pragma once

#include <QList>
#include <QString>

namespace ct {
namespace usb_adapter {

struct AdapterUnit {
    QString serial;  // the MCU ID, 24 hex digits, as the unit reports it
    QString usbNode; // /dev/bus/usb/BBB/DDD
    // The last six of the serial in lower case: what the cantriple-socketcan
    // package names the unit's interfaces after (ct<shortId>-can1..3).
    QString shortId() const { return serial.right(6).toLower(); }
};

// Every CAN Triple 2.0 that is a SocketCAN adapter now: 0483:5740 from
// "Minton Performance" with a vendor-class interface (the Manager's port has
// none). `sysfsDevices` is for tests; the real one is the default.
QList<AdapterUnit> adapterUnits(
    const QString &sysfsDevices = QStringLiteral("/sys/bus/usb/devices"));

// Ask `unit` to be the Manager's port again and, with `keep`, to start as it
// at every power-up too (firmware 2.0.10; earlier firmware refuses the keep).
// True once the unit has taken the request; its serial port appears a moment
// later. False with *error on a node this user may not write, a refusal, or a
// unit that has gone.
bool switchToManager(const AdapterUnit &unit, bool keep, QString *error);

} // namespace usb_adapter
} // namespace ct
