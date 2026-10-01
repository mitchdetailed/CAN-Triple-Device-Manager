// Recognising a CAN Triple 2.0 among the serial ports.
//
// The 1.x unit is reached through an ST-LINK's USB serial port; the 2.0 is its
// own USB serial device. It uses ST's shared Virtual COM Port IDs (0483:5740)
// for now, and its USB serial number is the chip's 96-bit unique ID as 24 hex
// digits, the form Device Info shows as the MCU ID (firmware src/board_v2.c).
// Other ST devices can carry the same IDs, so a port that matches is a
// CANDIDATE; the unit's answer to CMD_GET_HARDWARE, read as soon as the port is
// open, says what is really there.
//
// Why it matters beyond the port list: a 2.0's COM port belongs to the unit
// itself, so every restart takes it away and brings it back (the unit
// re-enumerates), where an ST-LINK's port stays put while the 1.x unit behind
// it restarts.
#pragma once

#include <QSerialPortInfo>
#include <QString>

namespace ct {

inline bool isCanTriple2Usb(quint16 vendorId, quint16 productId, const QString &serial)
{
    if (vendorId != 0x0483 || productId != 0x5740 || serial.size() != 24)
        return false;
    for (const QChar c : serial) {
        const bool hex = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                         || (c >= QLatin1Char('A') && c <= QLatin1Char('F'))
                         || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
        if (!hex)
            return false;
    }
    return true;
}

inline bool isCanTriple2Port(const QSerialPortInfo &info)
{
    return info.hasVendorIdentifier() && info.hasProductIdentifier()
           && isCanTriple2Usb(info.vendorIdentifier(), info.productIdentifier(),
                              info.serialNumber());
}

// By name, against the ports present now: false for a port that has gone.
inline bool isCanTriple2Port(const QString &portName)
{
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts())
        if (info.portName().compare(portName, Qt::CaseInsensitive) == 0)
            return isCanTriple2Port(info);
    return false;
}

} // namespace ct
