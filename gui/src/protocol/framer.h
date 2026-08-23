// Packet build / parse on top of COBS framing.
// Wire frame: 0x00 (resync) + COBS( [0x55][cmd][len u16 LE][payload][crc hi][crc lo] ) + 0x00
#pragma once

#include <QByteArray>
#include <QList>

#include "wire_structs.h"

namespace ct {

struct Packet {
    quint8 cmd = 0;
    QByteArray payload;
};

// Builds a complete wire frame ready to write to the serial port.
QByteArray buildFrame(quint8 cmd, const QByteArray &payload);

// The CRC16 a request for (cmd, payload) carries — the same value buildFrame
// puts in the frame, and the value the firmware echoes back in that request's
// ACK/NACK so DeviceLink can tell a genuine ACK from a duplicate left over from
// a retransmit. Exposed so the link can record it per request without rebuilding
// and re-parsing the frame.
quint16 frameCrc(quint8 cmd, const QByteArray &payload);

// Accumulates raw serial bytes, splits on 0x00, COBS-decodes, validates CRC
// and start marker. Invalid chunks (firmware printf noise, line corruption)
// are dropped silently, matching DESIGN.md §1.
class FrameSplitter
{
public:
    // Feed received bytes; returns every valid packet completed by this data.
    QList<Packet> feed(const QByteArray &data);
    void clear() { m_buffer.clear(); }

private:
    QByteArray m_buffer;
};

} // namespace ct
