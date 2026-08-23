#include "framer.h"

#include "cobs.h"
#include "crc16.h"

namespace ct {

// The largest COBS-encoded frame the device can send. A full READ reply is at
// most ~2036 decoded bytes (4-byte header + the ~2030-byte response cap + 2-byte
// CRC), and COBS adds at most one code byte per 254 plus one — call it ~2045.
// 2100 is comfortable headroom, and it is only ever an upper bound on the resync
// search window (see feed()), so being generous costs nothing and being wrong
// low would drop a legitimate oversized reply.
static constexpr int kMaxEncodedFrameBytes = 2100;

// The raw bytes the CRC covers: [0x55][cmd][len u16 LE][payload]. Shared by
// buildFrame and frameCrc so the value the frame carries and the value the link
// records for ACK matching cannot diverge.
static QByteArray rawForCrc(quint8 cmd, const QByteArray &payload)
{
    QByteArray raw;
    raw.reserve(payload.size() + 4);
    raw.append(char(START_MARKER));
    raw.append(char(cmd));
    const quint16 len = quint16(payload.size());
    raw.append(char(len & 0xFF));        // little-endian length
    raw.append(char((len >> 8) & 0xFF));
    raw.append(payload);
    return raw;
}

quint16 frameCrc(quint8 cmd, const QByteArray &payload)
{
    const QByteArray raw = rawForCrc(cmd, payload);
    return crc16(reinterpret_cast<const uint8_t *>(raw.constData()), raw.size());
}

QByteArray buildFrame(quint8 cmd, const QByteArray &payload)
{
    QByteArray raw = rawForCrc(cmd, payload);
    const quint16 crc = crc16(reinterpret_cast<const uint8_t *>(raw.constData()), raw.size());
    raw.append(char((crc >> 8) & 0xFF)); // CRC appended big-endian
    raw.append(char(crc & 0xFF));

    QByteArray frame;
    frame.append(char(0)); // leading resync delimiter (firmware ignores empty frames)
    frame.append(cobsEncode(raw));
    frame.append(char(0));
    return frame;
}

static bool parseChunk(const QByteArray &chunk, Packet &out)
{
    const QByteArray raw = cobsDecode(chunk);
    if (raw.size() < 6)
        return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(raw.constData());
    const quint16 expected = quint16((quint8(raw[raw.size() - 2]) << 8) | quint8(raw[raw.size() - 1]));
    if (crc16(bytes, raw.size() - 2) != expected)
        return false;
    if (bytes[0] != START_MARKER)
        return false;
    const quint16 len = quint16(bytes[2] | (bytes[3] << 8));
    if (4 + len + 2 != raw.size())
        return false; // header length must describe the payload exactly
    out.cmd = bytes[1];
    out.payload = raw.mid(4, len);
    return true;
}

QList<Packet> FrameSplitter::feed(const QByteArray &data)
{
    QList<Packet> packets;
    m_buffer.append(data);

    int start = 0;
    while (true) {
        const int zero = m_buffer.indexOf(char(0), start);
        if (zero < 0)
            break;
        if (zero > start) {
            const QByteArray chunk = m_buffer.mid(start, zero - start);
            Packet p;
            bool ok = parseChunk(chunk, p);
            // The firmware interleaves unframed printf text (never containing
            // 0x00) on the same UART, and frames carry only a trailing
            // delimiter — so noise glues to the front of the next frame's
            // bytes. Resync by retrying successive suffixes; CRC + start
            // marker + exact length make a false accept negligible, and a
            // chunk can hold at most one frame, so first match wins.
            //
            // The search starts near the END, not at offset 1. A valid frame is
            // at most kMaxEncodedFrameBytes long, so it can only ever be a
            // suffix of the chunk within that many bytes of the end — an offset
            // earlier than that would decode more bytes than any real frame and
            // could never validate. Scanning from offset 1 instead made this
            // O(n^2) in the chunk size (n offsets, each an O(n) decode), so a
            // flood of multi-kilobyte garbage between delimiters — a wrong baud
            // rate, or a hostile device — froze the UI thread for the length of
            // the flood. Bounding the window makes the cost per chunk constant.
            // For an ordinary chunk (real frames are small) minOffset is 1 and
            // the behaviour is exactly as before.
            const int minOffset = qMax(1, chunk.size() - kMaxEncodedFrameBytes);
            for (int offset = minOffset; !ok && offset < chunk.size() - 5; ++offset)
                ok = parseChunk(chunk.mid(offset), p);
            if (ok)
                packets.append(p);
        }
        start = zero + 1;
    }
    m_buffer.remove(0, start);

    // Guard against unbounded growth if the stream never contains a delimiter
    // (e.g. wrong baud rate producing continuous garbage).
    if (m_buffer.size() > 8192)
        m_buffer.clear();

    return packets;
}

} // namespace ct
